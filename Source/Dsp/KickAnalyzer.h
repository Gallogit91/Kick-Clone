#pragma once
// Analisis y modelado de un bombo (v2):
//   1. Analisis: eventos por flujo espectral (Events.h)
//   2. Extraccion: tres capas con mascaras suaves (Isolate.h)
//   3. Ajuste de cada capa a un oscilador:
//        cuerpo -> seno con envolvente de tono y de amplitud (seguimiento por cruces por cero)
//        click  -> rafaga de ruido filtrado paso alto (nivel, decaimiento, corte)
//        ruido  -> ruido blanco por un paso banda (centro, Q) con envolvente multisegmento
// Es procesado de senal clasico (no usa modelos de IA).

#include "Events.h"
#include "Isolate.h"
#include "KickModel.h"
#include <utility>
#include <vector>

namespace kc
{
struct Analysis
{
    KickModel model;
    int start = 0;                    // muestra de inicio del golpe en el audio de origen
    std::vector<float> isolated;      // cuerpo + click + ruido, desde 'start'
    std::vector<float> body, click, noise;
    float lowRatio = 0.0f;            // "parecido a bombo" del evento elegido
};

namespace detail
{
inline float peakAbs (const std::vector<float>& v, int i0, int i1)
{
    i0 = std::max (i0, 0);
    i1 = std::min (i1, (int) v.size());
    float m = 0.0f;
    for (int i = i0; i < i1; ++i) m = std::max (m, std::fabs (v[(size_t) i]));
    return m;
}

inline float rmsOf (const std::vector<float>& v, int i0, int i1)
{
    i0 = std::max (i0, 0);
    i1 = std::min (i1, (int) v.size());
    if (i1 <= i0) return 0.0f;
    double s = 0.0;
    for (int i = i0; i < i1; ++i) s += (double) v[(size_t) i] * v[(size_t) i];
    return (float) std::sqrt (s / (double) (i1 - i0));
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

// Estadisticos del espectro de potencia de x[i0,i1): frecuencia centroide, dispersion y
// frecuencia por debajo de la cual esta el 25 % de la energia (ignorando < minHz).
struct SpecStats { float centroid = 0, spread = 0, f25 = 0, f75 = 0, energy = 0; };

inline SpecStats spectrumStats (const std::vector<float>& x, int i0, int i1, double sr, float minHz)
{
    SpecStats s;
    i0 = std::max (0, i0);
    i1 = std::min ((int) x.size(), i1);
    const int len = i1 - i0;
    if (len < 16) return s;
    const int N = std::min (8192, nextPow2 (std::max (len, 256)));
    const Fft fft (N);
    std::vector<Cpx> b ((size_t) N);
    for (int i = 0; i < N; ++i)
    {
        if (i < std::min (len, N))
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * ((double) i + 0.5) / (double) std::min (len, N));
            b[(size_t) i].r = x[(size_t) (i0 + i)] * (float) w;
        }
    }
    fft.forward (b.data());

