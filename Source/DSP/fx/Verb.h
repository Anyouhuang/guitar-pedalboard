#pragma once

#include "../DspUtils.h"
#include "../ModelTypes.h"

namespace fx
{
inline const char* const verbConditionNames[] = { "Stable", "Critical", "Hazard" };

namespace verb_detail
{
constexpr int tickSize = 32;   // control rate: knob smoothing and LFOs move every 32 samples (any host block size)
constexpr int maxEarly = 12;   // early-reflection taps per side
constexpr int maxStages = 160; // all-pass stages per half spring

enum Type  { fdn = 0, springs, particle };
enum Extra { plain = 0, echoes, ducker, octave };

// the delay lines, all inside one buffer
enum { dPreL = 0, dPreR, dEarlyL, dEarlyR, dDiff, dLine = dDiff + 8, dAp = dLine + 8, dEchoL = dAp + 8, dEchoR, dShiftA, dShiftB, numDelays };

/** What makes one model: the structure (type / extra) and the sizes of its space. */
struct Program
{
    int type, extra;
    float rtMin, rtMax;           // decay time (s) at Decay 0 % and 100 %
    float level;                  // output gain: the wet signal sits a few dB under the input at the default knobs
    float toneHz;                 // output low-pass at Tone 50 %
    float lowCutHz;               // high-pass on the way in (also keeps DC out of the loops)
    float peakHz, peakDb, peakQ;  // voicing EQ on the wet signal (0 dB = none)

    // feedback delay network: 8 lines, Hadamard matrix, an all-pass inside every line
    float lineMs[8], tapMs[8];    // line lengths; where the outputs are picked up (even lines left, odd lines right)
    float apMs, apGain;           // the all-pass in line i is apMs * apSpread[i] long
    int numDiff;                  // input diffusers per side
    float diffMs[8], diffGain;    // [0..3] left, [4..7] right
    float dampHz, hfRatio;        // above dampHz the decay time is hfRatio times the mid one (at Tone 50 %)
    float bassHz, bassRatio;      // below bassHz it is bassRatio times the mid one
    float modMs, modHz;           // slow modulation of four lines
    float tankLevel, earlyLevel;
    int numEarly;
    float earlyMs[2][maxEarly], earlyGain[2][maxEarly];

