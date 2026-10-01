#pragma once

#include <juce_dsp/juce_dsp.h>
#include "DspUtils.h"

namespace fx
{
/** Chorus / Flanger / Phaser / Tremolo. The right channel's LFO runs 90 degrees ahead for width. */
class Modulation
{
public:
    enum Type { chorus = 0, flanger, phaser, tremolo };

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        for (auto* line : { &lines[0], &lines[1], &flangerLines[0], &flangerLines[1] })
            line->prepare ((int) (0.030 * fs) + 8);

        depth.reset (fs, 0.05);
        mix.reset (fs, 0.05);
        reset();
    }

    void reset()
    {
        for (auto* line : { &lines[0], &lines[1], &flangerLines[0], &flangerLines[1] })
            line->reset();

        resetPhaser();
        phase = 0.0;
        depth.setCurrentAndTargetValue (depth.getTargetValue());
        mix.setCurrentAndTargetValue (mix.getTargetValue());
    }

    /** Starts the LFO and phaser over but keeps the delay lines (which feed() keeps full), so a chorus
        or flanger that has just been picked fades in on real signal instead of an empty buffer. */
    void restart()
    {
        resetPhaser();
        phase = 0.0;
        depth.setCurrentAndTargetValue (depth.getTargetValue());
        mix.setCurrentAndTargetValue (mix.getTargetValue());
    }

    /** Call while bypassed: keeps the delay lines full of recent audio, so switching the chorus or
        flanger on fades in real signal instead of the silence of an empty buffer (which clicks). */
    void feed (const float* left, const float* right, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            lines[0].push (left[i]);
            lines[1].push (right[i]);
            flangerLines[0].push (left[i]);
            flangerLines[1].push (right[i]);
        }
    }

    /** rateHz = LFO speed, depth01 = sweep amount, mix01 = wet mix (tremolo: wave shape). */
    void setParameters (int newType, float rateHz, float depth01, float mix01)
    {
        newType = std::clamp (newType, 0, 3);
        if (newType != type)
        {
            type = newType;
            resetPhaser(); // keep the delay lines: clearing them would click
        }

        rate = rateHz;
        depth.setTargetValue (depth01);
        mix.setTargetValue (mix01);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        float* io[] = { left, right };
        const double increment = rate / fs;
        constexpr double twoPi = 2.0 * pi;

        for (int i = 0; i < numSamples; ++i)
        {
            const float d = depth.getNextValue();
            const float m = mix.getNextValue();
            const float lfo[] = { (float) std::sin (twoPi * phase),
                                  (float) std::sin (twoPi * (phase + 0.25)) };

            phase += increment;
            if (phase >= 1.0)
                phase -= 1.0;

            for (int ch = 0; ch < 2; ++ch)
            {
                const float x = io[ch][i];
                float wet = x;

                // `lines` always hold the plain input (for the chorus); the flanger's own lines also
                // carry its feedback, so switching between the two never reads the other's echo
                switch (type)
                {
                    case chorus:
                    {
                        const float delayMs = 12.0f + 6.0f * d * lfo[ch]; // 6 .. 18 ms
                        wet = lines[ch].read (delayMs * 0.001f * (float) fs);
                        lines[ch].push (x);
                        flangerLines[ch].push (x);
                        io[ch][i] = x * (1.0f - m) + wet * m;
                        break;
                    }

                    case flanger:
                    {
                        const float delayMs = 0.25f + 4.0f * d * 0.5f * (lfo[ch] + 1.0f); // 0.25 .. 4.25 ms
                        wet = flangerLines[ch].read (delayMs * 0.001f * (float) fs);
                        flangerLines[ch].push (x + 0.7f * wet);
                        lines[ch].push (x);
                        io[ch][i] = x * (1.0f - m) + wet * m;
                        break;
                    }

                    case phaser:
                    {
                        // six first-order all-pass stages swept around 1 kHz, with a little feedback
                        const double fc = 1000.0 * std::pow (2.0, 2.3 * d * lfo[ch]);
                        const double t  = std::tan (pi * fc / fs);
                        const float  a  = (float) ((t - 1.0) / (t + 1.0));

                        float v = x + 0.4f * phaserFeedback[ch];
                        for (auto& s : allpassState[ch])
                        {
                            const float y = a * v + s;
                            s = v - a * y;
                            v = y;
                        }
                        phaserFeedback[ch] = v;
                        lines[ch].push (x);
                        flangerLines[ch].push (x);
                        io[ch][i] = x * (1.0f - m) + v * m;
                        break;
                    }

                    default: // tremolo: both channels share the left LFO, "mix" morphs sine -> square
                    {
                        lines[ch].push (x);
                        flangerLines[ch].push (x);
                        const float k = 0.5f + 9.0f * m;
                        const float shaped = std::tanh (k * lfo[0]) / std::tanh (k);
                        io[ch][i] = x * (1.0f - d * 0.5f * (shaped + 1.0f));
                        break;
                    }
                }
            }
        }
    }

private:
    void resetPhaser()
    {
        for (auto& stages : allpassState)
            std::fill (std::begin (stages), std::end (stages), 0.0f);

        phaserFeedback[0] = phaserFeedback[1] = 0.0f;
    }

    double fs = 48000.0, phase = 0.0;
    float rate = 1.0f;
    int type = chorus;

    DelayLine lines[2], flangerLines[2];
    float allpassState[2][6] {};
    float phaserFeedback[2] {};

    juce::SmoothedValue<float> depth { 0.5f }, mix { 0.5f };
};

} // namespace fx
