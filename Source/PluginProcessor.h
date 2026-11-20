#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>

#include "Dsp/KickModel.h"
#include "Dsp/KickAnalyzer.h"
#include "Dsp/KickEngine.h"

// Audio de origen (mono) del que se ha extraido el bombo.
struct SourceAudio
{
    std::vector<float> mono;
    double sampleRate = 44100.0;
    int startSample = 0;      // inicio del golpe analizado
    juce::File file;
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
    // Carga un audio, lo analiza y reemplaza el modelo. Devuelve false (con 'error') si falla.
    // Con analyse=false solo se carga el audio para mostrarlo (se conserva el modelo actual);
    // 'startHint' es entonces el inicio del golpe que se resalta.
    bool loadAudioFile (const juce::File& file, juce::String& error, bool analyse = true, int startHint = 0);
    // Vuelve a analizar el audio cargado buscando el golpe a partir de 'sample'.
    bool reanalyzeFrom (int sample, juce::String& error);

    kc::KickModel getModel() const;
    void setModel (const kc::KickModel& m);
    int getModelVersion() const { return modelVersion.load(); }
    std::shared_ptr<const SourceAudio> getSource() const;

    kc::KickParams getCurrentParams() const;
    float getLevelDb() const { return pLevel->load(); }

    void triggerPreview() { previewRequested.store (true); }
    bool exportWav (const juce::File& file, juce::String& error) const;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::ValueTree modelToTree() const;
    void modelFromTree (const juce::ValueTree& t);

    // Modelo compartido con el hilo de audio (copia POD bajo spin lock)
    kc::KickModel model;
    mutable juce::SpinLock modelLock;
    std::atomic<int> modelVersion { 0 };

    kc::KickModel audioModel;
    int audioModelVersion = -1;

    std::shared_ptr<const SourceAudio> source;
    mutable juce::SpinLock sourceLock;

    kc::KickEngine engine;
    juce::SmoothedValue<float> levelSmoothed;
    std::atomic<bool> previewRequested { false };

    std::atomic<float>* pTune = nullptr;
    std::atomic<float>* pPitch = nullptr;
    std::atomic<float>* pDecay = nullptr;
    std::atomic<float>* pClick = nullptr;
    std::atomic<float>* pNoise = nullptr;
    std::atomic<float>* pDrive = nullptr;
    std::atomic<float>* pKey = nullptr;
    std::atomic<float>* pLevel = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickCloneProcessor)
};