    // springs
    int numSprings, stages;       // stages: dispersion all-passes per half of a spring
    float springMs[3];            // one-way transit time of each spring
    float springCoef, drive;      // all-pass coefficient (how strong the chirp is); tube drive in front of the tank
};

inline const float apSpread[8]    = { 1.0f, 1.19f, 1.41f, 1.69f, 1.93f, 2.23f, 2.53f, 2.83f };
inline const float rateSpread[4]  = { 1.0f, 1.31f, 0.77f, 1.63f };
inline const float shiftMs[2]     = { 61.3f, 70.9f };                       // pitch shifter windows
inline const float particleApMs[8] = { 23.7f, 37.1f, 53.9f, 71.3f, 26.3f, 34.7f, 57.7f, 67.9f };
inline const float particleDelayMs[2] = { 97.1f, 113.3f };

inline const Program programs[] = {
    // Plate - a studio plate (EMT 140 style): no separate reflections, the full density from the first
    // milliseconds (four diffusers, short output taps), bright, the lows ring longer than the highs.
    { fdn, plain, 0.4f, 6.0f, 0.192f, 12000.0f, 120.0f, 0.0f, 0.0f, 1.0f,
      { 23.9f, 28.1f, 31.7f, 37.3f, 41.9f, 47.1f, 53.3f, 59.9f }, { 0.3f, 0.5f, 2.9f, 3.7f, 6.1f, 7.3f, 9.7f, 11.3f },
      3.7f, 0.55f, 4, { 1.9f, 3.7f, 5.9f, 8.3f, 2.3f, 3.1f, 6.7f, 7.9f }, 0.7f,
      8000.0f, 0.6f, 350.0f, 1.3f, 0.25f, 0.9f, 1.0f, 0.0f, 0, {}, {}, 0, 0, {}, 0.0f, 0.0f },

    // Room - a small studio live room: mostly early reflections (12 taps per side in the first 60 ms),
    // a short, quieter tail behind them.
    { fdn, plain, 0.15f, 1.6f, 0.184f, 7000.0f, 90.0f, 0.0f, 0.0f, 1.0f,
      { 17.3f, 20.9f, 23.9f, 27.7f, 31.9f, 35.3f, 39.7f, 44.9f }, { 0.3f, 0.5f, 3.1f, 4.3f, 7.7f, 9.1f, 13.3f, 15.1f },
      2.3f, 0.5f, 3, { 2.1f, 4.9f, 7.1f, 0.0f, 2.7f, 4.3f, 7.7f, 0.0f }, 0.6f,
      5000.0f, 0.5f, 200.0f, 0.85f, 0.2f, 0.7f, 0.8f, 1.0f, 12,
      { { 2.9f, 6.7f, 9.5f, 13.3f, 17.9f, 21.1f, 26.3f, 30.7f, 36.1f, 41.9f, 49.3f, 57.1f },
        { 3.7f, 5.9f, 10.9f, 14.7f, 16.3f, 22.9f, 25.1f, 32.3f, 37.7f, 43.3f, 47.9f, 59.3f } },
      { { 0.84f, -0.71f, 0.66f, 0.58f, -0.52f, 0.47f, -0.41f, 0.38f, 0.33f, -0.29f, 0.25f, -0.21f },
        { 0.80f, 0.74f, -0.63f, 0.57f, 0.54f, -0.45f, 0.42f, -0.36f, 0.34f, 0.28f, -0.26f, 0.20f } },
      0, 0, {}, 0.0f, 0.0f },

    // Chamber - an elongated echo chamber (hallway, stairwell): a few short lines for the cross-section and
    // long ones for the length, reflections that come back in pairs along the long axis.
    { fdn, plain, 0.4f, 5.0f, 0.194f, 7500.0f, 90.0f, 0.0f, 0.0f, 1.0f,
      { 14.3f, 17.9f, 21.1f, 26.9f, 33.7f, 47.9f, 63.1f, 83.3f }, { 0.3f, 0.5f, 5.3f, 6.7f, 11.3f, 14.9f, 23.3f, 31.1f },
      2.9f, 0.55f, 3, { 3.1f, 5.3f, 9.1f, 0.0f, 3.7f, 4.7f, 9.7f, 0.0f }, 0.65f,
      5500.0f, 0.5f, 250.0f, 1.1f, 0.3f, 0.6f, 1.0f, 0.6f, 8,
      { { 4.1f, 11.3f, 21.7f, 26.3f, 43.1f, 48.7f, 64.9f, 86.3f }, { 5.3f, 9.7f, 22.9f, 27.7f, 41.9f, 50.3f, 66.7f, 84.1f } },
      { { 0.70f, -0.62f, 0.66f, 0.50f, -0.52f, 0.40f, 0.38f, -0.28f }, { 0.68f, 0.60f, -0.64f, 0.52f, 0.50f, -0.41f, 0.36f, 0.29f } },
      0, 0, {}, 0.0f, 0.0f },

    // Hall - a concert hall: long lines, sparse soft reflections, output taps deep inside the lines so the
    // sound builds up slowly; long smooth tail, warm (long bass decay, damped highs).
    { fdn, plain, 0.8f, 10.0f, 0.265f, 6500.0f, 80.0f, 0.0f, 0.0f, 1.0f,
      { 41.3f, 47.9f, 56.3f, 63.7f, 72.1f, 81.7f, 90.3f, 101.9f }, { 0.4f, 0.6f, 17.1f, 21.3f, 33.7f, 38.9f, 55.3f, 61.1f },
      5.3f, 0.6f, 4, { 4.7f, 8.9f, 13.3f, 19.1f, 5.3f, 8.3f, 14.1f, 18.1f }, 0.7f,
      3800.0f, 0.35f, 250.0f, 1.35f, 0.5f, 0.45f, 1.0f, 0.35f, 8,
      { { 8.3f, 19.1f, 27.7f, 38.9f, 51.1f, 63.7f, 79.3f, 97.1f }, { 10.1f, 17.3f, 29.9f, 41.3f, 47.9f, 66.1f, 82.7f, 94.3f } },
      { { 0.50f, -0.45f, 0.48f, 0.40f, -0.36f, 0.33f, -0.28f, 0.24f }, { 0.48f, 0.46f, -0.44f, 0.41f, 0.37f, -0.32f, 0.27f, -0.25f } },
      0, 0, {}, 0.0f, 0.0f },

    // Echo - Line 6 original: a ping-pong echo (its time follows PreDelay) whose repeats are heard on their
    // own and also feed a lush, strongly modulated tail.
    { fdn, echoes, 0.8f, 8.0f, 0.203f, 6500.0f, 100.0f, 0.0f, 0.0f, 1.0f,
      { 31.1f, 36.7f, 42.9f, 49.3f, 55.7f, 62.3f, 69.1f, 76.9f }, { 0.4f, 0.6f, 9.1f, 11.7f, 19.3f, 23.9f, 31.7f, 37.3f },
      4.3f, 0.6f, 4, { 3.7f, 6.7f, 10.1f, 14.9f, 4.1f, 6.1f, 11.3f, 13.7f }, 0.7f,
      4500.0f, 0.45f, 250.0f, 1.1f, 0.8f, 0.55f, 1.0f, 0.0f, 0, {}, {}, 0, 0, {}, 0.0f, 0.0f },

    // Tile - a small tiled room: hard walls, so strong, bright, clearly separate reflections (little
    // diffusion), short lines, hardly any damping, a bright ring around 3 kHz.
    { fdn, plain, 0.2f, 2.5f, 0.161f, 11000.0f, 140.0f, 3200.0f, 3.0f, 0.9f,
      { 11.9f, 14.3f, 16.9f, 19.7f, 22.7f, 26.3f, 30.1f, 34.7f }, { 0.3f, 0.5f, 2.3f, 3.1f, 5.9f, 7.1f, 10.3f, 12.7f },
      1.7f, 0.45f, 2, { 1.3f, 3.1f, 0.0f, 0.0f, 1.7f, 2.9f, 0.0f, 0.0f }, 0.5f,
      9000.0f, 0.75f, 250.0f, 0.8f, 0.12f, 1.1f, 0.7f, 1.0f, 8,
      { { 2.3f, 5.3f, 8.9f, 12.1f, 17.3f, 23.9f, 31.1f, 39.7f }, { 3.1f, 4.7f, 9.7f, 13.9f, 16.1f, 25.3f, 29.9f, 41.3f } },
      { { 0.90f, -0.78f, 0.70f, 0.61f, -0.52f, 0.44f, -0.37f, 0.30f }, { 0.88f, 0.80f, -0.68f, 0.60f, 0.55f, -0.43f, 0.38f, -0.29f } },
      0, 0, {}, 0.0f, 0.0f },

    // Cave - Line 6 original: a huge cavern. Very long lines, far-wall slaps up to a quarter of a second
    // away, dark (strong damping), a booming low-mid resonance and a bass that rings longest.
    { fdn, plain, 1.5f, 14.0f, 0.277f, 3200.0f, 60.0f, 280.0f, 5.0f, 1.4f,
      { 61.7f, 73.3f, 84.1f, 97.3f, 109.9f, 124.7f, 139.1f, 157.3f }, { 0.5f, 0.7f, 29.3f, 37.1f, 61.3f, 71.9f, 97.3f, 113.1f },
      7.1f, 0.6f, 4, { 7.9f, 12.7f, 19.9f, 27.1f, 8.9f, 11.3f, 21.1f, 25.9f }, 0.7f,
      2200.0f, 0.3f, 300.0f, 1.6f, 0.7f, 0.3f, 1.0f, 0.5f, 6,
      { { 23.3f, 57.1f, 89.9f, 131.3f, 187.7f, 251.9f }, { 31.7f, 49.3f, 97.1f, 139.7f, 173.9f, 263.3f } },
      { { 0.55f, -0.50f, 0.46f, 0.40f, -0.34f, 0.28f }, { 0.53f, 0.50f, -0.45f, 0.41f, 0.35f, -0.27f } },
      0, 0, {}, 0.0f, 0.0f },

    // Ducking - a hall whose level is pulled down by an envelope follower on the dry signal and swells
    // back when the playing stops.
    { fdn, ducker, 0.8f, 10.0f, 0.255f, 7000.0f, 90.0f, 0.0f, 0.0f, 1.0f,
      { 37.9f, 44.3f, 51.7f, 59.3f, 67.7f, 76.1f, 85.9f, 95.3f }, { 0.4f, 0.6f, 13.3f, 16.9f, 27.1f, 32.3f, 45.7f, 51.1f },
      4.9f, 0.6f, 4, { 4.3f, 7.9f, 12.1f, 17.3f, 4.9f, 7.3f, 12.9f, 16.7f }, 0.7f,
      4200.0f, 0.4f, 250.0f, 1.2f, 0.5f, 0.5f, 1.0f, 0.3f, 6,
      { { 9.1f, 20.3f, 31.9f, 44.3f, 58.7f, 77.9f }, { 11.3f, 18.7f, 33.1f, 46.1f, 56.3f, 80.3f } },
      { { 0.50f, -0.46f, 0.44f, 0.38f, -0.33f, 0.27f }, { 0.48f, 0.47f, -0.42f, 0.39f, 0.32f, -0.28f } },
      0, 0, {}, 0.0f, 0.0f },

    // Octo - Line 6 original: a shimmer. What comes out of two of the eight lines goes through an octave-up
    // pitch shifter and back into the network, so every layer grows another one an octave above it.
    { fdn, octave, 1.5f, 15.0f, 0.193f, 8000.0f, 120.0f, 0.0f, 0.0f, 1.0f,
      { 37.1f, 43.3f, 50.9f, 57.7f, 65.3f, 73.9f, 81.1f, 89.9f }, { 0.4f, 0.6f, 12.7f, 15.1f, 25.3f, 29.9f, 41.9f, 47.3f },
      4.7f, 0.6f, 4, { 6.1f, 10.7f, 16.3f, 23.9f, 6.7f, 9.7f, 17.9f, 22.3f }, 0.7f,
      6000.0f, 0.6f, 250.0f, 1.0f, 0.6f, 0.5f, 1.0f, 0.0f, 0, {}, {}, 0, 0, {}, 0.0f, 0.0f },

    // Spring - a studio spring reverb: three springs of different length, moderate dispersion, clean drive,
    // the springs coupled to each other so the echoes blur quickly.
    { springs, plain, 0.8f, 4.0f, 0.579f, 5200.0f, 80.0f, 0.0f, 0.0f, 1.0f,
      {}, {}, 0.0f, 0.0f, 0, {}, 0.0f, 1500.0f, 0.55f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0, {}, {},
      3, 64, { 29.3f, 36.7f, 43.1f }, 0.5f, 0.0f },

    // '63 Spring - Fender 6G15 tube reverb unit: a two-spring tank driven by a tube (asymmetric soft
    // clipping), strong dispersion (the "drip"), band-limited to the tank's 100 Hz .. 4 kHz.
    { springs, plain, 1.0f, 5.0f, 0.661f, 4200.0f, 130.0f, 0.0f, 0.0f, 1.0f,
      {}, {}, 0.0f, 0.0f, 0, {}, 0.0f, 1500.0f, 0.45f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0, {}, {},
      2, 150, { 33.1f, 41.3f, 0.0f }, 0.5f, 1.4f },

    // Particle Verb - Line 6 original: two cross-coupled chains of long modulated all-passes (a pad with a
    // slow attack) with a pitch shifter in each feedback path: off (Stable), a few cents up per trip
    // (Critical), or swinging over an octave with more feedback and modulation (Hazard).
    { particle, plain, 2.0f, 30.0f, 0.754f, 9000.0f, 100.0f, 0.0f, 0.0f, 1.0f,
      {}, {}, 0.0f, 0.62f, 4, { 6.1f, 10.7f, 16.3f, 23.9f, 6.7f, 9.7f, 17.9f, 22.3f }, 0.7f, 5000.0f, 1.0f, 0.0f, 1.0f, 1.2f, 0.45f, 1.0f, 0.0f, 0, {}, {},
      0, 0, {}, 0.0f, 0.0f },
};

inline float zap (float v) noexcept { return (v > 1.0e-18f || v < -1.0e-18f) ? v : 0.0f; } // no denormals in the loops

inline int nextPrime (int n) noexcept
{
    if (n < 3)
        return 3;
    for (n |= 1;; n += 2)
    {
        bool prime = true;
        for (int d = 3; d * d <= n; d += 2)
            if (n % d == 0) { prime = false; break; }
        if (prime)
            return n;
    }
}

inline float poleCoef (double hz, double fs) noexcept { return (float) (1.0 - std::exp (-2.0 * pi * std::min (hz, 0.45 * fs) / fs)); }
} // namespace verb_detail

//==============================================================================
/** The HD500X's reverb models (12). Stereo; returns the wet signal only.
    Knobs:  Decay, PreDelay (ms), Tone, Mix   |   Particle Verb: Dwell, Condition, Gain, Mix

    Three structures:
    - a feedback delay network (8 modulated lines, Hadamard matrix, an all-pass in every line, decay per band)
      behind a pre-delay, early-reflection taps and input diffusers: Plate, Room, Chamber, Hall, Echo (plus a
      ping-pong echo), Tile, Cave, Ducking (plus a ducker), Octo (plus a feedback path through octave-up shifters)
    - springs: recirculating delays with chains of first-order all-passes (dispersion), run at about 9.6 kHz
    - Particle Verb: two cross-coupled all-pass chains with a pitch shifter in each feedback path */
class VerbFx
{
public:
    enum Variant { plate = 0, room, chamber, hall, echo, tile, cave, ducking, octo, spring, spring63, particleVerb, numVariants };

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        using namespace verb_detail;
        fs = sampleRate;

