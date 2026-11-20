#pragma once
// Analisis de un bombo a partir de audio: localiza el golpe y extrae
//  - envolvente de tono (seguimiento por cruces por cero del cuerpo < 400 Hz)
//  - envolvente de amplitud (maximos por periodo del cuerpo)
//  - click (energia > 1.5 kHz al inicio) y cola de ruido
// Es analisis de senal clasico (no usa modelos de IA).

#include "KickModel.h"
#include <vector>
#include <utility>

namespace kc
{
namespace detail
{
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static Biquad make (bool highpass, double fc, double sr)
    {
        const double w0 = 2.0 * 3.14159265358979323846 * fc / sr;
        const double c = std::cos (w0), s = std::sin (w0);
        const double alpha = s / (2.0 * 0.70710678118);
        const double a0 = 1.0 + alpha;
        Biquad q;
        if (highpass) { q.b0 = (1 + c) / 2; q.b1 = -(1 + c); q.b2 = (1 + c) / 2; }
        else          { q.b0 = (1 - c) / 2; q.b1 =  (1 - c); q.b2 = (1 - c) / 2; }
        q.a1 = -2.0 * c; q.a2 = 1.0 - alpha;
        q.b0 /= a0; q.b1 /= a0; q.b2 /= a0; q.a1 /= a0; q.a2 /= a0;
        return q;
    }
};

// Filtro Butterworth de 4o orden (2 biquads en cascada), de fase no lineal pero estable.
inline std::vector<float> filter4 (const float* x, int n, bool highpass, double fc, double sr)
{
    std::vector<float> y ((size_t) n);
    const Biquad q = Biquad::make (highpass, std::min (fc, sr * 0.45), sr);
    double x1[2] {}, x2[2] {}, y1[2] {}, y2[2] {};
    for (int i = 0; i < n; ++i)
    {
        double v = x[i];
        for (int s = 0; s < 2; ++s)
        {
            const double o = q.b0 * v + q.b1 * x1[s] + q.b2 * x2[s] - q.a1 * y1[s] - q.a2 * y2[s];
            x2[s] = x1[s]; x1[s] = v; y2[s] = y1[s]; y1[s] = o;
            v = o;
        }
        y[(size_t) i] = (float) v;
    }
    return y;
}

inline float peakAbs (const std::vector<float>& v, int i0, int i1)
{
    i0 = std::max (i0, 0);
    i1 = std::min (i1, (int) v.size());
    float m = 0.0f;
    for (int i = i0; i < i1; ++i) m = std::max (m, std::fabs (v[(size_t) i]));
    return m;
}

inline float medianOf (std::vector<float> v)
{
    if (v.empty()) return 0.0f;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return v[v.size() / 2];
}

// Interpolacion logaritmica de la curva medida (t en ms, f en Hz).
inline float interpMeasured (const std::vector<std::pair<float, float>>& pts, float tMs)
{
    if (tMs <= pts.front().first) return pts.front().second;
    if (tMs >= pts.back().first) return pts.back().second;
    size_t i = 0;
    while (i + 1 < pts.size() && pts[i + 1].first < tMs) ++i;
    const float span = std::max (1.0e-4f, pts[i + 1].first - pts[i].first);
    const float a = (tMs - pts[i].first) / span;
    const float l0 = std::log (pts[i].second), l1 = std::log (pts[i + 1].second);
    return std::exp (l0 + (l1 - l0) * a);
}
} // namespace detail

