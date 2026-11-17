#pragma once

#include "PedalLookAndFeel.h"
#include "../StandaloneAudio.h"

namespace ui
{
/** The standalone app's audio latency, in the header: driver, buffer and the round trip the driver reports,
    green / amber / red. Click it for the audio settings. Hidden inside a DAW (the host owns the audio there). */
class LatencyReadout : public juce::Component,
                       public juce::SettableTooltipClient
{
public:
    LatencyReadout() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    /** Call a few times per second. Returns true when there is something to show. */
    bool refresh()
    {
        const auto now = standalone::getAudioStatus();
        const auto text = describe (now);

        if (text != shown)
        {
            shown = text;
            status = now;
            setTooltip (tooltipFor (now));
            repaint();
        }

        return now.available;
    }

    void paint (juce::Graphics& g) override
    {
        if (! status.available)
            return;

        const auto area = getLocalBounds().toFloat();
        const auto colour = colourFor (status);

        const auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ area.getX() + 4.5f, area.getCentreY() });
        g.setColour (colour.withAlpha (0.35f));
        g.fillEllipse (dot.expanded (2.0f));
        g.setColour (colour);
        g.fillEllipse (dot);

        g.setFont (plainFont (12.0f));
        g.setColour (isMouseOver() ? colours::text : colours::dimText);
        g.drawFittedText (shown, getLocalBounds().withTrimmedLeft (14), juce::Justification::centredLeft, 1, 0.7f);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (status.available && ! e.mouseWasDraggedSinceMouseDown() && getLocalBounds().contains (e.getPosition()))
            standalone::showAudioSettings();
    }

    /** Under 15 ms feels immediate, up to 30 ms is playable, more than that is felt as lag. */
    static juce::Colour colourFor (const standalone::AudioStatus& s)
    {
        const double ms = s.roundTripMs();
        return ms <= 15.0 ? colours::ledGreen : ms <= 30.0 ? colours::amber : colours::ledRed;
    }

    static juce::String describe (const standalone::AudioStatus& s)
    {
        if (! s.available)
            return {};

        const double ms = s.roundTripMs();
        return s.shortDriver() + "  |  " + juce::String (s.bufferSize) + " smp  |  "
             + (ms < 10.0 ? juce::String (ms, 1) : juce::String (juce::roundToInt (ms))) + " ms";
    }

    static juce::String tooltipFor (const standalone::AudioStatus& s)
    {
        if (! s.available)
            return {};

        juce::String t;
        t << "Audio: " << s.driver << ", " << s.device << "\n"
          << "Buffer " << s.bufferSize << " samples (" << juce::String (s.bufferMs(), 1) << " ms) at "
          << juce::String (s.sampleRate / 1000.0, 1) << " kHz\n"
          << "Latency reported by the driver: in " << juce::String (s.inputMs(), 1) << " + out "
          << juce::String (s.outputMs(), 1) << " = " << juce::String (s.roundTripMs(), 1) << " ms\n\n"
          << "Click to open the audio settings.";

        if (s.isSharedMode())
            t << "\nThis driver type shares the device with other programs and can't go below Windows' 10 ms period."
                 " For less latency choose ASIO, Windows Audio (Exclusive Mode) or Windows Audio (Low Latency Mode).";
        else if (s.roundTripMs() > 15.0)
            t << "\nFor less latency try a smaller audio buffer size (64 - 256 samples); go back up if the sound crackles.";

        return t;
    }

private:
    standalone::AudioStatus status;
    juce::String shown;
};

} // namespace ui
