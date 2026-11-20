# Kick Clone

Plugin **VST3** (instrumento MIDI) para **Windows** y **macOS** que *clona* un bombo a partir de cualquier audio:
arrastras un archivo con un bombo (una muestra, una canción, un bucle) y el plugin lo analiza y lo resintetiza
con un motor propio, dejando todas las envolventes editables. Está pensado para **FL Studio, Ableton Live 12 y Cubase 15**.

> **Qué es y qué no es.** Es un plugin de resíntesis de bombos inspirado en la idea de *Drumclone* (Rare DSP),
> pero con código propio: el análisis es procesado de señal clásico (no usa modelos de IA) y no comparte código
> ni modelos con Drumclone. Por eso el resultado es una aproximación al bombo original, no una copia exacta.

## Cómo se usa

1. Carga **Kick Clone** como instrumento (pista de instrumento en Ableton/Cubase, *Channel* → generador en FL Studio).
2. Arrastra un audio a la ventana o pulsa **Select an audio file**. El plugin busca el primer bombo claro.
   - Si hay varios golpes (un bucle), **haz clic en la forma de onda** de arriba para elegir cuál clonar.
3. Toca notas MIDI. Cada nota dispara el bombo (por defecto suena siempre a su tono original).
4. Ajusta lo que quieras:
   - **pitch** y **amplitude**: arrastra los puntos. Doble clic en vacío añade un punto; doble clic sobre un punto lo quita.
   - **tune** (±12 st), **pitch drop** (cuánta caída de tono), **decay**, **click**, **noise**, **drive**,
     **key track** (0 % = todas las notas suenan igual; 100 % = sigue el teclado) y **level**.
5. **play** lo escucha sin MIDI; **export .wav** guarda el bombo resultante (48 kHz, mono, 24 bit).

El estado (envolventes, parámetros y ruta del audio de origen) se guarda con el proyecto.

## Qué extrae del audio

| Parte | Cómo se obtiene |
|---|---|
| Inicio del golpe | Energía < 400 Hz y refinado con la señal completa |
| Envolvente de tono | Seguimiento de semiperiodos por cruces por cero del cuerpo (< 400 Hz) |
| Envolvente de amplitud | Máximos del cuerpo en ventanas de un periodo |
| Click | Energía > 1,5 kHz en los primeros 4 ms |
| Cola de ruido | Energía > 1,5 kHz entre 10 y 25 ms y su caída |

Limitaciones conocidas: el cuerpo se modela como un seno (no capta armónicos propios del bombo original),
por debajo de 20 Hz no hay seguimiento de tono y en mezclas densas la extracción puede necesitar retoque manual de las envolventes.

## Descargar

Cada *push* a `main` compila Windows y macOS en GitHub Actions. En la pestaña **Actions → última ejecución → Artifacts**
están `KickClone-Windows-VST3.zip` y `KickClone-macOS-VST3.zip`. Si creas una etiqueta (`git tag v1.0.0 && git push --tags`),
se publica además una *Release* con ambos archivos.

### Instalación

- **Windows**: copia la carpeta `Kick Clone.vst3` a `C:\Program Files\Common Files\VST3\`.
- **macOS**: copia `Kick Clone.vst3` a `/Library/Audio/Plug-Ins/VST3/` (o `~/Library/Audio/Plug-Ins/VST3/`).
  El binario va firmado *ad hoc*, no notarizado: la primera vez macOS puede bloquearlo. Si ocurre, ejecuta
  `xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/Kick Clone.vst3"`.

Después, reescanea plugins en tu DAW.

## Compilar en local

Requisitos: CMake ≥ 3.22, un compilador C++17 (Visual Studio 2022 / Xcode) y conexión a internet (descarga JUCE 8.0.8).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
# resultado: build/KickClone_artefacts/Release/VST3/Kick Clone.vst3
```

### Pruebas

```bash
# Núcleo DSP (sin JUCE)
g++ -std=c++17 -O2 -I Source/Dsp tests/dsp_test.cpp -o dsp_test && ./dsp_test

# Procesador + interfaz (con JUCE; en Linux necesita un display, p. ej. xvfb-run)
cmake -B build -DKICK_BUILD_TESTS=ON && cmake --build build
xvfb-run -a build/KickCloneSmokeTest_artefacts/Release/KickCloneSmokeTest salida/
```

## Estructura

```
Source/Dsp/        Núcleo sin dependencias: modelo, analizador y motor de síntesis
Source/Plugin*.*   Procesador y editor (JUCE)
tests/             Pruebas del DSP y prueba de humo del procesador/interfaz
.github/workflows/ CI: tests + VST3 para Windows y macOS
```

## Licencia

El plugin se construye con [JUCE](https://juce.com) (AGPLv3 o licencia comercial de JUCE). Si distribuyes los binarios
debes cumplir con la licencia de JUCE que elijas; este proyecto aún no incluye un archivo de licencia propio, añade el que corresponda.
VST es una marca registrada de Steinberg Media Technologies GmbH.
