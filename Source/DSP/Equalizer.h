#pragma once

#include <juce_dsp/juce_dsp.h>
#include "DspUtils.h"

namespace fx
{
/** All EQ settings in plain units. */
struct EqSettings
{
    float lowCutHz  = 20.0f;
    float bassHz    = 100.0f,  bassDb    = 0.0f;
    float lowMidHz  = 400.0f,  lowMidDb  = 0.0f, lowMidQ  = 1.0f;
    float highMidHz = 2000.0f, highMidDb = 0.0f, highMidQ = 1.0f;
    float trebleHz  = 5000.0f, trebleDb  = 0.0f;
    float highCutHz = 20000.0f;
};

/** Six-band guitar EQ: low cut, bass shelf, two parametric mids, treble shelf, high cut.
    Knob moves are smoothed and the filters redesigned every few samples, so dragging is zipper-free. */
class Equalizer
{
public:
    enum Band { lowCut = 0, bass, lowMid, highMid, treble, highCut, numBands };

    /** Designs the filters for `s`. Shared by the audio path and the on-screen response curve,
        so what you see is exactly what you hear. */
    static void design (const EqSettings& s, double fs, Biquad (&filters)[numBands])
    {
        filters[lowCut] .setHighPass  (fs, s.lowCutHz, 0.707);
        filters[bass]   .setLowShelf  (fs, s.bassHz, s.bassDb);
        filters[lowMid] .setPeak      (fs, s.lowMidHz, s.lowMidQ, s.lowMidDb);
        filters[highMid].setPeak      (fs, s.highMidHz, s.highMidQ, s.highMidDb);
        filters[treble] .setHighShelf (fs, s.trebleHz, s.trebleDb);
        filters[highCut].setLowPass   (fs, s.highCutHz, 0.707);
    }

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        for (auto& v : smoothed)
            v.reset (fs, 0.04);

        reset();
    }

    void reset()
    {
        for (auto& f : filters)
            f.reset();

        for (auto& v : smoothed)
            v.setCurrentAndTargetValue (v.getTargetValue());

        design (currentSettings(), fs, filters);
    }

    void setParameters (const EqSettings& s)
    {
        // frequencies and Q are smoothed on a log scale, gains in dB
        const float targets[numValues] = { std::log2 (s.lowCutHz),
                                           std::log2 (s.bassHz),    s.bassDb,
                                           std::log2 (s.lowMidHz),  s.lowMidDb,  std::log2 (s.lowMidQ),
                                           std::log2 (s.highMidHz), s.highMidDb, std::log2 (s.highMidQ),
                                           std::log2 (s.trebleHz),  s.trebleDb,
                                           std::log2 (s.highCutHz) };

        for (int i = 0; i < numValues; ++i)
            smoothed[i].setTargetValue (targets[i]);
    }

    void process (float* data, int numSamples) noexcept
    {
        constexpr int updateInterval = 16;

        for (int start = 0; start < numSamples; start += updateInterval)
        {
            const int n = std::min (updateInterval, numSamples - start);

            if (isSmoothing())
            {
                for (auto& v : smoothed)
                    v.skip (n);

                design (currentSettings(), fs, filters);
            }

            for (int i = start; i < start + n; ++i)
            {
                float x = data[i];
                for (auto& f : filters)
                    x = f.process (x);
                data[i] = x;
            }
        }
    }

private:
    static constexpr int numValues = 12;

    bool isSmoothing() const noexcept
    {
        for (const auto& v : smoothed)
            if (v.isSmoothing())
                return true;
        return false;
    }

    EqSettings currentSettings() const
    {
        auto v = [this] (int i) { return smoothed[i].getCurrentValue(); };
        auto hz = [&] (int i) { return std::exp2 (v (i)); };

        EqSettings s;
        s.lowCutHz  = hz (0);
        s.bassHz    = hz (1);  s.bassDb    = v (2);
        s.lowMidHz  = hz (3);  s.lowMidDb  = v (4);  s.lowMidQ  = hz (5);
        s.highMidHz = hz (6);  s.highMidDb = v (7);  s.highMidQ = hz (8);
        s.trebleHz  = hz (9);  s.trebleDb  = v (10);
        s.highCutHz = hz (11);
        return s;
    }

    double fs = 48000.0;
    Biquad filters[numBands];
    juce::SmoothedValue<float> smoothed[numValues];
};

} // namespace fx
