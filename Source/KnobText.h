#pragma once

#include <juce_core/juce_core.h>
#include "DSP/Models.h"

namespace fx
{
/** A knob value as text, e.g. "5.0", "35 %", "-3.0 dB", "380 ms", "0.80 Hz", "1/8.". */
inline juce::String knobText (const KnobSpec& spec, float value)
{
    switch (spec.unit)
    {
        case Unit::percent: return juce::String (juce::roundToInt (value)) + " %";
        case Unit::db:      return juce::String (value, 1) + " dB";
        case Unit::ms:      return juce::String (juce::roundToInt (value)) + " ms";
        case Unit::hz:      return juce::String (value, 2) + " Hz";
        case Unit::choice:  return spec.choices[juce::jlimit (0, (int) spec.max, juce::roundToInt (value))];
        case Unit::semitones:
            return (value > 0.0f ? "+" : "") + (spec.step >= 1.0f ? juce::String (juce::roundToInt (value)) : juce::String (value, 1)) + " st";
        case Unit::freq:
            return value >= 1000.0f ? juce::String (value / 1000.0f, value >= 10000.0f ? 1 : 2) + " kHz"
                                    : juce::String (juce::roundToInt (value)) + " Hz";
        case Unit::knob:    break;
    }
    return juce::String (value, 1);
}

/** Text typed into a knob's box -> value (choices by name or number). */
inline float knobValueFromText (const KnobSpec& spec, const juce::String& text)
{
    if (spec.unit == Unit::choice)
    {
        for (int i = 0; i <= (int) spec.max; ++i)
            if (text.trim().equalsIgnoreCase (spec.choices[i]))
                return (float) i;
    }
    const float number = text.getFloatValue();
    const bool kilo = spec.unit == Unit::freq && text.containsIgnoreCase ("k");
    return juce::jlimit (spec.min, spec.max, kilo ? number * 1000.0f : number);
}
} // namespace fx
