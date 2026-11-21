# Kick Clone

Plugin **VST3** (instrumento MIDI) para **Windows** y **macOS** que *clona* un bombo a partir de cualquier audio:
arrastras un archivo con un bombo (una muestra, una canción, un bucle) y el plugin lo analiza y lo resintetiza
con un motor propio, dejando todas las envolventes editables. Está pensado para **FL Studio, Ableton Live 12 y Cubase 15**.

> **Qué es y qué no es.** Es un plugin de resíntesis de bombos inspirado en la idea de *Drumclone* (Rare DSP),
> pero con código propio: el análisis es procesado de señal clásico (no usa modelos de IA) y no comparte código
> ni modelos con Drumclone. Por eso el resultado es una aproximación al bombo original, no una copia exacta.

## Novedades de la versión 2.0

- **Pipeline de 5 etapas**: análisis tiempo/frecuencia (flujo espectral, tono, nivel, umbral adaptativo) → extracción con máscaras espectrales suaves
  (cuerpo / click / ruido) → resíntesis con dinámica y estéreo → osciladores (seno + ruido pasa-banda) con envolventes de modulación → mezcla y postproceso.
- **Editores de envolvente multisegmento**: *pitch*, *body amplitude* y *noise* (arrastrar puntos; Shift = ajuste fino; rueda = zoom; doble clic en vacío añade punto, sobre un punto lo quita).
- **Ajuste a nivel de muestra** del inicio del golpe (◂ ▸, Shift ×10, Ctrl/Cmd ×100, o flechas del teclado) con zoom de forma de onda hasta ver cada muestra.
- **Forma de onda en tiempo real**: aislado vs. original vs. clon, que se actualiza con cada cambio.
- **Modelos de síntesis sin filtrado de la fuente**: el clon se genera, no se filtra, así que no arrastra artefactos del audio original.
- **Controles**: tune, pitch drop, decay, harmonics, drive, click, click decay, noise, noise tone, noise decay, width (estéreo), dynamics (compresor + limitador), key track, level; voces mono/poly.
- **play iso** escucha el golpe aislado; **export clone** (estéreo, 48 kHz, 24 bit) y **export iso** (mono, frecuencia de la fuente).
- Arrastrar y soltar, 100 % offline; un análisis de 30 s tarda ~0,1 s en la máquina de pruebas.

## Cómo se usa

1. Carga **Kick Clone** como instrumento (pista de instrumento en Ableton/Cubase, generador en FL Studio).
2. Arrastra un audio a la ventana o pulsa **Select an audio file**. Se detectan los golpes; si hay varios, haz clic en un marcador de la forma de onda.
3. Toca notas MIDI. Ajusta envolventes y controles; los cambios se ven y se oyen al instante.
4. El estado (envolventes, parámetros y ruta del audio) se guarda con el proyecto; los proyectos de la v1 se cargan igual.

## Limitaciones honestas

- Es procesado de señal clásico, no un modelo de IA, y no comparte código ni modelos con Drumclone: es una aproximación, no una copia exacta.
- En mezclas con bajo o caja sonando a la vez, el cuerpo extraído puede contaminarse; retoca las envolventes.
- Compilado y probado en Linux (pruebas DSP + prueba de humo del procesador/UI). Windows, macOS y los DAW los verifica GitHub Actions al compilar, pero no se ha probado el plugin dentro de FL Studio, Ableton ni Cubase.
- JUCE se usa bajo AGPLv3 / licencia comercial: para distribuir el plugin comercialmente necesitas una licencia de JUCE.

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
