#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
juce::String envToString (const kc::Envelope& e)
{
    juce::StringArray parts;
    for (int i = 0; i < e.n; ++i)
        parts.add (juce::String (e.t[i], 3) + ":" + juce::String (e.v[i], 6));
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

// WAV PCM de 24 bit, 1 o 2 canales.
bool writeWav24 (const juce::File& file, const std::vector<const float*>& channels, size_t numSamples,
                 int sampleRate, float gain, juce::String& error)
{
    const uint16_t nCh = (uint16_t) channels.size();
    if (nCh < 1 || numSamples == 0) { error = "Nothing to export."; return false; }

    const uint32_t dataBytes = (uint32_t) (numSamples * nCh * 3u);
    std::vector<uint8_t> b;
    b.reserve (44 + dataBytes);
    putTag (b, "RIFF"); putU32 (b, 36u + dataBytes); putTag (b, "WAVE");
    putTag (b, "fmt "); putU32 (b, 16u); putU16 (b, 1); putU16 (b, nCh);
    putU32 (b, (uint32_t) sampleRate); putU32 (b, (uint32_t) sampleRate * nCh * 3u); putU16 (b, (uint16_t) (nCh * 3)); putU16 (b, 24);
    putTag (b, "data"); putU32 (b, dataBytes);
    for (size_t i = 0; i < numSamples; ++i)
        for (uint16_t c = 0; c < nCh; ++c)
        {
            const int32_t q = (int32_t) std::lround (juce::jlimit (-1.0f, 1.0f, channels[c][i] * gain) * 8388607.0f);
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

    auto pct = [&] (const char* id, const char* name, float def, float maxV)
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name,
                    NormalisableRange<float> (0.0f, maxV, 0.1f), def, attrs ("%", 0, false)));
    };
    auto scale = [&] (const char* id, const char* name)
    {
        NormalisableRange<float> r (25.0f, 400.0f, 0.1f);
        r.setSkewForCentre (100.0f);
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, 100.0f, attrs ("%", 0, false)));
    };

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "tune", 1 }, "Tune",
                NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f, attrs ("st", 2, true)));
    pct ("pitchDrop", "Pitch Drop", 100.0f, 200.0f);
    scale ("decay", "Decay");
    pct ("harmonics", "Harmonics", 0.0f, 100.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "drive", 1 }, "Drive",
                NormalisableRange<float> (0.0f, 24.0f, 0.1f), 0.0f, attrs ("dB", 1, false)));

    pct ("click", "Click", 100.0f, 200.0f);
    scale ("clickDecay", "Click Decay");

    pct ("noise", "Noise", 100.0f, 200.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "noiseTone", 1 }, "Noise Tone",
                NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, attrs ("st", 1, true)));
    scale ("noiseDecay", "Noise Decay");

    pct ("width", "Width", 60.0f, 140.0f);
    pct ("dynamics", "Dynamics", 0.0f, 100.0f);
    pct ("keytrack", "Key Track", 0.0f, 100.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "level", 1 }, "Level",
                NormalisableRange<float> (-24.0f, 6.0f, 0.1f), 0.0f, attrs ("dB", 1, true)));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "voices", 1 }, "Voices",
                StringArray { "Mono", "Poly" }, 0));

    return layout;
}

KickCloneProcessor::KickCloneProcessor()
    : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createLayout())
{
    pTune       = apvts.getRawParameterValue ("tune");
    pPitch      = apvts.getRawParameterValue ("pitchDrop");
    pDecay      = apvts.getRawParameterValue ("decay");
    pHarm       = apvts.getRawParameterValue ("harmonics");
    pDrive      = apvts.getRawParameterValue ("drive");
    pClick      = apvts.getRawParameterValue ("click");
    pClickDecay = apvts.getRawParameterValue ("clickDecay");
    pNoise      = apvts.getRawParameterValue ("noise");
    pNoiseTone  = apvts.getRawParameterValue ("noiseTone");
    pNoiseDecay = apvts.getRawParameterValue ("noiseDecay");
    pWidth      = apvts.getRawParameterValue ("width");
    pDyn        = apvts.getRawParameterValue ("dynamics");
    pKey        = apvts.getRawParameterValue ("keytrack");
    pLevel      = apvts.getRawParameterValue ("level");
    pVoices     = apvts.getRawParameterValue ("voices");

    model = kc::KickModel::makeDefault();
    audioModel = model;
}

//==============================================================================
void KickCloneProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    engine.prepare (sampleRate);
    post.prepare (sampleRate);
    levelSmoothed.reset (sampleRate, 0.02);
    levelSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pLevel->load()));
    scratchR.assign ((size_t) juce::jmax (samplesPerBlock, 64), 0.0f);
    audioModelVersion = -1;
    isoPlaying = false;
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
    p.harmonics = pHarm->load() * 0.01f;
    p.driveDb = pDrive->load();
    p.clickGain = pClick->load() * 0.01f;
    p.clickDecayScale = pClickDecay->load() * 0.01f;
    p.noiseGain = pNoise->load() * 0.01f;
    p.noiseToneSt = pNoiseTone->load();
    p.noiseDecayScale = pNoiseDecay->load() * 0.01f;
    p.width = pWidth->load() * 0.01f;
    p.dynamics = pDyn->load() * 0.01f;
    p.keytrack = pKey->load() * 0.01f;
    p.poly = pVoices->load() > 0.5f;
    return p;
}

