#include "PluginEditor.h"

//==============================================================================
// Estetica: negro puro, tipografia fina, lineas de 1 px, rojo solo como acento.
namespace col
{
const juce::Colour bg   (0xff000000);
const juce::Colour text (0xffd0d0d0);
const juce::Colour dim  (0xff7c7c7c);
const juce::Colour grid (0xff1c1c1c);
const juce::Colour line (0xff353535);
const juce::Colour white(0xffffffff);
const juce::Colour red  (0xffe5161c);
}

static juce::Font uiFont (float height, bool light = true)
{
   #if JUCE_WINDOWS
    return juce::Font (juce::FontOptions ("Segoe UI", light ? "Light" : "Regular", height));
   #elif JUCE_MAC
    return juce::Font (juce::FontOptions ("Helvetica Neue", light ? "Light" : "Regular", height));
   #else
    juce::ignoreUnused (light);
    return juce::Font (juce::FontOptions (height));
   #endif
}

static float textWidth (const juce::Font& f, const juce::String& s)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText (f, s, 0.0f, 0.0f);
    return ga.getBoundingBox (0, -1, false).getWidth();
}

static bool isAudioFile (const juce::String& path)
{
    const auto ext = juce::File (path).getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".mp3" || ext == ".ogg";
}

//==============================================================================
class MinimalLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MinimalLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, col::bg);
        setColour (juce::Slider::thumbColourId, col::text);
    }

    int getSliderThumbRadius (juce::Slider&) override { return 4; }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float, float, juce::Slider::SliderStyle, juce::Slider& s) override
    {
        const float cy = (float) y + (float) height * 0.5f;
        g.setColour (col::line);
        g.fillRect ((float) x, cy - 0.5f, (float) width, 1.0f);

        const float origin = (float) x + (float) s.valueToProportionOfLength (s.getDoubleClickReturnValue()) * (float) width;
        g.setColour (col::text);
        g.fillRect (juce::jmin (origin, sliderPos), cy - 0.5f, std::abs (sliderPos - origin), 1.0f);
        g.fillEllipse (sliderPos - 3.5f, cy - 3.5f, 7.0f, 7.0f);
    }
};

//==============================================================================
// Texto subrayado clicable (como "Select an audio file" de la referencia).
class LinkText : public juce::Component
{
public:
    LinkText (const juce::String& t, float h, bool rightAligned = false, bool underlined = true)
        : text (t), height (h), right (rightAligned), underline (underlined)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    std::function<void()> onClick;

    void setText (const juce::String& t) { text = t; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto f = uiFont (height);
        g.setFont (f);
        g.setColour (hover ? col::white : col::text);
        const auto b = getLocalBounds();
        g.drawText (text, b, right ? juce::Justification::centredRight : juce::Justification::centredLeft, false);
        if (underline)
        {
            const float w = textWidth (f, text);
            const float x0 = right ? (float) b.getRight() - w : (float) b.getX();
            const float y = (float) b.getCentreY() + height * 0.5f + 1.0f;
            g.fillRect (x0, y, w, 1.0f);
        }
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked() && getLocalBounds().contains (e.getPosition()) && onClick)
            onClick();
    }

private:
    juce::String text;
    float height;
    bool right, underline, hover = false;
};

//==============================================================================
class LabeledSlider : public juce::Component
{
public:
    LabeledSlider (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& caption_)
        : caption (caption_), param (apvts.getParameter (paramId))
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        addAndMakeVisible (slider);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, paramId, slider);
        if (param != nullptr)
            slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
        slider.onValueChange = [this] { repaint(); };
    }

    void resized() override { slider.setBounds (getLocalBounds().withTrimmedTop (20)); }

    void paint (juce::Graphics& g) override
    {
        g.setFont (uiFont (12.0f));
        g.setColour (col::dim);
        g.drawText (caption, 0, 0, getWidth() / 2 + 10, 18, juce::Justification::centredLeft, false);
        g.setColour (col::text);
        if (param != nullptr)
            g.drawText (param->getCurrentValueAsText(), getWidth() / 2 - 10, 0, getWidth() / 2 + 10, 18,
                        juce::Justification::centredRight, false);
    }

