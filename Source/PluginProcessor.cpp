#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
juce::String envToString (const kc::Envelope& e)
{
    juce::StringArray parts;
    for (int i = 0; i < e.n; ++i)
        parts.add (juce::String (e.t[i], 3) + ":" + juce::String (e.v[i], 5));
    return parts.joinIntoString (",");
}

kc::Envelope envFromString (const juce::String& s)
{
    kc::Envelope e;
    for (auto& tok : juce::StringArray::fromTokens (s, ",", ""))
    {
        auto kv = juce::StringArray::fromTokens (tok, ":", "");
        if (kv.size() == 2)
            e.add (kv[0].getFloatValue(), kv[1].getFloatValue());
    }
    return e;
}

// Deja la envolvente en un estado seguro (tiempos crecientes, valores acotados).
bool sanitize (kc::Envelope& e, bool isPitch)
{
    if (e.n < 2) return false;
    float lastT = -1.0f;
    for (int i = 0; i < e.n; ++i)
    {
        e.t[i] = juce::jlimit (0.0f, kc::kAxisMaxMs, e.t[i]);
        if (i == 0) e.t[i] = 0.0f;
        else if (e.t[i] <= lastT) e.t[i] = juce::jmin (kc::kAxisMaxMs, lastT + 0.5f);
        lastT = e.t[i];
        e.v[i] = isPitch ? juce::jlimit (kc::kMinHz, kc::kMaxHz, e.v[i])
                         : juce::jlimit (0.0f, 1.0f, e.v[i]);
    }
    return true;
}

void putU32 (std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back ((uint8_t) ((v >> (8 * i)) & 0xff)); }
void putU16 (std::vector<uint8_t>& b, uint16_t v) { for (int i = 0; i < 2; ++i) b.push_back ((uint8_t) ((v >> (8 * i)) & 0xff)); }
void putTag (std::vector<uint8_t>& b, const char* t) { for (int i = 0; i < 4; ++i) b.push_back ((uint8_t) t[i]); }
} // namespace

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout KickCloneProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto attrs = [] (const String& unit, int decimals, bool signedText)
    {
        return AudioParameterFloatAttributes()
            .withLabel (unit)
            .withStringFromValueFunction ([=] (float v, int)
            {
                String s (v, decimals);
                if (signedText && v > 0.0f) s = "+" + s;
                return s + " " + unit;
            });
    };

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "tune", 1 }, "Tune",
                NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f, attrs ("st", 2, true)));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "pitchDrop", 1 }, "Pitch Drop",
                NormalisableRange<float> (0.0f, 200.0f, 0.1f), 100.0f, attrs ("%", 0, false)));

    NormalisableRange<float> decayRange (25.0f, 400.0f, 0.1f);
    decayRange.setSkewForCentre (100.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "decay", 1 }, "Decay",
                decayRange, 100.0f, attrs ("%", 0, false)));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "click", 1 }, "Click",
                NormalisableRange<float> (0.0f, 200.0f, 0.1f), 100.0f, attrs ("%", 0, false)));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "noise", 1 }, "Noise",
                NormalisableRange<float> (0.0f, 200.0f, 0.1f), 100.0f, attrs ("%", 0, false)));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "drive", 1 }, "Drive",
                NormalisableRange<float> (0.0f, 24.0f, 0.1f), 0.0f, attrs ("dB", 1, false)));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "keytrack", 1 }, "Key Track",
                NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, attrs ("%", 0, false)));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "level", 1 }, "Level",
                NormalisableRange<float> (-24.0f, 6.0f, 0.1f), 0.0f, attrs ("dB", 1, true)));

    return layout;
}

KickCloneProcessor::KickCloneProcessor()
    : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createLayout())
{
    pTune  = apvts.getRawParameterValue ("tune");
    pPitch = apvts.getRawParameterValue ("pitchDrop");
    pDecay = apvts.getRawParameterValue ("decay");
    pClick = apvts.getRawParameterValue ("click");
    pNoise = apvts.getRawParameterValue ("noise");
    pDrive = apvts.getRawParameterValue ("drive");
    pKey   = apvts.getRawParameterValue ("keytrack");
    pLevel = apvts.getRawParameterValue ("level");

    model = kc::KickModel::makeDefault();
    audioModel = model;
}

//==============================================================================
void KickCloneProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate);
    levelSmoothed.reset (sampleRate, 0.02);
    levelSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pLevel->load()));
    audioModelVersion = -1;
}

bool KickCloneProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

