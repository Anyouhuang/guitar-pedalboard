#pragma once

#include <cstdint>

#include "../DspUtils.h"
#include "../ModelTypes.h"

namespace fx
{
inline const char* const delayFxHeads12Names[] = { "Off", "1", "2", "1+2" };
inline const char* const delayFxHeads34Names[] = { "Off", "3", "4", "3+4" };
inline const char* const delayFxBitNames[]     = { "6 bit", "7 bit", "8 bit", "9 bit", "10 bit", "11 bit", "12 bit", "13 bit", "14 bit", "15 bit",
                                                   "16 bit", "17 bit", "18 bit", "19 bit", "20 bit", "21 bit", "22 bit", "23 bit", "24 bit" };
inline constexpr int numDelayFxBitNames = 19;

namespace delayfx_detail
{
    constexpr double twoPi = 6.283185307179586;
    constexpr double ln2 = 0.6931471805599453;

    /** Linear parameter ramp kept in double, so the JavaScript port steps through exactly the same values
        (the quantiser of Lo Res Delay and the swell trigger must take the same decisions in both). */
    struct Ramp
    {
        void prepare (double fs, double seconds) noexcept { steps = std::max (1, (int) (seconds * fs)); }

        void set (double v) noexcept
        {
            if (v == target)
                return;

            target = v;
            countdown = steps;
            step = (target - current) / (double) steps;
        }

        void snap() noexcept { current = target; countdown = 0; }

        double next() noexcept
        {
            if (countdown > 0)
            {
                if (--countdown == 0) current = target;
                else                  current += step;
            }
            return current;
        }

        bool isZero() const noexcept { return countdown == 0 && current == 0.0; }

        double current = 0.0, target = 0.0, step = 0.0;
        int countdown = 0, steps = 1;
    };

    /** Circular delay line over a piece of the engine's memory. `read (d)` / `readInt (d)` give the sample pushed
        `d` pushes ago (the most recent push is d = 1). */
    struct Line
    {
        float* data = nullptr;
        int size = 0, writePos = 0;

        void push (float x) noexcept
        {
            data[writePos] = x;
            if (++writePos == size)
                writePos = 0;
        }

        float readInt (int d) const noexcept
        {
            int i = writePos - d;
            if (i < 0) i += size;
            return data[i];
        }

        /** 4-point Hermite; an integer `d` returns the stored sample exactly. */
        float read (double d) const noexcept
        {
            if (d < 2.0) d = 2.0;
            else if (d > (double) (size - 4)) d = (double) (size - 4);

            const int di = (int) d;
            const float frac = (float) (d - (double) di);
            int i0 = writePos - di;  if (i0 < 0) i0 += size;
            int im = i0 + 1;         if (im >= size) im -= size;
            int i1 = i0 - 1;         if (i1 < 0) i1 += size;
            int i2 = i1 - 1;         if (i2 < 0) i2 += size;

            const float xm1 = data[im], x0 = data[i0], x1 = data[i1], x2 = data[i2];
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return ((c3 * frac + c2) * frac + c1) * frac + x0;
        }
    };

    /** One read position of a digital delay. A new time is reached by crossfading to a second read
        position (no pitch bend, no click), one crossfade at a time. Times are whole samples. */
    struct Tap
    {
        double current = 9600.0, next = 9600.0, target = 9600.0;
        int count = 0; // samples left of the running crossfade
    };

    /** Transparent up to +-1, then bends over towards +-1.5: keeps a digital loop at 100 % feedback bounded. */
    inline float softLimit (float x) noexcept
    {
        const float a = std::abs (x);
        if (a <= 1.0f)
            return x;

        const float over = a - 1.0f, y = 1.0f + over / (1.0f + 2.0f * over);
        return x < 0.0f ? -y : y;
    }

    inline double softLimit (double x) noexcept
    {
        const double a = std::abs (x);
        if (a <= 1.0)
            return x;

        const double over = a - 1.0, y = 1.0 + over / (1.0 + 2.0 * over);
        return x < 0.0 ? -y : y;
    }

    /** tanh with a bias: tape (bias 0) or a tube / FET / BBD stage (asymmetric, even harmonics).
        Unity gain for small signals, so the loop gain stays what the Fdbk knob says. */
    struct Saturator
    {
        void set (double drive, double bias) noexcept
        {
            const double o = std::tanh (drive * bias);
            gain = (float) drive;
            shift = (float) (drive * bias);
            offset = std::tanh (shift); // the same call as in process(): silence in gives exactly silence out
            norm = (float) (1.0 / (drive * (1.0 - o * o)));
        }

        float process (float x) const noexcept { return (std::tanh (gain * x + shift) - offset) * norm; }

        float gain = 1.0f, shift = 0.0f, offset = 0.0f, norm = 1.0f;
    };

    /** Peak filter (the same response as Biquad::setPeak) as a state-variable filter: it keeps its precision in
        float when the centre is very low compared with the sample rate (a 100 Hz head bump at 192 kHz). */
    struct Bell
    {
        void set (double fs, double hz, double q, double gainDb) noexcept
        {
            const double A = std::pow (10.0, gainDb / 40.0), k = 1.0 / (q * A);
            const double g = std::tan (pi * std::clamp (hz, 1.0, 0.49 * fs) / fs), k1 = 1.0 / (1.0 + g * (g + k));
            a1 = (float) k1;
            a2 = (float) (g * k1);
            a3 = (float) (g * g * k1);
            peak = (float) (k * (A * A - 1.0));
        }

        void reset() noexcept { s1 = s2 = 0.0f; }

        float process (float x) noexcept
        {
            const float v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
            s1 = 2.0f * v1 - s1;
            s2 = 2.0f * v2 - s2;
            return x + peak * v1;
        }

        float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, peak = 0.0f, s1 = 0.0f, s2 = 0.0f;
    };

    /** What makes each tape / platter / BBD echo its own: the playback filters, the saturation in the
        record path, the transport's wow and flutter, and the preamp the dry signal passes through. */
    struct Voicing
    {
        double lowPassHz, lowPassQ, highPassHz;  // playback: high-frequency loss and coupling caps, once per repeat
        double bumpHz, bumpDb, bumpQ;            // head bump (or the platter's presence peak); 0 dB = none
        double preHz;                            // BBD anti-alias filter before the line; 0 = none
        double driveMin, driveMax, bias;         // record-path saturation (the Drive knob runs min..max)
        double wowHz[3], wowDeviation[3], drift; // transport speed errors at 100 % (fraction of the speed)
        int dry;                                 // preamp colouring the dry signal: 0 none, 1 EP-1, 2 EP-3, 3 Echorec
    };

