#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "KnobText.h"

namespace
{
using Attributes = juce::AudioParameterFloatAttributes;

std::unique_ptr<juce::AudioParameterFloat> floatParam (const juce::String& id, const juce::String& name,
                                                       juce::NormalisableRange<float> range, float defaultValue,
                                                       std::function<juce::String (float)> toText,
                                                       std::function<float (const juce::String&)> fromText = {})
{
    auto attributes = Attributes().withStringFromValueFunction ([toText] (float v, int) { return toText (v); });
    if (fromText)
        attributes = attributes.withValueFromStringFunction (fromText);

    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range, defaultValue, attributes);
}

std::unique_ptr<juce::AudioParameterBool> boolParam (const juce::String& id, const juce::String& name, bool defaultValue)
{
    return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, defaultValue);
}

juce::NormalisableRange<float> linear (float low, float high, float step = 0.0f) { return { low, high, step }; }

juce::NormalisableRange<float> skewed (float low, float high, float centre)
{
    juce::NormalisableRange<float> range (low, high);
    range.setSkewForCentre (centre);
    return range;
}

juce::String asDb (float v)       { return juce::String (v, 1) + " dB"; }
juce::String asQ (float v)        { return juce::String (v, 2); }
juce::String asBpm (float v)      { return juce::String (v, 1) + " BPM"; }
juce::String asSignedDb (float v) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }
juce::String asFreq (float v)     { return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz"
                                                        : juce::String (juce::roundToInt (v)) + " Hz"; }
float parseFreq (const juce::String& text)
{
    const auto v = text.getFloatValue();
    return text.containsIgnoreCase ("k") ? v * 1000.0f : v;
}

void storeMax (std::atomic<float>& target, float value)
{
    auto previous = target.load();
    while (value > previous && ! target.compare_exchange_weak (previous, value)) {}
}
} // namespace

//==============================================================================
PedalboardProcessor::PedalboardProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::mono(),   true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PEDALBOARD", createLayout())
{
    for (int s = 0; s < fx::numSlots; ++s)
    {
        slotOnValues[(size_t) s]    = apvts.getRawParameterValue (ParamIDs::slotOn (s));
        slotModelValues[(size_t) s] = apvts.getRawParameterValue (ParamIDs::slotModel (s));
        for (int k = 0; k < fx::maxKnobs; ++k)
            slotKnobValues[(size_t) s][(size_t) k] = apvts.getRawParameterValue (ParamIDs::slotKnob (s, k));
    }

    ampModelValue = apvts.getRawParameterValue (ParamIDs::ampModel);
    cabModelValue = apvts.getRawParameterValue (ParamIDs::cabModel);
    for (int k = 0; k < fx::maxAmpKnobs; ++k)
        ampKnobValues[(size_t) k] = apvts.getRawParameterValue (ParamIDs::ampKnob (k));
    for (int k = 0; k < fx::maxCabKnobs; ++k)
        cabKnobValues[(size_t) k] = apvts.getRawParameterValue (ParamIDs::cabKnob (k));

    chain.setAnalyzerTap (&analyzerTap);
}

