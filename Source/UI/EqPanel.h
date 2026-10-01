#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "DSP/Equalizer.h"
#include "DSP/SpectrumAnalyzer.h"
#include "ParamIDs.h"
#include "Widgets.h"

namespace ui
{
struct EqBandInfo
{
    const char* name;
    const char* freqId;
    const char* gainId; // nullptr for the cut filters
    const char* qId;    // nullptr unless the band is parametric
    juce::Colour colour;
};

inline const std::array<EqBandInfo, fx::Equalizer::numBands>& eqBands()
{
    using namespace ParamIDs;
    static const std::array<EqBandInfo, fx::Equalizer::numBands> bands { {
        { "LOW CUT",  eqLowCut,    nullptr,     nullptr,  juce::Colour (0xffa8b2bd) },
        { "BASS",     eqBassFreq,  eqBassGain,  nullptr,  juce::Colour (0xffff6b5e) },
        { "LO MID",   eqLoMidFreq, eqLoMidGain, eqLoMidQ, juce::Colour (0xffffb020) },
        { "HI MID",   eqHiMidFreq, eqHiMidGain, eqHiMidQ, juce::Colour (0xff35e07a) },
        { "TREBLE",   eqTrebFreq,  eqTrebGain,  nullptr,  juce::Colour (0xff4fb3ff) },
        { "HIGH CUT", eqHighCut,   nullptr,     nullptr,  juce::Colour (0xffc58bff) },
    } };
    return bands;
}

//==============================================================================
/** Frequency-response graph with draggable band points and a live spectrum of the post-EQ signal. */
class EqDisplay : public juce::Component
{
public:
    explicit EqDisplay (juce::AudioProcessorValueTreeState& s) : state (s) {}

    std::function<void (int band)> onBandSelected;

    void setSelectedBand (int band) { selected = band; repaint(); }

    void setShowAnalyzer (bool shouldShow)
    {
        showAnalyzer = shouldShow;
        analyzer.clear();
        repaint();
    }

    void pushSamples (const float* data, int numSamples) { analyzer.push (data, numSamples); }

    /** Call at the UI refresh rate. */
    void update (double sampleRate, bool eqEnabled)
    {
        if (sampleRate > 0.0)
            fs = sampleRate;

        eqActive = eqEnabled;
        analyzer.setSampleRate (fs);
        if (showAnalyzer)
            analyzer.process();

        repaint();
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff08090b));
        g.fillRoundedRectangle (area, 8.0f);
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawRoundedRectangle (area.reduced (0.5f), 8.0f, 1.0f);

        drawGrid (g);

        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (plot.toNearestInt());

            if (showAnalyzer)
                drawSpectrum (g);

            drawCurves (g);
        }

        drawReadout (g);
        drawNodes (g); // points stay on top of the readout when dragged near the top
    }

    void resized() override
    {
        plot = getLocalBounds().toFloat().withTrimmedLeft (30.0f).withTrimmedBottom (16.0f).reduced (6.0f, 8.0f);
    }

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int band = bandAt (e.position);
        if (band != hovered)
        {
            hovered = band;
            setMouseCursor (band >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override { hovered = -1; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = bandAt (e.position);
        if (dragging < 0)
            return;

        select (dragging);
        forEachParam (dragging, [] (juce::RangedAudioParameter& p) { p.beginChangeGesture(); });
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;

        const auto& band = eqBands()[(size_t) dragging];
        setParam (band.freqId, freqForX (e.position.x));
        if (band.gainId != nullptr)
            setParam (band.gainId, dbForY (e.position.y));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0)
            forEachParam (dragging, [] (juce::RangedAudioParameter& p) { p.endChangeGesture(); });
        dragging = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int band = bandAt (e.position);
        if (band >= 0)
            forEachParam (band, [] (juce::RangedAudioParameter& p)
            {
                p.beginChangeGesture();
                p.setValueNotifyingHost (p.getDefaultValue());
                p.endChangeGesture();
            });
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const int hit = bandAt (e.position);
        const int band = hit >= 0 ? hit : selected;
        const char* qId = eqBands()[(size_t) band].qId;

        if (qId == nullptr)
            return;

        select (band);
        auto* p = state.getParameter (qId);
        const float q = value (qId) * std::pow (2.0f, wheel.deltaY * (wheel.isReversed ? -2.0f : 2.0f));
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (q));
        p->endChangeGesture();
    }