        // one buffer for every delay line, each a power-of-two stretch of it
        int total = 0;
        for (int d = 0; d < numDelays; ++d)
        {
            const double seconds = d < dEarlyL ? 0.2 : d < dDiff ? 0.28 : d < dLine ? 0.03 : d < dAp ? 0.17
                                 : d < dEchoL ? 0.08 : d < dShiftA ? 0.51 : 0.09;
            int size = 64;
            while (size < (int) (seconds * fs) + 8)
                size *= 2;
            bs[d] = total;
            mk[d] = size - 1;
            total += size;
        }
        pool.assign ((size_t) total, 0.0f);
        chain.assign ((size_t) (6 * maxStages), 0.0f);

        smoothCoef = 1.0 / (1.0 + 0.05 * fs / tickSize);
        fadeLen = (int) (0.04 * fs);
        fadeStep = 1.0f / (float) fadeLen;
        decim = std::max (1, (int) std::floor (fs / 9600.0 + 0.5));
        driveGain.reset (fs, 0.03);
        reset();
    }

    void reset()
    {
        using namespace verb_detail;
        std::fill (pool.begin(), pool.end(), 0.0f);
        std::fill (chain.begin(), chain.end(), 0.0f);
        wp = wpLow = tickPos = decPhase = 0;

        activeVariant = variant;
        prog = &programs[variant];
        decaySm = decayTarget;
        toneSm = toneTarget;
        activeCondition = condition;
        activeEcho = echoTarget;
        preCur = preNext = preTarget;
        echoCur = echoNext = echoTarget;
        preFade = echoFade = 0;
        driveGain.setCurrentAndTarget (driveGain.getTarget());

        for (int i = 0; i < 8; ++i)
            dampHi[i] = dampLo[i] = 0.0f;
        for (int i = 0; i < 4; ++i)
            tone[i] = 0.0f;
        for (int i = 0; i < 3; ++i)
            springFb[i] = springLp[i] = 0.0f;
        for (int i = 0; i < 2; ++i)
        {
            lowState[i] = loopBack[i] = loopHp[i] = lowPrev[i] = lowCur[i] = shiftPole[i] = 0.0f;
            voice[i].reset();
            shiftLp[i].reset();
            shiftHp[i].reset();
            shiftPhase[i] = 0.5;
            shiftRate[i] = 0.0;
            hazardPhase[i] = 1.3 * i;
        }
        for (auto& f : aaIn)
            f.reset();
        for (auto& f : aaOut)
            f.reset();
        echoLp = lowHp = duckEnv = duckAmount = 0.0f;

        configure();
        depthSm = depthTarget();
        updateCoefficients();
        for (int c = 0; c < numCoefs; ++c)
        {
            coef[c] = coefTarget[c];
            coefInc[c] = 0.0f;
        }
        ramping = false;
        outGain = outGainTarget;
        outGainInc = 0.0;

        for (int i = 0; i < 4; ++i)
        {
            modPhase[i] = 1.7 * i;
            modDelay[i] = modBase[i] + depthSm * std::sin (modPhase[i]);
            modInc[i] = 0.0;
        }
        if (prog->type == particle && condition == 1)
            for (int i = 0; i < 2; ++i)
                shiftRate[i] = criticalRate (i);
    }

    void setModel (int newVariant) noexcept { variant = std::clamp (newVariant, 0, (int) numVariants - 1); }

    void setParameters (const float* k)
    {
        using namespace verb_detail;
        decayTarget = std::clamp ((double) k[0] * 0.01, 0.0, 1.0);

        if (programs[variant].type == particle)
        {
            condition = std::clamp ((int) std::floor ((double) k[1] + 0.5), 0, 2);
            driveGain.setTarget ((float) std::pow (10.0, ((double) k[2] - 50.0) * 0.012)); // +-12 dB
            toneTarget = 0.5;
            preTarget = 0;
        }
        else
        {
            preTarget = (int) std::floor (std::clamp ((double) k[1], 0.0, 200.0) * 0.001 * fs + 0.5);
            toneTarget = std::clamp ((double) k[2] * 0.01, 0.0, 1.0);
        }

        echoTarget = (int) std::floor (0.1 * fs + 0.5) + 2 * preTarget; // Echo: 100 ms + twice the pre-delay
        mix = std::clamp (k[3] * 0.01f, 0.0f, 1.0f);
    }

    float getMix() const noexcept { return mix; }

    /** About the time the tail needs to fall 100 dB at the current knobs (plus the pre-delay). */
    float getTailSeconds() const noexcept
    {
        using namespace verb_detail;
        const Program& p = programs[variant];
        double rt = (double) p.rtMin * std::pow ((double) p.rtMax / (double) p.rtMin, decayTarget);
        if (p.type == fdn)
            rt *= p.extra == octave ? 1.3 : std::max (1.0, (double) p.bassRatio); // Octo: the octave layers ring on

        double tail = rt * (100.0 / 60.0) + (double) preTarget / fs;
        if (p.extra == echoes)
            tail += 2.0 * (double) echoTarget / fs;
        if (p.type == springs)
            tail += 0.1;
        return (float) std::clamp (tail, 0.5, 20.0);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        using namespace verb_detail;
        if (activeVariant != variant)
            reset();

        for (int pos = 0; pos < numSamples;)
        {
            if (tickPos == 0)
                controlTick();

            const int n = std::min (numSamples - pos, tickSize - tickPos);
            inputStage (left + pos, right + pos, n);

            if (prog->type == fdn)           tankStage (n);
            else if (prog->type == springs)  springStage (n);
            else                             particleStage (n);

            outputStage (left + pos, right + pos, n);
            wp = (wp + n) & 0x3fffffff;
            tickPos = (tickPos + n) & (tickSize - 1);
            pos += n;
        }
    }

private:
    using Program = verb_detail::Program;

