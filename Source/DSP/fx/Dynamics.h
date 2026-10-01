#pragma once

#include "../DspUtils.h"
#include "../ModelTypes.h"

namespace fx
{
namespace dyn_detail
{
    /** Linear up to `knee`, then bends over smoothly towards `ceiling` (an op-amp or tube running out of headroom). */
    inline float softClip (float v, float knee, float ceiling) noexcept
    {
        const float a = std::abs (v);
        if (a <= knee)
            return v;

        const float range = ceiling - knee;
        const float s = knee + range * std::tanh ((a - knee) / range);
        return v < 0.0f ? -s : s;
    }

    /** Per-sample step of a one-pole follower with the time constant `seconds`. */
    inline float stepFor (double seconds, double fs) noexcept
    {
        return (float) (1.0 - std::exp (-1.0 / (seconds * fs)));
    }

    // Tube Comp (LA-2A): the T4 cell's attack, its fast first release stage, and the slow "memory" stage
    constexpr double tubeAttack = 0.010, tubeRelease = 0.060, tubeLag = 0.004, tubeMemCharge = 0.8, tubeMemRelease = 2.5;
    constexpr float  tubeMemShare = 0.6f;       // how much of the gain reduction the slow stage can hold on to
    constexpr float  tubeDetector = 0.837f;     // what the 10 / 60 ms follower reads for a sine of peak 1
    constexpr double tubeReference = 0.15;      // input peak that the automatic make-up brings back to unity

    // Red Comp (Dyna Comp): output level the feedback loop holds, loop stiffness, cap charge / discharge
    constexpr float  redThreshold = 0.25f, redStiffness = 8.0f, redLevel = 0.62f;
    constexpr double redAttack = 0.005, redRelease = 0.45, redTone = 5500.0;
    constexpr float  redMaxGainDb = 34.0f;

    // Blue Comp (CS-1): photocoupler feedback loop
    constexpr float  blueThreshold = 0.22f, blueDetector = 0.934f, blueLevel = 0.56f;
    constexpr double blueAttack = 0.005, blueRelease = 0.15, blueLag = 0.003;
    constexpr float  blueMaxGainDb = 26.0f;

    // Vetta Comp / Vetta Juice: feed-forward, in decibels
    constexpr float  vettaRatio = 2.35f, vettaKnee = 6.0f, juiceThresholdDb = -44.0f, juiceKnee = 12.0f, juiceMinSlope = 0.1f;
    constexpr double vettaDetector = 0.010, vettaAttack = 0.008, vettaRelease = 0.15, juiceAttack = 0.003, juiceRelease = 0.25;

    // Boost Comp (Micro Amp + Dyna-style squash)
    constexpr float  boostThreshold = 0.30f, boostStiffness = 8.0f, boostCompGainDb = 20.0f, boostDriveDb = 26.0f, boostEqDb = 12.0f;
    constexpr double boostBassHz = 120.0, boostTrebleHz = 3000.0;

    // Added to the signal so that followers and filters settle at 1e-20 in silence instead of in the denormals
    // (which are slow, above all in the doubles of the JavaScript port). Far below anything audible.
    constexpr float tiny = 1.0e-20f, flushBelow = 1.0e-15f;
}

/** The HD500X's Dynamics models (without the Noise Gate, which is the older fx::NoiseGate). All mono.

    Hard Gate       Open Threshold, Close Threshold, Hold, Decay
    Tube Comp       Threshold, Level
    Red Comp        Sustain, Level
    Blue Comp       Sustain, Level
    Blue Comp Treb  Sustain, Level
    Vetta Comp      Sensitivity, Level
    Vetta Juice     Amount, Level
    Boost Comp      Drive, Bass, Comp, Treble, Output */
class DynamicsFx
{
public:
    enum Variant { hardGate = 0, tubeComp, redComp, blueComp, blueCompTreb, vettaComp, vettaJuice, boostComp, numVariants };

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        fs = sampleRate;

        for (auto* s : { &level, &pre, &side, &slope, &makeup, &drive, &bass, &treble })
            s->reset (fs, 0.03);

        dc.prepare (fs);
        redToneFilter.setCutoff (fs, dyn_detail::redTone);
        blueTrebleShelf.setHighShelf (fs, 3000.0, 7.0);
        bassFilter.setCutoff (fs, dyn_detail::boostBassHz);
        trebleFilter.setCutoff (fs, dyn_detail::boostTrebleHz);