private:
    static constexpr float minHz = fx::SpectrumAnalyzer::minHz, maxHz = fx::SpectrumAnalyzer::maxHz;
    static constexpr float rangeDb = 18.0f, spectrumRangeDb = 84.0f;

    //==============================================================================
    float value (const char* id) const { return state.getRawParameterValue (id)->load(); }

    void setParam (const char* id, float newValue)
    {
        if (auto* p = state.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (newValue));
    }

    template <typename Fn>
    void forEachParam (int band, Fn&& fn)
    {
        const auto& info = eqBands()[(size_t) band];
        for (const char* id : { info.freqId, info.gainId, info.qId })
            if (id != nullptr)
                if (auto* p = state.getParameter (id))
                    fn (*p);
    }

    void select (int band)
    {
        if (onBandSelected != nullptr)
            onBandSelected (band);
    }

    fx::EqSettings readSettings() const
    {
        using namespace ParamIDs;
        fx::EqSettings s;
        s.lowCutHz  = value (eqLowCut);
        s.bassHz    = value (eqBassFreq);  s.bassDb    = value (eqBassGain);
        s.lowMidHz  = value (eqLoMidFreq); s.lowMidDb  = value (eqLoMidGain); s.lowMidQ  = value (eqLoMidQ);
        s.highMidHz = value (eqHiMidFreq); s.highMidDb = value (eqHiMidGain); s.highMidQ = value (eqHiMidQ);
        s.trebleHz  = value (eqTrebFreq);  s.trebleDb  = value (eqTrebGain);
        s.highCutHz = value (eqHighCut);
        return s;
    }

    //==============================================================================
    float xForFreq (float hz) const  { return plot.getX() + plot.getWidth() * std::log (hz / minHz) / std::log (maxHz / minHz); }
    float freqForNorm (float n) const { return minHz * std::pow (maxHz / minHz, juce::jlimit (0.0f, 1.0f, n)); }
    float freqForX (float x) const    { return freqForNorm ((x - plot.getX()) / plot.getWidth()); }
    float yForDb (float db) const     { return plot.getCentreY() - db / rangeDb * plot.getHeight() * 0.5f; }
    float dbForY (float y) const      { return (plot.getCentreY() - y) / (plot.getHeight() * 0.5f) * rangeDb; }

    float yForSpectrum (float db) const
    {
        return plot.getBottom() - plot.getHeight() * juce::jlimit (0.0f, 1.0f, (db + spectrumRangeDb) / spectrumRangeDb);
    }

    juce::Point<float> nodePosition (int band, const fx::EqSettings& s) const
    {
        switch (band)
        {
            case fx::Equalizer::lowCut:  return { xForFreq (s.lowCutHz),  yForDb (-3.0f) };
            case fx::Equalizer::bass:    return { xForFreq (s.bassHz),    yForDb (s.bassDb) };
            case fx::Equalizer::lowMid:  return { xForFreq (s.lowMidHz),  yForDb (s.lowMidDb) };
            case fx::Equalizer::highMid: return { xForFreq (s.highMidHz), yForDb (s.highMidDb) };
            case fx::Equalizer::treble:  return { xForFreq (s.trebleHz),  yForDb (s.trebleDb) };
            default:                     return { xForFreq (s.highCutHz), yForDb (-3.0f) };
        }
    }

    int bandAt (juce::Point<float> pos) const
    {
        const auto s = readSettings();
        int best = -1;
        float bestDistance = 14.0f;

        for (int b = 0; b < fx::Equalizer::numBands; ++b)
        {
            const float d = nodePosition (b, s).getDistanceFrom (pos);
            if (d < bestDistance)
            {
                bestDistance = d;
                best = b;
            }
        }
        return best;
    }

    //==============================================================================
    //==============================================================================
    void drawGrid (juce::Graphics& g)
    {
        g.setFont (plainFont (10.5f));

        for (float hz : { 30.0f, 40.0f, 60.0f, 70.0f, 80.0f, 90.0f, 300.0f, 400.0f, 600.0f, 700.0f, 800.0f, 900.0f,
                          3000.0f, 4000.0f, 6000.0f, 7000.0f, 8000.0f, 9000.0f })
        {
            g.setColour (juce::Colours::white.withAlpha (0.035f));
            g.drawVerticalLine ((int) xForFreq (hz), plot.getY(), plot.getBottom());
        }

        const std::pair<float, const char*> labels[] = { { 50.0f, "50" }, { 100.0f, "100" }, { 200.0f, "200" }, { 500.0f, "500" },
                                                         { 1000.0f, "1k" }, { 2000.0f, "2k" }, { 5000.0f, "5k" }, { 10000.0f, "10k" } };
        for (const auto& [hz, text] : labels)
        {
            const float x = xForFreq (hz);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (colours::dimText);
            g.drawText (text, juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 3.0f, 40.0f, 14.0f), juce::Justification::centred);
        }

        for (int db = -12; db <= 12; db += 6)
        {
            const float y = yForDb ((float) db);
            g.setColour (juce::Colours::white.withAlpha (db == 0 ? 0.16f : 0.07f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            g.setColour (colours::dimText);
            g.drawText ((db > 0 ? "+" : "") + juce::String (db), juce::Rectangle<float> (2.0f, y - 7.0f, 26.0f, 14.0f),
                        juce::Justification::centredRight);
        }
    }

    void drawSpectrum (juce::Graphics& g)
    {
        constexpr int columns = fx::SpectrumAnalyzer::numColumns;
        const float* levels = analyzer.getColumnsDb();

        juce::Path spectrum;
        spectrum.startNewSubPath (plot.getX(), plot.getBottom());
        for (int c = 0; c < columns; ++c)
            spectrum.lineTo (plot.getX() + plot.getWidth() * (float) c / (columns - 1), yForSpectrum (levels[c]));
        spectrum.lineTo (plot.getRight(), plot.getBottom());
        spectrum.closeSubPath();

        g.setGradientFill (juce::ColourGradient (juce::Colour (0x5534c3ff), 0.0f, plot.getY(),
                                                 juce::Colour (0x0834c3ff), 0.0f, plot.getBottom(), false));
        g.fillPath (spectrum);
        g.setColour (juce::Colour (0x8834c3ff));
        g.strokePath (spectrum, juce::PathStrokeType (1.0f));
    }

    void drawCurves (juce::Graphics& g)
    {
        fx::Biquad filters[fx::Equalizer::numBands];
        fx::Equalizer::design (readSettings(), fs, filters);

        const int points = juce::jmax (2, (int) plot.getWidth() / 2);
        juce::Path total, selectedBand;
        const float zeroY = yForDb (0.0f);
        selectedBand.startNewSubPath (plot.getX(), zeroY);

        for (int i = 0; i < points; ++i)
        {
            const float x  = plot.getX() + plot.getWidth() * (float) i / (float) (points - 1);
            const double hz = freqForX (x);

            double totalGain = 1.0;
            for (const auto& f : filters)
                totalGain *= f.getMagnitude (hz, fs);

            const float yTotal = yForDb (fx::gainToDb ((float) totalGain));
            const float ySel   = yForDb (fx::gainToDb ((float) filters[selected].getMagnitude (hz, fs)));

            if (i == 0) total.startNewSubPath (x, yTotal);
            else        total.lineTo (x, yTotal);
            selectedBand.lineTo (x, ySel);
        }

        selectedBand.lineTo (plot.getRight(), zeroY);
        selectedBand.closeSubPath();

        const auto bandColour = eqBands()[(size_t) selected].colour;
        g.setColour (bandColour.withAlpha (eqActive ? 0.18f : 0.07f));
        g.fillPath (selectedBand);

        g.setColour (juce::Colours::white.withAlpha (eqActive ? 0.95f : 0.3f));
        g.strokePath (total, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void drawNodes (juce::Graphics& g)
    {
        const auto s = readSettings();

        for (int b = 0; b < fx::Equalizer::numBands; ++b)
        {
            const auto pos = nodePosition (b, s);
            const auto colour = eqBands()[(size_t) b].colour.withMultipliedAlpha (eqActive ? 1.0f : 0.45f);
            const float r = b == selected ? 7.5f : 6.0f;

            if (b == selected || b == hovered)
            {
                g.setColour (colour.withAlpha (0.25f));
                g.fillEllipse (juce::Rectangle<float> (r * 3.2f, r * 3.2f).withCentre (pos));
            }

            g.setColour (colour);
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (pos));
            g.setColour (b == selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.6f));
            g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (pos), b == selected ? 2.0f : 1.0f);
        }
    }

    void drawReadout (juce::Graphics& g)
    {
        const auto& band = eqBands()[(size_t) selected];
        juce::String text = band.name;

        for (const char* id : { band.freqId, band.gainId, band.qId })
            if (id != nullptr)
                if (auto* p = state.getParameter (id))
                    text << "   " << (id == band.qId ? "Q " : "") << p->getCurrentValueAsText();

        auto top = plot.withHeight (16.0f).translated (0.0f, 2.0f).reduced (6.0f, 0.0f);
        g.setFont (boldFont (12.0f));
        const float textWidth = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text);
        g.setColour (juce::Colour (0xc008090b));
        g.fillRoundedRectangle (top.withWidth (textWidth + 12.0f).translated (-6.0f, 0.0f).expanded (0.0f, 2.0f), 4.0f);
        g.setColour (band.colour);
        g.drawText (text, top, juce::Justification::centredLeft);

        g.setFont (plainFont (11.0f));
        g.setColour (colours::dimText);
        g.drawText (eqActive ? "drag points  |  wheel: Q  |  double-click: reset" : "EQ BYPASSED",
                    top, juce::Justification::centredRight);
    }

    //==============================================================================
    juce::AudioProcessorValueTreeState& state;
    juce::Rectangle<float> plot;
    double fs = 48000.0;
    bool eqActive = true, showAnalyzer = true;
    int selected = fx::Equalizer::lowMid, hovered = -1, dragging = -1;

    fx::SpectrumAnalyzer analyzer;
};