juce::AudioProcessorValueTreeState::ParameterLayout PedalboardProcessor::createLayout()
{
    using namespace ParamIDs;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const auto board = fx::defaultBoard();

    layout.add (floatParam (inputGain,  "Input",  linear (-24.0f, 24.0f, 0.1f), 0.0f, asDb),
                floatParam (outputGain, "Master", linear (-40.0f, 12.0f, 0.1f), 0.0f, asDb),
                boolParam  (mute, "Mute", false),
                boolParam  (cab,  "Amp Block On", true),
                std::make_unique<juce::AudioParameterInt> (juce::ParameterID { ampPosition, 1 }, "Amp Position", 0, fx::numSlots, board.ampPosition),
                floatParam (tempo, "Tempo", linear (30.0f, 300.0f, 0.1f), 120.0f, asBpm));

    layout.add (boolParam  (eqOn,        "EQ On", true),
                floatParam (eqLowCut,    "EQ Low Cut",       skewed (20.0f, 600.0f, 100.0f),    20.0f,    asFreq, parseFreq),
                floatParam (eqBassFreq,  "EQ Bass Freq",     skewed (40.0f, 500.0f, 120.0f),    100.0f,   asFreq, parseFreq),
                floatParam (eqBassGain,  "EQ Bass Gain",     linear (-15.0f, 15.0f, 0.1f),         0.0f,     asSignedDb),
                floatParam (eqLoMidFreq, "EQ Low Mid Freq",  skewed (100.0f, 2000.0f, 450.0f),  400.0f,   asFreq, parseFreq),
                floatParam (eqLoMidGain, "EQ Low Mid Gain",  linear (-15.0f, 15.0f, 0.1f),         0.0f,     asSignedDb),
                floatParam (eqLoMidQ,    "EQ Low Mid Q",     skewed (0.3f, 6.0f, 1.0f),         1.0f,     asQ),
                floatParam (eqHiMidFreq, "EQ High Mid Freq", skewed (500.0f, 8000.0f, 2000.0f), 2000.0f,  asFreq, parseFreq),
                floatParam (eqHiMidGain, "EQ High Mid Gain", linear (-15.0f, 15.0f, 0.1f),         0.0f,     asSignedDb),
                floatParam (eqHiMidQ,    "EQ High Mid Q",    skewed (0.3f, 6.0f, 1.0f),         1.0f,     asQ),
                floatParam (eqTrebFreq,  "EQ Treble Freq",   skewed (1500.0f, 15000.0f, 5000.0f), 5000.0f, asFreq, parseFreq),
                floatParam (eqTrebGain,  "EQ Treble Gain",   linear (-15.0f, 15.0f, 0.1f),         0.0f,     asSignedDb),
                floatParam (eqHighCut,   "EQ High Cut",      skewed (1000.0f, 20000.0f, 6000.0f), 20000.0f, asFreq, parseFreq));

    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { noiseHum, 1 }, "Hum Filter",
                                                              juce::StringArray { "Off", "50 Hz", "60 Hz" }, 0),
                floatParam (noiseAmount, "Denoise", linear (0.0f, 100.0f, 1.0f), 0.0f,
                            [] (float v) { return v < 0.5f ? juce::String ("Off") : juce::String (juce::roundToInt (v)) + " %"; }),
                boolParam  (tunerOn, "Tuner On", true));

    // ---- the amp block: amp model, cabinet, and their knobs (stored as travel, like the slots')
    juce::StringArray ampNames { "No Amp" }, cabNames { "No Cab" };
    for (const auto& m : fx::amps()) ampNames.add (m.name);
    for (const auto& m : fx::cabs()) cabNames.add (m.name);

    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { ampModel, 1 }, "Amp Model", ampNames, board.amp.amp + 1),
                std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { cabModel, 1 }, "Cab Model", cabNames, board.amp.cab + 1));

    auto blockKnob = [&] (const juce::String& id, const juce::String& name, float def, std::function<const fx::KnobSpec* ()> spec)
    {
        layout.add (floatParam (id, name, linear (0.0f, 1.0f), def,
                                [spec] (float travel)
                                {
                                    const auto* k = spec();
                                    return k != nullptr ? fx::knobText (*k, k->fromNorm (travel)) : juce::String (travel, 3);
                                },
                                [spec] (const juce::String& text)
                                {
                                    const auto* k = spec();
                                    return k != nullptr ? k->toNorm (fx::knobValueFromText (*k, text)) : text.getFloatValue();
                                }));
    };

    const auto& firstAmp = fx::amps().front().knobs;
    for (int k = 0; k < fx::maxAmpKnobs; ++k)
        blockKnob (ampKnob (k), "Amp Knob " + juce::String (k + 1),
                   k < (int) firstAmp.size() ? firstAmp[(size_t) k].toNorm (firstAmp[(size_t) k].def) : 0.5f,
                   [this, k] { return ampKnobSpec (k); });

    const auto& defaultCab = fx::cabs()[(size_t) std::max (0, board.amp.cab)].knobs;
    for (int k = 0; k < fx::maxCabKnobs; ++k)
        blockKnob (cabKnob (k), "Cab Knob " + juce::String (k + 1),
                   k < (int) defaultCab.size() ? defaultCab[(size_t) k].toNorm (defaultCab[(size_t) k].def) : 0.5f,
                   [this, k] { return cabKnobSpec (k); });

    juce::StringArray modelNames;
    for (const auto& m : fx::models())
        modelNames.add (m.category == fx::Category::none ? juce::String (m.name)
                                                          : juce::String (fx::categoryName (m.category)) + ": " + m.name);

    for (int s = 0; s < fx::numSlots; ++s)
    {
        const auto& slot = board.slots[(size_t) s];
        const auto& info = fx::modelInfo (slot.model);
        const auto prefix = "Slot " + juce::String (s + 1) + " ";

        layout.add (boolParam (slotOn (s), prefix + "On", slot.on),
                    std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { slotModel (s), 1 }, prefix + "Model",
                                                                  modelNames, slot.model));

        for (int k = 0; k < fx::maxKnobs; ++k)
        {
            // Knobs are stored as travel (0..1); their text follows whatever model the slot holds.
            const float def = k < (int) info.knobs.size() ? info.knobs[(size_t) k].toNorm (slot.knobs[(size_t) k]) : 0.5f;
            layout.add (floatParam (slotKnob (s, k), prefix + "Knob " + juce::String (k + 1), linear (0.0f, 1.0f), def,
                                    [this, s, k] (float travel)
                                    {
                                        const auto* spec = knobSpec (s, k);
                                        return spec != nullptr ? fx::knobText (*spec, spec->fromNorm (travel)) : juce::String (travel, 3);
                                    },
                                    [this, s, k] (const juce::String& text)
                                    {
                                        const auto* spec = knobSpec (s, k);
                                        return spec != nullptr ? spec->toNorm (fx::knobValueFromText (*spec, text)) : text.getFloatValue();
                                    }));
        }
    }

    return layout;
}