    inline constexpr Voicing voicingEp1     { 3800.0, 0.65, 85.0,  120.0, 2.5, 0.9,  0.0,    0.7, 3.2, 0.12,   { 0.55, 1.9, 7.3 }, { 0.0055, 0.0022, 0.002 }, 0.0022, 1 };
    inline constexpr Voicing voicingEp3     { 4600.0, 0.70, 65.0,  100.0, 2.0, 1.0,  0.0,    0.9, 0.9, 0.03,   { 0.55, 1.9, 7.3 }, { 0.0055, 0.0022, 0.002 }, 0.0022, 2 };
    inline constexpr Voicing voicingSweep   { 3800.0, 0.65, 85.0,  120.0, 2.5, 0.9,  0.0,    1.4, 1.4, 0.12,   { 0.55, 1.9, 7.3 }, { 0.0055, 0.0022, 0.002 }, 0.0022, 1 };
    inline constexpr Voicing voicingPlatter { 5000.0, 0.70, 160.0, 1800.0, 1.5, 0.8, 0.0,    0.6, 2.6, 0.10,   { 0.37, 2.6, 9.5 }, { 0.003, 0.006, 0.003 },   0.002,  3 };
    inline constexpr Voicing voicingDmm     { 3600.0, 0.75, 60.0,  0.0, 0.0, 1.0,    5000.0, 1.15, 1.15, 0.08, { 1.0, 1.0, 1.0 },  { 0.0, 0.0, 0.0 },         0.0,    0 };
    inline constexpr Voicing voicingDm2     { 2900.0, 0.80, 110.0, 0.0, 0.0, 1.0,    4000.0, 1.7, 1.7, 0.12,   { 1.0, 1.0, 1.0 },  { 0.0, 0.0, 0.0 },         0.0,    0 };
    inline constexpr Voicing voicingSwell   { 5000.0, 0.70, 50.0,  100.0, 1.5, 1.0,  0.0,    0.9, 0.9, 0.0,    { 0.55, 1.9, 7.3 }, { 0.0055, 0.0022, 0.002 }, 0.0022, 0 };
    inline constexpr Voicing voicingRe101   { 4200.0, 0.70, 95.0,  95.0, 3.0, 1.0,   0.0,    1.1, 1.1, 0.05,   { 0.45, 1.3, 8.9 }, { 0.005, 0.0025, 0.002 },  0.0025, 0 };

    // Bass / Treble on the repeats (Digital Delay, Tape Echo, Analog Echo): first-order shelves, +-9 dB, whose
    // transitions are centred on these frequencies for boost and cut alike
    constexpr double eqRangeDb = 9.0, eqBassHz = 250.0, eqTrebleHz = 2500.0;

    // Sweep Echo's filter: a resonant wah-like sweep around `sweepCentreHz`, up to +-`sweepOctaves` at full depth
    constexpr double sweepCentreHz = 800.0, sweepOctaves = 2.2, sweepDamping = 0.4;

    // Auto-Volume Echo: a note starts above `swellOn`, the swell closes again below `swellOff`; while notes ring
    // into each other, a new pick is a fast envelope `swellOnsetRatio` times above the slow one
    constexpr double swellOn = 0.006, swellOff = 0.002, swellOnsetRatio = 1.6;

    // Multi-Head: where the four playback heads sit, as a fraction of Time (head 4), and the level at which
    // the playback mixer starts to run out of headroom (several heads in runaway add up)
    constexpr double headRatio[4] = { 0.25, 0.5, 0.75, 1.0 };
    constexpr float headMixKnee = 0.6f;

    // Tape, platter and BBD echoes regenerate past unity at the top of the Fdbk knob, like the originals: the
    // knob is the loop gain up to `regenKnee`, then rises to `regenMax` at 100 % (the saturation bounds the runaway)
    constexpr double regenKnee = 0.85, regenMax = 1.2;
}

/** The HD500X's 19 Delay models. Returns the wet signal only (the slot mixes it with the dry signal, see
    getMix()); `processDry` adds the original unit's preamp colour to the slot's dry signal for the four echo
    units that have one.

    Knobs: Time, Note, Fdbk, <two of the model's own>, Mix. The Note knob is ignored here (the caller has
    already put the tempo-synced time into Time). Stereo Delay: L Time, L Note, L Fdbk, R Time, R Note, R Fdbk, Mix.

    Ping Pong         Offset, Spread      stereo effect on the mono sum: L echo -> R echo -> back to L
    Dynamic Dly       Thresh, Ducking     TC 2290: the echoes are turned down while you play
    Stereo Delay      (see above)         two independent clean delays
    Digital Delay     Bass, Treble        clean repeats, shelving EQ on the repeats
    Dig Dly W/Mod     ModSpd, Depth       chorus on the repeats (quadrature L/R)
    Reverse           ModSpd, Depth       chunks of length Time played backwards (mono effect)
    Lo Res Delay      Tone, Res           the line is quantised to 6..24 bits
    Tube Echo (Dry)   Wow/Flt, Drive      Echoplex EP-1: tape loop, tube preamp
    Tape Echo (Dry)   Bass, Treble        Echoplex EP-3: tape loop, solid state, cleaner
    Sweep Echo (Dry)  Swp Spd, Swp Dep    EP-1 tone plus a swept resonant filter on the repeats
    Echo Platter (Dry) Wow/Flt, Drive     Binson Echorec: magnetic drum, tube electronics
    Analog W/Mod      ModSpd, Depth       Deluxe Memory Man: BBD with chorus / vibrato on the echoes
    Analog Echo       Bass, Treble        Boss DM-2: dark, gritty BBD
    Auto-Volume Echo  ModDep, Swell       each note is faded in, plus a tape-style echo (see getMix())
    Multi-Head        Heads 1-2, 3-4      Space Echo: one tape, four heads at 1/4, 2/4, 3/4 and 4/4 of Time (mono effect)

    Time changes: the digital models crossfade to the new time; the tape, platter and BBD models glide
    (pitch bends like a motor or a clock changing speed); Reverse takes the new time at its next chunk. */
class DelayFx
{
public:
    enum Variant { pingPong = 0, dynamicDly, stereoDelay, digitalDelay, digMod, reverse, loRes, tubeEcho, tubeEchoDry,
                   tapeEcho, tapeEchoDry, sweepEcho, sweepEchoDry, echoPlatter, echoPlatterDry, analogMod, analogEcho,
                   autoVolume, multiHead, numVariants };

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        using namespace delayfx_detail;
        fs = sampleRate;

        // 2 s of delay plus room for wow / chorus; Reverse reads up to twice its chunk length back,
        // so it uses both halves as one line
        lineSize = (int) (2.1 * fs) + 64;
        memory.assign ((size_t) lineSize * 2, 0.0f);
        line[0].data = memory.data();             line[0].size = lineSize;
        line[1].data = memory.data() + lineSize;  line[1].size = lineSize;
        whole.data   = memory.data();             whole.size   = lineSize * 2;

        xfadeLength = std::max (1, (int) (0.04 * fs));
        xfadeStep = 1.0f / (float) xfadeLength;
        reverseFadeMax = std::max (1, (int) (0.02 * fs));

        for (Ramp* r : { &feedback[0], &feedback[1], &modAmp, &bassGain, &trebleGain, &bassCoef, &trebleCoef, &spreadDirect, &spreadCross,
                         &wowAmp[0], &wowAmp[1], &wowAmp[2], &driftAmp, &sweepDepth, &sweepMix, &directGain, &echoGain, &outGain,
                         &headGain[0], &headGain[1], &headGain[2], &headGain[3] })
            r->prepare (fs, 0.04);

        glideCoef = stepFor (0.2);
        duckAttack = stepFor (0.002);   duckRelease = stepFor (0.12);
        duckClose = stepFor (0.012);    duckOpen = stepFor (0.3);
        swellFastRelease = (float) std::exp (-1.0 / (0.03 * fs));
        swellSlowAttack = stepFor (0.03);  swellSlowRelease = stepFor (0.15);
        swellDipStep = 1.0 / (0.004 * fs);
        swellCloseStep = 1.0 / (0.15 * fs);
        swellHoldSamples = (int) (0.08 * fs);
        noiseInterval = std::max (1, (int) (fs / 40.0));
        noiseCoef = (float) (1.0 - std::exp (-twoPi * 1.2 / fs));

        updateVoicing();
        setParameters (knobs); // everything measured in samples depends on the rate
        reset();
    }

