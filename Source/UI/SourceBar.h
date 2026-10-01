#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "TestSignalPlayer.h"
#include "Widgets.h"

namespace ui
{
/** Picks what feeds the board: the live guitar, the built-in demo riff or a looped audio file,
    plus buttons that pluck open strings (to try the tuner) and strum basic chords. */
class SourceBar : public juce::Component
{
public:
    explicit SourceBar (TestSignalPlayer& p) : player (p)
    {
        const std::pair<ChipButton*, TestSignalPlayer::Source> sources[] = {
            { &liveButton, TestSignalPlayer::live }, { &demoButton, TestSignalPlayer::demo }, { &fileButton, TestSignalPlayer::file } };

        for (auto [button, source] : sources)
        {
            button->onClick = [this, s = source] { choose (s); };
            addAndMakeVisible (*button);
        }

        liveButton.setTooltip ("Use the guitar plugged into your audio interface");
        demoButton.setTooltip ("Loop a built-in guitar riff - no guitar needed");
        fileButton.setTooltip ("Loop an audio file (a clean DI guitar recording works best)");

        transportButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1c1c20));
        transportButton.setColour (juce::TextButton::textColourOffId, colours::text);
        transportButton.setTooltip ("Stop / play the demo riff or audio file (plucked strings still sound)");
        transportButton.onClick = [this] { player.setPlaying (! player.isPlaying()); refresh(); };
        addChildComponent (transportButton);

        loadButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1c1c20));
        loadButton.setColour (juce::TextButton::textColourOffId, colours::text);
        loadButton.setTooltip ("Choose a WAV / MP3 / FLAC / AIFF / OGG file to loop");
        loadButton.onClick = [this] { chooseFile(); };
        addAndMakeVisible (loadButton);

        const char* names[] = { "E", "A", "D", "G", "B", "e" };
        const char* octaves[] = { "E2", "A2", "D3", "G3", "B3", "E4" };
        for (int i = 0; i < 6; ++i)
        {
            auto b = std::make_unique<juce::TextButton> (names[i]);
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1c1c20));
            b->setColour (juce::TextButton::textColourOffId, colours::text);
            b->setTooltip (juce::String ("Pluck the open ") + octaves[i] + " string (" + juce::String (fx::openStringHz[i], 2) + " Hz)");
            b->onClick = [this, i]
            {
                // plucking means "let me hear this string": stop the looping demo / file first
                if (player.getSource() != TestSignalPlayer::live && player.isPlaying())
                    player.setPlaying (false);
                player.pluck (i);
                refresh();
            };
            addAndMakeVisible (*b);
            stringButtons.push_back (std::move (b));
        }

        for (int c = 0; c < fx::numBasicChords; ++c)
        {
            const auto& chord = fx::basicChords[c];
            auto b = std::make_unique<juce::TextButton> (chord.name);
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1c1c20));
            b->setColour (juce::TextButton::textColourOffId, colours::text);
            b->setColour (juce::TextButton::buttonOnColourId, colours::amber.withAlpha (0.35f));
            b->setTooltip (juce::String ("Play ") + chord.name + " (" + chord.shape + ")");
            b->onClick = [this, c]
            {
                if (player.getSource() != TestSignalPlayer::live && player.isPlaying())
                    player.setPlaying (false); // hear the chord on its own
                player.playChord (c);
                refresh();
            };
            addAndMakeVisible (*b);
            chordButtons.push_back (std::move (b));
        }

        patternBox.addItemList ({ "Arpeggio", "Strum loop", "Single" }, 1);
        patternBox.setSelectedItemIndex (player.getChordPattern(), juce::dontSendNotification);
        patternBox.setTooltip ("How the CHORD buttons play: a looping arpeggio, a down-strum on every beat, or one strum");
        patternBox.onChange = [this] { player.setChordPattern (patternBox.getSelectedItemIndex()); };
        addAndMakeVisible (patternBox);

        tempoBox.setSliderStyle (juce::Slider::IncDecButtons);
        tempoBox.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
        tempoBox.setRange (TestSignalPlayer::minTempo, TestSignalPlayer::maxTempo, 1.0);
        tempoBox.setTextValueSuffix (" BPM");
        tempoBox.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 58, 26);
        tempoBox.setColour (juce::Slider::textBoxTextColourId, colours::text);
        tempoBox.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::black.withAlpha (0.5f));
        tempoBox.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff1c1c20));
        tempoBox.setValue (player.getChordTempo(), juce::dontSendNotification);
        tempoBox.setTooltip ("Tempo of the chord loop: click to type a number, or drag / use the arrows");
        tempoBox.onValueChange = [this] { player.setChordTempo ((float) tempoBox.getValue()); };
        addAndMakeVisible (tempoBox);

        stopChordButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1c1c20));
        stopChordButton.setColour (juce::TextButton::textColourOffId, colours::text);
        stopChordButton.setTooltip ("Stop the chord loop");
        stopChordButton.onClick = [this] { player.stopChord(); refresh(); };
        addAndMakeVisible (stopChordButton);

        refresh();
    }

    /** Call from the editor's timer to follow the player's state. */
    void refresh()
    {
        const auto source = player.getSource();
        liveButton.setToggleState (source == TestSignalPlayer::live, juce::dontSendNotification);
        demoButton.setToggleState (source == TestSignalPlayer::demo, juce::dontSendNotification);
        fileButton.setToggleState (source == TestSignalPlayer::file, juce::dontSendNotification);

        const int looping = player.getLoopingChord();
        for (int c = 0; c < (int) chordButtons.size(); ++c)
            chordButtons[(size_t) c]->setToggleState (c == looping, juce::dontSendNotification);

        const bool stopped = source != TestSignalPlayer::live && ! player.isPlaying();
        transportButton.setVisible (source != TestSignalPlayer::live);
        transportButton.setButtonText (stopped ? "PLAY" : "STOP");

        juce::String text;
        switch (source)
        {
            case TestSignalPlayer::live: text = "guitar input  (no guitar? try DEMO RIFF)"; break;
            case TestSignalPlayer::demo: text = "built-in riff, 100 BPM, looping"; break;
            case TestSignalPlayer::file: text = player.getFile().getFileName(); break;
        }
        if (stopped)
            text = "stopped - PLUCK a string or strum a CHORD, or press PLAY";
        if (looping >= 0)
            text = juce::String ("chord loop: ") + fx::basicChords[looping].name + "  |  " + patternBox.getText()
                   + "  |  " + juce::String (juce::roundToInt (player.getChordTempo())) + " BPM";

        const float newProgress = source == TestSignalPlayer::live ? -1.0f : player.getProgress();
        if (text != statusText || std::abs (newProgress - progress) > 0.001f)
        {
            statusText = text;
            progress = newProgress;
            repaint (statusArea.toNearestInt().expanded (2));
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff26262c), 0.0f, area.getY(),
                                                 juce::Colour (0xff18181c), 0.0f, area.getBottom(), false));
        g.fillRoundedRectangle (area, 10.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (area, 10.0f, 1.2f);

        g.setFont (boldFont (12.0f));
        g.setColour (colours::dimText);
        g.drawText ("INPUT", inputLabelArea, juce::Justification::centredLeft);
        g.drawText ("PLUCK", pluckLabelArea, juce::Justification::centredLeft);
        g.drawText ("CHORD", chordLabelArea, juce::Justification::centredLeft);

        auto status = statusArea;
        g.setFont (plainFont (12.0f));
        g.setColour (colours::text.withAlpha (0.8f));
        g.drawFittedText (statusText, status.withTrimmedBottom (6.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);

        if (progress >= 0.0f)
        {
            const auto bar = status.removeFromBottom (3.0f);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRoundedRectangle (bar, 1.5f);
            g.setColour (colours::amber);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * progress), 1.5f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14, 7);

        // second row: single strings and chords
        auto row2 = area.removeFromBottom (30);
        area.removeFromBottom (6);
        pluckLabelArea = row2.removeFromLeft (48).toFloat();
        for (auto& b : stringButtons)
        {
            b->setBounds (row2.removeFromLeft (28));
            row2.removeFromLeft (4);
        }
        row2.removeFromLeft (12);
        chordLabelArea = row2.removeFromLeft (52).toFloat();
        // widths add up to 936 px of the 940 available: 48 + 192 + 12 + 52 + 360 + 6 + 96 + 6 + 108 + 6 + 50
        for (auto& b : chordButtons)
        {
            b->setBounds (row2.removeFromLeft (36));
            row2.removeFromLeft (4);
        }
        row2.removeFromLeft (6);
        patternBox.setBounds (row2.removeFromLeft (96));
        row2.removeFromLeft (6);
        tempoBox.setBounds (row2.removeFromLeft (108));
        row2.removeFromLeft (6);
        stopChordButton.setBounds (row2.removeFromLeft (50));

        inputLabelArea = area.removeFromLeft (46).toFloat();
        for (auto* chip : { &liveButton, &demoButton, &fileButton })
        {
            chip->setBounds (area.removeFromLeft (94));
            area.removeFromLeft (4);
        }

        area.removeFromLeft (4);
        transportButton.setBounds (area.removeFromLeft (60));
        area.removeFromLeft (8);
        loadButton.setBounds (area.removeFromLeft (96));
        area.removeFromLeft (14);
        statusArea = area.toFloat();
    }