    //==============================================================================
    /** Lengths, fixed filters and gains of the current model at this sample rate (no allocation). */
    void configure() noexcept
    {
        using namespace verb_detail;
        const Program& p = *prog;
        const double perMs = 0.001 * fs;
        auto whole = [] (double v) { return (int) std::floor (v + 0.5); };

        duckOn = p.extra == ducker;
        echoOn = p.extra == echoes;
        shiftOn = p.extra == octave;
        lowCoef = poleCoef (p.lowCutHz, fs);
        voiceOn = p.peakDb != 0.0f;
        if (voiceOn)
            for (auto& v : voice)
                v.setPeak (fs, p.peakHz, p.peakQ, p.peakDb);

        numEarly = 0;
        numDiff = p.numDiff;
        diffGain = p.diffGain;
        for (int i = 0; i < 8; ++i)
            diffLen[i] = nextPrime (whole ((double) p.diffMs[i] * perMs));
        for (int i = 0; i < 4; ++i)
        {
            modStep[i] = 2.0 * pi * (double) p.modHz * (double) rateSpread[i] * tickSize / fs;
            modBase[i] = 8.0;
        }
        for (int i = 0; i < 2; ++i)
        {
            shiftLen[i] = whole ((double) shiftMs[i] * perMs);
            shiftLp[i].setLowPass (fs, p.type == particle ? 5000.0 : 3400.0, 0.7071);
        }
        shiftCoef = poleCoef (5000.0, fs);
        for (auto& f : shiftHp)
            f.setHighPass (fs, 160.0, 0.7071);

        if (p.type == fdn)
        {
            numEarly = p.numEarly;
            apGain = p.apGain;
            tankLevel = p.tankLevel;
            loCoef = poleCoef (p.bassHz, fs);
            for (int i = 0; i < 8; ++i)
            {
                lineLen[i] = nextPrime (whole ((double) p.lineMs[i] * perMs));
                apLen[i] = nextPrime (whole ((double) p.apMs * (double) apSpread[i] * perMs));
                tapLen[i] = std::clamp (whole ((double) p.tapMs[i] * perMs), 1, lineLen[i] * 3 / 4);
                loopLen[i] = (double) lineLen[i];
                if (i < 4)
                    modBase[i] = (double) lineLen[i];
            }
            for (int side = 0; side < 2; ++side)
                for (int k = 0; k < numEarly; ++k)
                {
                    const int e = side * maxEarly + k;
                    earlyLen[e] = std::max (1, whole ((double) p.earlyMs[side][k] * perMs));
                    earlyGain[e] = p.earlyGain[side][k] * p.earlyLevel;
                    earlyBase[e] = bs[((k & 1) ^ side) != 0 ? dEarlyR : dEarlyL]; // every other tap comes from the other side
                }

            if (shiftOn)
                for (int i = 0; i < 2; ++i)
                    shiftRate[i] = -1.0 / (double) shiftLen[i]; // one octave up

            echoCoef = poleCoef (4500.0, fs);
            duckHold = (float) std::exp (-1.0 / (0.05 * fs));
            duckAttack = (float) (1.0 - std::exp (-1.0 / (0.01 * fs)));
            duckRelease = (float) (1.0 - std::exp (-1.0 / (0.25 * fs)));
        }
        else if (p.type == springs)
        {
            const double fsLow = fs / decim;
            static constexpr double q6[3] = { 0.5176, 0.7071, 1.9319 }; // sixth-order Butterworth
            static constexpr float outL3[3] = { 1.0f, 0.5f, -0.5f }, outR3[3] = { 0.5f, -0.5f, 1.0f };
            static constexpr float outL2[3] = { 1.0f, 0.6f, 0.0f },  outR2[3] = { -0.6f, 1.0f, 0.0f };
            for (int i = 0; i < 3; ++i)
            {
                aaIn[i].setLowPass (fs, 0.4 * fsLow, q6[i]);
                aaOut[i].setLowPass (fs, 0.42 * fsLow, q6[i]);
                aaOut[3 + i].setLowPass (fs, 0.42 * fsLow, q6[i]);
            }

            numSprings = p.numSprings;
            stages = std::min (p.stages, maxStages);
            for (int s = 0; s < 3; ++s)
            {
                springOutL[s] = numSprings == 3 ? outL3[s] : outL2[s];
                springOutR[s] = numSprings == 3 ? outR3[s] : outR2[s];
            }
            springCoef = p.springCoef;
            const double lowDelay = (double) stages * (1.0 - (double) p.springCoef) / (1.0 + (double) p.springCoef); // of one chain, at low frequencies
            for (int s = 0; s < numSprings; ++s)
            {
                springLen[s] = std::max (2, whole ((double) p.springMs[s] * 0.001 * fsLow - lowDelay));
                apLen[2 * s] = nextPrime (whole (0.37 * (double) p.springMs[s] * 0.001 * fsLow));     // reflections inside the spring
                apLen[2 * s + 1] = nextPrime (whole (0.23 * (double) p.springMs[s] * 0.001 * fsLow));
                loopLen[s] = 2.0 * ((double) springLen[s] + lowDelay) / fsLow; // seconds per round trip
            }
            lowCoef = poleCoef (p.lowCutHz, fsLow);
            loCoef = poleCoef (p.dampHz, fsLow);

            drive = p.drive;
            driveBias = std::tanh (0.25f);
            driveNorm = drive > 0.0f ? 1.0f / (drive * (1.0f - driveBias * driveBias)) : 1.0f;
        }
        else
        {
            for (int side = 0; side < 2; ++side)
            {
                for (int k = 0; k < 4; ++k)
                {
                    const int a = side * 4 + k;
                    apLen[a] = nextPrime (whole ((double) particleApMs[a] * perMs));
                    if (k < 2)
                        modBase[side * 2 + k] = (double) apLen[a];
                }
                lineLen[side] = nextPrime (whole ((double) particleDelayMs[side] * perMs));
                loopLen[side] = ((double) lineLen[side] + 0.5 * (double) shiftLen[side]) / fs; // seconds in the delay and the shifter
            }
        }
    }

    double depthTarget() const noexcept
    {
        const double depth = (double) prog->modMs * 0.001 * fs;
        return prog->type == verb_detail::particle && activeCondition == 2 ? 2.5 * depth : depth;
    }

    double criticalRate (int side) const noexcept { return (1.0 - std::pow (2.0, 14.0 / 1200.0)) / (double) shiftLen[side]; }