    void reset()
    {
        using namespace delayfx_detail;
        std::fill (memory.begin(), memory.end(), 0.0f);
        line[0].writePos = line[1].writePos = whole.writePos = 0;

        for (int c = 0; c < 2; ++c)
        {
            lowPass[c].reset();  highPass[c].reset();  bump[c].reset();  preFilter[c].reset();
            shelfLow[0][c] = shelfLow[1][c] = shelfHigh[0][c] = shelfHigh[1][c] = 0.0f;
            dryEq[c].reset();    dryHighPass[c].reset(); dryLowPass[c].reset();
            sweep1[c] = sweep2[c] = 0.0f;
            toneState[c] = 0.0;
            tap[c].current = tap[c].next = tap[c].target;
            tap[c].count = 0;
        }

        glide = timeTarget;
        lfoPhase = sweepPhase = 0.0;
        wowPhase[0] = wowPhase[1] = wowPhase[2] = 0.0;
        noiseState = 0x2545f491u;
        noiseCount = 0;
        noiseTarget = noise1 = noise2 = 0.0;

        duckEnv = 0.0;
        duckGain = 1.0;
        swellFast = swellSlow = swellPos = swellGain = 0.0;
        swellOpen = swellDipping = false;
        swellHold = 0;

        reversePos = 0;
        reverseLength = reversePrevious = reverseTarget;
        reverseFade = std::min (reverseLength / 4, reverseFadeMax);

        updateBlock (0, true);

        for (Ramp* r : { &feedback[0], &feedback[1], &modAmp, &bassGain, &trebleGain, &bassCoef, &trebleCoef, &spreadDirect, &spreadCross,
                         &wowAmp[0], &wowAmp[1], &wowAmp[2], &driftAmp, &sweepDepth, &sweepMix, &directGain, &echoGain, &outGain,
                         &headGain[0], &headGain[1], &headGain[2], &headGain[3] })
            r->snap();
    }

    void setModel (int newVariant) noexcept
    {
        variant = std::clamp (newVariant, 0, (int) numVariants - 1);
        updateVoicing();
    }

    void setParameters (const float* k)
    {
        using namespace delayfx_detail;
        const bool twoTimes = variant == stereoDelay;
        const double seconds = toSeconds (k[0]);
        const double fb = toUnit (k[2]);
        const double a = twoTimes ? 0.0 : (double) k[3], b = twoTimes ? 0.0 : (double) k[4];
        const double mixValue = toUnit (k[twoTimes ? 6 : 5]);
        const bool regenerates = variant >= tubeEcho && variant != autoVolume;
        const double loopGain = regenerates && fb > regenKnee ? regenKnee + (fb - regenKnee) * (regenMax - regenKnee) / (1.0 - regenKnee) : fb;
        double first = seconds, spacing = seconds, tailFeedback = std::min (1.0, loopGain); // for getTailSeconds()
        double fbRight = fb, wowDepth = 0.0, loopScale = 1.0;

        if (k != knobs)
            std::copy (k, k + numKnobsRead, knobs);

        mix = (float) mixValue;
        timeTarget = seconds * fs;
        tap[0].target = tap[1].target = std::floor (seconds * fs + 0.5);

        switch (variant)
        {
            case pingPong:
            {
                // Offset: the right delay as a percentage of the left one. Spread: mono .. hard left / right.
                const double right = std::max (0.001, seconds * toUnit (k[3]));
                const double angle = (1.0 - toUnit (k[4])) * (pi / 4.0);
                tap[1].target = std::floor (right * fs + 0.5);
                spreadDirect.set (std::cos (angle));
                spreadCross.set (std::sin (angle));
                first = spacing = seconds + right;
                break;
            }

            case dynamicDly:
            {
                // Thresh: -60 .. 0 dB. Ducking: how far the echoes are turned down (100 % = muted).
                const double keep = 1.0 - toUnit (k[4]);
                duckThreshold = std::pow (10.0, (-60.0 + 0.6 * std::clamp (a, 0.0, 100.0)) / 20.0);
                duckDepth = 1.0 - keep * keep;
                break;
            }

            case stereoDelay:
            {
                const double secondsRight = toSeconds (k[3]);
                fbRight = toUnit (k[5]);
                tap[1].target = std::floor (secondsRight * fs + 0.5);
                if (secondsRight * (1.0 + repeats (fbRight)) > seconds * (1.0 + repeats (fb)))
                {
                    first = spacing = secondsRight;
                    tailFeedback = fbRight;
                }
                break;
            }

            case digitalDelay:
            case tapeEcho:
            case tapeEchoDry:
            case analogEcho:
            {
                // Shelves, flat at 50 %. A cut sits inside the loop (every repeat loses a little more, as on the
                // originals), a boost behind it (every repeat gets it once), so the loop gain never exceeds Fdbk.
                const double bassDb   = (std::clamp (a, 0.0, 100.0) - 50.0) / 50.0 * eqRangeDb;
                const double trebleDb = (std::clamp (b, 0.0, 100.0) - 50.0) / 50.0 * eqRangeDb;
                const double bassRatio = std::pow (10.0, bassDb / 20.0), trebleRatio = std::pow (10.0, trebleDb / 20.0);
                bassGain.set (bassRatio);
                trebleGain.set (trebleRatio);

                // out = in + (gain - 1) * low-passed (or high-passed) in, the corner placed so that boost and cut are mirror images
                bassCoef.set (onePoleCoef (eqBassHz / std::sqrt (bassRatio)));
                trebleCoef.set (onePoleCoef (eqTrebleHz * std::sqrt (trebleRatio)));
                wowDepth = variant == tapeEcho || variant == tapeEchoDry ? 0.18 : 0.0;
                break;
            }

            case digMod:
            case analogMod:
            {
                // Depth is the pitch deviation (up to +-1.2 %); the sweep is capped at 5 ms so slow speeds stay a chorus
                const double rate = std::clamp (a, 0.05, 10.0);
                lfoInc = twoPi * rate / fs;
                modAmp.set (std::min (std::min (0.005, 0.25 * seconds), toUnit (k[4]) * 0.012 / (twoPi * rate)) * fs);
                break;
            }

            case reverse:
            {
                const double rate = std::clamp (a, 0.05, 10.0);
                lfoInc = twoPi * rate / fs;
                modAmp.set (std::min (0.003, toUnit (k[4]) * 0.010 / (twoPi * rate)) * fs);
                reverseTarget = (int) tap[0].target;
                first = spacing = 2.0 * seconds; // a chunk is heard up to twice its length after it went in
                break;
            }

            case loRes:
                toneTarget = toUnit (k[3]);
                quantScale = std::ldexp (1.0, 5 + (int) std::clamp (std::floor (b + 0.5), 0.0, 18.0)); // 2^(bits - 1)
                break;

            case tubeEcho:
            case tubeEchoDry:
            case echoPlatter:
            case echoPlatterDry:
                wowDepth = toUnit (k[3]);
                driveTarget = toUnit (k[4]);
                break;

            case sweepEcho:
            case sweepEchoDry:
            {
                const double depth = toUnit (k[4]);
                sweepInc = twoPi * std::clamp (a, 0.05, 10.0) / fs;
                sweepDepth.set (depth * sweepOctaves);
                sweepMix.set (std::min (1.0, depth * 5.0)); // no sweep, no filter: plain EP-1 echoes
                wowDepth = 0.15;
                driveTarget = 0.4; // the dry path's preamp (processDry)
                break;
            }

            case autoVolume:
                // The swell has to replace the dry signal, so this model does its own mixing: it returns the
                // swelled signal and the echoes already balanced by Mix, and getMix() tells the slot "all wet".
                wowDepth = toUnit (k[3]);
                swellRise = (double) (float) (1.0 / (0.02 * std::pow (100.0, toUnit (k[4])) * fs)); // 20 ms .. 2 s
                directGain.set (std::min (1.0, 2.0 - 2.0 * mixValue));
                echoGain.set (std::min (1.0, 2.0 * mixValue));
                mix = 1.0f;
                break;

            case multiHead:
            {
                const int heads = (int) std::clamp (std::floor (a + 0.5), 0.0, 3.0) | ((int) std::clamp (std::floor (b + 0.5), 0.0, 3.0) << 2);
                int count = 0, last = 3;
                for (int h = 0; h < 4; ++h)
                    if ((heads >> h) & 1) { ++count; last = h; }

                // The mix of the selected heads (each at 1 / sqrt (heads)) is what gets recorded again, as on the real
                // unit: more heads, denser repeats. It is scaled so that the loop gain is Fdbk when all heads add up.
                const double gain = count > 0 ? 1.0 / std::sqrt ((double) count) : 0.0;
                for (int h = 0; h < 4; ++h)
                    headGain[h].set (((heads >> h) & 1) ? gain : 0.0);

                loopScale = count > 0 ? gain : 1.0;
                wowDepth = 0.2;
                first = spacing = seconds * headRatio[last];
                break;
            }

            default:
                break;
        }

        feedback[0].set (loopGain * loopScale);
        feedback[1].set (fbRight);
        setWow (wowDepth, seconds);
        tail = (float) std::clamp (first + spacing * repeats (tailFeedback) + 0.1, first + 0.1, 20.0);
    }