private:
    void choose (TestSignalPlayer::Source source)
    {
        if (source == TestSignalPlayer::file && ! player.getFile().existsAsFile())
            chooseFile(); // nothing loaded yet: ask for a file first
        else
            player.setSource (source);

        refresh();
    }

    void chooseFile()
    {
        const auto start = player.getFile().existsAsFile() ? player.getFile()
                                                           : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        chooser = std::make_unique<juce::FileChooser> ("Choose an audio file to loop (a clean DI guitar recording works best)",
                                                       start, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

        juce::Component::SafePointer<SourceBar> safeThis (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safeThis] (const juce::FileChooser& fc)
                              {
                                  if (safeThis == nullptr || fc.getResult() == juce::File())
                                      return;

                                  const auto error = safeThis->player.loadFile (fc.getResult());
                                  if (error.isNotEmpty())
                                      juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOk (
                                                                        juce::MessageBoxIconType::WarningIcon, "Audio file", error),
                                                                    nullptr);
                                  safeThis->refresh();
                              });
    }

    TestSignalPlayer& player;

    ChipButton liveButton { "LIVE INPUT", colours::ledGreen, 0x5C },
               demoButton { "DEMO RIFF", colours::amber, 0x5C },
               fileButton { "AUDIO FILE", juce::Colour (0xff4fb3ff), 0x5C };
    juce::TextButton transportButton { "STOP" }, loadButton { "LOAD FILE..." };
    std::vector<std::unique_ptr<juce::TextButton>> stringButtons, chordButtons;
    juce::ComboBox patternBox;
    juce::Slider tempoBox;
    juce::TextButton stopChordButton { "STOP" };
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<float> inputLabelArea, pluckLabelArea, chordLabelArea, statusArea;
    juce::String statusText;
    float progress = -1.0f;
};

} // namespace ui
