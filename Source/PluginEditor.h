#pragma once

#include "PluginProcessor.h"
#include "UI/EqPanel.h"
#include "UI/PedalLookAndFeel.h"
#include "UI/ChainStrip.h"
#include "UI/SlotEditor.h"
#include "UI/SourceBar.h"
#include "UI/TunerDisplay.h"
#include "UI/Widgets.h"

class PedalboardEditor : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit PedalboardEditor (PedalboardProcessor&);
    ~PedalboardEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Shows the block at chain position 0..8 in the editor panel (as clicking it does). */
    void selectBlock (int position);

    /** Runs one UI refresh now (normally the 30 Hz timer does). */
    void refresh() { timerCallback(); }

private:
    void timerCallback() override;
    void setUpHeaderKnob (juce::Slider&, juce::Label&, const juce::String& text);

    PedalboardProcessor& processor;
    ui::PedalLookAndFeel lookAndFeel; // declared first so it outlives every component that uses it

    juce::Rectangle<int> headerArea;
    juce::Rectangle<float> titleArea;

    ui::TunerDisplay tuner;
    ui::LevelMeter inputMeter, outputMeter;
    juce::Slider inputKnob, masterKnob;
    juce::Label inputLabel, masterLabel;
    ui::LedButton cabButton { "AMP / CAB", ui::colours::ledGreen };
    ui::LedButton muteButton { "MUTE", ui::colours::ledRed };

    std::unique_ptr<ui::ChainStrip> chainStrip;
    std::unique_ptr<ui::SlotEditor> slotEditor;
    std::unique_ptr<ui::EqPanel> eqPanel;
    std::unique_ptr<ui::SourceBar> sourceBar;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SliderAttachment> inputAttachment, masterAttachment;
    std::unique_ptr<ButtonAttachment> cabAttachment, muteAttachment;

    std::vector<float> audioScratch = std::vector<float> (8192);
    juce::TooltipWindow tooltips { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PedalboardEditor)
};