    /** Wet signal only, in place. */
    void process (float* left, float* right, int numSamples) noexcept
    {
        updateBlock (numSamples, false);

        switch (variant)
        {
            case pingPong:      processPingPong (left, right, numSamples); break;
            case dynamicDly:
            case stereoDelay:
            case digitalDelay:
            case digMod:        processDigital (left, right, numSamples); break;
            case reverse:       processReverse (left, right, numSamples); break;
            case loRes:         processLoRes (left, right, numSamples); break;
            case multiHead:     processMultiHead (left, right, numSamples); break;
            default:            processAnalog (left, right, numSamples); break;
        }
    }

    /** The slot's dry signal, in place: Tube Echo, Tape Echo, Sweep Echo and Echo Platter pass it through the
        original unit's preamp; every other model (and the "Dry" variants) leave it untouched. */
    void processDry (float* left, float* right, int numSamples) noexcept
    {
        if (dryKind == 0)
            return;

        if (approach (dryDrive, driveTarget, 8.0 * (double) numSamples / fs))
            updateDrySaturator();

        float* io[2] = { left, right };
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < numSamples; ++i)
            {
                float x = dryEq[c].process (io[c][i]);
                x = dryHighPass[c].highPass (drySaturator.process (x));
                io[c][i] = dryLowPass[c].lowPass (x) * dryGain;
            }
    }

    /** 0..1 from the Mix knob. (Auto-Volume Echo reports 1 and balances swell and echoes itself.) */
    float getMix() const noexcept { return mix; }

    /** About how long the repeats need to fall by 60 dB at the current Time and Fdbk. */
    float getTailSeconds() const noexcept { return tail; }