private:
    juce::String caption;
    juce::RangedAudioParameter* param = nullptr;
    juce::Slider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

//==============================================================================
// Editor de envolvente multisegmento: arrastrar puntos, doble clic para anadir/quitar.
class EnvelopeEditor : public juce::Component
{
public:
    enum class Kind { pitch, amp };

    EnvelopeEditor (KickCloneProcessor& p, Kind k) : proc (p), kind (k) { refresh(); }

    void refresh()
    {
        if (dragIndex >= 0) return;   // no pisar la edicion en curso
        const auto m = proc.getModel();
        env = kind == Kind::pitch ? m.pitch : m.amp;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = plot();
        g.setFont (uiFont (12.0f));
        g.setColour (col::dim);
        g.drawText (kind == Kind::pitch ? "pitch" : "amplitude", 0, 0, 120, 16, juce::Justification::centredLeft, false);

        g.setFont (uiFont (10.5f));
        // rejilla horizontal
        if (kind == Kind::pitch)
        {
            struct L { float hz; const char* s; };
            for (auto l : { L { 20, "20" }, L { 50, "50" }, L { 100, "100" }, L { 200, "200" }, L { 500, "500" }, L { 1000, "1k" } })
            {
                const float y = vToY (l.hz);
                g.setColour (col::grid); g.fillRect (r.getX(), y, r.getWidth(), 1.0f);
                g.setColour (col::dim);  g.drawText (l.s, 0, (int) y - 7, (int) r.getX() - 4, 14, juce::Justification::centredRight, false);
            }
        }
        else
        {
            for (int db : { 0, -12, -24, -36, -48, -60 })
            {
                const float y = vToY (db <= -60 ? 0.0f : std::pow (10.0f, (float) db / 20.0f));
                g.setColour (col::grid); g.fillRect (r.getX(), y, r.getWidth(), 1.0f);
                g.setColour (col::dim);  g.drawText (juce::String (db), 0, (int) y - 7, (int) r.getX() - 4, 14, juce::Justification::centredRight, false);
            }
        }

        // rejilla vertical (tiempo)
        for (float t : { 10.0f, 30.0f, 100.0f, 300.0f, 1000.0f })
        {
            const float x = tToX (t);
            g.setColour (col::grid); g.fillRect (x, r.getY(), 1.0f, r.getHeight());
            g.setColour (col::dim);
            const bool last = t >= 1000.0f;
            g.drawText (juce::String ((int) t) + (last ? " ms" : ""),
                        last ? getWidth() - 60 : (int) x - 30, (int) r.getBottom() + 2, 60, 14,
                        last ? juce::Justification::centredRight : juce::Justification::centred, false);
        }

        g.setColour (col::line);
        g.drawRect (r, 1.0f);

        if (env.n < 1) return;

        // curva (misma funcion que usa el motor)
        juce::Path path;
        bool started = false;
        for (float x = r.getX(); x <= r.getRight(); x += 1.0f)
        {
            const float t = xToT (x);
            const float v = kind == Kind::pitch ? kc::evalPitchHz (env, t) : kc::evalAmp (env, t);
            const float y = vToY (v);
            if (! started) { path.startNewSubPath (x, y); started = true; }
            else path.lineTo (x, y);
        }
        g.setColour (col::white);
        g.strokePath (path, juce::PathStrokeType (1.3f));

        for (int i = 0; i < env.n; ++i)
        {
            const bool active = (i == hoverIndex || i == dragIndex);
            const float cx = tToX (env.t[i]), cy = vToY (env.v[i]);
            const float rad = active ? 5.0f : 3.5f;
            g.setColour (active ? col::red : col::white);
            g.fillEllipse (cx - rad, cy - rad, rad * 2.0f, rad * 2.0f);
        }

        if (hoverIndex >= 0 || dragIndex >= 0)
        {
            const int i = dragIndex >= 0 ? dragIndex : hoverIndex;
            const auto label = juce::String (env.t[i], 1) + " ms  "
                             + (kind == Kind::pitch ? juce::String (env.v[i], 1) + " Hz"
                                                    : (env.v[i] <= 0.001f ? juce::String ("-inf dB")
                                                                          : juce::String (20.0f * std::log10 (env.v[i]), 1) + " dB"));
            g.setFont (uiFont (11.0f));
            g.setColour (col::text);
            g.drawText (label, getWidth() - 170, 0, 170, 16, juce::Justification::centredRight, false);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h != hoverIndex) { hoverIndex = h; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverIndex != -1) { hoverIndex = -1; repaint(); }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragIndex = hit (e.position);
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragIndex < 0 || dragIndex >= env.n) return;
        float t = xToT (e.position.x);
        const float v = yToV (e.position.y);
        if (dragIndex == 0)
            t = 0.0f;
        else
        {
            const float lo = env.t[dragIndex - 1] + 0.5f;
            const float hi = dragIndex == env.n - 1 ? kc::kAxisMaxMs : env.t[dragIndex + 1] - 0.5f;
            t = juce::jlimit (lo, juce::jmax (lo, hi), t);
        }
        env.t[dragIndex] = t;
        env.v[dragIndex] = v;
        commit();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragIndex = -1;
        refresh();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h >= 0)
        {
            if (h > 0 && env.n > 2) { env.removeAt (h); hoverIndex = -1; commit(); }
        }
        else if (plot().contains (e.position))
        {
            const float t = xToT (e.position.x);
            if (t < 0.5f) return;
            const float v = kind == Kind::pitch ? kc::evalPitchHz (env, t) : kc::evalAmp (env, t);
            if (env.insertSorted (t, v) >= 0) commit();
        }
    }

