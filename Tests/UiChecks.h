// UI interaction checks for the plugin editor, run without a window or an audio device:  UiSnapshot check
//
// Mouse events are built by hand and handed to the component under the pointer and then to the slider that
// listens to it, which is the order JUCE delivers them in. Nothing moves the real mouse.

#pragma once

#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace uichecks
{
inline int failures = 0;

inline void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (! ok)
        ++failures;
}

template <typename Type, typename Predicate>
Type* findChild (juce::Component& root, Predicate&& matches)
{
    for (auto* child : root.getChildren())
    {
        if (auto* typed = dynamic_cast<Type*> (child))
            if (matches (*typed))
                return typed;
        if (auto* deeper = findChild<Type> (*child, matches))
            return deeper;
    }
    return nullptr;
}

inline juce::MouseEvent mouseEvent (juce::Component& target, juce::Point<float> position, juce::Point<float> downPosition,
                                    int clicks, bool dragged)
{
    const auto now = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys::leftButtonModifier,
             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
             juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY,
             &target, &target, now, downPosition, now, clicks, dragged };
}

/** Presses at `from` and releases at `to` (the same point = a plain click) on a bar-style slider. */
inline void pressAndRelease (juce::Slider& slider, juce::Point<float> from, juce::Point<float> to, int clicks = 1)
{
    // the slider's text box covers a bar-style slider: it gets the events first and the slider listens to it
    auto* under = slider.getComponentAt (from.toInt());
    auto& target = under != nullptr ? *under : static_cast<juce::Component&> (slider);
    const bool viaChild = &target != &slider;
    const bool dragged = from != to;

    const auto down = mouseEvent (target, from, from, clicks, false);
    target.mouseDown (down);
    if (viaChild) slider.mouseDown (down);

    if (clicks == 2)
    {
        target.mouseDoubleClick (down);
        if (viaChild) slider.mouseDoubleClick (down);
    }

    if (dragged)
    {
        const auto drag = mouseEvent (target, to, from, clicks, true);
        target.mouseDrag (drag);
        if (viaChild) slider.mouseDrag (drag);
    }

    const auto up = mouseEvent (target, to, from, clicks, dragged);
    target.mouseUp (up);
    if (viaChild) slider.mouseUp (up);
}

/** True if a click on the slider's text box opens a text field (which, on a bar-style slider, covers the bar).
    Asked of the text box itself: without a window its click-to-edit never fires, so it can't be observed here. */
inline bool clickOpensTextField (juce::Slider& slider)
{
    auto* box = findChild<juce::Label> (slider, [] (juce::Label&) { return true; });
    return box != nullptr && (box->isEditableOnSingleClick() || box->isEditableOnDoubleClick());
}

/** Level in dB of the last part of `seconds` of quiet hiss sent through the whole plugin. */
inline float hissLevelDb (PedalboardProcessor& processor, double seconds)
{
    const double rate = 48000.0;
    const int block = 256;
    processor.setPlayConfigDetails (1, 2, rate, block);
    processor.prepareToPlay (rate, block);

    juce::Random random (1234);
    juce::AudioBuffer<float> buffer (2, block);
    juce::MidiBuffer midi;
    const int blocks = (int) (seconds * rate / block);
    double sum = 0.0;
    int counted = 0;

    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < block; ++i)
            buffer.setSample (0, i, 0.0005f * (random.nextFloat() * 2.0f - 1.0f)); // hiss around -70 dBFS
        buffer.clear (1, 0, block);
        processor.processBlock (buffer, midi);

        if (b >= blocks / 2) // after everything has settled
            for (int i = 0; i < block; ++i)
            {
                sum += (double) buffer.getSample (0, i) * buffer.getSample (0, i);
                ++counted;
            }
    }

    processor.releaseResources();
    return juce::Decibels::gainToDecibels ((float) std::sqrt (sum / std::max (1, counted)), -160.0f);
}