    /** Everything that follows the Decay and Tone knobs (called at the control rate while they move). */
    void updateCoefficients() noexcept
    {
        using namespace verb_detail;
        const Program& p = *prog;
        double rt = (double) p.rtMin * std::pow ((double) p.rtMax / (double) p.rtMin, decaySm);
        const double tilt = std::pow (4.0, toneSm - 0.5); // 0.5 .. 2

        if (p.type == fdn)
        {
            const double hf = std::clamp ((double) p.hfRatio * tilt, 0.05, 1.0);
            coefTarget[cDamp] = poleCoef ((double) p.dampHz * tilt, fs);
            for (int i = 0; i < 8; ++i)
            {
                const double e = -3.0 * loopLen[i] / fs;
                const double mid = std::pow (10.0, e / rt), high = std::pow (10.0, e / (rt * hf)), low = std::pow (10.0, e / (rt * (double) p.bassRatio));
                coefTarget[i] = (float) (0.35355339059327373 * high);
                coefTarget[8 + i] = (float) (0.35355339059327373 * (mid - high));
                coefTarget[16 + i] = (float) (0.35355339059327373 * (low - mid));
                coefTarget[24 + i] = (float) std::pow (10.0, -3.0 * (double) apLen[i] / (fs * rt)); // the all-pass loses in proportion to its length too
            }
            coefTarget[cEcho] = (float) std::min (0.85, std::pow (10.0, -3.0 * (double) echoTarget / (fs * rt)));
            coefTarget[cShift] = (float) (2.0 * (0.75 + 0.5 * decaySm) / std::sqrt (rt)); // about the same share of octave whatever the decay time
        }
        else if (p.type == springs)
        {
            for (int s = 0; s < numSprings; ++s)
                coefTarget[s] = (float) -std::pow (10.0, -3.0 * loopLen[s] / rt); // [0..2]: gain of each spring's loop
            for (int a = 0; a < 6; ++a)
                coefTarget[24 + a] = (float) std::pow (10.0, -3.0 * (double) apLen[a] * decim / (fs * rt));
            coefTarget[3] = (float) std::clamp ((double) p.hfRatio * tilt, 0.1, 0.95); // [3]: how much of the highs survives a round trip
        }
        else
        {
            if (activeCondition == 2)
                rt *= 1.5;
            for (int side = 0; side < 2; ++side)
                coefTarget[side] = (float) std::pow (10.0, -3.0 * loopLen[side] / rt); // [0..1]: gain of each feedback path
            for (int a = 0; a < 8; ++a)
                coefTarget[24 + a] = (float) std::pow (10.0, -3.0 * (double) apLen[a] / (fs * rt));
        }

        const double mid = (double) p.toneHz;
        const double hz = toneSm < 0.5 ? 900.0 * std::pow (mid / 900.0, 2.0 * toneSm) : mid * std::pow (18000.0 / mid, 2.0 * toneSm - 1.0);
        coefTarget[cTone] = poleCoef (hz, fs);
        outGainTarget = (double) p.level * std::pow (rt, -0.3); // longer decays build up more level: take some of it back
    }

    void controlTick() noexcept
    {
        using namespace verb_detail;

        // filter states that have died away become zero (a one-pole can stall on a denormal number)
        for (int i = 0; i < 8; ++i)
        {
            dampHi[i] = zap (dampHi[i]);
            dampLo[i] = zap (dampLo[i]);
        }
        for (int i = 0; i < 4; ++i)
            tone[i] = zap (tone[i]);
        for (int i = 0; i < 3; ++i)
            springLp[i] = zap (springLp[i]);
        for (int i = 0; i < 2; ++i)
        {
            lowState[i] = zap (lowState[i]);
            shiftPole[i] = zap (shiftPole[i]);
            loopHp[i] = zap (loopHp[i]);
        }
        echoLp = zap (echoLp);
        lowHp = zap (lowHp);
        duckAmount = zap (duckAmount);

        bool moving = false;
        if (decaySm != decayTarget)
        {
            decaySm += (decayTarget - decaySm) * smoothCoef;
            if (std::abs (decayTarget - decaySm) < 1.0e-4)
                decaySm = decayTarget;
            moving = true;
        }
        if (toneSm != toneTarget)
        {
            toneSm += (toneTarget - toneSm) * smoothCoef;
            if (std::abs (toneTarget - toneSm) < 1.0e-4)
                toneSm = toneTarget;
            moving = true;
        }
        if (activeCondition != condition || activeEcho != echoTarget)
        {
            activeCondition = condition;
            activeEcho = echoTarget; // Echo: the feedback follows the echo time
            moving = true;
        }
        if (moving)
        {
            // the loop gains glide to their new values over the next tick (no steps in the feedback)
            updateCoefficients();
            for (int c = 0; c < numCoefs; ++c)
                coefInc[c] = (coefTarget[c] - coef[c]) * (1.0f / tickSize);
            ramping = true;
        }
        else if (ramping)
        {
            for (int c = 0; c < numCoefs; ++c)
                coef[c] = coefTarget[c];
            ramping = false;
        }
        outGainInc = (outGainTarget - outGain) * (1.0 / tickSize);

        // pre-delay and echo time change by crossfading to a second tap
        if (preFade == 0 && preCur != preTarget)   { preNext = preTarget;   preFade = fadeLen; }
        if (echoFade == 0 && echoCur != echoTarget) { echoNext = echoTarget; echoFade = fadeLen; }

        // modulation: four sine LFOs, the delay times move in straight lines between the ticks
        depthSm += (depthTarget() - depthSm) * 0.02;
        for (int i = 0; i < 4; ++i)
        {
            modPhase[i] += modStep[i];
            if (modPhase[i] >= 2.0 * pi)
                modPhase[i] -= 2.0 * pi;
            modInc[i] = (modBase[i] + depthSm * std::sin (modPhase[i]) - modDelay[i]) * (1.0 / tickSize);
        }

        if (prog->type == particle)
        {
            // Hazard: the two shifters swing between +2 .. +12 and -12 .. +2 semitones
            hazardPhase[0] += 2.0 * pi * 0.13 * tickSize / fs;
            hazardPhase[1] += 2.0 * pi * 0.09 * tickSize / fs;
            for (int i = 0; i < 2; ++i)
            {
                if (hazardPhase[i] >= 2.0 * pi)
                    hazardPhase[i] -= 2.0 * pi;

                double target = 0.0;
                if (activeCondition == 1)
                    target = criticalRate (i);
                else if (activeCondition == 2)
                    target = (1.0 - std::pow (2.0, ((i == 0 ? 7.0 : -5.0) + (i == 0 ? 5.0 : 7.0) * std::sin (hazardPhase[i])) / 12.0)) / (double) shiftLen[i];

                shiftRate[i] += (target - shiftRate[i]) * 0.1;
                if (std::abs (target - shiftRate[i]) < 1.0e-12)
                    shiftRate[i] = target;
            }
        }
    }

    //==============================================================================
    /** Pre-delay (both sides) and the ducker's envelope follower. */
    void inputStage (const float* left, const float* right, int n) noexcept
    {
        using namespace verb_detail;
        float* const P = pool.data();
        const int bL = bs[dPreL], bR = bs[dPreR], m = mk[dPreL];

        for (int j = 0; j < n; ++j)
        {
            const int w = wp + j;
            const float inL = left[j], inR = right[j];
            P[bL + (w & m)] = inL;
            P[bR + (w & m)] = inR;

            float xl = P[bL + ((w - preCur) & m)], xr = P[bR + ((w - preCur) & m)];
            if (preFade > 0)
            {
                const float g = (float) preFade * fadeStep;
                const float nl = P[bL + ((w - preNext) & m)], nr = P[bR + ((w - preNext) & m)];
                xl = nl + (xl - nl) * g;
                xr = nr + (xr - nr) * g;
                if (--preFade == 0)
                    preCur = preNext;
            }
            sx[0][j] = xl;
            sx[1][j] = xr;

            if (duckOn)
            {
                const float level = 0.5f * (std::abs (inL) + std::abs (inR));
                duckEnv = zap (std::max (level, duckEnv * duckHold));
                const float target = std::min (1.0f, duckEnv * 16.0f); // fully ducked above -24 dBFS
                duckAmount += (target - duckAmount) * (target > duckAmount ? duckAttack : duckRelease);
                duckGain[j] = 1.0f - 0.87f * duckAmount; // down to -18 dB
            }
        }
    }

