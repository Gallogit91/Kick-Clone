// Pruebas del nucleo DSP (sin JUCE).  Compilar:  g++ -std=c++17 -O2 -I Source/Dsp tests/dsp_test.cpp
#include "KickAnalyzer.h"
#include "KickEngine.h"
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace kc;

static int failures = 0;
#define CHECK(cond, msg) do { if (! (cond)) { std::printf ("  FALLO: %s\n", msg); ++failures; } else std::printf ("  ok: %s\n", msg); } while (0)

// Bombo sintetico con parametros conocidos, anadido a 'buf' en la posicion 'at'.
static void addKick (std::vector<float>& buf, double sr, int at, double f0, double fEnd, double tauPitchMs,
                     double tauAmpMs, double clickAmp, double gain)
{
    std::mt19937 rng (1);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    double phase = 0.0;
    const int len = (int) (sr * 0.8);
    for (int i = 0; i < len && at + i < (int) buf.size(); ++i)
    {
        const double t = i / sr;
        const double f = fEnd + (f0 - fEnd) * std::exp (-t * 1000.0 / tauPitchMs);
        phase += f / sr;
        double y = std::sin (2.0 * 3.14159265358979 * phase) * std::exp (-t * 1000.0 / tauAmpMs);
        y += clickAmp * u (rng) * std::exp (-t * 1000.0 / 1.5);
        buf[(size_t) (at + i)] += (float) (y * gain);
    }
}

static float rmsOf (const std::vector<float>& v, size_t a, size_t b)
{
    double s = 0; b = std::min (b, v.size());
    for (size_t i = a; i < b; ++i) s += (double) v[i] * v[i];
    return (float) std::sqrt (s / std::max<size_t> (1, b - a));
}