/** The preset bank, through its bar in the editor: recall, save, save as, rename, and what is kept in the state. */
inline void runPresetChecks()
{
    std::cout << "Preset bank\n";

    PedalboardProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    auto* pedalboard = dynamic_cast<PedalboardEditor*> (editor.get());
    auto* bank = pedalboard != nullptr ? findChild<ui::PresetBar> (*editor, [] (ui::PresetBar&) { return true; }) : nullptr;
    check (bank != nullptr, "found the preset bar");
    if (bank == nullptr)
        return;

    const auto settle = [&] { for (int i = 0; i < 7; ++i) pedalboard->refresh(); }; // the bar compares the board every 6th refresh
    const auto tile = [&] (int number)
    {
        return findChild<juce::Button> (*bank, [number] (juce::Button& b) { return b.getName() == "Preset " + juce::String (number); });
    };
    const auto button = [&] (const juce::String& name)
    {
        return findChild<juce::TextButton> (*bank, [&name] (juce::TextButton& b) { return b.getName() == name; });
    };
    const auto click = [&] (juce::Button* b) { if (b != nullptr) b->onClick(); settle(); };
    const auto value = [&] (const juce::String& id) { return processor.apvts.getRawParameterValue (id)->load(); };
    const auto setNorm = [&] (const juce::String& id, float normalised) { processor.parameter (id).setValueNotifyingHost (normalised); settle(); };
    const auto names = [&] (PedalboardProcessor& p)
    {
        juce::StringArray list;
        for (int i = 0; i < PedalboardProcessor::numPresets; ++i)
            list.add (p.isPresetEmpty (i) ? juce::String ("-") : p.getPresetName (i));
        return list.joinIntoString (", ");
    };
    const auto ampKey = [] (PedalboardProcessor& p) { return p.getAmpModel() >= 0 ? juce::String (fx::amps()[(size_t) p.getAmpModel()].key) : juce::String(); };
    const auto slotKey = [] (PedalboardProcessor& p, int s) { return juce::String (fx::modelInfo (p.getSlotModel (s)).key); };
    const auto knob1 = ParamIDs::slotKnob (0, 0);
    settle();

    // ---- what a new bank holds
    check (names (processor) == "Default, Clean, Crunch, Lead, Ambient, Acoustic, -, -", "a new bank: " + names (processor));
    check (processor.getCurrentPreset() == 0 && ! processor.isPresetModified(), "preset 1 is the board as it opens, and it is the current one");
    check (tile (1) != nullptr && tile (8) != nullptr && tile (1)->getButtonText() == "Default" && tile (8)->getButtonText() == "+",
           "eight tiles: named ones, and \"+\" on the empty ones");
    check (bank->getHeight() == 44 && bank->getY() > 0, "the bar is one row (" + juce::String (bank->getHeight()) + " px)");

    // ---- recalling: the whole board changes, the guitar / room settings do not
    setNorm (ParamIDs::inputGain, 0.7f);
    setNorm (ParamIDs::outputGain, 0.3f);
    setNorm (ParamIDs::mute, 1.0f);
    setNorm (ParamIDs::noiseAmount, 0.4f);
    setNorm (ParamIDs::noiseHum, 1.0f);
    setNorm (ParamIDs::tunerOn, 0.0f);
    setNorm (ParamIDs::tempo, processor.parameter (ParamIDs::tempo).convertTo0to1 (96.0f));
    const float in = value (ParamIDs::inputGain), out = value (ParamIDs::outputGain), denoise = value (ParamIDs::noiseAmount),
                hum = value (ParamIDs::noiseHum);
    check (! processor.isPresetModified(), "input, master, MUTE, tuner, noise settings and the TAP tempo do not count as changes to a preset");

    click (tile (3));
    check (processor.getCurrentPreset() == 2 && ampKey (processor) == "brit_j_45_normal" && slotKey (processor, 0) == "noise_gate"
               && slotKey (processor, 1) == "screamer" && ! processor.isSlotOn (1) && processor.getAmpPosition() == 2,
           "clicking Crunch recalls it: amp " + ampKey (processor) + ", slot 2 " + slotKey (processor, 1));
    check (! processor.isPresetModified(), "a preset that was just recalled is not marked as changed");

    click (tile (5));
    check (processor.getCurrentPreset() == 4 && ampKey (processor) == "class_a_30_tb" && slotKey (processor, 2) == "ping_pong"
               && std::abs (value (ParamIDs::eqLowCut) - 90.0f) < 0.5f,
           "clicking Ambient recalls its amp, effects and EQ (low cut " + juce::String (value (ParamIDs::eqLowCut), 0) + " Hz)");
    check (value (ParamIDs::inputGain) == in && value (ParamIDs::outputGain) == out && value (ParamIDs::mute) > 0.5f
               && value (ParamIDs::noiseAmount) == denoise && value (ParamIDs::noiseHum) == hum && value (ParamIDs::tunerOn) < 0.5f,
           "recalling presets left input, master, MUTE, tuner and noise settings alone");
    check (std::abs (processor.getTempo() - 96.0f) < 0.05f, "and the TAP tempo: still " + juce::String (processor.getTempo(), 0) + " BPM after two recalls");

    click (tile (6));
    check (processor.getCurrentPreset() == 5 && slotKey (processor, 0) == "acoustic_sim" && processor.isSlotOn (0)
               && processor.getAmpModel() < 0 && processor.getCabModel() < 0 && value (ParamIDs::cab) < 0.5f && ! processor.isPresetModified(),
           "clicking Acoustic: the acoustic simulator first, the amp block empty and off");
    click (tile (5));

    // ---- changing something marks the preset; SAVE keeps it; without SAVE it is dropped
    setNorm (knob1, 0.9f);
    check (processor.isPresetModified() && tile (5)->getTooltip().contains ("not saved"), "turning a knob marks the current preset as changed");
    click (button ("SAVE"));
    check (! processor.isPresetModified() && processor.getCurrentPreset() == 4, "SAVE stores the board in the current preset");

    setNorm (knob1, 0.2f);
    click (tile (2));
    click (tile (5));
    check (std::abs (value (knob1) - 0.9f) < 1.0e-4f, "a change that was not saved is gone after recalling another preset and coming back (knob at "
                                                           + juce::String (value (knob1), 2) + ", saved 0.90)");

    setNorm (knob1, 0.3f);
    click (tile (5));
    check (std::abs (value (knob1) - 0.9f) < 1.0e-4f && ! processor.isPresetModified(), "clicking the current preset goes back to its saved settings");

    // ---- an empty preset: clicking it stores the board there
    click (tile (7));
    check (processor.getCurrentPreset() == 6 && processor.getPresetName (6) == "Preset 7" && ! processor.isPresetModified()
               && ampKey (processor) == "class_a_30_tb" && tile (7)->getButtonText() == "Preset 7",
           "clicking an empty preset stores the board there: " + names (processor));

    // ---- SAVE AS: the next preset clicked is overwritten instead of recalled
    click (tile (2)); // Clean
    click (button ("SAVE AS"));
    check (button ("SAVE AS")->getButtonText() == "CANCEL", "SAVE AS waits for a preset to be picked");
    click (tile (3));
    check (processor.getCurrentPreset() == 2 && processor.getPresetName (2) == "Crunch" && ampKey (processor) == "blackface_double_normal"
               && button ("SAVE AS")->getButtonText() == "SAVE AS" && ! processor.isPresetModified(),
           "SAVE AS, then Crunch: Crunch now holds the Clean board and keeps its name");
    click (tile (4));
    click (tile (3));
    check (ampKey (processor) == "blackface_double_normal", "and that is what Crunch recalls from now on");

    click (button ("SAVE AS"));
    click (button ("SAVE AS")); // CANCEL
    click (tile (4));
    check (processor.getCurrentPreset() == 3 && ampKey (processor) == "brit_j_800", "after CANCEL a click recalls again (Lead: " + ampKey (processor) + ")");

    // ---- RENAME
    auto* nameEditor = findChild<juce::TextEditor> (*bank, [] (juce::TextEditor&) { return true; });
    check (nameEditor != nullptr && ! nameEditor->isVisible(), "the name field is hidden until RENAME");
    if (nameEditor != nullptr)
    {
        const auto solo = juce::String::fromUTF8 ("Solo 獨奏");
        click (button ("RENAME"));
        check (nameEditor->isVisible() && nameEditor->getText() == "Lead" && nameEditor->getBounds() == tile (4)->getBounds(),
               "RENAME opens a name field on the current preset");
        nameEditor->setText (solo, false);
        nameEditor->onReturnKey();
        settle();
        check (processor.getPresetName (3) == solo && tile (4)->getButtonText() == solo && ! nameEditor->isVisible(),
               "Enter keeps the new name, also in Chinese (" + juce::String (processor.getPresetName (3).length()) + " characters)");

        click (button ("RENAME"));
        nameEditor->setText ("Something else", false);
        nameEditor->onEscapeKey();
        settle();
        check (processor.getPresetName (3) == solo && ! nameEditor->isVisible(), "Esc leaves the name as it was");

        processor.renamePreset (3, "   ");
        processor.renamePreset (7, "Nothing here");
        check (processor.getPresetName (3) == solo && processor.isPresetEmpty (7), "a blank name is refused, an empty preset cannot be named");
        processor.renamePreset (1, "A name that is far too long for a tile");
        check (processor.getPresetName (1).length() == PedalboardProcessor::maxPresetNameLength,
               "names are cut to " + juce::String (PedalboardProcessor::maxPresetNameLength) + " characters: \"" + processor.getPresetName (1) + "\"");
        check (! processor.isPresetModified(), "renaming is not a change to the sound");
    }

    // ---- the bank is part of the saved state
    juce::MemoryBlock saved;
    processor.getStateInformation (saved);
    {
        PedalboardProcessor reopened;
        reopened.setStateInformation (saved.getData(), (int) saved.getSize());
        check (names (reopened) == names (processor) && reopened.getCurrentPreset() == processor.getCurrentPreset(),
               "saving and reopening keeps the bank: " + names (reopened));
        check (! reopened.isPresetModified() && ampKey (reopened) == ampKey (processor), "and the board, still matching its preset");

        bool sameContents = true;
        for (int i = 0; i < PedalboardProcessor::numPresets; ++i)
        {
            if (processor.isPresetEmpty (i))
                continue;
            processor.loadPreset (i);
            reopened.loadPreset (i);
            for (auto* p : reopened.getParameters())
                sameContents = sameContents && std::abs (p->getValue() - processor.getParameters()[p->getParameterIndex()]->getValue()) < 1.0e-5f;
        }
        check (sameContents, "every preset recalls the same board after reopening");

        juce::MemoryBlock again;
        reopened.getStateInformation (again);
        const auto xml = juce::AudioProcessor::getXmlFromBinary (again.getData(), (int) again.getSize());
        int banks = 0;
        if (xml != nullptr)
            for (auto* child : xml->getChildWithTagNameIterator ("PRESETS"))
                banks += child != nullptr ? 1 : 0;
        check (banks == 1, "the state holds the bank once, however often it is saved and reopened (" + juce::String (banks) + ")");
    }

    // ---- a state from before the preset bank: its board must not be lost
    {
        const auto strip = [] (const juce::MemoryBlock& state)
        {
            juce::MemoryBlock old;
            if (auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize()))
            {
                xml->deleteAllChildElementsWithTagName ("PRESETS");
                juce::AudioProcessor::copyXmlToBinary (*xml, old);
            }
            return old;
        };

        PedalboardProcessor tweaked;
        tweaked.setAmpModel (fx::findIn (fx::amps(), "treadplate"));
        tweaked.setSlotModel (5, fx::findModel ("plate"));
        juce::MemoryBlock state;
        tweaked.getStateInformation (state);
        const auto old = strip (state);

        PedalboardProcessor upgraded;
        upgraded.setStateInformation (old.getData(), (int) old.getSize());
        check (names (upgraded) == "My Board, Clean, Crunch, Lead, Ambient, Acoustic, -, -" && upgraded.getCurrentPreset() == 0,
               "an older state: its board becomes preset 1 (" + names (upgraded) + ")");
        check (ampKey (upgraded) == "treadplate" && ! upgraded.isPresetModified(), "the older board is on the board, unchanged");
        upgraded.loadPreset (1);
        upgraded.loadPreset (0);
        check (ampKey (upgraded) == "treadplate" && slotKey (upgraded, 5) == "plate" && ! upgraded.isPresetModified(),
               "and it comes back after trying an example");

        PedalboardProcessor untouched;
        juce::MemoryBlock plain;
        untouched.getStateInformation (plain);
        const auto oldPlain = strip (plain);
        PedalboardProcessor upgradedPlain;
        upgradedPlain.setStateInformation (oldPlain.getData(), (int) oldPlain.getSize());
        check (upgradedPlain.getPresetName (0) == "Default", "an older state with the default board: preset 1 is called Default");
    }

    // ---- a bank saved by 1.4.0: each preset stored a tempo, and the bank did not count the examples it had
    {
        const auto as140 = [] (const juce::MemoryBlock& state, bool full)
        {
            juce::MemoryBlock old;
            if (auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize()))
            {
                if (auto* bank = xml->getChildByName ("PRESETS"))
                {
                    bank->removeAttribute ("examples");
                    int index = 0;
                    for (auto* preset : bank->getChildIterator())
                    {
                        if (index == 5 && ! full)
                            preset->removeAllAttributes(); // 1.4.0 had four examples: this one was still empty
                        else if (preset->hasAttribute ("filled"))
                            preset->setAttribute ("tempo", 100.0 + index);
                        ++index;
                    }
                }
                juce::AudioProcessor::copyXmlToBinary (*xml, old);
            }
            return old;
        };

        PedalboardProcessor source;
        source.savePreset (6); // the user's own, in 7
        juce::MemoryBlock state;
        source.getStateInformation (state);

        const auto old = as140 (state, false);
        PedalboardProcessor upgraded;
        upgraded.setStateInformation (old.getData(), (int) old.getSize());
        check (names (upgraded) == "Default, Clean, Crunch, Lead, Ambient, Acoustic, Preset 7, -",
               "a bank from 1.4.0 gets the new Acoustic example in its first empty preset: " + names (upgraded));
        check (upgraded.getCurrentPreset() == 6 && ! upgraded.isPresetModified(), "its current preset is still the current one, unchanged");

        upgraded.parameter (ParamIDs::tempo).setValueNotifyingHost (upgraded.parameter (ParamIDs::tempo).convertTo0to1 (140.0f));
        const float tempoBefore = upgraded.getTempo();
        upgraded.loadPreset (4);
        upgraded.loadPreset (2);
        check (std::abs (upgraded.getTempo() - tempoBefore) < 0.01f && ! upgraded.isPresetModified(),
               "the tempo its presets had stored is ignored: " + juce::String (upgraded.getTempo(), 0) + " BPM before and after recalling two");

        juce::MemoryBlock again;
        upgraded.getStateInformation (again);
        PedalboardProcessor reopened;
        reopened.setStateInformation (again.getData(), (int) again.getSize());
        check (names (reopened) == names (upgraded), "saved and reopened, nothing is added a second time: " + names (reopened));

        // every preset in use: nothing is replaced
        source.savePreset (7);
        juce::MemoryBlock fullState;
        source.getStateInformation (fullState);
        const auto oldFull = as140 (fullState, true);
        PedalboardProcessor upgradedFull;
        upgradedFull.setStateInformation (oldFull.getData(), (int) oldFull.getSize());
        check (names (upgradedFull) == "Default, Clean, Crunch, Lead, Ambient, Acoustic, Preset 7, Preset 8",
               "a full bank is left exactly as it was: " + names (upgradedFull));
    }

    // ---- recalling while the audio runs
    {
        const double rate = 48000.0;
        const int block = 128;
        processor.setPlayConfigDetails (1, 2, rate, block);
        processor.prepareToPlay (rate, block);
        processor.parameter (ParamIDs::mute).setValueNotifyingHost (0.0f);
        processor.getTestSignal().setSource (TestSignalPlayer::live);

        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        bool finite = true;
        double energy = 0.0;
        float peak = 0.0f;
        for (int b = 0; b < 1500; ++b) // 4 seconds
        {
            if (b % 60 == 30)
                processor.loadPreset ((b / 60) % 5);

            for (int i = 0; i < block; ++i)
                buffer.setSample (0, i, 0.25f * (float) std::sin (2.0 * 3.14159265358979 * 196.0 * (double) (b * block + i) / rate));
            buffer.clear (1, 0, block);
            processor.processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < block; ++i)
                {
                    const float s = buffer.getSample (ch, i);
                    finite = finite && std::isfinite (s);
                    peak = std::max (peak, std::abs (s));
                    energy += (double) s * s;
                }
        }
        processor.releaseResources();
        check (finite && energy > 1.0 && peak < 1.99f, "recalling a preset every 160 ms while a note plays: sound all the way, peak "
                                                          + juce::String (juce::Decibels::gainToDecibels (peak), 1) + " dBFS");
    }
}

