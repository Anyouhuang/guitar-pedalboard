#pragma once

#include <juce_dsp/juce_dsp.h>
#include "DspUtils.h"

namespace fx
{
/** Building blocks shared by the distortion models. */
namespace shape
{
    /** Audio-taper pot: 0..1 travel -> 0..1 resistance share, `k` sets how log the taper is. */
    inline float taper (float x01, float k) noexcept { return (std::exp (k * x01) - 1.0f) / (std::exp (k) - 1.0f); }

    /** Hard-knee clipper, like a pair of silicon diodes to ground. */
    inline float diode (float x) noexcept { return x / std::pow (1.0f + std::pow (std::abs (x), 2.5f), 0.4f); }

    /** Biased transistor stage: soft, asymmetric (even harmonics). */
    inline float asym (float x, float bias) noexcept { return std::tanh (x + bias) - std::tanh (bias); }

    /** Triode-like: the negative half compresses earlier than the positive half. */
    inline float tube (float x) noexcept { return x >= 0.0f ? std::tanh (x) : 0.7f * std::tanh (x / 0.7f); }

    /** 0..100 % EQ knob -> +-range dB, flat at 50 %. */
    inline float eqDb (float percent, float range) noexcept { return (percent - 50.0f) * (range / 50.0f); }
}

//==============================================================================
/** One distortion pedal. The engine runs processOversampled() at 4x the sample rate
    (gain stages and clipping), then processBase() at the normal rate (tone, EQ, output). */
class DriveModel
{
public:
    virtual ~DriveModel() = default;

    void prepare (double baseRate, double overRate)
    {
        fs = baseRate;
        os = overRate;
        gain.reset (os, 0.05);
        level.reset (fs, 0.05);
        dc.prepare (fs);
    }

    virtual void reset()
    {
        gain.setCurrentAndTargetValue (gain.getTargetValue());
        level.setCurrentAndTargetValue (level.getTargetValue());
        dc.reset();
        eqBass.reset();
        eqMid.reset();
        eqTreble.reset();
    }

    /** Knob values in plain units: Drive, Bass, Mid (or the model's own control), Treble, Output (see Models.h). */
    virtual void setParameters (const float* knobs) = 0;
    virtual void processOversampled (float* x, int n) noexcept = 0;
    virtual void processBase (float* x, int n) noexcept = 0;

protected:
    static float drive01 (const float* k) noexcept { return k[0] / 100.0f; }

    /** Line 6 gave every HD distortion Bass / Mid / Treble (50 % = flat) and an Output level.
        `makeup` sets the level at 50 % drive; the loudness trim keeps the volume roughly steady across the
        Drive knob (turning it down gives a near-clean boost, as on the HD), keeping 30 % of the natural change. */
    void setEqAndOutput (float drivePercent, float bassPercent, float midPercent, float treblePercent, float outputDb, float makeup)
    {
        eqBass.setLowShelf (fs, 120.0, shape::eqDb (bassPercent, 12.0f));
        eqMid.setPeak (fs, 800.0, 0.8, shape::eqDb (midPercent, 12.0f));
        eqTreble.setHighShelf (fs, 3000.0, shape::eqDb (treblePercent, 12.0f));

        const float d = std::clamp (drivePercent / 25.0f, 0.0f, 4.0f);
        const int i = std::min ((int) d, 3);
        const float trimDb = loudnessTrim[i] + (loudnessTrim[i + 1] - loudnessTrim[i]) * (d - (float) i);
        level.setTargetValue (dbToGain (outputDb + trimDb) * makeup);
    }

public:
    /** dB added at 0, 25, 50, 75 and 100 % drive (measured by PedalTest's "loudness vs Drive" table). */
    const float* loudnessTrim = noTrim;

protected:
    static constexpr float noTrim[5] = {};

    /** DC blocker, the EQ and the output level: the last step of every model. */
    void finish (float* x, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            x[i] = eqTreble.process (eqMid.process (eqBass.process (dc.process (x[i])))) * level.getNextValue();
    }