    double sp = 0.0, sf = 0.0;
    const int k0 = std::max (1, (int) std::ceil ((double) minHz * N / sr));
    for (int k = k0; k < N / 2; ++k)
    {
        const double p = (double) b[(size_t) k].r * b[(size_t) k].r + (double) b[(size_t) k].i * b[(size_t) k].i;
        sp += p; sf += p * ((double) k * sr / N);
    }
    if (sp <= 1.0e-18) return s;
    s.energy = (float) sp;
    const double c = sf / sp;
    double v = 0.0, acc = 0.0;
    bool got25 = false, got75 = false;
    for (int k = k0; k < N / 2; ++k)
    {
        const double f = (double) k * sr / N;
        const double p = (double) b[(size_t) k].r * b[(size_t) k].r + (double) b[(size_t) k].i * b[(size_t) k].i;
        v += p * (f - c) * (f - c);
        acc += p;
        if (! got25 && acc >= 0.25 * sp) { s.f25 = (float) f; got25 = true; }
        if (! got75 && acc >= 0.75 * sp) { s.f75 = (float) f; got75 = true; }
    }
    s.centroid = (float) c;
    s.spread = (float) std::sqrt (v / sp);
    return s;
}

// Ajusta un paso banda (centro, Q) al reparto de energia: el centro es la media geometrica de los
// cuartiles 25/75 % y la Q se busca para que un paso banda ideal de 2o orden, recortado a
// [minHz, maxHz], reproduzca el cociente f75/f25 medido.
inline void fitBand (const SpecStats& st, float minHz, float maxHz, float& centerHz, float& q)
{
    // Busqueda conjunta (centro, Q): para cada candidato se calculan los cuartiles 25/75 % de la
    // potencia de un paso banda ideal de 2o orden recortado a [minHz, maxHz] y se compara con los medidos.
    constexpr int G = 400;
    const double lf0 = std::log ((double) minHz), lf1 = std::log ((double) maxHz), dl = (lf1 - lf0) / G;
    double f[G], w[G];
    for (int g = 0; g < G; ++g) { f[g] = std::exp (lf0 + dl * (g + 0.5)); w[g] = f[g] * dl; }

    const double t25 = std::log (std::max ((double) st.f25, (double) minHz));
    const double t75 = std::log (std::max ((double) st.f75, (double) minHz));
    const double guess = std::sqrt (std::max ((double) st.f25, 1.0) * std::max ((double) st.f75, 1.0));

    double bestErr = 1.0e30, bestC = guess, bestQ = 0.7;
    for (int ci = 0; ci < 41; ++ci)
    {
        const double c = std::clamp (guess * std::pow (2.0, -1.0 + 2.0 * (double) ci / 40.0), 200.0, (double) maxHz);
        for (int qi = 0; qi < 30; ++qi)
        {
            const double Q = 0.3 * std::pow (10.0 / 0.3, (double) qi / 29.0);
            double cdf[G];
            double acc = 0.0;
            for (int g = 0; g < G; ++g)
            {
                const double x = f[g] / c, d = x - 1.0 / x;
                acc += w[g] / (1.0 + Q * Q * d * d);
                cdf[g] = acc;
            }
            auto quant = [&] (double frac) {
                const double t = frac * acc;
                for (int g = 0; g < G; ++g)
                    if (cdf[g] >= t) return std::log (f[g]);
                return std::log (f[G - 1]); };
            const double e25 = quant (0.25) - t25, e75 = quant (0.75) - t75;
            const double err = e25 * e25 + e75 * e75;
            if (err < bestErr) { bestErr = err; bestC = c; bestQ = Q; }
        }
    }
    centerHz = (float) bestC;
    q = (float) bestQ;
}

// Desplazamiento (muestras) hasta el arranque real del cuerpo: primer valor >= 3 % del pico,
// retrocediendo al cruce por cero anterior (el seno nace con fase 0).
inline int bodyOnsetOffset (const std::vector<float>& body, double sr)
{
    const int lim = std::min ((int) body.size(), (int) std::lround (0.020 * sr));
    float peak = 0.0f;
    for (int i = 0; i < lim; ++i) peak = std::max (peak, std::fabs (body[(size_t) i]));
    if (peak < 1.0e-6f) return 0;
    int i0 = 0;
    while (i0 < lim && std::fabs (body[(size_t) i0]) < 0.03f * peak) ++i0;
    if (i0 >= lim) return 0;
    int z = i0;
    while (z > 0 && std::fabs (body[(size_t) z - 1]) > 0.002f * peak && ((body[(size_t) z - 1] >= 0.0f) == (body[(size_t) z] >= 0.0f))) --z;
    return z;
}
} // namespace detail

