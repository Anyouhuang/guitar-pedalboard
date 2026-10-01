#pragma once

// JUCE-free test harness for one effect engine (see docs/engine-contract.md).
//
// A test program includes the engine header and calls harness::run<Engine> (...). For every model it
//   - renders a test riff at the default knobs and writes the result as reference data for the JavaScript port
//   - checks the level, NaN/Inf, the knob extremes, random knob changes, silence, other sample rates,
//     that reset() really clears everything, and the speed
// Output: test_output/standalone/<category>/  (input.f32, <key>.f32, <key>_auto.f32, manifest.json)
//
// Build and run:  powershell -ExecutionPolicy Bypass -File Tests\standalone\build.ps1 -Name <category>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <pmmintrin.h>
#include <xmmintrin.h>

#include "../../Source/DSP/DspUtils.h"
#include "../../Source/DSP/ModelTypes.h"

namespace harness
{
constexpr double testRate = 48000.0;
constexpr int blockSize = 256;

constexpr int maxKnobs = 12; // slots use at most fx::maxKnobs (8); the amp has 11
using Knobs = std::array<float, maxKnobs>;
struct Step { int atSample; Knobs knobs; };

struct Options
{
    bool wetOnly = false;               // delay / reverb engines return the wet signal only
    float minLevelDb = -20.0f;          // allowed output level vs the input, at the default knobs
    float maxLevelDb = 12.0f;
    float maxPeak = 8.0f;               // at any knob setting (the input peaks at 0.4)
    float minRealtimeFactor = 8.0f;     // C++ speed of one engine
    float maxSilencePeak = 1.0e-3f;     // output allowed with no input (raise only for models that hum by design)
    int maxKnobs = fx::maxKnobs;        // per model
    std::vector<std::string> mayBeQuiet; // keys of models that may fall below minLevelDb (say why in the test)
};

/** Deterministic random numbers, identical in the JavaScript tests. */
struct Lcg
{
    uint32_t state;
    float next01() noexcept { state = state * 1664525u + 1013904223u; return (float) (state >> 8) * (1.0f / 16777216.0f); }
};

//==============================================================================
/** A plucked-string riff (power chord, single-note line, one long note), then silence from 4.0 s,
    so tails and "does it go quiet" can be seen. Peaks at 0.4 like a guitar on an audio interface. */
inline std::vector<float> makeInput (double rate = testRate, double seconds = 5.0)
{
    Lcg rng { 1234u };
    std::vector<float> riff ((size_t) (rate * seconds), 0.0f);

    auto pluck = [&] (double start, double hz, float amplitude)
    {
        const int period = std::max (2, (int) std::lround (rate / hz));
        std::vector<float> string ((size_t) period);
        for (auto& s : string)
            s = amplitude * (rng.next01() * 2.0f - 1.0f);

        float previous = 0.0f;
        for (int i = (int) (start * rate), k = 0; i < (int) riff.size(); ++i, ++k)
        {
            const size_t idx = (size_t) (k % period);
            const float current = string[idx];
            string[idx] = 0.996f * 0.5f * (current + previous);
            previous = current;
            riff[(size_t) i] += current;
        }
    };

    const double chord[] = { 82.41, 123.47, 164.81 };
    for (int i = 0; i < 3; ++i)
        pluck (0.10 + 0.012 * i, chord[i], 0.30f);

    const double line[] = { 146.83, 164.81, 196.00, 220.00, 246.94, 293.66, 329.63 };
    for (int i = 0; i < 7; ++i)
        pluck (1.2 + 0.2 * i, line[i], 0.35f);

    pluck (2.8, 110.0, 0.40f);

    // pickup hum and hiss, about -70 dBFS
    for (size_t i = 0; i < riff.size(); ++i)
        riff[i] += 0.0003f * (rng.next01() * 2.0f - 1.0f) + 0.0002f * (float) std::sin (2.0 * fx::pi * 60.0 * (double) i / rate);

    float peak = 0.0f;
    for (auto s : riff) peak = std::max (peak, std::abs (s));
    for (auto& s : riff) s *= 0.4f / peak;

    // the player stops: 50 ms fade at 4.0 s, then nothing at all
    const size_t stop = (size_t) (4.0 * rate), fade = (size_t) (0.05 * rate);
    for (size_t i = stop; i < riff.size(); ++i)
        riff[i] *= i < stop + fade ? 1.0f - (float) (i - stop) / (float) fade : 0.0f;

    return riff;
}

struct Stats
{
    float peak = 0.0f, rms = 0.0f;
    bool finite = true;
};

inline Stats measure (const std::vector<float>& x)
{
    Stats s;
    double sum = 0.0;
    for (float v : x)
    {
        s.finite = s.finite && std::isfinite (v);
        s.peak = std::max (s.peak, std::abs (v));
        sum += (double) v * v;
    }
    s.rms = (float) std::sqrt (sum / (double) std::max<size_t> (1, x.size()));
    return s;
}

inline float toDb (float gain) { return gain > 1.0e-9f ? 20.0f * std::log10 (gain) : -180.0f; }

/** Runs the engine the way a slot does: model, knobs, reset, then blocks of `blockSize` with the knobs
    set before each block. Mono input on both sides; returns interleaved stereo. */
template <typename Fx>
std::vector<float> render (Fx& effect, int variant, const std::vector<float>& input, const Knobs& knobs,
                           const std::vector<Step>& schedule = {})
{
    effect.setModel (variant);
    effect.setParameters (knobs.data());
    effect.reset();

    std::vector<float> out (input.size() * 2);
    float left[blockSize], right[blockSize];

    for (int pos = 0; pos < (int) input.size(); pos += blockSize)
    {
        const int n = std::min (blockSize, (int) input.size() - pos);
        const Knobs* k = &knobs;
        for (const auto& s : schedule)
            if (pos >= s.atSample)
                k = &s.knobs;

        effect.setParameters (k->data());
        std::copy (input.begin() + pos, input.begin() + pos + n, left);
        std::copy (input.begin() + pos, input.begin() + pos + n, right);
        effect.process (left, right, n);

        for (int i = 0; i < n; ++i)
        {
            out[(size_t) (pos + i) * 2]     = left[i];
            out[(size_t) (pos + i) * 2 + 1] = right[i];
        }
    }
    return out;
}

//==============================================================================
inline std::string jsonString (const char* text)
{
    std::string out = "\"";
    for (const char* c = text; *c != 0; ++c)
    {
        if (*c == '"' || *c == '\\') out += '\\';
        out += *c;
    }
    return out + "\"";
}

inline std::string jsonNumber (double v)
{
    char buffer[40];
    std::snprintf (buffer, sizeof (buffer), "%.9g", v);
    return buffer;
}

inline std::string jsonKnobs (const Knobs& k)
{
    std::string out = "[";
    for (size_t i = 0; i < k.size(); ++i)
        out += (i ? "," : "") + jsonNumber (k[i]);
    return out + "]";
}

inline std::string jsonModel (const fx::ModelInfo& m)
{
    std::string out = "{\"key\":" + jsonString (m.key) + ",\"name\":" + jsonString (m.name)
                    + ",\"category\":" + std::to_string ((int) m.category) + ",\"engine\":" + std::to_string ((int) m.engine)
                    + ",\"variant\":" + std::to_string (m.variant) + ",\"basedOn\":" + jsonString (m.basedOn)
                    + ",\"timeKnob\":" + std::to_string (m.timeKnob) + ",\"noteKnob\":" + std::to_string (m.noteKnob)
                    + ",\"timeKnob2\":" + std::to_string (m.timeKnob2) + ",\"noteKnob2\":" + std::to_string (m.noteKnob2)
                    + ",\"stereo\":" + (m.stereo ? "true" : "false") + ",\"trails\":" + (m.trails ? "true" : "false") + ",\"knobs\":[";
    for (size_t k = 0; k < m.knobs.size(); ++k)
    {
        const auto& spec = m.knobs[k];
        out += std::string (k ? "," : "") + "{\"name\":" + jsonString (spec.name) + ",\"min\":" + jsonNumber (spec.min)
             + ",\"max\":" + jsonNumber (spec.max) + ",\"def\":" + jsonNumber (spec.def) + ",\"centre\":" + jsonNumber (spec.centre)
             + ",\"step\":" + jsonNumber (spec.step) + ",\"unit\":" + std::to_string ((int) spec.unit) + ",\"choices\":[";
        if (spec.unit == fx::Unit::choice)
            for (int c = 0; c <= (int) spec.max; ++c)
                out += std::string (c ? "," : "") + jsonString (spec.choices[c]);
        out += "]}";
    }
    return out + "]}";
}

inline void writeFloats (const std::filesystem::path& file, const std::vector<float>& data)
{
    std::ofstream out (file, std::ios::binary);
    out.write ((const char*) data.data(), (std::streamsize) (data.size() * sizeof (float)));
}

inline Knobs defaultKnobs (const fx::ModelInfo& m)
{
    Knobs k {};
    for (size_t i = 0; i < m.knobs.size(); ++i)
        k[i] = m.knobs[i].def;
    return k;
}

//==============================================================================
/** Tests every model of one engine and writes the reference data. Returns the number of failed checks. */
template <typename Fx>
int run (const std::string& category, const std::vector<fx::ModelInfo>& models, const std::filesystem::path& outDir,
         const Options& options = {})
{
    _MM_SET_FLUSH_ZERO_MODE (_MM_FLUSH_ZERO_ON);          // as in the plugin (ScopedNoDenormals)
    _MM_SET_DENORMALS_ZERO_MODE (_MM_DENORMALS_ZERO_ON);

    int failures = 0;
    auto check = [&] (bool ok, const std::string& what)
    {
        if (! ok)
        {
            ++failures;
            std::printf ("    FAIL: %s\n", what.c_str());
        }
    };

    std::filesystem::create_directories (outDir);
    const auto input = makeInput();
    writeFloats (outDir / "input.f32", input);
    const float inputRms = measure (input).rms;

    std::string modelsJson, casesJson;
    Fx shared; // used for every model in turn, like a slot that gets another model picked
    shared.prepare (testRate, blockSize);

    std::printf ("== %s: %d models ==\n", category.c_str(), (int) models.size());
    std::printf ("  model                  level   peak    44.1k   96k   speed\n");

    for (size_t index = 0; index < models.size(); ++index)
    {
        const auto& m = models[index];
        const std::string key = m.key;
        const Knobs defaults = defaultKnobs (m);
        check ((int) m.knobs.size() <= options.maxKnobs, key + " has too many knobs");
        check (m.variant == (int) index, key + ": variant must equal its position in the model list");

        // default knobs on a fresh engine: the reference
        Fx fresh;
        fresh.prepare (testRate, blockSize);
        const auto started = std::chrono::steady_clock::now();
        const auto reference = render (fresh, m.variant, input, defaults);
        const double elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();
        const double speed = ((double) input.size() / testRate) / std::max (1.0e-9, elapsed);
        const auto stats = measure (reference);
        writeFloats (outDir / (key + ".f32"), reference);

        check (stats.finite, key + " produced NaN/Inf");
        check (stats.peak < options.maxPeak, key + " is far too loud at its default knobs");
        const float level = toDb (stats.rms / inputRms);
        const bool quietAllowed = std::find (options.mayBeQuiet.begin(), options.mayBeQuiet.end(), key) != options.mayBeQuiet.end();
        if (! options.wetOnly)
            check ((level >= options.minLevelDb || quietAllowed) && level <= options.maxLevelDb,
                   key + ": level at default knobs is " + jsonNumber (level) + " dB vs the input");
        else
            check (stats.rms > 1.0e-5f || quietAllowed, key + " is silent");
        check (speed >= options.minRealtimeFactor, key + " is too slow: " + jsonNumber (speed) + "x realtime");

        // the same through the shared engine: reset() must leave nothing behind from the previous model
        const auto again = render (shared, m.variant, input, defaults);
        check (again.size() == reference.size() && std::memcmp (again.data(), reference.data(), reference.size() * sizeof (float)) == 0,
               key + ": reset() / setModel() leave state behind (output differs from a fresh engine)");

        // every knob at both ends
        for (size_t k = 0; k < m.knobs.size(); ++k)
            for (float value : { m.knobs[k].min, m.knobs[k].max })
            {
                Knobs knobs = defaults;
                knobs[k] = value;
                const auto s = measure (render (shared, m.variant, input, knobs));
                check (s.finite && s.peak < options.maxPeak,
                       key + ": knob " + m.knobs[k].name + " = " + jsonNumber (value) + " gives peak " + jsonNumber (s.peak));
            }

        // knobs jumping around while playing (also a reference for the JavaScript port)
        std::vector<Step> schedule;
        Lcg rng { 99u + (uint32_t) index };
        for (int at = (int) (0.2 * testRate); at < (int) input.size(); at += (int) (0.2 * testRate))
        {
            Step step { at, defaults };
            for (size_t k = 0; k < m.knobs.size(); ++k)
                step.knobs[k] = m.knobs[k].fromNorm (rng.next01());
            schedule.push_back (step);
        }
        const auto automated = render (shared, m.variant, input, defaults, schedule);
        const auto autoStats = measure (automated);
        writeFloats (outDir / (key + "_auto.f32"), automated);
        check (autoStats.finite && autoStats.peak < options.maxPeak, key + ": random knob changes give peak " + jsonNumber (autoStats.peak));

        // silence in, silence out
        const std::vector<float> silence ((size_t) (2.0 * testRate), 0.0f);
        const auto quiet = measure (render (shared, m.variant, silence, defaults));
        check (quiet.finite && quiet.peak < options.maxSilencePeak, key + " makes sound (" + jsonNumber (quiet.peak) + ") with no input");

        // other sample rates
        float otherLevels[2] {};
        int r = 0;
        for (double rate : { 44100.0, 96000.0 })
        {
            Fx other;
            other.prepare (rate, blockSize);
            const auto otherInput = makeInput (rate);
            const auto s = measure (render (other, m.variant, otherInput, defaults));
            otherLevels[r] = toDb (s.rms / measure (otherInput).rms);
            check (s.finite && s.peak < options.maxPeak, key + " misbehaves at " + jsonNumber (rate) + " Hz");
            if (stats.rms > 1.0e-5f)
                check (std::abs (otherLevels[r] - level) < 6.0f, key + ": level at " + jsonNumber (rate) + " Hz differs by "
                                                                   + jsonNumber (otherLevels[r] - level) + " dB from 48 kHz");
            ++r;
        }

        std::printf ("  %-20s %7.1f %6.1f  %7.1f %6.1f %6.0fx\n", key.c_str(), level, toDb (stats.peak), otherLevels[0], otherLevels[1], speed);

        modelsJson += (index ? ",\n    " : "") + jsonModel (m);
        std::string extra;
        if constexpr (requires (Fx& f) { f.getMix(); f.getTailSeconds(); })
        {
            render (shared, m.variant, silence, defaults);
            extra = ",\"mix\":" + jsonNumber (shared.getMix()) + ",\"tailSeconds\":" + jsonNumber (shared.getTailSeconds());
            check (shared.getMix() >= 0.0f && shared.getMix() <= 1.0f && shared.getTailSeconds() > 0.0f, key + ": getMix() / getTailSeconds() out of range");
        }

        casesJson += std::string (index ? ",\n    " : "") + "{\"name\":" + jsonString (key.c_str()) + ",\"variant\":" + std::to_string (m.variant)
                   + ",\"knobs\":" + jsonKnobs (defaults) + extra + "},\n    {\"name\":" + jsonString ((key + "_auto").c_str())
                   + ",\"variant\":" + std::to_string (m.variant) + ",\"knobs\":" + jsonKnobs (defaults) + ",\"schedule\":[";
        for (size_t i = 0; i < schedule.size(); ++i)
            casesJson += std::string (i ? "," : "") + "{\"atSample\":" + std::to_string (schedule[i].atSample) + ",\"knobs\":" + jsonKnobs (schedule[i].knobs) + "}";
        casesJson += "]}";
    }

    std::ofstream manifest (outDir / "manifest.json");
    manifest << "{\n  \"category\": " << jsonString (category.c_str()) << ",\n  \"sampleRate\": " << testRate
             << ",\n  \"blockSize\": " << blockSize << ",\n  \"wetOnly\": " << (options.wetOnly ? "true" : "false")
             << ",\n  \"models\": [\n    " << modelsJson << "\n  ],\n  \"cases\": [\n    " << casesJson << "\n  ]\n}\n";

    std::printf ("%s\n", failures == 0 ? "ALL CHECKS PASSED" : (std::to_string (failures) + " CHECK(S) FAILED").c_str());
    return failures;
}

} // namespace harness
