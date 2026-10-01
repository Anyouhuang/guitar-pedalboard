#pragma once

#include <juce_dsp/juce_dsp.h>
#include "DspUtils.h"

namespace fx
{
/** Analog-style stereo delay: the repeats get darker and softly saturate as they feed back.
    Switching it off only stops new input, so the existing echoes ring out ("trails"). */
class Delay
{
public:
    static constexpr double maxSeconds = 2.0;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        for (auto& line : lines)
            line.prepare ((int) (maxSeconds * fs) + 8);

        for (auto& f : highPass)
            f.setCutoff (fs, 70.0);

        time.reset (fs, 0.3); // gliding the delay time gives a tape-like pitch bend instead of clicks
        feedback.reset (fs, 0.05);
        mix.reset (fs, 0.05);
        send.reset (fs, 0.03);
        reset();
    }

    void reset()
    {
        for (auto& line : lines)  line.reset();
        for (auto& f : lowPass)   f.reset();
        for (auto& f : highPass)  f.reset();
        active = false;
        silentSamples = 0;
    }

    void setParameters (bool enabled, float timeMs, float feedback01, float mix01, float tone01)
    {
        const float delaySamples = std::clamp (timeMs * 0.001f * (float) fs, 1.0f, (float) lines[0].getMaxDelay());

        if (enabled && ! active)
        {
            reset();
            active = true;
            time.setCurrentAndTargetValue (delaySamples);
        }

        send.setTargetValue (enabled ? 1.0f : 0.0f);
        time.setTargetValue (delaySamples);
        feedback.setTargetValue (feedback01);
        mix.setTargetValue (mix01);

        for (auto& f : lowPass)
            f.setCutoff (fs, 1000.0 * std::pow (12.0, (double) tone01)); // 1 kHz .. 12 kHz
    }

    bool isActive() const noexcept { return active; }

    /** Adds the echoes to the signal in place. */
    void process (float* left, float* right, int numSamples) noexcept
    {
        if (! active)
            return;

        float* io[] = { left, right };
        float peak = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float d  = time.getNextValue();
            const float fb = feedback.getNextValue();
            const float m  = mix.getNextValue();
            const float s  = send.getNextValue();

            for (int ch = 0; ch < 2; ++ch)
            {
                float wet = lines[ch].read (d);
                wet = highPass[ch].highPass (lowPass[ch].lowPass (wet));
                lines[ch].push (saturate (io[ch][i] * s + fb * wet));
                io[ch][i] += m * wet;
                peak = std::max (peak, std::abs (wet));
            }
        }

        // Once switched off, stop processing after the repeats have died away.
        if (send.getTargetValue() == 0.0f && ! send.isSmoothing())
        {
            silentSamples = peak < 1.0e-5f ? silentSamples + numSamples : 0;
            if (silentSamples > (int) time.getTargetValue() + (int) (0.1 * fs))
                reset();
        }
        else
        {
            silentSamples = 0;
        }
    }

private:
    static float saturate (float x) noexcept { return 1.5f * std::tanh (x * (1.0f / 1.5f)); }

    double fs = 48000.0;
    DelayLine lines[2];
    OnePole lowPass[2], highPass[2];
    juce::SmoothedValue<float> time { 1.0f }, feedback { 0.0f }, mix { 0.0f }, send { 0.0f };
    bool active = false;
    int silentSamples = 0;
};

} // namespace fx