    /** Two-tap crossfading pitch shifter on its own delay line; the two windows always add up to one,
        so it never adds energy (it can sit inside a feedback loop). */
    float shift (int s, float x, int w) noexcept
    {
        using namespace verb_detail;
        float* const P = pool.data();
        const int b = bs[dShiftA + s], m = mk[dShiftA + s];
        P[b + (w & m)] = x;

        double ph = shiftPhase[s] + shiftRate[s];
        if (ph < 0.0)        ph += 1.0;
        else if (ph >= 1.0)  ph -= 1.0;
        shiftPhase[s] = ph;

        const double span = (double) shiftLen[s];
        const double d1 = 2.0 + span * ph, d2 = 2.0 + span * (ph < 0.5 ? ph + 0.5 : ph - 0.5);
        const int i1 = (int) d1, i2 = (int) d2;
        const float f1 = (float) (d1 - i1), f2 = (float) (d2 - i2);
        const float a0 = P[b + ((w - i1) & m)], a1 = P[b + ((w - i1 - 1) & m)];
        const float c0 = P[b + ((w - i2) & m)], c1 = P[b + ((w - i2 - 1) & m)];
        const float x1 = a0 + f1 * (a1 - a0), x2 = c0 + f2 * (c1 - c0);

        const float tri = (float) (1.0 - std::abs (2.0 * ph - 1.0));
        const float win = tri * tri * (3.0f - 2.0f * tri);
        return x2 + win * (x1 - x2);
    }

    //==============================================================================
    void tankStage (int n) noexcept
    {
        using namespace verb_detail;
        float* const P = pool.data();
        const int eM = mk[dEarlyL], cM = mk[dEchoL];
        float o[8];

        for (int j = 0; j < n; ++j)
        {
            const int w = wp + j;
            float xl = sx[0][j], xr = sx[1][j];
            if (ramping)
                for (int c = 0; c < cTone; ++c)
                    coef[c] += coefInc[c];

            lowState[0] += lowCoef * (xl - lowState[0]);
            lowState[1] += lowCoef * (xr - lowState[1]);
            xl -= lowState[0];
            xr -= lowState[1];

            // early reflections
            float el = 0.0f, er = 0.0f;
            if (numEarly > 0)
            {
                P[bs[dEarlyL] + (w & eM)] = xl;
                P[bs[dEarlyR] + (w & eM)] = xr;
                for (int k = 0; k < numEarly; ++k)
                {
                    el += earlyGain[k] * P[earlyBase[k] + ((w - earlyLen[k]) & eM)];
                    er += earlyGain[maxEarly + k] * P[earlyBase[maxEarly + k] + ((w - earlyLen[maxEarly + k]) & eM)];
                }
            }

            // Echo: ping-pong repeats (left first), heard directly and sent into the tank
            if (echoOn)
            {
                float a = P[bs[dEchoL] + ((w - echoCur) & cM)], b = P[bs[dEchoR] + ((w - echoCur) & cM)];
                if (echoFade > 0)
                {
                    const float g = (float) echoFade * fadeStep;
                    const float na = P[bs[dEchoL] + ((w - echoNext) & cM)], nb = P[bs[dEchoR] + ((w - echoNext) & cM)];
                    a = na + (a - na) * g;
                    b = nb + (b - nb) * g;
                    if (--echoFade == 0)
                        echoCur = echoNext;
                }
                echoLp += echoCoef * (b - echoLp);
                P[bs[dEchoL] + (w & cM)] = zap (0.5f * (xl + xr) + coef[cEcho] * echoLp);
                P[bs[dEchoR] + (w & cM)] = zap (coef[cEcho] * a);
                el = 0.8f * a;
                er = 0.8f * b;
                xl += 0.7f * a;
                xr += 0.7f * b;
            }

            // input diffusers
            for (int k = 0; k < numDiff; ++k)
            {
                const int dl = dDiff + k, dr = dDiff + 4 + k;
                const float zl = P[bs[dl] + ((w - diffLen[k]) & mk[dl])], zr = P[bs[dr] + ((w - diffLen[4 + k]) & mk[dr])];
                const float vl = xl - diffGain * zl, vr = xr - diffGain * zr;
                P[bs[dl] + (w & mk[dl])] = zap (vl);
                P[bs[dr] + (w & mk[dr])] = zap (vr);
                xl = zl + diffGain * vl;
                xr = zr + diffGain * vr;
            }

            // the eight lines: four with a slowly moving length, read with third-order Lagrange interpolation
            // (between its two middle points it never has a gain above one, and it keeps the highs)
            for (int i = 0; i < 4; ++i)
            {
                const double d = modDelay[i];
                modDelay[i] = d + modInc[i];
                const int di = (int) d;
                const float x = (float) (d - di), xp = x + 1.0f, xm = x - 1.0f, xn = x - 2.0f;
                const int b = bs[dLine + i], m = mk[dLine + i];
                o[i] = -x * xm * xn * (1.0f / 6.0f) * P[b + ((w - di + 1) & m)] + xp * xm * xn * 0.5f * P[b + ((w - di) & m)]
                     - xp * x * xn * 0.5f * P[b + ((w - di - 1) & m)] + xp * x * xm * (1.0f / 6.0f) * P[b + ((w - di - 2) & m)];
            }
            for (int i = 4; i < 8; ++i)
                o[i] = P[bs[dLine + i] + ((w - lineLen[i]) & mk[dLine + i])];

            // decay per band: high, mid and low frequencies lose a different amount per trip
            for (int i = 0; i < 8; ++i)
            {
                const float x = o[i];
                dampHi[i] += coef[cDamp] * (x - dampHi[i]);
                dampLo[i] += loCoef * (x - dampLo[i]);
                o[i] = coef[i] * x + coef[8 + i] * dampHi[i] + coef[16 + i] * dampLo[i];
            }

            // Octo: what comes out of two lines is band-limited, shifted up an octave and fed in again (crosswise),
            // so an octave layer grows on top of every layer. The low-pass ends the climb; the high-pass keeps
            // out what is too slow for the shifter to move (that would be a plain feedback loop)
            if (shiftOn)
            {
                shiftPole[0] += shiftCoef * (o[6] - shiftPole[0]);
                shiftPole[1] += shiftCoef * (o[7] - shiftPole[1]);
                const float upL = shift (0, shiftHp[0].process (shiftLp[0].process (shiftPole[0])), w);
                const float upR = shift (1, shiftHp[1].process (shiftLp[1].process (shiftPole[1])), w);
                xl += coef[cShift] * upR;
                xr += coef[cShift] * upL;
            }

            // Hadamard matrix (lossless; its 1 / sqrt (8) is part of the gains)
            const float a0 = o[0] + o[1], a1 = o[0] - o[1], a2 = o[2] + o[3], a3 = o[2] - o[3];
            const float a4 = o[4] + o[5], a5 = o[4] - o[5], a6 = o[6] + o[7], a7 = o[6] - o[7];
            const float b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
            const float b4 = a4 + a6, b5 = a5 + a7, b6 = a4 - a6, b7 = a5 - a7;
            o[0] = b0 + b4 + xl;  o[1] = b1 + b5 + xr;  o[2] = b2 + b6 - xl;  o[3] = b3 + b7 - xr;
            o[4] = b0 - b4 + xl;  o[5] = b1 - b5 + xr;  o[6] = b2 - b6 - xl;  o[7] = b3 - b7 - xr;

            // an all-pass in front of every line, then into the line
            for (int i = 0; i < 8; ++i)
            {
                const int da = dAp + i, dl = dLine + i;
                const float z = coef[24 + i] * P[bs[da] + ((w - apLen[i]) & mk[da])];
                const float v = o[i] - apGain * z;
                P[bs[da] + (w & mk[da])] = zap (v);
                P[bs[dl] + (w & mk[dl])] = zap (z + apGain * v);
            }

            const float tl = P[bs[dLine]     + ((w - tapLen[0]) & mk[dLine])]     - P[bs[dLine + 2] + ((w - tapLen[2]) & mk[dLine + 2])]
                           + P[bs[dLine + 4] + ((w - tapLen[4]) & mk[dLine + 4])] - P[bs[dLine + 6] + ((w - tapLen[6]) & mk[dLine + 6])];
            const float tr = P[bs[dLine + 1] + ((w - tapLen[1]) & mk[dLine + 1])] - P[bs[dLine + 3] + ((w - tapLen[3]) & mk[dLine + 3])]
                           + P[bs[dLine + 5] + ((w - tapLen[5]) & mk[dLine + 5])] - P[bs[dLine + 7] + ((w - tapLen[7]) & mk[dLine + 7])];

            sy[0][j] = el + tankLevel * tl;
            sy[1][j] = er + tankLevel * tr;
        }
    }