void KickCloneProcessor::mixIsoPreview (float* l, float* r, int n)
{
    if (! isoPlaying) return;
    const juce::SpinLock::ScopedTryLockType lock (isoLock);
    if (! lock.isLocked()) return;   // el editor esta publicando un audio nuevo: este bloque no suena

    const int size = (int) isoBuf.size();
    const double ratio = isoSr / juce::jmax (1.0, currentSampleRate);
    for (int i = 0; i < n; ++i)
    {
        const int idx = (int) isoPos;
        if (idx + 1 >= size) { isoPlaying = false; return; }
        const float frac = (float) (isoPos - (double) idx);
        const float s = isoBuf[(size_t) idx] + (isoBuf[(size_t) idx + 1] - isoBuf[(size_t) idx]) * frac;
        l[i] += s;
        r[i] += s;
        isoPos += ratio;
    }
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

    float* L = buffer.getWritePointer (0);
    float* R = nullptr;
    if (numChannels > 1)
        R = buffer.getWritePointer (1);
    else
    {
        if ((int) scratchR.size() < numSamples) scratchR.resize ((size_t) numSamples);   // solo en hosts que exceden el bloque anunciado
        std::fill (scratchR.begin(), scratchR.begin() + numSamples, 0.0f);
        R = scratchR.data();
    }

    int pos = 0;

    if (previewRequested.exchange (false))
        engine.noteOn (audioModel, params, 36, 1.0f);
    if (isoRequested.exchange (false))
    {
        isoPlaying = true;
        isoPos = 0.0;
    }

    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();
        const int sp = juce::jlimit (0, numSamples - 1, metadata.samplePosition);
        if (sp > pos)
        {
            engine.render (L + pos, R + pos, sp - pos);
            pos = sp;
        }

        if (msg.isNoteOn())
            engine.noteOn (audioModel, params, msg.getNoteNumber(), msg.getFloatVelocity());
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            engine.reset();
    }

    if (pos < numSamples)
        engine.render (L + pos, R + pos, numSamples - pos);

    mixIsoPreview (L, R, numSamples);

    post.setAmount (pDyn->load() * 0.01f);
    post.process (L, R, numSamples);

    levelSmoothed.setTargetValue (juce::Decibels::decibelsToGain (pLevel->load()));
    for (int i = 0; i < numSamples; ++i)
    {
        const float g = levelSmoothed.getNextValue();
        L[i] *= g;
        R[i] *= g;
    }

    if (numChannels == 1)
        for (int i = 0; i < numSamples; ++i)
            L[i] = 0.5f * (L[i] + R[i]);

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

void KickCloneProcessor::publishSource (std::shared_ptr<SourceAudio> src)
{
    // Audio de audicion del aislado (se intercambia bajo el spin lock; el hilo de audio solo hace try-lock)
    std::vector<float> iso = src->isolated;
    {
        const juce::SpinLock::ScopedLockType lock (isoLock);
        isoBuf.swap (iso);
        isoSr = src->sampleRate;
    }
    {
        const juce::SpinLock::ScopedLockType lock (sourceLock);
        source = std::move (src);
    }
}

bool KickCloneProcessor::applyAnalysis (std::shared_ptr<SourceAudio> src, kc::Analysis&& a, bool replaceModel,
                                        bool publishOnFail, juce::String& error)
{
    if (replaceModel && ! a.model.valid)
    {
        error = "No clear kick found there. Click on one of the markers in the waveform.";
        if (publishOnFail)
        {
            src->isolated.clear();
            publishSource (src);
            sendChangeMessage();
        }
        return false;
    }

    src->startSample = a.start;
    src->isolated = std::move (a.isolated);
    publishSource (src);

    if (replaceModel)
        setModel (a.model);
    else
        sendChangeMessage();
    return true;
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

    auto mono = std::make_shared<std::vector<float>> ((size_t) numSamples, 0.0f);
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* d = buf.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
            (*mono)[(size_t) i] += d[i] / (float) numCh;
    }

    auto src = std::make_shared<SourceAudio>();
    src->sampleRate = sr;
    src->file = file;
    src->mono = mono;
    src->events = kc::detectEvents (mono->data(), numSamples, sr);

    kc::Analysis a = analyse ? kc::analyzeKick (mono->data(), numSamples, sr, 0, &src->events)
                             : kc::analyzeKickAt (mono->data(), numSamples, sr, juce::jlimit (0, numSamples - 1, startHint), &src->events);

    if (! analyse)
        a.start = juce::jlimit (0, numSamples - 1, startHint);

    return applyAnalysis (src, std::move (a), analyse, true, error);
}

