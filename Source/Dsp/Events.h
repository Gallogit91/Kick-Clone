#pragma once
// Deteccion de eventos percusivos:
//  - flujo espectral con compresion logaritmica (dominio tiempo-frecuencia)
//  - umbral adaptativo (mediana local) y seleccion de picos
//  - "lowRatio": que parte de la energia posterior esta por debajo de 200 Hz (bombo ~ alto)
//  - refinado del inicio en el dominio del tiempo, a resolucion de muestra

#include "Fft.h"
#include <vector>

namespace kc
{
struct DrumEvent
{
    int sample = 0;          // inicio refinado
    float strength = 0.0f;   // 0..1, relativo al evento mas fuerte
    float lowRatio = 0.0f;   // 0..1, energia < 200 Hz / energia 20 Hz..8 kHz tras el ataque
};

// Busca el comienzo del golpe cerca de 'approx' (muestras). Devuelve la muestra de inicio
// (cruce por cero anterior al primer valor significativo).
inline int refineStart (const float* x, int n, int approx, int radius, double sr)
{
    approx = std::clamp (approx, 0, std::max (0, n - 1));
    const int a = std::max (0, approx - radius);
    const int b = std::min (n, approx + 2 * radius);
    if (b - a < 8) return approx;

    float peak = 0.0f;
    for (int i = a; i < b; ++i) peak = std::max (peak, std::fabs (x[i]));
    if (peak < 1.0e-7f) return approx;

    float base = 0.0f;
    for (int i = std::max (0, a - radius); i < a; ++i) base = std::max (base, std::fabs (x[i]));

    // Con una cola fuerte previa (otro sonido aun sonando) el valor absoluto ya supera el umbral
    // antes del golpe: se trabaja con la senal diferenciada, que resalta los transitorios.
    if (base > 0.25f * peak)
    {
        auto d = [x] (int i) { return i > 0 ? x[i] - x[i - 1] : x[i]; };
        float dPeak = 0.0f, dBase = 0.0f;
        for (int i = a; i < b; ++i) dPeak = std::max (dPeak, std::fabs (d (i)));
        for (int i = std::max (0, a - radius); i < a; ++i) dBase = std::max (dBase, std::fabs (d (i)));
        if (dPeak < 1.0e-7f) return approx;
        const float dThr = std::min (std::max (0.15f * dPeak, 2.5f * dBase), 0.6f * dPeak);
        int j0 = a;
        while (j0 < b && std::fabs (d (j0)) < dThr) ++j0;
        return j0 >= b ? approx : std::max (0, j0 - 1);
    }

    const float thr = std::min (std::max (0.12f * peak, 2.0f * base), 0.6f * peak);
    int i0 = a;
    while (i0 < b && std::fabs (x[i0]) < thr) ++i0;
    if (i0 >= b) return approx;

    // Retrocede hasta el cruce por cero anterior, pero sin atravesar silencio:
    // un valor ~0 no cuenta como "mismo signo".
    const int lim = std::max (0, i0 - (int) std::lround (0.003 * sr));
    const float quiet = 0.01f * thr;
    int z = i0;
    while (z > lim && std::fabs (x[z - 1]) > quiet && ((x[z - 1] >= 0.0f) == (x[z] >= 0.0f))) --z;
    return std::max (0, z - 1);
}

inline std::vector<DrumEvent> detectEvents (const float* x, int n, double sr)
{
    std::vector<DrumEvent> events;
    if (x == nullptr || n < (int) (sr * 0.05) || sr < 8000.0) return events;

    float globalPeak = 0.0f;
    for (int i = 0; i < n; ++i) globalPeak = std::max (globalPeak, std::fabs (x[i]));
    if (globalPeak < 1.0e-4f) return events;

    const int N = nextPow2 ((int) (sr * 0.0232));
    const int hop = N / 4;
    const Stft stft (N, hop);
    std::vector<Cpx> spec;
    stft.analyze (x, n, spec);
    const int F = stft.numFrames (n), B = stft.bins();
    const double binHz = sr / (double) N;
    const int kLow0 = std::max (1, (int) std::ceil (20.0 / binHz));
    const int kLow1 = std::max (kLow0 + 1, (int) (200.0 / binHz));
    const int kTot1 = std::min (B - 1, std::max (kLow1 + 1, (int) (8000.0 / binHz)));
    const float norm = 1.0f / ((float) N * 0.25f);

    std::vector<float> odf ((size_t) F, 0.0f), eLow ((size_t) F, 0.0f), eTot ((size_t) F, 0.0f);
    std::vector<float> prev ((size_t) B, 0.0f);
    for (int f = 0; f < F; ++f)
    {
        const Cpx* s = &spec[(size_t) f * (size_t) B];
        float flux = 0.0f, el = 0.0f, et = 0.0f;
        for (int k = 1; k < B; ++k)
        {
            const float mag = std::sqrt (s[k].r * s[k].r + s[k].i * s[k].i) * norm;
            const float l = std::log1p (300.0f * mag);
            const float d = l - prev[(size_t) k];
            if (d > 0.0f) flux += (k < kLow1 ? 3.0f : 1.0f) * d;
            prev[(size_t) k] = l;
            const float e = mag * mag;
            if (k >= kLow0 && k < kLow1) el += e;
            if (k >= kLow0 && k <= kTot1) et += e;
        }
        odf[(size_t) f] = flux;
        eLow[(size_t) f] = el;
        eTot[(size_t) f] = et;
    }

    float maxOdf = 0.0f;
    for (float v : odf) maxOdf = std::max (maxOdf, v);
    if (maxOdf < 1.0e-6f) return events;

    // Umbral adaptativo: mediana local (+-120 ms) y fraccion del maximo global
    const int W = std::max (2, (int) std::lround (0.12 * sr / (double) hop));
    std::vector<float> thr ((size_t) F), win;
    for (int f = 0; f < F; ++f)
    {
        win.assign (odf.begin() + std::max (0, f - W), odf.begin() + std::min (F, f + W + 1));
        std::nth_element (win.begin(), win.begin() + (long) win.size() / 2, win.end());
        thr[(size_t) f] = 1.5f * win[win.size() / 2] + 0.15f * maxOdf;
    }

    // Picos: maximo local en +-2 fotogramas y por encima del umbral
    struct Peak { int f; float v; };
    std::vector<Peak> peaks;
    for (int f = 0; f < F; ++f)
    {
        const float v = odf[(size_t) f];
        if (v <= thr[(size_t) f]) continue;
        bool isMax = true;
        for (int k = std::max (0, f - 2); k <= std::min (F - 1, f + 2) && isMax; ++k)
            if (k != f && (odf[(size_t) k] > v || (odf[(size_t) k] == v && k < f))) isMax = false;
        if (isMax) peaks.push_back ({ f, v });
    }

    // Distancia minima 40 ms: gana el mas fuerte
    const int minDist = (int) std::lround (0.04 * sr);
    std::vector<Peak> kept;
    for (auto& p : peaks)
    {
        if (! kept.empty() && (p.f - kept.back().f) * hop < minDist)
        {
            if (p.v > kept.back().v) kept.back() = p;
        }
        else
            kept.push_back (p);
    }

    for (auto& p : kept)
    {
        const int approx = stft.frameStart (p.f) + N / 2;
        const int c0 = std::max (0, approx - N), c1 = std::min (n, approx + N);
        float lp = 0.0f;
        for (int i = c0; i < c1; ++i) lp = std::max (lp, std::fabs (x[i]));
        if (lp < 0.02f * globalPeak) continue;   // eventos fantasma muy por debajo del maximo

        DrumEvent e;
        e.sample = refineStart (x, n, approx, N, sr);
        e.strength = p.v / maxOdf;

        // Aumento de energia respecto al fotograma anterior al ataque: lo que el golpe "aporta",
        // no lo que ya sonaba (la cola de un bombo no debe hacer pasar un hi-hat por bombo).
        const int f0 = std::max (0, p.f - 1);
        float sl = 0.0f, st = 0.0f;
        for (int f = p.f; f <= std::min (F - 1, p.f + 3); ++f)
        {
            sl += std::max (0.0f, eLow[(size_t) f] - eLow[(size_t) f0]);
            st += std::max (0.0f, eTot[(size_t) f] - eTot[(size_t) f0]);
        }
        e.lowRatio = st > 1.0e-12f ? std::min (1.0f, sl / st) : 0.0f;

        if (! events.empty() && e.sample - events.back().sample < minDist / 2) continue;
        events.push_back (e);
    }

    std::sort (events.begin(), events.end(), [] (const DrumEvent& a, const DrumEvent& b) { return a.sample < b.sample; });
    return events;
}
} // namespace kc
