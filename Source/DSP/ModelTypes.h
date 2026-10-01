#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <vector>

namespace fx
{
/** The board is an HD500X-style signal chain: eight FX slots plus the amp/cab block.
    Each slot holds one model and up to eight knobs. */
constexpr int numSlots = 8;
constexpr int maxKnobs = 8;

// Numbering is saved in test data and shared with the web version: only ever append.
enum class Category { none = 0, dynamics, distortion, modulation, delay, reverb, filter, pitch, eq, wah, volume,
                      amp, cab, // the amp block's own lists, not offered in the FX slots
                      numCategories };
enum class Engine   { none = 0, gate, distortion, modulation, delay, reverb, // the first engines (NoiseGate.h ... ReverbFx.h)
                      dynamicsFx, modFx, filterFx, pitchFx, eqFx, delayFx, reverbFx, wahFx, volumeFx, // Source/DSP/fx/*.h
                      ampFx, cabFx,
                      numEngines };
enum class Unit     { knob, percent, db, ms, hz, choice, semitones, freq };

inline const char* categoryName (Category c)
{
    constexpr const char* names[] = { "Empty", "Dynamics", "Distortion", "Modulation", "Delay", "Reverb",
                                      "Filter", "Pitch", "Preamp+EQ", "Wah", "Volume/Pan", "Amp", "Cab" };
    return names[(int) c];
}

/** The order the categories are listed in, as on the HD500X. */
inline constexpr Category categoryOrder[] = { Category::none, Category::dynamics, Category::distortion, Category::modulation,
                                              Category::filter, Category::pitch, Category::eq, Category::delay,
                                              Category::reverb, Category::volume, Category::wah };

//==============================================================================
/** One knob of a model, in plain units. The travel is skewed like juce::NormalisableRange:
    `centre` (when set) sits at the middle of the knob. */
struct KnobSpec
{
    const char* name = "";
    float min = 0.0f, max = 1.0f, def = 0.0f;
    float centre = 0.0f; // 0 = linear travel
    float step = 0.0f;
    Unit unit = Unit::knob;
    const char* const* choices = nullptr; // Unit::choice: one name per step (min = 0, max = count - 1)

    float skew() const noexcept
    {
        return centre > 0.0f ? std::log (0.5f) / std::log ((centre - min) / (max - min)) : 1.0f;
    }

    /** 0..1 knob travel -> value */
    float fromNorm (float n) const noexcept
    {
        float p = std::clamp (n, 0.0f, 1.0f);
        if (centre > 0.0f && p > 0.0f)
            p = std::exp (std::log (p) / skew());

        float v = min + (max - min) * p;
        if (step > 0.0f)
            v = min + step * std::round ((v - min) / step);
        return std::clamp (v, min, max);
    }

    /** value -> 0..1 knob travel */
    float toNorm (float v) const noexcept
    {
        const float p = std::clamp ((v - min) / (max - min), 0.0f, 1.0f);
        return centre > 0.0f ? std::pow (p, skew()) : p;
    }
};

// Knob constructors. The HD500X shows most parameters as 0..100 %, so percent() is the usual one.
inline KnobSpec knob10 (const char* name, float def)  { return { name, 0.0f, 10.0f, def, 0.0f, 0.1f, Unit::knob }; }
inline KnobSpec percent (const char* name, float def, float max = 100.0f) { return { name, 0.0f, max, def, 0.0f, 1.0f, Unit::percent }; }
inline KnobSpec decibels (const char* name, float min, float max, float def, float step = 0.1f) { return { name, min, max, def, 0.0f, step, Unit::db }; }
inline KnobSpec levelDb (float def = 0.0f) { return decibels ("Level", -30.0f, 12.0f, def); }
inline KnobSpec millis (const char* name, float min, float max, float def, float centre) { return { name, min, max, def, centre, 1.0f, Unit::ms }; }
inline KnobSpec hertz (const char* name, float min, float max, float def, float centre) { return { name, min, max, def, centre, 0.01f, Unit::hz }; }
/** Audio frequency, shown as "250 Hz" / "2.50 kHz". */
inline KnobSpec freq (const char* name, float min, float max, float def, float centre) { return { name, min, max, def, centre, 1.0f, Unit::freq }; }
inline KnobSpec semitones (const char* name, float min, float max, float def, float step = 1.0f) { return { name, min, max, def, 0.0f, step, Unit::semitones }; }
inline KnobSpec choice (const char* name, const char* const* names, int count, int def)
{
    return { name, 0.0f, (float) (count - 1), (float) def, 0.0f, 1.0f, Unit::choice, names };
}

//==============================================================================
/** Note values for tempo-synced times; index 0 means "use the milliseconds on the time knob". */
inline const char* const delayNoteNames[]  = { "ms", "1/4", "1/8.", "1/8", "1/8T", "1/16" };
inline constexpr double  delayNoteBeats[]  = { 0.0, 1.0, 0.75, 0.5, 1.0 / 3.0, 0.25 };
inline constexpr int     numDelayNotes     = 6;
inline const char* const reverbNoteNames[] = { "ms", "1/32", "1/16", "1/8", "1/4" };
inline constexpr double  reverbNoteBeats[] = { 0.0, 0.125, 0.25, 0.5, 1.0 };

struct ModelInfo
{
    const char* key;      // stable id: saved state, web version and tests use it
    const char* name;
    Category category;
    Engine engine;
    int variant;          // which model inside the engine
    const char* basedOn;  // the hardware the model imitates
    std::vector<KnobSpec> knobs;
    int timeKnob = -1, noteKnob = -1; // tempo sync: the time knob (ms) and its note-value choice
    const double* noteBeats = nullptr;
    bool stereo = false;  // first engines only: otherwise the slot sums to mono before the engine
    bool trails = false;  // delay / reverb: keeps ringing out after being switched off
    int timeKnob2 = -1, noteKnob2 = -1; // a second synced time (Stereo Delay's right side)
};

/** One slot of the chain, in plain units (the processor converts knob travel with KnobSpec::fromNorm). */
struct SlotParams
{
    bool on = false;
    int model = 0;
    std::array<float, maxKnobs> knobs {};
};

} // namespace fx