// Analisis con inicio exacto (sin refinar): util para ajuste fino a nivel de muestra.
// 'events' (opcional, ordenados) se usa para acotar la longitud del golpe.
inline Analysis analyzeKickAt (const float* x, int n, double sr, int start,
                               const std::vector<DrumEvent>* events = nullptr)
{
    using namespace detail;
    Analysis A;
    A.start = start;
    if (x == nullptr || sr < 8000.0 || start < 0 || start >= n) return A;

    auto ms = [sr] (double v) { return (int) std::lround (v * 0.001 * sr); };

    int maxLen = std::min ((int) (kAxisMaxMs * 0.001 * sr), n - start);
    int noiseMax = maxLen;
    if (events != nullptr)
        for (const auto& e : *events)
        {
            if (e.sample <= start + ms (8.0)) continue;
            noiseMax = std::min (noiseMax, e.sample - start - ms (1.0));       // cualquier golpe corta el ruido
            if (e.sample > start + ms (30.0) && e.lowRatio >= 0.35f)
            {
                maxLen = std::min (maxLen, e.sample - start - ms (2.0));       // otro bombo corta el cuerpo
                break;
            }
        }
    if (maxLen < ms (20.0)) return A;
    noiseMax = std::max (ms (8.0), std::min (noiseMax, maxLen));

    IsolateSettings cfg;
    Layers layers = isolateLayers (x, n, sr, start, maxLen, noiseMax, cfg);
    const std::vector<float>& body = layers.body;

    // --- Longitud del cuerpo: envolvente por ms (con maximo deslizante) hasta -60 dB
    const int hop = std::max (1, ms (1.0));
    const int nh = maxLen / hop;
    if (nh < 10) return A;
    std::vector<float> env ((size_t) nh), envS ((size_t) nh);
    for (int h = 0; h < nh; ++h) env[(size_t) h] = peakAbs (body, h * hop, (h + 1) * hop);
    for (int h = 0; h < nh; ++h)
    {
        float mx = 0.0f;
        for (int k = std::max (0, h - 10); k <= std::min (nh - 1, h + 10); ++k) mx = std::max (mx, env[(size_t) k]);
        envS[(size_t) h] = mx;
    }
    float segPeak = 0.0f;
    int peakIdx = 0;
    for (int h = 0; h < std::min (nh, 100); ++h)
        if (envS[(size_t) h] > segPeak) { segPeak = envS[(size_t) h]; peakIdx = h; }
    if (segPeak < 1.0e-5f) return A;

    // Fin: -60 dB, o antes si la envolvente vuelve a subir (otro sonido grave entra en el cuerpo)
    int endH = nh;
    float runMin = 1.0e9f;
    int minH = peakIdx;
    for (int h = peakIdx + 1; h < nh; ++h)
    {
        const float e = envS[(size_t) h];
        if (e < segPeak * 0.001f) { endH = h; break; }
        if (e < runMin) { runMin = e; minH = h; }
        if (e > runMin * 3.0f && e > 0.01f * segPeak) { endH = minH; break; }
    }
    endH = std::max (endH, std::min (nh, 40));
    const int lenSamples = std::min (maxLen, endH * hop);
    const float lengthMs = (float) (lenSamples * 1000.0 / sr);
    const int end = lenSamples;
    const float segPeakRaw = peakAbs (body, 0, end);
    if (segPeakRaw < 1.0e-5f) return A;

    // Un "golpe" 40 dB por debajo del maximo del archivo es ruido de fondo, no un bombo
    float globalPeak = 0.0f;
    for (int i = 0; i < n; ++i) globalPeak = std::max (globalPeak, std::fabs (x[i]));
    if (segPeakRaw < 0.01f * globalPeak) return A;

    KickModel& m = A.model;

    // --- Tono: semiperiodos entre cruces por cero (con histeresis de amplitud)
    std::vector<std::pair<float, float>> pts;
    {
        const float thr = 0.02f * segPeakRaw;
        double lastCross = -1.0;
        float halfPeak = 0.0f;
        for (int i = 1; i < end; ++i)
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
                        const double t = 0.5 * (c + lastCross) * 1000.0 / sr;
                        if (f >= kMinHz && f <= 800.0) pts.emplace_back ((float) t, (float) f);
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

    // --- Amplitud del cuerpo: maximo en una ventana de un periodo alrededor de cada punto
    float rawAt[kMaxPoints] {};
    float timesAmp[kMaxPoints] {};
    int na = 0;
    float bodyGain = 0.0f;
    for (float T : { 0.0f, 2.0f, 6.0f, 15.0f, 35.0f, 70.0f, 130.0f, 250.0f, 450.0f, 700.0f })
    {
        if (T >= lengthMs * 0.98f || na >= kMaxPoints - 1) break;
        const float f = std::max (evalPitchHz (m.pitch, T), kMinHz);
        const double wMs = std::clamp (1000.0 / f, 2.0, 30.0);
        const int c = ms (T);
        const int w = std::max (1, ms (wMs * 0.5));
        const float raw = peakAbs (body, std::max (0, c - w), std::min (end, c + w + 1));
        rawAt[na] = raw; timesAmp[na] = T; ++na;
        bodyGain = std::max (bodyGain, raw);
    }
    if (na < 2 || bodyGain < 1.0e-5f) return A;
    m.amp.clear();
    for (int i = 0; i < na; ++i) m.amp.add (timesAmp[i], rawAt[i] / bodyGain);
    m.amp.add (lengthMs, 0.0f);
    m.bodyGain = std::min (bodyGain, 2.0f);

    // --- Click
    {
        const std::vector<float>& cl = layers.click;
        const int step = std::max (1, ms (0.25));
        int pk = 0; float pkv = 0.0f;
        for (int k = 0; k < 24; ++k)
        {
            const float v = rmsOf (cl, k * step, (k + 1) * step);
            if (v > pkv) { pkv = v; pk = k; }
        }
        m.clickLevel = pkv < 0.002f * std::max (m.bodyGain, 1.0e-3f) ? 0.0f : std::min (pkv, 1.5f);
        m.clickDecayMs = 1.5f;
        m.clickHz = 1500.0f;
        if (m.clickLevel > 0.0f)
        {
            int k = pk + 1;
            while (k < 80 && rmsOf (cl, k * step, (k + 1) * step) > pkv * 0.368f) ++k;
            m.clickDecayMs = std::clamp ((float) ((k - pk) * step * 1000.0 / sr), 0.4f, 8.0f);
            const auto st = spectrumStats (cl, 0, std::min ((int) cl.size(), ms (6.0)), sr, 300.0f);
            if (st.f25 > 0.0f) m.clickHz = std::clamp (st.f25, 400.0f, 8000.0f);
        }
    }

    // --- Ruido: banda (centro y Q) y envolvente multisegmento (RMS)
    {
        const std::vector<float>& nz = layers.noise;
        const int nLen = std::min ((int) nz.size(), noiseMax);

        // Fin real del ruido: ultima ventana de 5 ms con RMS > 3 % del pico de la capa
        const int win = std::max (1, ms (5.0));
        float peak = 0.0f;
        for (int i = 0; i + win <= nLen; i += win) peak = std::max (peak, rmsOf (nz, i, i + win));
        int lastWin = 0;
        for (int i = 0; i + win <= nLen; i += win)
            if (rmsOf (nz, i, i + win) >= 0.03f * peak) lastWin = i + win;
        const float nEndMs = std::min ((float) (lastWin * 1000.0 / sr) + 5.0f, (float) (nLen * 1000.0 / sr));

        float rmsAt[kMaxPoints] {};
        float tAt[kMaxPoints] {};
        int nn = 0;
        for (float T : { 0.0f, 5.0f, 10.0f, 20.0f, 40.0f, 80.0f, 160.0f, 320.0f, 600.0f })
        {
            if (T >= nEndMs * 0.98f || nn >= kMaxPoints - 1) break;
            const double hw = std::max (1.5, 0.3 * (double) T);
            const int c = ms (T);
            rmsAt[nn] = rmsOf (nz, std::max (0, c - ms (hw)), std::min (nLen, c + ms (hw) + 1));
            tAt[nn] = T; ++nn;
        }

        m.noise.clear();
        if (nn >= 2 && peak > 2.0e-4f)
        {
            for (int i = 0; i < nn; ++i) m.noise.add (tAt[i], std::min (rmsAt[i], 1.0f));
            m.noise.add (std::max (nEndMs, tAt[nn - 1] + 1.0f), 0.0f);

            const auto st = spectrumStats (nz, ms (4.0), std::min (nLen, ms (200.0)), sr, 300.0f);
            if (st.f25 > 0.0f && st.f75 > 0.0f)
                fitBand (st, 300.0f, (float) (0.5 * sr), m.noiseHz, m.noiseQ);
        }
        else
        {
            m.noise.add (0.0f, 0.0003f);
            m.noise.add (10.0f, 0.0f);
        }
    }

    m.lengthMs = std::max (lengthMs, m.noise.endMs());
    m.valid = true;

    A.isolated = layers.sum();
    A.body = std::move (layers.body);
    A.click = std::move (layers.click);
    A.noise = std::move (layers.noise);
    return A;
}

// Analiza y, si el cuerpo arranca unas muestras despues del click (el inicio lo marca el
// transitorio), vuelve a analizar desde el arranque real del seno para que el clon quede en fase.
inline Analysis alignToBody (const float* x, int n, double sr, int start, const std::vector<DrumEvent>* events)
{
    Analysis a = analyzeKickAt (x, n, sr, start, events);
    if (! a.model.valid) return a;
    const int off = detail::bodyOnsetOffset (a.body, sr);
    if (off >= 2 && off < (int) std::lround (0.003 * sr) && start + off < n)
    {
        Analysis b = analyzeKickAt (x, n, sr, start + off, events);
        if (b.model.valid) return b;
    }
    return a;
}

// Elige el bombo: el primer evento "parecido a bombo" a partir de searchFrom
// (los mas fuertes primero para descartar golpes marginales). 'events' es opcional.
inline Analysis analyzeKick (const float* x, int n, double sr, int searchFrom = 0,
                             const std::vector<DrumEvent>* eventsIn = nullptr)
{
    std::vector<DrumEvent> own;
    if (eventsIn == nullptr)
    {
        own = detectEvents (x, n, sr);
        eventsIn = &own;
    }
    const auto& ev = *eventsIn;

    // Fuerza maxima entre los candidatos "bombo"
    float maxKick = 0.0f;
    for (const auto& e : ev)
        if (e.sample >= searchFrom && e.lowRatio >= 0.45f) maxKick = std::max (maxKick, e.strength);

    const DrumEvent* pick = nullptr;
    for (const auto& e : ev)
        if (e.sample >= searchFrom && e.lowRatio >= 0.45f && e.strength >= 0.4f * maxKick) { pick = &e; break; }

    if (pick == nullptr)   // sin candidato claro: el de mayor strength*lowRatio
    {
        float best = 0.0f;
        for (const auto& e : ev)
            if (e.sample >= searchFrom && e.strength * e.lowRatio > best) { best = e.strength * e.lowRatio; pick = &e; }
        if (pick != nullptr && pick->lowRatio < 0.2f) pick = nullptr;
    }

    if (pick == nullptr)
    {
        // Sin eventos (p. ej. un fundido de entrada lento): primer valor significativo
        if (x == nullptr || n < 64 || searchFrom >= n) return Analysis {};
        float peak = 0.0f;
        const int e0 = std::clamp (searchFrom, 0, n - 1), e1 = std::min (n, e0 + (int) (sr * 1.5));
        for (int i = e0; i < e1; ++i) peak = std::max (peak, std::fabs (x[i]));
        if (peak < 1.0e-4f) return Analysis {};
        const int s = refineStart (x, n, e0 + (int) (sr * 0.01), (int) (sr * 0.02), sr);
        return alignToBody (x, n, sr, std::max (s, e0), &ev);
    }

    Analysis a = alignToBody (x, n, sr, pick->sample, &ev);
    a.lowRatio = pick->lowRatio;
    return a;
}
} // namespace kc
