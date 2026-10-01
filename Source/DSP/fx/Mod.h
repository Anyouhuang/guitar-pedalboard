#pragma once

#include "../DspUtils.h"
#include "../ModelTypes.h"

namespace fx
{
// choice knobs of the modulation models
inline const char* const modStepNames[]     = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16",
                                                "Mute", "Skip", "Full" };
inline const char* const modPanNames[]      = { "Left", "Center", "Right" };
inline const char* const modSweepNames[]    = { "Up", "Down", "Stereo" };
inline const char* const modStageNames[]    = { "4", "8", "12", "16" };
inline const char* const modSwitchNames[]   = { "Off", "On" };
inline const char* const modChorusNames[]   = { "Chorus", "Vibrato" };
inline const char* const modHarmonicNames[] = { "Even", "Odd" };
inline const char* const modRotorNames[]    = { "Slow", "Fast" };

namespace mod_detail
{
constexpr double twoPi = 2.0 * pi;
constexpr int stepMute = 16, stepSkip = 17;  // positions in modStepNames (0..15 = 1..16 pulses, 18 = Full)
constexpr int hilbertSections = 6;           // per path
constexpr int barberStages = 8;

/** Coefficient of a first-order all-pass (a + z^-1) / (1 + a z^-1) whose phase is -90 degrees at w = pi f / fs:
    (tan w - 1) / (tan w + 1), with sin and cos as polynomials (good to 1e-6 for w <= 1.35). */
inline double allpassCoef (double w) noexcept
{
    const double w2 = w * w;
    const double s = w * (1.0 + w2 * (-1.0 / 6.0 + w2 * (1.0 / 120.0 + w2 * (-1.0 / 5040.0 + w2 * (1.0 / 362880.0)))));
    const double c = 1.0 + w2 * (-1.0 / 2.0 + w2 * (1.0 / 24.0 + w2 * (-1.0 / 720.0 + w2 * (1.0 / 40320.0 + w2 * (-1.0 / 3628800.0)))));
    return (s - c) / (s + c);
}

/** A sine bent towards a square: g = 0 leaves it alone, large g gives a square with soft edges. */
inline double sineToSquare (double s, double g) noexcept { return s * (1.0 + g) / (1.0 + g * std::abs (s)); }

/** How hard a "square" LFO may be bent so that its edges still take about 10 ms (no clicks). */
inline double squareEdge (double rateHz) noexcept { return std::clamp (60.0 / rateHz, 1.0, 4000.0); }

/** Triangle (0) -> sine (0.5) -> square (1), -1..1, phase 0..1. */
inline double morphLfo (double p, double shape, double edge) noexcept
{
    const double s = std::sin (twoPi * p);
    if (shape < 0.5)
    {
        double q = p + 0.25;
        if (q >= 1.0) q -= 1.0;
        const double tri = 1.0 - 4.0 * std::abs (q - 0.5);
        return tri + 2.0 * shape * (s - tri);
    }
    const double amount = 2.0 * shape - 1.0;
    return sineToSquare (s, amount * amount * edge);
}

/** A chain of identical first-order all-pass stages (one state each). */
inline float allpassChain (float x, float a, float* z, int count) noexcept
{
    for (int s = 0; s < count; ++s)
    {
        const float y = a * x + z[s];
        z[s] = x - a * y;
        x = y;
    }
    return x;
}

/** What a bucket-brigade line (or a tape, or a tube) does to peaks: x - 4/27 x^3, flat beyond +-1.5. */
inline float softClip (float x) noexcept
{
    x = x > 1.5f ? 1.5f : (x < -1.5f ? -1.5f : x);
    return x - (4.0f / 27.0f) * x * x * x;
}

/** tanh, near enough (Pade), for the rotary speakers' tube amps. */
template <typename Type>
inline Type tubeClip (Type x) noexcept
{
    x = x > Type (3) ? Type (3) : (x < Type (-3) ? Type (-3) : x);
    const Type x2 = x * x;
    return x * (Type (27) + x2) / (Type (27) + Type (9) * x2);
}

/** Push-pull pair of output valves whose bias `b` the tremolo oscillator moves. Each half follows the smooth
    rectifier h(v) = (v + sqrt (v^2 + 1)) / 2, so the gain falls off gradually as the bias moves towards cut-off,
    and notes loud enough to swing past the bias shift are turned down less than quiet ones. The guitar's full
    level (0.4) is 0.8 of the curve's knee; scaled to unity gain at the resting bias. */
constexpr double biasRest = 1.0, biasSwing = 4.0;
inline float biasStage (float x, float b) noexcept
{
    const float p = b + 2.0f * x, q = b - 2.0f * x;
    return 0.1464466f * (p - q + std::sqrt (p * p + 1.0f) - std::sqrt (q * q + 1.0f));
}

/** Balance gain of one side: unity from the centre to its own end, then a smooth fade to nothing. */
inline float balance (double x) noexcept
{
    const double t = x < 0.5 ? 2.0 * x : 1.0;
    return (float) (t * (2.0 - t));
}

/** Power-of-two delay buffer, 4-point Hermite read (the same interpolation as fx::DelayLine). */
struct Line
{
    void prepare (int samples)
    {
        int size = 16;
        while (size < samples + 8)
            size <<= 1;
        buffer.assign ((size_t) size, 0.0f);
        mask = size - 1;
        write = 0;
    }

    void reset() noexcept { std::fill (buffer.begin(), buffer.end(), 0.0f); write = 0; }

    void push (float x) noexcept
    {
        buffer[(size_t) write] = x;
        write = (write + 1) & mask;
    }

    /** The sample pushed `delay` pushes ago (1 = the latest), delay >= 2. */
    float read (float delay) const noexcept
    {
        const int di = (int) delay;
        const float frac = delay - (float) di;
        const float* b = buffer.data();
        const int at = write - di;
        const float xm1 = b[(at + 1) & mask], x0 = b[at & mask], x1 = b[(at - 1) & mask], x2 = b[(at - 2) & mask];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * frac + c2) * frac + c1) * frac + x0;
    }

    std::vector<float> buffer;
    int mask = 15, write = 0;
};

/** One path of the 90-degree phase splitter: sections (c - z^-2) / (1 - c z^-2) in series.
    h holds the last two samples of the input and of every section's output. */
inline float hilbertPath (float x, const float* c, float* h) noexcept
{
    for (int k = 0; k < hilbertSections; ++k)
    {
        const float y = c[k] * (x + h[2 * k + 3]) - h[2 * k + 1];
        h[2 * k + 1] = h[2 * k];
        h[2 * k] = x;
        x = y;
    }
    h[2 * hilbertSections + 1] = h[2 * hilbertSections];
    h[2 * hilbertSections] = x;
    return x;
}

/** One microphone looking at a rotor. `facing` is the cosine of the angle between the rotor's mouth and the mic:
    the path gets longer as the mouth turns away (Doppler), and the sound quieter and duller (the part above the
    `split` filter is shadowed more than the part below). state[index] is the split filter. */
inline float rotorTap (const Line& line, double facing, double depth, double baseSamples, double swingSamples,
                       float split, float* state, int index, float shadowLow, float shadowHigh) noexcept
{
    const float s = line.read ((float) (baseSamples + swingSamples * depth * (1.0 - facing)));
    const float low = state[index] + split * (s - state[index]);
    state[index] = low;
    const float away = (float) (0.5 * depth * (1.0 - facing));
    return low * (1.0f - shadowLow * away) + (s - low) * (1.0f - shadowHigh * away);
}

/** The four bucket-brigade flangers. Delay = longest / clock, and the clock follows the control voltage
    either linearly (MXR: the notches move evenly in Hz) or exponentially (A/DA: evenly in octaves). */
struct FlangerVoicing
{
    double maxDelayMs, ratio;  // longest delay and the sweep range (longest : shortest)
    bool exponential;
    double lowPassHz;          // the line's reconstruction filter (two poles)
    float drive;               // level into the line's soft clipping
    double offset;             // LFO phase of the right side's line, in cycles
    bool compressor, mono;
};
inline constexpr FlangerVoicing flangerVoicings[] = {
    { 10.0,  20.0, false, 10000.0, 0.5f, 0.25,  false, false },  // Analog Flanger
    { 12.25, 35.0, true,   9000.0, 0.5f, 0.5,   true,  false },  // Jet Flanger
    { 10.0,  20.0, false,  6500.0, 1.0f, 0.125, false, false },  // AC Flanger
    { 12.25, 35.0, true,   6000.0, 1.0f, 0.0,   true,  true  },  // 80A Flanger
};
} // namespace mod_detail

