#pragma once
// Motor de sintesis del bombo: seno con envolvente de tono + envolvente de
// amplitud + click + ruido + saturacion. Sin asignaciones de memoria en el
// hilo de audio.

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
        endMs = (model.amp.n > 0 ? model.amp.t[model.amp.n - 1] : 100.0f) * params.decayScale + 3.0f;

        phase = 0.0;
        timeMs = 0.0;
        stepMs = 1000.0 / sr;
        clickEnv = 1.0f;
        noiseEnv = 1.0f;
        clickCoef = std::exp (-(float) stepMs / std::max (model.clickDecayMs, 0.05f));
        noiseCoef = std::exp (-(float) stepMs / std::max (model.noiseDecayMs, 1.0f));
        hpA = 1.0f - std::exp (-2.0f * 3.14159265f * 1500.0f / (float) sr);
        hpClick = hpNoise = 0.0f;
        seedClick = 22222u;
        seedNoise = 777u;
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

    float tick()
    {
        if (! active) return 0.0f;

        const float tEnv = (float) timeMs;
        const float a = evalAmp (model.amp, tEnv / params.decayScale);

        // Tono
        float f = evalPitchHz (model.pitch, tEnv);
        f = fEnd * std::pow (std::max (f / fEnd, 1.0e-3f), params.pitchAmount);
        f = std::clamp (f * transpose, 10.0f, (float) (sr * 0.45));
        phase += (double) f / sr;
        phase -= std::floor (phase);
        float body = a * (float) std::sin (6.283185307179586 * phase);

        // Click (ruido blanco filtrado paso alto, decae rapido)
        float click = 0.0f;
        if (model.clickLevel > 1.0e-4f && params.clickGain > 0.0f)
        {
            const float n = nextNoise (seedClick);
            hpClick += hpA * (n - hpClick);
            click = (n - hpClick) * clickEnv * model.clickLevel * params.clickGain;
            clickEnv *= clickCoef;
        }

        // Cola de ruido
        float noise = 0.0f;
        if (model.noiseLevel > 1.0e-4f && params.noiseGain > 0.0f)
        {
            const float n = nextNoise (seedNoise);
            hpNoise += hpA * (n - hpNoise);
            noise = (n - hpNoise) * noiseEnv * model.noiseLevel * params.noiseGain;
            noiseEnv *= noiseCoef;
        }

        float y = body + click + noise;

        // Saturacion que conserva el pico del cuerpo
        if (params.driveDb > 0.05f)
        {
            const float g = std::pow (10.0f, params.driveDb / 20.0f);
            const float ref = std::tanh (g * std::max (model.bodyGain, 0.05f));
            y = std::tanh (g * y * model.bodyGain) / std::max (ref, 1.0e-3f) * model.bodyGain;
        }
        else
            y *= model.bodyGain;

        // Final suave y cierre por retrigger
        const float endFade = std::clamp ((endMs - tEnv) / 3.0f, 0.0f, 1.0f);
        y *= velGain * endFade * release;

        timeMs += stepMs;
        if (releaseStep > 0.0f)
        {
            release -= releaseStep;
            if (release <= 0.0f) { active = false; return y; }
        }
        if (timeMs >= endMs) active = false;
        return y;
    }

private:
    static float nextNoise (uint32_t& s)
    {
        s = s * 1664525u + 1013904223u;
        return (float) ((int32_t) s) * (1.0f / 2147483648.0f);
    }

    KickModel model;
    KickParams params;
    double sr = 44100.0, phase = 0.0, timeMs = 0.0, stepMs = 0.0;
    float transpose = 1.0f, velGain = 1.0f, fEnd = 50.0f, endMs = 100.0f;
    float clickEnv = 1.0f, noiseEnv = 1.0f, clickCoef = 0.0f, noiseCoef = 0.0f;
    float hpA = 0.1f, hpClick = 0.0f, hpNoise = 0.0f;
    float release = 1.0f, releaseStep = 0.0f;
    uint32_t seedClick = 22222u, seedNoise = 777u;
    bool active = false;
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

    void noteOn (const KickModel& m, const KickParams& p, int midiNote, float velocity)
    {
        for (auto& v : voices) v.fadeOut (4.0f);
        int idx = -1;
        for (int i = 0; i < kVoices; ++i)
            if (! voices[i].isActive()) { idx = i; break; }
        if (idx < 0) idx = next;
        next = (idx + 1) % kVoices;
        voices[idx].start (m, p, midiNote, velocity);
    }

    // Suma en 'out' (mono).
    void render (float* out, int numSamples)
    {
        for (auto& v : voices)
        {
            if (! v.isActive()) continue;
            for (int i = 0; i < numSamples && v.isActive(); ++i)
                out[i] += v.tick();
        }
    }

private:
    KickVoice voices[kVoices];
    int next = 0;
    double sr = 44100.0;
};

// Renderizado offline de un golpe (vista previa, exportacion, tests).
inline std::vector<float> renderOneShot (const KickModel& m, const KickParams& p, double sr,
                                         int midiNote = 36, float velocity = 1.0f)
{
    KickVoice v;
    v.prepare (sr);
    v.start (m, p, midiNote, velocity);
    const float endMs = (m.amp.n > 0 ? m.amp.t[m.amp.n - 1] : 100.0f) * p.decayScale + 5.0f;
    std::vector<float> out ((size_t) std::max (16.0, endMs * 0.001 * sr), 0.0f);
    for (auto& s : out)
        s = v.tick();
    return out;
}
} // namespace kc