    double fs = 48000.0, os = 192000.0;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gain { 1.0f };
    juce::SmoothedValue<float> level { 1.0f };
    DcBlocker dc;
    Biquad eqBass, eqMid, eqTreble;
};

//==============================================================================
/** Chandler Tube Driver: one 12AX7 stage pushed hard, warm and round with a singing sustain. */
class TubeDriveModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        gain.setTargetValue (2.0f + 90.0f * shape::taper (drive01 (k), 3.0f));
        inputHp.setCutoff (os, 110.0);
        coupling.setCutoff (os, 40.0);
        bandLimit.setCutoff (os, 7000.0);
        post.setCutoff (fs, 6500.0);
        setEqAndOutput (k[0], k[1], k[2], k[3], k[4], 0.16f);
    }

    void reset() override { DriveModel::reset(); inputHp.reset(); coupling.reset(); bandLimit.reset(); post.reset(); }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float s1 = shape::tube (gain.getNextValue() * inputHp.highPass (x[i]));
            const float s2 = shape::tube (2.2f * coupling.highPass (s1));
            x[i] = bandLimit.lowPass (s2);
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
            x[i] = post.lowPass (x[i]);
        finish (x, n);
    }

private:
    OnePole inputHp, coupling, bandLimit, post;
};

//==============================================================================
/** Ibanez TS808: an op-amp with diodes in its feedback loop. Only the mids above ~720 Hz get the
    gain and clip, the lows pass clean underneath: the famous mid hump. TONE is the original's. */
class ScreamerModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        const double rd = 500e3 * shape::taper (drive01 (k), 3.0f); // 500k drive pot
        gain.setTargetValue ((float) (1.0 + (51e3 + rd) / 4.7e3));
        clipHp.setCutoff (os, 720.0);
        feedbackCap.setCutoff (os, 1.0 / (2.0 * pi * (51e3 + rd) * 51e-12)); // 51 pF across the diodes
        toneLp.setCutoff (fs, 723.0);
        const float t = k[2] / 100.0f;
        treble = 0.05f + 1.6f * t * t;
        setEqAndOutput (k[0], k[1], 50.0f, k[3], k[4], 0.32f);
    }

    void reset() override { DriveModel::reset(); clipHp.reset(); feedbackCap.reset(); toneLp.reset(); }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float v = feedbackCap.lowPass (gain.getNextValue() * clipHp.highPass (x[i]));
            x[i] += 0.6f * std::tanh (v * (1.0f / 0.6f));
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        // passive 723 Hz low-pass plus the tone knob's treble
        for (int i = 0; i < n; ++i)
        {
            const float low = toneLp.lowPass (x[i]);
            x[i] = low + treble * (x[i] - low);
        }
        finish (x, n);
    }

private:
    OnePole clipHp, feedbackCap, toneLp;
    float treble = 0.5f;
};

//==============================================================================
/** DOD 250: op-amp gain stage into diodes to ground. Brighter and raspier than the Screamer. */
class OverdriveModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        const float g = 1.0f + 1.0e6f * shape::taper (drive01 (k), 3.5f) / 4.7e3f;
        gain.setTargetValue (g);
        gainHp.setCutoff (os, 720.0);
        opAmp.setCutoff (os, std::min (20000.0, 1.0e6 / g)); // 741: 1 MHz gain-bandwidth
        bandLimit.setCutoff (os, 12000.0);
        post.setCutoff (fs, 7000.0);
        setEqAndOutput (k[0], k[1], k[2], k[3], k[4], 0.23f);
    }

    void reset() override { DriveModel::reset(); gainHp.reset(); opAmp.reset(); bandLimit.reset(); post.reset(); }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float v = opAmp.lowPass (x[i] + (gain.getNextValue() - 1.0f) * gainHp.highPass (x[i]));
            x[i] = bandLimit.lowPass (0.55f * shape::diode (v * (1.0f / 0.55f)));
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
            x[i] = post.lowPass (x[i]);
        finish (x, n);
    }

