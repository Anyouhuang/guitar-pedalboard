#include "PluginEditor.h"

namespace
{
constexpr int editorWidth  = 1000;
constexpr int editorHeight = 872;
constexpr int margin       = 16;
} // namespace

PedalboardEditor::PedalboardEditor (PedalboardProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    setLookAndFeel (&lookAndFeel);

    using namespace ParamIDs;
    auto& state = processor.apvts;

    chainStrip = std::make_unique<ui::ChainStrip> (processor);
    slotEditor = std::make_unique<ui::SlotEditor> (processor);
    chainStrip->onSelect = [this] (int position) { slotEditor->showPosition (position); };
    slotEditor->onMoved  = [this] (int position) { chainStrip->setSelected (position); };
    addAndMakeVisible (*chainStrip);
    addAndMakeVisible (*slotEditor);

    eqPanel = std::make_unique<ui::EqPanel> (state);
    addAndMakeVisible (*eqPanel);

    sourceBar = std::make_unique<ui::SourceBar> (processor.getTestSignal());
    addAndMakeVisible (*sourceBar);

    tuner.onClick = [this]
    {
        auto& param = processor.parameter (ParamIDs::tunerOn);
        param.beginChangeGesture();
        param.setValueNotifyingHost (param.getValue() > 0.5f ? 0.0f : 1.0f);
        param.endChangeGesture();
    };

    setUpHeaderKnob (inputKnob, inputLabel, "INPUT");
    setUpHeaderKnob (masterKnob, masterLabel, "MASTER");
    inputAttachment  = std::make_unique<SliderAttachment> (state, inputGain, inputKnob);
    masterAttachment = std::make_unique<SliderAttachment> (state, outputGain, masterKnob);

    cabButton.setTooltip ("The amp block (amp model + speaker cabinet + microphone). Keep the cabinet on for headphones / studio monitors; switch the block off when playing into a real guitar amp.");
    muteButton.setTooltip ("Silence the output, e.g. while tuning.");
    cabAttachment  = std::make_unique<ButtonAttachment> (state, cab, cabButton);
    muteAttachment = std::make_unique<ButtonAttachment> (state, mute, muteButton);

    for (auto* c : std::initializer_list<juce::Component*> { &tuner, &inputMeter, &outputMeter, &cabButton, &muteButton })
        addAndMakeVisible (c);

    setSize (editorWidth, editorHeight);
    startTimerHz (30);
}

PedalboardEditor::~PedalboardEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void PedalboardEditor::selectBlock (int position)
{
    chainStrip->setSelected (position);
    slotEditor->showPosition (position);
}

void PedalboardEditor::setUpHeaderKnob (juce::Slider& knob, juce::Label& label, const juce::String& text)
{
    ui::styleKnob (knob, ui::colours::amber);
    addAndMakeVisible (knob);

    label.setText (text, juce::dontSendNotification);
    label.setFont (ui::boldFont (12.0f));
    label.setColour (juce::Label::textColourId, ui::colours::dimText);
    label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (label);
}

//==============================================================================
void PedalboardEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::colours::board);

    // faint carpet texture on the board
    g.setColour (juce::Colours::white.withAlpha (0.015f));
    for (int y = 0; y < getHeight(); y += 3)
        g.drawHorizontalLine (y, 0.0f, (float) getWidth());

    const auto header = headerArea.toFloat();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2b2b31), 0.0f, header.getY(),
                                             juce::Colour (0xff17171b), 0.0f, header.getBottom(), false));
    g.fillRoundedRectangle (header, 12.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawRoundedRectangle (header.reduced (1.0f), 11.0f, 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (header, 12.0f, 1.2f);

    auto title = titleArea;
    g.setFont (ui::boldFont (26.0f));
    g.setColour (ui::colours::text);
    g.drawText ("GUITAR", title.removeFromTop (28.0f), juce::Justification::centredLeft);
    g.setColour (ui::colours::drive);
    g.drawText ("PEDALBOARD", title.removeFromTop (28.0f), juce::Justification::centredLeft);
    g.setFont (ui::plainFont (12.0f));
    g.setColour (ui::colours::dimText);
    g.drawFittedText ("8 FX slots + amp/cab, HD500X-style chain", title.removeFromTop (20.0f).toNearestInt(),
                      juce::Justification::centredLeft, 1, 0.7f);
}

void PedalboardEditor::resized()
{
    auto area = getLocalBounds().reduced (margin);
    headerArea = area.removeFromTop (124);
    area.removeFromTop (10);
    sourceBar->setBounds (area.removeFromTop (80));
    area.removeFromTop (12);

    // header: title | input | tuner | master | buttons
    auto header = headerArea.reduced (18, 10);
    titleArea = header.removeFromLeft (230).toFloat().withSizeKeepingCentre (230.0f, 76.0f);

    auto buttons = header.removeFromRight (104).withSizeKeepingCentre (104, 72);
    cabButton.setBounds (buttons.removeFromTop (32));
    muteButton.setBounds (buttons.removeFromBottom (32));
    header.removeFromRight (16);

    auto layoutKnob = [] (juce::Rectangle<int> section, juce::Slider& knob, juce::Label& label, ui::LevelMeter& meter)
    {
        meter.setBounds (section.removeFromRight (12).withSizeKeepingCentre (12, 84));
        section.removeFromRight (4);
        label.setBounds (section.removeFromTop (16));
        knob.setBounds (section);
    };

    layoutKnob (header.removeFromLeft (100), inputKnob, inputLabel, inputMeter);
    layoutKnob (header.removeFromRight (100), masterKnob, masterLabel, outputMeter);
    tuner.setBounds (header.withSizeKeepingCentre (juce::jmin (header.getWidth() - 40, 400), 92));

    // the signal chain, the selected block's editor, and the EQ rack unit underneath
    chainStrip->setBounds (area.removeFromTop (96));
    area.removeFromTop (10);
    slotEditor->setBounds (area.removeFromTop (262));
    area.removeFromTop (12);
    eqPanel->setBounds (area);
}

//==============================================================================
void PedalboardEditor::timerCallback()
{
    const double rate = processor.getSampleRate();
    if (rate > 0.0 && rate != tuner.getPreparedRate())
        tuner.prepare (rate);

    // switched off, the tuner costs nothing: the processor stops feeding it and no pitch detection runs
    tuner.setActive (processor.apvts.getRawParameterValue (ParamIDs::tunerOn)->load() > 0.5f);

    for (int n; (n = processor.readTunerSamples (audioScratch.data(), (int) audioScratch.size())) > 0;)
        if (tuner.isActive() && tuner.getPreparedRate() > 0.0)
            tuner.pushSamples (audioScratch.data(), n);

    if (tuner.isActive() && tuner.getPreparedRate() > 0.0)
        tuner.update();

    for (int n; (n = processor.readAnalyzerSamples (audioScratch.data(), (int) audioScratch.size())) > 0;)
        eqPanel->pushAnalyzerSamples (audioScratch.data(), n);

    eqPanel->update (rate);
    sourceBar->refresh();
    chainStrip->tick();
    slotEditor->tick();

    inputMeter.setPeak (processor.takeInputPeak());
    outputMeter.setPeak (processor.takeOutputPeak());
}