kc::KickParams KickCloneProcessor::getCurrentParams() const
{
    kc::KickParams p;
    p.tuneSemitones = pTune->load();
    p.pitchAmount = pPitch->load() * 0.01f;
    p.decayScale = pDecay->load() * 0.01f;
    p.clickGain = pClick->load() * 0.01f;
    p.noiseGain = pNoise->load() * 0.01f;
    p.driveDb = pDrive->load();
    p.keytrack = pKey->load() * 0.01f;
    return p;
}

void KickCloneProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    buffer.clear();
    if (numChannels == 0 || numSamples == 0)
        return;

    // Copia local del modelo (sin bloquear: si el editor lo esta cambiando, se usa la anterior)
    const int v = modelVersion.load();
    if (v != audioModelVersion)
    {
        const juce::SpinLock::ScopedTryLockType lock (modelLock);
        if (lock.isLocked())
        {
            audioModel = model;
            audioModelVersion = v;
        }
    }

    const kc::KickParams params = getCurrentParams();
    float* out = buffer.getWritePointer (0);
    int pos = 0;

    if (previewRequested.exchange (false))
        engine.noteOn (audioModel, params, 36, 1.0f);

    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();
        const int sp = juce::jlimit (0, numSamples - 1, metadata.samplePosition);
        if (sp > pos)
        {
            engine.render (out + pos, sp - pos);
            pos = sp;
        }

        if (msg.isNoteOn())
            engine.noteOn (audioModel, params, msg.getNoteNumber(), msg.getFloatVelocity());
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            engine.reset();
    }

    if (pos < numSamples)
        engine.render (out + pos, numSamples - pos);

    levelSmoothed.setTargetValue (juce::Decibels::decibelsToGain (pLevel->load()));
    for (int i = 0; i < numSamples; ++i)
        out[i] *= levelSmoothed.getNextValue();

    for (int ch = 1; ch < numChannels; ++ch)
        buffer.copyFrom (ch, 0, buffer, 0, 0, numSamples);

    midi.clear();
}

juce::AudioProcessorEditor* KickCloneProcessor::createEditor()
{
    return new KickCloneEditor (*this);
}

//==============================================================================
kc::KickModel KickCloneProcessor::getModel() const
{
    const juce::SpinLock::ScopedLockType lock (modelLock);
    return model;
}

void KickCloneProcessor::setModel (const kc::KickModel& m)
{
    {
        const juce::SpinLock::ScopedLockType lock (modelLock);
        model = m;
    }
    ++modelVersion;
    sendChangeMessage();
}

std::shared_ptr<const SourceAudio> KickCloneProcessor::getSource() const
{
    const juce::SpinLock::ScopedLockType lock (sourceLock);
    return source;
}

bool KickCloneProcessor::loadAudioFile (const juce::File& file, juce::String& error, bool analyse, int startHint)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples < 64 || reader->sampleRate < 8000.0)
    {
        error = "That file could not be read as audio.";
        return false;
    }

    const double sr = reader->sampleRate;
    const int numSamples = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (sr * 30.0));
    const int numCh = (int) juce::jmin<unsigned int> (reader->numChannels, 2u);

    juce::AudioBuffer<float> buf (numCh, numSamples);
    if (! reader->read (&buf, 0, numSamples, 0, true, numCh > 1))
    {
        error = "Reading the audio failed.";
        return false;
    }

    auto src = std::make_shared<SourceAudio>();
    src->sampleRate = sr;
    src->file = file;
    src->mono.assign ((size_t) numSamples, 0.0f);
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* d = buf.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
            src->mono[(size_t) i] += d[i] / (float) numCh;
    }

    kc::KickModel m;
    if (analyse)
    {
        int start = 0;
        m = kc::analyzeKick (src->mono.data(), numSamples, sr, 0, &start);
        src->startSample = start;
    }
    else
        src->startSample = juce::jlimit (0, juce::jmax (0, numSamples - 1), startHint);

    {
        const juce::SpinLock::ScopedLockType lock (sourceLock);
        source = src;
    }

    if (analyse)
    {
        if (! m.valid)
        {
            error = "No clear kick found. Click on the waveform to point at one.";
            sendChangeMessage();
            return false;
        }
        setModel (m);
    }
    else
        sendChangeMessage();

    return true;
}

bool KickCloneProcessor::reanalyzeFrom (int sample, juce::String& error)
{
    auto src = getSource();
    if (src == nullptr || src->mono.empty())
    {
        error = "Load an audio file first.";
        return false;
    }

    int start = 0;
    const int from = juce::jmax (0, sample - (int) (0.02 * src->sampleRate));
    auto m = kc::analyzeKick (src->mono.data(), (int) src->mono.size(), src->sampleRate, from, &start);
    if (! m.valid)
    {
        error = "No clear kick found there.";
        return false;
    }

    auto updated = std::make_shared<SourceAudio> (*src);
    updated->startSample = start;
    {
        const juce::SpinLock::ScopedLockType lock (sourceLock);
        source = updated;
    }
    setModel (m);
    return true;
}