//==============================================================================
/** The EQ rack unit: switches on the left, the graph in the middle, band controls on the right. */
class EqPanel : public juce::Component
{
public:
    explicit EqPanel (juce::AudioProcessorValueTreeState& s) : state (s), display (s)
    {
        addAndMakeVisible (display);
        display.onBandSelected = [this] (int band) { selectBand (band); };

        onButton.setTooltip ("Turn the EQ on / off");
        onAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, ParamIDs::eqOn, onButton);
        addAndMakeVisible (onButton);

        analyzerButton.setToggleState (true, juce::dontSendNotification);
        analyzerButton.setTooltip ("Show the live spectrum of the sound coming out of the EQ");
        analyzerButton.onClick = [this] { display.setShowAnalyzer (analyzerButton.getToggleState()); };
        addAndMakeVisible (analyzerButton);

        flatButton.setTooltip ("Reset every EQ band to flat");
        flatButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1c1c20));
        flatButton.setColour (juce::TextButton::textColourOffId, colours::text);
        flatButton.onClick = [this] { resetAllBands(); };
        addAndMakeVisible (flatButton);

        // noise section: mains-hum filter (on the input) and hiss reduction (on the output)
        humBox.addItemList ({ "Hum: off", "Hum: 50 Hz", "Hum: 60 Hz" }, 1);
        humBox.setTooltip ("Removes mains hum (50 or 60 Hz and its harmonics) from the guitar input. Taiwan, the Americas: 60 Hz; Europe, China: 50 Hz.");
        humAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, ParamIDs::noiseHum, humBox);
        addAndMakeVisible (humBox);

        denoiseBar.setSliderStyle (juce::Slider::LinearBar);
        denoiseBar.setColour (juce::Slider::trackColourId, juce::Colour (0xff34c3ff).withAlpha (0.45f));
        denoiseBar.setColour (juce::Slider::textBoxTextColourId, colours::text);
        denoiseBar.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::black.withAlpha (0.5f));
        denoiseBar.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff1c1c20));
        denoiseBar.setTooltip ("Hiss reduction: open while you play, turns the hiss down as the sound dies away. 0 = off; drag right for more.");
        denoiseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, ParamIDs::noiseAmount, denoiseBar);
        denoiseBar.textFromValueFunction = [] (double v) { return v < 0.5 ? juce::String ("Denoise: off") : "Denoise " + juce::String (juce::roundToInt (v)) + " %"; };
        denoiseBar.updateText();
        addAndMakeVisible (denoiseBar);

        for (int b = 0; b < fx::Equalizer::numBands; ++b)
        {
            const auto& info = eqBands()[(size_t) b];
            auto button = std::make_unique<ChipButton> (info.name, info.colour, 0xE0);
            button->onClick = [this, b] { selectBand (b); };
            addAndMakeVisible (*button);
            bandButtons.push_back (std::move (button));
        }

        for (auto* knob : { &freqKnob, &gainKnob, &qKnob })
        {
            styleKnob (*knob, colours::amber);
            addChildComponent (*knob);
        }

        using LabelText = std::pair<juce::Label*, const char*>;
        for (auto [label, text] : { LabelText { &freqLabel, "FREQ" }, LabelText { &gainLabel, "GAIN" }, LabelText { &qLabel, "Q" } })
        {
            label->setText (text, juce::dontSendNotification);
            label->setFont (boldFont (12.0f));
            label->setColour (juce::Label::textColourId, colours::dimText);
            label->setJustificationType (juce::Justification::centred);
            addChildComponent (*label);
        }

        selectBand (fx::Equalizer::lowMid);
    }

    void pushAnalyzerSamples (const float* data, int numSamples) { display.pushSamples (data, numSamples); }

    void update (double sampleRate)
    {
        display.update (sampleRate, state.getRawParameterValue (ParamIDs::eqOn)->load() > 0.5f);
    }

    void selectBand (int band)
    {
        selected = band;
        display.setSelectedBand (band);
        bandButtons[(size_t) band]->setToggleState (true, juce::dontSendNotification);

        const auto& info = eqBands()[(size_t) band];
        using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;

        const auto bind = [&] (juce::Slider& knob, juce::Label& label, std::unique_ptr<Attachment>& attachment, const char* id)
        {
            attachment.reset();
            knob.setVisible (id != nullptr);
            label.setVisible (id != nullptr);
            knob.setColour (juce::Slider::rotarySliderFillColourId, info.colour);
            if (id != nullptr)
                attachment = std::make_unique<Attachment> (state, id, knob);
        };

        bind (freqKnob, freqLabel, freqAttachment, info.freqId);
        bind (gainKnob, gainLabel, gainAttachment, info.gainId);
        bind (qKnob, qLabel, qAttachment, info.qId);
        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff26262c), 0.0f, area.getY(),
                                                 juce::Colour (0xff16161a), 0.0f, area.getBottom(), false));
        g.fillRoundedRectangle (area, 12.0f);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawRoundedRectangle (area.reduced (1.0f), 11.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (area, 12.0f, 1.2f);

        auto title = titleArea;
        g.setFont (boldFont (22.0f));
        g.setColour (colours::text);
        g.drawText ("EQ", title.removeFromTop (26.0f), juce::Justification::centredLeft);
        g.setFont (plainFont (11.5f));
        g.setColour (colours::dimText);
        g.drawText ("6-band, at the output", title.removeFromTop (16.0f), juce::Justification::centredLeft);

        g.setFont (boldFont (11.0f));
        g.drawText ("NOISE", noiseLabelArea, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 12);

        // left: title and switches
        auto left = area.removeFromLeft (130);
        titleArea = left.removeFromTop (42).toFloat();
        left.removeFromTop (4);
        onButton.setBounds (left.removeFromTop (26));
        left.removeFromTop (4);
        analyzerButton.setBounds (left.removeFromTop (26));
        left.removeFromTop (4);
        flatButton.setBounds (left.removeFromTop (24));

        // noise section at the bottom
        denoiseBar.setBounds (left.removeFromBottom (24));
        left.removeFromBottom (4);
        humBox.setBounds (left.removeFromBottom (24));
        noiseLabelArea = left.removeFromBottom (16).toFloat();
        area.removeFromLeft (14);

        // right: band chips (2 x 3) and the selected band's knobs
        auto right = area.removeFromRight (264);
        area.removeFromRight (14);
        right = right.withSizeKeepingCentre (right.getWidth(), 58 + 10 + 100);

        auto chips = right.removeFromTop (58);
        const int chipWidth = chips.getWidth() / 3;
        for (int b = 0; b < (int) bandButtons.size(); ++b)
            bandButtons[(size_t) b]->setBounds (chips.getX() + (b % 3) * chipWidth, chips.getY() + (b / 3) * 30, chipWidth - 4, 26);

        right.removeFromTop (10);
        using KnobAndLabel = std::pair<juce::Slider*, juce::Label*>;
        std::vector<KnobAndLabel> visible;
        for (auto pair : { KnobAndLabel { &freqKnob, &freqLabel }, KnobAndLabel { &gainKnob, &gainLabel }, KnobAndLabel { &qKnob, &qLabel } })
            if (pair.first->isVisible())
                visible.push_back (pair);

        const int knobWidth = 84;
        auto knobRow = right.withSizeKeepingCentre (knobWidth * (int) visible.size(), right.getHeight());
        for (auto [knob, label] : visible)
        {
            auto cell = knobRow.removeFromLeft (knobWidth);
            label->setBounds (cell.removeFromTop (16));
            knob->setBounds (cell);
        }

        display.setBounds (area);
    }

private:
    void resetAllBands()
    {
        for (const auto& info : eqBands())
            for (const char* id : { info.freqId, info.gainId, info.qId })
                if (id != nullptr)
                    if (auto* p = state.getParameter (id))
                    {
                        p->beginChangeGesture();
                        p->setValueNotifyingHost (p->getDefaultValue());
                        p->endChangeGesture();
                    }
    }

    juce::AudioProcessorValueTreeState& state;
    int selected = fx::Equalizer::lowMid;
    juce::Rectangle<float> titleArea, noiseLabelArea;

    EqDisplay display;
    LedButton onButton { "EQ ON", colours::ledGreen };
    LedButton analyzerButton { "ANALYZER", juce::Colour (0xff34c3ff) };
    juce::TextButton flatButton { "FLAT" };
    juce::ComboBox humBox;
    juce::Slider denoiseBar;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> humAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> denoiseAttachment;
    std::vector<std::unique_ptr<ChipButton>> bandButtons;

    juce::Slider freqKnob, gainKnob, qKnob;
    juce::Label freqLabel, gainLabel, qLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> freqAttachment, gainAttachment, qAttachment;
};

} // namespace ui
