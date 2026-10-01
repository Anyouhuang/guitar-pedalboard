#pragma once

#include <juce_core/juce_core.h>
#include "DspUtils.h"

namespace fx
{
/** Standard-tuning open strings, low E to high e. */
inline constexpr float openStringHz[6] = { 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f };

inline float fretHz (int string, int fret) { return openStringHz[string] * std::pow (2.0f, (float) fret / 12.0f); }

/** Open-position chord shapes, frets from low E to high e; -1 = string not played (muted). */
struct Chord
{
    const char* name;
    const char* shape; // the usual chord-chart spelling, e.g. "x32010"
    int frets[6];
};

inline constexpr Chord basicChords[] = {
    { "C",  "x32010", { -1, 3, 2, 0, 1, 0 } },
    { "D",  "xx0232", { -1, -1, 0, 2, 3, 2 } },
    { "E",  "022100", { 0, 2, 2, 1, 0, 0 } },
    { "F",  "133211", { 1, 3, 3, 2, 1, 1 } },
    { "G",  "320003", { 3, 2, 0, 0, 0, 3 } },
    { "A",  "x02220", { -1, 0, 2, 2, 2, 0 } },
    { "Am", "x02210", { -1, 0, 2, 2, 1, 0 } },
    { "Dm", "xx0231", { -1, -1, 0, 2, 3, 1 } },
    { "Em", "022000", { 0, 2, 2, 0, 0, 0 } },
};
inline constexpr int numBasicChords = (int) (sizeof (basicChords) / sizeof (basicChords[0]));

//==============================================================================
/** Karplus-Strong plucked string. A first-order all-pass supplies the fractional part of the loop
    delay, so the pitch is accurate to well under a cent and the tuner reads it correctly. */
class PluckedString
{
public:
    void prepare (double sampleRate, float lowestHz = 40.0f)
    {
        fs = sampleRate;
        buffer.assign ((size_t) (fs / lowestHz) + 4, 0.0f);
        active = false;
    }

    /** t60 = seconds to decay by 60 dB (short = palm mute), brightness 0..1 = pick attack. */
    void pluck (float hz, float velocity, float t60, float brightness, juce::Random& rng) noexcept
    {
        const double period = fs / hz;
        length = juce::jlimit (2, (int) buffer.size(), (int) std::floor (period - 0.6));
        const double fraction = period - 0.5 - length; // the 2-point average adds half a sample
        allpassCoef = (float) ((1.0 - fraction) / (1.0 + fraction));
        loopGain = (float) std::pow (10.0, -3.0 * period / (fs * t60));

        OnePole pick;
        pick.setCutoff (fs, 700.0 + 9000.0 * brightness);
        float mean = 0.0f;
        for (int i = 0; i < length; ++i)
        {
            buffer[(size_t) i] = pick.lowPass (rng.nextFloat() * 2.0f - 1.0f);
            mean += buffer[(size_t) i];
        }
        mean /= (float) length;

        float peak = 1.0e-6f;
        for (int i = 0; i < length; ++i)
            peak = std::max (peak, std::abs (buffer[(size_t) i] -= mean));
        for (int i = 0; i < length; ++i)
            buffer[(size_t) i] *= velocity / peak;

        pos = 0;
        previous = allpassIn = allpassOut = 0.0f;
        remaining = (int) (fs * t60 * 1.2);
        active = true;
    }

    bool isActive() const noexcept { return active; }

    /** Mutes a ringing string over ~60 ms, like resting a finger on it (no click). */
    void damp() noexcept
    {
        if (! active)
            return;
        loopGain = (float) std::pow (10.0, -3.0 * length / (fs * 0.06));
        remaining = std::min (remaining, (int) (fs * 0.1));
    }