private:
    using Ramp = delayfx_detail::Ramp;

    static double toSeconds (float ms) noexcept { return std::clamp ((double) ms, 20.0, 2000.0) * 0.001; }
    static double toUnit (float percent) noexcept { return std::clamp ((double) percent * 0.01, 0.0, 1.0); }
    float stepFor (double seconds) const noexcept { return (float) (1.0 - std::exp (-1.0 / (seconds * fs))); }

    /** Coefficient of a one-pole (TPT) low-pass at `hz`. */
    double onePoleCoef (double hz) const noexcept
    {
        const double g = std::tan (pi * std::clamp (hz, 1.0, 0.49 * fs) / fs);
        return g / (1.0 + g);
    }

    /** A bass and a treble shelf: out = in + amount * (low- / high-passed in). Amounts and corner coefficients
        are ramped per sample, so knob moves cannot click. */
    static float shelfPair (float x, float& lowState, float& highState, float bass, float treble, float lowCoef, float highCoef) noexcept
    {
        float v = (x - lowState) * lowCoef, low = v + lowState;
        lowState = low + v;
        x += bass * low;

        v = (x - highState) * highCoef;
        low = v + highState;
        highState = low + v;
        return x + treble * (x - low);
    }

    /** How many repeats after the first one until they are 60 dB down. */
    static double repeats (double fb) noexcept
    {
        if (fb >= 0.999) return 1.0e6;
        return std::log (0.001) / std::log (std::max (fb, 1.0e-6));
    }

    /** Moves `value` towards `target` by at most `maxStep`; true if it changed. */
    static bool approach (double& value, double target, double maxStep) noexcept
    {
        if (value == target)
            return false;

        value = target > value ? std::min (target, value + maxStep) : std::max (target, value - maxStep);
        return true;
    }

    const delayfx_detail::Voicing& voicing() const noexcept
    {
        using namespace delayfx_detail;
        switch (variant)
        {
            case tapeEcho: case tapeEchoDry:        return voicingEp3;
            case sweepEcho: case sweepEchoDry:      return voicingSweep;
            case echoPlatter: case echoPlatterDry:  return voicingPlatter;
            case analogMod:                         return voicingDmm;
            case analogEcho:                        return voicingDm2;
            case autoVolume:                        return voicingSwell;
            case multiHead:                         return voicingRe101;
            default:                                return voicingEp1;
        }
    }

    /** The fixed filters of the selected model. */
    void updateVoicing()
    {
        const auto& v = voicing();
        const bool dryThrough = variant == tubeEcho || variant == tapeEcho || variant == sweepEcho || variant == echoPlatter;
        dryKind = dryThrough ? v.dry : 0;

        for (int c = 0; c < 2; ++c)
        {
            lowPass[c].setLowPass (fs, v.lowPassHz, v.lowPassQ);
            highPass[c].setCutoff (fs, v.highPassHz);
            bump[c].set (fs, v.bumpHz > 0.0 ? v.bumpHz : 100.0, v.bumpQ, v.bumpDb);
            preFilter[c].setCutoff (fs, v.preHz > 0.0 ? v.preHz : 5000.0);

            // the dry signal's way through the unit
            if (dryKind == 2)       dryEq[c].setHighShelf (fs, 2200.0, 2.5);   // EP-3: the FET preamp's treble lift
            else if (dryKind == 3)  dryEq[c].setPeak (fs, 1200.0, 0.7, 2.0);   // Echorec: mid-forward valve mixer
            else                    dryEq[c].setPeak (fs, 1000.0, 0.7, 0.0);   // EP-1: flat before the tube
            dryHighPass[c].setCutoff (fs, dryKind == 3 ? 90.0 : (dryKind == 2 ? 45.0 : 30.0));
            dryLowPass[c].setCutoff (fs, dryKind == 3 ? 6500.0 : (dryKind == 2 ? 16000.0 : 9000.0));
        }

        dryGain = dryKind == 2 ? 0.93f : 1.0f;
        bbdTime = -1.0;
    }

    void updateDrySaturator() noexcept
    {
        if (dryKind == 2)       drySaturator.set (0.45, 0.05);
        else if (dryKind == 3)  drySaturator.set (0.5 + 1.0 * dryDrive, 0.10);
        else                    drySaturator.set (0.5 + 1.2 * dryDrive, 0.12);
    }

    /** Transport speed errors -> delay modulation. A speed error a sin (w t) moves an echo of length T by
        (2 a / w) sin (w T / 2): slow wow grows with the delay time, fast flutter does not. */
    void setWow (double depth, double seconds) noexcept
    {
        const auto& v = voicing();
        for (int j = 0; j < 3; ++j)
        {
            const double w = delayfx_detail::twoPi * v.wowHz[j];
            wowAmp[j].set (depth * (2.0 * v.wowDeviation[j] / w) * std::abs (std::sin (0.5 * w * seconds)) * fs);
            wowInc[j] = w / fs;
        }
        driftAmp.set (depth * v.drift * 8.0 * std::min (seconds, 0.5) * fs);
    }

    /** Once per block: knob values that are slewed at block rate and the filters that follow them.
        `force` (from reset) jumps to the targets and recalculates everything. */
    void updateBlock (int numSamples, bool force) noexcept
    {
        using namespace delayfx_detail;
        const double n = (double) numSamples / fs;
        const auto& v = voicing();

        // record-path saturation (Drive)
        if (approach (drive, driveTarget, force ? 1.0e9 : 8.0 * n) || force)
        {
            const bool hasDriveKnob = v.driveMax > v.driveMin;
            const double g = v.driveMin + (v.driveMax - v.driveMin) * (hasDriveKnob ? drive : 0.0);
            saturator.set (g, v.bias);
            outGain.set (hasDriveKnob ? std::pow (g, 0.25) : 1.0); // a hot tape returns a little louder
        }

        if (force)
        {
            dryDrive = driveTarget;
            updateDrySaturator();
        }

        // Lo Res Delay's Tone: 500 Hz .. 20 kHz, wide open at 100 %
        if (approach (tone, toneTarget, force ? 1.0e9 : 6.0 * n) || force)
        {
            const double g = std::tan (pi * std::min (500.0 * std::pow (40.0, tone), 0.49 * fs) / fs);
            toneCoef = (float) (g / (1.0 + g));
            toneOpen = tone >= 0.995;
        }

        // BBD: a longer delay means a slower clock and a lower reconstruction filter
        if (variant == analogMod || variant == analogEcho)
        {
            if (force)
                glide = timeTarget;

            if (bbdTime < 0.0 || std::abs (glide - bbdTime) > 0.01 * bbdTime)
            {
                bbdTime = glide;
                const double hz = v.lowPassHz * std::min (1.0, std::pow (0.3 * fs / glide, 0.4));
                lowPass[0].setLowPass (fs, hz, v.lowPassQ);
                lowPass[1].setLowPass (fs, hz, v.lowPassQ);
            }
        }
    }

    //==============================================================================
    /** Whole-sample read with the crossfade to a new time. */
    float readTap (const delayfx_detail::Line& l, delayfx_detail::Tap& t) noexcept
    {
        if (t.count == 0)
        {
            if (t.target == t.current)
                return l.readInt ((int) t.current);

            t.next = t.target;
            t.count = xfadeLength;
        }

        const float from = l.readInt ((int) t.current), to = l.readInt ((int) t.next);
        const float g = (float) (xfadeLength - t.count) * xfadeStep;
        if (--t.count == 0)
            t.current = t.next;
        return from + (to - from) * g;
    }

    /** The same with a modulated (fractional) read position. */
    float readTap (const delayfx_detail::Line& l, delayfx_detail::Tap& t, double offset) noexcept
    {
        if (t.count == 0)
        {
            if (t.target == t.current)
                return l.read (t.current + offset);

            t.next = t.target;
            t.count = xfadeLength;
        }

        const float from = l.read (t.current + offset), to = l.read (t.next + offset);
        const float g = (float) (xfadeLength - t.count) * xfadeStep;
        if (--t.count == 0)
            t.current = t.next;
        return from + (to - from) * g;
    }

    void advanceLfo() noexcept
    {
        lfoPhase += lfoInc;
        if (lfoPhase >= delayfx_detail::twoPi)
            lfoPhase -= delayfx_detail::twoPi;
    }

    /** Dynamic Dly: gain for the echoes, from the level of what is being played right now. */
    double nextDuckGain (float l, float r) noexcept
    {
        const double level = std::max (std::abs ((double) l), std::abs ((double) r));
        duckEnv += (level - duckEnv) * (double) (level > duckEnv ? duckAttack : duckRelease);

        const double amount = std::clamp ((duckEnv - 0.5 * duckThreshold) / duckThreshold, 0.0, 1.0);
        const double target = 1.0 - amount * duckDepth;
        duckGain += (target - duckGain) * (double) (target < duckGain ? duckClose : duckOpen); // down fast, bloom slowly
        return duckGain;
    }

    /** Auto-Volume Echo: gain that closes at every new note and fades it in. Kept in double and free of
        transcendental functions, so the JavaScript port triggers on exactly the same samples. */
    double nextSwellGain (float l, float r) noexcept
    {
        using namespace delayfx_detail;
        const double level = 0.5 * (std::abs ((double) l) + std::abs ((double) r));
        swellFast = level > swellFast ? level : swellFast * (double) swellFastRelease;
        swellSlow += (swellFast - swellSlow) * (double) (swellFast > swellSlow ? swellSlowAttack : swellSlowRelease);
        if (swellHold > 0)
            --swellHold;

        if (swellFast < swellOff)
        {
            swellOpen = false;
        }
        else if (swellFast > swellOn && swellHold == 0 && (! swellOpen || swellFast > swellOnsetRatio * swellSlow))
        {
            swellOpen = true;
            swellDipping = true; // a new note: down in 4 ms, then up over the Swell time
            swellHold = swellHoldSamples;
        }

        if (swellDipping)
        {
            swellGain -= swellDipStep;
            if (swellGain <= 0.0)
            {
                swellGain = swellPos = 0.0;
                swellDipping = false;
            }
        }
        else
        {
            swellPos = swellOpen ? std::min (1.0, swellPos + swellRise) : std::max (0.0, swellPos - swellCloseStep);
            swellGain = swellPos * swellPos;
        }
        return swellGain;
    }

    /** Wow, flutter and a slow random drift of the transport, as a delay offset in samples. */
    double nextWow() noexcept
    {
        using namespace delayfx_detail;
        double offset = 0.0;
        for (int j = 0; j < 3; ++j)
        {
            offset += wowAmp[j].next() * std::sin (wowPhase[j]);
            wowPhase[j] += wowInc[j];
            if (wowPhase[j] >= twoPi)
                wowPhase[j] -= twoPi;
        }

        if (--noiseCount <= 0)
        {
            noiseCount = noiseInterval;
            noiseState = noiseState * 1664525u + 1013904223u;
            noiseTarget = (double) (noiseState >> 8) * (2.0 / 16777216.0) - 1.0;
        }
        noise1 += (double) noiseCoef * (noiseTarget - noise1);
        noise2 += (double) noiseCoef * (noise1 - noise2);
        return offset + driftAmp.next() * noise2;
    }

    bool wowIsOn() const noexcept
    {
        return ! (wowAmp[0].isZero() && wowAmp[1].isZero() && wowAmp[2].isZero() && driftAmp.isZero());
    }

    /** Tape-style time change: the delay glides to its target, which bends the pitch of what is in the line
        (at most an octave up on the way to a shorter time, about an octave and a half down to a longer one). */
    double nextGlide() noexcept
    {
        glide += std::clamp ((timeTarget - glide) * (double) glideCoef, -1.0, 0.66);
        return glide;
    }

    //==============================================================================
    // Dynamic Dly, Stereo Delay, Digital Delay, Dig Dly W/Mod: one clean line per side
    void processDigital (float* left, float* right, int numSamples) noexcept
    {
        using namespace delayfx_detail;
        float* io[2] = { left, right };
        const bool useEq = variant == digitalDelay, useMod = variant == digMod, ducking = variant == dynamicDly;

        for (int i = 0; i < numSamples; ++i)
        {
            const float outputGain = ducking ? (float) nextDuckGain (left[i], right[i]) : 1.0f;
            float bass = 1.0f, treble = 1.0f, lowCoef = 0.0f, highCoef = 0.0f;
            double offset[2] = { 0.0, 0.0 };

            if (useEq)
            {
                bass = (float) bassGain.next();      lowCoef = (float) bassCoef.next();
                treble = (float) trebleGain.next();  highCoef = (float) trebleCoef.next();
            }

            if (useMod)
            {
                const double amp = modAmp.next();
                offset[0] = amp * std::sin (lfoPhase); // the right side a quarter cycle ahead: the chorus spreads out
                offset[1] = amp * std::cos (lfoPhase);
                advanceLfo();
            }

            for (int c = 0; c < 2; ++c)
            {
                float wet = useMod ? readTap (line[c], tap[c], offset[c]) : readTap (line[c], tap[c]);
                if (useEq) // cuts, inside the loop
                    wet = shelfPair (wet, shelfLow[0][c], shelfHigh[0][c], std::min (bass, 1.0f) - 1.0f, std::min (treble, 1.0f) - 1.0f, lowCoef, highCoef);

                line[c].push (softLimit (io[c][i] + (float) feedback[c].next() * wet));

                if (useEq) // boosts, on the way out
                    wet = shelfPair (wet, shelfLow[1][c], shelfHigh[1][c], std::max (bass, 1.0f) - 1.0f, std::max (treble, 1.0f) - 1.0f, lowCoef, highCoef);
                io[c][i] = wet * outputGain;
            }
        }
    }

    // Ping Pong: the mono sum goes into the left line, its output into the right line (first "ping", then
    // "pong" at the same level), and the right line's output back into the left one, turned down by Fdbk.
    void processPingPong (float* left, float* right, int numSamples) noexcept
    {
        using namespace delayfx_detail;
        for (int i = 0; i < numSamples; ++i)
        {
            const float in = 0.5f * (left[i] + right[i]);
            const float ping = readTap (line[0], tap[0]), pong = readTap (line[1], tap[1]);
            const float fb = (float) feedback[0].next();
            const float direct = (float) spreadDirect.next(), cross = (float) spreadCross.next();

            line[0].push (softLimit (in + fb * pong));
            line[1].push (ping);
            left[i]  = direct * ping + cross * pong;
            right[i] = direct * pong + cross * ping;
        }
    }

    // Lo Res Delay: what goes into the line is rounded to the chosen word length (no dither, like an early
    // digital delay), again on every trip round the loop. Everything the rounding depends on is computed in
    // double from exact values, so the JavaScript port rounds the same way.
    void processLoRes (float* left, float* right, int numSamples) noexcept
    {
        using namespace delayfx_detail;
        float* io[2] = { left, right };
        const double coef = (double) toneCoef, xf = (double) xfadeLength;

        for (int i = 0; i < numSamples; ++i)
            for (int c = 0; c < 2; ++c)
            {
                auto& t = tap[c];
                double wet;

                if (t.count == 0 && t.target == t.current)
                {
                    wet = (double) line[c].readInt ((int) t.current);
                }
                else
                {
                    if (t.count == 0)
                    {
                        t.next = t.target;
                        t.count = xfadeLength;
                    }

                    const double from = (double) line[c].readInt ((int) t.current), to = (double) line[c].readInt ((int) t.next);
                    wet = from + (to - from) * ((double) (xfadeLength - t.count) / xf);
                    if (--t.count == 0)
                        t.current = t.next;
                }

                // Tone: one-pole low-pass on the repeats (inside the loop: every repeat is a little darker)
                const double v = (wet - toneState[c]) * coef, low = v + toneState[c];
                toneState[c] = low + v;
                if (! toneOpen)
                    wet = low;

                const double in = softLimit ((double) io[c][i] + feedback[c].next() * wet);
                line[c].push ((float) (std::floor (in * quantScale + 0.5) / quantScale));
                io[c][i] = (float) wet;
            }
    }

    // Reverse: the read position runs backwards from "now" for one chunk (Time), then jumps back to "now".
    // Every piece of the input is heard once, reversed; at each jump the old read head keeps running
    // underneath the new one for an equal-power crossfade.
    void processReverse (float* left, float* right, int numSamples) noexcept
    {
        using namespace delayfx_detail;
        for (int i = 0; i < numSamples; ++i)
        {
            const float in = 0.5f * (left[i] + right[i]);
            const double offset = modAmp.next() * (1.0 + std::sin (lfoPhase)); // chorus: the read head wanders a little
            advanceLfo();

            float wet = whole.read (2.0 + 2.0 * (double) reversePos + offset);
            if (reversePos < reverseFade)
            {
                const double x = 0.5 * pi * ((double) reversePos + 0.5) / (double) reverseFade;
                const float old = whole.read (2.0 + 2.0 * (double) (reversePrevious + reversePos) + offset);
                wet = (float) std::cos (x) * old + (float) std::sin (x) * wet;
            }

            whole.push (softLimit (in + (float) feedback[0].next() * wet));
            left[i] = right[i] = wet;

            if (++reversePos >= reverseLength)
            {
                reversePos = 0;
                reversePrevious = reverseLength;
                reverseLength = reverseTarget; // a new Time is taken up here
                reverseFade = std::min (reverseLength / 4, reverseFadeMax);
            }
        }
    }

    // Multi-Head: one tape, one record head, four playback heads. The selected heads are mixed, and that mix
    // is also what is fed back to the record head.
    void processMultiHead (float* left, float* right, int numSamples) noexcept
    {
        using namespace delayfx_detail;
        const bool wow = wowIsOn();

        for (int i = 0; i < numSamples; ++i)
        {
            const float in = 0.5f * (left[i] + right[i]);
            const double d = nextGlide() + (wow ? nextWow() : 0.0);
            float wet = 0.0f;

            for (int h = 0; h < 4; ++h)
            {
                const double g = headGain[h].next();
                if (g != 0.0)
                    wet += (float) g * line[0].read (d * headRatio[h]);
            }

            wet = headMixKnee * softLimit (wet * (1.0f / headMixKnee));
            wet = bump[0].process (highPass[0].highPass (lowPass[0].process (wet)));
            line[0].push (saturator.process (in + (float) feedback[0].next() * wet));
            left[i] = right[i] = wet;
        }
    }

    // Tube / Tape / Sweep Echo, Echo Platter, Analog W/Mod, Analog Echo, Auto-Volume Echo: per side
    //   read (gliding, wobbling) -> playback filters -> out
    //   in + Fdbk * out -> saturation -> line
    // so every repeat is filtered and saturated once more than the one before.
    void processAnalog (float* left, float* right, int numSamples) noexcept
    {
        using namespace delayfx_detail;
        float* io[2] = { left, right };
        const auto& v = voicing();
        const bool wow = wowIsOn(), sineMod = variant == analogMod, useBump = v.bumpDb != 0.0, usePre = v.preHz > 0.0;
        const bool useEq = variant == tapeEcho || variant == tapeEchoDry || variant == analogEcho;
        const bool sweeping = variant == sweepEcho || variant == sweepEchoDry, swelling = variant == autoVolume;

        for (int i = 0; i < numSamples; ++i)
        {
            double d = nextGlide();
            if (wow)
                d += nextWow();

            if (sineMod)
            {
                d += modAmp.next() * std::sin (lfoPhase);
                advanceLfo();
            }

            const float fb = (float) feedback[0].next(), level = (float) outGain.next();
            float bass = 1.0f, treble = 1.0f, lowCoef = 0.0f, highCoef = 0.0f;
            float swell = 1.0f, direct = 0.0f, echo = 1.0f;
            float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, sweepAmount = 0.0f;

            if (useEq)
            {
                bass = (float) bassGain.next();      lowCoef = (float) bassCoef.next();
                treble = (float) trebleGain.next();  highCoef = (float) trebleCoef.next();
            }

            if (swelling)
            {
                swell = (float) nextSwellGain (left[i], right[i]);
                direct = (float) directGain.next();
                echo = (float) echoGain.next();
            }

            if (sweeping)
            {
                // state-variable filter, the cutoff swept up and down by a sine
                const double octaves = sweepDepth.next() * std::sin (sweepPhase);
                sweepPhase += sweepInc;
                if (sweepPhase >= twoPi)
                    sweepPhase -= twoPi;

                const double g = std::tan (pi * std::min (sweepCentreHz * std::exp (ln2 * octaves), 0.45 * fs) / fs);
                const double k1 = 1.0 / (1.0 + g * (g + sweepDamping));
                a1 = (float) k1;
                a2 = (float) (g * k1);
                a3 = (float) (g * g * k1);
                sweepAmount = (float) sweepMix.next();
            }

            for (int c = 0; c < 2; ++c)
            {
                const float in = io[c][i] * swell;
                float wet = highPass[c].highPass (lowPass[c].process (line[c].read (d)));
                if (useBump)
                    wet = bump[c].process (wet);

                if (useEq) // cuts, inside the loop
                    wet = shelfPair (wet, shelfLow[0][c], shelfHigh[0][c], std::min (bass, 1.0f) - 1.0f, std::min (treble, 1.0f) - 1.0f, lowCoef, highCoef);

                float record = saturator.process (in + fb * wet);
                if (usePre)
                    record = preFilter[c].lowPass (record);
                line[c].push (record);

                if (useEq) // boosts, on the way out
                    wet = shelfPair (wet, shelfLow[1][c], shelfHigh[1][c], std::max (bass, 1.0f) - 1.0f, std::max (treble, 1.0f) - 1.0f, lowCoef, highCoef);

                if (sweeping)
                {
                    const float v3 = wet - sweep2[c];
                    const float v1 = a1 * sweep1[c] + a2 * v3;
                    const float v2 = sweep2[c] + a2 * sweep1[c] + a3 * v3;
                    sweep1[c] = 2.0f * v1 - sweep1[c];
                    sweep2[c] = 2.0f * v2 - sweep2[c];
                    wet += sweepAmount * (0.5f * v2 + (float) sweepDamping * v1 - wet); // some low-pass under the band-pass peak
                }

                io[c][i] = swelling ? in * direct + wet * echo : wet * level;
            }
        }
    }

    //==============================================================================
    double fs = 48000.0;
    int variant = digitalDelay;
    float mix = 0.35f, tail = 1.0f;
    static constexpr int numKnobsRead = 7;
    float knobs[numKnobsRead] = { 500.0f, 1.0f, 40.0f, 50.0f, 50.0f, 35.0f, 35.0f }; // the last setParameters, for prepare()

    std::vector<float> memory;
    delayfx_detail::Line line[2], whole;
    int lineSize = 0;

    // times and feedback
    double timeTarget = 9600.0, glide = 9600.0; // samples
    float glideCoef = 0.0f;
    delayfx_detail::Tap tap[2];
    int xfadeLength = 1;
    float xfadeStep = 1.0f;
    Ramp feedback[2];

    // modulation: chorus LFO, tape wow / flutter / drift
    double lfoPhase = 0.0, lfoInc = 0.0;
    Ramp modAmp;
    double wowPhase[3] {}, wowInc[3] {};
    Ramp wowAmp[3], driftAmp;
    uint32_t noiseState = 0;
    int noiseCount = 0, noiseInterval = 1;
    double noiseTarget = 0.0, noise1 = 0.0, noise2 = 0.0;
    float noiseCoef = 0.0f;

    // loop colour
    Biquad lowPass[2];
    delayfx_detail::Bell bump[2];
    OnePole highPass[2], preFilter[2];
    float shelfLow[2][2] {}, shelfHigh[2][2] {}; // [inside the loop, on the output][side]
    delayfx_detail::Saturator saturator;
    double drive = 0.0, driveTarget = 0.0, bbdTime = -1.0;
    Ramp bassGain, trebleGain, bassCoef, trebleCoef, outGain;

    // Ping Pong
    Ramp spreadDirect, spreadCross;

    // Dynamic Dly
    double duckThreshold = 0.03, duckDepth = 0.0, duckEnv = 0.0, duckGain = 1.0;
    float duckAttack = 0.0f, duckRelease = 0.0f, duckClose = 0.0f, duckOpen = 0.0f;

    // Lo Res Delay
    double tone = 1.0, toneTarget = 1.0, toneState[2] {}, quantScale = 8388608.0;
    float toneCoef = 1.0f;
    bool toneOpen = true;

    // Reverse
    int reversePos = 0, reverseLength = 9600, reversePrevious = 9600, reverseTarget = 9600, reverseFade = 1, reverseFadeMax = 1;

    // Sweep Echo
    double sweepPhase = 0.0, sweepInc = 0.0;
    Ramp sweepDepth, sweepMix;
    float sweep1[2] {}, sweep2[2] {};

    // Auto-Volume Echo
    double swellFast = 0.0, swellSlow = 0.0, swellPos = 0.0, swellGain = 0.0, swellRise = 0.001, swellDipStep = 0.01, swellCloseStep = 0.001;
    float swellFastRelease = 0.0f, swellSlowAttack = 0.0f, swellSlowRelease = 0.0f;
    int swellHold = 0, swellHoldSamples = 0;
    bool swellOpen = false, swellDipping = false;
    Ramp directGain, echoGain;

    // Multi-Head
    Ramp headGain[4];

    // the dry signal's preamp (processDry)
    int dryKind = 0;
    Biquad dryEq[2];
    OnePole dryHighPass[2], dryLowPass[2];
    delayfx_detail::Saturator drySaturator;
    double dryDrive = 0.0;
    float dryGain = 1.0f;
};