//==============================================================================
/** The HD500X's 22 modulation models. Every model has its own process routine below, with a comment saying what
    the original is and what was modelled; the knobs are listed in modModels() at the end of this file.
    Stereo classes as in docs/hd500x-models.md:
      mono             the input is summed and the result written to both sides (80A Flanger)
      stereo through   each side keeps its own dry signal. Tremolos and the panner turn each side's own signal up
                       and down; the other effects are worked out from the mono sum and added to both sides,
                       alike (ST/M: U-Vibe, Pitch Vibrato) or differently (ST/S: two lines, two sweeps, two mics)
      true stereo      each side is processed on its own (Pattern Tremolo, Script Phase, Barberpole, Frequency Shifter)
    Mix is dry / effect at equal power (tremolos: linear), and 0 % always returns the input untouched.
    LFO phases and everything that steers a filter or a delay are doubles; audio state is float. */
class ModFx
{
public:
    enum Variant { patternTremolo = 0, panner, biasTremolo, optoTremolo, scriptPhase, pannedPhaser, barberpolePhaser,
                   dualPhaser, uVibe, phaser, pitchVibrato, dimension, analogChorus, triChorus, analogFlanger, jetFlanger,
                   acFlanger, flanger80A, frequencyShifter, ringModulator, rotaryDrum, rotaryDrumHorn, numVariants };

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        using namespace mod_detail;
        fs = sampleRate;
        invFs = 1.0 / fs;

        for (auto& line : lines)
            line.prepare ((int) (0.026 * fs));
        for (auto& s : sm)
            s.reset (fs, 0.04);

        envAttack   = (float) glide (0.005);
        envRelease  = (float) glide (0.25);
        compAttack  = (float) glide (0.005);
        compRelease = (float) glide (0.12);
        tremCoef    = (float) glide (0.0012);
        ldrFast     = glide (0.004);
        ldrSlow     = glide (0.045);
        cellUp      = glide (0.008);
        cellDown    = glide (0.02);
        lampUp      = glide (0.012);
        lampDown    = glide (0.035);
        fbLowPass   = (float) lowPass (6000.0);
        fbHighPass  = (float) lowPass (100.0);

        // Barberpole: eight fixed all-pass stages spread evenly (in octaves) from 100 Hz to 6.4 kHz
        for (int i = 0; i < barberStages; ++i)
        {
            const double t = std::tan (pi * 100.0 * std::pow (64.0, (double) i / 7.0) / fs);
            barberCoef[i] = (float) ((t - 1.0) / (t + 1.0));
        }

        designHilbert();
        reset();
    }

    void reset()
    {
        using namespace mod_detail;
        for (auto& s : sm)
            s.setCurrentAndTarget (s.getTarget());
        for (auto& line : lines)
            line.reset();

        phase = phase2 = seqPhase = 0.0;
        ldrA = ldrB = lamp = rise = 0.0;
        drumAngle = hornAngle = 0.0;
        drumSpeed = drumTarget;
        hornSpeed = hornTarget;
        env = compEnv = fbL = fbR = 0.0f;
        std::fill (std::begin (apL), std::end (apL), 0.0f);
        std::fill (std::begin (apR), std::end (apR), 0.0f);
        std::fill (std::begin (lp), std::end (lp), 0.0f);
        std::fill (std::begin (hilbert), std::end (hilbert), 0.0f);
        hilbertDelay[0] = hilbertDelay[1] = 0.0f;

        seqStep = steps[0] == stepSkip ? nextStep (0) : 0;
        trem1 = trem2 = stepGain (steps[seqStep], 0.0);
    }

    void setModel (int newVariant) noexcept { variant = std::clamp (newVariant, 0, (int) numVariants - 1); }

    void setParameters (const float* k)
    {
        using namespace mod_detail;
        auto pct  = [&] (int i) { return (double) k[i] / 100.0; };
        auto pick = [&] (int i, int count) { return std::clamp ((int) (k[i] + 0.5f), 0, count - 1); };
        auto set  = [&] (int index, double value) { sm[index].setTarget ((float) value); };
        volSens = 0.0;

        switch (variant)
        {
            case patternTremolo:
                rateHz = k[0];
                for (int i = 0; i < 4; ++i)
                    steps[i] = pick (1 + i, 19);
                break;

            case panner:
            case biasTremolo:
            case optoTremolo:
                rateHz = k[0];
                set (smDepth, pct (1));
                set (smShape, pct (2));
                volSens = 3.0 * pct (3);
                set (smDry, 1.0 - pct (4));
                set (smWet, pct (4));
                break;

            case scriptPhase:
                rateHz = k[0];
                break;

            case pannedPhaser:
            {
                rateHz = k[0];
                set (smDepth, pct (1));
                // the panner sweeps the left half, everything or the right half; with Pan Spd at 0 it parks there
                const int pan = pick (2, 3);
                rate2Hz = k[3];
                const double moving = std::min (1.0, rate2Hz / 0.05);
                set (smA, 0.5 * pan + moving * (0.25 - 0.25 * pan));
                set (smB, moving * (pan == 1 ? 0.5 : 0.25));
                setMix (pct (4), pannedFeedback, true);
                break;
            }

            case barberpolePhaser:
            {
                rateHz = k[0];
                const int mode = pick (2, 3);
                const double feedback = 0.8 * pct (1) * std::min (1.0, 2.0 * pct (3));
                set (smFeedback, feedback);
                set (smDirL, mode == 1 ? -1.0 : 1.0);
                set (smDirR, mode == 0 ? 1.0 : -1.0);
                setMix (pct (3), feedback, true);
                break;
            }

            case dualPhaser:
                rateHz = k[0];
                set (smDepth, pct (1));
                set (smFeedback, 0.8 * pct (2));
                set (smShape, pct (3));
                setMix (pct (4), 0.8 * pct (2), true);
                break;

            case uVibe:
                rateHz = k[0];
                set (smDepth, pct (1));
                set (smFeedback, 0.7 * pct (2));
                volSens = 3.0 * pct (3);
                setMix (pct (4), 0.7 * pct (2), true);
                break;

            case phaser:
            {
                rateHz = k[0];
                set (smDepth, pct (1));
                set (smFeedback, 0.8 * pct (2));
                const int stages = pick (3, 4);
                for (int t = 0; t < 4; ++t)
                    set (smTap0 + t, t == stages ? 1.0 : 0.0);
                setMix (pct (4), 0.8 * pct (2), true);
                break;
            }

            case pitchVibrato:
            {
                rateHz = k[0];
                set (smDepth, pct (1));
                const double slow = 1.0 - pct (2);
                riseStep = invFs / (0.02 + 3.0 * slow * slow);
                volSens = 3.0 * pct (3);
                setMix (pct (4), 0.0, false);
                wetLowPass = (float) lowPass (5500.0);
                break;
            }

            case dimension:
            {
                // the four mode buttons add up: more modulation with every button, and button 4 doubles the speed
                const int s1 = pick (0, 2), s2 = pick (1, 2), s3 = pick (2, 2), s4 = pick (3, 2);
                const double amount = s1 + 2.0 * s2 + 3.0 * s3 + 4.0 * s4;
                set (smDepth, 0.002 * (1.0 - std::exp (-amount / 3.5)));
                rateHz = s4 != 0 ? 0.5 : 0.25;
                setMix (amount > 0.0 ? pct (4) : 0.0, 0.0, false);
                wetLowPass = (float) lowPass (9000.0);
                toneCoef = (float) lowPass (200.0);
                break;
            }

            case analogChorus:
            {
                rateHz = k[0];
                set (smDepth, pct (1));
                const int vibrato = pick (2, 2);
                set (smA, vibrato);
                set (smB, 0.0032 * std::min (1.0, 1.2 / rateHz));  // chorus swing (s): held back at high speeds
                set (smTone, lowPass (1200.0 * std::pow (10.0, pct (3))));
                wetLowPass = (float) lowPass (8000.0);
                const double mix = pct (4);
                if (vibrato != 0)
                {
                    // the CE-1's vibrato has no dry signal: from 50 % up the Mix knob leaves it that way
                    const double angle = 0.5 * pi * std::min (1.0, 2.0 * mix);
                    set (smDry, std::cos (angle));
                    set (smWet, std::sin (angle));
                    set (smDryR, std::cos (angle));
                    set (smWetR, std::sin (angle));
                }
                else
                {
                    // chorus on the left output, the untouched signal on the right (the CE-1's two jacks)
                    set (smDry, std::cos (0.5 * pi * mix));
                    set (smWet, std::sin (0.5 * pi * mix));
                    set (smDryR, 1.0);
                    set (smWetR, 0.0);
                }
                break;
            }

            case triChorus:
                rateHz = k[0];
                set (smDepth, pct (1));
                set (smA, pct (2));
                set (smB, pct (3));
                set (smC, std::min (1.0, 1.5 / rateHz));  // the swing is held back at high speeds
                setMix (pct (4), 0.0, false);
                wetLowPass = (float) lowPass (9000.0);
                break;

            case analogFlanger:
            case jetFlanger:
            case acFlanger:
            case flanger80A:
            {
                const auto& voicing = flangerVoicings[variant - analogFlanger];
                rateHz = k[0];
                set (smDepth, pct (1));
                set (smA, pct (3));
                double feedback = (variant == analogFlanger ? 0.85 : variant == flanger80A ? 0.93 : 0.9) * pct (2);
                if (variant == flanger80A && pick (4, 2) == 1)
                    feedback = -feedback;  // Odd: the regeneration is inverted
                set (smFeedback, feedback);

                // dry and delayed signal at equal power, turned down by what the regeneration adds
                const double mix = variant == analogFlanger || variant == jetFlanger ? pct (4) : 0.5;
                const double c = std::cos (0.5 * pi * mix), s = std::sin (0.5 * pi * mix);
                const double norm = 1.0 / std::sqrt (c * c + s * s / (1.0 - feedback * feedback));
                set (smDry, c * norm);
                set (smWet, s * norm);
                set (smB, std::min (1.0, 2.0 * mix));
                wetLowPass = (float) lowPass (voicing.lowPassHz);
                break;
            }

            case frequencyShifter:
            {
                rateHz = k[0];
                const int mode = pick (1, 3);
                set (smDirL, mode == 1 ? -1.0 : 1.0);
                set (smDirR, mode == 0 ? 1.0 : -1.0);
                setMix (pct (2), 0.0, false);
                break;
            }

            case ringModulator:
                rateHz = k[0];
                set (smDepth, pct (1));
                set (smShape, pct (2));
                set (smA, pct (3));
                setMix (pct (4), 0.0, false);
                break;

            case rotaryDrum:
            case rotaryDrumHorn:
            {
                const bool fast = pick (0, 2) == 1;
                drumTarget = fast ? 5.7 : 0.67;
                hornTarget = fast ? 6.8 : 0.8;
                set (smDepth, pct (1));
                if (variant == rotaryDrum)
                    set (smTone, lowPass (1500.0 * std::pow (6.0, pct (2))));
                else
                    set (smA, pct (2));
                // the tube amp: more gain into the clipping, turned back down so a full-level note stays where it was
                const double gain = 1.0 + 7.0 * pct (3) * pct (3);
                set (smB, gain);
                set (smC, 0.4 / tubeClip (0.4 * gain));
                setMix (pct (4), 0.0, false);
                break;
            }

            default:
                break;
        }
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        switch (variant)
        {
            case patternTremolo:   processPattern (left, right, numSamples); break;
            case panner:           processPanner (left, right, numSamples); break;
            case biasTremolo:      processBias (left, right, numSamples); break;
            case optoTremolo:      processOpto (left, right, numSamples); break;
            case scriptPhase:      processScript (left, right, numSamples); break;
            case pannedPhaser:     processPanned (left, right, numSamples); break;
            case barberpolePhaser: processBarberpole (left, right, numSamples); break;
            case dualPhaser:       processDual (left, right, numSamples); break;
            case uVibe:            processVibe (left, right, numSamples); break;
            case phaser:           processPhaser (left, right, numSamples); break;
            case pitchVibrato:     processVibrato (left, right, numSamples); break;
            case dimension:        processDimension (left, right, numSamples); break;
            case analogChorus:     processChorus (left, right, numSamples); break;
            case triChorus:        processTriChorus (left, right, numSamples); break;
            case analogFlanger:
            case jetFlanger:
            case acFlanger:
            case flanger80A:       processFlanger (left, right, numSamples, mod_detail::flangerVoicings[variant - analogFlanger]); break;
            case frequencyShifter: processShifter (left, right, numSamples); break;
            case ringModulator:    processRing (left, right, numSamples); break;
            case rotaryDrum:       processDrum (left, right, numSamples); break;
            case rotaryDrumHorn:   processLeslie (left, right, numSamples); break;
            default: break;
        }
    }

