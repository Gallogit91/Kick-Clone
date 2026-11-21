#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>

#include "Dsp/KickModel.h"
#include "Dsp/KickAnalyzer.h"
#include "Dsp/KickEngine.h"

// Audio de origen (inmutable una vez publicado) y lo que se extrajo de el.
struct SourceAudio
{
    std::shared_ptr<const std::vector<float>> mono;
    double sampleRate = 44100.0;
    int startSample = 0;                 // inicio del golpe analizado
    juce::File file;
    std::vector<kc::DrumEvent> events;   // golpes detectados (para elegir cual clonar)
    std::vector<float> isolated;         // cuerpo + click + ruido aislados, desde startSample

    int numSamples() const { return mono != nullptr ? (int) mono->size() : 0; }
};

class KickCloneProcessor : public juce::AudioProcessor,
                           public juce::ChangeBroadcaster
{
public:
    KickCloneProcessor();
    ~KickCloneProcessor() override = default;

    // --- AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Kick Clone"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 1.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- API para el editor (hilo de mensajes)
    // Carga un audio, detecta los golpes, aisla el bombo y ajusta el modelo. Con analyse=false solo
    // se carga el audio para mostrarlo y se conserva el modelo actual ('startHint' = golpe resaltado).
    bool loadAudioFile (const juce::File& file, juce::String& error, bool analyse = true, int startHint = 0);
    // Clona el golpe mas cercano a 'sample' (o el que haya justo ahi si no hay ninguno detectado).
    bool selectAt (int sample, juce::String& error);
    // Mueve el inicio del golpe 'delta' muestras y vuelve a extraer (ajuste a nivel de muestra).
    bool nudgeStart (int delta, juce::String& error);

    kc::KickModel getModel() const;
    void setModel (const kc::KickModel& m);
    int getModelVersion() const { return modelVersion.load(); }
    std::shared_ptr<const SourceAudio> getSource() const;

    kc::KickParams getCurrentParams() const;
    double getCurrentSampleRate() const { return currentSampleRate; }

    void triggerPreview() { previewRequested.store (true); }          // suena el clon
    void triggerIsoPreview() { isoRequested.store (true); }           // suena el golpe aislado
    bool exportWav (const juce::File& file, juce::String& error) const;          // el clon (estereo)
    bool exportIsolatedWav (const juce::File& file, juce::String& error) const;  // el aislado (mono)

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::ValueTree modelToTree() const;
    void modelFromTree (const juce::ValueTree& t);

    // publishOnFail: si el analisis no encuentra bombo, publica igualmente el audio nuevo (carga) o
    // deja todo como estaba (clic o ajuste sobre un audio ya cargado).
    bool applyAnalysis (std::shared_ptr<SourceAudio> src, kc::Analysis&& a, bool replaceModel, bool publishOnFail, juce::String& error);
    void publishSource (std::shared_ptr<SourceAudio> src);
    void mixIsoPreview (float* l, float* r, int n);

    // Modelo compartido con el hilo de audio (copia POD bajo spin lock)
    kc::KickModel model;
    mutable juce::SpinLock modelLock;
    std::atomic<int> modelVersion { 0 };

    kc::KickModel audioModel;
    int audioModelVersion = -1;

    std::shared_ptr<const SourceAudio> source;
    mutable juce::SpinLock sourceLock;

    // Audicion del golpe aislado
    std::vector<float> isoBuf;
    double isoSr = 44100.0;
    juce::SpinLock isoLock;
    std::atomic<bool> isoRequested { false };
    bool isoPlaying = false;
    double isoPos = 0.0;

    kc::KickEngine engine;
    kc::PostChain post;
    juce::SmoothedValue<float> levelSmoothed;
    std::atomic<bool> previewRequested { false };
    std::vector<float> scratchR;
    double currentSampleRate = 44100.0;

    std::atomic<float>* pTune = nullptr;
    std::atomic<float>* pPitch = nullptr;
    std::atomic<float>* pDecay = nullptr;
    std::atomic<float>* pHarm = nullptr;
    std::atomic<float>* pDrive = nullptr;
    std::atomic<float>* pClick = nullptr;
    std::atomic<float>* pClickDecay = nullptr;
    std::atomic<float>* pNoise = nullptr;
    std::atomic<float>* pNoiseTone = nullptr;
    std::atomic<float>* pNoiseDecay = nullptr;
    std::atomic<float>* pWidth = nullptr;
    std::atomic<float>* pDyn = nullptr;
    std::atomic<float>* pKey = nullptr;
    std::atomic<float>* pLevel = nullptr;
    std::atomic<float>* pVoices = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickCloneProcessor)
};
