#pragma once

#include "../DspUtils.h"
#include "../ModelTypes.h"

namespace fx
{
namespace wah_detail
{
    /** What makes one wah: where its resonance sits with the pedal back (heel) and forward (toe), how sharp and
        how loud the peak is at each end, and how much of the signal below the resonance still gets through.

        An inductor wah is a resonant low-pass at heart (flat below the peak, 12 dB/octave above it) with the
        band-pass peak on top; `lowDb` is the level of that flat part. The inductor-less Colorsound is a pure
        band-pass (6 dB/octave on both sides) and has no such part. `couplingHz` is the input cap's bass cut. */
    struct Voice
    {
        double heelHz, toeHz, taper;   // taper > 1: more of the pedal's travel is spent in the low part of the sweep
        double heelQ, toeQ;
        double heelPeakDb, toePeakDb;
        double heelLowDb, toeLowDb;
        bool   lowPassPath;
        double couplingHz;
    };

    inline constexpr Voice voices[] =
    {
        // Fassel - Cry Baby Super / Jen Super with the Fasel inductor: the sweet, vocal one. A strong peak that
        // stays fairly narrow all the way and opens up a little brighter at the toe.
        { 410.0, 2050.0, 1.0,   6.0, 4.2,   11.0, 13.0,   -12.0, -17.0,   true, 110.0 },

        // Conductor - Maestro Boomerang: the "wow-wow". Sits low, a broad peak, plenty of bass left in.
        { 300.0, 1350.0, 1.1,   4.0, 3.0,   10.5, 11.0,    -7.5, -11.5,   true,  60.0 },

        // Throaty - RMC Real McCoy (Clyde McCoy clone, halo inductor): growls in the low mids, the sharpest
        // peak of the inductor wahs at the heel, never gets as bright as a Cry Baby.
        { 330.0, 1750.0, 0.9,   7.0, 4.8,   13.0, 13.0,   -11.0, -16.0,   true,  90.0 },

        // Colorful - Colorsound Wah-Fuzz-Swell's wah: no inductor, a transistor band-pass whose one swept
        // resistor changes frequency, Q and gain together - wide and mild at the heel, a thin piercing whistle
        // at the toe, over more than three octaves, with no bass passed around the filter.
        { 280.0, 2400.0, 1.2,   2.2, 8.5,    6.0, 16.0,  -100.0, -100.0, false, 120.0 },

        // Vetta Wah - Line 6 original: a modern, even filter. Same Q and same gain over a very wide sweep.
        { 300.0, 2800.0, 1.0,   4.5, 4.5,   10.5, 10.5,   -14.0, -14.0,   true,  40.0 },

        // Chrome - Vox V847: the short, mellow sweep (it stops at 1.6 kHz), peak getting broader towards the toe.
        { 440.0, 1600.0, 1.0,   5.5, 3.6,   11.0, 11.5,   -11.0, -15.0,   true, 100.0 },

        // Chrome Custom - modded V847 (more first-stage gain, other inductor, wider Q, 470k pot): a longer sweep
        // at both ends, a broad peak, louder and fuller.
        { 360.0, 2300.0, 1.35,  3.4, 2.6,   11.5, 12.5,    -9.0, -13.0,   true,  60.0 },

        // Weeper - Arbiter / Dunlop Cry Baby: the sharp, thin one. Narrow peak, the least bass, and a toe
        // position that bites.
        { 350.0, 2200.0, 1.0,   7.5, 5.0,   12.0, 15.0,   -14.0, -20.0,   true, 150.0 },
    };

    constexpr int tick = 16; // samples between coefficient updates (interpolated in between)
    constexpr float tiny = 1.0e-20f;
}

/** The HD500X's eight wahs. Stereo: one filter per side. Knobs: Position (0 = heel, 100 = toe), Mix.
    There is no pedal here: Position is a knob (or host automation), so it glides (20 ms) and the filter
    coefficients are interpolated sample by sample - fast moves give a fast sweep, never steps. */
class WahFx
{
public:
    enum Variant { fassel = 0, conductor, throaty, colorful, vettaWah, chrome, chromeCustom, weeper, numVariants };

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        fs = sampleRate;
        position.reset (fs, 0.02);
        mix.reset (fs, 0.03);
        configure();
        reset();
    }

    void reset()
    {
        position.setCurrentAndTarget (position.getTarget());
        mix.setCurrentAndTarget (mix.getTarget());

        computeTargets (position.getTarget());
        g = gTarget;  k = kTarget;  lowGain = lowTarget;  bandGain = bandTarget;
        gStep = kStep = lowStep = bandStep = 0.0f;
        tickCount = 0;
        moving = modelChanged = false;

        for (int ch = 0; ch < 2; ++ch)
        {
            ic1[ch] = ic2[ch] = 0.0f;
            coupling[ch].reset();
        }
    }

    void setModel (int newVariant)
    {
        variant = std::clamp (newVariant, 0, (int) numVariants - 1);
        configure();
    }