private:
    enum { smDepth = 0, smFeedback, smShape, smA, smB, smC, smDry, smWet, smDryR, smWetR,
           smTap0, smTap1, smTap2, smTap3, smDirL, smDirR, smTone, numSmoothed };

    static constexpr double pannedFeedback = 0.3;

    double glide (double seconds) const   { return 1.0 - std::exp (-1.0 / (seconds * fs)); }

    /** Coefficient c of the one-pole low-pass y += c (x - y) that is 3 dB down at `hz` at any sample rate. */
    double lowPass (double hz) const
    {
        const double b = 2.0 - std::cos (mod_detail::twoPi * std::min (hz, 0.45 * fs) / fs);
        return 1.0 - (b - std::sqrt (b * b - 1.0));
    }

    /** Dry and effect at equal power (both 0.707 at 50 %). With feedback around a phaser the effect is made up so
        the notches stay complete, and everything is turned down by what the resonances add. */
    void setMix (double mix, double feedback, bool makeUp)
    {
        const double c = std::cos (0.5 * pi * mix), s = std::sin (0.5 * pi * mix);
        const double wetGain = makeUp ? 1.0 + feedback : 1.0;
        const double norm = 1.0 / std::sqrt (c * c + s * s * (1.0 + feedback) / (1.0 - feedback));
        sm[smDry].setTarget ((float) (c * norm));
        sm[smWet].setTarget ((float) (s * wetGain * norm));
    }

    /** The 90-degree phase splitter of the frequency shifter and the barberpole: two all-pass chains whose
        outputs are a quarter cycle apart from 30 Hz to fs/2 - 30 Hz. It is an elliptic half-band filter in
        polyphase form (coefficients after Valenzuela and Constantinides) turned by fs/4. */
    void designHilbert()
    {
        using namespace mod_detail;
        const int count = 2 * hilbertSections, order = 2 * count + 1;
        const double transition = 60.0 / fs;
        double k = std::tan ((1.0 - transition * 2.0) * pi / 4.0);
        k *= k;
        const double root = std::pow (1.0 - k * k, 0.25);
        const double e = 0.5 * (1.0 - root) / (1.0 + root), e4 = e * e * e * e;
        const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));

        for (int index = 0; index < count; ++index)
        {
            const int c = index + 1;
            double num = 0.0, den = 0.5, sign = 1.0;
            for (int i = 0; i < 14; ++i)
            {
                num += sign * std::pow (q, (double) (i * (i + 1))) * std::sin ((double) ((2 * i + 1) * c) * pi / (double) order);
                sign = -sign;
            }
            sign = -1.0;
            for (int i = 1; i < 14; ++i)
            {
                den += sign * std::pow (q, (double) (i * i)) * std::cos ((double) (2 * i * c) * pi / (double) order);
                sign = -sign;
            }
            num *= std::pow (q, 0.25);
            const double ww = (num / den) * (num / den);
            const double x = std::sqrt ((1.0 - ww * k) * (1.0 - ww / k)) / (1.0 + ww);
            const float coef = (float) ((1.0 - x) / (1.0 + x));
            if ((index & 1) != 0)
                hilbertQ[index / 2] = coef;
            else
                hilbertI[index / 2] = coef;
        }
    }

    /** VolSens: the louder the playing, the faster the LFO (up to four times at 100 %). Returns the phase step. */
    double sensedIncrement (float mono) noexcept
    {
        const float a = std::abs (mono);
        env += (a > env ? envAttack : envRelease) * (a - env);
        return rateHz * invFs * (1.0 + volSens * std::min (1.0, 2.5 * (double) env));
    }

    //==============================================================================
    // Pattern Tremolo (inspired by the Lightfoot Labs Goatkeeper): a four-step sequencer in front of a VCA.
    // Speed is the step rate; a step chops the sound into 1..16 even pulses, mutes it, passes it (Full) or is
    // left out of the pattern (Skip). The VCA's edges take about 4 ms. True stereo.
    int nextStep (int from) const noexcept
    {
        for (int i = 1; i <= 4; ++i)
            if (steps[(from + i) & 3] != mod_detail::stepSkip)
                return (from + i) & 3;
        return from;
    }

    static float stepGain (int step, double position) noexcept
    {
        if (step == mod_detail::stepMute)
            return 0.0f;
        if (step > mod_detail::stepMute)
            return 1.0f;  // Full (and a step switched to Skip while it plays)
        const double pulse = position * (double) (step + 1);
        return pulse - std::floor (pulse) < 0.5 ? 1.0f : 0.0f;
    }

    void processPattern (float* l, float* r, int n) noexcept
    {
        const double inc = rateHz * invFs;
        for (int i = 0; i < n; ++i)
        {
            seqPhase += inc;
            if (seqPhase >= 1.0)
            {
                seqPhase -= 1.0;
                seqStep = nextStep (seqStep);
            }
            const float target = stepGain (steps[seqStep], seqPhase);
            trem1 += tremCoef * (target - trem1);
            trem2 += tremCoef * (trem1 - trem2);
            l[i] *= trem2;
            r[i] *= trem2;
        }
    }

    //==============================================================================
    // Panner: constant-power auto-pan (both sides at unity in the centre). Shape morphs the LFO from triangle
    // through sine to square; VolSens speeds it up with the playing level. Stereo through, stereo effect.
    void processPanner (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double edge = squareEdge (rateHz);
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            phase += sensedIncrement (0.5f * (inL + inR));
            if (phase >= 1.0) phase -= 1.0;
            const double u = morphLfo (phase, sm[smShape].next(), edge);
            const double angle = 0.25 * pi * (1.0 + (double) sm[smDepth].next() * u);
            const float gl = (float) (1.4142135623730951 * std::cos (angle)), gr = (float) (1.4142135623730951 * std::sin (angle));
            const float dry = sm[smDry].next(), wet = sm[smWet].next();
            l[i] = inL * (dry + wet * gl);
            r[i] = inR * (dry + wet * gr);
        }
    }

    //==============================================================================
    // Bias Tremolo (1960 Vox AC-15): the oscillator shifts the bias of the push-pull output valves towards
    // cut-off, which turns the gain down smoothly - and turns quiet notes down further than loud ones, which push
    // through the bias shift (see biasStage). Level is the bias swing, Shape bends the sine towards a square.
    // The right side's oscillator runs a quarter cycle apart (Line 6's "3-D, wide stereo").
    void processBias (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double edge = squareEdge (rateHz);
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            phase += sensedIncrement (0.5f * (inL + inR));
            if (phase >= 1.0) phase -= 1.0;
            const double amount = sm[smShape].next(), g = amount * amount * edge;
            const double swing = biasSwing * (double) sm[smDepth].next();
            const double uL = sineToSquare (std::sin (twoPi * phase), g), uR = sineToSquare (std::cos (twoPi * phase), g);
            const float bL = (float) (biasRest - swing * (0.5 - 0.5 * uL)), bR = (float) (biasRest - swing * (0.5 - 0.5 * uR));
            const float dry = sm[smDry].next(), wet = sm[smWet].next();
            l[i] = dry * inL + wet * biasStage (inL, bL);
            r[i] = dry * inR + wet * biasStage (inR, bR);
        }
    }

    //==============================================================================
    // Opto Tremolo (blackface Fender): the oscillator flashes a neon lamp at a photocell that shunts the signal.
    // The lamp only lights near the top of the oscillator's swing; the cell's resistance drops within a few ms
    // and recovers over about 45 ms, so the volume falls quickly and comes back slowly - the lopsided blackface
    // chop, which also gets shallower at high speeds. Shape makes the lamp's flashes squarer. One cell for both
    // sides.
    void processOpto (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double edge = squareEdge (rateHz);
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            phase += sensedIncrement (0.5f * (inL + inR));
            if (phase >= 1.0) phase -= 1.0;
            const double amount = sm[smShape].next();
            const double drive = sineToSquare (std::sin (twoPi * phase), 2.0 + amount * amount * edge);
            const double light = drive > 0.25 ? (drive - 0.25) / 0.75 : 0.0;
            ldrA += (light > ldrA ? ldrFast : ldrSlow) * (light - ldrA);
            const double level = sm[smDepth].next();
            const float gain = (float) (1.0 / (1.0 + 24.0 * level * level * ldrA * ldrA));
            const float g = sm[smDry].next() + sm[smWet].next() * gain;
            l[i] = inL * g;
            r[i] = inR * g;
        }
    }

    //==============================================================================
    // Script Phase ('74 MXR Phase 90, script logo): four identical JFET all-pass stages mixed 1:1 with the dry
    // signal, which gives two notches (at 0.41 and 2.41 times the stages' corner frequency). The corner sweeps
    // 200 Hz .. 1.8 kHz on the pedal's rounded triangle. No regeneration: the script-logo circuit has no
    // feedback resistor (that came with the block logo; the "Phaser" model has the Fdbk knob). True stereo.
    void processScript (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double inc = rateHz * invFs, low = pi * 200.0 / fs;
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            const double tri = 1.0 - std::abs (2.0 * phase - 1.0);
            const double bent = 0.5 * (tri + tri * tri * (3.0 - 2.0 * tri));
            const float a = (float) allpassCoef (low * std::exp (2.1972245773362196 * bent));  // ln 9
            l[i] = 0.6f * (inL + allpassChain (inL, a, apL, 4));
            r[i] = 0.6f * (inR + allpassChain (inR, a, apR, 4));
        }
    }

    //==============================================================================
    // Panned Phaser (Ibanez Flying Pan): a four-stage phaser with a little fixed resonance, whose all-pass
    // signal is moved across the stereo field by a second, independent LFO: where it goes the phasing is deep,
    // on the other side only the dry signal is left. Pan picks the half of the field the panner works in.
    void processPanned (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double inc = rateHz * invFs, panInc = rate2Hz * invFs, centre = pi * 550.0 / fs;
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            phase2 += panInc;
            if (phase2 >= 1.0) phase2 -= 1.0;
            const float a = (float) allpassCoef (centre * std::exp (1.1090354888959124 * (double) sm[smDepth].next() * std::sin (twoPi * phase)));  // +-1.6 octaves
            fbL = allpassChain (0.5f * (inL + inR) + (float) pannedFeedback * fbL, a, apL, 4);
            const double pan = (double) sm[smA].next() + (double) sm[smB].next() * std::sin (twoPi * phase2);
            const float dry = sm[smDry].next(), wet = sm[smWet].next() * fbL;
            l[i] = dry * inL + wet * balance (1.0 - pan);
            r[i] = dry * inR + wet * balance (pan);
        }
    }

    //==============================================================================
    // Barberpole Phaser (modular-synth barberpole, after Bode): the signal goes through eight fixed all-pass
    // stages and is then shifted by Speed Hz with a single-sideband frequency shifter. Against the dry signal
    // every notch of the phaser then climbs (or falls) through the spectrum for ever, a new one appearing at
    // the bottom as one leaves at the top, once per 1 / Speed seconds. Feedback goes round shifter and stages,
    // so the resonances travel too. Stereo mode: up on the left, down on the right. True stereo.
    void processBarberpole (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double inc = rateHz * invFs;
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            const float c = (float) std::cos (twoPi * phase), s = (float) std::sin (twoPi * phase);
            const float fb = sm[smFeedback].next(), dry = sm[smDry].next(), wet = sm[smWet].next();
            const float dirL = sm[smDirL].next(), dirR = sm[smDirR].next();

            float x = inL + fb * fbL;
            for (int k = 0; k < barberStages; ++k)
            {
                const float y = barberCoef[k] * x + apL[k];
                apL[k] = x - barberCoef[k] * y;
                x = y;
            }
            float si = hilbertPath (x, hilbertI, hilbert);
            float sq = hilbertDelay[0];
            hilbertDelay[0] = hilbertPath (x, hilbertQ, hilbert + 14);
            const float wetL = si * c - dirL * sq * s;
            lp[0] += fbLowPass * (wetL - lp[0]);
            lp[1] += fbHighPass * (lp[0] - lp[1]);
            fbL = lp[0] - lp[1];

            x = inR + fb * fbR;
            for (int k = 0; k < barberStages; ++k)
            {
                const float y = barberCoef[k] * x + apR[k];
                apR[k] = x - barberCoef[k] * y;
                x = y;
            }
            si = hilbertPath (x, hilbertI, hilbert + 28);
            sq = hilbertDelay[1];
            hilbertDelay[1] = hilbertPath (x, hilbertQ, hilbert + 42);
            const float wetR = si * c - dirR * sq * s;
            lp[2] += fbLowPass * (wetR - lp[2]);
            lp[3] += fbHighPass * (lp[2] - lp[3]);
            fbR = lp[2] - lp[3];

            l[i] = dry * inL + wet * wetL;
            r[i] = dry * inR + wet * wetR;
        }
    }

    //==============================================================================
    // Dual Phaser (Mu-Tron Bi-Phase): two six-stage phasers (three notches each) on one sweep generator, the
    // second one reversed, so the two sweeps cross; phasor A is heard on the left, B on the right. The stages
    // are steered by photocells, which follow the generator with a little lag (quicker towards bright than
    // back): with LFO Shp at square the sweep jumps between two positions with rounded corners.
    void processDual (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double inc = rateHz * invFs, centre = pi * 420.0 / fs, edge = squareEdge (rateHz);
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i], m = 0.5f * (inL + inR);
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            const double amount = sm[smShape].next();
            const double u = sineToSquare (std::sin (twoPi * phase), amount * amount * edge);
            ldrA += (u > ldrA ? cellUp : cellDown) * (u - ldrA);
            ldrB += (-u > ldrB ? cellUp : cellDown) * (-u - ldrB);
            const double range = 1.1783502069519070 * (double) sm[smDepth].next();  // +-1.7 octaves
            const float aA = (float) allpassCoef (centre * std::exp (range * ldrA)), aB = (float) allpassCoef (centre * std::exp (range * ldrB));
            const float fb = sm[smFeedback].next(), dry = sm[smDry].next(), wet = sm[smWet].next();
            fbL = allpassChain (m + fb * fbL, aA, apL, 6);
            fbR = allpassChain (m + fb * fbR, aB, apR, 6);
            l[i] = dry * inL + wet * fbL;
            r[i] = dry * inR + wet * fbR;
        }
    }

    //==============================================================================
    // U-Vibe (Uni-Vibe): four all-pass stages steered by four photocells around one lamp. The stages have very
    // different capacitors (15 nF, 220 nF, 470 pF, 4.7 nF), so instead of a phaser's even notches there is one
    // broad dip in the upper mids and a throb in the bass. The lamp is what shapes the sweep: it follows the
    // sine oscillator with thermal lag (heating faster than cooling) and its light goes with the square of its
    // temperature, so the sweep lingers at the dark end. Mix at 100 % is the Uni-Vibe's vibrato setting.
    // One effect for both sides.
    void processVibe (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double base = pi * 600.0 / fs;
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i], m = 0.5f * (inL + inR);
            phase += sensedIncrement (m);
            if (phase >= 1.0) phase -= 1.0;
            const double glow = 0.5 + 0.5 * std::sin (twoPi * phase);
            lamp += (glow > lamp ? lampUp : lampDown) * (glow - lamp);
            const double w = base * std::exp (2.772588722239781 * (double) sm[smDepth].next() * (lamp * lamp - 0.45));  // 4 octaves
            const float fb = sm[smFeedback].next();
            float x = m + fb * fbL;
            for (int k = 0; k < 4; ++k)
            {
                static constexpr double ratio[] = { 0.3133, 0.02136, 10.0, 1.0 };  // 4.7 nF / the stage's capacitor
                const float a = (float) allpassCoef (std::min (1.35, w * ratio[k]));
                const float y = a * x + apL[k];
                apL[k] = x - a * y;
                x = y;
            }
            fbL = x;
            const float dry = sm[smDry].next(), wet = sm[smWet].next() * x;
            l[i] = dry * inL + wet;
            r[i] = dry * inR + wet;
        }
    }

    //==============================================================================
    // Phaser (Line 6's take on the Phase 90 with everything adjustable): 4, 8, 12 or 16 all-pass stages
    // (2 to 8 notches), sine sweep of up to +-1.6 octaves around 600 Hz, regeneration. The right side's chain
    // sweeps a quarter cycle ahead. The Stages switch crossfades between taps of the chain.
    void processPhaser (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double inc = rateHz * invFs, centre = pi * 600.0 / fs;
        int taps = 1;
        for (int t = 1; t < 4; ++t)
            if (sm[smTap0 + t].getTarget() > 0.0f || sm[smTap0 + t].isSmoothing())
                taps = t + 1;

        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i], m = 0.5f * (inL + inR);
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            const double range = 1.1090354888959124 * (double) sm[smDepth].next();
            const float aL = (float) allpassCoef (centre * std::exp (range * std::sin (twoPi * phase)));
            const float aR = (float) allpassCoef (centre * std::exp (range * std::cos (twoPi * phase)));
            const float fb = sm[smFeedback].next(), dry = sm[smDry].next(), wet = sm[smWet].next();
            float xl = m + fb * fbL, xr = m + fb * fbR, outL = 0.0f, outR = 0.0f;
            for (int t = 0; t < 4; ++t)
            {
                const float gain = sm[smTap0 + t].next();
                if (t < taps)
                {
                    xl = allpassChain (xl, aL, apL + 4 * t, 4);
                    xr = allpassChain (xr, aR, apR + 4 * t, 4);
                    outL += gain * xl;
                    outR += gain * xr;
                }
            }
            fbL = outL;
            fbR = outR;
            l[i] = dry * inL + wet * outL;
            r[i] = dry * inR + wet * outR;
        }
    }

    //==============================================================================
    // Pitch Vibrato (Boss VB-2): a bucket-brigade line of about 5 ms whose clock a sine LFO moves by up to
    // +-1.6 ms, so the pitch deviation grows with Depth and with Speed (2 pi * Speed * swing), and the line's
    // filters take the top off. Rise is the VB-2's rise time: after switching on, depth and speed come up
    // gradually (0 % = about 3 s, 100 % = at once). One effect for both sides.
    void processVibrato (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i], m = 0.5f * (inL + inR);
            rise = std::min (1.0, rise + riseStep);
            const double ramp = rise * rise * (3.0 - 2.0 * rise);
            phase += sensedIncrement (m) * (0.5 + 0.5 * ramp);
            if (phase >= 1.0) phase -= 1.0;
            const float w = lines[0].read ((float) (fs * (0.005 + 0.0016 * (double) sm[smDepth].next() * ramp * std::sin (twoPi * phase))));
            lines[0].push (softClip (m));
            lp[0] += wetLowPass * (w - lp[0]);
            lp[1] += wetLowPass * (lp[0] - lp[1]);
            const float dry = sm[smDry].next(), wet = sm[smWet].next() * lp[1];
            l[i] = dry * inL + wet;
            r[i] = dry * inR + wet;
        }
    }

    //==============================================================================
    // Dimension (Roland Dimension D): two bucket-brigade lines of 8 ms modulated in opposite directions by one
    // slow triangle (0.25 Hz; 0.5 Hz with button 4), so one side is always a few cents sharp while the other is
    // flat. Each side gets its own line plus the other line inverted, which widens the image without an
    // audible wobble. The lines only carry the signal above 200 Hz. No button pressed = dry.
    void processDimension (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double inc = rateHz * invFs;
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i], m = 0.5f * (inL + inR);
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            const double swing = (double) sm[smDepth].next() * (1.0 - 4.0 * std::abs (phase - 0.5));
            const float a = lines[0].read ((float) (fs * (0.008 + swing))), b = lines[0].read ((float) (fs * (0.008 - swing)));
            lp[4] += toneCoef * (m - lp[4]);
            lines[0].push (softClip (m - lp[4]));
            lp[0] += wetLowPass * (a - lp[0]);
            lp[1] += wetLowPass * (lp[0] - lp[1]);
            lp[2] += wetLowPass * (b - lp[2]);
            lp[3] += wetLowPass * (lp[2] - lp[3]);
            const float dry = sm[smDry].next(), wet = sm[smWet].next();
            l[i] = dry * inL + wet * (lp[1] - 0.4f * lp[3]);
            r[i] = dry * inR + wet * (lp[3] - 0.4f * lp[1]);
        }
    }

    //==============================================================================
    // Analog Chorus (Boss CE-1): one dark, band-limited bucket-brigade line of about 7 ms. Chorus: a triangle
    // LFO, the effect on the left output and the untouched signal on the right, as on the CE-1's two jacks.
    // Vibrato: a sine LFO with more swing and no dry signal, on both sides. Tone is the line's treble.
    void processChorus (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double inc = rateHz * invFs;
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            const double vibrato = sm[smA].next();
            const double tri = 1.0 - 4.0 * std::abs (phase - 0.5);
            const double lfo = tri + vibrato * (-std::cos (twoPi * phase) - tri);
            const double chorusSwing = sm[smB].next();
            const double swing = (double) sm[smDepth].next() * (chorusSwing + vibrato * (0.0024 - chorusSwing));
            const float w = lines[0].read ((float) (fs * (0.007 - 0.002 * vibrato + swing * lfo)));
            lines[0].push (softClip (0.5f * (inL + inR)));
            lp[0] += wetLowPass * (w - lp[0]);
            lp[1] += wetLowPass * (lp[0] - lp[1]);
            lp[2] += sm[smTone].next() * (lp[1] - lp[2]);
            l[i] = sm[smDry].next() * inL + sm[smWet].next() * lp[2];
            r[i] = sm[smDryR].next() * inR + sm[smWetR].next() * lp[2];
        }
    }

    //==============================================================================
    // Tri Chorus (Song Bird / DyTronics Tri-Stereo Chorus): three bucket-brigade lines (6, 8 and 10.5 ms) whose
    // LFOs are 120 degrees apart, each with a faster, smaller LFO on top (again 120 degrees apart) for the
    // shimmer. Line 1 goes left, line 3 right, line 2 to both; Depth, Depth2 and Depth3 are the three lines'
    // modulation depths.
    void processTriChorus (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double inc = rateHz * invFs, inc2 = 5.3 * inc;
        const double slowSwing = 0.0022 * fs, quickSwing = 0.00012 * fs;
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            phase2 += inc2;
            if (phase2 >= 1.0) phase2 -= 1.0;
            const double s1 = std::sin (twoPi * phase), c1 = std::cos (twoPi * phase);
            const double s2 = std::sin (twoPi * phase2), c2 = std::cos (twoPi * phase2);
            const double scale = sm[smC].next(), slow = slowSwing * scale, quick = quickSwing * scale;
            const double d1 = sm[smDepth].next(), d2 = sm[smA].next(), d3 = sm[smB].next();
            const float t1 = lines[0].read ((float) (0.006 * fs + d1 * (slow * s1 + quick * s2)));
            const float t2 = lines[0].read ((float) (0.008 * fs + d2 * (slow * (-0.5 * s1 + 0.8660254037844386 * c1) + quick * (-0.5 * s2 - 0.8660254037844386 * c2))));
            const float t3 = lines[0].read ((float) (0.0105 * fs + d3 * (slow * (-0.5 * s1 - 0.8660254037844386 * c1) + quick * (-0.5 * s2 + 0.8660254037844386 * c2))));
            lines[0].push (softClip (0.5f * (inL + inR)));
            const float wl = 0.75f * (t1 + 0.7f * t2), wr = 0.75f * (t3 + 0.7f * t2);
            lp[0] += wetLowPass * (wl - lp[0]);
            lp[1] += wetLowPass * (lp[0] - lp[1]);
            lp[2] += wetLowPass * (wr - lp[2]);
            lp[3] += wetLowPass * (lp[2] - lp[3]);
            const float dry = sm[smDry].next(), wet = sm[smWet].next();
            l[i] = dry * inL + wet * lp[1];
            r[i] = dry * inR + wet * lp[3];
        }
    }

    //==============================================================================
    // The four flangers (see FlangerVoicing). A triangle LFO and the Manual knob make the control voltage
    // (Depth / Width / Range at full = the whole range, Manual without effect, as on the pedals); the delay
    // follows it as a bucket-brigade clock would. Regeneration goes round the line, its filters and its soft
    // clipping.
    //   Analog Flanger  inspired by the MXR Flanger: 0.5 .. 10 ms, clock linear in the control voltage (the
    //                   sweep rushes through the low notes and lingers high up: the "uniquely shaped" sweep)
    //   Jet Flanger     inspired by the A/DA: 0.35 .. 12.25 ms (35:1), exponential sweep, compressor in front,
    //                   the two sides' lines sweep in opposite directions
    //   AC Flanger      the MXR with its Reticon line: darker, the line clips sooner, fixed 1:1 mix
    //   80A Flanger     the A/DA with its Reticon line: mono, fixed mix, compressor, Even / Odd turns the
    //                   regeneration's polarity (peaks on the even or on the odd harmonics of the delay)
    void processFlanger (float* l, float* r, int n, const mod_detail::FlangerVoicing& v) noexcept
    {
        using namespace mod_detail;
        const double inc = rateHz * invFs, maxDelay = 0.001 * v.maxDelayMs * fs, lnRatio = std::log (v.ratio);
        const float drive = v.drive, invDrive = 1.0f / v.drive;
        for (int i = 0; i < n; ++i)
        {
            float inL = l[i], inR = r[i];
            const float m = 0.5f * (inL + inR);
            float feed = m;
            if (v.compressor)
            {
                // the compressor in front: 2:1 above -20 dB, 2 dB of make-up. It works on the dry signal too
                // (so the comb stays deep), but fades out of it below Mix 50 %, so that Mix 0 % is the input.
                const float a = std::abs (m);
                compEnv += (a > compEnv ? compAttack : compRelease) * (a - compEnv);
                const float g = 1.25f * std::sqrt (0.1f / std::max (compEnv, 0.1f));
                const float dryGain = 1.0f + sm[smB].next() * (g - 1.0f);
                feed = m * g;
                inL *= dryGain;
                inR *= dryGain;
            }
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            const double depth = sm[smDepth].next(), manual = sm[smA].next();
            const float fb = sm[smFeedback].next(), dry = sm[smDry].next(), wet = sm[smWet].next();

            double cv = manual + depth * (1.0 - std::abs (2.0 * phase - 1.0) - manual);
            float w = invDrive * lines[0].read ((float) (v.exponential ? maxDelay * std::exp (-lnRatio * cv) : maxDelay / (1.0 + (v.ratio - 1.0) * cv)));
            lp[0] += wetLowPass * (w - lp[0]);
            lp[1] += wetLowPass * (lp[0] - lp[1]);
            lines[0].push (softClip (drive * (feed + fb * lp[1])));

            if (v.mono)
            {
                l[i] = r[i] = dry * 0.5f * (inL + inR) + wet * lp[1];
                continue;
            }

            double pr = phase + v.offset;
            if (pr >= 1.0) pr -= 1.0;
            cv = manual + depth * (1.0 - std::abs (2.0 * pr - 1.0) - manual);
            w = invDrive * lines[1].read ((float) (v.exponential ? maxDelay * std::exp (-lnRatio * cv) : maxDelay / (1.0 + (v.ratio - 1.0) * cv)));
            lp[2] += wetLowPass * (w - lp[2]);
            lp[3] += wetLowPass * (lp[2] - lp[3]);
            lines[1].push (softClip (drive * (feed + fb * lp[3])));

            l[i] = dry * inL + wet * lp[1];
            r[i] = dry * inR + wet * lp[3];
        }
    }

    //==============================================================================
    // Frequency Shifter (modular-synth, Bode type): single-sideband modulation. The phase splitter makes two
    // copies of the signal 90 degrees apart; multiplied with a quadrature oscillator and added, one sideband
    // cancels and every partial moves by the same number of Hz (so harmonics stop being harmonic).
    // Stereo mode: up on the left, down on the right. True stereo.
    void processShifter (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double inc = rateHz * invFs;
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            const float c = (float) std::cos (twoPi * phase), s = (float) std::sin (twoPi * phase);
            const float dry = sm[smDry].next(), wet = sm[smWet].next();
            const float dirL = sm[smDirL].next(), dirR = sm[smDirR].next();

            float si = hilbertPath (inL, hilbertI, hilbert);
            float sq = hilbertDelay[0];
            hilbertDelay[0] = hilbertPath (inL, hilbertQ, hilbert + 14);
            l[i] = dry * inL + wet * (si * c - dirL * sq * s);

            si = hilbertPath (inR, hilbertI, hilbert + 28);
            sq = hilbertDelay[1];
            hilbertDelay[1] = hilbertPath (inR, hilbertQ, hilbert + 42);
            r[i] = dry * inR + wet * (si * c - dirR * sq * s);
        }
    }

    //==============================================================================
    // Ring Modulator: the signal times a carrier, which leaves the sum and the difference of every partial with
    // the carrier (both sidebands) and nothing of the original. Depth below 100 % lets the original back in
    // (amplitude modulation); Shape bends the carrier towards a square; AM/FM blends over to frequency
    // modulation, where the carrier moves a short delay instead. The right side's carrier is 90 degrees ahead.
    void processRing (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double inc = rateHz * invFs;
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            const double amount = sm[smShape].next(), g = 12.0 * amount * amount, depth = sm[smDepth].next();
            const double carL = sineToSquare (std::sin (twoPi * phase), g), carR = sineToSquare (std::cos (twoPi * phase), g);
            const float fmL = lines[0].read ((float) (fs * (0.0006 + 0.00045 * depth * carL)));
            const float fmR = lines[0].read ((float) (fs * (0.0006 + 0.00045 * depth * carR)));
            lines[0].push (0.5f * (inL + inR));
            const float amL = inL * (float) (1.0 - depth + depth * carL), amR = inR * (float) (1.0 - depth + depth * carR);
            const float blend = sm[smA].next(), dry = sm[smDry].next(), wet = sm[smWet].next();
            l[i] = dry * inL + wet * (amL + blend * (fmL - amL));
            r[i] = dry * inR + wet * (amR + blend * (fmR - amR));
        }
    }

    //==============================================================================
    // Rotors: the speed moves towards slow / fast exponentially, with the rotor's own time constants.
    static void spin (double& speed, double& angle, double target, double up, double down, double invRate) noexcept
    {
        speed += (target > speed ? up : down) * (target - speed);
        angle += speed * invRate;
        if (angle >= 1.0) angle -= 1.0;
    }

    // Rotary Drum (Fender Vibratone): a 10" speaker firing into one rotating foam drum - no horn. Two mics a
    // quarter turn apart hear the drum's mouth come and go: Doppler vibrato (+-0.35 ms of path), loudness and,
    // above 1.2 kHz, much stronger shadowing. 40 / 342 rpm; the light drum takes about 0.5 s to speed up and
    // 0.8 s to slow down. Drive is the amp in front, Tone the speaker's treble.
    void processDrum (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double up = glide (0.5), down = glide (0.8), base = 0.001 * fs, swing = 0.00035 * fs;
        const float band = (float) lowPass (5000.0), split = (float) lowPass (1200.0);
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            const float x = tubeClip (0.5f * (inL + inR) * sm[smB].next()) * sm[smC].next();
            lp[0] += band * (x - lp[0]);
            lp[1] += band * (lp[0] - lp[1]);
            lines[0].push (lp[1]);
            spin (drumSpeed, drumAngle, drumTarget, up, down, invFs);
            const double depth = sm[smDepth].next();
            const double c = std::cos (twoPi * drumAngle), s = std::sin (twoPi * drumAngle);
            const float micL = rotorTap (lines[0], c, depth, base, swing, split, lp, 4, 0.45f, 0.85f);
            const float micR = rotorTap (lines[0], -s, depth, base, swing, split, lp, 5, 0.45f, 0.85f);
            const float tone = sm[smTone].next();
            lp[2] += tone * (micL - lp[2]);
            lp[3] += tone * (micR - lp[3]);
            const float dry = sm[smDry].next(), wet = sm[smWet].next() * (float) (1.0 + 0.3 * depth);
            l[i] = dry * inL + wet * lp[2];
            r[i] = dry * inR + wet * lp[3];
        }
    }

    // Rotary Drm/Hrn (Leslie 145): tube amp, 800 Hz crossover, treble horn and bass drum turning in opposite
    // directions, two mics a quarter turn apart. Horn: 48 / 408 rpm, up to speed in about a second, strong
    // Doppler (+-0.5 ms) and shadowing. Drum: 40 / 342 rpm, heavy - about 4.5 s up and 3.5 s down - with
    // little Doppler and mostly a swell in loudness. So after a speed change the two drift against each
    // other. Depth is the drum's modulation, Horn Dep the horn's.
    void processLeslie (float* l, float* r, int n) noexcept
    {
        using namespace mod_detail;
        const double hornUp = glide (0.3), hornDown = glide (0.5), drumUp = glide (1.5), drumDown = glide (1.2);
        const double base = 0.001 * fs, hornSwing = 0.0005 * fs, drumSwing = 0.00025 * fs;
        const float cross = (float) lowPass (800.0), top = (float) lowPass (7000.0);
        const float hornSplit = (float) lowPass (2500.0), drumSplit = (float) lowPass (400.0);
        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            const float x = tubeClip (0.5f * (inL + inR) * sm[smB].next()) * sm[smC].next();
            // 12 dB per octave both ways; the horn is wired out of phase, as such a crossover needs to add up flat
            lp[0] += cross * (x - lp[0]);
            lp[1] += cross * (lp[0] - lp[1]);
            const float h1 = x - lp[8];
            lp[8] += cross * h1;
            const float h2 = h1 - lp[9];
            lp[9] += cross * h2;
            lp[2] += top * (-h2 - lp[2]);
            lines[0].push (lp[2]);
            lines[1].push (lp[1]);
            spin (hornSpeed, hornAngle, hornTarget, hornUp, hornDown, invFs);
            spin (drumSpeed, drumAngle, drumTarget, drumUp, drumDown, invFs);
            const double drumDepth = sm[smDepth].next(), hornDepth = sm[smA].next();
            const double hc = std::cos (twoPi * hornAngle), hs = std::sin (twoPi * hornAngle);
            const double dc = std::cos (twoPi * drumAngle), ds = std::sin (twoPi * drumAngle);
            const float micL = (float) (1.0 + 0.35 * hornDepth) * rotorTap (lines[0], hc, hornDepth, base, hornSwing, hornSplit, lp, 4, 0.5f, 0.9f)
                             + (float) (1.0 + 0.2 * drumDepth) * rotorTap (lines[1], dc, drumDepth, base, drumSwing, drumSplit, lp, 6, 0.4f, 0.7f);
            const float micR = (float) (1.0 + 0.35 * hornDepth) * rotorTap (lines[0], -hs, hornDepth, base, hornSwing, hornSplit, lp, 5, 0.5f, 0.9f)
                             + (float) (1.0 + 0.2 * drumDepth) * rotorTap (lines[1], ds, drumDepth, base, drumSwing, drumSplit, lp, 7, 0.4f, 0.7f);
            const float dry = sm[smDry].next(), wet = sm[smWet].next();
            l[i] = dry * inL + wet * micL;
            r[i] = dry * inR + wet * micR;
        }
    }

    //==============================================================================
    double fs = 48000.0, invFs = 1.0 / 48000.0;
    int variant = patternTremolo;

    // from the knobs
    double rateHz = 1.0, rate2Hz = 0.0, volSens = 0.0, riseStep = 1.0, drumTarget = 0.67, hornTarget = 0.8;
    int steps[4] {};
    float toneCoef = 1.0f, wetLowPass = 1.0f;
    Smoothed sm[numSmoothed];

    // fixed for the sample rate
    float envAttack = 0.0f, envRelease = 0.0f, compAttack = 0.0f, compRelease = 0.0f, tremCoef = 0.0f, fbLowPass = 0.0f, fbHighPass = 0.0f;
    double ldrFast = 0.0, ldrSlow = 0.0, cellUp = 0.0, cellDown = 0.0, lampUp = 0.0, lampDown = 0.0;
    float barberCoef[mod_detail::barberStages] {};
    float hilbertI[mod_detail::hilbertSections] {}, hilbertQ[mod_detail::hilbertSections] {};

    // state
    mod_detail::Line lines[2];
    double phase = 0.0, phase2 = 0.0, seqPhase = 0.0;     // LFOs, carrier, step sequencer (cycles)
    double ldrA = 0.0, ldrB = 0.0, lamp = 0.0, rise = 0.0; // photocells, the Uni-Vibe's lamp, the vibrato's rise
    double drumSpeed = 0.67, drumAngle = 0.0, hornSpeed = 0.8, hornAngle = 0.0;
    int seqStep = 0;
    float env = 0.0f, compEnv = 0.0f, trem1 = 1.0f, trem2 = 1.0f, fbL = 0.0f, fbR = 0.0f;
    float apL[16] {}, apR[16] {};                          // all-pass stages
    float lp[10] {};                                        // one-pole filters, used differently by every model
    float hilbert[4 * (2 * mod_detail::hilbertSections + 2)] {};  // left I, left Q, right I, right Q
    float hilbertDelay[2] {};
};