const fx::KnobSpec* PedalboardProcessor::knobSpec (int slot, int knob) const
{
    const auto* model = slotModelValues[(size_t) slot];
    if (model == nullptr)
        return nullptr;

    const auto& knobs = fx::modelInfo ((int) std::lround (model->load())).knobs;
    return knob < (int) knobs.size() ? &knobs[(size_t) knob] : nullptr;
}

const fx::KnobSpec* PedalboardProcessor::ampKnobSpec (int knob) const
{
    if (ampModelValue == nullptr)
        return nullptr;

    const auto& knobs = fx::amps()[(size_t) std::max (0, getAmpModel())].knobs; // "no amp": the first amp's knobs still label them
    return knob < (int) knobs.size() ? &knobs[(size_t) knob] : nullptr;
}

const fx::KnobSpec* PedalboardProcessor::cabKnobSpec (int knob) const
{
    if (cabModelValue == nullptr)
        return nullptr;

    const auto& knobs = fx::cabs()[(size_t) std::max (0, getCabModel())].knobs;
    return knob < (int) knobs.size() ? &knobs[(size_t) knob] : nullptr;
}

fx::FxParams PedalboardProcessor::readParameters() const
{
    using namespace ParamIDs;
    auto value = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    auto on    = [&] (const char* id) { return value (id) > 0.5f; };

    fx::FxParams p;
    p.inputGainDb  = value (inputGain);
    p.outputGainDb = value (outputGain);
    p.mute  = on (mute);
    p.amp.on = on (cab);
    p.amp.amp = getAmpModel();
    p.amp.cab = getCabModel();

    if (p.amp.amp >= 0)
    {
        const auto& knobs = fx::amps()[(size_t) p.amp.amp].knobs;
        for (size_t k = 0; k < knobs.size(); ++k)
            p.amp.ampKnobs[k] = knobs[k].fromNorm (ampKnobValues[k]->load());
    }

    if (p.amp.cab >= 0)
    {
        const auto& knobs = fx::cabs()[(size_t) p.amp.cab].knobs;
        for (size_t k = 0; k < knobs.size(); ++k)
            p.amp.cabKnobs[k] = knobs[k].fromNorm (cabKnobValues[k]->load());
    }
    p.ampPosition = (int) std::lround (value (ampPosition));

    p.eqOn = on (eqOn);
    p.eq.lowCutHz  = value (eqLowCut);
    p.eq.bassHz    = value (eqBassFreq);
    p.eq.bassDb    = value (eqBassGain);
    p.eq.lowMidHz  = value (eqLoMidFreq);
    p.eq.lowMidDb  = value (eqLoMidGain);
    p.eq.lowMidQ   = value (eqLoMidQ);
    p.eq.highMidHz = value (eqHiMidFreq);
    p.eq.highMidDb = value (eqHiMidGain);
    p.eq.highMidQ  = value (eqHiMidQ);
    p.eq.trebleHz  = value (eqTrebFreq);
    p.eq.trebleDb  = value (eqTrebGain);
    p.eq.highCutHz = value (eqHighCut);
    p.humMode = (int) std::lround (value (noiseHum));
    p.denoise = value (noiseAmount);

    const double bpm = value (tempo);
    for (size_t s = 0; s < (size_t) fx::numSlots; ++s)
    {
        auto& slot = p.slots[s];
        slot.on = slotOnValues[s]->load() > 0.5f;
        slot.model = std::clamp ((int) std::lround (slotModelValues[s]->load()), 0, fx::numModels() - 1);

        const auto& knobs = fx::modelInfo (slot.model).knobs;
        for (size_t k = 0; k < knobs.size(); ++k)
            slot.knobs[k] = knobs[k].fromNorm (slotKnobValues[s][k]->load());

        fx::resolveTempo (slot, bpm);
    }
    return p;
}