private:
    static constexpr float kTimeExp = 0.4f;

    juce::Rectangle<float> plot() const
    {
        return getLocalBounds().toFloat().withTrimmedLeft (34.0f).withTrimmedBottom (18.0f)
                                         .withTrimmedTop (20.0f).withTrimmedRight (8.0f);
    }

    float tToX (float t) const
    {
        const auto r = plot();
        return r.getX() + std::pow (juce::jlimit (0.0f, 1.0f, t / kc::kAxisMaxMs), kTimeExp) * r.getWidth();
    }

    float xToT (float x) const
    {
        const auto r = plot();
        const float a = juce::jlimit (0.0f, 1.0f, (x - r.getX()) / juce::jmax (1.0f, r.getWidth()));
        return std::pow (a, 1.0f / kTimeExp) * kc::kAxisMaxMs;
    }

    float vToY (float v) const
    {
        const auto r = plot();
        float a;
        if (kind == Kind::pitch)
            a = std::log (juce::jlimit (kc::kMinHz, kc::kMaxHz, v) / kc::kMinHz) / std::log (kc::kMaxHz / kc::kMinHz);
        else
        {
            const float db = v <= 1.0e-3f ? -60.0f : 20.0f * std::log10 (juce::jmin (v, 1.0f));
            a = juce::jlimit (0.0f, 1.0f, 1.0f + db / 60.0f);
        }
        return r.getBottom() - a * r.getHeight();
    }

    float yToV (float y) const
    {
        const auto r = plot();
        const float a = juce::jlimit (0.0f, 1.0f, (r.getBottom() - y) / juce::jmax (1.0f, r.getHeight()));
        if (kind == Kind::pitch)
            return kc::kMinHz * std::pow (kc::kMaxHz / kc::kMinHz, a);
        if (a < 0.01f) return 0.0f;
        return juce::jmin (1.0f, std::pow (10.0f, (a - 1.0f) * 60.0f / 20.0f));
    }

    int hit (juce::Point<float> p) const
    {
        int best = -1;
        float bestD = 10.0f;
        for (int i = 0; i < env.n; ++i)
        {
            const float d = p.getDistanceFrom ({ tToX (env.t[i]), vToY (env.v[i]) });
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }

    void commit()
    {
        auto m = proc.getModel();
        (kind == Kind::pitch ? m.pitch : m.amp) = env;
        proc.setModel (m);
        repaint();
    }

    KickCloneProcessor& proc;
    Kind kind;
    kc::Envelope env;
    int hoverIndex = -1, dragIndex = -1;
};

