#pragma once

#include "DspUtils.h"

namespace fx
{
/** Mains hum remover for the guitar input (mono): narrow notches on 50 or 60 Hz and its first harmonics.
    Put before the effects, because distortion and compression would amplify the hum.

    The notches are only a few Hz wide, so notes are left alone (a note that sits right next to a harmonic,
    like B2 at 123.5 Hz next to 120 Hz, loses well under 1 dB). Filters this narrow and this low need more
    precision than a float's worth of coefficient, so this one works in doubles. */
class HumFilter
{
public:
    enum Mode { off = 0, hz50, hz60 };
    static constexpr int numNotches = 6;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        amount.reset (fs, 0.03);
        design();
        reset();
    }

    void reset() noexcept
    {
        clear();
        if (wanted != off)
            tuned = wanted;
        design();
        amount.setCurrentAndTarget (wanted != off ? 1.0f : 0.0f);
    }

    void setMode (int mode)
    {
        wanted = std::clamp (mode, (int) off, (int) hz60);

        // retune only while it is faded out, so going from 50 to 60 Hz never clicks
        if (wanted != off && wanted != tuned && isIdle())
        {
            tuned = wanted;
            design();
            clear();
        }

        amount.setTarget (wanted != off && wanted == tuned ? 1.0f : 0.0f);
    }

    void process (float* data, int numSamples) noexcept
    {
        if (isIdle())
        {
            needsClear = true;
            return;
        }

        if (needsClear)
        {
            clear();
            needsClear = false;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const double x = data[i];
            double y = x;
            for (auto& n : notches)
            {
                const double out = n.b0 * y + n.z1;
                n.z1 = n.b1 * y - n.a1 * out + n.z2;
                n.z2 = n.b2 * y - n.a2 * out;
                y = out;
            }
            data[i] = (float) (x + (double) amount.next() * (y - x));
        }
    }

    /** Bandwidth (-3 dB) of the notch on harmonic `k` (1 = the mains frequency): wider higher up, because
        a drift of the mains frequency moves the harmonics k times as far. */
    static double bandwidthHz (int k) noexcept { return 2.0 + 0.4 * (k - 1); }

private:
    struct Notch { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0; };

    bool isIdle() const noexcept { return ! amount.isSmoothing() && amount.getCurrent() <= 0.0f; }

    void clear() noexcept
    {
        for (auto& n : notches)
            n.z1 = n.z2 = 0.0;
    }

    void design() noexcept
    {
        const double mains = tuned == hz50 ? 50.0 : 60.0;
        for (int k = 1; k <= numNotches; ++k)
        {
            const double f = mains * k;
            const double w0 = 2.0 * pi * f / fs;
            const double alpha = std::sin (w0) / (2.0 * (f / bandwidthHz (k)));
            const double a0 = 1.0 + alpha;
            auto& n = notches[k - 1];
            n.b0 = 1.0 / a0;
            n.b1 = -2.0 * std::cos (w0) / a0;
            n.b2 = 1.0 / a0;
            n.a1 = n.b1;
            n.a2 = (1.0 - alpha) / a0;
        }
    }

    double fs = 48000.0;
    Notch notches[numNotches];
    int wanted = off, tuned = hz60;
    Smoothed amount { 0.0f };
    bool needsClear = false;
};

//==============================================================================
/** Hiss reduction for the output (stereo), single-ended like the rack units guitarists use: while you
    play it is wide open and does nothing; as the sound dies away, a low-pass slides down over the hiss and
    the level is eased down by up to 12 dB. No look-ahead, so no latency.
    Amount sets how loud the signal must be to open it: 0 % = off, higher = more noise removed (and, set too
    high, the ends of quiet notes get duller). */
class Denoiser
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        attack = (float) std::exp (-1.0 / (0.002 * fs));
        release = (float) std::exp (-1.0 / (0.12 * fs));
        mix.reset (fs, 0.03);
        gain.reset (fs, 0.02);
        reset();
    }

    void reset() noexcept
    {
        for (auto& f : filters)
            f.reset();
        env = 1.0f; // starts open: a note played right away is never dulled
        counter = 0;
        mix.setCurrentAndTarget (mix.getTarget());
        gain.setCurrentAndTarget (1.0f);
        retune (1.0f);
    }

    void setAmount (float percent) noexcept
    {
        amount = std::clamp (percent, 0.0f, 100.0f);
        thresholdDb = -75.0f + 0.4f * amount; // -75 .. -35 dBFS
        mix.setTarget (amount > 0.0f ? 1.0f : 0.0f);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        if (! mix.isSmoothing() && mix.getCurrent() <= 0.0f)
        {
            needsReset = true;
            return;
        }

        if (needsReset)
        {
            for (auto& f : filters)
                f.reset();
            env = 1.0f;
            counter = 0;
            gain.setCurrentAndTarget (1.0f);
            retune (1.0f);
            needsReset = false;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const float l = left[i], r = right[i];
            const float level = std::max (std::abs (l), std::abs (r));
            env = level + (level > env ? attack : release) * (env - level);

            // every 16 samples: how far open? (0 = at or below the threshold, 1 = 18 dB above it)
            if (counter == 0)
            {
                const float x = std::clamp ((gainToDb (env) - thresholdDb) / 18.0f, 0.0f, 1.0f);
                const float open = x * x * (3.0f - 2.0f * x);
                retune (open);
                gain.setTarget (dbToGain (-12.0f * (1.0f - open)));
            }
            counter = (counter + 1) & 15;

            const float g = gain.next(), m = mix.next();
            const float yl = filters[1].lowPass (filters[0].lowPass (l)) * g;
            const float yr = filters[3].lowPass (filters[2].lowPass (r)) * g;
            left[i]  = l + m * (yl - l);
            right[i] = r + m * (yr - r);
        }
    }

private:
    /** Low-pass cutoff for how open it is: 1 kHz when closed, out of the way (near Nyquist) when open. */
    void retune (float open) noexcept
    {
        const double cutoff = 1000.0 * std::pow (0.49 * fs / 1000.0, (double) open);
        for (auto& f : filters)
            f.setCutoff (fs, cutoff);
    }

    double fs = 48000.0;
    OnePole filters[4]; // two in series per side
    Smoothed mix { 0.0f }, gain { 1.0f };
    float amount = 0.0f, thresholdDb = -75.0f;
    float env = 1.0f, attack = 0.99f, release = 0.999f;
    int counter = 0;
    bool needsReset = false;
};

} // namespace fx