int main()
{
    const double sr = 48000.0;
    int failuresBefore = 0;

    // ---------- Test 1: bombo aislado (200 -> 50 Hz) ----------
    std::printf ("Test 1: bombo aislado, 200->50 Hz, tau amp 90 ms\n");
    std::vector<float> a ((size_t) (sr * 2.0), 0.0f);
    const int at = (int) (sr * 0.3);
    addKick (a, sr, at, 200.0, 50.0, 20.0, 90.0, 0.25, 0.8);

    int start = -1;
    KickModel m = analyzeKick (a.data(), (int) a.size(), sr, 0, &start);
    CHECK (m.valid, "modelo valido");
    CHECK (std::abs (start - at) < (int) (sr * 0.004), "inicio detectado a +-4 ms");
    const float fEnd = m.pitch.v[m.pitch.n - 1];
    std::printf ("    fEnd=%.1f Hz, pitch pts=%d, amp pts=%d, len=%.0f ms, body=%.2f, click=%.2f/%.1fms, noise=%.2f\n",
                 fEnd, m.pitch.n, m.amp.n, m.lengthMs, m.bodyGain, m.clickLevel, m.clickDecayMs, m.noiseLevel);
    CHECK (std::abs (fEnd - 50.0f) < 4.0f, "tono final ~50 Hz (+-4)");
    const float f20 = evalPitchHz (m.pitch, 20.0f);
    const float expect20 = 50.0f + 150.0f * std::exp (-1.0f);   // 105 Hz
    std::printf ("    f(20ms)=%.1f (esperado %.1f)\n", f20, expect20);
    CHECK (std::abs (f20 - expect20) < 0.2f * expect20, "tono a 20 ms ~105 Hz (+-20%)");
    CHECK (m.bodyGain > 0.5f && m.bodyGain <= 1.0f, "ganancia de cuerpo razonable");
    CHECK (m.clickLevel > 0.05f, "click detectado");
    // amplitud: a los 90 ms debe haber caido ~ -8.7 dB (e^-1)
    const float a90 = evalAmp (m.amp, 90.0f) / std::max (evalAmp (m.amp, 0.0f), 1e-6f);
    std::printf ("    amp(90)/amp(0)=%.2f (esperado ~0.37)\n", a90);
    CHECK (a90 > 0.2f && a90 < 0.6f, "caida de amplitud a 90 ms coherente");

    // ---------- Test 2: resintesis comparable ----------
    std::printf ("Test 2: resintesis vs original\n");
    KickParams p;
    std::vector<float> r = renderOneShot (m, p, sr);
    std::vector<float> orig (a.begin() + at, a.begin() + at + (long) r.size());
    // correlacion normalizada sobre los primeros 150 ms
    const size_t N = std::min<size_t> (r.size(), (size_t) (sr * 0.15));
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < N; ++i) { ab += orig[i] * r[i]; aa += orig[i] * orig[i]; bb += r[i] * r[i]; }
    const double corr = ab / std::sqrt (aa * bb + 1e-12);
    const float rmsO = rmsOf (orig, 0, N), rmsR = rmsOf (r, 0, N);
    std::printf ("    correlacion(150ms)=%.2f, RMS orig=%.3f clon=%.3f\n", corr, rmsO, rmsR);
    CHECK (rmsR > 0.5f * rmsO && rmsR < 1.6f * rmsO, "nivel RMS del clon cercano al original");
    CHECK (corr > 0.5, "correlacion de forma de onda > 0.5");
    float peak = 0; bool finite = true;
    for (float s : r) { peak = std::max (peak, std::fabs (s)); finite = finite && std::isfinite (s); }
    CHECK (finite && peak <= 1.2f, "sin NaN y sin pico excesivo");
    CHECK (std::fabs (r.back()) < 1e-3f, "el golpe termina en silencio (sin chasquido final)");

    // ---------- Test 3: bucle con dos bombos + ruido de fondo, buscar el 2o ----------
    std::printf ("Test 3: segundo bombo de un bucle (searchFrom)\n");
    std::vector<float> loop ((size_t) (sr * 3.0), 0.0f);
    std::mt19937 rng (5);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    for (auto& s : loop) s = 0.002f * u (rng);
    addKick (loop, sr, (int) (sr * 0.2), 180.0, 55.0, 25.0, 100.0, 0.2, 0.7);
    addKick (loop, sr, (int) (sr * 1.5), 220.0, 45.0, 18.0, 110.0, 0.2, 0.8);
    int s2 = -1;
    KickModel m2 = analyzeKick (loop.data(), (int) loop.size(), sr, (int) (sr * 1.4), &s2);
    CHECK (m2.valid, "modelo valido");
    CHECK (std::abs (s2 - (int) (sr * 1.5)) < (int) (sr * 0.005), "detecta el 2o bombo (+-5 ms)");
    CHECK (std::abs (m2.pitch.v[m2.pitch.n - 1] - 45.0f) < 5.0f, "tono final ~45 Hz");
    int s1 = -1;
    KickModel m1 = analyzeKick (loop.data(), (int) loop.size(), sr, 0, &s1);
    CHECK (m1.valid && std::abs (s1 - (int) (sr * 0.2)) < (int) (sr * 0.005), "por defecto detecta el 1er bombo");
    CHECK (m1.lengthMs < 1000.0f, "longitud limitada al eje");

    // ---------- Test 4: entradas degeneradas ----------
    std::printf ("Test 4: entradas degeneradas\n");
    std::vector<float> silence ((size_t) sr, 0.0f);
    CHECK (! analyzeKick (silence.data(), (int) silence.size(), sr).valid, "silencio -> no valido");
    std::vector<float> tiny (100, 0.5f);
    CHECK (! analyzeKick (tiny.data(), (int) tiny.size(), sr).valid, "audio demasiado corto -> no valido");
    CHECK (! analyzeKick (nullptr, 0, sr).valid, "nullptr -> no valido");
    std::vector<float> noise ((size_t) (sr * 1.0));
    for (auto& s : noise) s = u (rng);
    KickModel mn = analyzeKick (noise.data(), (int) noise.size(), sr);
    std::printf ("    ruido blanco: valid=%d (no debe colgarse ni dar NaN)\n", (int) mn.valid);
    if (mn.valid)
    {
        auto rn = renderOneShot (mn, p, sr);
        bool ok = true; for (float s : rn) ok = ok && std::isfinite (s);
        CHECK (ok, "renderizado finito");
    }

    // ---------- Test 5: motor (voces, retrigger, tune, parametros extremos) ----------
    std::printf ("Test 5: motor\n");
    KickModel d = KickModel::makeDefault();
    KickEngine eng;
    eng.prepare (sr);
    std::vector<float> out ((size_t) sr, 0.0f);
    eng.noteOn (d, p, 36, 1.0f);
    eng.render (out.data(), (int) (sr * 0.1));
    eng.noteOn (d, p, 36, 1.0f);   // retrigger a mitad
    eng.render (out.data() + (int) (sr * 0.1), (int) (sr * 0.5));
    bool fin = true; float pk = 0;
    for (float s : out) { fin = fin && std::isfinite (s); pk = std::max (pk, std::fabs (s)); }
    CHECK (fin && pk > 0.3f && pk <= 1.2f, "motor: salida finita y con nivel");
    // salto maximo entre muestras en el retrigger: sin chasquido enorme
    float maxJump = 0;
    for (size_t i = (size_t) (sr * 0.095); i < (size_t) (sr * 0.11); ++i)
        maxJump = std::max (maxJump, std::fabs (out[i + 1] - out[i]));
    std::printf ("    salto max en retrigger=%.3f\n", maxJump);
    CHECK (maxJump < 0.35f, "retrigger sin salto brusco");

    KickParams ext; ext.tuneSemitones = 12; ext.keytrack = 1; ext.pitchAmount = 2; ext.decayScale = 4;
    ext.clickGain = 2; ext.noiseGain = 2; ext.driveDb = 24;
    auto re = renderOneShot (d, ext, sr, 60, 0.3f);
    bool fe = true; float pe = 0; for (float s : re) { fe = fe && std::isfinite (s); pe = std::max (pe, std::fabs (s)); }
    CHECK (fe && pe <= 1.5f, "parametros extremos: finito y acotado");

    // tune +12 st duplica la frecuencia: cuenta de cruces por cero en 100..300 ms
    KickParams t0, t12; t12.tuneSemitones = 12.0f;
    auto z0 = renderOneShot (d, t0, sr), z12 = renderOneShot (d, t12, sr);
    auto crossings = [&] (const std::vector<float>& v) {
        int c = 0;
        for (size_t i = (size_t) (sr * 0.15); i + 1 < (size_t) (sr * 0.25); ++i)
            if ((v[i] < 0) != (v[i + 1] < 0)) ++c;
        return c;
    };
    const int c0 = crossings (z0), c12 = crossings (z12);
    std::printf ("    cruces 0 st=%d, +12 st=%d\n", c0, c12);
    CHECK (c0 > 0 && std::abs (c12 - 2 * c0) <= std::max (2, c0 / 5), "+12 semitonos ~ doble de frecuencia");

    std::printf ("\n%s (%d fallos)\n", failures == 0 ? "TODO CORRECTO" : "HAY FALLOS", failures);
    (void) failuresBefore;
    return failures == 0 ? 0 : 1;
}