//==============================================================================
PedalboardProcessor::Block PedalboardProcessor::getBlock (int position) const
{
    const int amp = getAmpPosition();
    if (position == amp)
        return { true, -1 };
    return { false, position < amp ? position : position - 1 };
}

int PedalboardProcessor::getAmpPosition() const
{
    return std::clamp ((int) std::lround (apvts.getRawParameterValue (ParamIDs::ampPosition)->load()), 0, fx::numSlots);
}

int PedalboardProcessor::getAmpModel() const
{
    return std::clamp ((int) std::lround (ampModelValue->load()), 0, (int) fx::amps().size()) - 1;
}

int PedalboardProcessor::getCabModel() const
{
    return std::clamp ((int) std::lround (cabModelValue->load()), 0, (int) fx::cabs().size()) - 1;
}

int PedalboardProcessor::getSlotModel (int slot) const
{
    return std::clamp ((int) std::lround (slotModelValues[(size_t) slot]->load()), 0, fx::numModels() - 1);
}

bool PedalboardProcessor::isSlotOn (int slot) const { return slotOnValues[(size_t) slot]->load() > 0.5f; }
float PedalboardProcessor::getTempo() const         { return apvts.getRawParameterValue (ParamIDs::tempo)->load(); }

void PedalboardProcessor::setParameter (juce::RangedAudioParameter& p, float plainValue)
{
    const float normalised = p.convertTo0to1 (plainValue);
    if (std::abs (p.getValue() - normalised) < 1.0e-7f)
        return;

    p.beginChangeGesture();
    p.setValueNotifyingHost (normalised);
    p.endChangeGesture();
}

void PedalboardProcessor::setSlotModel (int slot, int model)
{
    model = std::clamp (model, 0, fx::numModels() - 1);
    const bool wasEmpty = getSlotModel (slot) == 0;
    const auto& knobs = fx::modelInfo (model).knobs;

    setParameter (parameter (ParamIDs::slotModel (slot)), (float) model);
    for (int k = 0; k < fx::maxKnobs; ++k)
        setParameter (parameter (ParamIDs::slotKnob (slot, k)),
                      k < (int) knobs.size() ? knobs[(size_t) k].toNorm (knobs[(size_t) k].def) : 0.5f);

    if (model == 0)
        setParameter (parameter (ParamIDs::slotOn (slot)), 0.0f);
    else if (wasEmpty)
        setParameter (parameter (ParamIDs::slotOn (slot)), 1.0f);
}

void PedalboardProcessor::setAmpModel (int amp)
{
    amp = std::clamp (amp, -1, (int) fx::amps().size() - 1);
    if (amp == getAmpModel())
        return;

    setParameter (parameter (ParamIDs::ampModel), (float) (amp + 1));
    if (amp < 0)
        return;

    const auto& knobs = fx::amps()[(size_t) amp].knobs;
    for (int k = 0; k < (int) knobs.size(); ++k)
        setParameter (parameter (ParamIDs::ampKnob (k)), knobs[(size_t) k].toNorm (knobs[(size_t) k].def));

    // like on the HD500X, an amp brings the cabinet it is usually played through
    if (const int cab = fx::findIn (fx::cabs(), fx::defaultCabFor (fx::amps()[(size_t) amp].key)); cab >= 0)
        setCabModel (cab);
}

void PedalboardProcessor::setCabModel (int cab)
{
    cab = std::clamp (cab, -1, (int) fx::cabs().size() - 1);
    if (cab == getCabModel())
        return;

    setParameter (parameter (ParamIDs::cabModel), (float) (cab + 1));
    if (cab < 0)
        return;

    const auto& knobs = fx::cabs()[(size_t) cab].knobs;
    for (int k = 0; k < (int) knobs.size(); ++k)
        setParameter (parameter (ParamIDs::cabKnob (k)), knobs[(size_t) k].toNorm (knobs[(size_t) k].def));
}