//==============================================================================
/** In the order of ModFx::Variant. */
inline std::vector<ModelInfo> modModels()
{
    const auto speed = [] (float def) { return hertz ("Speed", 0.05f, 10.0f, def, 1.0f); };
    const auto model = [] (const char* key, const char* name, int variant, const char* basedOn, std::vector<KnobSpec> knobs)
    {
        return ModelInfo { key, name, Category::modulation, Engine::modFx, variant, basedOn, std::move (knobs) };
    };

    return {
        model ("pattern_tremolo", "Pattern Tremolo", ModFx::patternTremolo, "Inspired by Lightfoot Labs Goatkeeper",
               { speed (2.0f), choice ("Step 1", modStepNames, 19, 0), choice ("Step 2", modStepNames, 19, 1),
                 choice ("Step 3", modStepNames, 19, 3), choice ("Step 4", modStepNames, 19, 1) }),
        model ("panner", "Panner", ModFx::panner, "Auto-panner",
               { speed (1.5f), percent ("Depth", 100.0f), percent ("Shape", 50.0f), percent ("VolSens", 0.0f), percent ("Mix", 100.0f) }),
        model ("bias_tremolo", "Bias Tremolo", ModFx::biasTremolo, "1960 Vox AC-15 tremolo",
               { speed (4.0f), percent ("Level", 50.0f), percent ("Shape", 0.0f), percent ("VolSens", 0.0f), percent ("Mix", 100.0f) }),
        model ("opto_tremolo", "Opto Tremolo", ModFx::optoTremolo, "Blackface Fender optical tremolo",
               { speed (5.0f), percent ("Level", 60.0f), percent ("Shape", 30.0f), percent ("VolSens", 0.0f), percent ("Mix", 100.0f) }),
        model ("script_phase", "Script Phase", ModFx::scriptPhase, "MXR Phase 90 (script logo)",
               { speed (0.7f) }),
        model ("panned_phaser", "Panned Phaser", ModFx::pannedPhaser, "Ibanez Flying Pan",
               { speed (0.5f), percent ("Depth", 70.0f), choice ("Pan", modPanNames, 3, 1),
                 hertz ("Pan Spd", 0.0f, 10.0f, 0.3f, 1.0f), percent ("Mix", 50.0f) }),
        model ("barberpole_phaser", "Barberpole Phaser", ModFx::barberpolePhaser, "Modular-synth barberpole phaser",
               { speed (0.3f), percent ("Fdbk", 40.0f), choice ("Mode", modSweepNames, 3, 0), percent ("Mix", 50.0f) }),
        model ("dual_phaser", "Dual Phaser", ModFx::dualPhaser, "Mu-Tron Bi-Phase",
               { speed (0.35f), percent ("Depth", 75.0f), percent ("Fdbk", 45.0f), percent ("LFO Shp", 0.0f), percent ("Mix", 50.0f) }),
        model ("u_vibe", "U-Vibe", ModFx::uVibe, "Uni-Vibe",
               { speed (1.6f), percent ("Depth", 80.0f), percent ("Fdbk", 0.0f), percent ("VolSens", 0.0f), percent ("Mix", 50.0f) }),
        model ("phaser_hd", "Phaser", ModFx::phaser, "Inspired by MXR Phase 90",
               { speed (0.5f), percent ("Depth", 70.0f), percent ("Fdbk", 40.0f), choice ("Stages", modStageNames, 4, 0), percent ("Mix", 50.0f) }),
        model ("pitch_vibrato", "Pitch Vibrato", ModFx::pitchVibrato, "Boss VB-2",
               { speed (5.0f), percent ("Depth", 40.0f), percent ("Rise", 75.0f), percent ("VolSens", 0.0f), percent ("Mix", 100.0f) }),
        model ("dimension", "Dimension", ModFx::dimension, "Roland Dimension D",
               { choice ("Sw1", modSwitchNames, 2, 0), choice ("Sw2", modSwitchNames, 2, 0), choice ("Sw3", modSwitchNames, 2, 1),
                 choice ("Sw4", modSwitchNames, 2, 0), percent ("Mix", 50.0f) }),
        model ("analog_chorus", "Analog Chorus", ModFx::analogChorus, "Boss CE-1 Chorus Ensemble",
               { speed (0.6f), percent ("Depth", 50.0f), choice ("Ch Vib", modChorusNames, 2, 0), percent ("Tone", 50.0f), percent ("Mix", 50.0f) }),
        model ("tri_chorus", "Tri Chorus", ModFx::triChorus, "Song Bird / DyTronics Tri-Stereo Chorus",
               { speed (0.5f), percent ("Depth", 60.0f), percent ("Depth2", 50.0f), percent ("Depth3", 60.0f), percent ("Mix", 50.0f) }),
        model ("analog_flanger", "Analog Flanger", ModFx::analogFlanger, "Inspired by MXR Flanger",
               { speed (0.3f), percent ("Depth", 70.0f), percent ("Fdbk", 50.0f), percent ("Manual", 30.0f), percent ("Mix", 50.0f) }),
        model ("jet_flanger", "Jet Flanger", ModFx::jetFlanger, "Inspired by A/DA Flanger",
               { speed (0.2f), percent ("Depth", 85.0f), percent ("Fdbk", 65.0f), percent ("Manual", 30.0f), percent ("Mix", 50.0f) }),
        model ("ac_flanger", "AC Flanger", ModFx::acFlanger, "MXR Flanger",
               { speed (0.35f), percent ("Width", 60.0f), percent ("Regen", 55.0f), percent ("Manual", 25.0f) }),
        model ("80a_flanger", "80A Flanger", ModFx::flanger80A, "A/DA Flanger",
               { speed (0.25f), percent ("Range", 80.0f), percent ("Enhance", 60.0f), percent ("Manual", 30.0f),
                 choice ("Even Odd", modHarmonicNames, 2, 0) }),
        model ("frequency_shifter", "Frequency Shifter", ModFx::frequencyShifter, "Modular-synth frequency shifter",
               { hertz ("Freq", 0.0f, 2000.0f, 12.0f, 40.0f), choice ("Mode", modSweepNames, 3, 0), percent ("Mix", 50.0f) }),
        model ("ring_modulator", "Ring Modulator", ModFx::ringModulator, "Ring modulator",
               { hertz ("Speed", 1.0f, 2000.0f, 120.0f, 100.0f), percent ("Depth", 100.0f), percent ("Shape", 0.0f),
                 percent ("AM/FM", 0.0f), percent ("Mix", 50.0f) }),
        model ("rotary_drum", "Rotary Drum", ModFx::rotaryDrum, "Fender Vibratone",
               { choice ("Speed", modRotorNames, 2, 1), percent ("Depth", 70.0f), percent ("Tone", 60.0f), percent ("Drive", 25.0f), percent ("Mix", 80.0f) }),
        model ("rotary_drm_hrn", "Rotary Drm/Hrn", ModFx::rotaryDrumHorn, "Leslie 145",
               { choice ("Speed", modRotorNames, 2, 0), percent ("Depth", 70.0f), percent ("Horn Dep", 70.0f), percent ("Drive", 30.0f), percent ("Mix", 100.0f) }),
    };
}

} // namespace fx