//==============================================================================
// Forma de onda del audio cargado (arriba, clic para elegir otro golpe) y
// comparacion golpe original / clon (abajo).
class WaveformView : public juce::Component, private juce::Timer
{
public:
    explicit WaveformView (KickCloneProcessor& p) : proc (p)
    {
        refresh();
        startTimerHz (10);
    }

    std::function<void (const juce::String&)> onStatus;

    void refresh()
    {
        src = proc.getSource();
        model = proc.getModel();
        lastVersion = proc.getModelVersion();
        lastParams = proc.getCurrentParams();
        rebuild();
        repaint();
    }

    void resized() override { rebuild(); }

    void paint (juce::Graphics& g) override
    {
        const auto all = getLocalBounds().toFloat();
        g.setColour (col::line);
        g.drawRect (all, 1.0f);

        if (src == nullptr || src->mono.empty())
        {
            g.setFont (uiFont (14.0f));
            g.setColour (col::dim);
            g.drawText ("Load an audio file by dragging or using the file selector", getLocalBounds(),
                        juce::Justification::centred, false);
            return;
        }

        const auto top = topArea();
        const float n = (float) src->mono.size();

        // forma de onda completa
        g.setColour (col::dim);
        drawPeaks (g, fullPeaks, top, fullScale);

        // region analizada
        const float x0 = top.getX() + (float) src->startSample / n * top.getWidth();
        const float regionLen = (float) clone.size();
        const float x1 = juce::jmin (top.getRight(), x0 + regionLen / n * top.getWidth());
        g.setColour (col::white.withAlpha (0.10f));
        g.fillRect (x0, top.getY(), juce::jmax (2.0f, x1 - x0), top.getHeight());
        g.setColour (col::red);
        g.fillRect (x0, top.getY(), 1.0f, top.getHeight());

        // separador y zoom
        const auto zoom = zoomArea();
        g.setColour (col::line);
        g.fillRect (zoom.getX(), zoom.getY(), zoom.getWidth(), 1.0f);

        g.setColour (col::dim);
        drawPeaks (g, origPeaks, zoom, zoomScale);
        g.setColour (col::red);
        drawPeaks (g, clonePeaks, zoom, zoomScale);

        g.setFont (uiFont (10.5f));
        g.setColour (col::dim);
        g.drawText ("original", (int) zoom.getX() + 6, (int) zoom.getY() + 3, 60, 12, juce::Justification::centredLeft, false);
        g.setColour (col::red);
        g.drawText ("clone", (int) zoom.getX() + 62, (int) zoom.getY() + 3, 60, 12, juce::Justification::centredLeft, false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (src == nullptr || src->mono.empty()) return;
        const auto top = topArea();
        if (! top.contains (e.position)) return;
        const float a = juce::jlimit (0.0f, 1.0f, (e.position.x - top.getX()) / juce::jmax (1.0f, top.getWidth()));
        juce::String err;
        if (! proc.reanalyzeFrom ((int) (a * (float) src->mono.size()), err) && onStatus)
            onStatus (err);
        else if (onStatus)
            onStatus ({});
    }

private:
    using Peaks = std::vector<std::pair<float, float>>;

    juce::Rectangle<float> topArea() const
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        return r.removeFromTop (std::floor (r.getHeight() * 0.45f));
    }