        detectorHold = (int) (0.010 * fs);
        detectorRelease = dyn_detail::stepFor (0.002, fs);
        openStep = (float) (1.0 / (0.001 * fs));

        configure();
        reset();
    }

    void reset()
    {
        for (auto* s : { &level, &pre, &side, &slope, &makeup, &drive, &bass, &treble })
            s->setCurrentAndTarget (s->getTarget());

        env = mem = lag = cap = gainReduction = 0.0f;
        gateGain = 0.0f;
        gateOpen = false;
        holdCount = detectorCount = 0;
        dc.reset();
        redToneFilter.reset();
        blueTrebleShelf.reset();
        bassFilter.reset();
        trebleFilter.reset();
    }

    void setModel (int newVariant)
    {
        variant = std::clamp (newVariant, 0, (int) numVariants - 1);
        configure();
    }

    void setParameters (const float* k)
    {
        using namespace dyn_detail;

        switch (variant)
        {
            case hardGate:
            {
                openThreshold  = dbToGain (k[0]);
                closeThreshold = dbToGain (std::min (k[0], k[1])); // never above the open threshold
                holdSamples    = (int) std::lround ((double) k[2] * 0.001 * fs);
                // Decay: the time the gain takes to fall 60 dB once the hold has run out
                decayFactor    = (float) std::exp (-6.907755 / (std::max (1.0f, k[3]) * 0.001 * fs));
                break;
            }

            case tubeComp:
            {
                if (k[0] != cachedKnob)
                {
                    cachedKnob = k[0];

                    // The T4 cell's law: gain = 1 / (1 + (y / T)^2), with y the compressed signal (feedback), so
                    // y (1 + (y/T)^2) = x. Far above T that is 3:1; T is where the 3:1 line meets the 1:1 line.
                    const double threshold = std::pow (10.0, (double) k[0] / 20.0);
                    sideTarget = (float) (1.0 / (tubeDetector * threshold));

                    // Make-up tied to the threshold: whatever the cell takes off a signal at the reference level.
                    const double invT2 = 1.0 / (threshold * threshold);
                    double y = std::min (tubeReference, std::cbrt (tubeReference / invT2));
                    for (int i = 0; i < 6; ++i)
                        y -= (y + invT2 * y * y * y - tubeReference) / (1.0 + 3.0 * invT2 * y * y);
                    makeupTarget = (float) (tubeReference / y);
                }

                side.setTarget (sideTarget);
                makeup.setTarget (makeupTarget);
                level.setTarget (dbToGain (k[1]));
                break;
            }

            case redComp:
            {
                // Sustain raises the gain of the OTA; the loop then holds the output at its fixed level
                pre.setTarget (dbToGain (redMaxGainDb * k[0] * 0.01f));
                level.setTarget (dbToGain (k[1]) * redLevel);
                break;
            }

            case blueComp:
            case blueCompTreb:
            {
                pre.setTarget (dbToGain (blueMaxGainDb * k[0] * 0.01f));
                level.setTarget (dbToGain (k[1]) * blueLevel);
                break;
            }

            case vettaComp:
            {
                side.setTarget (-50.0f * k[0] * 0.01f);             // Sensitivity: threshold 0 ... -50 dBFS
                slope.setTarget (1.0f - 1.0f / vettaRatio);
                level.setTarget (dbToGain (k[1]));
                break;
            }

            case vettaJuice:
            {
                side.setTarget (juiceThresholdDb);
                slope.setTarget ((1.0f - juiceMinSlope) * k[0] * 0.01f); // Amount: 1:1 ... 10:1
                level.setTarget (dbToGain (k[1]));
                break;
            }

            default: // boostComp
            {
                const float amount = k[2] * 0.01f;
                drive.setTarget (dbToGain (boostDriveDb * k[0] * 0.01f));
                bass.setTarget (dbToGain (boostEqDb * (k[1] - 50.0f) * 0.02f));
                pre.setTarget (dbToGain (boostCompGainDb * amount));
                side.setTarget (amount * boostStiffness / boostThreshold);
                treble.setTarget (dbToGain (boostEqDb * (k[3] - 50.0f) * 0.02f));
                level.setTarget (dbToGain (k[4]));
                break;
            }
        }
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        switch (variant)
        {
            case hardGate:      processGate (left, right, numSamples); break;
            case tubeComp:      processTube (left, right, numSamples); break;
            case redComp:       processRed (left, right, numSamples); break;
            case blueComp:
            case blueCompTreb:  processBlue (left, right, numSamples); break;
            case vettaComp:
            case vettaJuice:    processVetta (left, right, numSamples); break;
            default:            processBoost (left, right, numSamples); break;
        }
    }

private:
    /** Time constants of the selected model. */
    void configure()
    {
        using namespace dyn_detail;
        cachedKnob = -1.0e9f;

        switch (variant)
        {
            case tubeComp:
                attack = stepFor (tubeAttack, fs);        release = stepFor (tubeRelease, fs);
                lagStep = stepFor (tubeLag, fs);
                memCharge = stepFor (tubeMemCharge, fs);  memRelease = stepFor (tubeMemRelease, fs);
                break;
            case redComp:
            case boostComp:
                attack = stepFor (redAttack, fs);         release = stepFor (redRelease, fs);
                break;
            case blueComp:
            case blueCompTreb:
                attack = stepFor (blueAttack, fs);        release = stepFor (blueRelease, fs);
                lagStep = stepFor (blueLag, fs);
                break;
            case vettaComp:
                attack = stepFor (vettaAttack, fs);       release = stepFor (vettaRelease, fs);
                lagStep = stepFor (vettaDetector, fs);    knee = vettaKnee;
                break;
            case vettaJuice:
                attack = stepFor (juiceAttack, fs);       release = stepFor (juiceRelease, fs);
                lagStep = stepFor (vettaDetector, fs);    knee = juiceKnee;
                break;
            default:
                break;
        }
    }