bool KickCloneProcessor::selectAt (int sample, juce::String& error)
{
    auto cur = getSource();
    if (cur == nullptr || cur->numSamples() == 0)
    {
        error = "Load an audio file first.";
        return false;
    }

    auto src = std::make_shared<SourceAudio> (*cur);
    const float* x = src->mono->data();
    const int n = src->numSamples();
    const double sr = src->sampleRate;

    int start = -1;
    int bestDist = (int) (0.04 * sr);
    for (const auto& e : src->events)
        if (std::abs (e.sample - sample) < bestDist) { bestDist = std::abs (e.sample - sample); start = e.sample; }

    if (start < 0)
        start = kc::refineStart (x, n, sample, (int) (0.02 * sr), sr);

    return applyAnalysis (src, kc::alignToBody (x, n, sr, start, &src->events), true, false, error);
}

bool KickCloneProcessor::nudgeStart (int delta, juce::String& error)
{
    auto cur = getSource();
    if (cur == nullptr || cur->numSamples() == 0)
    {
        error = "Load an audio file first.";
        return false;
    }

    auto src = std::make_shared<SourceAudio> (*cur);
    const int start = juce::jlimit (0, src->numSamples() - 1, src->startSample + delta);
    return applyAnalysis (src, kc::analyzeKickAt (src->mono->data(), src->numSamples(), src->sampleRate, start, &src->events), true, false, error);
}

//==============================================================================
bool KickCloneProcessor::exportWav (const juce::File& file, juce::String& error) const
{
    constexpr int sr = 48000;
    const auto buf = kc::renderOneShot (getModel(), getCurrentParams(), (double) sr, 36, 1.0f);
    const float gain = juce::Decibels::decibelsToGain (pLevel->load());
    return writeWav24 (file, { buf.l.data(), buf.r.data() }, buf.l.size(), sr, gain, error);
}

bool KickCloneProcessor::exportIsolatedWav (const juce::File& file, juce::String& error) const
{
    auto src = getSource();
    if (src == nullptr || src->isolated.empty())
    {
        error = "There is no isolated kick to export yet.";
        return false;
    }
    return writeWav24 (file, { src->isolated.data() }, src->isolated.size(), (int) std::lround (src->sampleRate), 1.0f, error);
}

//==============================================================================
juce::ValueTree KickCloneProcessor::modelToTree() const
{
    const auto m = getModel();
    juce::ValueTree t ("KICKMODEL");
    t.setProperty ("version", 2, nullptr);
    t.setProperty ("pitch", envToString (m.pitch), nullptr);
    t.setProperty ("amp", envToString (m.amp), nullptr);
    t.setProperty ("noiseEnv", envToString (m.noise), nullptr);
    t.setProperty ("body", m.bodyGain, nullptr);
    t.setProperty ("click", m.clickLevel, nullptr);
    t.setProperty ("clickDecay", m.clickDecayMs, nullptr);
    t.setProperty ("clickHz", m.clickHz, nullptr);
    t.setProperty ("noiseHz", m.noiseHz, nullptr);
    t.setProperty ("noiseQ", m.noiseQ, nullptr);
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
    const auto def = kc::KickModel::makeDefault();
    const int version = (int) t.getProperty ("version", 1);

    kc::KickModel m;
    m.pitch = envFromString (t.getProperty ("pitch").toString());
    m.amp = envFromString (t.getProperty ("amp").toString());
    m.bodyGain = juce::jlimit (0.01f, 2.0f, (float) t.getProperty ("body", 0.9f));
    m.clickLevel = juce::jlimit (0.0f, 2.0f, (float) t.getProperty ("click", 0.2f));
    m.clickDecayMs = juce::jlimit (0.2f, 20.0f, (float) t.getProperty ("clickDecay", 1.5f));
    m.lengthMs = juce::jlimit (20.0f, kc::kAxisMaxMs, (float) t.getProperty ("length", 400.0f));

    if (version >= 2)
    {
        m.clickHz = juce::jlimit (100.0f, 20000.0f, (float) t.getProperty ("clickHz", def.clickHz));
        m.noiseHz = juce::jlimit (100.0f, 20000.0f, (float) t.getProperty ("noiseHz", def.noiseHz));
        m.noiseQ = juce::jlimit (0.25f, 12.0f, (float) t.getProperty ("noiseQ", def.noiseQ));
        m.noise = envFromString (t.getProperty ("noiseEnv").toString());
        if (! sanitize (m.noise, false)) m.noise = def.noise;
    }
    else
    {
        // Estado de la v1: el click era relativo al cuerpo y no existia la envolvente de ruido
        m.clickLevel = juce::jlimit (0.0f, 2.0f, m.clickLevel * m.bodyGain);
        m.clickHz = def.clickHz;
        m.noiseHz = def.noiseHz;
        m.noiseQ = def.noiseQ;
        m.noise = def.noise;
    }

    m.valid = sanitize (m.pitch, true) && sanitize (m.amp, false);
    setModel (m.valid ? m : def);
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
            // solo para mostrar el audio y el aislado; se conserva el modelo guardado
            juce::String err;
            loadAudioFile (f, err, false, (int) mt.getProperty ("start", 0));
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KickCloneProcessor();
}