private:
    OnePole gainHp, opAmp, bandLimit, post;
};

//==============================================================================
/** Pro Co RAT: huge op-amp gain whose bandwidth shrinks as the gain goes up (LM308),
    hard diode clipping, then the passive FILTER (turn up = darker). */
class ClassicDistModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        const float rd = 100e3f * shape::taper (drive01 (k), 4.0f); // 100k distortion pot
        gain.setTargetValue (rd / 1000.0f + 0.001f);                 // kOhm, smoothed on a log scale
        leg1.setCutoff (os, 60.5);   // 560R + 4.7 uF to ground
        leg2.setCutoff (os, 1539.0); // 47R + 2.2 uF to ground
        const double hfGain = 1.0 + rd / 43.4;
        opAmp.setCutoff (os, std::min (20000.0, 1.0e6 / hfGain));
        const double rf = 100e3 * shape::taper (k[2] / 100.0f, 3.0f);
        filter.setCutoff (fs, 1.0 / (2.0 * pi * (1.5e3 + rf) * 3.3e-9)); // 32 kHz .. 475 Hz
        setEqAndOutput (k[0], k[1], 50.0f, k[3], k[4], 0.19f);
    }

    void reset() override { DriveModel::reset(); leg1.reset(); leg2.reset(); opAmp.reset(); filter.reset(); }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float rd = 1000.0f * (gain.getNextValue() - 0.001f);
            const float v = x[i] + rd * (leg1.highPass (x[i]) * (1.0f / 560.0f) + leg2.highPass (x[i]) * (1.0f / 47.0f));
            x[i] = 0.55f * shape::diode (opAmp.lowPass (v) * (1.0f / 0.55f));
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
            x[i] = filter.lowPass (x[i]);
        finish (x, n);
    }

private:
    OnePole leg1, leg2, opAmp, filter;
};

//==============================================================================
/** BOSS MT-2 Metal Zone: heavy and scooped - tight lows, two clipping stages, mids pulled back. */
class HeavyDistModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        gain.setTargetValue (15.0f + 600.0f * shape::taper (drive01 (k), 3.0f));
        tight.setCutoff (os, 140.0);
        preMid.setPeak (os, 900.0, 0.8, 8.0);
        interHp.setCutoff (os, 220.0);
        interLp.setCutoff (os, 6000.0);
        bandLimit.setCutoff (os, 9000.0);
        fizz.setLowPass (fs, 7000.0, 0.707);
        scoop.setPeak (fs, 650.0, 0.9, -5.0);
        setEqAndOutput (k[0], k[1], k[2], k[3], k[4], 0.1f);
    }

    void reset() override
    {
        DriveModel::reset();
        tight.reset(); preMid.reset(); interHp.reset(); interLp.reset(); bandLimit.reset(); fizz.reset(); scoop.reset();
    }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float s1 = std::tanh (gain.getNextValue() * preMid.process (tight.highPass (x[i])));
            const float s2 = shape::diode (4.0f * interLp.lowPass (interHp.highPass (s1)));
            x[i] = bandLimit.lowPass (s2);
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
            x[i] = scoop.process (fizz.process (x[i]));
        finish (x, n);
    }

private:
    OnePole tight, interHp, interLp, bandLimit;
    Biquad preMid, fizz, scoop;
};

//==============================================================================
/** Colorsound Overdriver: raw, fat three-transistor overdrive. */
class ColorDriveModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        gain.setTargetValue (2.0f + 300.0f * shape::taper (drive01 (k), 3.5f));
        inputHp.setCutoff (os, 45.0);
        bandLimit.setCutoff (os, 8000.0);
        post.setCutoff (fs, 7500.0);
        setEqAndOutput (k[0], k[1], k[2], k[3], k[4], 0.12f);
    }

    void reset() override { DriveModel::reset(); inputHp.reset(); bandLimit.reset(); post.reset(); }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float s1 = shape::asym (gain.getNextValue() * inputHp.highPass (x[i]), 0.35f);
            x[i] = bandLimit.lowPass (std::tanh (1.8f * s1));
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
            x[i] = post.lowPass (x[i]);
        finish (x, n);
    }