    //==============================================================================
    /** One step of the springs, at the low rate. Each spring: delay and all-pass chain out to the far end
        (the output), delay and chain back, loss, and into the spring (and its neighbours) again. */
    void springStep (float x) noexcept
    {
        using namespace verb_detail;
        float* const P = pool.data();
        const int w = wpLow;
        wpLow = (wpLow + 1) & 0x3fffffff;

        lowHp += lowCoef * (x - lowHp);
        x -= lowHp;

        float back[3] = { 0.0f, 0.0f, 0.0f }, outL = 0.0f, outR = 0.0f;
        for (int s = 0; s < numSprings; ++s)
        {
            const int da = dLine + 2 * s, db = da + 1;
            float* c = chain.data() + 2 * s * maxStages;

            P[bs[da] + (w & mk[da])] = zap (x * springIn[s] + springFb[s]);
            float t = P[bs[da] + ((w - springLen[s]) & mk[da])];
            for (int k = 0; k < stages; ++k)
            {
                const float y = springCoef * t + c[k];
                c[k] = t - springCoef * y;
                t = y;
            }
            outL += springOutL[s] * t;
            outR += springOutR[s] * t;

            // on the way back: the same again, with an all-pass before and after (the reflections inside
            // a real spring, which blur the repeats a little more on every trip)
            for (int half = 0; half < 2; ++half)
            {
                const int a = 2 * s + half, d = dAp + a;
                const float z = coef[24 + a] * P[bs[d] + ((w - apLen[a]) & mk[d])];
                const float v = t - 0.5f * z;
                P[bs[d] + (w & mk[d])] = zap (v);
                t = z + 0.5f * v;
                if (half == 1)
                    break;

                P[bs[db] + (w & mk[db])] = t;
                t = P[bs[db] + ((w - springLen[s]) & mk[db])];
                c += maxStages;
                for (int k = 0; k < stages; ++k)
                {
                    const float y = springCoef * t + c[k];
                    c[k] = t - springCoef * y;
                    t = y;
                }
            }
            springLp[s] += loCoef * (t - springLp[s]);
            back[s] = springLp[s] + coef[3] * (t - springLp[s]);
        }

        if (numSprings == 3)
        {
            const float m = (back[0] + back[1] + back[2]) * (2.0f / 3.0f); // Householder matrix: the springs feed each other
            for (int s = 0; s < 3; ++s)
                springFb[s] = coef[s] * (back[s] - m);
        }
        else
        {
            springFb[0] = coef[0] * (0.8f * back[0] + 0.6f * back[1]); // a rotation: the two springs share their mounts
            springFb[1] = coef[1] * (0.8f * back[1] - 0.6f * back[0]);
        }

        lowPrev[0] = lowCur[0];
        lowPrev[1] = lowCur[1];
        lowCur[0] = outL;
        lowCur[1] = outR;
    }

    void springStage (int n) noexcept
    {
        const float decStep = 1.0f / (float) decim;
        for (int j = 0; j < n; ++j)
        {
            if (ramping)
            {
                for (int c = 0; c < 4; ++c)
                    coef[c] += coefInc[c];
                for (int c = 24; c < 32; ++c)
                    coef[c] += coefInc[c];
            }

            float m = 0.5f * (sx[0][j] + sx[1][j]);
            if (drive > 0.0f)
                m = (std::tanh (drive * m + 0.25f) - driveBias) * driveNorm; // the tube that drives the tank
            m = aaIn[2].process (aaIn[1].process (aaIn[0].process (m)));

            if (++decPhase >= decim)
            {
                decPhase = 0;
                springStep (m);
            }

            const float fr = (float) (decPhase + 1) * decStep;
            sy[0][j] = aaOut[2].process (aaOut[1].process (aaOut[0].process (lowPrev[0] + (lowCur[0] - lowPrev[0]) * fr)));
            sy[1][j] = aaOut[5].process (aaOut[4].process (aaOut[3].process (lowPrev[1] + (lowCur[1] - lowPrev[1]) * fr)));
        }
    }

    //==============================================================================
    void particleStage (int n) noexcept
    {
        using namespace verb_detail;
        float* const P = pool.data();
        const float g = prog->apGain;

        for (int j = 0; j < n; ++j)
        {
            const int w = wp + j;
            const float gain = driveGain.next();
            if (ramping)
            {
                coef[0] += coefInc[0];
                coef[1] += coefInc[1];
                for (int c = 24; c < 32; ++c)
                    coef[c] += coefInc[c];
            }

            lowState[0] += lowCoef * (sx[0][j] - lowState[0]);
            lowState[1] += lowCoef * (sx[1][j] - lowState[1]);
            float in[2] = { (sx[0][j] - lowState[0]) * gain, (sx[1][j] - lowState[1]) * gain }, early[2] = { 0.0f, 0.0f }, late[2] = { 0.0f, 0.0f };

            // input diffusers: a dense burst instead of a click, before the long all-passes smear it further
            for (int k = 0; k < numDiff; ++k)
                for (int side = 0; side < 2; ++side)
                {
                    const int d = dDiff + side * 4 + k;
                    const float z = P[bs[d] + ((w - diffLen[side * 4 + k]) & mk[d])];
                    const float v = in[side] - diffGain * z;
                    P[bs[d] + (w & mk[d])] = zap (v);
                    in[side] = z + diffGain * v;
                }

            for (int side = 0; side < 2; ++side)
            {
                // the input plus what comes back from the other side, soft-limited (this keeps Hazard bounded)
                float s = std::clamp (in[side] + loopBack[side], -3.0f, 3.0f);
                s -= s * s * s * (1.0f / 27.0f);

                for (int k = 0; k < 4; ++k)
                {
                    const int a = side * 4 + k, b = bs[dAp + a], m = mk[dAp + a];
                    float z;
                    if (k < 2)
                    {
                        const int mi = side * 2 + k;
                        const double d = modDelay[mi];
                        modDelay[mi] = d + modInc[mi];
                        const int di = (int) d;
                        const float fr = (float) (d - di);
                        const float z0 = P[b + ((w - di) & m)], z1 = P[b + ((w - di - 1) & m)];
                        z = z0 + fr * (z1 - z0);
                    }
                    else
                    {
                        z = P[b + ((w - apLen[a]) & m)];
                    }
                    z *= coef[24 + a];

                    const float v = s - g * z;
                    P[b + (w & m)] = zap (v);
                    s = z + g * v;
                    if (k == 1)
                        early[side] = s;
                }
                late[side] = s;

                // round to the other side: delay, low-pass, pitch shifter, low cut, loss
                const int dl = dLine + side;
                P[bs[dl] + (w & mk[dl])] = s;
                float t = shift (side, shiftLp[side].process (P[bs[dl] + ((w - lineLen[side]) & mk[dl])]), w);
                loopHp[side] += lowCoef * (t - loopHp[side]);
                t -= loopHp[side];
                loopBack[side ^ 1] = zap (t * coef[side]);
            }

            sy[0][j] = late[0] - 0.6f * early[1];
            sy[1][j] = late[1] + 0.6f * early[0];
        }
    }

