#pragma once

#include <juce_dsp/juce_dsp.h>
#include "DspUtils.h"

namespace fx
{
/** FFT analyser that reduces the spectrum to a fixed number of log-spaced columns (20 Hz .. 20 kHz),
    in dBFS, with analyser-style fall-off. UI-thread only. */
class SpectrumAnalyzer
{
public:
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder, numColumns = 240;
    static constexpr float minHz = 20.0f, maxHz = 20000.0f, floorDb = -120.0f;

    SpectrumAnalyzer() { clear(); }

    /** Frequency shown at a (fractional) column index. */
    static float columnFrequency (float column)
    {
        return minHz * std::pow (maxHz / minHz, juce::jlimit (0.0f, 1.0f, column / (float) (numColumns - 1)));
    }

    void setSampleRate (double newRate) { if (newRate > 0.0) fs = newRate; }

    void clear() { std::fill (std::begin (columns), std::end (columns), floorDb); }

    void push (const float* data, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            ring[(size_t) ringPos] = data[i];
            ringPos = (ringPos + 1) % fftSize;
        }
        newSamples += numSamples;
    }

    /** Analyses the latest fftSize samples (call once per UI frame). */
    void process()
    {
        constexpr float fallPerFrame = 1.6f; // ~48 dB/s at 30 fps

        if (newSamples == 0)
        {
            for (auto& v : columns)
                v = std::max (floorDb, v - fallPerFrame);
            return;
        }
        newSamples = 0;

        for (int i = 0; i < fftSize; ++i)
            fftData[(size_t) i] = ring[(size_t) ((ringPos + i) % fftSize)];
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

        window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        const float norm = 4.0f / (float) fftSize; // full-scale sine through a Hann window -> 0 dBFS
        const int maxBin = fftSize / 2;

        for (int c = 0; c < numColumns; ++c)
        {
            const float binLo = columnFrequency ((float) c - 0.5f) * (float) fftSize / (float) fs;
            const float binHi = columnFrequency ((float) c + 0.5f) * (float) fftSize / (float) fs;
            float magnitude = 0.0f;

            if (binHi - binLo < 1.0f) // low end: fewer bins than columns, interpolate
            {
                const float bin = juce::jlimit (0.0f, (float) maxBin - 1.0f, 0.5f * (binLo + binHi));
                const int i0 = (int) bin;
                magnitude = juce::jmap (bin - (float) i0, fftData[(size_t) i0], fftData[(size_t) i0 + 1]);
            }
            else // high end: several bins per column, keep the loudest
            {
                for (int b = std::max (0, (int) std::ceil (binLo)); b <= std::min (maxBin, (int) binHi); ++b)
                    magnitude = std::max (magnitude, fftData[(size_t) b]);
            }

            const float db = std::max (floorDb, gainToDb (magnitude * norm));
            columns[c] = db > columns[c] ? db : std::max (db, columns[c] - fallPerFrame);
        }
    }

    const float* getColumnsDb() const noexcept { return columns; }

private:
    double fs = 48000.0;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false };
    std::vector<float> ring = std::vector<float> ((size_t) fftSize, 0.0f);
    std::vector<float> fftData = std::vector<float> ((size_t) fftSize * 2, 0.0f);
    int ringPos = 0, newSamples = 0;
    float columns[numColumns];
};

} // namespace fx