private:
    OnePole inputHp, bandLimit, post;
};

//==============================================================================
/** Maestro FZ-1: thin, brassy and gated - quiet notes sputter out instead of sustaining. */
class BuzzSawModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        gain.setTargetValue (20.0f + 500.0f * shape::taper (drive01 (k), 3.0f));
        inputHp.setCutoff (os, 300.0);
        bandLimit.setCutoff (os, 6000.0);
        horn.setPeak (fs, 1300.0, 1.5, 6.0);
        post.setCutoff (fs, 4500.0);
        setEqAndOutput (k[0], k[1], k[2], k[3], k[4], 0.092f);
    }

    void reset() override { DriveModel::reset(); inputHp.reset(); bandLimit.reset(); horn.reset(); post.reset(); }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float v = gain.getNextValue() * inputHp.highPass (x[i]);
            const float gated = v > 0.25f ? v - 0.25f : (v < -0.25f ? v + 0.25f : 0.0f); // starved transistor
            const float y = gated >= 0.0f ? std::tanh (2.5f * gated) : 0.6f * std::tanh (gated * (2.5f / 0.6f));
            x[i] = bandLimit.lowPass (y);
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
            x[i] = post.lowPass (horn.process (x[i]));
        finish (x, n);
    }

private:
    OnePole inputHp, bandLimit, post;
    Biquad horn;
};

//==============================================================================
/** Arbiter Fuzz Face: two germanium transistors, fat and touch-sensitive. Its bias shifts as you dig in,
    so hard-picked notes get more asymmetric and sputter; it cleans up with the guitar's volume. */
class FacialFuzzModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        gain.setTargetValue (3.0f + 250.0f * shape::taper (drive01 (k), 2.5f));
        pickupLoad.setCutoff (os, 4500.0); // 22k input impedance loads the pickup
        inputHp.setCutoff (os, 70.0);
        bandLimit.setCutoff (os, 7000.0);
        attack = (float) std::exp (-1.0 / (0.002 * os));
        release = (float) std::exp (-1.0 / (0.1 * os));
        post.setCutoff (fs, 5000.0);
        setEqAndOutput (k[0], k[1], k[2], k[3], k[4], 0.12f);
    }

    void reset() override { DriveModel::reset(); pickupLoad.reset(); inputHp.reset(); bandLimit.reset(); post.reset(); env = 0.0f; }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float v = gain.getNextValue() * inputHp.highPass (pickupLoad.lowPass (x[i]));
            const float a = std::abs (v);
            env = a + (a > env ? attack : release) * (env - a);
            const float bias = 0.15f + 0.25f * env / (1.0f + env);
            x[i] = bandLimit.lowPass (std::tanh (1.6f * shape::asym (v, bias)));
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
            x[i] = post.lowPass (x[i]);
        finish (x, n);
    }

private:
    OnePole pickupLoad, inputHp, bandLimit, post;
    float env = 0.0f, attack = 0.99f, release = 0.999f;
};

//==============================================================================
/** Vox Tone Bender: three germanium transistors, lots of gain, raspy and bright. */
class JumboFuzzModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        gain.setTargetValue (10.0f + 900.0f * shape::taper (drive01 (k), 3.0f));
        inputHp.setCutoff (os, 120.0);
        bandLimit.setCutoff (os, 8000.0);
        dark.setCutoff (fs, 700.0);
        bright.setCutoff (fs, 1100.0);
        setEqAndOutput (k[0], k[1], k[2], k[3], k[4], 0.17f);
    }

    void reset() override { DriveModel::reset(); inputHp.reset(); bandLimit.reset(); dark.reset(); bright.reset(); }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float s1 = shape::asym (gain.getNextValue() * inputHp.highPass (x[i]), 0.3f);
            x[i] = bandLimit.lowPass (std::tanh (3.0f * s1));
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        // the original's tone network, set a little past halfway
        for (int i = 0; i < n; ++i)
            x[i] = 0.45f * dark.lowPass (x[i]) + 0.55f * 1.3f * bright.highPass (x[i]);
        finish (x, n);
    }

