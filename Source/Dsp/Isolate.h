#pragma once
// Extraccion de un golpe en tres capas (cuerpo, click, ruido) a partir de audio complejo.
//
//  1. Cuerpo: mascara espectral suave (paso bajo de fase cero) aplicada en el dominio STFT.
//     Al ser de fase cero no desplaza ni deforma el ataque, a diferencia de un filtro IIR.
//  2. Resto (por encima del corte): se separa en el tiempo con un crossfade de coseno elevado
//     en "click" (primeros milisegundos) y "ruido" (cola).
//  3. Todas las capas se multiplican por una ventana suave alrededor del evento, de modo que
//     lo que suena antes o despues (otros golpes) no entra en la extraccion.
//
// Las tres capas suman la senal original dentro del evento (reconstruccion completa).

#include "Fft.h"
#include <vector>

namespace kc
{
struct IsolateSettings
{
    float bodyCutoffHz = 400.0f;   // corte de la mascara del cuerpo
    float clickStartMs = 1.5f;     // el crossfade click -> ruido empieza aqui
    float clickEndMs = 6.0f;       // ...y termina aqui
};

struct Layers
{
    int start = 0;                     // muestra del audio de origen donde empiezan las capas
    std::vector<float> body, click, noise;

    int size() const { return (int) body.size(); }

    std::vector<float> sum() const
    {
        std::vector<float> s (body.size(), 0.0f);
        for (size_t i = 0; i < s.size(); ++i) s[i] = body[i] + click[i] + noise[i];
        return s;
    }
};

// length: muestras del cuerpo/click. noiseLength: muestras de la capa de ruido (<= length).
inline Layers isolateLayers (const float* x, int n, double sr, int start, int length, int noiseLength,
                             const IsolateSettings& cfg = {})
{
    Layers L;
    L.start = start;
    length = std::max (0, std::min (length, n - start));
    noiseLength = std::max (0, std::min (noiseLength, length));
    L.body.assign ((size_t) length, 0.0f);
    L.click.assign ((size_t) length, 0.0f);
    L.noise.assign ((size_t) length, 0.0f);
    if (x == nullptr || length < 16 || sr < 8000.0) return L;

    const int pre = (int) std::lround (0.010 * sr);
    const int post = (int) std::lround (0.030 * sr);
    const int a = start - pre;
    const int segLen = pre + length + post;

    std::vector<float> seg ((size_t) segLen, 0.0f);
    for (int i = 0; i < segLen; ++i)
    {
        const int idx = a + i;
        if (idx >= 0 && idx < n) seg[(size_t) i] = x[idx];
    }

    // --- Mascara espectral suave del cuerpo
    const int N = nextPow2 ((int) (sr * 0.0464));
    const Stft stft (N, N / 4);
    std::vector<Cpx> spec;
    stft.analyze (seg.data(), segLen, spec);
    const int F = stft.numFrames (segLen), B = stft.bins();
    std::vector<float> mask ((size_t) B);
    for (int k = 0; k < B; ++k)
    {
        const double f = (double) k * sr / (double) N;
        const double r = f / (double) cfg.bodyCutoffHz;
        mask[(size_t) k] = (float) (1.0 / (1.0 + std::pow (r, 8.0)));
    }
    for (int f = 0; f < F; ++f)
        for (int k = 0; k < B; ++k)
        {
            Cpx& c = spec[(size_t) f * (size_t) B + (size_t) k];
            c.r *= mask[(size_t) k];
            c.i *= mask[(size_t) k];
        }
    std::vector<float> bodyFull;
    stft.synthesize (spec, segLen, bodyFull);

    // --- Ventanas temporales
    auto ms = [sr] (double v) { return v * 0.001 * sr; };
    const double t1 = ms (cfg.clickStartMs), t2 = std::max (t1 + 1.0, ms (cfg.clickEndMs));
    const int fadeIn = std::max (2, (int) ms (0.5));
    const int fadeOut = std::max (2, std::min ((int) ms (10.0), length / 4));
    const int nFadeOut = std::max (2, std::min ((int) ms (5.0), std::max (2, noiseLength / 4)));

    for (int i = 0; i < length; ++i)
    {
        const size_t s = (size_t) (pre + i);
        const float body = bodyFull[s];
        const float high = seg[s] - body;

        float g = 1.0f;
        if (i < fadeIn) g = 0.5f - 0.5f * (float) std::cos (kPi * (double) i / (double) fadeIn);
        if (i >= length - fadeOut)
            g *= 0.5f + 0.5f * (float) std::cos (kPi * (double) (i - (length - fadeOut)) / (double) fadeOut);

        // crossfade click -> ruido con coseno elevado
        float gc = 1.0f;
        if ((double) i >= t2) gc = 0.0f;
        else if ((double) i > t1) gc = 0.5f + 0.5f * (float) std::cos (kPi * ((double) i - t1) / (t2 - t1));

        float gn = 1.0f;
        if (i >= noiseLength) gn = 0.0f;
        else if (i >= noiseLength - nFadeOut)
            gn = 0.5f + 0.5f * (float) std::cos (kPi * (double) (i - (noiseLength - nFadeOut)) / (double) nFadeOut);

        L.body[(size_t) i] = body * g;
        L.click[(size_t) i] = high * g * gc;
        L.noise[(size_t) i] = high * g * (1.0f - gc) * gn;
    }
    return L;
}
} // namespace kc
