#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "DSP/TapTempo.h"
#include "Widgets.h"

namespace ui
{
/** Pill-shaped TAP button with a beat LED and the current tempo. */
class TapButton : public juce::Button
{
public:
    TapButton() : juce::Button ("TAP")
    {
        setTriggeredOnMouseDown (true); // the moment you press is the beat, not when you let go
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void setDisplay (bool ledOn, const juce::String& bpm)
    {
        if (ledOn != led || bpm != bpmText)
        {
            led = ledOn;
            bpmText = bpm;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto area = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (juce::Colour (down ? 0xff0a0a0c : (highlighted ? 0xdd2a2a30 : 0xcc111114)));
        g.fillRoundedRectangle (area, 5.0f);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (area, 5.0f, 1.0f);

        auto ledArea = area.removeFromLeft (18.0f);
        drawLed (g, juce::Rectangle<float> (8.0f, 8.0f).withCentre (ledArea.getCentre().translated (2.0f, 0.0f)), colours::amber, led);

        g.setColour (colours::text);
        g.setFont (boldFont (12.0f));
        g.drawText (bpmText.isEmpty() ? juce::String ("TAP") : "TAP " + bpmText, area.withTrimmedRight (4.0f), juce::Justification::centred);
    }

private:
    bool led = false;
    juce::String bpmText;
};

//==============================================================================
/** The global tempo, like the HD500X's TAP switch: tapping sets the BPM that every
    tempo-synced time (delay time, reverb pre-delay set to a note value) follows. */
class GlobalTempo : public juce::Component
{
public:
    GlobalTempo (juce::AudioProcessorValueTreeState& state, const char* tempoParamId)
        : tempoParam (*state.getParameter (tempoParamId))
    {
        tapButton.onClick = [this] { tapped(); };
        tapButton.setTooltip ("Tap 3-4 times in time with the music: sets the tempo that delay / reverb times set to a note value follow");
        addAndMakeVisible (tapButton);

        bpmBox.setSliderStyle (juce::Slider::IncDecButtons);
        bpmBox.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
        bpmBox.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 58, 24);
        bpmBox.setColour (juce::Slider::textBoxTextColourId, colours::text);
        bpmBox.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::black.withAlpha (0.5f));
        bpmBox.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff1c1c20));
        bpmBox.setTooltip ("Tempo (BPM): click to type, or drag / use the arrows");
        addAndMakeVisible (bpmBox);
        bpmAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, tempoParamId, bpmBox);
        bpmBox.textFromValueFunction = [] (double v) { return juce::String (v, v == std::round (v) ? 0 : 1); };
        bpmBox.updateText();
    }

    /** Call from the UI timer: blinks the LED on the beat. */
    void tick()
    {
        const double bpm = tempoParam.convertFrom0to1 (tempoParam.getValue());
        const double beat = 60000.0 / std::max (1.0, bpm);
        const double since = juce::Time::getMillisecondCounterHiRes() - (tapTempo.getLastTapMs() > 0.0 ? tapTempo.getLastTapMs() : 0.0);
        tapButton.setDisplay (std::fmod (since, beat) < 90.0, {});
    }

    void resized() override
    {
        auto area = getLocalBounds();
        tapButton.setBounds (area.removeFromTop (area.getHeight() / 2).reduced (0, 2));
        bpmBox.setBounds (area.reduced (0, 2));
    }

private:
    void tapped()
    {
        const double beat = tapTempo.tap (juce::Time::getMillisecondCounterHiRes());
        if (beat <= 0.0)
            return;

        tempoParam.beginChangeGesture();
        tempoParam.setValueNotifyingHost (tempoParam.convertTo0to1 ((float) (60000.0 / beat)));
        tempoParam.endChangeGesture();
    }

    juce::RangedAudioParameter& tempoParam;
    fx::TapTempo tapTempo;
    TapButton tapButton;
    juce::Slider bpmBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bpmAttachment;
};

} // namespace ui