    //==============================================================================
    /** Voicing EQ, the Tone low-pass (12 dB / octave), level and ducking. */
    void outputStage (float* left, float* right, int n) noexcept
    {
        for (int j = 0; j < n; ++j)
        {
            float yl = sy[0][j], yr = sy[1][j];
            if (voiceOn)
            {
                yl = voice[0].process (yl);
                yr = voice[1].process (yr);
            }

            if (ramping)
                coef[cTone] += coefInc[cTone];
            const float toneCoef = coef[cTone];
            tone[0] += toneCoef * (yl - tone[0]);
            tone[1] += toneCoef * (tone[0] - tone[1]);
            tone[2] += toneCoef * (yr - tone[2]);
            tone[3] += toneCoef * (tone[2] - tone[3]);

            outGain += outGainInc;
            const float g = duckOn ? (float) outGain * duckGain[j] : (float) outGain;
            left[j] = tone[1] * g;
            right[j] = tone[3] * g;
        }
    }

    //==============================================================================
    double fs = 48000.0;
    int variant = plate, activeVariant = plate;
    const Program* prog = &verb_detail::programs[0];

    // knobs
    double decayTarget = 0.5, toneTarget = 0.5, decaySm = 0.5, toneSm = 0.5, smoothCoef = 0.02;
    int preTarget = 0, echoTarget = 4800, condition = 0, activeCondition = 0, activeEcho = 4800;
    float mix = 0.3f;
    Smoothed driveGain { 1.0f };

    // the delay memory
    std::vector<float> pool, chain;
    int bs[verb_detail::numDelays] {}, mk[verb_detail::numDelays] {};
    int wp = 0, wpLow = 0, tickPos = 0;
    float sx[2][verb_detail::tickSize] {}, sy[2][verb_detail::tickSize] {}, duckGain[verb_detail::tickSize] {};

    // pre-delay, echo
    int preCur = 0, preNext = 0, preFade = 0, echoCur = 4800, echoNext = 4800, echoFade = 0, fadeLen = 1920;
    float fadeStep = 0.0f, echoCoef = 0.0f, echoLp = 0.0f;
    bool echoOn = false, duckOn = false, shiftOn = false, voiceOn = false;

    // feedback delay network
    int numEarly = 0, numDiff = 0;
    int lineLen[8] {}, apLen[8] {}, tapLen[8] {}, diffLen[8] {};
    int earlyLen[2 * verb_detail::maxEarly] {}, earlyBase[2 * verb_detail::maxEarly] {};
    float earlyGain[2 * verb_detail::maxEarly] {};
    double loopLen[8] {};
    // everything in the signal path that follows Decay and Tone, ramped from tick to tick.
    // [0..31] per line: gain of the highs, mids - highs, lows - mids, loss of the all-pass
    // (springs and Particle Verb keep their loop gains in [0..3] and the losses of their all-passes in [24..31])
    enum { cDamp = 32, cShift, cEcho, cTone, numCoefs };
    float coef[numCoefs] {}, coefTarget[numCoefs] {}, coefInc[numCoefs] {};
    bool ramping = false;
    float dampHi[8] {}, dampLo[8] {};
    float loCoef = 0.0f, lowCoef = 0.0f, lowState[2] {};
    float diffGain = 0.7f, apGain = 0.5f, tankLevel = 1.0f;

    // modulation
    double modPhase[4] {}, modStep[4] {}, modBase[4] {}, modDelay[4] {}, modInc[4] {}, depthSm = 0.0;

    // pitch shifters (Octo, Particle Verb)
    double shiftPhase[2] {}, shiftRate[2] {}, hazardPhase[2] {};
    int shiftLen[2] {};
    float shiftCoef = 0.5f, shiftPole[2] {};
    Biquad shiftLp[2], shiftHp[2];

    // ducker
    float duckEnv = 0.0f, duckAmount = 0.0f, duckHold = 0.0f, duckAttack = 0.0f, duckRelease = 0.0f;

    // springs
    int decim = 5, decPhase = 0, numSprings = 2, stages = 32, springLen[3] {};
    float springCoef = 0.6f, drive = 0.0f, driveBias = 0.0f, driveNorm = 1.0f, lowHp = 0.0f;
    float springFb[3] {}, springLp[3] {}, lowPrev[2] {}, lowCur[2] {};
    static constexpr float springIn[3] = { 1.0f, -0.9f, 0.8f };
    float springOutL[3] {}, springOutR[3] {};
    Biquad aaIn[3], aaOut[6]; // band limits on the way down to the low rate and back up (left, right)

    // Particle Verb
    float loopBack[2] {}, loopHp[2] {};

    // output
    Biquad voice[2];
    float tone[4] {};
    double outGain = 1.0, outGainInc = 0.0, outGainTarget = 1.0;
};

/** In the order of VerbFx::Variant. */
inline std::vector<ModelInfo> verbModels()
{
    auto verb = [] (const char* key, const char* name, int variant, const char* basedOn, float decay, float preDelay, float mix)
    {
        ModelInfo m { key, name, Category::reverb, Engine::reverbFx, variant, basedOn,
                      { percent ("Decay", decay), millis ("PreDelay", 0.0f, 200.0f, preDelay, 50.0f), percent ("Tone", 50.0f), percent ("Mix", mix) } };
        m.trails = true;
        return m;
    };

    std::vector<ModelInfo> models {
        verb ("plate",     "Plate",      VerbFx::plate,    "Studio plate reverb",                          50.0f, 10.0f, 30.0f),
        verb ("room",      "Room",       VerbFx::room,     "Studio room, mostly early reflections",        45.0f,  5.0f, 35.0f),
        verb ("chamber",   "Chamber",    VerbFx::chamber,  "Elongated echo chamber (hallway, stairwell)",  50.0f, 15.0f, 30.0f),
        verb ("hall",      "Hall",       VerbFx::hall,     "Concert hall",                                 50.0f, 30.0f, 30.0f),
        verb ("echo_verb", "Echo",       VerbFx::echo,     "Line 6 original: echoes feeding a lush reverb", 50.0f, 90.0f, 30.0f),
        verb ("tile",      "Tile",       VerbFx::tile,     "Tiled room (bathroom, shower)",                45.0f,  5.0f, 30.0f),
        verb ("cave",      "Cave",       VerbFx::cave,     "Line 6 original: cavernous echo chamber",      45.0f, 50.0f, 30.0f),
        verb ("ducking",   "Ducking",    VerbFx::ducking,  "Hall with ducking",                            55.0f, 30.0f, 40.0f),
        verb ("octo",      "Octo",       VerbFx::octo,     "Line 6 original: octave-harmonised decay",     60.0f, 40.0f, 35.0f),
        verb ("spring",    "Spring",     VerbFx::spring,   "Studio spring reverb",                         50.0f,  0.0f, 30.0f),
        verb ("spring_63", "'63 Spring", VerbFx::spring63, "1963 Fender tube spring reverb unit (6G15)",   55.0f,  0.0f, 35.0f),
    };

    ModelInfo particle { "particle_verb", "Particle Verb", Category::reverb, Engine::reverbFx, VerbFx::particleVerb,
                         "Line 6 original: modulated pad reverb",
                         { percent ("Dwell", 55.0f), choice ("Condition", verbConditionNames, 3, 0), percent ("Gain", 50.0f), percent ("Mix", 40.0f) } };
    particle.trails = true;
    models.push_back (particle);
    return models;
}

} // namespace fx
