#pragma once

#include "PluginProcessor.h"

class MinimalLookAndFeel;
class LinkText;
class EnvelopeEditor;
class WaveformView;
class LabeledSlider;

class KickCloneEditor : public juce::AudioProcessorEditor,
                        public juce::ChangeListener,
                        public juce::FileDragAndDropTarget
{
public:
    explicit KickCloneEditor (KickCloneProcessor&);
    ~KickCloneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void loadFile (const juce::File& file);
    void chooseFile();
    void exportFile();
    void setStatus (const juce::String& text);

    KickCloneProcessor& proc;

    std::unique_ptr<MinimalLookAndFeel> lnf;
    std::unique_ptr<LinkText> selectLink, playLink, exportLink, inputToggle;
    std::unique_ptr<EnvelopeEditor> pitchEditor, ampEditor;
    std::unique_ptr<WaveformView> waves;
    std::vector<std::unique_ptr<LabeledSlider>> sliders;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::String status;
    bool inputOpen = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickCloneEditor)
};
