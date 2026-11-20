#pragma once
// Modelo de bombo (POD, sin asignaciones de memoria) + parametros de sintesis.
// Todo el codigo de Source/Dsp es C++ estandar, sin dependencias de JUCE,
// para poder probarlo con tests independientes.

#include <algorithm>
#include <cmath>

namespace kc
{
constexpr int kMaxPoints = 12;
constexpr float kAxisMaxMs = 1000.0f;   // eje de tiempo de los editores y limite de los puntos
constexpr float kMinHz = 20.0f;
constexpr float kMaxHz = 1000.0f;

// Envolvente multisegmento: n puntos (tiempo en ms, valor).
//  - Pitch: valor en Hz, interpolacion logaritmica, mantiene el ultimo valor.
//  - Amp:   valor lineal 0..1, interpolacion geometrica, silencio tras el ultimo punto.
struct Envelope
{
    int n = 0;
    float t[kMaxPoints] {};
    float v[kMaxPoints] {};

    void clear() { n = 0; }

    bool add (float timeMs, float value)
    {
        if (n >= kMaxPoints) return false;
        t[n] = timeMs; v[n] = value; ++n;
        return true;
    }

    // Inserta manteniendo el orden por tiempo. Devuelve el indice o -1.
    int insertSorted (float timeMs, float value)
    {
        if (n >= kMaxPoints) return -1;
        int i = n;
        while (i > 0 && t[i - 1] > timeMs) { t[i] = t[i - 1]; v[i] = v[i - 1]; --i; }
        t[i] = timeMs; v[i] = value; ++n;
        return i;
    }

    void removeAt (int idx)
    {
        for (int i = idx; i < n - 1; ++i) { t[i] = t[i + 1]; v[i] = v[i + 1]; }
        if (n > 0) --n;
    }
};

inline float evalPitchHz (const Envelope& e, float tMs)
{
    if (e.n == 0) return 50.0f;
    if (tMs <= e.t[0] || e.n == 1) return e.v[0];
    if (tMs >= e.t[e.n - 1]) return e.v[e.n - 1];
    int i = 0;
    while (i < e.n - 2 && tMs > e.t[i + 1]) ++i;
    const float span = std::max (1.0e-4f, e.t[i + 1] - e.t[i]);
    const float a = (tMs - e.t[i]) / span;
    const float l0 = std::log (std::max (e.v[i], 1.0f));
    const float l1 = std::log (std::max (e.v[i + 1], 1.0f));
    return std::exp (l0 + (l1 - l0) * a);
}

inline float evalAmp (const Envelope& e, float tMs)
{
    constexpr float floorV = 1.0e-4f;
    if (e.n == 0) return 0.0f;
    if (tMs < 0.0f) return 0.0f;
    if (tMs >= e.t[e.n - 1]) return 0.0f;
    if (e.n == 1 || tMs <= e.t[0]) return e.v[0];
    int i = 0;
    while (i < e.n - 2 && tMs > e.t[i + 1]) ++i;
    const float span = std::max (1.0e-4f, e.t[i + 1] - e.t[i]);
    const float a = (tMs - e.t[i]) / span;
    const float l0 = std::log (std::max (e.v[i], floorV));
    const float l1 = std::log (std::max (e.v[i + 1], floorV));
    const float r = std::exp (l0 + (l1 - l0) * a);
    return r <= floorV * 1.01f ? 0.0f : r;
}

// Bombo completo: lo que extrae el analizador y lo que el usuario edita.
struct KickModel
{
    bool valid = false;
    Envelope pitch;           // Hz
    Envelope amp;             // 0..1
    float bodyGain = 0.9f;    // amplitud pico del cuerpo
    float clickLevel = 0.25f; // relativo a bodyGain
    float clickDecayMs = 1.5f;
    float noiseLevel = 0.0f;  // relativo a bodyGain
    float noiseDecayMs = 60.0f;
    float lengthMs = 350.0f;

    // Bombo por defecto, para que el plugin suene bien nada mas cargarlo.
    static KickModel makeDefault()
    {
        KickModel m;
        m.valid = true;
        m.pitch.add (0.0f, 230.0f);
        m.pitch.add (4.0f, 150.0f);
        m.pitch.add (12.0f, 88.0f);
        m.pitch.add (35.0f, 58.0f);
        m.pitch.add (90.0f, 48.0f);
        m.pitch.add (200.0f, 46.0f);
        m.amp.add (0.0f, 1.0f);
        m.amp.add (30.0f, 0.85f);
        m.amp.add (100.0f, 0.45f);
        m.amp.add (200.0f, 0.15f);
        m.amp.add (320.0f, 0.02f);
        m.amp.add (420.0f, 0.0f);
        m.bodyGain = 0.9f;
        m.clickLevel = 0.18f;
        m.clickDecayMs = 1.2f;
        m.noiseLevel = 0.0f;
        m.noiseDecayMs = 40.0f;
        m.lengthMs = 420.0f;
        return m;
    }
};

// Parametros "vivos" de la sintesis (se capturan al disparar la nota).
struct KickParams
{
    float tuneSemitones = 0.0f;
    float keytrack = 0.0f;     // 0..1 : cuanto sigue la nota MIDI (relativa a la 36)
    float pitchAmount = 1.0f;  // 0..2 : escala de la caida de tono
    float decayScale = 1.0f;   // 0.25..4 : estira la envolvente de amplitud
    float clickGain = 1.0f;    // 0..2
    float noiseGain = 1.0f;    // 0..2
    float driveDb = 0.0f;      // 0..24
};
} // namespace kc
