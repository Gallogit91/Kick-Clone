#pragma once
// Motor de resintesis (v2). Por cada golpe mezcla tres capas:
//   cuerpo : oscilador senoidal con envolvente de tono y de amplitud (+ armonicos opcionales)
//   click  : rafaga de ruido filtrado paso alto con decaimiento exponencial
//   ruido  : ruido blanco por un paso banda (SVF) con envolvente multisegmento
// El cuerpo es mono (centro); click y ruido son estereo y su ancho se ajusta con 'width'.
// Despues van la saturacion, la dinamica de salida (PostChain) y un limitador suave de seguridad.
// Sin asignaciones de memoria en el hilo de audio.

#include "KickModel.h"
#include <cstdint>
#include <vector>

namespace kc
{
class KickVoice
{
public:
    void prepare (double sampleRate) { sr = sampleRate; active = false; }

    bool isActive() const { return active; }

    void start (const KickModel& m, const KickParams& p, int midiNote, float velocity)
    {
        model = m;
        params = p;
        const float semis = p.tuneSemitones + p.keytrack * (float) (midiNote - 36);
        transpose = std::pow (2.0f, semis / 12.0f);
        velGain = std::pow (std::clamp (velocity, 0.0f, 1.0f), 0.7f);
        fEnd = std::max (model.pitch.n > 0 ? model.pitch.v[model.pitch.n - 1] : 50.0f, 1.0f);

        const float ampEnd = model.amp.endMs() * params.decayScale;
        const float noiseEnd = model.noise.endMs() * params.noiseDecayScale;
        const float clickEnd = model.clickDecayMs * params.clickDecayScale * 8.0f;
        endMs = std::max ({ ampEnd, noiseEnd, clickEnd, 5.0f }) + 3.0f;

        phase = 0.0;
        timeMs = 0.0;
        stepMs = 1000.0 / sr;

        // Click
        clickCoef = std::exp (-(float) stepMs / std::max (model.clickDecayMs * params.clickDecayScale, 0.05f));
        clickEnv = 1.0f;
        const float wc = (float) (2.0 * 3.14159265358979 * std::clamp (model.clickHz, 100.0f, (float) (0.45 * sr)) / sr);
        hpA = 1.0f - std::exp (-wc);
        const float pw = 1.0f - (wc / 3.14159265f) * std::atan (3.14159265f / wc);
        clickNorm = 1.0f / std::sqrt (std::max (pw, 0.05f));
        hpL = hpR = 0.0f;

        // Ruido: SVF paso banda (TPT) con ganancia que normaliza su RMS a 1
        const float fc = std::clamp (model.noiseHz * std::pow (2.0f, params.noiseToneSt / 12.0f), 40.0f, (float) (0.45 * sr));
        const float q = std::clamp (model.noiseQ, 0.25f, 12.0f);
        const float g = std::tan (3.14159265f * fc / (float) sr);
        svfK = 1.0f / q;
        svfA1 = 1.0f / (1.0f + g * (g + svfK));
        svfA2 = g * svfA1;
        svfA3 = g * svfA2;
        // Potencia del filtro sobre ruido blanco: (1/pi) * integral de |H|^2 dw, con la sustitucion
        // u = tan(w/2)/g (respuesta exacta del filtro digital, tambien cerca de Nyquist).
        {
            const double kk = (double) svfK, gg = (double) g;
            constexpr int G = 600;
            const double lo = std::log (1.0e-4), hi = std::log (1.0e4), dl = (hi - lo) / G;
            double sum = 0.0;
            for (int i = 0; i < G; ++i)
            {
                const double u = std::exp (lo + dl * (i + 0.5));
                const double d = 1.0 - u * u;
                const double h2 = kk * kk * u * u / (d * d + kk * kk * u * u);
                sum += h2 * (2.0 * gg / (1.0 + gg * gg * u * u)) * u * dl;
            }
            const float power = (float) (sum / 3.14159265358979);
            noiseNorm = std::min (1.0f / std::sqrt (std::max (power, 1.0e-6f)), 60.0f);
        }
        ic1L = ic2L = ic1R = ic2R = 0.0f;

        seedClickL = 22222u; seedClickR = 91919u;
        seedNoiseL = 777u;   seedNoiseR = 31337u;

        // Ancho estereo: matriz mid/side con potencia constante
        const float b = std::clamp (params.width, 0.0f, 1.4f);
        wB = b;
        wA = std::sqrt (std::max (2.0f - b * b, 0.0f));

        release = 1.0f;
        releaseStep = 0.0f;
        active = model.valid && model.amp.n > 0;
    }

    // Cierre rapido (retrigger) para evitar chasquidos.
    void fadeOut (float ms)
    {
        if (! active) return;
        releaseStep = (float) (1.0 / std::max (1.0, sr * ms * 0.001));
    }

