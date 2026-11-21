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

static juce::String msText (float ms, double sr, int decimals)
{
    return juce::String (ms, decimals) + " ms (" + juce::String ((int) std::lround ((double) ms * sr * 0.001)) + " smp)";
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
// Editor de envolvente multisegmento.
//   - arrastrar un punto lo mueve (con Mayus, 10 veces mas fino)
//   - doble clic en vacio anade un punto; doble clic sobre un punto lo quita
//   - rueda del raton: zoom del eje de tiempo (para ajustar a nivel de muestra); doble clic en el titulo lo restablece
class EnvelopeEditor : public juce::Component
{
public:
    enum class Kind { pitch, amp, noise };

    EnvelopeEditor (KickCloneProcessor& p, Kind k, const juce::String& title_) : proc (p), kind (k), title (title_) { refresh(); }

    void refresh()
    {
        if (dragIndex >= 0) return;   // no pisar la edicion en curso
        env = pick (proc.getModel());
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = plot();
        const double sr = sampleRate();

        g.setFont (uiFont (12.0f));
        g.setColour (col::dim);
        g.drawText (title, 0, 0, 150, 16, juce::Justification::centredLeft, false);
        if (viewMaxMs < kc::kAxisMaxMs - 0.5f)
        {
            g.setColour (col::red);
            g.drawText ("zoom " + juce::String (viewMaxMs, 0) + " ms", 90, 0, 80, 16, juce::Justification::centredLeft, false);
        }

        g.setFont (uiFont (10.5f));
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

        // rejilla vertical (tiempo): solo las marcas que caben en la vista
        float lastTick = 0.0f;
        for (float t : { 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f })
            if (t <= viewMaxMs * 1.001f) lastTick = t;
        for (float t : { 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f })
        {
            if (t > viewMaxMs * 1.001f) continue;
            const float x = tToX (t);
            if (x < r.getX() + 14.0f) continue;
            g.setColour (col::grid); g.fillRect (x, r.getY(), 1.0f, r.getHeight());
            g.setColour (col::dim);
            const bool last = juce::approximatelyEqual (t, lastTick);
            g.drawText (juce::String ((int) t) + (last ? " ms" : ""),
                        last ? juce::jmin ((int) x - 30, getWidth() - 62) : (int) x - 30, (int) r.getBottom() + 2, 60, 14,
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
            if (env.t[i] > viewMaxMs && i != dragIndex) continue;
            const bool active = (i == hoverIndex || i == dragIndex);
            const float cx = tToX (env.t[i]), cy = vToY (env.v[i]);
            const float rad = active ? 5.0f : 3.5f;
            g.setColour (active ? col::red : col::white);
            g.fillEllipse (cx - rad, cy - rad, rad * 2.0f, rad * 2.0f);
        }

        if (hoverIndex >= 0 || dragIndex >= 0)
        {
            const int i = dragIndex >= 0 ? dragIndex : hoverIndex;
            const auto label = msText (env.t[i], sr, 2) + "   "
                             + (kind == Kind::pitch ? juce::String (env.v[i], 1) + " Hz"
                                                    : (env.v[i] <= 0.001f ? juce::String ("-inf dB")
                                                                          : juce::String (20.0f * std::log10 (env.v[i]), 1) + " dB"));
            g.setFont (uiFont (11.0f));
            g.setColour (col::text);
            g.drawText (label, getWidth() - 260, 0, 260, 16, juce::Justification::centredRight, false);
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
        if (dragIndex >= 0)
        {
            dragPos = { tToX (env.t[dragIndex]), vToY (env.v[dragIndex]) };
            lastMouse = e.position;
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragIndex < 0 || dragIndex >= env.n) return;

        const float fine = e.mods.isShiftDown() ? 0.1f : 1.0f;   // Mayus = ajuste fino
        dragPos += (e.position - lastMouse) * fine;
        lastMouse = e.position;

        float t = xToT (dragPos.x);
        const float v = yToV (dragPos.y);
        if (dragIndex == 0)
            t = 0.0f;
        else
        {
            const float lo = env.t[dragIndex - 1] + 0.01f;
            const float hi = dragIndex == env.n - 1 ? kc::kAxisMaxMs : env.t[dragIndex + 1] - 0.01f;
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
        if (e.position.y < 18.0f && e.position.x < 180.0f)
        {
            viewMaxMs = kc::kAxisMaxMs;
            repaint();
            return;
        }

        const int h = hit (e.position);
        if (h >= 0)
        {
            if (h > 0 && env.n > 2) { env.removeAt (h); hoverIndex = -1; commit(); }
        }
        else if (plot().contains (e.position))
        {
            const float t = xToT (e.position.x);
            if (t < 0.05f) return;
            const float v = kind == Kind::pitch ? kc::evalPitchHz (env, t) : kc::evalAmp (env, t);
            if (env.insertSorted (t, v) >= 0) commit();
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (! plot().expanded (30.0f).contains (e.position)) return;
        const float f = wheel.deltaY > 0.0f ? 0.8f : 1.25f;
        viewMaxMs = juce::jlimit (4.0f, kc::kAxisMaxMs, viewMaxMs * f);
        repaint();
    }

private:
    static constexpr float kTimeExp = 0.4f;

    double sampleRate() const
    {
        if (auto s = proc.getSource()) return s->sampleRate;
        const double sr = proc.getCurrentSampleRate();
        return sr > 0.0 ? sr : 44100.0;
    }

    const kc::Envelope& pick (const kc::KickModel& m) const { return kind == Kind::pitch ? m.pitch : kind == Kind::amp ? m.amp : m.noise; }
    kc::Envelope& pick (kc::KickModel& m) const { return kind == Kind::pitch ? m.pitch : kind == Kind::amp ? m.amp : m.noise; }

    juce::Rectangle<float> plot() const
    {
        return getLocalBounds().toFloat().withTrimmedLeft (34.0f).withTrimmedBottom (18.0f)
                                         .withTrimmedTop (20.0f).withTrimmedRight (8.0f);
    }

    float tToX (float t) const
    {
        const auto r = plot();
        return r.getX() + std::pow (juce::jlimit (0.0f, 1.0f, t / viewMaxMs), kTimeExp) * r.getWidth();
    }

    float xToT (float x) const
    {
        const auto r = plot();
        const float a = juce::jlimit (0.0f, 1.0f, (x - r.getX()) / juce::jmax (1.0f, r.getWidth()));
        return std::pow (a, 1.0f / kTimeExp) * viewMaxMs;
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
            if (env.t[i] > viewMaxMs) continue;
            const float d = p.getDistanceFrom ({ tToX (env.t[i]), vToY (env.v[i]) });
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }

    void commit()
    {
        auto m = proc.getModel();
        pick (m) = env;
        proc.setModel (m);
        repaint();
    }

    KickCloneProcessor& proc;
    Kind kind;
    juce::String title;
    kc::Envelope env;
    float viewMaxMs = kc::kAxisMaxMs;
    int hoverIndex = -1, dragIndex = -1;
    juce::Point<float> dragPos, lastMouse;
};

//==============================================================================
// Arriba: audio de origen con los golpes detectados (clic = clonar ese golpe).
// Abajo: el golpe aislado (u original) frente al clon, con zoom hasta la muestra individual
// (rueda = zoom, arrastrar = desplazar, doble clic = vista completa).
class WaveformView : public juce::Component, private juce::Timer
{
public:
    explicit WaveformView (KickCloneProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (true);
        refresh();
        startTimerHz (20);
    }

    std::function<void (const juce::String&)> onStatus;
    std::function<void (int)> onNudge;      // desplazamiento en muestras

    void refresh()
    {
        const auto newSrc = proc.getSource();
        const bool changed = newSrc != src;
        src = newSrc;
        model = proc.getModel();
        lastVersion = proc.getModelVersion();
        lastParams = proc.getCurrentParams();
        rebuild();
        if (changed) resetView();
        repaint();
    }

    void resized() override { rebuild(); }

    void paint (juce::Graphics& g) override
    {
        g.setColour (col::line);
        g.drawRect (getLocalBounds().toFloat(), 1.0f);

        if (src == nullptr || src->numSamples() == 0)
        {
            g.setFont (uiFont (14.0f));
            g.setColour (col::dim);
            g.drawText ("Load an audio file by dragging or using the file selector", getLocalBounds(),
                        juce::Justification::centred, false);
            return;
        }

        const auto top = topArea();
        const double n = (double) src->numSamples();

        // forma de onda completa
        g.setColour (col::dim);
        drawPeaks (g, fullPeaks, top, fullScale);

        // golpes detectados
        for (const auto& e : src->events)
        {
            const float x = top.getX() + (float) ((double) e.sample / n) * top.getWidth();
            const bool kickLike = e.lowRatio >= 0.45f;
            g.setColour (kickLike ? col::white.withAlpha (0.55f) : col::dim.withAlpha (0.45f));
            const float h = kickLike ? top.getHeight() : top.getHeight() * 0.35f;
            g.fillRect (x, top.getBottom() - h, 1.0f, h);
        }

        // region analizada
        const float x0 = top.getX() + (float) ((double) src->startSample / n) * top.getWidth();
        const float x1 = juce::jmin (top.getRight(), x0 + (float) ((double) clone.size() / n) * top.getWidth());
        g.setColour (col::white.withAlpha (0.10f));
        g.fillRect (x0, top.getY(), juce::jmax (2.0f, x1 - x0), top.getHeight());
        g.setColour (col::red);
        g.fillRect (x0, top.getY(), 1.0f, top.getHeight());

        // separador y zoom
        const auto zoom = zoomArea();
        g.setColour (col::line);
        g.fillRect (zoom.getX(), zoom.getY(), zoom.getWidth(), 1.0f);

        if (clone.empty()) return;

        const double spp = viewLen / (double) juce::jmax (1.0f, zoom.getWidth());
        const float srD = (float) src->sampleRate;

        // marcas de tiempo
        g.setFont (uiFont (10.0f));
        const double viewMs = viewLen * 1000.0 / (double) srD;
        const double stepMs = viewMs > 400 ? 100 : viewMs > 150 ? 50 : viewMs > 60 ? 20 : viewMs > 25 ? 10 : viewMs > 10 ? 5 : viewMs > 4 ? 2 : viewMs > 1.5 ? 1 : 0.5;
        for (double t = std::ceil (viewStart * 1000.0 / srD / stepMs) * stepMs; ; t += stepMs)
        {
            const double smp = t * 0.001 * srD;
            if (smp > viewStart + viewLen) break;
            const float x = zoom.getX() + (float) ((smp - viewStart) / spp);
            g.setColour (col::grid);
            g.fillRect (x, zoom.getY() + 1.0f, 1.0f, zoom.getHeight() - 14.0f);
            g.setColour (col::dim);
            g.drawText (juce::String (t, t < 10 ? 1 : 0), (int) x - 20, (int) zoom.getBottom() - 13, 40, 12, juce::Justification::centred, false);
        }

        g.setColour (col::dim);
        drawTrace (g, zoom, targetPtr, targetSize, zoomScale, spp);
        g.setColour (col::red);
        drawTrace (g, zoom, clone.data(), (int) clone.size(), zoomScale, spp);

        g.setFont (uiFont (10.5f));
        g.setColour (col::dim);
        g.drawText (showIsolated ? "isolated (click to see original)" : "original (click to see isolated)",
                    (int) zoom.getX() + 6, (int) zoom.getY() + 3, 200, 12, juce::Justification::centredLeft, false);
        g.setColour (col::red);
        g.drawText ("clone", (int) zoom.getX() + 210, (int) zoom.getY() + 3, 60, 12, juce::Justification::centredLeft, false);

        g.setColour (col::dim);
        juce::String info = juce::String ((int) viewLen) + " smp";
        if (hoverSample >= 0)
            info = msText ((float) (hoverSample * 1000.0 / srD), srD, 3) + "   |   " + info;
        g.drawText (info, (int) zoom.getRight() - 300, (int) zoom.getY() + 3, 294, 12, juce::Justification::centredRight, false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        if (src == nullptr || src->numSamples() == 0) return;

        const auto top = topArea();
        const auto zoom = zoomArea();
        panning = false;

        if (top.contains (e.position))
        {
            const float a = juce::jlimit (0.0f, 1.0f, (e.position.x - top.getX()) / juce::jmax (1.0f, top.getWidth()));
            juce::String err;
            const bool ok = proc.selectAt ((int) ((double) a * (double) src->numSamples()), err);
            if (onStatus) onStatus (ok ? juce::String() : err);
        }
        else if (zoom.contains (e.position))
        {
            if (juce::Rectangle<float> (zoom.getX(), zoom.getY(), 210.0f, 16.0f).contains (e.position))
            {
                showIsolated = ! showIsolated;
                rebuildTarget();
                repaint();
                return;
            }
            panning = true;
            lastDragX = e.position.x;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! panning) return;
        const auto zoom = zoomArea();
        const double spp = viewLen / (double) juce::jmax (1.0f, zoom.getWidth());
        viewStart -= (double) (e.position.x - lastDragX) * spp;
        lastDragX = e.position.x;
        clampView();
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { panning = false; }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto zoom = zoomArea();
        int s = -1;
        if (! clone.empty() && zoom.contains (e.position))
            s = (int) (viewStart + (double) (e.position.x - zoom.getX()) / (double) zoom.getWidth() * viewLen);
        if (s != hoverSample) { hoverSample = s; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverSample != -1) { hoverSample = -1; repaint(); }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (zoomArea().contains (e.position)) { resetView(); repaint(); }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const auto zoom = zoomArea();
        if (clone.empty() || ! zoom.contains (e.position)) return;
        const double f = wheel.deltaY > 0.0f ? 0.8 : 1.25;
        const double anchor = viewStart + (double) (e.position.x - zoom.getX()) / (double) zoom.getWidth() * viewLen;
        const double newLen = juce::jlimit (32.0, (double) clone.size(), viewLen * f);
        viewStart = anchor - (anchor - viewStart) * newLen / viewLen;
        viewLen = newLen;
        clampView();
        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k.getKeyCode() != juce::KeyPress::leftKey && k.getKeyCode() != juce::KeyPress::rightKey)
            return false;
        int step = 1;
        if (k.getModifiers().isShiftDown()) step = 10;
        if (k.getModifiers().isCommandDown() || k.getModifiers().isCtrlDown()) step = 100;
        if (onNudge) onNudge (k.getKeyCode() == juce::KeyPress::leftKey ? -step : step);
        return true;
    }

private:
    using Peaks = std::vector<std::pair<float, float>>;

    juce::Rectangle<float> topArea() const
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        return r.removeFromTop (std::floor (r.getHeight() * 0.32f));
    }

    juce::Rectangle<float> zoomArea() const
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        r.removeFromTop (std::floor (r.getHeight() * 0.32f));
        return r;
    }

    // Maximos/minimos por pixel de 'data'.
    static Peaks computePeaks (const float* data, int64_t dataSize, int width)
    {
        Peaks out ((size_t) juce::jmax (width, 0), { 0.0f, 0.0f });
        if (width <= 0 || dataSize <= 0 || data == nullptr) return out;
        for (int px = 0; px < width; ++px)
        {
            const int64_t a = dataSize * px / width;
            const int64_t b = juce::jmin (dataSize, juce::jmax (a + 1, dataSize * (px + 1) / width));
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

    // Traza en la vista actual: barras min/max por pixel y, a nivel de muestra, linea con puntos.
    void drawTrace (juce::Graphics& g, juce::Rectangle<float> area, const float* d, int size, float scale, double spp) const
    {
        if (d == nullptr || size <= 0) return;
        const float cy = area.getCentreY() - 6.0f;
        const float half = (area.getHeight() - 24.0f) * 0.5f;
        auto yOf = [&] (float v) { return cy - v * scale * half; };

        if (spp > 1.0)
        {
            for (int px = 0; px < (int) area.getWidth(); ++px)
            {
                const int64_t a = (int64_t) (viewStart + (double) px * spp);
                const int64_t b = juce::jmax (a + 1, (int64_t) (viewStart + (double) (px + 1) * spp));
                if (a >= size) break;
                const int64_t step = juce::jmax<int64_t> (1, (b - a) / 64);
                float mn = 1.0e9f, mx = -1.0e9f;
                for (int64_t i = a; i < juce::jmin<int64_t> (b, size); i += step)
                {
                    mn = juce::jmin (mn, d[i]);
                    mx = juce::jmax (mx, d[i]);
                }
                if (mn > mx) continue;
                const float y1 = yOf (mx), y2 = yOf (mn);
                g.fillRect (area.getX() + (float) px, y1, 1.0f, juce::jmax (1.0f, y2 - y1));
            }
            return;
        }

        const int i0 = juce::jmax (0, (int) std::floor (viewStart) - 1);
        const int i1 = juce::jmin (size - 1, (int) std::ceil (viewStart + viewLen) + 1);
        juce::Path p;
        for (int i = i0; i <= i1; ++i)
        {
            const float x = area.getX() + (float) (((double) i - viewStart) / spp);
            if (i == i0) p.startNewSubPath (x, yOf (d[i])); else p.lineTo (x, yOf (d[i]));
        }
        g.strokePath (p, juce::PathStrokeType (1.0f));
        if (1.0 / spp >= 5.0)   // 5 o mas pixeles por muestra: se ven las muestras
            for (int i = i0; i <= i1; ++i)
            {
                const float x = area.getX() + (float) (((double) i - viewStart) / spp);
                g.fillEllipse (x - 2.0f, yOf (d[i]) - 2.0f, 4.0f, 4.0f);
            }
    }

    void resetView()
    {
        viewStart = 0.0;
        viewLen = juce::jmax (32.0, (double) clone.size());
    }

    void clampView()
    {
        const double total = juce::jmax (32.0, (double) clone.size());
        viewLen = juce::jlimit (32.0, total, viewLen);
        viewStart = juce::jlimit (0.0, total - viewLen, viewStart);
    }

    void rebuildTarget()
    {
        targetPtr = nullptr;
        targetSize = 0;
        if (src == nullptr) return;
        if (showIsolated)
        {
            targetPtr = src->isolated.data();
            targetSize = (int) src->isolated.size();
        }
        else if (src->mono != nullptr)
        {
            targetPtr = src->mono->data() + juce::jlimit (0, juce::jmax (0, src->numSamples() - 1), src->startSample);
            targetSize = juce::jmax (0, src->numSamples() - src->startSample);
        }
        renderScale();
    }

    void renderScale()
    {
        float m = 0.0f;
        const int n = (int) clone.size();
        for (int i = 0; i < n; ++i)
        {
            m = juce::jmax (m, std::abs (clone[(size_t) i]));
            if (i < targetSize) m = juce::jmax (m, std::abs (targetPtr[i]));
        }
        zoomScale = 1.0f / juce::jmax (1.0e-4f, m);
    }

    void rebuild()
    {
        if (src == nullptr || src->numSamples() == 0 || getWidth() < 4)
        {
            fullPeaks.clear(); clone.clear(); targetPtr = nullptr; targetSize = 0;
            return;
        }
        fullPeaks = computePeaks (src->mono->data(), (int64_t) src->numSamples(), (int) topArea().getWidth());
        fullScale = 1.0f / juce::jmax (1.0e-4f, maxAbs (fullPeaks));
        renderClone();
    }

    void renderClone()
    {
        if (src == nullptr || ! model.valid || getWidth() < 4) return;
        clone = kc::renderOneShot (model, lastParams, src->sampleRate, 36, 1.0f).mid();
        rebuildTarget();
        clampView();
    }

    static bool sameParams (const kc::KickParams& a, const kc::KickParams& b)
    {
        using juce::exactlyEqual;
        return exactlyEqual (a.tuneSemitones, b.tuneSemitones) && exactlyEqual (a.keytrack, b.keytrack)
            && exactlyEqual (a.pitchAmount, b.pitchAmount) && exactlyEqual (a.decayScale, b.decayScale)
            && exactlyEqual (a.harmonics, b.harmonics) && exactlyEqual (a.driveDb, b.driveDb)
            && exactlyEqual (a.clickGain, b.clickGain) && exactlyEqual (a.clickDecayScale, b.clickDecayScale)
            && exactlyEqual (a.noiseGain, b.noiseGain) && exactlyEqual (a.noiseToneSt, b.noiseToneSt)
            && exactlyEqual (a.noiseDecayScale, b.noiseDecayScale) && exactlyEqual (a.width, b.width)
            && exactlyEqual (a.dynamics, b.dynamics);
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
    const float* targetPtr = nullptr;
    int targetSize = 0;
    Peaks fullPeaks;
    float fullScale = 1.0f, zoomScale = 1.0f;
    double viewStart = 0.0, viewLen = 1024.0;
    bool showIsolated = true, panning = false;
    float lastDragX = 0.0f;
    int hoverSample = -1;
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

    voicesLink = std::make_unique<LinkText> ("voices: mono", 13.0f, true);
    if (auto* vp = proc.apvts.getParameter ("voices"))
    {
        voicesAttach = std::make_unique<juce::ParameterAttachment> (*vp, [this] (float v)
        {
            voicesLink->setText (v > 0.5f ? "voices: poly" : "voices: mono");
        });
        voicesAttach->sendInitialUpdate();
        voicesLink->onClick = [this, vp]
        {
            const float cur = vp->convertFrom0to1 (vp->getValue());
            voicesAttach->setValueAsCompleteGesture (cur > 0.5f ? 0.0f : 1.0f);
        };
    }
    addAndMakeVisible (*voicesLink);

    playLink = std::make_unique<LinkText> ("play", 13.0f, true);
    playLink->onClick = [this] { proc.triggerPreview(); };
    addAndMakeVisible (*playLink);

    isoLink = std::make_unique<LinkText> ("play iso", 13.0f, true);
    isoLink->onClick = [this] { proc.triggerIsoPreview(); };
    addAndMakeVisible (*isoLink);

    exportLink = std::make_unique<LinkText> ("export clone", 13.0f, true);
    exportLink->onClick = [this] { exportFile (false); };
    addAndMakeVisible (*exportLink);

    exportIsoLink = std::make_unique<LinkText> ("export iso", 13.0f, true);
    exportIsoLink->onClick = [this] { exportFile (true); };
    addAndMakeVisible (*exportIsoLink);

    inputToggle = std::make_unique<LinkText> (juce::String::fromUTF8 ("\xe2\x96\xbe  input"), 12.0f, false, false);
    inputToggle->onClick = [this]
    {
        inputOpen = ! inputOpen;
        inputToggle->setText (juce::String::fromUTF8 (inputOpen ? "\xe2\x96\xbe  input" : "\xe2\x96\xb8  input"));
        resized();
        repaint();
    };
    addAndMakeVisible (*inputToggle);

    nudgeBack = std::make_unique<LinkText> (juce::String::fromUTF8 ("\xe2\x97\x82"), 14.0f, true, false);
    nudgeBack->onClick = [this] { nudge (-1); };
    addAndMakeVisible (*nudgeBack);
    nudgeFwd = std::make_unique<LinkText> (juce::String::fromUTF8 ("\xe2\x96\xb8"), 14.0f, false, false);
    nudgeFwd->onClick = [this] { nudge (1); };
    addAndMakeVisible (*nudgeFwd);

    pitchEditor = std::make_unique<EnvelopeEditor> (proc, EnvelopeEditor::Kind::pitch, "pitch");
    ampEditor = std::make_unique<EnvelopeEditor> (proc, EnvelopeEditor::Kind::amp, "body amplitude");
    noiseEditor = std::make_unique<EnvelopeEditor> (proc, EnvelopeEditor::Kind::noise, "noise");
    addAndMakeVisible (*pitchEditor);
    addAndMakeVisible (*ampEditor);
    addAndMakeVisible (*noiseEditor);

    waves = std::make_unique<WaveformView> (proc);
    waves->onStatus = [this] (const juce::String& s) { setStatus (s); };
    waves->onNudge = [this] (int d)
    {
        juce::String err;
        setStatus (proc.nudgeStart (d, err) ? juce::String() : err);
    };
    addAndMakeVisible (*waves);

    struct Def { const char* id; const char* caption; };
    for (auto d : { Def { "tune", "tune" }, Def { "pitchDrop", "pitch drop" }, Def { "decay", "decay" },
                    Def { "harmonics", "harmonics" }, Def { "drive", "drive" },
                    Def { "click", "click" }, Def { "clickDecay", "click decay" },
                    Def { "noise", "noise" }, Def { "noiseTone", "noise tone" }, Def { "noiseDecay", "noise decay" },
                    Def { "width", "width" }, Def { "dynamics", "dynamics" },
                    Def { "keytrack", "key track" }, Def { "level", "level" } })
    {
        sliders.push_back (std::make_unique<LabeledSlider> (proc.apvts, d.id, d.caption));
        addAndMakeVisible (*sliders.back());
    }

    proc.addChangeListener (this);

    setResizable (true, true);
    setResizeLimits (900, 600, 1800, 1100);
    setSize (1060, 660);
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
    g.drawText ("v2.0", badge, juce::Justification::centred, false);

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
        g.drawText (status, infoX, 66, getWidth() - infoX - 20, 20, juce::Justification::centredLeft, true);
    }
    else if (src != nullptr && src->file != juce::File())
    {
        g.setColour (col::dim);
        juce::String info = src->file.getFileName();
        int kicks = 0;
        for (const auto& e : src->events) if (e.lowRatio >= 0.45f) ++kicks;
        info += "   |   " + juce::String ((int) src->events.size()) + " hits detected, " + juce::String (kicks) + " kick-like";
        g.drawText (info, infoX, 66, getWidth() - infoX - 20, 20, juce::Justification::centredLeft, true);
    }

    // Inicio del golpe (ajuste a nivel de muestra)
    if (src != nullptr && src->numSamples() > 0)
    {
        g.setColour (col::dim);
        g.setFont (uiFont (11.5f));
        const juce::String s = "start " + juce::String (src->startSample) + " smp  ("
                             + juce::String ((double) src->startSample / src->sampleRate, 4) + " s)   shift x10, ctrl x100";
        const auto r = nudgeBack->getBounds();
        g.drawText (s, r.getX() - 330, r.getY(), 320, r.getHeight(), juce::Justification::centredRight, false);
    }
}

void KickCloneEditor::resized()
{
    constexpr int margin = 18;

    selectLink->setBounds (margin, 66, 165, 20);

    // Enlaces de la cabecera, de derecha a izquierda
    int right = getWidth() - margin;
    auto place = [&] (LinkText& l, int w) { l.setBounds (right - w, 16, w, 20); right -= w + 14; };
    place (*exportIsoLink, 80);
    place (*exportLink, 100);
    place (*isoLink, 70);
    place (*playLink, 44);
    place (*voicesLink, 110);

    int y = getHeight() - margin;

    // Controles: dos filas de siete
    const int rowH = 40, gap = 14;
    const int perRow = (int) sliders.size() / 2;
    const int colW = (getWidth() - margin * 2 - gap * (perRow - 1)) / perRow;
    for (int row = 1; row >= 0; --row)
    {
        for (int c = 0; c < perRow; ++c)
            sliders[(size_t) (row * perRow + c)]->setBounds (margin + c * (colW + gap), y - rowH, colW, rowH);
        y -= rowH + (row == 1 ? 8 : 12);
    }

    // Forma de onda ("input")
    if (inputOpen)
    {
        const int panelH = juce::jlimit (150, 260, getHeight() / 3);
        waves->setVisible (true);
        waves->setBounds (margin, y - panelH, getWidth() - margin * 2, panelH);
        y -= panelH + 4;
    }
    else
        waves->setVisible (false);

    const int rowY = y - 22;
    inputToggle->setBounds (margin, rowY, 90, 22);
    nudgeFwd->setBounds (getWidth() - margin - 22, rowY, 22, 22);
    nudgeBack->setBounds (getWidth() - margin - 50, rowY, 22, 22);
    y = rowY - 4;

    // Envolventes (tres, una junto a otra)
    const int envTop = 100;
    const int envH = juce::jmax (60, y - envTop);
    const int eGap = 14;
    const int eW = (getWidth() - margin * 2 - eGap * 2) / 3;
    pitchEditor->setBounds (margin, envTop, eW, envH);
    ampEditor->setBounds (margin + eW + eGap, envTop, eW, envH);
    noiseEditor->setBounds (margin + (eW + eGap) * 2, envTop, eW, envH);
}

void KickCloneEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    pitchEditor->refresh();
    ampEditor->refresh();
    noiseEditor->refresh();
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

void KickCloneEditor::nudge (int direction)
{
    const auto mods = juce::ModifierKeys::currentModifiers;
    int step = 1;
    if (mods.isShiftDown()) step = 10;
    if (mods.isCommandDown() || mods.isCtrlDown()) step = 100;
    juce::String err;
    setStatus (proc.nudgeStart (direction * step, err) ? juce::String() : err);
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

void KickCloneEditor::exportFile (bool isolated)
{
    chooser = std::make_unique<juce::FileChooser> (isolated ? "Export the isolated kick as .wav" : "Export the cloned kick as .wav",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile (isolated ? "kick_isolated.wav" : "kick_clone.wav"),
                                                   "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, isolated] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f == juce::File()) return;
                              if (! f.hasFileExtension ("wav")) f = f.withFileExtension ("wav");
                              juce::String err;
                              const bool ok = isolated ? proc.exportIsolatedWav (f, err) : proc.exportWav (f, err);
                              setStatus (ok ? juce::String() : err);
                          });
}
