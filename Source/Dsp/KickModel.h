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
//  - Amp / Noise: valor lineal 0..1, interpolacion geometrica, silencio tras el ultimo punto.
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

    float endMs() const { return n > 0 ? t[n - 1] : 0.0f; }
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
// Todos los niveles son absolutos (amplitud de la senal, no relativos).
struct KickModel
{
    bool valid = false;

    // Cuerpo: oscilador senoidal
    Envelope pitch;            // Hz
    Envelope amp;              // 0..1, multiplica a bodyGain
    float bodyGain = 0.9f;     // amplitud pico del cuerpo

    // Transitorio: rafaga de ruido filtrado paso alto que decae exponencialmente
    float clickLevel = 0.15f;  // RMS al inicio
    float clickDecayMs = 1.5f; // constante de tiempo
    float clickHz = 1500.0f;   // frecuencia de corte del paso alto

    // Ruido: ruido blanco por un paso banda, con envolvente multisegmento (RMS)
    Envelope noise;
    float noiseHz = 4000.0f;
    float noiseQ = 0.7f;

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
        m.clickHz = 1800.0f;
        m.noise.add (0.0f, 0.0f);
        m.noise.add (6.0f, 0.03f);
        m.noise.add (40.0f, 0.008f);
        m.noise.add (120.0f, 0.0f);
        m.noiseHz = 3500.0f;
        m.noiseQ = 0.8f;
        m.lengthMs = 420.0f;
        return m;
    }
};

// Parametros "vivos" de la sintesis (se capturan al disparar la nota).
struct KickParams
{
    float tuneSemitones = 0.0f;
    float keytrack = 0.0f;         // 0..1 : cuanto sigue la nota MIDI (relativa a la 36)
    float pitchAmount = 1.0f;      // 0..2 : escala de la caida de tono
    float decayScale = 1.0f;       // 0.25..4 : estira la envolvente de amplitud del cuerpo
    float harmonics = 0.0f;        // 0..1 : de seno puro a armonicos ricos
    float driveDb = 0.0f;          // 0..24
    float clickGain = 1.0f;        // 0..2
    float clickDecayScale = 1.0f;  // 0.25..4
    float noiseGain = 1.0f;        // 0..2
    float noiseToneSt = 0.0f;      // -24..24 : desplaza la banda del ruido
    float noiseDecayScale = 1.0f;  // 0.25..4 : estira la envolvente del ruido
    float width = 0.6f;            // 0..1.4 : 0 = mono, 1 = canales de ruido independientes
    float dynamics = 0.0f;         // 0..1 : compresion/limitacion de salida
    bool poly = false;             // false = monofonico (cada nota corta la anterior)
};
} // namespace kc
