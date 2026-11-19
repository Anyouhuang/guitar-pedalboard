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

    std::cout << (failures == 0 ? "ALL UI CHECKS PASSED" : "UI CHECKS FAILED: " + juce::String (failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
} // namespace uichecks
