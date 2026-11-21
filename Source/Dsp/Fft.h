#pragma once
// FFT radix-2 y STFT (ventana de Hann, solapamiento-suma). Sin dependencias.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace kc
{
constexpr double kPi = 3.14159265358979323846;

struct Cpx { float r = 0.0f, i = 0.0f; };

inline int nextPow2 (int v)
{
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

class Fft
{
public:
    explicit Fft (int sizePow2)
        : n (sizePow2), rev ((size_t) sizePow2), tw ((size_t) sizePow2 / 2)
    {
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        for (int k = 0; k < n / 2; ++k)
        {
            const double a = -2.0 * kPi * (double) k / (double) n;
            tw[(size_t) k].r = (float) std::cos (a);
            tw[(size_t) k].i = (float) std::sin (a);
        }
    }

    int size() const { return n; }
    void forward (Cpx* a) const { run (a, false); }
    void inverse (Cpx* a) const { run (a, true); }   // incluye la escala 1/N

private:
    void run (Cpx* a, bool inv) const
    {
        for (int i = 0; i < n; ++i)
        {
            const int j = rev[(size_t) i];
            if (j > i) std::swap (a[i], a[j]);
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len >> 1, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < half; ++k)
                {
                    Cpx w = tw[(size_t) (k * step)];
                    if (inv) w.i = -w.i;
                    Cpx& u = a[i + k];
                    Cpx& v = a[i + k + half];
                    const float tr = v.r * w.r - v.i * w.i;
                    const float ti = v.r * w.i + v.i * w.r;
                    v.r = u.r - tr; v.i = u.i - ti;
                    u.r += tr;      u.i += ti;
                }
        }
        if (inv)
        {
            const float s = 1.0f / (float) n;
            for (int i = 0; i < n; ++i) { a[i].r *= s; a[i].i *= s; }
        }
    }

    int n;
    std::vector<int> rev;
    std::vector<Cpx> tw;
};

// STFT con ventana de Hann periodica. El fotograma f empieza en f*hop-(N-hop), de modo
// que cada muestra de la senal queda cubierta por N/hop fotogramas (reconstruccion perfecta).
class Stft
{
public:
    Stft (int fftSize, int hopSize)
        : N (fftSize), hop (hopSize), fft (fftSize), win ((size_t) fftSize)
    {
        for (int i = 0; i < N; ++i)
            win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) N));
    }

    int fftSize() const { return N; }
    int hopSize() const { return hop; }
    int bins() const { return N / 2 + 1; }
    int numFrames (int n) const { return n <= 0 ? 0 : (n + N - hop + hop - 1) / hop; }
    int frameStart (int f) const { return f * hop - (N - hop); }

    // out: numFrames*bins espectros (bins 0..N/2).
    void analyze (const float* x, int n, std::vector<Cpx>& out) const
    {
        const int F = numFrames (n), B = bins();
        out.assign ((size_t) F * (size_t) B, Cpx {});
        std::vector<Cpx> buf ((size_t) N);
        for (int f = 0; f < F; ++f)
        {
            const int s = frameStart (f);
            for (int j = 0; j < N; ++j)
            {
                const int idx = s + j;
                buf[(size_t) j].r = (idx >= 0 && idx < n) ? x[idx] * win[(size_t) j] : 0.0f;
                buf[(size_t) j].i = 0.0f;
            }
            fft.forward (buf.data());
            for (int k = 0; k < B; ++k)
                out[(size_t) f * (size_t) B + (size_t) k] = buf[(size_t) k];
        }
    }

    // y: n muestras (se sobrescribe).
    void synthesize (const std::vector<Cpx>& spec, int n, std::vector<float>& y) const
    {
        const int F = numFrames (n), B = bins();
        std::vector<double> acc ((size_t) n, 0.0), nrm ((size_t) n, 0.0);
        std::vector<Cpx> buf ((size_t) N);
        for (int f = 0; f < F; ++f)
        {
            const Cpx* sp = &spec[(size_t) f * (size_t) B];
            for (int k = 0; k < B; ++k) buf[(size_t) k] = sp[k];
            buf[0].i = 0.0f;
            buf[(size_t) (N / 2)].i = 0.0f;
            for (int k = 1; k < N / 2; ++k)
            {
                buf[(size_t) (N - k)].r = sp[k].r;
                buf[(size_t) (N - k)].i = -sp[k].i;
            }
            fft.inverse (buf.data());
            const int s = frameStart (f);
            for (int j = 0; j < N; ++j)
            {
                const int idx = s + j;
                if (idx < 0 || idx >= n) continue;
                const double w = (double) win[(size_t) j];
                acc[(size_t) idx] += (double) buf[(size_t) j].r * w;
                nrm[(size_t) idx] += w * w;
            }
        }
        y.assign ((size_t) n, 0.0f);
        for (int i = 0; i < n; ++i)
            y[(size_t) i] = nrm[(size_t) i] > 1.0e-9 ? (float) (acc[(size_t) i] / nrm[(size_t) i]) : 0.0f;
    }

private:
    int N, hop;
    Fft fft;
    std::vector<float> win;
};
} // namespace kc