    float next() noexcept
    {
        if (! active)
            return 0.0f;

        const float out = buffer[(size_t) pos];
        const float averaged = 0.5f * (out + previous) * loopGain;
        previous = out;

        const float y = allpassCoef * averaged + allpassIn - allpassCoef * allpassOut;
        allpassIn = averaged;
        allpassOut = y;

        buffer[(size_t) pos] = y;
        if (++pos >= length)
            pos = 0;

        if (--remaining <= 0)
            active = false;

        return out;
    }

private:
    double fs = 48000.0;
    std::vector<float> buffer;
    int length = 2, pos = 0, remaining = 0;
    float loopGain = 0.99f, allpassCoef = 0.0f, previous = 0.0f, allpassIn = 0.0f, allpassOut = 0.0f;
    bool active = false;
};

//==============================================================================
/** A ~10 s rock riff for testing without a guitar: palm-muted chugs, power chords, a pentatonic
    lick and a ringing open-E chord with room for delay/reverb tails. Loops seamlessly.
    Peaks around -8 dBFS, like a DI'd guitar through an audio interface. */
inline std::vector<float> renderDemoRiff (double fs)
{
    constexpr double eighth = 0.3; // 100 BPM
    constexpr double loopSeconds = 32 * eighth + 1.2;

    struct Note { double at; float hz; float velocity, t60, brightness; double length; };
    std::vector<Note> notes;

    auto chord = [&] (double at, std::initializer_list<float> hz, bool muted, double length, float velocity = 0.33f)
    {
        int string = 0;
        for (float f : hz)
        {
            const double strumOffset = 0.009 * string; // a down-strum reaches each string a little later
            notes.push_back ({ at * eighth + strumOffset, f, velocity * (1.0f - 0.04f * (float) string),
                               muted ? 0.16f : 3.5f, muted ? 0.35f : 0.8f, muted ? eighth : length * eighth });
            ++string;
        }
    };

    const std::initializer_list<float> e5 { 82.41f, 123.47f, 164.81f },
                                       g5 { 98.00f, 146.83f, 196.00f },
                                       a5 { 110.00f, 164.81f, 220.00f },
                                       d5 { 146.83f, 220.00f, 293.66f },
                                       eMajor { 82.41f, 123.47f, 164.81f, 207.65f, 246.94f, 329.63f };

    // bar 1-2: chugs and power chords
    for (double t : { 0.0, 1.0, 2.0, 3.0 })  chord (t, e5, true, 1.0);
    chord (4.0, e5, false, 2.0);
    chord (6.0, g5, false, 2.0);
    for (double t : { 8.0, 9.0, 10.0 })      chord (t, e5, true, 1.0);
    chord (11.0, a5, false, 2.0);
    chord (13.0, g5, false, 1.0);
    chord (14.0, d5, false, 2.0);

    // bar 3: E minor pentatonic lick
    const float lick[] = { 329.63f, 392.00f, 440.00f, 493.88f, 440.00f, 392.00f, 329.63f, 293.66f };
    for (int i = 0; i < 8; ++i)
        notes.push_back ({ (16 + i) * eighth, lick[i], 0.38f, 2.5f, 0.85f, (i == 6 ? 1.5 : 1.0) * eighth });

    // bar 4: open E major, left to ring
    chord (24.0, eMajor, false, 8.0 + 4.0, 0.3f);

    const int loopLength = (int) (loopSeconds * fs);
    std::vector<float> out ((size_t) loopLength + (size_t) (6.0 * fs), 0.0f);
    const int fade = (int) (0.025 * fs);

    juce::Random rng (2024);
    PluckedString string;
    string.prepare (fs);

    for (const auto& n : notes)
    {
        string.pluck (n.hz, n.velocity, n.t60, n.brightness, rng);
        const int start = (int) (n.at * fs);
        const int held  = (int) (n.length * fs);

        for (int i = 0; i < held + fade && start + i < (int) out.size() && string.isActive(); ++i)
        {
            const float env = i < held ? 1.0f : 1.0f - (float) (i - held) / (float) fade; // finger lifts off
            out[(size_t) (start + i)] += env * string.next();
        }
    }

    // fold the ringing tail back onto the start so the loop has no seam
    for (size_t i = (size_t) loopLength; i < out.size(); ++i)
        out[i - (size_t) loopLength] += out[i];
    out.resize ((size_t) loopLength);

    float peak = 1.0e-6f;
    for (auto s : out)
        peak = std::max (peak, std::abs (s));
    for (auto& s : out)
        s *= 0.4f / peak;

    return out;
}

} // namespace fx
