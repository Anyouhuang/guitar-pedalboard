#pragma once

#include <juce_dsp/juce_dsp.h>
#include "DspUtils.h"

namespace fx
{
/** Stereo room reverb (JUCE's Freeverb) on a send, so the tail rings out after switching off.
    A pre-delay (up to 500 ms, can be tapped in time) holds the reverb back so the dry note stays clear. */
class ReverbFx
{
public:
    void prepare (double sampleRate, int maxBlockSize)
    {
        fs = sampleRate;
        reverb.setSampleRate (fs);
        wetLeft.assign ((size_t) maxBlockSize, 0.0f);
        wetRight.assign ((size_t) maxBlockSize, 0.0f);
        for (auto& line : preDelayLines)
            line.prepare ((int) (maxPreDelaySeconds * fs) + 8);

        send.reset (fs, 0.03);
        preDelay.reset (fs, 0.2);
        reset();
    }

    void reset()
    {
        reverb.reset();
        for (auto& line : preDelayLines)
            line.reset();
        preDelay.setCurrentAndTargetValue (preDelay.getTargetValue());
        active = false;
        silentSamples = 0;
    }

    void setParameters (bool enabled, float size01, float damping01, float mix01, float preDelayMs = 0.0f)
    {
        const float preDelaySamples = std::clamp (preDelayMs * 0.001f * (float) fs, 0.0f, (float) (maxPreDelaySeconds * fs));
        const bool starting = enabled && ! active;

        if (starting)
        {
            for (auto& line : preDelayLines)
                line.reset();
            preDelay.setCurrentAndTargetValue (preDelaySamples);
            active = true;
        }

        send.setTargetValue (enabled ? 1.0f : 0.0f);
        preDelay.setTargetValue (preDelaySamples);

        juce::Reverb::Parameters p;
        p.roomSize = 0.3f + 0.68f * size01;
        p.damping  = damping01;
        p.wetLevel = wetScale * mix01;
        p.dryLevel = 0.0f; // the dry signal bypasses the reverb entirely
        p.width    = 1.0f;
        reverb.setParameters (p);

        // Clears the tank and jumps juce::Reverb's internal 10 ms gain ramps straight to these settings;
        // otherwise its default dry level would leak through for the first few milliseconds.
        if (starting)
            reverb.setSampleRate (fs);
    }

    bool isActive() const noexcept { return active; }

    /** Adds the reverb to the signal in place. numSamples must not exceed the prepared block size. */
    void process (float* left, float* right, int numSamples) noexcept
    {
        if (! active)
            return;

        for (int i = 0; i < numSamples; ++i)
        {
            const float s = send.getNextValue();
            const float d = preDelay.getNextValue() + 1.0f; // read(1) = the sample just pushed = no delay
            preDelayLines[0].push (left[i] * s);
            preDelayLines[1].push (right[i] * s);
            wetLeft[(size_t) i]  = preDelayLines[0].read (d);
            wetRight[(size_t) i] = preDelayLines[1].read (d);
        }

        reverb.processStereo (wetLeft.data(), wetRight.data(), numSamples);

        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            left[i]  += wetLeft[(size_t) i];
            right[i] += wetRight[(size_t) i];
            peak = std::max ({ peak, std::abs (wetLeft[(size_t) i]), std::abs (wetRight[(size_t) i]) });
        }

        if (send.getTargetValue() == 0.0f && ! send.isSmoothing())
        {
            silentSamples = peak < 1.0e-5f ? silentSamples + numSamples : 0;
            if (silentSamples > (int) ((0.3 + maxPreDelaySeconds) * fs))
                reset();
        }
        else
        {
            silentSamples = 0;
        }
    }

private:
    static constexpr float wetScale = 0.35f;
    static constexpr double maxPreDelaySeconds = 0.5;

    double fs = 48000.0;
    juce::Reverb reverb;
    std::vector<float> wetLeft, wetRight;
    DelayLine preDelayLines[2];
    juce::SmoothedValue<float> send { 0.0f }, preDelay { 0.0f };
    bool active = false;
    int silentSamples = 0;
};

} // namespace fx