void PedalboardProcessor::moveBlock (int from, int to)
{
    from = std::clamp (from, 0, numBlocks - 1);
    to   = std::clamp (to, 0, numBlocks - 1);
    if (from == to)
        return;

    struct SlotState { float on, model; std::array<float, fx::maxKnobs> knobs; };
    std::array<SlotState, fx::numSlots> saved {};
    for (int s = 0; s < fx::numSlots; ++s)
    {
        auto& st = saved[(size_t) s];
        st.on = slotOnValues[(size_t) s]->load();
        st.model = slotModelValues[(size_t) s]->load();
        for (int k = 0; k < fx::maxKnobs; ++k)
            st.knobs[(size_t) k] = slotKnobValues[(size_t) s][(size_t) k]->load();
    }

    std::vector<Block> order;
    for (int i = 0; i < numBlocks; ++i)
        order.push_back (getBlock (i));

    const auto moved = order[(size_t) from];
    order.erase (order.begin() + from);
    order.insert (order.begin() + to, moved);

    int slot = 0;
    for (int i = 0; i < numBlocks; ++i)
    {
        const auto& block = order[(size_t) i];
        if (block.isAmp)
        {
            setParameter (parameter (ParamIDs::ampPosition), (float) i);
            continue;
        }

        const auto& st = saved[(size_t) block.slot];
        setParameter (parameter (ParamIDs::slotModel (slot)), st.model);
        setParameter (parameter (ParamIDs::slotOn (slot)), st.on);
        for (int k = 0; k < fx::maxKnobs; ++k)
            setParameter (parameter (ParamIDs::slotKnob (slot, k)), st.knobs[(size_t) k]);
        ++slot;
    }
}

//==============================================================================
void PedalboardProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    testSignal.prepare (sampleRate);
    chain.prepare (sampleRate, samplesPerBlock);
    chain.setParameters (readParameters());
    chain.reset(); // start with the pedals already in their switched state

    // Each distortion slot adds its oversampling delay (about 0.25 ms) only while it is on,
    // so there is no fixed latency worth reporting to the host.
    setLatencySamples (0);
}

bool PedalboardProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const auto monoOrStereo = [] (const juce::AudioChannelSet& set)
    {
        return set == juce::AudioChannelSet::mono() || set == juce::AudioChannelSet::stereo();
    };
    return monoOrStereo (in) && monoOrStereo (out);
}

void PedalboardProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();

    if (numOut == 0)
    {
        buffer.clear();
        return;
    }

    // The guitar is mono: only the first input channel is used. With no input bus it's silence,
    // which the test signal (demo riff / file / plucked strings) can still replace.
    float* input = buffer.getWritePointer (0);
    if (numIn == 0)
        juce::FloatVectorOperations::clear (input, numSamples);

    testSignal.process (input, numSamples);
    if (apvts.getRawParameterValue (ParamIDs::tunerOn)->load() > 0.5f)
        tunerTap.push (input, numSamples);

    chain.setParameters (readParameters());
    chain.process (input,
                   buffer.getWritePointer (0),
                   numOut > 1 ? buffer.getWritePointer (1) : nullptr,
                   numSamples);

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    storeMax (inputPeak, chain.getInputPeak());
    storeMax (outputPeak, chain.getOutputPeak());
}

//==============================================================================
juce::AudioProcessorEditor* PedalboardProcessor::createEditor()
{
    return new PedalboardEditor (*this);
}

void PedalboardProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("chainVersion", 2, nullptr);
    state.setProperty ("inputSource", (int) testSignal.getSource(), nullptr);
    state.setProperty ("testFile", testSignal.getFile().getFullPathName(), nullptr);
    state.setProperty ("chordPattern", testSignal.getChordPattern(), nullptr);
    state.setProperty ("chordTempo", testSignal.getChordTempo(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PedalboardProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    // States from 1.x (fixed pedal row) have none of the slot parameters: keep the default board then.
    if (xml->getIntAttribute ("chainVersion", 1) >= 2)
        apvts.replaceState (juce::ValueTree::fromXml (*xml));

    testSignal.setChordPattern (xml->getIntAttribute ("chordPattern", TestSignalPlayer::arpeggio));
    testSignal.setChordTempo ((float) xml->getDoubleAttribute ("chordTempo", 90.0));

    const auto testFilePath = xml->getStringAttribute ("testFile");
    const auto testFile = juce::File::isAbsolutePath (testFilePath) ? juce::File (testFilePath) : juce::File();
    if (testFile.existsAsFile())
        testSignal.loadFile (testFile);

    const auto source = (TestSignalPlayer::Source) xml->getIntAttribute ("inputSource", TestSignalPlayer::live);
    testSignal.setSource (source == TestSignalPlayer::file && ! testFile.existsAsFile() ? TestSignalPlayer::live : source);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PedalboardProcessor();
}