private:
    OnePole inputHp, bandLimit, dark, bright;
};

//==============================================================================
/** Electro-Harmonix Big Muff Pi: two cascaded diode clipping stages for endless sustain,
    and the passive tone stack that scoops the mids. */
class FuzzPiModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        gain.setTargetValue (1.0f + 80.0f * shape::taper (drive01 (k), 3.0f));
        inputHp.setCutoff (os, 90.0);
        stage1Lp.setCutoff (os, 2800.0);
        interHp.setCutoff (os, 120.0);
        stage2Lp.setCutoff (os, 3200.0);
        toneLp.setCutoff (fs, 723.0);
        toneHp.setCutoff (fs, 1850.0);
        setEqAndOutput (k[0], k[1], k[2], k[3], k[4], 0.25f);
    }

    void reset() override
    {
        DriveModel::reset();
        inputHp.reset(); stage1Lp.reset(); interHp.reset(); stage2Lp.reset(); toneLp.reset(); toneHp.reset();
    }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float s1 = std::tanh (gain.getNextValue() * 4.0f * inputHp.highPass (x[i]));
            const float s2 = std::tanh (18.0f * interHp.highPass (stage1Lp.lowPass (s1)));
            x[i] = stage2Lp.lowPass (s2);
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        // the original's tone knob at noon: half low-pass, half high-pass = the mid scoop
        for (int i = 0; i < n; ++i)
            x[i] = 0.5f * toneLp.lowPass (x[i]) + 0.5f * toneHp.highPass (x[i]);
        finish (x, n);
    }

private:
    OnePole inputHp, stage1Lp, interHp, stage2Lp, toneLp, toneHp;
};

//==============================================================================
/** Roland AP-7 Jet Phaser: a fuzz into a resonant phaser, for the "jet plane" whoosh.
    Knobs: Drive, Fdbk (phaser resonance), Tone, Speed (Hz), Output. */
class JetFuzzModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        gain.setTargetValue (10.0f + 500.0f * shape::taper (drive01 (k), 3.0f));
        inputHp.setCutoff (os, 100.0);
        bandLimit.setCutoff (os, 6000.0);
        feedback = 0.85f * k[1] / 100.0f;
        tone.setCutoff (fs, 800.0 * std::pow (12.0, (double) k[2] / 100.0)); // 800 Hz .. 9.6 kHz
        rate = k[3];
        setEqAndOutput (k[0], 50.0f, 50.0f, 50.0f, k[4], 0.15f);
    }

    void reset() override
    {
        DriveModel::reset();
        inputHp.reset(); bandLimit.reset(); tone.reset();
        std::fill (std::begin (stages), std::end (stages), 0.0f);
        last = 0.0f;
        phase = 0.0;
    }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
            x[i] = bandLimit.lowPass (shape::diode (gain.getNextValue() * inputHp.highPass (x[i])));
    }

    void processBase (float* x, int n) noexcept override
    {
        const double increment = rate / fs;
        for (int i = 0; i < n; ++i)
        {
            const double lfo = std::sin (2.0 * pi * phase);
            phase += increment;
            if (phase >= 1.0)
                phase -= 1.0;

            // six first-order all-passes swept 250 Hz .. 4 kHz, with resonance
            const double fc = 1000.0 * std::pow (4.0, 0.8 * lfo);
            const double t = std::tan (pi * fc / fs);
            const float a = (float) ((t - 1.0) / (t + 1.0));

            float v = x[i] + feedback * last;
            for (auto& s : stages)
            {
                const float y = a * v + s;
                s = v - a * y;
                v = y;
            }
            last = v;
            x[i] = tone.lowPass (0.5f * (x[i] + v));
        }
        finish (x, n);
    }