//==============================================================================
namespace delayfx_detail
{
    inline KnobSpec timeKnob (const char* name, float def) { return millis (name, 20.0f, 2000.0f, def, 500.0f); }
    inline KnobSpec noteKnob (const char* name, int def)   { return choice (name, delayNoteNames, numDelayNotes, def); }
    inline KnobSpec speedKnob (const char* name, float def) { return hertz (name, 0.05f, 10.0f, def, 1.0f); }

    /** Time, Note, Fdbk, the model's two controls, Mix. */
    inline ModelInfo delayModel (const char* key, const char* name, int variant, const char* basedOn,
                                 float timeMs, float fdbk, KnobSpec first, KnobSpec second, float mix)
    {
        ModelInfo m { key, name, Category::delay, Engine::delayFx, variant, basedOn,
                      { timeKnob ("Time", timeMs), noteKnob ("Note", 1), percent ("Fdbk", fdbk), first, second, percent ("Mix", mix) } };
        m.timeKnob = 0;
        m.noteKnob = 1;
        m.noteBeats = delayNoteBeats;
        m.trails = true;
        return m;
    }
}

/** In the order of DelayFx::Variant. */
inline std::vector<ModelInfo> delayFxModels()
{
    using namespace delayfx_detail;
    using V = DelayFx;

    // Stereo Delay: two times, each with its own tempo sync (dotted eighth against a quarter by default)
    ModelInfo stereo { "stereo_delay", "Stereo Delay", Category::delay, Engine::delayFx, V::stereoDelay, "Line 6 high-res digital delay",
                       { timeKnob ("L Time", 375.0f), noteKnob ("L Note", 2), percent ("L Fdbk", 35.0f),
                         timeKnob ("R Time", 500.0f), noteKnob ("R Note", 1), percent ("R Fdbk", 35.0f), percent ("Mix", 35.0f) } };
    stereo.timeKnob = 0;   stereo.noteKnob = 1;
    stereo.timeKnob2 = 3;  stereo.noteKnob2 = 4;
    stereo.noteBeats = delayNoteBeats;
    stereo.trails = true;

    return {
        // Line 6 original. Two lines feeding each other: the echo bounces left, right, left... Offset shortens
        // the right line (the bounce gets a swing), Spread narrows the two sides towards mono.
        delayModel ("ping_pong", "Ping Pong", V::pingPong, "Line 6 original",
                    400.0f, 45.0f, percent ("Offset", 100.0f), percent ("Spread", 100.0f), 35.0f),

        // TC Electronic 2290 "dynamic delay": a clean digital delay whose output is ducked by the playing level
        // and comes up (in about 0.3 s) when the playing stops or drops below the threshold.
        delayModel ("dynamic_dly", "Dynamic Dly", V::dynamicDly, "TC Electronic 2290",
                    450.0f, 40.0f, percent ("Thresh", 50.0f), percent ("Ducking", 60.0f), 40.0f),

        stereo,

        // Line 6 original: bit-transparent repeats; Bass / Treble are shelves inside the loop, so they add up
        // from repeat to repeat (flat at 50 %).
        delayModel ("digital_delay", "Digital Delay", V::digitalDelay, "Line 6 original",
                    500.0f, 40.0f, percent ("Bass", 50.0f), percent ("Treble", 50.0f), 35.0f),

        // Line 6 original: the read position is swept by a sine (in quadrature left / right), so only the
        // repeats are chorused, more with each trip round the loop.
        delayModel ("dig_dly_w_mod", "Dig Dly W/Mod", V::digMod, "Line 6 original",
                    450.0f, 40.0f, speedKnob ("ModSpd", 0.8f), percent ("Depth", 50.0f), 35.0f),

        // Line 6 original: every Time-long piece of the input comes back reversed, with crossfaded joins.
        delayModel ("reverse", "Reverse", V::reverse, "Line 6 original",
                    800.0f, 20.0f, speedKnob ("ModSpd", 0.5f), percent ("Depth", 20.0f), 50.0f),

        // Line 6 original after early digital delays: undithered word-length reduction inside the loop (the grit
        // grows and the tail gates off), Tone is the low-pass on the repeats.
        delayModel ("lo_res_delay", "Lo Res Delay", V::loRes, "Line 6 original",
                    400.0f, 40.0f, percent ("Tone", 60.0f), choice ("Res", delayFxBitNames, numDelayFxBitNames, 4), 35.0f),

        // Maestro Echoplex EP-1: tape loop with a sliding head and a tube preamp. Asymmetric tube saturation
        // into the tape (Drive), head bump and treble loss on each pass, wow and flutter. The dry signal runs
        // through the tube stage too ("Dry": it does not).
        delayModel ("tube_echo", "Tube Echo", V::tubeEcho, "'63 Maestro EP-1 Echoplex",
                    350.0f, 45.0f, percent ("Wow/Flt", 35.0f), percent ("Drive", 40.0f), 35.0f),
        delayModel ("tube_echo_dry", "Tube Echo Dry", V::tubeEchoDry, "'63 Maestro EP-1 Echoplex",
                    350.0f, 45.0f, percent ("Wow/Flt", 35.0f), percent ("Drive", 40.0f), 35.0f),

        // Maestro Echoplex EP-3: the solid-state one. Wider band and much less saturation than the EP-1, a
        // little fixed wow, Bass / Treble on the repeats; its FET preamp brightens the dry signal.
        delayModel ("tape_echo", "Tape Echo", V::tapeEcho, "Maestro EP-3 Echoplex",
                    380.0f, 40.0f, percent ("Bass", 50.0f), percent ("Treble", 50.0f), 35.0f),
        delayModel ("tape_echo_dry", "Tape Echo Dry", V::tapeEchoDry, "Maestro EP-3 Echoplex",
                    380.0f, 40.0f, percent ("Bass", 50.0f), percent ("Treble", 50.0f), 35.0f),

        // Line 6 original: the EP-1's tape echoes through a resonant filter that a sine sweeps up and down.
        delayModel ("sweep_echo", "Sweep Echo", V::sweepEcho, "Line 6 original (EP-1 with a sweeping filter)",
                    400.0f, 45.0f, speedKnob ("Swp Spd", 0.4f), percent ("Swp Dep", 60.0f), 40.0f),
        delayModel ("sweep_echo_dry", "Sweep Echo Dry", V::sweepEchoDry, "Line 6 original (EP-1 with a sweeping filter)",
                    400.0f, 45.0f, speedKnob ("Swp Spd", 0.4f), percent ("Swp Dep", 60.0f), 40.0f),

        // Binson Echorec: a magnetic drum instead of tape. Steadier than tape but with the drum's once-per-turn
        // wobble and idler flutter, thin lows and a present midrange that the repeats keep, tube electronics.
        delayModel ("echo_platter", "Echo Platter", V::echoPlatter, "Binson EchoRec",
                    300.0f, 50.0f, percent ("Wow/Flt", 30.0f), percent ("Drive", 35.0f), 40.0f),
        delayModel ("echo_platter_dry", "Echo Platter Dry", V::echoPlatterDry, "Binson EchoRec",
                    300.0f, 50.0f, percent ("Wow/Flt", 30.0f), percent ("Drive", 35.0f), 40.0f),

        // Electro-Harmonix Deluxe Memory Man: bucket-brigade line, band-limited before and after it (darker at
        // long times, as the clock slows down), soft asymmetric clipping, and the clock modulated for its
        // chorus / vibrato on the echoes.
        delayModel ("analog_w_mod", "Analog W/Mod", V::analogMod, "Electro-Harmonix Deluxe Memory Man",
                    400.0f, 40.0f, speedKnob ("ModSpd", 0.6f), percent ("Depth", 35.0f), 35.0f),

        // Boss DM-2: a shorter, darker and dirtier BBD than the Memory Man; each repeat loses more treble and
        // picks up more distortion. Bass / Treble on the repeats.
        delayModel ("analog_echo", "Analog Echo", V::analogEcho, "Boss DM-2",
                    300.0f, 35.0f, percent ("Bass", 50.0f), percent ("Treble", 50.0f), 35.0f),

        // Line 6 original: an envelope-triggered volume swell (every new note fades in over the Swell time) into
        // a tape-style echo with wow and flutter (ModDep).
        delayModel ("auto_volume_echo", "Auto-Volume Echo", V::autoVolume, "Line 6 original",
                    450.0f, 35.0f, percent ("ModDep", 30.0f), percent ("Swell", 50.0f), 40.0f),

        // Roland RE-101 Space Echo: one tape passing four playback heads at fixed distances; the head switches
        // pick the rhythm, and the mix of the selected heads is recorded again.
        delayModel ("multi_head", "Multi-Head", V::multiHead, "Roland RE-101 Space Echo",
                    480.0f, 35.0f, choice ("Heads 1-2", delayFxHeads12Names, 4, 2), choice ("Heads 3-4", delayFxHeads34Names, 4, 2), 35.0f),
    };
}

} // namespace fx
