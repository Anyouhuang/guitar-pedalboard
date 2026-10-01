#pragma once

#include <juce_core/juce_core.h>

namespace ParamIDs
{
inline constexpr auto inputGain   = "inGain";
inline constexpr auto outputGain  = "master";
inline constexpr auto mute        = "mute";
inline constexpr auto cab         = "cab";
inline constexpr auto ampPosition = "ampPos";
inline constexpr auto ampModel    = "ampModel"; // 0 = no amp, else 1 + index in fx::amps()
inline constexpr auto cabModel    = "cabModel"; // 0 = no cab, else 1 + index in fx::cabs()
inline constexpr auto tempo       = "tempo";

inline constexpr auto eqOn        = "eqOn";
inline constexpr auto eqLowCut    = "eqLcFreq";
inline constexpr auto eqBassFreq  = "eqBassFreq";
inline constexpr auto eqBassGain  = "eqBassGain";
inline constexpr auto eqLoMidFreq = "eqLmFreq";
inline constexpr auto eqLoMidGain = "eqLmGain";
inline constexpr auto eqLoMidQ    = "eqLmQ";
inline constexpr auto eqHiMidFreq = "eqHmFreq";
inline constexpr auto eqHiMidGain = "eqHmGain";
inline constexpr auto eqHiMidQ    = "eqHmQ";
inline constexpr auto eqTrebFreq  = "eqTrebFreq";
inline constexpr auto eqTrebGain  = "eqTrebGain";
inline constexpr auto eqHighCut   = "eqHcFreq";

inline constexpr auto noiseHum    = "nrHum";    // mains-hum filter: Off / 50 Hz / 60 Hz
inline constexpr auto noiseAmount = "nrAmount"; // hiss reduction, 0..100 %
inline constexpr auto tunerOn     = "tunerOn";

/** FX slots: "s1On", "s1Model", "s1K1" .. "s1K8" (slots and knobs count from 1). Knobs are stored as
    0..1 travel; what they mean depends on the slot's model (see fx::models()). */
inline juce::String slotOn (int slot)              { return "s" + juce::String (slot + 1) + "On"; }
inline juce::String slotModel (int slot)           { return "s" + juce::String (slot + 1) + "Model"; }
inline juce::String slotKnob (int slot, int knob)  { return "s" + juce::String (slot + 1) + "K" + juce::String (knob + 1); }

/** The amp block's knobs ("ampK1".., "cabK1"..), also stored as 0..1 travel; their meaning follows the amp / cab model. */
inline juce::String ampKnob (int knob) { return "ampK" + juce::String (knob + 1); }
inline juce::String cabKnob (int knob) { return "cabK" + juce::String (knob + 1); }
} // namespace ParamIDs