private:
    OnePole inputHp, bandLimit, tone;
    float stages[6] {};
    float last = 0.0f, feedback = 0.4f;
    double rate = 0.4, phase = 0.0;
};

//==============================================================================
/** Line 6 Drive: the Mid knob morphs the clipping - '70s fuzz at the bottom, modern high gain (RAT / Super
    Distortion) in the middle, Tone Bender grit at the top. Bass and Treble are the usual EQ. */
class Line6DriveModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        gain.setTargetValue (3.0f + 250.0f * shape::taper (drive01 (k), 3.0f));
        const float m = k[2] / 100.0f;
        fuzzWeight   = std::max (0.0f, 1.0f - 2.0f * m);
        modernWeight = 1.0f - std::abs (2.0f * m - 1.0f);
        gritWeight   = std::max (0.0f, 2.0f * m - 1.0f);
        inputHp.setCutoff (os, 80.0);
        midPush.setCutoff (os, 600.0);
        bandLimit.setCutoff (os, 7500.0);
        setEqAndOutput (k[0], k[1], 50.0f, k[3], k[4], 0.12f);
    }

    void reset() override { DriveModel::reset(); inputHp.reset(); midPush.reset(); bandLimit.reset(); }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = gain.getNextValue();
            const float v = inputHp.highPass (x[i]) + modernWeight * 1.5f * midPush.highPass (x[i]);
            const float fuzz   = std::tanh (2.5f * shape::asym (4.0f * g * v, 0.3f));
            const float modern = shape::diode (1.5f * g * v);
            const float grit   = shape::diode (2.0f * g * v + 0.25f) - shape::diode (0.25f);
            x[i] = bandLimit.lowPass (0.9f * fuzzWeight * fuzz + modernWeight * modern + gritWeight * grit);
        }
    }

    void processBase (float* x, int n) noexcept override { finish (x, n); }

private:
    OnePole inputHp, midPush, bandLimit;
    float fuzzWeight = 0.0f, modernWeight = 1.0f, gritWeight = 0.0f;
};

//==============================================================================
/** Line 6 Distortion: massive, over-the-top gain that stays tight - the lows are cut before clipping
    and put back after. */
class Line6DistModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        gain.setTargetValue (20.0f + 1200.0f * shape::taper (drive01 (k), 3.0f));
        tight.setCutoff (os, 250.0);
        interLp.setCutoff (os, 7000.0);
        bandLimit.setCutoff (os, 10000.0);
        body.setLowShelf (fs, 110.0, 6.0);
        fizz.setLowPass (fs, 6500.0, 0.707);
        setEqAndOutput (k[0], k[1], k[2], k[3], k[4], 0.094f);
    }

    void reset() override { DriveModel::reset(); tight.reset(); interLp.reset(); bandLimit.reset(); body.reset(); fizz.reset(); }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float s1 = shape::asym (gain.getNextValue() * tight.highPass (x[i]), 0.2f);
            x[i] = bandLimit.lowPass (shape::diode (3.0f * interLp.lowPass (s1)));
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
            x[i] = fizz.process (body.process (x[i]));
        finish (x, n);
    }

private:
    OnePole tight, interLp, bandLimit;
    Biquad body, fizz;
};

//==============================================================================
/** PAiA Roctave Divider: flip-flops count the guitar's cycles to make square waves one and two octaves
    down that follow the playing's envelope, blended with the fuzz by the Sub knob. Play single notes. */
class SubOctaveFuzzModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        gain.setTargetValue (5.0f + 200.0f * shape::taper (drive01 (k), 3.0f));
        inputHp.setCutoff (os, 90.0);
        track1.setCutoff (os, 700.0);
        track2.setCutoff (os, 700.0);
        subLp.setCutoff (os, 2500.0);
        bandLimit.setCutoff (os, 7000.0);
        envAttack = (float) std::exp (-1.0 / (0.003 * os));
        envRelease = (float) std::exp (-1.0 / (0.06 * os));
        sub = k[2] / 100.0f;
        post.setCutoff (fs, 6000.0);
        setEqAndOutput (k[0], k[1], 50.0f, k[3], k[4], 0.17f);
    }

    void reset() override
    {
        DriveModel::reset();
        inputHp.reset(); track1.reset(); track2.reset(); subLp.reset(); bandLimit.reset(); post.reset();
        env = 0.0f;
        high = false;
        flip1 = flip2 = 1.0f;
    }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float in = inputHp.highPass (x[i]);
            const float a = std::abs (in);
            env = a + (a > env ? envAttack : envRelease) * (env - a);

            // comparator with hysteresis on the low-passed signal; each rising edge toggles the dividers
            const float tracked = track2.lowPass (track1.lowPass (in));
            const float threshold = 0.1f * env;
            if (! high && tracked > threshold)
            {
                high = true;
                flip1 = -flip1;
                if (flip1 > 0.0f)
                    flip2 = -flip2;
            }
            else if (high && tracked < -threshold)
            {
                high = false;
            }

            const float octaves = subLp.lowPass (2.0f * env * (0.7f * flip1 + 0.5f * flip2));
            const float fuzz = std::tanh (gain.getNextValue() * in);
            x[i] = bandLimit.lowPass (fuzz + sub * octaves);
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
            x[i] = post.lowPass (x[i]);
        finish (x, n);
    }

private:
    OnePole inputHp, track1, track2, subLp, bandLimit, post;
    float env = 0.0f, envAttack = 0.99f, envRelease = 0.999f;
    float sub = 0.5f, flip1 = 1.0f, flip2 = 1.0f;
    bool high = false;
};

//==============================================================================
/** Tycobrahe Octavia: an output transformer and two diodes rectify the signal, which adds the octave
    above, then a fuzz. Rings best with the neck pickup above the 12th fret. */
class OctaveFuzzModel : public DriveModel
{
public:
    void setParameters (const float* k) override
    {
        gain.setTargetValue (2.0f + 40.0f * shape::taper (drive01 (k), 2.5f));
        inputHp.setCutoff (os, 150.0);
        transformer.setCutoff (os, 30.0);
        bandLimit.setCutoff (os, 6000.0);
        post.setCutoff (fs, 5000.0);
        setEqAndOutput (k[0], k[1], k[2], k[3], k[4], 0.14f);
    }

    void reset() override { DriveModel::reset(); inputHp.reset(); transformer.reset(); bandLimit.reset(); post.reset(); }

    void processOversampled (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float v = std::tanh (gain.getNextValue() * inputHp.highPass (x[i]));
            const float folded = transformer.highPass (std::abs (v));
            x[i] = bandLimit.lowPass (std::tanh (6.0f * folded));
        }
    }

    void processBase (float* x, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
            x[i] = post.lowPass (x[i]);
        finish (x, n);
    }

private:
    OnePole inputHp, transformer, bandLimit, post;
};

//==============================================================================
/** The distortion engine of a slot: owns every model (so switching never allocates)
    and the 4x oversampler they share. */
class Distortion
{
public:
    enum Variant { tubeDrive = 0, screamer, overdrive, classicDist, heavyDist, colorDrive, buzzSaw,
                   facialFuzz, jumboFuzz, fuzzPi, jetFuzz, line6Drive, line6Dist, subOctaveFuzz, octaveFuzz,
                   numVariants };