    juce::Rectangle<float> zoomArea() const
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        r.removeFromTop (std::floor (r.getHeight() * 0.45f));
        return r;
    }

    // Maximos/minimos por pixel de 'data' repartidos sobre 'totalLen' muestras.
    static Peaks computePeaks (const float* data, int64_t dataSize, int64_t totalLen, int width)
    {
        Peaks out ((size_t) juce::jmax (width, 0), { 0.0f, 0.0f });
        if (width <= 0 || totalLen <= 0 || data == nullptr) return out;
        for (int px = 0; px < width; ++px)
        {
            const int64_t a = totalLen * px / width;
            const int64_t b = juce::jmin (dataSize, juce::jmax (a + 1, totalLen * (px + 1) / width));
            if (a >= dataSize) break;
            const int64_t step = juce::jmax<int64_t> (1, (b - a) / 64);
            float mn = 1.0e9f, mx = -1.0e9f;
            for (int64_t i = a; i < b; i += step)
            {
                mn = juce::jmin (mn, data[i]);
                mx = juce::jmax (mx, data[i]);
            }
            out[(size_t) px] = { mn > mx ? 0.0f : mn, mx < mn ? 0.0f : mx };
        }
        return out;
    }

    static float maxAbs (const Peaks& p)
    {
        float m = 0.0f;
        for (auto& q : p) m = juce::jmax (m, std::abs (q.first), std::abs (q.second));
        return m;
    }

    static void drawPeaks (juce::Graphics& g, const Peaks& p, juce::Rectangle<float> area, float scale)
    {
        const float cy = area.getCentreY();
        const float half = area.getHeight() * 0.46f;
        for (size_t i = 0; i < p.size(); ++i)
        {
            const float y1 = cy - p[i].second * scale * half;
            const float y2 = cy - p[i].first * scale * half;
            g.fillRect (area.getX() + (float) i, juce::jmin (y1, y2), 1.0f, juce::jmax (1.0f, std::abs (y2 - y1)));
        }
    }

    void rebuild()
    {
        if (src == nullptr || src->mono.empty() || getWidth() < 4)
        {
            fullPeaks.clear(); origPeaks.clear(); clonePeaks.clear(); clone.clear();
            return;
        }
        fullPeaks = computePeaks (src->mono.data(), (int64_t) src->mono.size(), (int64_t) src->mono.size(),
                                  (int) topArea().getWidth());
        fullScale = 1.0f / juce::jmax (1.0e-4f, maxAbs (fullPeaks));
        renderClone();
    }

    void renderClone()
    {
        if (src == nullptr || ! model.valid || getWidth() < 4) return;
        clone = kc::renderOneShot (model, lastParams, src->sampleRate, 36, 1.0f);
        const int w = (int) zoomArea().getWidth();
        const int64_t len = (int64_t) clone.size();
        const int64_t avail = juce::jmax<int64_t> (0, (int64_t) src->mono.size() - src->startSample);
        origPeaks = computePeaks (src->mono.data() + src->startSample, avail, len, w);
        clonePeaks = computePeaks (clone.data(), len, len, w);
        zoomScale = 1.0f / juce::jmax (1.0e-4f, juce::jmax (maxAbs (origPeaks), maxAbs (clonePeaks)));
    }

    static bool sameParams (const kc::KickParams& a, const kc::KickParams& b)
    {
        using juce::exactlyEqual;
        return exactlyEqual (a.tuneSemitones, b.tuneSemitones) && exactlyEqual (a.keytrack, b.keytrack)
            && exactlyEqual (a.pitchAmount, b.pitchAmount) && exactlyEqual (a.decayScale, b.decayScale)
            && exactlyEqual (a.clickGain, b.clickGain) && exactlyEqual (a.noiseGain, b.noiseGain)
            && exactlyEqual (a.driveDb, b.driveDb);
    }

    void timerCallback() override
    {
        const auto params = proc.getCurrentParams();
        const int ver = proc.getModelVersion();
        if (ver != lastVersion || ! sameParams (params, lastParams))
        {
            lastVersion = ver;
            lastParams = params;
            model = proc.getModel();
            renderClone();
            repaint();
        }
    }

    KickCloneProcessor& proc;
    std::shared_ptr<const SourceAudio> src;
    kc::KickModel model;
    kc::KickParams lastParams;
    int lastVersion = -1;
    std::vector<float> clone;
    Peaks fullPeaks, origPeaks, clonePeaks;
    float fullScale = 1.0f, zoomScale = 1.0f;
};