//==============================================================================
bool KickCloneProcessor::exportWav (const juce::File& file, juce::String& error) const
{
    constexpr int sr = 48000;
    const auto m = getModel();
    auto data = kc::renderOneShot (m, getCurrentParams(), (double) sr, 36, 1.0f);
    const float gain = juce::Decibels::decibelsToGain (pLevel->load());

    const uint32_t dataBytes = (uint32_t) data.size() * 3u;
    std::vector<uint8_t> b;
    b.reserve (44 + dataBytes);
    putTag (b, "RIFF"); putU32 (b, 36u + dataBytes); putTag (b, "WAVE");
    putTag (b, "fmt "); putU32 (b, 16u); putU16 (b, 1); putU16 (b, 1);
    putU32 (b, (uint32_t) sr); putU32 (b, (uint32_t) sr * 3u); putU16 (b, 3); putU16 (b, 24);
    putTag (b, "data"); putU32 (b, dataBytes);
    for (float s : data)
    {
        const int32_t q = (int32_t) std::lround (juce::jlimit (-1.0f, 1.0f, s * gain) * 8388607.0f);
        b.push_back ((uint8_t) (q & 0xff));
        b.push_back ((uint8_t) ((q >> 8) & 0xff));
        b.push_back ((uint8_t) ((q >> 16) & 0xff));
    }

    if (! file.replaceWithData (b.data(), b.size()))
    {
        error = "Could not write " + file.getFullPathName();
        return false;
    }
    return true;
}

//==============================================================================
juce::ValueTree KickCloneProcessor::modelToTree() const
{
    const auto m = getModel();
    juce::ValueTree t ("KICKMODEL");
    t.setProperty ("pitch", envToString (m.pitch), nullptr);
    t.setProperty ("amp", envToString (m.amp), nullptr);
    t.setProperty ("body", m.bodyGain, nullptr);
    t.setProperty ("click", m.clickLevel, nullptr);
    t.setProperty ("clickDecay", m.clickDecayMs, nullptr);
    t.setProperty ("noise", m.noiseLevel, nullptr);
    t.setProperty ("noiseDecay", m.noiseDecayMs, nullptr);
    t.setProperty ("length", m.lengthMs, nullptr);
    if (auto src = getSource())
    {
        t.setProperty ("source", src->file.getFullPathName(), nullptr);
        t.setProperty ("start", src->startSample, nullptr);
    }
    return t;
}

void KickCloneProcessor::modelFromTree (const juce::ValueTree& t)
{
    kc::KickModel m;
    m.pitch = envFromString (t.getProperty ("pitch").toString());
    m.amp = envFromString (t.getProperty ("amp").toString());
    m.bodyGain = juce::jlimit (0.01f, 1.0f, (float) t.getProperty ("body", 0.9f));
    m.clickLevel = juce::jlimit (0.0f, 2.0f, (float) t.getProperty ("click", 0.2f));
    m.clickDecayMs = juce::jlimit (0.2f, 20.0f, (float) t.getProperty ("clickDecay", 1.5f));
    m.noiseLevel = juce::jlimit (0.0f, 2.0f, (float) t.getProperty ("noise", 0.0f));
    m.noiseDecayMs = juce::jlimit (5.0f, 600.0f, (float) t.getProperty ("noiseDecay", 40.0f));
    m.lengthMs = juce::jlimit (20.0f, kc::kAxisMaxMs, (float) t.getProperty ("length", 400.0f));
    m.valid = sanitize (m.pitch, true) && sanitize (m.amp, false);
    setModel (m.valid ? m : kc::KickModel::makeDefault());
}

void KickCloneProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    auto old = state.getChildWithName ("KICKMODEL");
    if (old.isValid())
        state.removeChild (old, nullptr);
    state.appendChild (modelToTree(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void KickCloneProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    auto mt = tree.getChildWithName ("KICKMODEL");
    if (mt.isValid())
        tree.removeChild (mt, nullptr);
    apvts.replaceState (tree);

    if (mt.isValid())
    {
        modelFromTree (mt);
        const juce::File f (mt.getProperty ("source").toString());
        if (f.existsAsFile())
        {
            juce::String err;
            // solo para mostrar la forma de onda; se conserva el modelo guardado
            loadAudioFile (f, err, false, (int) mt.getProperty ("start", 0));
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KickCloneProcessor();
}