    void prepare (double sampleRate, int maxBlockSize)
    {
        fs = sampleRate;
        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
            1, oversamplingOrder, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        oversampler->initProcessing ((size_t) maxBlockSize);

        models.clear();
        models.push_back (std::make_unique<TubeDriveModel>());
        models.push_back (std::make_unique<ScreamerModel>());
        models.push_back (std::make_unique<OverdriveModel>());
        models.push_back (std::make_unique<ClassicDistModel>());
        models.push_back (std::make_unique<HeavyDistModel>());
        models.push_back (std::make_unique<ColorDriveModel>());
        models.push_back (std::make_unique<BuzzSawModel>());
        models.push_back (std::make_unique<FacialFuzzModel>());
        models.push_back (std::make_unique<JumboFuzzModel>());
        models.push_back (std::make_unique<FuzzPiModel>());
        models.push_back (std::make_unique<JetFuzzModel>());
        models.push_back (std::make_unique<Line6DriveModel>());
        models.push_back (std::make_unique<Line6DistModel>());
        models.push_back (std::make_unique<SubOctaveFuzzModel>());
        models.push_back (std::make_unique<OctaveFuzzModel>());

        const float defaults[] = { 50.0f, 50.0f, 50.0f, 50.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        for (size_t i = 0; i < models.size(); ++i)
        {
            auto& m = models[i];
            m->loudnessTrim = loudnessTrims[i];
            m->prepare (fs, fs * (1 << oversamplingOrder));
            m->setParameters (defaults);
            m->reset();
        }
        reset();
    }

    void reset()
    {
        if (oversampler != nullptr)
            oversampler->reset();
        models[(size_t) current]->reset();
    }

    void setModel (int variant) noexcept { current = std::clamp (variant, 0, (int) numVariants - 1); }
    void setParameters (const float* knobs) { models[(size_t) current]->setParameters (knobs); }

    float getLatencySamples() const { return oversampler != nullptr ? oversampler->getLatencyInSamples() : 0.0f; }

    void process (float* data, int numSamples) noexcept
    {
        float* channels[] = { data };
        juce::dsp::AudioBlock<float> block (channels, 1, (size_t) numSamples);
        auto& model = *models[(size_t) current];

        auto up = oversampler->processSamplesUp (block);
        model.processOversampled (up.getChannelPointer (0), (int) up.getNumSamples());
        oversampler->processSamplesDown (block);
        model.processBase (data, numSamples);
    }

private:
    static constexpr int oversamplingOrder = 2; // 2^2 = 4x

    static constexpr float loudnessTrims[numVariants][5] = {
        { 8.1f, 2.4f, 0.0f, -1.2f, -1.8f },    // Tube Drive
        { 2.5f, 1.2f, -0.1f, -1.1f, -2.0f },   // Screamer
        { 14.1f, 3.2f, 0.1f, -1.2f, -1.9f },   // Overdrive
        { 16.1f, 1.4f, 0.1f, -0.3f, -0.4f },   // Classic Dist
        { 2.2f, 1.3f, 1.0f, 0.9f, 0.9f },      // Heavy Dist
        { 11.3f, 2.4f, 0.2f, -0.7f, -1.1f },   // Color Drive
        { 3.3f, 1.2f, 0.3f, -0.2f, -0.3f },    // Buzz Saw
        { 9.9f, 2.0f, 0.4f, -0.3f, -0.6f },    // Facial Fuzz
        { 3.0f, 0.3f, -0.2f, -0.3f, -0.4f },   // Jumbo Fuzz
        { 2.0f, 0.5f, 0.2f, 0.1f, 0.1f },      // Fuzz Pi
        { 7.4f, 2.9f, 1.4f, 0.6f, 0.4f },      // Jet Fuzz
        { 4.6f, -0.5f, -1.7f, -2.3f, -2.5f },  // Line 6 Drive
        { 1.6f, -0.1f, -0.4f, -0.5f, -0.5f },  // Line 6 Distortion
        { 5.9f, 0.6f, -1.9f, -3.2f, -3.9f },   // Sub Octave Fuzz
        { 7.0f, 3.0f, 0.8f, -0.4f, -0.8f },    // Octave Fuzz
    };

    double fs = 48000.0;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    std::vector<std::unique_ptr<DriveModel>> models;
    int current = 0;
};

} // namespace fx