    //==============================================================================
    // Hard Gate (Line 6, HD only): a gate that opens above one threshold and closes below another, waits for the
    // Hold time, then fades out over the Decay time. It shuts completely - hence "lopped-off power chords".
    void processGate (float* left, float* right, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float x = 0.5f * (left[i] + right[i]);
            const float a = std::abs (x) + dyn_detail::tiny;

            // level detector: keeps each peak for 10 ms (a full cycle of the lowest notes), then lets go quickly,
            // so the reading does not ripple with the waveform and the Hold knob means what it says
            if (a >= env)                 { env = a; detectorCount = detectorHold; }
            else if (detectorCount > 0)   --detectorCount;
            else                          env -= env * detectorRelease;

            if (env >= openThreshold)
            {
                gateOpen = true;
                holdCount = holdSamples;
            }
            else if (gateOpen)
            {
                if (env >= closeThreshold)  holdCount = holdSamples;
                else if (holdCount > 0)     --holdCount;
                else                        gateOpen = false;
            }

            if (gateOpen)
            {
                gateGain = std::min (1.0f, gateGain + openStep); // opens within 1 ms: the pick attack survives
            }
            else
            {
                gateGain *= decayFactor;
                if (gateGain < 1.0e-5f)
                    gateGain = 0.0f;
            }

            left[i] = right[i] = x * gateGain;
        }
    }

    //==============================================================================
    // Tube Comp (Teletronix LA-2A). The T4 optical cell sits in a feedback loop: the side chain listens to the
    // already compressed signal. Modelled: the soft knee that ends up around 3:1, the ~10 ms attack, the release in
    // two stages (roughly half of it within 0.1 s, the rest over seconds, and the longer the cell was lit the more is
    // left to the slow stage), make-up gain that follows the threshold, and a tube output stage that adds a little
    // second harmonic and rounds off the peaks the cell is too slow to catch.
    void processTube (float* left, float* right, int numSamples) noexcept
    {
        using namespace dyn_detail;

        for (int i = 0; i < numSamples; ++i)
        {
            const float x = 0.5f * (left[i] + right[i]) + tiny;
            const float y = x / (1.0f + lag * lag);

            const float u = side.next() * std::abs (y);
            env += (u - env) * (u > env ? attack : release);

            const float memTarget = tubeMemShare * env;
            mem += (memTarget - mem) * (memTarget > mem ? memCharge : memRelease);

            lag += (std::max (env, mem) - lag) * lagStep; // the photoresistor lags behind the light

            float v = softClip (y * makeup.next(), 0.7f, 1.4f);
            v -= 0.06f * v * v;
            const float out = dc.process (v) * level.next();
            left[i] = right[i] = out;
        }
    }

    //==============================================================================
    // Red Comp (MXR Dyna Comp). A CA3080 OTA whose gain is pulled down by the rectified output (feedback): the
    // rectifier only conducts above its own threshold, so quiet notes get the full Sustain gain and everything
    // louder is squashed to one level (about 10:1). The cap charges within a few ms and discharges in about half a
    // second, which is the Dyna's sound: the pick attack pops through, the note is ducked right after it and then
    // blooms as the gain creeps back. The top end is rolled off like the original's.
    void processRed (float* left, float* right, int numSamples) noexcept
    {
        using namespace dyn_detail;

        for (int i = 0; i < numSamples; ++i)
        {
            const float x = 0.5f * (left[i] + right[i]) + tiny;
            const float y = softClip (x * pre.next() * std::exp (-cap), 0.6f, 1.0f); // runs into the rails on hard attacks

            const float over = (std::abs (y) - redThreshold) * (redStiffness / redThreshold);
            if (over > cap)  cap += (over - cap) * attack;
            else             cap -= cap * release;

            const float out = redToneFilter.lowPass (y) * level.next();
            left[i] = right[i] = out;
        }

        if (cap < flushBelow)
            cap = 0.0f;
    }

    //==============================================================================
    // Blue Comp / Blue Comp Treb (Boss CS-1). Feedback compressor around a photocoupler: a softer knee and a lower
    // ratio (about 4:1) than the Dyna Comp, a quicker release (~150 ms) and no ducking, so it evens out rather than
    // squashes. Sustain is the gain in front of the cell. "Treb" is the CS-1's treble switch: a lift above 3 kHz.
    void processBlue (float* left, float* right, int numSamples) noexcept
    {
        using namespace dyn_detail;
        const bool trebleSwitch = variant == blueCompTreb;

        for (int i = 0; i < numSamples; ++i)
        {
            const float x = 0.5f * (left[i] + right[i]) + tiny;
            const float y = softClip (x * pre.next() / (1.0f + lag * lag * lag), 0.6f, 1.0f);

            const float u = std::abs (y) * (1.0f / (blueDetector * blueThreshold));
            env += (u - env) * (u > env ? attack : release);
            lag += (env - lag) * lagStep;

            const float out = (trebleSwitch ? blueTrebleShelf.process (y) : y) * level.next();
            left[i] = right[i] = out;
        }
    }

    //==============================================================================
    // Vetta Comp and Vetta Juice (from the Line 6 Vetta II amp). Clean digital feed-forward compressors.
    // Vetta Comp: fixed 2.35:1, Sensitivity lowers the threshold, Level adds up to 12 dB.
    // Vetta Juice: fixed low threshold (-44 dBFS), Amount is the ratio (1:1 ... 10:1), Level has 30 dB of gain.
    void processVetta (float* left, float* right, int numSamples) noexcept
    {
        using namespace dyn_detail;
        const float halfKnee = 0.5f * knee;

        for (int i = 0; i < numSamples; ++i)
        {
            const float x = 0.5f * (left[i] + right[i]) + tiny;
            const float a = std::abs (x);
            env = a > env ? a : env - env * lagStep; // peak reading

            const float over = 20.0f * std::log10 (std::max (env, 1.0e-6f)) - side.next();
            float above = 0.0f;
            if (over >= halfKnee)        above = over;
            else if (over > -halfKnee)   above = (over + halfKnee) * (over + halfKnee) / (2.0f * knee);

            const float target = above * slope.next();
            gainReduction += (target - gainReduction) * (target > gainReduction ? attack : release);

            // (the ceiling is the unit's own full scale: only Level at its top with little compression gets there)
            const float out = softClip (x * level.next() * std::exp (-0.115129255f * gainReduction), 1.0f, 2.0f);
            left[i] = right[i] = out;
        }

        if (gainReduction < flushBelow)
            gainReduction = 0.0f;
    }

    //==============================================================================
    // Boost Comp (inspired by the MXR Micro Amp). One clean op-amp gain stage of up to 26 dB (Drive) that only
    // runs out of headroom at the very top, Bass and Treble shelves (50 % = flat), and in front of it a Dyna-style
    // feedback squash whose amount and gain both grow with Comp (Comp at 0 = no compression at all).
    void processBoost (float* left, float* right, int numSamples) noexcept
    {
        using namespace dyn_detail;

        for (int i = 0; i < numSamples; ++i)
        {
            const float x = 0.5f * (left[i] + right[i]) + tiny;
            float y = x * pre.next() * std::exp (-cap);

            const float over = (std::abs (y) - boostThreshold) * side.next();
            if (over > cap)  cap += (over - cap) * attack;
            else             cap -= cap * release;

            y *= drive.next();
            y += (bass.next() - 1.0f) * bassFilter.lowPass (y);
            y += (treble.next() - 1.0f) * trebleFilter.highPass (y);

            const float out = softClip (y, 1.0f, 1.6f) * level.next();
            left[i] = right[i] = out;
        }

        if (cap < flushBelow)
            cap = 0.0f;
    }

    //==============================================================================
    double fs = 48000.0;
    int variant = hardGate;

    Smoothed level { 1.0f }, pre { 1.0f }, side { 0.0f }, slope { 0.0f }, makeup { 1.0f }, drive { 1.0f }, bass { 1.0f }, treble { 1.0f };
    float cachedKnob = -1.0e9f, sideTarget = 0.0f, makeupTarget = 1.0f;

    // time constants of the selected model (per-sample steps)
    float attack = 0.0f, release = 0.0f, lagStep = 0.0f, memCharge = 0.0f, memRelease = 0.0f, knee = 6.0f;

    // compressor state
    float env = 0.0f, mem = 0.0f, lag = 0.0f, cap = 0.0f, gainReduction = 0.0f;

    // gate
    float openThreshold = 0.01f, closeThreshold = 0.005f, decayFactor = 0.999f, openStep = 0.02f, detectorRelease = 0.01f, gateGain = 0.0f;
    int holdSamples = 0, holdCount = 0, detectorHold = 0, detectorCount = 0;
    bool gateOpen = false;

    DcBlocker dc;
    OnePole redToneFilter, bassFilter, trebleFilter;
    Biquad blueTrebleShelf;
};