    // Devuelve la muestra estereo (no suma sobre las salidas).
    void tick (float& outL, float& outR)
    {
        outL = outR = 0.0f;
        if (! active) return;

        const float tEnv = (float) timeMs;

        // --- Cuerpo
        const float a = evalAmp (model.amp, tEnv / params.decayScale);
        float f = evalPitchHz (model.pitch, tEnv);
        f = fEnd * std::pow (std::max (f / fEnd, 1.0e-3f), params.pitchAmount);
        f = std::clamp (f * transpose, 10.0f, (float) (sr * 0.45));
        phase += (double) f / sr;
        phase -= std::floor (phase);
        float s = (float) std::sin (6.283185307179586 * phase);
        if (params.harmonics > 0.001f)
        {
            const float k = 1.0f + 14.0f * params.harmonics;
            s = std::tanh (k * s) / std::tanh (k);
        }
        const float body = a * model.bodyGain * s;

        // --- Click
        float cL = 0.0f, cR = 0.0f;
        if (model.clickLevel > 1.0e-4f && params.clickGain > 0.0f)
        {
            const float nl = nextNoise (seedClickL), nr = nextNoise (seedClickR);
            hpL += hpA * (nl - hpL);
            hpR += hpA * (nr - hpR);
            const float amp = clickEnv * model.clickLevel * params.clickGain * clickNorm;
            cL = (nl - hpL) * amp;
            cR = (nr - hpR) * amp;
            clickEnv *= clickCoef;
        }

        // --- Ruido por paso banda
        float nL = 0.0f, nR = 0.0f;
        const float nEnv = evalAmp (model.noise, tEnv / params.noiseDecayScale);
        if (nEnv > 0.0f && params.noiseGain > 0.0f)
        {
            const float amp = nEnv * params.noiseGain * noiseNorm;
            nL = bandpass (nextNoise (seedNoiseL), ic1L, ic2L) * amp;
            nR = bandpass (nextNoise (seedNoiseR), ic1R, ic2R) * amp;
        }
        else
        {
            // mantiene el filtro "en marcha" sin energia para evitar saltos
            bandpass (0.0f, ic1L, ic2L);
            bandpass (0.0f, ic1R, ic2R);
        }

        const float sL = cL + nL, sR = cR + nR;
        const float sum = sL + sR, diff = sL - sR;
        float L = body + 0.5f * (wA * sum + wB * diff);
        float R = body + 0.5f * (wA * sum - wB * diff);

        // --- Saturacion (conserva el pico del cuerpo)
        if (params.driveDb > 0.05f)
        {
            const float g = std::max (std::pow (10.0f, params.driveDb / 20.0f) - 1.0f, 1.0e-3f);
            const float P = std::max (model.bodyGain, 0.05f);
            const float th = std::tanh (g);
            L = P * std::tanh (g * L / P) / th;
            R = P * std::tanh (g * R / P) / th;
        }

        // --- Final suave y cierre por retrigger
        const float endFade = std::clamp ((endMs - tEnv) / 3.0f, 0.0f, 1.0f);
        const float k = velGain * endFade * release;
        outL = L * k;
        outR = R * k;

        timeMs += stepMs;
        if (releaseStep > 0.0f)
        {
            release -= releaseStep;
            if (release <= 0.0f) { active = false; return; }
        }
        if (timeMs >= endMs) active = false;
    }

private:
    static float nextNoise (uint32_t& s)
    {
        s = s * 1664525u + 1013904223u;
        return (float) ((int32_t) s) * (1.7320508f / 2147483648.0f);   // ruido uniforme con RMS = 1
    }

    float bandpass (float v0, float& ic1, float& ic2) const
    {
        const float v3 = v0 - ic2;
        const float v1 = svfA1 * ic1 + svfA2 * v3;
        const float v2 = ic2 + svfA2 * ic1 + svfA3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return svfK * v1;   // paso banda de ganancia unidad en el centro
    }

