// Prueba de humo del procesador real (sin DAW): MIDI -> audio, carga y analisis de audio,
// seleccion de golpes, ajuste por muestras, estado guardar/restaurar (incluida la v1),
// exportaciones, audicion del aislado, salida mono y captura de la interfaz.
// Uso: KickCloneSmokeTest <carpeta_de_salida>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <chrono>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) do { if (! (cond)) { std::printf ("  FALLO: %s\n", msg); ++failures; } else std::printf ("  ok: %s\n", msg); } while (0)

static float peakOf (const juce::AudioBuffer<float>& b)
{
    float p = 0; bool finite = true;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float s = b.getSample (ch, i);
            finite = finite && std::isfinite (s);
            p = std::max (p, std::abs (s));
        }
    return finite ? p : -1.0f;
}

static void saveSnapshot (juce::AudioProcessorEditor& ed, const juce::File& out)
{
    juce::Image img (juce::Image::ARGB, ed.getWidth(), ed.getHeight(), true);
    juce::Graphics g (img);
    ed.paintEntireComponent (g, false);
    out.deleteFile();
    juce::FileOutputStream os (out);
    juce::PNGImageFormat png;
    png.writeImageToStream (img, os);
}

static double nowSec()
{
    return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Audio de prueba: bombo, hi-hat, bombo (mas grave), caja; con un poco de ruido de fondo.
static void writeTestAudio (const juce::File& wav, double sr, double seconds, bool longLoop)
{
    std::vector<float> data ((size_t) (sr * seconds), 0.0f);
    juce::Random rnd (3);
    for (auto& s : data) s = 0.002f * (rnd.nextFloat() * 2 - 1);

    auto addKick = [&] (double at, double f0, double fEnd, double tauP, double tauA)
    {
        double phase = 0.0;
        for (int i = 0; i < (int) (sr * 0.7); ++i)
        {
            const size_t idx = (size_t) (at * sr) + (size_t) i;
            if (idx >= data.size()) break;
            const double t = i / sr;
            phase += (fEnd + (f0 - fEnd) * std::exp (-t / tauP)) / sr;
            data[idx] += (float) (0.75 * (std::sin (6.2831853 * phase) * std::exp (-t / tauA)
                                          + 0.22 * (rnd.nextFloat() * 2 - 1) * std::exp (-t / 0.0015)));
        }
    };
    auto addHat = [&] (double at)
    {
        float prev = 0.0f;
        for (int i = 0; i < (int) (sr * 0.04); ++i)
        {
            const size_t idx = (size_t) (at * sr) + (size_t) i;
            if (idx >= data.size()) break;
            const float n = rnd.nextFloat() * 2 - 1;
            data[idx] += 0.4f * (n - prev) * std::exp (-(float) i / (float) (sr * 0.008));
            prev = n;
        }
    };

    if (! longLoop)
    {
        addKick (0.25, 185, 48, 0.020, 0.11);
        addHat (0.75);
        addKick (1.25, 230, 42, 0.016, 0.14);
        addHat (1.75);
    }
    else
        for (int i = 0; i < 40; ++i)
        {
            if (i % 2 == 0) addKick (0.1 + i * 0.5, 190, 48, 0.02, 0.11); else addHat (0.1 + i * 0.5);
        }

    juce::AudioBuffer<float> b (1, (int) data.size());
    b.copyFrom (0, 0, data.data(), (int) data.size());
    wav.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> os (wav.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (os.get(), sr, 1, 24, {}, 0));
    if (w) { os.release(); w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File outDir (argc > 1 ? argv[1] : ".");
    outDir.createDirectory();

    std::printf ("Procesador: MIDI -> audio (estereo)\n");
    KickCloneProcessor proc;
    proc.setPlayConfigDetails (0, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);
    {
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 110), 10);
        proc.processBlock (buf, midi);
        const float pk = peakOf (buf);
        std::printf ("    pico = %.3f\n", pk);
        CHECK (pk > 0.2f && pk <= 1.0f, "la nota MIDI produce sonido finito y acotado a 0 dBFS");
        CHECK (std::abs (buf.getSample (0, 3)) < 1e-6f, "silencio antes de la nota (posicion de muestra respetada)");
        double diff = 0; for (int i = 0; i < 512; ++i) diff += std::abs (buf.getSample (0, i) - buf.getSample (1, i));
        CHECK (diff > 0.0, "los canales L y R no son identicos (click/ruido estereo)");
        juce::AudioBuffer<float> empty (2, 512);
        juce::MidiBuffer none;
        for (int i = 0; i < 400; ++i) proc.processBlock (empty, none);   // ~4 s
        CHECK (peakOf (empty) < 1e-4f, "el golpe termina y queda en silencio");
    }

    std::printf ("Audio con varios golpes: deteccion, aislamiento y modelado\n");
    const juce::File wav = outDir.getChildFile ("test_loop.wav");
    writeTestAudio (wav, 44100.0, 2.5, false);
    juce::String err;
    const double t0 = nowSec();
    const bool loaded = proc.loadAudioFile (wav, err);
    const double tLoad = nowSec() - t0;
    std::printf ("    carga + deteccion + analisis: %.0f ms\n", tLoad * 1000);
    CHECK (loaded, "loadAudioFile ok");
    auto src = proc.getSource();
    CHECK (src != nullptr && src->events.size() == 4, "detecta los 4 golpes");
    CHECK (src != nullptr && std::abs (src->startSample - (int) (44100 * 0.25)) < 300, "elige el primer bombo (~250 ms)");
    CHECK (src != nullptr && ! src->isolated.empty(), "hay golpe aislado para mostrar y exportar");
    auto m = proc.getModel();
    std::printf ("    fEnd=%.1f Hz, len=%.0f ms, noise pts=%d\n", m.pitch.v[m.pitch.n - 1], m.lengthMs, m.noise.n);
    CHECK (std::abs (m.pitch.v[m.pitch.n - 1] - 48.0f) < 5.0f, "tono final detectado ~48 Hz");
    CHECK (m.noise.n >= 2, "el modelo incluye envolvente de ruido");
    CHECK (tLoad < 1.0, "procesado en menos de 1 s");
    CHECK (! proc.loadAudioFile (outDir.getChildFile ("no_existe.wav"), err), "archivo inexistente -> error limpio");
    CHECK (proc.getSource() != nullptr && proc.getSource()->startSample == src->startSample, "un error de carga no destruye el audio actual");

    std::printf ("Seleccion de otro golpe y ajuste a nivel de muestra\n");
    CHECK (proc.selectAt ((int) (44100 * 1.25) + 400, err), "selectAt sobre el segundo bombo ok");
    auto src2 = proc.getSource();
    auto m2 = proc.getModel();
    std::printf ("    start=%d (esperado ~%d), fEnd=%.1f\n", src2->startSample, (int) (44100 * 1.25), m2.pitch.v[m2.pitch.n - 1]);
    CHECK (std::abs (src2->startSample - (int) (44100 * 1.25)) < 300, "clona el segundo bombo");
    CHECK (std::abs (m2.pitch.v[m2.pitch.n - 1] - 42.0f) < 5.0f, "modelo del segundo bombo (~42 Hz)");
    const int before = src2->startSample;
    CHECK (proc.nudgeStart (+1, err) && proc.getSource()->startSample == before + 1, "nudge +1 muestra");
    CHECK (proc.nudgeStart (-100, err) && proc.getSource()->startSample == before - 99, "nudge -100 muestras");
    CHECK (proc.getModel().valid, "el modelo sigue siendo valido tras el nudge");
    const auto modelBefore = proc.getModel();
    const int startBefore = proc.getSource()->startSample;
    CHECK (! proc.selectAt (0, err) && ! err.isEmpty(), "pulsar en el silencio da un error limpio");
    CHECK (proc.getSource()->startSample == startBefore && ! proc.getSource()->isolated.empty(), "...y no toca el audio aislado actual");
    CHECK (proc.getModel().pitch.n == modelBefore.pitch.n && std::abs (proc.getModel().bodyGain - modelBefore.bodyGain) < 1e-6f, "...ni el modelo actual");

    {
        proc.apvts.getParameter ("tune")->setValueNotifyingHost (proc.apvts.getParameter ("tune")->convertTo0to1 (-5.0f));
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        proc.processBlock (buf, midi);
        CHECK (peakOf (buf) > 0.2f, "suena con el modelo extraido y tune -5 st");
    }

    std::printf ("Audicion del golpe aislado\n");
    {
        proc.triggerIsoPreview();
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer none;
        float pk = 0;
        for (int i = 0; i < 20; ++i) { proc.processBlock (buf, none); pk = std::max (pk, peakOf (buf)); }
        CHECK (pk > 0.1f, "play iso produce el golpe aislado sin MIDI");
        for (int i = 0; i < 400; ++i) proc.processBlock (buf, none);
        CHECK (peakOf (buf) < 1e-4f, "la audicion del aislado termina");
    }

    std::printf ("Estado guardar/restaurar\n");
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    {
        KickCloneProcessor proc2;
        proc2.setStateInformation (state.getData(), (int) state.getSize());
        auto a = proc.getModel(), b = proc2.getModel();
        CHECK (b.valid && b.pitch.n == a.pitch.n && b.amp.n == a.amp.n && b.noise.n == a.noise.n, "modelo restaurado (mismos puntos, incluido el ruido)");
        CHECK (std::abs (b.pitch.v[b.pitch.n - 1] - a.pitch.v[a.pitch.n - 1]) < 0.01f, "valores de envolvente restaurados");
        CHECK (std::abs (b.noiseHz - a.noiseHz) < 1.0f && std::abs (b.noiseQ - a.noiseQ) < 0.01f && std::abs (b.clickHz - a.clickHz) < 1.0f,
               "banda de ruido y corte del click restaurados");
        CHECK (std::abs (proc2.getCurrentParams().tuneSemitones - (-5.0f)) < 0.05f, "parametros restaurados");
        CHECK (proc2.getSource() != nullptr && proc2.getSource()->startSample == proc.getSource()->startSample, "audio de origen e inicio del golpe restaurados");
        CHECK (proc2.getSource() != nullptr && ! proc2.getSource()->isolated.empty(), "golpe aislado reconstruido al restaurar");
        const char garbage[] = "esto no es un estado valido";
        proc2.setStateInformation (garbage, (int) sizeof (garbage));
        CHECK (proc2.getModel().valid, "estado corrupto ignorado sin romper");
    }

    std::printf ("Compatibilidad con estados de la v1\n");
    {
        juce::ValueTree t ("PARAMETERS");
        juce::ValueTree mt ("KICKMODEL");   // sin 'version': formato de la v1
        mt.setProperty ("pitch", "0.000:200.00000,10.000:90.00000,100.000:50.00000", nullptr);
        mt.setProperty ("amp", "0.000:1.00000,100.000:0.40000,300.000:0.00000", nullptr);
        mt.setProperty ("body", 0.8f, nullptr);
        mt.setProperty ("click", 0.25f, nullptr);   // v1: relativo al cuerpo
        mt.setProperty ("clickDecay", 1.5f, nullptr);
        mt.setProperty ("noise", 0.0f, nullptr);
        mt.setProperty ("length", 300.0f, nullptr);
        t.appendChild (mt, nullptr);
        juce::MemoryBlock blk;
        if (auto xml = t.createXml()) juce::AudioProcessor::copyXmlToBinary (*xml, blk);
        KickCloneProcessor p3;
        p3.setStateInformation (blk.getData(), (int) blk.getSize());
        auto v = p3.getModel();
        std::printf ("    click v1 0.25 x cuerpo 0.8 -> %.3f\n", v.clickLevel);
        CHECK (v.valid && v.noise.n >= 2 && std::abs (v.clickLevel - 0.2f) < 0.01f, "estado v1: modelo valido, click convertido a absoluto, ruido por defecto");
        juce::AudioBuffer<float> buf (2, 256);
        p3.setPlayConfigDetails (0, 2, 48000.0, 256);
        p3.prepareToPlay (48000.0, 256);
        juce::MidiBuffer midi; midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), 0);
        p3.processBlock (buf, midi);
        CHECK (peakOf (buf) > 0.1f, "estado v1: suena");
    }

    std::printf ("Exportar WAV\n");
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        const juce::File ex = outDir.getChildFile ("export_clone.wav"), ei = outDir.getChildFile ("export_iso.wav");
        CHECK (proc.exportWav (ex, err), "exportWav (clon) ok");
        CHECK (proc.exportIsolatedWav (ei, err), "exportIsolatedWav ok");
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (ex));
        CHECK (r != nullptr && r->sampleRate == 48000.0 && r->numChannels == 2 && r->bitsPerSample == 24, "clon: WAV 48 kHz estereo 24 bit");
        if (r)
        {
            juce::AudioBuffer<float> b (2, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
            CHECK (b.getMagnitude (0, b.getNumSamples()) > 0.2f && b.getMagnitude (0, b.getNumSamples()) <= 1.0f, "clon con contenido y sin pasar de 0 dBFS");
        }
        std::unique_ptr<juce::AudioFormatReader> ri (fm.createReaderFor (ei));
        CHECK (ri != nullptr && std::abs (ri->sampleRate - 44100.0) < 1.0 && ri->numChannels == 1 && ri->bitsPerSample == 24, "aislado: WAV a la frecuencia del original, mono 24 bit");
        KickCloneProcessor empty;
        CHECK (! empty.exportIsolatedWav (outDir.getChildFile ("nada.wav"), err), "sin audio cargado, exportar el aislado da error limpio");
    }

    std::printf ("Salida mono y modo poly\n");
    {
        KickCloneProcessor pm;
        pm.setPlayConfigDetails (0, 1, 48000.0, 512);
        pm.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buf (1, 512);
        juce::MidiBuffer midi; midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), 0);
        pm.processBlock (buf, midi);
        CHECK (peakOf (buf) > 0.2f, "layout mono: suena");

        auto* vp = pm.apvts.getParameter ("voices");
        vp->setValueNotifyingHost (vp->convertTo0to1 (1.0f));
        CHECK (pm.getCurrentParams().poly, "el parametro voices pasa a poly");
        juce::AudioBuffer<float> big (2, 4096);   // bloque mayor que el anunciado
        pm.setPlayConfigDetails (0, 2, 48000.0, 512);
        juce::MidiBuffer m2; m2.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), 0);
        m2.addEvent (juce::MidiMessage::noteOn (1, 40, (juce::uint8) 100), 2000);
        pm.processBlock (big, m2);
        CHECK (peakOf (big) > 0.2f && peakOf (big) <= 1.0f, "poly con bloque mayor que el anunciado: finito y acotado");
    }

    std::printf ("Tiempo con un audio de 30 s\n");
    {
        const juce::File big = outDir.getChildFile ("test_30s.wav");
        writeTestAudio (big, 44100.0, 30.0, true);
        KickCloneProcessor p5;
        const double t1 = nowSec();
        const bool ok = p5.loadAudioFile (big, err);
        const double dt = nowSec() - t1;
        std::printf ("    carga + deteccion + analisis de 30 s: %.0f ms\n", dt * 1000);
        CHECK (ok && p5.getModel().valid, "audio de 30 s analizado");
        CHECK (dt < 1.0, "procesado de 30 s en menos de 1 s");
        const double t2 = nowSec();
        p5.selectAt ((int) (44100 * 10.1), err);
        std::printf ("    seleccionar otro golpe: %.0f ms\n", (nowSec() - t2) * 1000);
        CHECK ((nowSec() - t2) < 1.0, "cambiar de golpe en menos de 1 s");
    }

    std::printf ("Interfaz\n");
    {
        KickCloneProcessor pe;
        pe.setPlayConfigDetails (0, 2, 44100.0, 512);
        pe.prepareToPlay (44100.0, 512);
        pe.loadAudioFile (wav, err);
        std::unique_ptr<juce::AudioProcessorEditor> ed (pe.createEditor());
        ed->setSize (1060, 660);
        saveSnapshot (*ed, outDir.getChildFile ("editor_loaded.png"));
        ed->setSize (900, 600);   // tamano minimo
        saveSnapshot (*ed, outDir.getChildFile ("editor_small.png"));
        ed.reset();

        KickCloneProcessor fresh;
        std::unique_ptr<juce::AudioProcessorEditor> ed2 (fresh.createEditor());
        ed2->setSize (1060, 660);
        saveSnapshot (*ed2, outDir.getChildFile ("editor_empty.png"));
        CHECK (true, "editor creado y renderizado (con audio, tamano minimo y vacio)");
    }

    std::printf ("\n%s (%d fallos)\n", failures == 0 ? "TODO CORRECTO" : "HAY FALLOS", failures);
    return failures == 0 ? 0 : 1;
}
