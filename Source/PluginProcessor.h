#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "DSP/FxChain.h"
#include "ParamIDs.h"
#include "TestSignalPlayer.h"

//==============================================================================
class PedalboardProcessor : public juce::AudioProcessor
{
public:
    PedalboardProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override    { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    /** Called from the editor's timer: raw input for the tuner, the output for the analyser. */
    int readTunerSamples (float* dest, int maxSamples)    { return tunerTap.pull (dest, maxSamples); }
    int readAnalyzerSamples (float* dest, int maxSamples) { return analyzerTap.pull (dest, maxSamples); }

    /** Built-in demo riff / audio file / plucked strings, for playing without a guitar. */
    TestSignalPlayer& getTestSignal() noexcept { return testSignal; }

    /** Peak levels since the last call (for the meters). */
    float takeInputPeak()  { return inputPeak.exchange (0.0f); }
    float takeOutputPeak() { return outputPeak.exchange (0.0f); }

    //==============================================================================
    // Signal chain editing (message thread). Chain positions run 0..numSlots: the eight slots
    // in order, with the amp/cab block inserted at its position.
    static constexpr int numBlocks = fx::numSlots + 1;

    struct Block
    {
        bool isAmp = false;
        int slot = -1;
    };

    Block getBlock (int position) const;
    int getAmpPosition() const;
    int getAmpModel() const; // index in fx::amps(), -1 = no amp
    int getCabModel() const; // index in fx::cabs(), -1 = no cab
    int getSlotModel (int slot) const;
    bool isSlotOn (int slot) const;
    float getTempo() const;

    /** Puts `model` in the slot with its knobs at their defaults (an empty slot is switched on). */
    void setSlotModel (int slot, int model);

    /** Picks the amp (its knobs go to their defaults, and the cab it is usually played through is selected) or the cab. */
    void setAmpModel (int amp);
    void setCabModel (int cab);

    /** Moves the block at chain position `from` to position `to`, shifting the others. */
    void moveBlock (int from, int to);

    juce::RangedAudioParameter& parameter (const juce::String& id) const { return *apvts.getParameter (id); }

    //==============================================================================
    // Preset bank (message thread): eight boards that can be recalled at any time, also while playing.
    // A preset holds the eight slots, the amp block, the chain order and the EQ. It does not hold the input /
    // master levels, MUTE, the tuner or the noise section (those belong to the guitar and the room, not to a
    // sound), nor the TAP tempo, which is one setting for the whole unit. The bank is saved with the state.
    static constexpr int numPresets = 8;
    static constexpr int maxPresetNameLength = 16;

    int getCurrentPreset() const noexcept { return currentPreset; }
    bool isPresetEmpty (int index) const;
    juce::String getPresetName (int index) const;

    /** True when the board no longer matches the current preset: something was changed and not saved. */
    bool isPresetModified() const;

    /** Recalls a preset: the whole board changes in one go. Empty presets are ignored. */
    void loadPreset (int index);

    /** Stores the board in a preset (it keeps its name, a new one is called "Preset N") and makes it the current one. */
    void savePreset (int index);

    void renamePreset (int index, const juce::String& name);

private:
    // Declared before apvts: the knob parameters' text functions read the slot models through these.
    std::array<std::atomic<float>*, fx::numSlots> slotOnValues {}, slotModelValues {};
    std::array<std::array<std::atomic<float>*, fx::maxKnobs>, fx::numSlots> slotKnobValues {};
    std::atomic<float>* ampModelValue = nullptr;
    std::atomic<float>* cabModelValue = nullptr;
    std::array<std::atomic<float>*, fx::maxAmpKnobs> ampKnobValues {};
    std::array<std::atomic<float>*, fx::maxCabKnobs> cabKnobValues {};

public:
    juce::AudioProcessorValueTreeState apvts;

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    const fx::KnobSpec* knobSpec (int slot, int knob) const;
    const fx::KnobSpec* ampKnobSpec (int knob) const;
    const fx::KnobSpec* cabKnobSpec (int knob) const;
    fx::FxParams readParameters() const;
    static void setParameter (juce::RangedAudioParameter&, float plainValue);

    /** The bank as it first is: the board as it is now (so recalling an example never loses it), then the examples. */
    void resetPresetBank();
    void storeBoard (juce::ValueTree preset, const juce::String& name) const;
    void storeFactoryPreset (juce::ValueTree preset, const fx::FactoryPreset&) const;

    /** Puts the examples this bank has not been offered yet (`alreadyOffered` of them it has) into empty presets. */
    void offerNewExamples (int alreadyOffered);
    bool boardIsDefault() const;

    fx::FxChain chain;
    fx::AudioTap tunerTap, analyzerTap;
    TestSignalPlayer testSignal;

    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };

    std::vector<juce::RangedAudioParameter*> presetParameters; // what a preset holds
    juce::ValueTree presetBank { "PRESETS" };                  // numPresets children, each a list of parameter id -> value
    int currentPreset = 0;
    std::atomic<bool> holdParameters { false }; // set while a preset is being recalled: the audio thread keeps the old board

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PedalboardProcessor)
};