// x: audio mono. searchFrom: muestra desde la que buscar el golpe.
// outStart: (opcional) muestra donde empieza el golpe detectado.
inline KickModel analyzeKick (const float* x, int n, double sr, int searchFrom = 0, int* outStart = nullptr)
{
    using namespace detail;
    KickModel m;
    if (x == nullptr || n < (int) (sr * 0.03) || sr < 8000.0)
        return m;

    auto ms = [sr] (double v) { return (int) std::lround (v * 0.001 * sr); };

    searchFrom = std::clamp (searchFrom, 0, n - 1);
    const std::vector<float> body = filter4 (x, n, false, 400.0, sr);

    // --- Envolvente por milisegundo y su version con maximo deslizante (+-10 ms)
    const int hop = std::max (1, ms (1.0));
    const int nh = n / hop;
    if (nh < 20) return m;
    std::vector<float> env ((size_t) nh), envS ((size_t) nh);
    for (int h = 0; h < nh; ++h)
        env[(size_t) h] = peakAbs (body, h * hop, (h + 1) * hop);
    for (int h = 0; h < nh; ++h)
    {
        float mx = 0.0f;
        for (int k = std::max (0, h - 10); k <= std::min (nh - 1, h + 10); ++k)
            mx = std::max (mx, env[(size_t) k]);
        envS[(size_t) h] = mx;
    }

    // --- Inicio del golpe
    const int h0 = searchFrom / hop;
    const int hLim = std::min (nh, h0 + 1500);
    float peak = 0.0f;
    for (int h = h0; h < hLim; ++h) peak = std::max (peak, envS[(size_t) h]);
    if (peak < 1.0e-5f) return m;

    int hOn = h0;
    while (hOn < hLim && envS[(size_t) hOn] < 0.3f * peak) ++hOn;
    // envS mira 10 ms hacia delante: avanzamos hasta el primer milisegundo con energia propia
    while (hOn < hLim - 1 && env[(size_t) hOn] < 0.05f * peak) ++hOn;
    hOn = std::min (hOn, nh - 1);

    const int a = std::max (0, hOn * hop - ms (8.0));
    const int b = std::min (n, hOn * hop + ms (2.0));
    float mx = peakAbs (std::vector<float> (x + a, x + b), 0, b - a);
    int start = a;
    for (int i = a; i < b; ++i)
        if (std::fabs (x[i]) >= 0.2f * mx) { start = i; break; }
    start = std::max (0, start - ms (0.3));
    if (outStart) *outStart = start;

    // --- Fin del golpe
    const int hs = start / hop;
    float segPeak = 0.0f;
    int peakIdx = hs;
    for (int h = hs; h < std::min (nh, hs + 100); ++h)
        if (envS[(size_t) h] > segPeak) { segPeak = envS[(size_t) h]; peakIdx = h; }
    if (segPeak < 1.0e-5f) return m;

    int endH = std::min (nh, hs + (int) kAxisMaxMs);
    float runMin = 1.0e9f;
    for (int h = peakIdx + 1; h < endH; ++h)
    {
        const float e = envS[(size_t) h];
        runMin = std::min (runMin, e);
        if (e > runMin * 4.0f && e > 0.2f * segPeak) { endH = h; break; }   // otro golpe
        if (e < segPeak * 0.001f) { endH = h; break; }                      // -60 dB
    }
    endH = std::max (endH, std::min (nh, hs + 40));
    const int lenSamples = std::min (n - start, (endH - hs) * hop);
    const float lengthMs = (float) (lenSamples * 1000.0 / sr);
    const int end = start + lenSamples;
    const float segPeakRaw = peakAbs (body, start, end);
    if (segPeakRaw < 1.0e-5f) return m;

    // --- Tono: semiperiodos entre cruces por cero con histeresis de amplitud
    std::vector<std::pair<float, float>> pts;
    {
        const float thr = 0.02f * segPeakRaw;
        double lastCross = -1.0;
        float halfPeak = 0.0f;
        for (int i = start + 1; i < end; ++i)
        {
            const float y0 = body[(size_t) i - 1], y1 = body[(size_t) i];
            halfPeak = std::max (halfPeak, std::fabs (y1));
            if ((y0 < 0.0f && y1 >= 0.0f) || (y0 >= 0.0f && y1 < 0.0f))
            {
                const double c = (i - 1) + (double) std::fabs (y0) / (double) std::max (1.0e-12f, std::fabs (y1 - y0));
                if (halfPeak >= thr)
                {
                    if (lastCross >= 0.0)
                    {
                        const double half = (c - lastCross) / sr;
                        const double f = 0.5 / std::max (half, 1.0e-6);
                        const double t = (0.5 * (c + lastCross) - start) * 1000.0 / sr;
                        if (f >= kMinHz && f <= 800.0)
                            pts.emplace_back ((float) t, (float) f);
                    }
                    lastCross = c;
                    halfPeak = 0.0f;
                }
            }
        }
    }

    if (pts.size() >= 3)
    {
        std::vector<std::pair<float, float>> sm = pts;
        for (size_t i = 1; i + 1 < pts.size(); ++i)
            sm[i].second = medianOf ({ pts[i - 1].second, pts[i].second, pts[i + 1].second });
        pts.swap (sm);
    }
    else
    {
        pts.clear();
        pts.emplace_back (0.0f, 55.0f);
        pts.emplace_back (std::max (lengthMs * 0.5f, 20.0f), 55.0f);
    }

    const float tl = pts.back().first;
    std::vector<float> tail;
    for (auto& p : pts) if (p.first >= 0.6f * tl) tail.push_back (p.second);
    if (tail.size() < 3)
        for (size_t i = pts.size() >= 3 ? pts.size() - 3 : 0; i < pts.size(); ++i) tail.push_back (pts[i].second);
    const float fEnd = std::clamp (medianOf (tail), kMinHz, 400.0f);

    m.pitch.clear();
    for (float T : { 0.0f, 2.0f, 5.0f, 10.0f, 20.0f, 40.0f, 80.0f, 160.0f, 320.0f })
        if (T < tl * 0.95f)
            m.pitch.add (T, std::clamp (interpMeasured (pts, T), kMinHz, kMaxHz));
    m.pitch.add (std::max (tl, m.pitch.n > 0 ? m.pitch.t[m.pitch.n - 1] + 1.0f : 1.0f), fEnd);

    // --- Amplitud: maximo del cuerpo en una ventana de un periodo alrededor de cada punto
    float rawAt[kMaxPoints] {};
    float timesAmp[kMaxPoints] {};
    int na = 0;
    float bodyGain = 0.0f;
    for (float T : { 0.0f, 2.0f, 6.0f, 15.0f, 35.0f, 70.0f, 130.0f, 250.0f, 450.0f, 700.0f })
    {
        if (T >= lengthMs * 0.98f || na >= kMaxPoints - 1) break;
        const float f = std::max (evalPitchHz (m.pitch, T), kMinHz);
        const double wMs = std::clamp (1000.0 / f, 2.0, 30.0);
        const int c = start + ms (T);
        const int w = std::max (1, ms (wMs * 0.5));
        const float raw = peakAbs (body, std::max (start, c - w), std::min (end, c + w + 1));
        rawAt[na] = raw; timesAmp[na] = T; ++na;
        bodyGain = std::max (bodyGain, raw);
    }
    if (na < 2 || bodyGain < 1.0e-5f) return KickModel {};
    m.amp.clear();
    for (int i = 0; i < na; ++i) m.amp.add (timesAmp[i], rawAt[i] / bodyGain);
    m.amp.add (lengthMs, 0.0f);
    m.bodyGain = std::min (bodyGain, 1.0f);

    // --- Click y ruido (banda > 1.5 kHz)
    const std::vector<float> hp = filter4 (x, n, true, 1500.0, sr);
    {
        const float cpk = peakAbs (hp, start, start + ms (4.0));
        const float ref = std::max (m.bodyGain, 1.0e-3f);
        m.clickLevel = cpk / ref < 0.01f ? 0.0f : std::min (cpk / ref, 1.5f);
        m.clickDecayMs = 1.5f;
        if (m.clickLevel > 0.0f)
        {
            const int step = std::max (1, ms (0.25));
            int pk = 0; float pkv = 0.0f;
            for (int k = 0; k < 16; ++k)
            {
                const float v = peakAbs (hp, start + k * step, start + (k + 1) * step);
                if (v > pkv) { pkv = v; pk = k; }
            }
            int k = pk + 1;
            while (k < 80 && peakAbs (hp, start + k * step, start + (k + 1) * step) > pkv * 0.368f) ++k;
            m.clickDecayMs = std::clamp ((float) ((k - pk) * step * 1000.0 / sr), 0.4f, 8.0f);
        }

        const float npk = peakAbs (hp, start + ms (10.0), std::min (end, start + ms (25.0)));
        m.noiseLevel = npk / ref < 0.01f ? 0.0f : std::min (npk / ref, 1.0f);
        m.noiseDecayMs = 40.0f;
        if (m.noiseLevel > 0.0f)
        {
            const int step = std::max (1, ms (5.0));
            int k = 3;   // ~15 ms
            while (k < 60 && peakAbs (hp, start + k * step, std::min (end, start + (k + 1) * step)) > npk * 0.368f) ++k;
            m.noiseDecayMs = std::clamp ((float) ((k - 3) * step * 1000.0 / sr), 15.0f, 300.0f);
        }
    }

    m.lengthMs = lengthMs;
    m.valid = true;
    return m;
}
} // namespace kc