//==============================================================================
KickCloneEditor::KickCloneEditor (KickCloneProcessor& p)
    : juce::AudioProcessorEditor (&p), proc (p)
{
    lnf = std::make_unique<MinimalLookAndFeel>();
    setLookAndFeel (lnf.get());

    selectLink = std::make_unique<LinkText> ("Select an audio file", 14.0f);
    selectLink->onClick = [this] { chooseFile(); };
    addAndMakeVisible (*selectLink);

    playLink = std::make_unique<LinkText> ("play", 13.0f, true);
    playLink->onClick = [this] { proc.triggerPreview(); };
    addAndMakeVisible (*playLink);

    exportLink = std::make_unique<LinkText> ("export .wav", 13.0f, true);
    exportLink->onClick = [this] { exportFile(); };
    addAndMakeVisible (*exportLink);

    inputToggle = std::make_unique<LinkText> (juce::String::fromUTF8 ("\xe2\x96\xbe  input"), 12.0f, false, false);
    inputToggle->onClick = [this]
    {
        inputOpen = ! inputOpen;
        inputToggle->setText (juce::String::fromUTF8 (inputOpen ? "\xe2\x96\xbe  input" : "\xe2\x96\xb8  input"));
        resized();
        repaint();
    };
    addAndMakeVisible (*inputToggle);

    pitchEditor = std::make_unique<EnvelopeEditor> (proc, EnvelopeEditor::Kind::pitch);
    ampEditor = std::make_unique<EnvelopeEditor> (proc, EnvelopeEditor::Kind::amp);
    addAndMakeVisible (*pitchEditor);
    addAndMakeVisible (*ampEditor);

    waves = std::make_unique<WaveformView> (proc);
    waves->onStatus = [this] (const juce::String& s) { setStatus (s); };
    addAndMakeVisible (*waves);

    struct Def { const char* id; const char* caption; };
    for (auto d : { Def { "tune", "tune" }, Def { "pitchDrop", "pitch drop" }, Def { "decay", "decay" },
                    Def { "click", "click" }, Def { "noise", "noise" }, Def { "drive", "drive" },
                    Def { "keytrack", "key track" }, Def { "level", "level" } })
    {
        sliders.push_back (std::make_unique<LabeledSlider> (proc.apvts, d.id, d.caption));
        addAndMakeVisible (*sliders.back());
    }

    proc.addChangeListener (this);

    setResizable (true, true);
    setResizeLimits (820, 520, 1600, 1000);
    setSize (980, 560);
}

