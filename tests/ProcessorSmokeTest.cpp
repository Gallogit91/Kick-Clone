// Prueba de humo del procesador real (sin DAW): MIDI -> audio, carga de audio,
// estado guardar/restaurar, exportacion WAV y captura de la interfaz.
// Uso: KickCloneSmokeTest <carpeta_de_salida>
#include "PluginProcessor.h"
#include "PluginEditor.h"
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

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File outDir (argc > 1 ? argv[1] : ".");
    outDir.createDirectory();

    std::printf ("Procesador: MIDI -> audio\n");
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
        CHECK (pk > 0.2f && pk <= 1.2f, "la nota MIDI produce sonido finito");
        CHECK (std::abs (buf.getSample (0, 3)) < 1e-6f, "silencio antes de la nota (posicion de muestra respetada)");
        CHECK (juce::approximatelyEqual (buf.getSample (0, 200), buf.getSample (1, 200)), "canales L y R iguales");
        juce::AudioBuffer<float> empty (2, 512);
        juce::MidiBuffer none;
        for (int i = 0; i < 400; ++i) proc.processBlock (empty, none);   // ~4 s
        CHECK (peakOf (empty) < 1e-4f, "el golpe termina y queda en silencio");
    }

    std::printf ("Cargar audio y analizar\n");
    const juce::File wav = outDir.getChildFile ("test_kick.wav");
    {
        const double sr = 44100.0;
        std::vector<float> data ((size_t) (sr * 2.0), 0.0f);
        double phase = 0.0;
        juce::Random rnd (3);
        for (int i = 0; i < (int) (sr * 0.7); ++i)
        {
            const double t = i / sr;
            phase += (48.0 + 170.0 * std::exp (-t / 0.018)) / sr;
            const double y = std::sin (6.2831853 * phase) * std::exp (-t / 0.11) * 0.8
                           + 0.25 * (rnd.nextFloat() * 2 - 1) * std::exp (-t / 0.0015);
            data[(size_t) (sr * 0.25) + (size_t) i] = (float) y;
        }
        juce::AudioBuffer<float> b (1, (int) data.size());
        b.copyFrom (0, 0, data.data(), (int) data.size());
        wav.deleteFile();
        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::FileOutputStream> os (wav.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (os.get(), sr, 1, 24, {}, 0));
        if (w) { os.release(); w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); }
    }
    juce::String err;
    CHECK (proc.loadAudioFile (wav, err), ("loadAudioFile ok " + err).toRawUTF8());
    auto m = proc.getModel();
    std::printf ("    fEnd=%.1f Hz, len=%.0f ms\n", m.pitch.v[m.pitch.n - 1], m.lengthMs);
    CHECK (std::abs (m.pitch.v[m.pitch.n - 1] - 48.0f) < 5.0f, "tono final detectado ~48 Hz");
    CHECK (proc.getSource() != nullptr && std::abs (proc.getSource()->startSample - (int) (44100 * 0.25)) < 300, "inicio ~250 ms");
    CHECK (! proc.loadAudioFile (outDir.getChildFile ("no_existe.wav"), err), "archivo inexistente -> error limpio");

    {
        proc.apvts.getParameter ("tune")->setValueNotifyingHost (proc.apvts.getParameter ("tune")->convertTo0to1 (-5.0f));
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        proc.processBlock (buf, midi);
        CHECK (peakOf (buf) > 0.2f, "suena con el modelo extraido y tune -5 st");
    }

    std::printf ("Estado guardar/restaurar\n");
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    KickCloneProcessor proc2;
    proc2.setStateInformation (state.getData(), (int) state.getSize());
    auto m2 = proc2.getModel();
    CHECK (m2.valid && m2.pitch.n == m.pitch.n && m2.amp.n == m.amp.n, "modelo restaurado (mismos puntos)");
    CHECK (std::abs (m2.pitch.v[m2.pitch.n - 1] - m.pitch.v[m.pitch.n - 1]) < 0.01f, "valores de envolvente restaurados");
    CHECK (std::abs (proc2.getCurrentParams().tuneSemitones - (-5.0f)) < 0.05f, "parametros restaurados");
    CHECK (proc2.getSource() != nullptr, "audio de origen recargado desde la ruta guardada");
    CHECK (proc2.getSource() != nullptr && proc2.getSource()->startSample == proc.getSource()->startSample,
           "inicio del golpe restaurado");
    const char garbage[] = "esto no es un estado valido";
    proc2.setStateInformation (garbage, (int) sizeof (garbage));
    CHECK (proc2.getModel().valid, "estado corrupto ignorado sin romper");

    std::printf ("Exportar WAV\n");
    const juce::File exp = outDir.getChildFile ("export.wav");
    CHECK (proc.exportWav (exp, err), "exportWav ok");
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (exp));
        CHECK (r != nullptr && r->sampleRate == 48000.0 && r->numChannels == 1 && r->bitsPerSample == 24, "WAV 48 kHz mono 24 bit legible");
        if (r)
        {
            juce::AudioBuffer<float> b (1, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, false);
            CHECK (b.getMagnitude (0, b.getNumSamples()) > 0.2f, "WAV con contenido");
        }
    }

    std::printf ("Interfaz\n");
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc2.createEditor());
        ed->setSize (980, 560);
        saveSnapshot (*ed, outDir.getChildFile ("editor_loaded.png"));
        ed.reset();

        KickCloneProcessor fresh;
        std::unique_ptr<juce::AudioProcessorEditor> ed2 (fresh.createEditor());
        ed2->setSize (980, 560);
        saveSnapshot (*ed2, outDir.getChildFile ("editor_empty.png"));
        CHECK (true, "editor creado y renderizado");
    }

    std::printf ("\n%s (%d fallos)\n", failures == 0 ? "TODO CORRECTO" : "HAY FALLOS", failures);
    return failures == 0 ? 0 : 1;
}
