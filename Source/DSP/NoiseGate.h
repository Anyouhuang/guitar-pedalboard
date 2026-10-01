#pragma once

#include "DspUtils.h"

namespace fx
{
/** Noise gate with hysteresis and hold, so decaying notes don't chatter on/off. */
class NoiseGate
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        envDecay    = (float) std::exp (-1.0 / (0.030 * fs));  // level detector decay
        attackCoef  = (float) std::exp (-1.0 / (0.0005 * fs)); // open fast so picking attack survives
        holdSamples = (int) (0.040 * fs);
        setParameters (-65.0f, 60.0f);
        reset();
    }

    void reset()
    {
        env = 0.0f;
        gain = 0.0f;
        holdCounter = 0;
        isOpen = false;
    }

    void setParameters (float thresholdDb, float releaseMs)
    {
        openThreshold  = dbToGain (thresholdDb);
        closeThreshold = openThreshold * 0.5f; // 6 dB hysteresis
        releaseCoef    = (float) std::exp (-1.0 / (std::max (1.0f, releaseMs) * 0.001 * fs));
    }

    void process (float* data, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float x = data[i];
            const float a = std::abs (x);
            env = a > env ? a : env * envDecay;

            if (env >= openThreshold)
            {
                isOpen = true;
                holdCounter = holdSamples;
            }
            else if (isOpen)
            {
                if (env >= closeThreshold)  holdCounter = holdSamples;
                else if (holdCounter > 0)   --holdCounter;
                else                        isOpen = false;
            }

            const float target = isOpen ? 1.0f : 0.0f;
            const float coef   = target > gain ? attackCoef : releaseCoef;
            gain = target + coef * (gain - target);
            data[i] = x * gain;
        }
    }

private:
    double fs = 48000.0;
    float env = 0.0f, gain = 0.0f;
    float envDecay = 0.999f, attackCoef = 0.9f, releaseCoef = 0.999f;
    float openThreshold = 0.001f, closeThreshold = 0.0005f;
    int holdSamples = 0, holdCounter = 0;
    bool isOpen = false;
};

} // namespace fx
