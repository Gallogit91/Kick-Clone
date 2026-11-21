// Pruebas del nucleo DSP (sin JUCE).  Compilar:  g++ -std=c++17 -O2 -I Source/Dsp tests/dsp_test.cpp
#include "KickAnalyzer.h"
#include "KickEngine.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace kc;

static int failures = 0;
#define CHECK(cond, msg) do { if (! (cond)) { std::printf ("  FALLO: %s\n", msg); ++failures; } else std::printf ("  ok: %s\n", msg); } while (0)

// ---------------------------------------------------------------- utilidades
static double sr = 48000.0;

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    static Biquad bandpass (double fc, double q, double fs)
    {
        const double w = 2.0 * kPi * fc / fs, al = std::sin (w) / (2.0 * q), a0 = 1.0 + al;
        Biquad f; f.b0 = al / a0; f.b1 = 0; f.b2 = -al / a0; f.a1 = -2.0 * std::cos (w) / a0; f.a2 = (1.0 - al) / a0;
        return f;
    }
    static Biquad highpass (double fc, double fs)
    {
        const double w = 2.0 * kPi * fc / fs, c = std::cos (w), al = std::sin (w) / (2.0 * 0.7071), a0 = 1.0 + al;
        Biquad f; f.b0 = (1 + c) / 2 / a0; f.b1 = -(1 + c) / a0; f.b2 = (1 + c) / 2 / a0; f.a1 = -2.0 * c / a0; f.a2 = (1.0 - al) / a0;
        return f;
    }
    double process (double x)
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

struct KickSpec
{
    double f0 = 200, fEnd = 50, tauPitchMs = 20, tauAmpMs = 90, clickAmp = 0.25, gain = 0.8;
    double noiseAmp = 0.0, noiseHz = 3000, noiseQ = 2.0, noiseTauMs = 40;
};

static void addKick (std::vector<float>& buf, int at, const KickSpec& k, unsigned seed = 1)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    Biquad bp = Biquad::bandpass (k.noiseHz, k.noiseQ, sr);
    double phase = 0.0;
    const int len = (int) (sr * 0.8);
    for (int i = 0; i < len && at + i < (int) buf.size(); ++i)
    {
        const double t = i / sr;
        const double f = k.fEnd + (k.f0 - k.fEnd) * std::exp (-t * 1000.0 / k.tauPitchMs);
        phase += f / sr;
        double y = std::sin (2.0 * kPi * phase) * std::exp (-t * 1000.0 / k.tauAmpMs);
        y += k.clickAmp * u (rng) * std::exp (-t * 1000.0 / 1.5);
        if (k.noiseAmp > 0.0) y += k.noiseAmp * bp.process (u (rng)) * std::exp (-t * 1000.0 / k.noiseTauMs);
        buf[(size_t) (at + i)] += (float) (y * k.gain);
    }
}

static void addHat (std::vector<float>& buf, int at, double amp, double durMs = 40.0, unsigned seed = 9)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    Biquad hp = Biquad::highpass (6000.0, sr);
    const int len = (int) (sr * durMs * 0.001);
    for (int i = 0; i < len && at + i < (int) buf.size(); ++i)
        buf[(size_t) (at + i)] += (float) (amp * hp.process (u (rng)) * std::exp (-(double) i / (sr * 0.008)));
}

static void addSnare (std::vector<float>& buf, int at, double amp, unsigned seed = 4)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    Biquad bp = Biquad::bandpass (2500.0, 0.6, sr);
    double ph = 0.0;
    for (int i = 0; i < (int) (sr * 0.2) && at + i < (int) buf.size(); ++i)
    {
        const double t = i / sr;
        ph += 190.0 / sr;
        buf[(size_t) (at + i)] += (float) (amp * (0.5 * std::sin (2 * kPi * ph) * std::exp (-t / 0.05) + 0.9 * bp.process (u (rng)) * std::exp (-t / 0.07)));
    }
}

static float rmsOf (const std::vector<float>& v, size_t a, size_t b)
{
    double s = 0; b = std::min (b, v.size());
    for (size_t i = a; i < b; ++i) s += (double) v[i] * v[i];
    return b > a ? (float) std::sqrt (s / (double) (b - a)) : 0.0f;
}

static double corrOf (const std::vector<float>& a, const std::vector<float>& b, size_t n0, size_t n1)
{
    n1 = std::min ({ n1, a.size(), b.size() });
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = n0; i < n1; ++i) { ab += (double) a[i] * b[i]; aa += (double) a[i] * a[i]; bb += (double) b[i] * b[i]; }
    return ab / std::sqrt (aa * bb + 1e-30);
}

static float peakOf (const std::vector<float>& v)
{
    float p = 0; for (float s : v) p = std::max (p, std::fabs (s)); return p;
}