inline int run()
{
    PedalboardProcessor processor;
    auto& signal = processor.getTestSignal();
    signal.setSource (TestSignalPlayer::live);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    auto* pedalboard = dynamic_cast<PedalboardEditor*> (editor.get());
    if (pedalboard == nullptr)
    {
        std::cout << "no editor\n";
        return 1;
    }
    pedalboard->refresh();

    const auto denoise = [&] { return processor.apvts.getRawParameterValue (ParamIDs::noiseAmount)->load(); };

    //==============================================================================
    std::cout << "Denoise bar\n";
    auto* bar = findChild<juce::Slider> (*editor, [] (juce::Slider& s) { return s.getSliderStyle() == juce::Slider::LinearBar; });
    check (bar != nullptr, "found the bar-style slider");
    if (bar == nullptr)
        return 1;

    const auto at = [bar] (float fraction) { return juce::Point<float> ((float) bar->getWidth() * fraction, (float) bar->getHeight() * 0.5f); };
    check (bar->isShowing() || (bar->isVisible() && bar->getWidth() > 60 && bar->getHeight() > 12),
           "it is visible, " + juce::String (bar->getWidth()) + " x " + juce::String (bar->getHeight()) + " px");
    check (denoise() < 0.5f, "starts off");

    pressAndRelease (*bar, at (0.6f), at (0.6f));
    check (std::abs (denoise() - 60.0f) <= 3.0f, "a click at 60 % of the bar sets about 60 %  (got " + juce::String (denoise(), 1) + ")");
    check (! clickOpensTextField (*bar), "a click does not open a text field over the bar");
    check (bar->getTextFromValue (bar->getValue()).startsWith ("Denoise 6"), "the bar reads \"" + bar->getTextFromValue (bar->getValue()) + "\"");

    pressAndRelease (*bar, at (0.6f), at (0.25f));
    check (std::abs (denoise() - 25.0f) <= 3.0f, "dragging to 25 % sets about 25 %  (got " + juce::String (denoise(), 1) + ")");

    pressAndRelease (*bar, at (0.9f), at (0.9f));
    pressAndRelease (*bar, at (0.9f), at (0.9f), 2);
    check (denoise() < 0.5f, "a double-click switches it off  (got " + juce::String (denoise(), 1) + ")");

    // for reference: what the bar did before (text box editable), reproduced with the same fake mouse
    bar->setTextBoxIsEditable (true);
    pressAndRelease (*bar, at (0.6f), at (0.6f));
    std::cout << "  (as it was before, text box editable: the same click gives " << juce::String (denoise(), 1) << " % and "
              << (clickOpensTextField (*bar) ? "opens a text field" : "opens no text field") << ")\n";
    bar->setTextBoxIsEditable (false);
    bar->setValue (0.0, juce::sendNotificationSync);

    //==============================================================================
    std::cout << "Denoise sound, through the whole plugin (empty board, quiet hiss in)\n";
    for (int s = 0; s < fx::numSlots; ++s)
        processor.setSlotModel (s, 0);
    processor.setAmpModel (-1);
    processor.setCabModel (-1);

    bar->setValue (0.0, juce::sendNotificationSync);
    const float offDb = hissLevelDb (processor, 2.0);
    pressAndRelease (*bar, at (0.6f), at (0.6f));
    const float onDb = hissLevelDb (processor, 2.0);
    check (onDb < offDb - 6.0f, "hiss with Denoise off " + juce::String (offDb, 1) + " dB, at "
                                    + juce::String (denoise(), 0) + " % " + juce::String (onDb, 1) + " dB");
    bar->setValue (0.0, juce::sendNotificationSync);

    //==============================================================================
    std::cout << "Header\n";
    check (findChild<juce::Button> (*editor, [] (juce::Button& b) { return b.getButtonText().containsIgnoreCase ("CAB"); }) == nullptr,
           "no AMP / CAB button");
    check (findChild<juce::Button> (*editor, [] (juce::Button& b) { return b.getButtonText() == "MUTE" && b.isVisible(); }) != nullptr,
           "MUTE is still there");

    //==============================================================================
    std::cout << "Input bar\n";
    auto* bar2 = findChild<ui::SourceBar> (*editor, [] (ui::SourceBar&) { return true; });
    check (bar2 != nullptr, "found the input bar");
    if (bar2 == nullptr)
        return 1;

    const auto visibleButton = [bar2] (const juce::String& text)
    {
        return findChild<juce::Button> (*bar2, [&] (juce::Button& b) { return b.getButtonText() == text && b.isVisible(); }) != nullptr;
    };
    const auto pluckAndChordShown = [&] { return visibleButton ("E") || visibleButton ("Am") || visibleButton ("G"); };
    const auto chipsShown = [&] { return visibleButton ("LIVE INPUT") && visibleButton ("DEMO RIFF") && visibleButton ("AUDIO FILE"); };

    check (chipsShown() && ! pluckAndChordShown(), "LIVE INPUT: only the source buttons, no PLUCK / CHORD");
    check (bar2->getHeight() == 44, "LIVE INPUT: the bar is one row (" + juce::String (bar2->getHeight()) + " px)");
    const int eqTopLive = bar->getScreenY();

    signal.setSource (TestSignalPlayer::demo);
    pedalboard->refresh();
    check (chipsShown() && pluckAndChordShown(), "DEMO RIFF: PLUCK and CHORD are back");
    check (bar2->getHeight() == 80, "DEMO RIFF: the bar is two rows (" + juce::String (bar2->getHeight()) + " px)");
    check (bar->getScreenY() == eqTopLive, "the Denoise bar stays where it was when the input bar changes height");

    // a chord loop must not keep playing with its STOP button hidden
    processor.setPlayConfigDetails (1, 2, 48000.0, 256);
    processor.prepareToPlay (48000.0, 256);
    juce::AudioBuffer<float> scratch (2, 256);
    juce::MidiBuffer midi;
    const auto runOneBlock = [&] { scratch.clear(); processor.processBlock (scratch, midi); pedalboard->refresh(); };

    signal.playChord (0);
    runOneBlock();
    check (signal.getLoopingChord() == 0, "a chord loop is running");
    if (auto* live = findChild<juce::Button> (*bar2, [] (juce::Button& b) { return b.getButtonText() == "LIVE INPUT"; }))
        live->onClick();
    runOneBlock();
    check (signal.getSource() == TestSignalPlayer::live && signal.getLoopingChord() < 0, "choosing LIVE INPUT stops a chord loop");
    processor.releaseResources();
    check (! pluckAndChordShown() && bar2->getHeight() == 44, "and the bar is one row again");

    check (findChild<ui::PresetBar> (*editor, [] (ui::PresetBar&) { return true; }) != nullptr
               && findChild<ui::PresetBar> (*editor, [] (ui::PresetBar&) { return true; })->getY() > bar2->getBottom(),
           "the preset bar sits under the input bar");

    runPresetChecks();

    std::cout << "Acoustic simulator\n";
    {
        PedalboardProcessor acoustic;
        const int model = fx::findModel ("acoustic_sim");
        check (model == fx::numModels() - 1, "it is the last model of the list (older saved boards keep their models)");
        acoustic.setSlotModel (5, model);
        std::unique_ptr<juce::AudioProcessorEditor> acousticEditor (acoustic.createEditor());
        if (auto* board = dynamic_cast<PedalboardEditor*> (acousticEditor.get()))
        {
            board->selectBlock (6); // slot 6 sits behind the amp block
            board->refresh();
        }
        const auto listed = [&] (const juce::String& text)
        {
            return findChild<juce::ComboBox> (*acousticEditor, [&text] (juce::ComboBox& box)
            {
                for (int i = 0; i < box.getNumItems(); ++i)
                    if (box.getItemText (i) == text)
                        return true;
                return false;
            }) != nullptr;
        };
        check (acoustic.isSlotOn (5) && listed ("Acoustic Sim"), "a slot holds it, and the model list offers Acoustic Sim");
        check (listed ("Jumbo") && listed ("Piezo"), "its Type is a list: Standard, Jumbo, Enhanced, Piezo");
    }

    std::cout << (failures == 0 ? "ALL UI CHECKS PASSED" : "UI CHECKS FAILED: " + juce::String (failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
} // namespace uichecks
