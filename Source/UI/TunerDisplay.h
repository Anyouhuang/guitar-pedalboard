#pragma once

#include "PedalLookAndFeel.h"
#include "DSP/PitchDetector.h"

namespace ui
{
/** Chromatic tuner. Feed it raw input with pushSamples(), then call update() ~30x per second.
    Click it to switch it off (it then does no work at all) or back on. */
class TunerDisplay : public juce::Component,
                     public juce::SettableTooltipClient
{
public:
    std::function<void()> onClick;

    TunerDisplay()
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("Tuner: click to switch it on / off");
    }

    bool isActive() const noexcept { return active; }

    void setActive (bool shouldBeActive)
    {
        if (active == shouldBeActive)
            return;

        active = shouldBeActive;
        frequency = 0.0f;
        pendingJumps = framesWithoutPitch = 0;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (onClick && ! e.mouseWasDraggedSinceMouseDown() && getLocalBounds().contains (e.getPosition()))
            onClick();
    }

    void prepare (double sampleRate)
    {
        detector.prepare (sampleRate);
        preparedRate = sampleRate;
        frequency = 0.0f;
    }

    double getPreparedRate() const noexcept { return preparedRate; }

    void pushSamples (const float* data, int numSamples) { detector.pushSamples (data, numSamples); }

    void update()
    {
        const float hz = detector.detect();

        if (hz > 0.0f)
        {
            framesWithoutPitch = 0;
            const bool bigJump = frequency <= 0.0f || std::abs (1200.0f * std::log2 (hz / frequency)) > 40.0f;

            if (! bigJump)
            {
                frequency += 0.3f * (hz - frequency); // steady the needle
                pendingJumps = 0;
            }
            else if (frequency <= 0.0f || ++pendingJumps >= 2) // ignore one-frame glitches
            {
                frequency = hz;
                pendingJumps = 0;
            }
        }
        else if (++framesWithoutPitch > 18)
        {
            frequency = 0.0f; // hold the last reading for ~0.6 s, then clear
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();

        g.setColour (juce::Colour (0xff050807));
        g.fillRoundedRectangle (area, 8.0f);
        g.setColour (colours::lcd);
        g.fillRoundedRectangle (area.reduced (3.0f), 6.0f);
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawRoundedRectangle (area.reduced (0.5f), 8.0f, 1.0f);

        if (! active)
        {
            g.setColour (colours::lcdText.withAlpha (0.35f));
            g.setFont (boldFont (20.0f));
            g.drawText ("TUNER OFF", area.withTrimmedBottom (18.0f), juce::Justification::centred);
            g.setFont (plainFont (12.0f));
            g.drawText ("click to switch it on", area.withTrimmedTop (44.0f), juce::Justification::centred);
            return;
        }

        area.reduce (14.0f, 8.0f);
        const bool hasPitch = frequency > 0.0f;
        const auto note = hasPitch ? fx::frequencyToNote (frequency) : fx::NoteInfo {};
        const float cents = note.cents;
        const float absCents = std::abs (cents);
        const auto stateColour = ! hasPitch       ? colours::lcdText.withAlpha (0.18f)
                               : absCents < 3.0f  ? colours::ledGreen
                               : absCents < 15.0f ? colours::amber
                                                  : colours::ledRed;

        // note name
        auto noteArea = area.removeFromLeft (84.0f);
        g.setColour (hasPitch ? stateColour : colours::lcdText.withAlpha (0.25f));
        g.setFont (boldFont (46.0f));
        g.drawText (hasPitch ? juce::String (fx::noteName (note.midiNote)) : juce::String ("-"),
                    noteArea.withTrimmedBottom (14.0f), juce::Justification::centred);
        g.setFont (plainFont (13.0f));
        g.setColour (colours::lcdText.withAlpha (hasPitch ? 0.7f : 0.3f));
        g.drawText (hasPitch ? "octave " + juce::String (fx::noteOctave (note.midiNote)) : juce::String ("TUNER"),
                    noteArea.removeFromBottom (16.0f), juce::Justification::centred);

        area.removeFromLeft (10.0f);

        // cents scale: -50 .. +50
        auto readout = area.removeFromBottom (18.0f);
        auto scale = area.reduced (4.0f, 6.0f);
        const auto xFor = [&] (float c) { return scale.getX() + scale.getWidth() * (c + 50.0f) / 100.0f; };

        for (int c = -50; c <= 50; c += 5)
        {
            const bool major = c % 25 == 0;
            const float h = c == 0 ? scale.getHeight() : (major ? scale.getHeight() * 0.55f : scale.getHeight() * 0.3f);
            g.setColour (colours::lcdText.withAlpha (c == 0 ? 0.6f : 0.25f));
            g.fillRect (juce::Rectangle<float> (xFor ((float) c) - 0.75f, scale.getCentreY() - h * 0.5f, 1.5f, h));
        }

        g.setColour (colours::ledGreen.withAlpha (0.12f));
        g.fillRect (juce::Rectangle<float> (xFor (-3.0f), scale.getY(), xFor (3.0f) - xFor (-3.0f), scale.getHeight()));

        if (hasPitch)
        {
            const float x = xFor (juce::jlimit (-50.0f, 50.0f, cents));
            g.setColour (stateColour.withAlpha (0.3f));
            g.fillRoundedRectangle (juce::Rectangle<float> (x - 6.0f, scale.getY() - 2.0f, 12.0f, scale.getHeight() + 4.0f), 4.0f);
            g.setColour (stateColour);
            g.fillRoundedRectangle (juce::Rectangle<float> (x - 2.0f, scale.getY() - 2.0f, 4.0f, scale.getHeight() + 4.0f), 2.0f);
        }

        g.setFont (plainFont (13.0f));
        g.setColour (colours::lcdText.withAlpha (hasPitch ? 0.85f : 0.35f));
        if (hasPitch)
        {
            g.drawText ((cents >= 0.0f ? "+" : "") + juce::String (cents, 1) + " cents", readout, juce::Justification::centredLeft);
            g.drawText (juce::String (frequency, 2) + " Hz", readout, juce::Justification::centredRight);
            if (absCents < 3.0f)
                g.drawText ("IN TUNE", readout, juce::Justification::centred);
        }
        else
        {
            g.drawText ("play a single string", readout, juce::Justification::centred);
        }
    }

private:
    fx::PitchDetector detector;
    double preparedRate = 0.0;
    float frequency = 0.0f;
    int pendingJumps = 0, framesWithoutPitch = 0;
    bool active = true;
};

} // namespace ui