static bool allFinite (const std::vector<float>& v)
{
    for (float s : v) if (! std::isfinite (s)) return false;
    return true;
}

static double goertzelDb (const std::vector<float>& x, size_t a, size_t b, double f)
{
    double c = 2.0 * std::cos (2.0 * kPi * f / sr), s1 = 0, s2 = 0;
    for (size_t i = a; i < b; ++i) { const double s = x[i] * (0.5 - 0.5 * std::cos (2 * kPi * (double) (i - a) / (double) (b - a))) + c * s1 - s2; s2 = s1; s1 = s; }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 10.0 * std::log10 (p + 1e-30);
}

static double nowSec()
{
    return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main()
{
    std::mt19937 rng (5);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);

    // ====================================================== 1. FFT / STFT
    std::printf ("Test 1: FFT y STFT\n");
    {
        Fft fft (1024);
        std::vector<Cpx> a (1024);
        for (int i = 0; i < 1024; ++i) a[(size_t) i].r = std::sin (2 * 3.14159265f * 50.0f * (float) i / 1024.0f);
        fft.forward (a.data());
        int pk = 0; float pv = 0;
        for (int k = 0; k < 512; ++k) { const float m = std::hypot (a[(size_t) k].r, a[(size_t) k].i); if (m > pv) { pv = m; pk = k; } }
        CHECK (pk == 50 && std::fabs (pv - 512.0f) < 1.0f, "FFT: seno de 50 ciclos -> bin 50 con amplitud N/2");
        fft.inverse (a.data());
        CHECK (std::fabs (a[100].r - std::sin (2 * 3.14159265f * 50.0f * 100.0f / 1024.0f)) < 1e-4f, "FFT inversa recupera la senal");

        std::vector<float> x (30001);
        for (auto& s : x) s = u (rng);
        Stft st (2048, 512);
        std::vector<Cpx> spec; st.analyze (x.data(), (int) x.size(), spec);
        std::vector<float> y; st.synthesize (spec, (int) x.size(), y);
        float maxErr = 0; for (size_t i = 0; i < x.size(); ++i) maxErr = std::max (maxErr, std::fabs (x[i] - y[i]));
        std::printf ("    error de reconstruccion STFT = %.2e\n", maxErr);
        CHECK (maxErr < 1e-4f, "STFT con mascara identidad reconstruye (error < 1e-4)");
    }

    // ====================================================== 2. Eventos en un bucle
    std::printf ("Test 2: deteccion de eventos en un bucle (2 bombos, hi-hat, caja)\n");
    std::vector<float> loop ((size_t) (sr * 3.0), 0.0f);
    for (auto& s : loop) s = 0.002f * u (rng);
    const int kA = (int) (sr * 0.20), hat = (int) (sr * 0.70), kB = (int) (sr * 1.20), snr = (int) (sr * 1.70);
    addKick (loop, kA, { 180, 55, 25, 100, 0.2, 0.7 });
    addHat (loop, hat, 0.5);
    addKick (loop, kB, { 220, 45, 18, 110, 0.2, 0.8 });
    addSnare (loop, snr, 0.7);
    auto ev = detectEvents (loop.data(), (int) loop.size(), sr);
    for (auto& e : ev) std::printf ("    evento @%.3f s  fuerza %.2f  lowRatio %.2f\n", e.sample / sr, e.strength, e.lowRatio);
    auto nearEv = [&] (int s) -> const DrumEvent* {
        for (auto& e : ev) if (std::abs (e.sample - s) < (int) (sr * 0.006)) return &e;
        return nullptr; };
    CHECK (nearEv (kA) && nearEv (kB) && nearEv (hat) && nearEv (snr), "detecta los 4 golpes (+-6 ms)");
    CHECK (ev.size() == 4, "ni mas ni menos que 4 eventos");
    if (nearEv (kA) && nearEv (hat) && nearEv (kB))
    {
        CHECK (nearEv (kA)->lowRatio > 0.6f && nearEv (kB)->lowRatio > 0.6f, "los bombos tienen lowRatio alto");
        CHECK (nearEv (hat)->lowRatio < 0.25f, "el hi-hat tiene lowRatio bajo");
    }
    auto an2 = analyzeKick (loop.data(), (int) loop.size(), sr, 0, &ev);
    CHECK (an2.model.valid && std::abs (an2.start - kA) < (int) (sr * 0.006), "por defecto elige el primer bombo");
    auto an2b = analyzeKick (loop.data(), (int) loop.size(), sr, (int) (sr * 0.9), &ev);
    CHECK (an2b.model.valid && std::abs (an2b.start - kB) < (int) (sr * 0.006), "searchFrom elige el segundo bombo");
    CHECK (std::abs (an2b.model.pitch.v[an2b.model.pitch.n - 1] - 45.0f) < 5.0f, "segundo bombo: tono final ~45 Hz");
    std::vector<float> hatOnly ((size_t) (sr * 1.0), 0.0f);
    addHat (hatOnly, (int) (sr * 0.3), 0.6);
    CHECK (! analyzeKick (hatOnly.data(), (int) hatOnly.size(), sr).model.valid, "solo un hi-hat -> no hay bombo");

    // ====================================================== 3. Bombo aislado
    std::printf ("Test 3: bombo aislado 200->50 Hz con click\n");
    std::vector<float> a1 ((size_t) (sr * 2.0), 0.0f);
    const int at = (int) (sr * 0.3);
    addKick (a1, at, { 200, 50, 20, 90, 0.25, 0.8 });
    auto an = analyzeKick (a1.data(), (int) a1.size(), sr);
    const KickModel& m = an.model;
    CHECK (m.valid, "modelo valido");
    CHECK (std::abs (an.start - at) < (int) (sr * 0.003), "inicio detectado a +-3 ms");
    std::printf ("    fEnd=%.1f Hz, pitch pts=%d, amp pts=%d, len=%.0f ms, body=%.2f, click=%.3f/%.1fms/%.0fHz, noise pts=%d\n",
                 m.pitch.v[m.pitch.n - 1], m.pitch.n, m.amp.n, m.lengthMs, m.bodyGain, m.clickLevel, m.clickDecayMs, m.clickHz, m.noise.n);
    CHECK (std::abs (m.pitch.v[m.pitch.n - 1] - 50.0f) < 4.0f, "tono final ~50 Hz (+-4)");
    const float f20 = evalPitchHz (m.pitch, 20.0f), expect20 = 50.0f + 150.0f * std::exp (-1.0f);
    std::printf ("    f(20ms)=%.1f (esperado %.1f)\n", f20, expect20);
    CHECK (std::abs (f20 - expect20) < 0.2f * expect20, "tono a 20 ms ~105 Hz (+-20%)");
    CHECK (m.bodyGain > 0.5f && m.bodyGain <= 1.0f, "ganancia de cuerpo razonable");
    CHECK (m.clickLevel > 0.05f, "click detectado");
    const float a90 = evalAmp (m.amp, 90.0f) / std::max (evalAmp (m.amp, 0.0f), 1e-6f);
    std::printf ("    amp(90)/amp(0)=%.2f (esperado ~0.37)\n", a90);
    CHECK (a90 > 0.2f && a90 < 0.6f, "caida de amplitud a 90 ms coherente");

    // ====================================================== 4. Capas: reconstruccion y separacion
    std::printf ("Test 4: capas aisladas\n");
    {
        const int len = (int) (sr * 0.4);
        auto L = isolateLayers (a1.data(), (int) a1.size(), sr, at, len, len);
        auto s = L.sum();
        double eErr = 0, eRef = 0;
        for (int i = (int) (sr * 0.005); i < len - (int) (sr * 0.02); ++i)
        {
            const double d = s[(size_t) i] - a1[(size_t) (at + i)];
            eErr += d * d; eRef += (double) a1[(size_t) (at + i)] * a1[(size_t) (at + i)];
        }
        std::printf ("    error relativo cuerpo+click+ruido vs original = %.2e\n", std::sqrt (eErr / eRef));
        CHECK (std::sqrt (eErr / eRef) < 1e-3, "las tres capas suman el original (error < 0.1 %)");
        CHECK (rmsOf (L.body, 0, (size_t) (sr * 0.05)) > 5.0f * rmsOf (L.click, 0, (size_t) (sr * 0.05)), "el cuerpo domina en graves");
        CHECK (rmsOf (L.click, 0, (size_t) (sr * 0.004)) > 3.0f * rmsOf (L.click, (size_t) (sr * 0.02), (size_t) (sr * 0.1)), "el click esta al inicio");
    }

    // ====================================================== 5. Aislamiento en mezcla con otros sonidos
    std::printf ("Test 5: aislamiento con hi-hat posterior y ruido de fondo\n");
    {
        std::vector<float> clean ((size_t) (sr * 1.5), 0.0f);
        const int p0 = (int) (sr * 0.3);
        addKick (clean, p0, { 190, 48, 22, 100, 0.2, 0.8, 0.3, 3000, 2.0, 50 });
        std::vector<float> mix = clean;
        const int hatAt = p0 + (int) (sr * 0.12);
        addHat (mix, hatAt, 0.45);
        addHat (mix, hatAt + (int) (sr * 0.2), 0.3);
        for (auto& s : mix) s += 0.003f * u (rng);

        auto evm = detectEvents (mix.data(), (int) mix.size(), sr);
        auto aClean = analyzeKick (clean.data(), (int) clean.size(), sr);
        auto aMix = analyzeKick (mix.data(), (int) mix.size(), sr, 0, &evm);
        CHECK (aClean.model.valid && aMix.model.valid, "ambos modelos validos");
        const size_t nCmp = (size_t) (sr * 0.2);
        const double cBody = corrOf (aClean.body, aMix.body, 0, nCmp);
        std::printf ("    correlacion del cuerpo limpio vs mezcla = %.4f\n", cBody);
        CHECK (cBody > 0.99, "el cuerpo aislado de la mezcla coincide con el del bombo limpio (>0.99)");
        const int hatRel = hatAt - aMix.start;
        const float nzAfter = rmsOf (aMix.noise, (size_t) (hatRel + (int) (sr * 0.002)), (size_t) (hatRel + (int) (sr * 0.05)));
        const float nzPeak = peakOf (aMix.noise);
        std::printf ("    ruido tras el hi-hat: rms=%.4f (pico de la capa %.3f)\n", nzAfter, nzPeak);
        CHECK (nzAfter < 0.02f, "el hi-hat no entra en la capa de ruido (se corta el evento)");
        CHECK (std::fabs (aMix.model.pitch.v[aMix.model.pitch.n - 1] - aClean.model.pitch.v[aClean.model.pitch.n - 1]) < 3.0f,
               "mismo tono final con y sin hi-hat");
    }

    // ====================================================== 6. Banda y envolvente del ruido
    std::printf ("Test 6: modelado del ruido (banda 3 kHz Q 2, tau 50 ms)\n");
    {
        std::vector<float> b ((size_t) (sr * 1.5), 0.0f);
        const int p0 = (int) (sr * 0.2);
        addKick (b, p0, { 180, 50, 20, 110, 0.15, 0.6, 0.35, 3000, 2.0, 50 });
        auto A = analyzeKick (b.data(), (int) b.size(), sr);
        std::printf ("    noiseHz=%.0f  noiseQ=%.2f  puntos=%d  noiseLen=%.0f ms\n", A.model.noiseHz, A.model.noiseQ, A.model.noise.n, A.model.noise.endMs());
        CHECK (A.model.valid && A.model.noiseHz > 2000.0f && A.model.noiseHz < 4500.0f, "centro del ruido ~3 kHz");
        CHECK (A.model.noiseQ > 0.8f, "ruido de banda estrecha (Q > 0.8)");
        const float r20 = evalAmp (A.model.noise, 20.0f), r80 = evalAmp (A.model.noise, 80.0f);
        std::printf ("    env ruido: 20 ms=%.4f  80 ms=%.4f (cociente esperado ~%.2f)\n", r20, r80, std::exp (-60.0 / 50.0));
        CHECK (r20 > 0.0f && r80 > 0.0f && r80 < r20, "envolvente de ruido decreciente");
        CHECK (r80 / r20 > 0.15f && r80 / r20 < 0.6f, "caida del ruido coherente con tau 50 ms");
    }

    // ====================================================== 7. Resintesis vs aislado
    std::printf ("Test 7: resintesis vs capa aislada\n");
    {
        std::vector<float> b ((size_t) (sr * 1.5), 0.0f);
        const int p0 = (int) (sr * 0.2);
        addKick (b, p0, { 200, 50, 20, 90, 0.25, 0.7, 0.2, 3000, 2.0, 40 });
        auto A = analyzeKick (b.data(), (int) b.size(), sr);
        KickParams p; p.width = 0.0f;
        auto r = renderOneShot (A.model, p, sr);
        auto mid = r.mid();
        const size_t N = (size_t) (sr * 0.15);
        const double corr = corrOf (A.isolated, mid, 0, N);
        const float rmsI = rmsOf (A.isolated, 0, N), rmsR = rmsOf (mid, 0, N);
        std::printf ("    correlacion(150ms)=%.2f  RMS aislado=%.3f  clon=%.3f (%.1f dB)\n", corr, rmsI, rmsR, 20 * std::log10 (rmsR / rmsI));
        CHECK (corr > 0.6, "correlacion de forma de onda > 0.6");
        CHECK (rmsR > 0.7f * rmsI && rmsR < 1.4f * rmsI, "nivel RMS del clon a +-3 dB del aislado");
        CHECK (allFinite (r.l) && allFinite (r.r), "salida finita");
        CHECK (peakOf (r.l) <= 1.0f && peakOf (r.r) <= 1.0f, "pico <= 0 dBFS");
        CHECK (std::fabs (r.l.back()) < 1e-3f, "termina en silencio");
        CHECK (std::fabs (r.l[0]) < 0.3f, "sin salto brusco al inicio");
    }

    // ====================================================== 8. Calibracion del motor
    std::printf ("Test 8: calibracion de ruido, click y armonicos\n");
    {
        for (auto cfg : { std::pair<float, float> { 500, 1.0f }, { 3000, 2.0f }, { 8000, 0.5f }, { 150, 4.0f }, { 12000, 1.0f } })
        {
            KickModel k;
            k.valid = true;
            k.pitch.add (0, 50); k.pitch.add (300, 50);
            k.amp.add (0, 0); k.amp.add (300, 0);
            k.clickLevel = 0.0f;
            k.noise.add (0, 0.1f); k.noise.add (400, 0.1f); k.noise.add (401, 0.0f);
            k.noiseHz = cfg.first; k.noiseQ = cfg.second;
            KickParams p; p.width = 1.0f;
            auto r = renderOneShot (k, p, sr);
            const float rm = rmsOf (r.l, (size_t) (sr * 0.05), (size_t) (sr * 0.35));
            char msg[96]; std::snprintf (msg, sizeof msg, "ruido %.0f Hz Q %.1f: RMS %.3f (objetivo 0.100) a +-3 dB", cfg.first, cfg.second, rm);
            CHECK (rm > 0.07f && rm < 0.142f, msg);
        }
        KickModel k;
        k.valid = true;
        k.pitch.add (0, 50); k.pitch.add (300, 50);
        k.amp.add (0, 0); k.amp.add (300, 0);
        k.noise.add (0, 0.0f); k.noise.add (10, 0.0f);
        k.clickLevel = 0.3f; k.clickDecayMs = 3.0f; k.clickHz = 2000.0f;
        KickParams p; p.width = 1.0f;
        auto r = renderOneShot (k, p, sr);
        const float rc = rmsOf (r.l, 0, (size_t) (sr * 0.0005));
        std::printf ("    click: RMS en 0.5 ms = %.3f (objetivo 0.300)\n", rc);
        CHECK (rc > 0.2f && rc < 0.42f, "nivel de click a +-3 dB");

        KickModel h;
        h.valid = true; h.pitch.add (0, 100); h.pitch.add (400, 100); h.amp.add (0, 1); h.amp.add (400, 1);
        h.noise.add (0, 0); h.noise.add (10, 0); h.clickLevel = 0; h.bodyGain = 0.8f;
        KickParams p0, p1; p1.harmonics = 1.0f;
        auto s0 = renderOneShot (h, p0, sr), s1 = renderOneShot (h, p1, sr);
        const double f1a = goertzelDb (s0.l, (size_t) (sr * 0.05), (size_t) (sr * 0.35), 100.0);
        const double f3a = goertzelDb (s0.l, (size_t) (sr * 0.05), (size_t) (sr * 0.35), 300.0);
        const double f1b = goertzelDb (s1.l, (size_t) (sr * 0.05), (size_t) (sr * 0.35), 100.0);
        const double f3b = goertzelDb (s1.l, (size_t) (sr * 0.05), (size_t) (sr * 0.35), 300.0);
        std::printf ("    3er armonico respecto al fundamental: harmonics 0 = %.1f dB, harmonics 100 = %.1f dB\n", f3a - f1a, f3b - f1b);
        CHECK (f3a - f1a < -60.0, "armonicos 0 %: seno puro (3er armonico < -60 dB)");
        CHECK (f3b - f1b > -20.0, "armonicos 100 %: 3er armonico > -20 dB");
        CHECK (peakOf (s1.l) <= 0.81f, "los armonicos conservan el pico del cuerpo");
    }

    // ====================================================== 9. Estereo, mono/poly, dinamica, tune
    std::printf ("Test 9: estereo, mono/poly, dinamica, parametros\n");
    {
        KickModel d = KickModel::makeDefault();
        d.noise.clear(); d.noise.add (0, 0.2f); d.noise.add (200, 0.2f); d.noise.add (201, 0.0f);
        KickParams p;
        p.width = 0.0f;
        auto m0 = renderOneShot (d, p, sr);
        double dd = 0; for (size_t i = 0; i < m0.l.size(); ++i) dd += std::fabs (m0.l[i] - m0.r[i]);
        CHECK (dd < 1e-3, "width 0 -> mono perfecto (L = R)");
        p.width = 1.0f;
        auto m1 = renderOneShot (d, p, sr);
        double dsum = 0, ddiff = 0;
        for (size_t i = 0; i < m1.l.size(); ++i) { dsum += std::fabs (m1.l[i] + m1.r[i]); ddiff += std::fabs (m1.l[i] - m1.r[i]); }
        std::printf ("    width 1: |L-R| / |L+R| = %.2f\n", ddiff / dsum);
        CHECK (ddiff / dsum > 0.15, "width 1 -> canales de ruido distintos");
        // potencia de ruido aproximadamente constante entre anchos
        p.width = 0.0f; const float e0 = rmsOf (renderOneShot (d, p, sr).l, (size_t) (sr * 0.2), (size_t) (sr * 0.4));
        p.width = 1.0f; const float e1 = rmsOf (renderOneShot (d, p, sr).l, (size_t) (sr * 0.2), (size_t) (sr * 0.4));
        std::printf ("    RMS de cola: width 0 = %.3f, width 1 = %.3f\n", e0, e1);
        CHECK (e0 > 0.0f && e1 / e0 > 0.6f && e1 / e0 < 1.7f, "el ancho no cambia mucho el nivel (potencia constante)");

        KickEngine eng; eng.prepare (sr);
        KickParams pm; pm.poly = false;
        eng.noteOn (d, pm, 36, 1.0f);
        std::vector<float> L ((size_t) (sr * 0.1), 0), R = L;
        eng.render (L.data(), R.data(), (int) (sr * 0.02));
        eng.noteOn (d, pm, 36, 1.0f);
        eng.render (L.data(), R.data(), (int) (sr * 0.02));
        eng.render (L.data(), R.data(), (int) (sr * 0.02));
        CHECK (eng.numActive() == 1, "modo mono: el retrigger corta la voz anterior");
        KickEngine eng2; eng2.prepare (sr);
        KickParams pp; pp.poly = true;
        eng2.noteOn (d, pp, 36, 1.0f);
        eng2.render (L.data(), R.data(), (int) (sr * 0.02));
        eng2.noteOn (d, pp, 40, 1.0f);
        eng2.render (L.data(), R.data(), (int) (sr * 0.02));
        CHECK (eng2.numActive() == 2, "modo poly: las voces suenan a la vez");

        // Solo cuerpo (sin click ni ruido, que son ruido blanco y saltan por naturaleza)
        KickModel bodyOnly = KickModel::makeDefault();
        bodyOnly.clickLevel = 0.0f;
        bodyOnly.noise.clear(); bodyOnly.noise.add (0, 0.0f); bodyOnly.noise.add (10, 0.0f);

        std::vector<float> out ((size_t) sr, 0.0f), outR = out;
        KickEngine e3; e3.prepare (sr);
        e3.noteOn (bodyOnly, pm, 36, 1.0f);
        e3.render (out.data(), outR.data(), (int) (sr * 0.1));
        e3.noteOn (bodyOnly, pm, 36, 1.0f);
        e3.render (out.data() + (int) (sr * 0.1), outR.data() + (int) (sr * 0.1), (int) (sr * 0.5));
        float maxJump = 0;
        for (size_t i = (size_t) (sr * 0.095); i < (size_t) (sr * 0.11); ++i) maxJump = std::max (maxJump, std::fabs (out[i + 1] - out[i]));
        std::printf ("    salto max en retrigger = %.3f\n", maxJump);
        CHECK (maxJump < 0.35f, "retrigger sin chasquido fuerte");

        KickParams ext; ext.tuneSemitones = 12; ext.keytrack = 1; ext.pitchAmount = 2; ext.decayScale = 4; ext.harmonics = 1;
        ext.clickGain = 2; ext.noiseGain = 2; ext.driveDb = 24; ext.width = 1.4f; ext.dynamics = 1.0f; ext.noiseToneSt = 24; ext.noiseDecayScale = 4; ext.clickDecayScale = 4;
        auto re = renderOneShot (d, ext, sr, 60, 0.3f);
        CHECK (allFinite (re.l) && allFinite (re.r) && peakOf (re.l) <= 1.0f && peakOf (re.r) <= 1.0f, "parametros extremos: finito y acotado a 0 dBFS");
        ext.noiseToneSt = -24; ext.tuneSemitones = -12; ext.pitchAmount = 0;
        auto re2 = renderOneShot (d, ext, sr, 24, 1.0f);
        CHECK (allFinite (re2.l) && peakOf (re2.l) <= 1.0f, "parametros extremos opuestos: finito y acotado");

        KickParams t0, t12; t12.tuneSemitones = 12.0f;
        auto z0 = renderOneShot (bodyOnly, t0, sr).l, z12 = renderOneShot (bodyOnly, t12, sr).l;
        auto crossings = [&] (const std::vector<float>& v) {
            int c = 0;
            for (size_t i = (size_t) (sr * 0.15); i + 1 < (size_t) (sr * 0.25); ++i)
                if ((v[i] < 0) != (v[i + 1] < 0)) ++c;
            return c; };
        const int c0 = crossings (z0), c12 = crossings (z12);
        std::printf ("    cruces 0 st=%d, +12 st=%d\n", c0, c12);
        CHECK (c0 > 0 && std::abs (c12 - 2 * c0) <= std::max (2, c0 / 5), "+12 semitonos ~ doble de frecuencia");

        // desplazamiento de la banda de ruido: +12 st -> centroide ~ doble
        KickModel nzm; nzm.valid = true; nzm.pitch.add (0, 50); nzm.pitch.add (300, 50); nzm.amp.add (0, 0); nzm.amp.add (300, 0);
        nzm.clickLevel = 0; nzm.noise.add (0, 0.1f); nzm.noise.add (400, 0.1f); nzm.noise.add (401, 0); nzm.noiseHz = 1500; nzm.noiseQ = 3;
        KickParams n0, n12; n12.noiseToneSt = 12;
        auto rn0 = renderOneShot (nzm, n0, sr).l, rn12 = renderOneShot (nzm, n12, sr).l;
        const auto s0 = detail::spectrumStats (rn0, (int) (sr * 0.05), (int) (sr * 0.35), sr, 100.0f);
        const auto s12 = detail::spectrumStats (rn12, (int) (sr * 0.05), (int) (sr * 0.35), sr, 100.0f);
        std::printf ("    centroide del ruido: %.0f Hz -> %.0f Hz con +12 st\n", s0.centroid, s12.centroid);
        CHECK (s12.centroid / s0.centroid > 1.6f && s12.centroid / s0.centroid < 2.5f, "noise tone +12 st ~ duplica la banda");

        // dinamica: ganancia en regimen estacionario con un seno de 100 Hz
        auto steadyGainDb = [&] (float amount, float inAmp) {
            PostChain pc; pc.prepare (sr); pc.setAmount (amount);
            std::vector<float> l ((size_t) sr), r;
            for (size_t i = 0; i < l.size(); ++i) l[i] = inAmp * (float) std::sin (2.0 * kPi * 100.0 * (double) i / sr);
            r = l;
            pc.process (l.data(), r.data(), (int) l.size());
            return 20.0 * std::log10 ((double) rmsOf (l, (size_t) (sr * 0.5), l.size()) / (double) (inAmp * 0.70710678f)); };
        const double g0Quiet = steadyGainDb (0.0f, 0.0316f), g0Loud = steadyGainDb (0.0f, 0.5f);
        const double g1Quiet = steadyGainDb (1.0f, 0.0316f), g1Loud = steadyGainDb (1.0f, 0.5f);
        std::printf ("    ganancia con dinamica 0 %%: -30 dBFS = %+.2f dB, -6 dBFS = %+.2f dB\n", g0Quiet, g0Loud);
        std::printf ("    ganancia con dinamica 100 %%: -30 dBFS = %+.2f dB, -6 dBFS = %+.2f dB\n", g1Quiet, g1Loud);
        CHECK (std::fabs (g0Quiet) < 0.05 && std::fabs (g0Loud) < 0.05, "dinamica 0 %: transparente (0 dB)");
        CHECK (g1Quiet - g1Loud > 5.0, "dinamica 100 %: comprime (el nivel alto recibe >5 dB menos ganancia que el bajo)");
        CHECK (g1Quiet > 0.0, "dinamica 100 %: el makeup sube las señales bajas");

        KickModel loud = KickModel::makeDefault(); loud.bodyGain = 1.0f;
        KickParams dp0, dp1; dp0.driveDb = 12; dp1.driveDb = 12; dp1.dynamics = 1.0f;
        auto l0 = renderOneShot (loud, dp0, sr).l, l1 = renderOneShot (loud, dp1, sr).l;
        std::printf ("    pico sin/con dinamica = %.3f / %.3f\n", peakOf (l0), peakOf (l1));
        CHECK (peakOf (l0) <= 1.0f && peakOf (l1) <= 1.0f, "limitador de seguridad: nunca supera 0 dBFS");
        CHECK (PostChain::softLimit (0.5f) == 0.5f && PostChain::softLimit (-0.8f) == -0.8f && PostChain::softLimit (5.0f) <= 1.0f && PostChain::softLimit (-5.0f) >= -1.0f,
               "softLimit: transparente por debajo de -1 dBFS y acotado");
    }

    // ====================================================== 10. Inicio exacto de un one-shot en la muestra 0 y entradas degeneradas
    std::printf ("Test 10: one-shot desde la muestra 0, entradas degeneradas\n");
    {
        std::vector<float> one ((size_t) (sr * 1.0), 0.0f);
        addKick (one, 0, { 170, 48, 20, 100, 0.2, 0.8 });
        auto ev1 = detectEvents (one.data(), (int) one.size(), sr);
        CHECK (! ev1.empty() && ev1[0].sample < (int) (sr * 0.003), "evento en la muestra 0 detectado");
        auto a0 = analyzeKick (one.data(), (int) one.size(), sr);
        CHECK (a0.model.valid && a0.start < (int) (sr * 0.003), "one-shot desde 0: analisis correcto");

        std::vector<float> silence ((size_t) sr, 0.0f);
        CHECK (! analyzeKick (silence.data(), (int) silence.size(), sr).model.valid, "silencio -> no valido");
        std::vector<float> tiny (100, 0.5f);
        CHECK (! analyzeKick (tiny.data(), (int) tiny.size(), sr).model.valid, "audio demasiado corto -> no valido");
        CHECK (! analyzeKick (nullptr, 0, sr).model.valid, "nullptr -> no valido");
        std::vector<float> dc ((size_t) sr, 0.6f);
        CHECK (! analyzeKick (dc.data(), (int) dc.size(), sr).model.valid || true, "DC constante no se cuelga");
        std::vector<float> noise ((size_t) sr);
        for (auto& s : noise) s = u (rng);
        auto an6 = analyzeKick (noise.data(), (int) noise.size(), sr);
        std::printf ("    ruido blanco: valid=%d (no debe colgarse ni dar NaN)\n", (int) an6.model.valid);
        if (an6.model.valid)
        {
            auto rn = renderOneShot (an6.model, KickParams {}, sr);
            CHECK (allFinite (rn.l) && allFinite (rn.r), "ruido blanco: renderizado finito");
        }
        std::vector<float> endKick ((size_t) (sr * 0.3), 0.0f);
        addKick (endKick, (int) (sr * 0.28), { 170, 48, 20, 100, 0.2, 0.8 });
        auto ae = analyzeKick (endKick.data(), (int) endKick.size(), sr);
        std::printf ("    golpe al final del audio: valid=%d\n", (int) ae.model.valid);
        CHECK (true, "golpe cortado al final no rompe");
        std::vector<float> clip ((size_t) (sr * 1.0), 0.0f);
        addKick (clip, (int) (sr * 0.2), { 190, 48, 22, 100, 0.2, 3.0 });
        for (auto& s : clip) s = std::clamp (s, -1.0f, 1.0f);
        auto ac = analyzeKick (clip.data(), (int) clip.size(), sr);
        CHECK (ac.model.valid && allFinite (renderOneShot (ac.model, KickParams {}, sr).l), "audio recortado (clipping): modelo y render finitos");
        for (double rate : { 22050.0, 44100.0, 96000.0, 192000.0 })
        {
            const double save = sr; sr = rate;
            std::vector<float> k2 ((size_t) (sr * 1.2), 0.0f);
            addKick (k2, (int) (sr * 0.15), { 190, 48, 22, 100, 0.2, 0.8 });
            auto ar = analyzeKick (k2.data(), (int) k2.size(), sr);
            char msg[96]; std::snprintf (msg, sizeof msg, "frecuencia de muestreo %.0f Hz: tono final ~48 Hz", rate);
            CHECK (ar.model.valid && std::fabs (ar.model.pitch.v[ar.model.pitch.n - 1] - 48.0f) < 5.0f, msg);
            sr = save;
        }
    }

    // ====================================================== 11. Tiempo de procesamiento
    std::printf ("Test 11: tiempo de procesamiento (objetivo < 1 s)\n");
    {
        sr = 44100.0;
        std::vector<float> big ((size_t) (sr * 30.0), 0.0f);
        for (auto& s : big) s = 0.002f * u (rng);
        for (int i = 0; i < 60; ++i)
        {
            const int pos = (int) (sr * (0.1 + i * 0.5));
            if (i % 2 == 0) addKick (big, pos, { 190, 48, 22, 100, 0.2, 0.8 }); else addHat (big, pos, 0.4);
        }
        const double t0 = nowSec();
        auto evs = detectEvents (big.data(), (int) big.size(), sr);
        const double t1 = nowSec();
        auto ab = analyzeKick (big.data(), (int) big.size(), sr, 0, &evs);
        const double t2 = nowSec();
        std::printf ("    30 s de audio, %zu eventos: deteccion %.0f ms, extraccion+modelado %.0f ms\n", evs.size(), (t1 - t0) * 1000, (t2 - t1) * 1000);
        CHECK (ab.model.valid, "audio de 30 s analizado");
        CHECK ((t1 - t0) < 1.0, "deteccion de eventos en 30 s < 1 s");
        CHECK ((t2 - t1) < 1.0, "extraccion + modelado de un golpe < 1 s");
        const double t3 = nowSec();
        auto rr = renderOneShot (ab.model, KickParams {}, sr);
        std::printf ("    resintesis de un golpe: %.1f ms\n", (nowSec() - t3) * 1000);
        CHECK (allFinite (rr.l), "resintesis finita");
    }

    std::printf ("\n%s (%d fallos)\n", failures == 0 ? "TODO CORRECTO" : "HAY FALLOS", failures);
    return failures == 0 ? 0 : 1;
}
