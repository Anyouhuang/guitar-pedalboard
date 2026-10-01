#pragma once

#include "PedalLookAndFeel.h"

namespace ui
{
inline void drawLed (juce::Graphics& g, juce::Rectangle<float> led, juce::Colour colour, bool on)
{
    if (on)
    {
        g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.55f), led.getCentre(),
                                                 colour.withAlpha (0.0f), led.getCentre().translated (led.getWidth() * 1.4f, 0.0f), true));
        g.fillEllipse (led.expanded (led.getWidth() * 0.9f));
    }

    g.setColour (on ? colour : colour.withMultipliedBrightness (0.28f));
    g.fillEllipse (led);
    g.setColour (juce::Colours::white.withAlpha (on ? 0.6f : 0.12f));
    g.fillEllipse (led.reduced (led.getWidth() * 0.3f).translated (-led.getWidth() * 0.12f, -led.getHeight() * 0.12f));
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawEllipse (led, 1.0f);
}

//==============================================================================
/** Stomp switch with a status LED above it. */
class FootSwitch : public juce::Button
{
public:
    FootSwitch() : juce::Button ("footswitch")
    {
        setClickingTogglesState (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto area = getLocalBounds().toFloat();
        const auto ledArea = area.removeFromTop (22.0f);
        drawLed (g, juce::Rectangle<float> (11.0f, 11.0f).withCentre (ledArea.getCentre()), colours::ledRed, getToggleState());

        const float d = juce::jmin (area.getWidth(), area.getHeight()) - 6.0f;
        const auto outer = juce::Rectangle<float> (d, d).withCentre (area.getCentre());

        // hex nut
        juce::Path nut;
        nut.addPolygon (outer.getCentre(), 6, d * 0.5f, 0.0f);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillPath (nut, juce::AffineTransform::translation (0.0f, 2.0f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffd8d8dc), outer.getX(), outer.getY(),
                                                 juce::Colour (0xff6c6c72), outer.getRight(), outer.getBottom(), false));
        g.fillPath (nut);

        // plunger
        const auto cap = outer.reduced (d * (down ? 0.22f : 0.19f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (highlighted ? 0xfff4f4f6 : 0xffe4e4e8), cap.getX(), cap.getY(),
                                                 juce::Colour (0xff75757c), cap.getRight(), cap.getBottom(), false));
        g.fillEllipse (cap);
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.drawEllipse (cap, 1.0f);
    }
};

//==============================================================================
/** Small labelled toggle with an LED, for the header (Cab, Mute). */
class LedButton : public juce::Button
{
public:
    LedButton (const juce::String& text, juce::Colour ledColour) : juce::Button (text), led (ledColour)
    {
        setClickingTogglesState (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto area = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (juce::Colour (down ? 0xff0e0e10 : (highlighted ? 0xff2c2c32 : 0xff1c1c20)));
        g.fillRoundedRectangle (area, 6.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (area, 6.0f, 1.0f);

        auto ledArea = area.removeFromLeft (26.0f);
        drawLed (g, juce::Rectangle<float> (9.0f, 9.0f).withCentre (ledArea.getCentre()), led, getToggleState());

        g.setColour (getToggleState() ? colours::text : colours::dimText);
        g.setFont (boldFont (13.0f));
        g.drawText (getButtonText(), area.withTrimmedRight (6.0f), juce::Justification::centredLeft);
    }

private:
    juce::Colour led;
};

//==============================================================================
/** Radio-style selector chip: colour dot + name, outlined in its colour when selected. */
class ChipButton : public juce::Button
{
public:
    ChipButton (const juce::String& name, juce::Colour c, int radioGroup) : juce::Button (name), colour (c)
    {
        setRadioGroupId (radioGroup);
        setClickingTogglesState (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const bool on = getToggleState();
        auto area = getLocalBounds().toFloat().reduced (1.0f);

        g.setColour (on ? colour.withAlpha (0.22f) : juce::Colour (highlighted ? 0xff2a2a30 : 0xff1c1c20));
        g.fillRoundedRectangle (area, 5.0f);
        g.setColour (on ? colour : juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (area, 5.0f, on ? 1.5f : 1.0f);

        auto dot = area.removeFromLeft (18.0f);
        g.setColour (colour);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (dot.getCentre().translated (3.0f, 0.0f)));

        g.setColour (on ? colours::text : colours::dimText);
        g.setFont (boldFont (11.5f));
        g.drawText (getButtonText(), area, juce::Justification::centred);
    }

private:
    juce::Colour colour;
};

//==============================================================================
/** Vertical peak meter with a peak-hold tick and clip indicator. */
class LevelMeter : public juce::Component
{
public:
    /** Call at the UI refresh rate with the peak since the last call. */
    void setPeak (float newPeak)
    {
        level = juce::jmax (newPeak, level * 0.82f);

        if (newPeak >= hold)       { hold = newPeak; holdFrames = 45; }
        else if (--holdFrames < 0) { hold *= 0.9f; }

        if (newPeak >= 1.0f) clipFrames = 60;
        else if (clipFrames > 0) --clipFrames;

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        const auto clipLed = area.removeFromTop (6.0f);
        area.removeFromTop (2.0f);

        g.setColour (clipFrames > 0 ? colours::ledRed : colours::ledRed.withMultipliedBrightness (0.25f));
        g.fillRoundedRectangle (clipLed, 2.0f);

        g.setColour (juce::Colour (0xff0a0a0c));
        g.fillRoundedRectangle (area, 2.0f);

        const auto toY = [&] (float gain)
        {
            const float db = juce::Decibels::gainToDecibels (gain, -60.0f);
            return area.getBottom() - area.getHeight() * juce::jmap (juce::jlimit (-60.0f, 0.0f, db), -60.0f, 0.0f, 0.0f, 1.0f);
        };

        // red above -3 dB, amber from -12 dB, green below
        juce::ColourGradient gradient (colours::ledRed, 0.0f, area.getY(), colours::ledGreen, 0.0f, area.getBottom(), false);
        gradient.addColour (0.05, colours::amber);
        gradient.addColour (0.20, colours::ledGreen);
        g.setGradientFill (gradient);
        g.fillRect (area.withTop (toY (level)).reduced (1.5f, 0.0f));

        if (hold > 0.001f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (area.getX() + 1.5f, toY (hold) - 1.0f, area.getWidth() - 3.0f, 2.0f));
        }
    }

private:
    float level = 0.0f, hold = 0.0f;
    int holdFrames = 0, clipFrames = 0;
};

} // namespace ui