    void setParameters (const float* knobs)
    {
        position.setTarget (std::clamp (knobs[0] * 0.01f, 0.0f, 1.0f));
        mix.setTarget (std::clamp (knobs[1] * 0.01f, 0.0f, 1.0f));
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        using wah_detail::tiny;
        float* const channels[2] = { left, right };

        for (int pos = 0; pos < numSamples;)
        {
            if (tickCount == 0)
                nextTick();

            const int n = std::min (tickCount, numSamples - pos);

            for (int i = pos; i < pos + n; ++i)
            {
                g += gStep;  k += kStep;  lowGain += lowStep;  bandGain += bandStep;
                const float a1 = 1.0f / (1.0f + g * (g + k));
                const float a2 = g * a1;
                const float a3 = g * a2;
                const float wetMix = mix.next();

                for (int ch = 0; ch < 2; ++ch)
                {
                    const float dry = channels[ch][i];
                    const float x = coupling[ch].highPass (dry + tiny) + tiny; // (tiny: no denormals in the tails)

                    // state-variable filter (trapezoidal integrators: stays stable while it is swept)
                    const float v3 = x - ic2[ch];
                    const float v1 = a1 * ic1[ch] + a2 * v3;       // band-pass
                    const float v2 = ic2[ch] + a2 * ic1[ch] + a3 * v3; // low-pass
                    ic1[ch] = 2.0f * v1 - ic1[ch];
                    ic2[ch] = 2.0f * v2 - ic2[ch];

                    const float wet = lowGain * v2 + bandGain * v1;
                    channels[ch][i] = dry + wetMix * (wet - dry);
                }
            }

            pos += n;
            tickCount -= n;
        }
    }

private:
    void configure()
    {
        for (auto& c : coupling)
            c.setCutoff (fs, wah_detail::voices[variant].couplingHz);

        modelChanged = true;
    }

    /** Filter settings for a pedal position (0..1). */
    void computeTargets (float pedal) noexcept
    {
        const auto& v = wah_detail::voices[variant];
        const double t = std::pow ((double) pedal, v.taper);
        const double hz = std::min (v.heelHz * std::pow (v.toeHz / v.heelHz, t), 0.45 * fs);
        const double q = v.heelQ * std::pow (v.toeQ / v.heelQ, t);
        const double peak = std::pow (10.0, (v.heelPeakDb + (v.toePeakDb - v.heelPeakDb) * t) / 20.0);
        const double low = v.lowPassPath ? std::pow (10.0, (v.heelLowDb + (v.toeLowDb - v.heelLowDb) * t) / 20.0) : 0.0;

        // At the resonance the band-pass output is Q times the input and the low-pass output Q times too (90
        // degrees apart): pick the band-pass share so the two together give the wanted peak.
        const double band = std::sqrt (std::max (0.0, (peak / q) * (peak / q) - low * low));

        gTarget = (float) std::tan (pi * hz / fs);
        kTarget = (float) (1.0 / q);
        lowTarget = (float) low;
        bandTarget = (float) band;
    }

    void nextTick() noexcept
    {
        using wah_detail::tick;
        tickCount = tick;

        if (moving) // land exactly on what the last ramp aimed at
        {
            g = gTarget;  k = kTarget;  lowGain = lowTarget;  bandGain = bandTarget;
            gStep = kStep = lowStep = bandStep = 0.0f;
            moving = false;
        }

        if (position.isSmoothing() || modelChanged)
        {
            modelChanged = false;
            computeTargets (position.skip (tick));
            gStep = (gTarget - g) * (1.0f / (float) tick);
            kStep = (kTarget - k) * (1.0f / (float) tick);
            lowStep = (lowTarget - lowGain) * (1.0f / (float) tick);
            bandStep = (bandTarget - bandGain) * (1.0f / (float) tick);
            moving = true;
        }
    }

    double fs = 48000.0;
    int variant = fassel;
    Smoothed position { 0.5f }, mix { 1.0f };

    float g = 0.1f, k = 0.2f, lowGain = 0.0f, bandGain = 1.0f;             // current filter settings
    float gTarget = 0.1f, kTarget = 0.2f, lowTarget = 0.0f, bandTarget = 1.0f;
    float gStep = 0.0f, kStep = 0.0f, lowStep = 0.0f, bandStep = 0.0f;
    int tickCount = 0;
    bool moving = false, modelChanged = true;

    float ic1[2] {}, ic2[2] {};
    OnePole coupling[2];
};

/** In the order of WahFx::Variant. */
inline std::vector<ModelInfo> wahModels()
{
    const auto model = [] (const char* key, const char* name, int variant, const char* basedOn)
    {
        return ModelInfo { key, name, Category::wah, Engine::wahFx, variant, basedOn,
                           { percent ("Position", 50.0f), percent ("Mix", 100.0f) } };
    };

    return {
        model ("fassel",        "Fassel",        WahFx::fassel,       "Dunlop Cry Baby Super / Jen Super Cry Baby (Fasel inductor)"),
        model ("conductor",     "Conductor",     WahFx::conductor,    "Maestro Boomerang"),
        model ("throaty",       "Throaty",       WahFx::throaty,      "RMC Real McCoy 1"),
        model ("colorful",      "Colorful",      WahFx::colorful,     "Colorsound Wah-Fuzz (wah section, inductor-less)"),
        model ("vetta_wah",     "Vetta Wah",     WahFx::vettaWah,     "Line 6 original (Vetta II)"),
        model ("chrome",        "Chrome",        WahFx::chrome,       "Vox V847"),
        model ("chrome_custom", "Chrome Custom", WahFx::chromeCustom, "Modded Vox V847"),
        model ("weeper",        "Weeper",        WahFx::weeper,       "Arbiter Cry Baby"),
    };
}

} // namespace fx