    KickModel model;
    KickParams params;
    double sr = 44100.0, phase = 0.0, timeMs = 0.0, stepMs = 0.0;
    float transpose = 1.0f, velGain = 1.0f, fEnd = 50.0f, endMs = 100.0f;
    float clickEnv = 1.0f, clickCoef = 0.0f, hpA = 0.1f, hpL = 0.0f, hpR = 0.0f, clickNorm = 1.0f;
    float svfK = 1.0f, svfA1 = 0.0f, svfA2 = 0.0f, svfA3 = 0.0f, noiseNorm = 1.0f;
    float ic1L = 0.0f, ic2L = 0.0f, ic1R = 0.0f, ic2R = 0.0f;
    float wA = 1.0f, wB = 1.0f;
    float release = 1.0f, releaseStep = 0.0f;
    uint32_t seedClickL = 22222u, seedClickR = 91919u, seedNoiseL = 777u, seedNoiseR = 31337u;
    bool active = false;
};

// Dinamica de salida: compresor enlazado (opcional) + limitador suave siempre activo.
class PostChain
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        atk = std::exp (-1.0f / (float) (0.005 * sr));
        rel = std::exp (-1.0f / (float) (0.120 * sr));
        env = 0.0f;
    }

    void reset() { env = 0.0f; }
    void setAmount (float d) { amount = std::clamp (d, 0.0f, 1.0f); }

    void process (float* l, float* r, int n)
    {
        const bool comp = amount > 0.001f;
        const float thrDb = -4.0f - 20.0f * amount;
        const float ratio = 1.0f + 5.0f * amount;
        const float makeup = std::pow (10.0f, 0.55f * (-thrDb) * (1.0f - 1.0f / ratio) / 20.0f);
        constexpr float knee = 6.0f;

        for (int i = 0; i < n; ++i)
        {
            float a = l[i], b = r[i];
            if (comp)
            {
                const float det = std::max (std::fabs (a), std::fabs (b));
                env = det > env ? atk * env + (1.0f - atk) * det : rel * env + (1.0f - rel) * det;
                const float lvl = 20.0f * std::log10 (env + 1.0e-9f);
                const float over = lvl - thrDb;
                float grDb = 0.0f;
                if (2.0f * over > knee)       grDb = (1.0f / ratio - 1.0f) * over;
                else if (2.0f * over > -knee) grDb = (1.0f / ratio - 1.0f) * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
                const float g = std::pow (10.0f, grDb / 20.0f) * makeup;
                a *= g; b *= g;
            }
            l[i] = softLimit (a);
            r[i] = softLimit (b);
        }
    }

    // Transparente por debajo de -1 dBFS; asintota en 0 dBFS.
    static float softLimit (float x)
    {
        constexpr float knee = 0.89f, room = 0.11f;
        const float ax = std::fabs (x);
        if (ax <= knee) return x;
        const float y = knee + room * std::tanh ((ax - knee) / room);
        return x < 0.0f ? -y : y;
    }

private:
    double sr = 44100.0;
    float atk = 0.0f, rel = 0.0f, env = 0.0f, amount = 0.0f;
};

class KickEngine
{
public:
    static constexpr int kVoices = 4;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        for (auto& v : voices) v.prepare (sampleRate);
        next = 0;
    }

    void reset() { prepare (sr); }

    int numActive() const
    {
        int c = 0;
        for (auto& v : voices) c += v.isActive() ? 1 : 0;
        return c;
    }

    void noteOn (const KickModel& m, const KickParams& p, int midiNote, float velocity)
    {
        if (! p.poly)
            for (auto& v : voices) v.fadeOut (4.0f);
        int idx = -1;
        for (int i = 0; i < kVoices; ++i)
            if (! voices[i].isActive()) { idx = i; break; }
        if (idx < 0) idx = next;
        next = (idx + 1) % kVoices;
        voices[idx].start (m, p, midiNote, velocity);
    }

    // Suma en l y r.
    void render (float* l, float* r, int numSamples)
    {
        for (auto& v : voices)
        {
            if (! v.isActive()) continue;
            for (int i = 0; i < numSamples && v.isActive(); ++i)
            {
                float a, b;
                v.tick (a, b);
                l[i] += a;
                r[i] += b;
            }
        }
    }

private:
    KickVoice voices[kVoices];
    int next = 0;
    double sr = 44100.0;
};

struct StereoBuffer
{
    std::vector<float> l, r;
    std::vector<float> mid() const
    {
        std::vector<float> m (l.size());
        for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5f * (l[i] + r[i]);
        return m;
    }
};

// Renderizado offline de un golpe con toda la cadena (vista previa, exportacion, tests).
inline StereoBuffer renderOneShot (const KickModel& m, const KickParams& p, double sr,
                                   int midiNote = 36, float velocity = 1.0f)
{
    KickVoice v;
    v.prepare (sr);
    v.start (m, p, midiNote, velocity);
    const float ampEnd = m.amp.endMs() * p.decayScale;
    const float noiseEnd = m.noise.endMs() * p.noiseDecayScale;
    const float endMs = std::max ({ ampEnd, noiseEnd, m.clickDecayMs * p.clickDecayScale * 8.0f, 5.0f }) + 5.0f;

    StereoBuffer out;
    const size_t len = (size_t) std::max (16.0, endMs * 0.001 * sr);
    out.l.assign (len, 0.0f);
    out.r.assign (len, 0.0f);
    for (size_t i = 0; i < len; ++i)
        v.tick (out.l[i], out.r[i]);

    PostChain post;
    post.prepare (sr);
    post.setAmount (p.dynamics);
    post.process (out.l.data(), out.r.data(), (int) len);
    return out;
}
} // namespace kc