/** In the order of DynamicsFx::Variant. */
inline std::vector<ModelInfo> dynamicsModels()
{
    const auto model = [] (const char* key, const char* name, int variant, const char* basedOn, std::vector<KnobSpec> knobs)
    {
        return ModelInfo { key, name, Category::dynamics, Engine::dynamicsFx, variant, basedOn, std::move (knobs) };
    };

    return {
        model ("hard_gate", "Hard Gate", DynamicsFx::hardGate, "Line 6 original: gate with separate open / close thresholds, hold and decay",
               { decibels ("Open Threshold", -90.0f, 0.0f, -45.0f, 0.5f), decibels ("Close Threshold", -90.0f, 0.0f, -55.0f, 0.5f),
                 millis ("Hold", 0.0f, 1000.0f, 50.0f, 150.0f), millis ("Decay", 1.0f, 2000.0f, 80.0f, 200.0f) }),
        model ("tube_comp", "Tube Comp", DynamicsFx::tubeComp, "Teletronix LA-2A",
               { decibels ("Threshold", -40.0f, 0.0f, -20.0f, 0.5f), decibels ("Level", -30.0f, 12.0f, 0.0f) }),
        model ("red_comp", "Red Comp", DynamicsFx::redComp, "MXR Dyna Comp",
               { percent ("Sustain", 50.0f), decibels ("Level", -30.0f, 12.0f, 0.0f) }),
        model ("blue_comp", "Blue Comp", DynamicsFx::blueComp, "Boss CS-1 Compression Sustainer, treble switch off",
               { percent ("Sustain", 50.0f), decibels ("Level", -30.0f, 12.0f, 0.0f) }),
        model ("blue_comp_treb", "Blue Comp Treb", DynamicsFx::blueCompTreb, "Boss CS-1 Compression Sustainer, treble switch on",
               { percent ("Sustain", 50.0f), decibels ("Level", -30.0f, 12.0f, 0.0f) }),
        model ("vetta_comp", "Vetta Comp", DynamicsFx::vettaComp, "Line 6 Vetta II compressor (fixed 2.35:1)",
               { percent ("Sensitivity", 50.0f), decibels ("Level", -30.0f, 12.0f, 5.0f) }),
        model ("vetta_juice", "Vetta Juice", DynamicsFx::vettaJuice, "Line 6 Vetta II compressor (variable ratio, 30 dB of gain)",
               { percent ("Amount", 50.0f), decibels ("Level", 0.0f, 30.0f, 13.0f) }),
        model ("boost_comp", "Boost Comp", DynamicsFx::boostComp, "Inspired by the MXR Micro Amp, plus Dyna-style compression",
               { percent ("Drive", 20.0f), percent ("Bass", 50.0f), percent ("Comp", 25.0f), percent ("Treble", 50.0f),
                 decibels ("Output", -30.0f, 12.0f, -5.0f) }),
    };
}

} // namespace fx
