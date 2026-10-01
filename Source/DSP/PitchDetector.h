#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace fx
{
/** YIN pitch detector for the tuner. Input is decimated to ~24 kHz, which is plenty for guitar
    fundamentals (50 Hz .. 1.5 kHz) and keeps the analysis cheap enough for the message thread. */
class PitchDetector
{
public:
    void prepare (double inputSampleRate)
    {
        decimation = std::max (1, (int) std::lround (inputSampleRate / 24000.0));
        rate   = inputSampleRate / decimation;
        maxTau = (int) std::ceil (rate / minFrequency) + 2;
        minTau = std::max (2, (int) std::floor (rate / maxFrequency));
        window = (int) std::ceil (rate * 0.040);

        ring.assign ((size_t) (window + maxTau + 2), 0.0f);
        frame.assign (ring.size(), 0.0f);
        diff.assign ((size_t) maxTau + 2, 0.0);
        cmnd.assign ((size_t) maxTau + 2, 1.0);
        writePos = 0;
        accumulator = 0.0f;
        accumulated = 0;
    }

    void pushSamples (const float* data, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            accumulator += data[i];
            if (++accumulated == decimation)
            {
                ring[(size_t) writePos] = accumulator / (float) decimation;
                writePos = (writePos + 1) % (int) ring.size();
                accumulator = 0.0f;
                accumulated = 0;
            }
        }
    }

    /** Returns the fundamental frequency in Hz of the most recent audio, or 0 if there's no clear pitch. */
    float detect()
    {
        const int size = (int) ring.size();
        for (int i = 0; i < size; ++i)
            frame[(size_t) i] = ring[(size_t) ((writePos + i) % size)];

        const float* x = frame.data();

        double energy = 0.0;
        for (int j = 0; j < size; ++j)
            energy += (double) x[j] * x[j];

        if (std::sqrt (energy / size) < minRms)
            return 0.0f;

        // 1. difference function
        for (int tau = 1; tau <= maxTau; ++tau)
        {
            double sum = 0.0;
            for (int j = 0; j < window; ++j)
            {
                const double delta = (double) x[j] - (double) x[j + tau];
                sum += delta * delta;
            }
            diff[(size_t) tau] = sum;
        }

        // 2. cumulative mean normalised difference
        double running = 0.0;
        cmnd[0] = 1.0;
        for (int tau = 1; tau <= maxTau; ++tau)
        {
            running += diff[(size_t) tau];
            cmnd[(size_t) tau] = running > 0.0 ? diff[(size_t) tau] * tau / running : 1.0;
        }

        // 3. first dip below the threshold, walked down to its local minimum
        int tau = -1;
        for (int t = minTau; t < maxTau; ++t)
        {
            if (cmnd[(size_t) t] < threshold)
            {
                while (t + 1 < maxTau && cmnd[(size_t) t + 1] < cmnd[(size_t) t])
                    ++t;
                tau = t;
                break;
            }
        }

        if (tau < 1)
            return 0.0f;

        // 4. parabolic interpolation on the raw difference function for sub-sample accuracy
        const double a = diff[(size_t) tau - 1], b = diff[(size_t) tau], c = diff[(size_t) tau + 1];
        const double denom = a + c - 2.0 * b;
        const double shift = std::abs (denom) > 1.0e-12 ? std::clamp (0.5 * (a - c) / denom, -1.0, 1.0) : 0.0;

        return (float) (rate / (tau + shift));
    }

private:
    static constexpr double minFrequency = 50.0, maxFrequency = 1500.0;
    static constexpr double threshold = 0.15;
    static constexpr double minRms = 0.002; // about -54 dBFS

    int decimation = 1, maxTau = 0, minTau = 2, window = 0, writePos = 0, accumulated = 0;
    double rate = 24000.0;
    float accumulator = 0.0f;
    std::vector<float> ring, frame;
    std::vector<double> diff, cmnd;
};

//==============================================================================
struct NoteInfo
{
    int midiNote = 0;
    float cents = 0.0f;
};

inline NoteInfo frequencyToNote (float hz, float a4 = 440.0f)
{
    const float midi = 69.0f + 12.0f * std::log2 (hz / a4);
    const int nearest = (int) std::lround (midi);
    return { nearest, (midi - (float) nearest) * 100.0f };
}

inline const char* noteName (int midiNote)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((midiNote % 12) + 12) % 12];
}

inline int noteOctave (int midiNote) { return midiNote / 12 - 1; }

} // namespace fx