KickCloneEditor::~KickCloneEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void KickCloneEditor::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);

    // Titulo
    const auto titleFont = uiFont (32.0f);
    g.setFont (titleFont);
    g.setColour (col::text);
    g.drawText ("kick clone", 16, 6, 260, 40, juce::Justification::centredLeft, false);

    // Insignia roja
    const float tw = textWidth (titleFont, "kick clone");
    const juce::Rectangle<float> badge (18.0f + tw + 2.0f, 9.0f, 30.0f, 13.0f);
    g.setColour (col::red);
    g.fillRect (badge);
    g.setColour (col::white);
    g.setFont (uiFont (10.0f, false));
    g.drawText ("v1.0", badge, juce::Justification::centred, false);

    g.setFont (uiFont (12.5f));
    g.setColour (col::dim);
    g.drawText ("kick modelling", 18, 44, 200, 16, juce::Justification::centredLeft, false);

    // Estado / nombre del archivo, junto al enlace
    const auto src = proc.getSource();
    g.setFont (uiFont (12.5f));
    const int infoX = 18 + 170;
    if (status.isNotEmpty())
    {
        g.setColour (col::red);
        g.drawText (status, infoX, 66, getWidth() - infoX - 220, 20, juce::Justification::centredLeft, false);
    }
    else if (src != nullptr && src->file != juce::File())
    {
        g.setColour (col::dim);
        g.drawText (src->file.getFileName(), infoX, 66, getWidth() - infoX - 220, 20, juce::Justification::centredLeft, true);
    }
}

void KickCloneEditor::resized()
{
    constexpr int margin = 18;
    auto r = getLocalBounds().reduced (margin, 0);

    selectLink->setBounds (margin, 66, 165, 20);
    exportLink->setBounds (getWidth() - margin - 110, 16, 110, 20);
    playLink->setBounds (getWidth() - margin - 110 - 70, 16, 60, 20);

    auto bottom = r.withTrimmedTop (0);
    // fila de controles
    const int slidersH = 40;
    auto sliderRow = bottom.removeFromBottom (slidersH + margin).withTrimmedBottom (margin);
    const int gap = 14;
    const int colW = (sliderRow.getWidth() - gap * ((int) sliders.size() - 1)) / (int) sliders.size();
    for (size_t i = 0; i < sliders.size(); ++i)
        sliders[i]->setBounds (sliderRow.getX() + (int) i * (colW + gap), sliderRow.getY(), colW, slidersH);

    bottom.removeFromBottom (10);

    // forma de onda ("input")
    const int panelH = inputOpen ? juce::jlimit (110, 190, getHeight() / 4) : 0;
    if (inputOpen)
    {
        waves->setVisible (true);
        waves->setBounds (bottom.removeFromBottom (panelH));
    }
    else
        waves->setVisible (false);

    inputToggle->setBounds (bottom.removeFromBottom (22).withWidth (90));
    bottom.removeFromBottom (4);

    // envolventes
    auto envArea = bottom.withTrimmedTop (juce::jmax (0, 102));
    envArea = juce::Rectangle<int> (margin, 102, getWidth() - margin * 2, juce::jmax (40, bottom.getBottom() - 102));
    const int half = (envArea.getWidth() - 18) / 2;
    pitchEditor->setBounds (envArea.getX(), envArea.getY(), half, envArea.getHeight());
    ampEditor->setBounds (envArea.getX() + half + 18, envArea.getY(), half, envArea.getHeight());
}

void KickCloneEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    pitchEditor->refresh();
    ampEditor->refresh();
    waves->refresh();
    repaint();
}

bool KickCloneEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (isAudioFile (f)) return true;
    return false;
}

void KickCloneEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files)
        if (isAudioFile (f)) { loadFile (juce::File (f)); return; }
}

void KickCloneEditor::setStatus (const juce::String& text)
{
    status = text;
    repaint();
}

void KickCloneEditor::loadFile (const juce::File& file)
{
    juce::String err;
    const bool ok = proc.loadAudioFile (file, err);
    setStatus (ok ? juce::String() : err);
}

void KickCloneEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Select an audio file", juce::File(),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) loadFile (f);
                          });
}

void KickCloneEditor::exportFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Export the kick as .wav",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile ("kick_clone.wav"),
                                                   "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f == juce::File()) return;
                              if (! f.hasFileExtension ("wav")) f = f.withFileExtension ("wav");
                              juce::String err;
                              setStatus (proc.exportWav (f, err) ? juce::String() : err);
                          });
}
