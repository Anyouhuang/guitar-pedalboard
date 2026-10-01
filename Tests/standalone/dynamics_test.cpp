// Standalone test of the Dynamics engine (see Harness.h): the harness run, then checks of what each model is
// supposed to do - static compression curves, attack and release times, make-up gain, the gate's thresholds.
#include "Harness.h"
#include "../../Source/DSP/fx/Dynamics.h"

namespace
{
using Fx = fx::DynamicsFx;
double sampleRate = harness::testRate; // (the checks at other rates change it for a while)
int testFailures = 0;

struct AtRate
{
    explicit AtRate (double rate) : previous (sampleRate) { sampleRate = rate; }
    ~AtRate() { sampleRate = previous; }
    double previous;
};

void check (bool ok, const char* what, double value)
{
    std::printf ("    %-4s %s (%.2f)\n", ok ? "ok" : "FAIL", what, value);
    if (! ok)
        ++testFailures;
}

float db (float gain) { return harness::toDb (gain); }

/** Mono `input` through one model, the knobs set before every block like a slot does. Returns the left output. */
std::vector<float> run (int variant, std::vector<float> knobs, const std::vector<float>& input)
{
    knobs.resize (8, 0.0f);
    Fx effect;
    effect.prepare (sampleRate, harness::blockSize);
    effect.setModel (variant);
    effect.setParameters (knobs.data());
    effect.reset();

    std::vector<float> out (input.size());
    float left[harness::blockSize], right[harness::blockSize];
    for (size_t pos = 0; pos < input.size(); pos += harness::blockSize)
    {
        const int n = (int) std::min<size_t> (harness::blockSize, input.size() - pos);
        effect.setParameters (knobs.data());
        std::copy_n (input.begin() + (std::ptrdiff_t) pos, n, left);
        std::copy_n (input.begin() + (std::ptrdiff_t) pos, n, right);
        effect.process (left, right, n);
        std::copy_n (left, n, out.begin() + (std::ptrdiff_t) pos);
    }
    return out;
}

/** A sine whose level (dBFS peak) changes at the given times: { { seconds, dB }, ... }, the last entry ends it. */
std::vector<float> steppedSine (double hz, const std::vector<std::pair<double, float>>& steps)
{
    std::vector<float> x ((size_t) (steps.back().first * sampleRate));
    size_t step = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        while (step + 1 < steps.size() && (double) i / sampleRate >= steps[step + 1].first)
            ++step;
        x[i] = fx::dbToGain (steps[step].second) * (float) std::sin (2.0 * fx::pi * hz * (double) i / sampleRate);
    }
    return x;
}

float rmsBetween (const std::vector<float>& x, double from, double to)
{
    double sum = 0.0;
    const size_t a = (size_t) (from * sampleRate), b = std::min (x.size(), (size_t) (to * sampleRate));
    for (size_t i = a; i < b; ++i)
        sum += (double) x[i] * x[i];
    return (float) std::sqrt (sum / (double) std::max<size_t> (1, b - a));
}

/** Output level (dB, as the peak of an equally loud sine) once a steady sine of `inDb` has settled. */
float staticOut (int variant, const std::vector<float>& knobs, float inDb, double hz = 1000.0, double seconds = 1.5)
{
    const auto out = run (variant, knobs, steppedSine (hz, { { 0.0, inDb }, { seconds, 0.0f } }));
    return db (rmsBetween (out, seconds - 0.2, seconds) * 1.41421356f);
}

float staticGain (int variant, const std::vector<float>& knobs, float inDb, double hz = 1000.0)
{
    return staticOut (variant, knobs, inDb, hz) - inDb;
}

/** Gain (dB) in 1 ms steps: output over input, for a 1 kHz stepped sine. */
std::vector<float> gainTrace (const std::vector<float>& in, const std::vector<float>& out)
{
    const int window = (int) (sampleRate / 1000.0);
    std::vector<float> trace;
    for (size_t pos = 0; pos + (size_t) window <= in.size(); pos += (size_t) window)
    {
        double si = 0.0, so = 0.0;
        for (int i = 0; i < window; ++i)
        {
            si += (double) in[pos + (size_t) i] * in[pos + (size_t) i];
            so += (double) out[pos + (size_t) i] * out[pos + (size_t) i];
        }
        trace.push_back (db ((float) std::sqrt (so / std::max (1.0e-30, si))));
    }
    return trace;
}

/** Milliseconds after `fromMs` until the gain first comes within `tolerance` dB of `target`. */
double msUntilWithin (const std::vector<float>& trace, double fromMs, float target, float tolerance)
{
    for (size_t i = (size_t) fromMs; i < trace.size(); ++i)
        if (std::abs (trace[i] - target) <= tolerance)
            return (double) i - fromMs;
    return 1.0e9;
}

struct StepResult { double attackMs, releaseMs; float reductionDb, afterRelease150; };

/** Quiet (lowDb) for 0.5 s, loud (highDb) for `loudSeconds`, quiet again for 5 s. */
StepResult stepResponse (int variant, const std::vector<float>& knobs, float lowDb, float highDb, double loudSeconds, float tolerance = 2.0f)
{
    const double up = 0.5, down = up + loudSeconds, end = down + 5.0;
    const auto in = steppedSine (1000.0, { { 0.0, lowDb }, { up, highDb }, { down, lowDb }, { end, 0.0f } });
    const auto trace = gainTrace (in, run (variant, knobs, in));

    const float quietGain = trace[(size_t) (up * 1000.0) - 5];
    const float loudGain  = trace[(size_t) (down * 1000.0) - 5];
    return { msUntilWithin (trace, up * 1000.0, loudGain, tolerance),
             msUntilWithin (trace, down * 1000.0, trace.back(), tolerance),
             quietGain - loudGain,
             trace.back() - trace[(size_t) (down * 1000.0) + 150] };
}

/** From a gain trace whose input drops below the close threshold at 1.0 s: ms until the gain starts to fall
    (-1 dB) and until it is down to -59 dB. */
std::pair<double, double> gateTiming (const std::vector<float>& trace)
{
    double holdMs = 0.0, closedMs = 0.0;
    for (size_t i = 1000; i < trace.size() && trace[i] > -1.0f; ++i)  holdMs = (double) i - 999.0;
    for (size_t i = 1000; i < trace.size() && trace[i] > -59.0f; ++i) closedMs = (double) i - 999.0;
    return { holdMs, closedMs };
}

//==============================================================================
void testHardGate()
{
    std::printf ("  Hard Gate: open -30 dB, close -50 dB, hold 100 ms, decay 200 ms\n");
    const std::vector<float> knobs { -30.0f, -50.0f, 100.0f, 200.0f };

    // between the thresholds (stays shut) -> loud (opens) -> between the thresholds (stays open) -> below close
    const auto in = steppedSine (1000.0, { { 0.0, -40.0f }, { 0.3, -20.0f }, { 0.6, -40.0f }, { 1.0, -60.0f }, { 2.0, 0.0f } });
    const auto out = run (Fx::hardGate, knobs, in);
    const auto trace = gainTrace (in, out);

    check (rmsBetween (out, 0.0, 0.3) == 0.0f, "shut below the open threshold: output is digital silence, rms", rmsBetween (out, 0.0, 0.3));
    check (msUntilWithin (trace, 300.0, 0.0f, 0.2f) <= 3.0, "opens within 3 ms of crossing the open threshold, ms", msUntilWithin (trace, 300.0, 0.0f, 0.2f));
    check (std::abs (trace[590]) < 0.05f, "open: unity gain, dB", trace[590]);
    check (std::abs (trace[990]) < 0.05f, "stays open between the two thresholds, dB", trace[990]);

    const auto [holdMs, closedMs] = gateTiming (trace);
    check (holdMs >= 100.0 && holdMs <= 125.0, "hold before the gain starts to fall, ms (knob 100 + detector)", holdMs);
    check (std::abs ((closedMs - holdMs) - 200.0 * 58.0 / 60.0) < 12.0, "decay from -1 to -59 dB, ms (knob 200 for 60 dB)", closedMs - holdMs);
    check (rmsBetween (out, 1.5, 2.0) == 0.0f, "then fully shut, rms", rmsBetween (out, 1.5, 2.0));

    // exact thresholds: a sine 1 dB under / over each
    auto opens = [&] (float levelDb)
    {
        const auto o = run (Fx::hardGate, knobs, steppedSine (1000.0, { { 0.0, levelDb }, { 0.5, 0.0f } }));
        return rmsBetween (o, 0.3, 0.5) > 0.0f;
    };
    auto staysOpen = [&] (float levelDb)
    {
        const auto o = run (Fx::hardGate, knobs, steppedSine (1000.0, { { 0.0, -10.0f }, { 0.2, levelDb }, { 1.2, 0.0f } }));
        return rmsBetween (o, 1.0, 1.2) > 0.0f;
    };
    check (! opens (-31.0f) && opens (-29.0f), "open threshold: -31 dB stays shut, -29 dB opens", -30.0);
    check (staysOpen (-49.0f) && ! staysOpen (-51.0f), "close threshold: -49 dB stays open, -51 dB shuts", -50.0);

    // Hold at 0 / Decay at 1 ms: the "lopped-off" setting still must not leave a step in the output
    const auto chop = run (Fx::hardGate, { -30.0f, -30.0f, 0.0f, 1.0f }, steppedSine (220.0, { { 0.0, -10.0f }, { 0.3, -60.0f }, { 0.6, 0.0f } }));
    float biggestStep = 0.0f;
    for (size_t i = (size_t) (0.29 * sampleRate); i + 1 < chop.size(); ++i)
        biggestStep = std::max (biggestStep, std::abs (chop[i + 1] - chop[i]));
    check (biggestStep < 0.02f, "fastest closing has no step bigger than the sine's own slope (0.009), max step", biggestStep);
}

//==============================================================================
void testTubeComp()
{
    std::printf ("  Tube Comp (LA-2A): static curve at Threshold -20 dB, Level 0\n");
    const std::vector<float> knobs { -20.0f, 0.0f };
    const float makeupDb = staticGain (Fx::tubeComp, knobs, -70.0f);
    std::printf ("    in dB / out dB / reduction:");
    for (float inDb : { -50.0f, -40.0f, -30.0f, -20.0f, -10.0f, 0.0f })
    {
        const float outDb = staticOut (Fx::tubeComp, knobs, inDb);
        std::printf ("  %.0f / %.1f / %.1f", inDb, outDb, inDb + makeupDb - outDb);
    }
    std::printf ("\n");

    const float atThreshold = -20.0f + makeupDb - staticOut (Fx::tubeComp, knobs, -20.0f);
    check (std::abs (makeupDb + staticGain (Fx::tubeComp, knobs, -50.0f) * -1.0f) < 0.3f, "no reduction 30 dB below the threshold, dB",
           makeupDb - staticGain (Fx::tubeComp, knobs, -50.0f));
    check (atThreshold > 2.3f && atThreshold < 4.3f, "soft knee: about 3.3 dB of reduction at the threshold, dB", atThreshold);

    const std::vector<float> low { -40.0f, 0.0f };
    const float kneeSlope = (staticOut (Fx::tubeComp, low, -30.0f) - staticOut (Fx::tubeComp, low, -40.0f)) / 10.0f;
    const float farSlope  = (staticOut (Fx::tubeComp, low, 0.0f) - staticOut (Fx::tubeComp, low, -10.0f)) / 10.0f;
    check (kneeSlope > 0.45f && kneeSlope < 0.75f, "knee: slope over the first 10 dB above the threshold", kneeSlope);
    check (farSlope > 0.30f && farSlope < 0.40f, "far above the threshold the slope nears 1/3 (3:1)", farSlope);

    std::printf ("    make-up follows the threshold (input at the -16.5 dB reference, and a quiet -60 dB input):\n");
    float previousQuiet = -100.0f;
    bool unityAtReference = true, makeupGrows = true;
    for (float threshold : { 0.0f, -10.0f, -20.0f, -30.0f, -40.0f })
    {
        const float atReference = staticGain (Fx::tubeComp, { threshold, 0.0f }, -16.478f);
        const float quiet = staticGain (Fx::tubeComp, { threshold, 0.0f }, -60.0f);
        std::printf ("      threshold %5.0f: gain at reference %+.2f dB, make-up %+.2f dB\n", threshold, atReference, quiet);
        unityAtReference = unityAtReference && std::abs (atReference) < 0.6f;
        makeupGrows = makeupGrows && quiet > previousQuiet + 1.0f;
        previousQuiet = quiet;
    }
    check (unityAtReference, "a signal at the reference level keeps its level at every threshold", 0.0);
    check (makeupGrows && previousQuiet > 14.7f && previousQuiet < 17.7f, "make-up grows as the threshold falls, at -40 dB", previousQuiet);

    const std::vector<float> deep { -30.0f, 0.0f };
    const auto brief = stepResponse (Fx::tubeComp, deep, -50.0f, -6.0f, 0.05, 1.0f);
    const auto held  = stepResponse (Fx::tubeComp, deep, -50.0f, -6.0f, 5.0, 1.0f);
    std::printf ("    step -50 -> -6 dB: reduction %.1f dB, attack %.0f ms; release after 50 ms burst %.0f ms, after 5 s %.0f ms\n",
                 held.reductionDb, held.attackMs, brief.releaseMs, held.releaseMs);
    check (held.attackMs >= 4.0 && held.attackMs <= 60.0, "attack (to within 1 dB): the T4 cell's ~10 ms", held.attackMs);
    check (brief.releaseMs < 250.0, "after a short peak the release is all fast stage, ms", brief.releaseMs);
    check (held.releaseMs > 1500.0, "after 5 s of compression the release takes seconds, ms", held.releaseMs);
    const float fastShare = 1.0f - held.afterRelease150 / held.reductionDb;
    check (fastShare > 0.30f && fastShare < 0.70f, "...yet roughly half of the reduction is gone within 150 ms, share", fastShare);
}

//==============================================================================
void testRedComp()
{
    std::printf ("  Red Comp (Dyna Comp): static curve at Sustain 50, Level 0\n");
    const std::vector<float> knobs { 50.0f, 0.0f };
    std::printf ("    in dB / out dB:");
    for (float inDb : { -60.0f, -50.0f, -40.0f, -30.0f, -20.0f, -10.0f, 0.0f })
        std::printf ("  %.0f / %.1f", inDb, staticOut (Fx::redComp, knobs, inDb, 500.0));
    std::printf ("\n");

    const float quietGain = staticGain (Fx::redComp, knobs, -70.0f, 200.0) - db (fx::dyn_detail::redLevel);
    check (std::abs (quietGain - 17.0f) < 0.5f, "below the threshold the gain is the Sustain gain (17 dB at 50 %), dB", quietGain);
    const float range = staticOut (Fx::redComp, knobs, -5.0f, 500.0) - staticOut (Fx::redComp, knobs, -25.0f, 500.0);
    check (range < 3.5f, "squash: 20 dB more input gives under 3.5 dB more output (harder than 6:1), dB", range);
    const float sustainSpan = staticGain (Fx::redComp, { 100.0f, 0.0f }, -80.0f, 200.0) - staticGain (Fx::redComp, { 0.0f, 0.0f }, -80.0f, 200.0);
    check (std::abs (sustainSpan - 34.0f) < 0.5f, "Sustain spans 34 dB of gain for quiet notes, dB", sustainSpan);
    const float loudSpan = staticOut (Fx::redComp, { 100.0f, 0.0f }, -10.0f, 500.0) - staticOut (Fx::redComp, { 20.0f, 0.0f }, -10.0f, 500.0);
    check (std::abs (loudSpan) < 3.0f, "...but a loud note comes out at the same level whatever the Sustain, dB", loudSpan);

    const float tilt = staticGain (Fx::redComp, knobs, -70.0f, 8000.0) - staticGain (Fx::redComp, knobs, -70.0f, 500.0);
    check (tilt < -3.0f && tilt > -7.0f, "rolled-off top: 8 kHz vs 500 Hz, dB", tilt);

    const auto step = stepResponse (Fx::redComp, knobs, -50.0f, -6.0f, 1.0);
    std::printf ("    step -50 -> -6 dB: reduction %.1f dB, attack %.0f ms, release %.0f ms\n", step.reductionDb, step.attackMs, step.releaseMs);
    check (step.attackMs <= 5.0, "fast attack (within 2 dB), ms", step.attackMs);
    check (step.releaseMs > 500.0 && step.releaseMs < 2500.0, "long release (within 2 dB), ms", step.releaseMs);
}

/** A 20 ms peak followed by a note 20 dB quieter: how far below its final level is the note 0.3 s later? */
float duckingAfterPeak (int variant, const std::vector<float>& knobs)
{
    const auto in = steppedSine (1000.0, { { 0.0, -80.0f }, { 0.2, -6.0f }, { 0.22, -26.0f }, { 4.0, 0.0f } });
    const auto trace = gainTrace (in, run (variant, knobs, in));
    return trace[3900] - trace[520];
}

void testBloom()
{
    std::printf ("  Ducking 0.3 s after a pick attack (the Dyna Comp's dull-then-bloom):\n");
    const float red = duckingAfterPeak (Fx::redComp, { 50.0f, 0.0f });
    const float blue = duckingAfterPeak (Fx::blueComp, { 50.0f, 0.0f });
    const float tube = duckingAfterPeak (Fx::tubeComp, { -30.0f, 0.0f });
    const float vetta = duckingAfterPeak (Fx::vettaComp, { 70.0f, 0.0f });
    check (red > 6.0f, "Red Comp still holds the note down, dB", red);
    check (blue < 2.5f, "Blue Comp has let go, dB", blue);
    check (tube < 2.5f, "Tube Comp has let go (short peak = fast release only), dB", tube);
    check (vetta < 2.5f, "Vetta Comp has let go, dB", vetta);
}

//==============================================================================
void testBlueComp()
{
    std::printf ("  Blue Comp (CS-1): static curve at Sustain 50, Level 0\n");
    const std::vector<float> knobs { 50.0f, 0.0f };
    std::printf ("    in dB / out dB:");
    for (float inDb : { -60.0f, -50.0f, -40.0f, -30.0f, -20.0f, -10.0f, 0.0f })
        std::printf ("  %.0f / %.1f", inDb, staticOut (Fx::blueComp, knobs, inDb));
    std::printf ("\n");

    const float quietGain = staticGain (Fx::blueComp, knobs, -70.0f) - db (fx::dyn_detail::blueLevel);
    check (std::abs (quietGain - 13.0f) < 0.5f, "quiet notes get the Sustain gain (13 dB at 50 %), dB", quietGain);
    const float slope = (staticOut (Fx::blueComp, knobs, -5.0f) - staticOut (Fx::blueComp, knobs, -15.0f)) / 10.0f;
    check (slope > 0.22f && slope < 0.36f, "about 4:1 above the knee (softer than the Red Comp), slope", slope);
    const float redSlope = (staticOut (Fx::redComp, knobs, -5.0f) - staticOut (Fx::redComp, knobs, -15.0f)) / 10.0f;
    check (redSlope < 0.6f * slope, "Red Comp's slope is well below the Blue Comp's", redSlope);

    const auto step = stepResponse (Fx::blueComp, knobs, -50.0f, -6.0f, 1.0);
    std::printf ("    step -50 -> -6 dB: reduction %.1f dB, attack %.0f ms, release %.0f ms\n", step.reductionDb, step.attackMs, step.releaseMs);
    check (step.attackMs <= 20.0, "attack (within 2 dB), ms", step.attackMs);
    check (step.releaseMs > 80.0 && step.releaseMs < 500.0, "release (within 2 dB) of a few hundred ms, ms", step.releaseMs);

    auto tilt = [&] (int variant) { return staticGain (variant, knobs, -70.0f, 6000.0) - staticGain (variant, knobs, -70.0f, 300.0); };
    check (std::abs (tilt (Fx::blueComp)) < 0.2f, "treble switch off: flat, 6 kHz vs 300 Hz, dB", tilt (Fx::blueComp));
    check (tilt (Fx::blueCompTreb) > 4.5f && tilt (Fx::blueCompTreb) < 7.5f, "treble switch on: lift at 6 kHz, dB", tilt (Fx::blueCompTreb));
    const float sameCurve = staticOut (Fx::blueCompTreb, knobs, -10.0f, 300.0) - staticOut (Fx::blueComp, knobs, -10.0f, 300.0);
    check (std::abs (sameCurve) < 0.3f, "...and the same compression below the lift, dB", sameCurve);
}

//==============================================================================
void testVetta()
{
    std::printf ("  Vetta Comp: Sensitivity 80 (threshold -40 dB), Level 0\n");
    const std::vector<float> knobs { 80.0f, 0.0f };
    std::printf ("    in dB / out dB:");
    for (float inDb : { -60.0f, -50.0f, -40.0f, -30.0f, -20.0f, -10.0f, 0.0f })
        std::printf ("  %.0f / %.1f", inDb, staticOut (Fx::vettaComp, knobs, inDb));
    std::printf ("\n");

    const float slopeA = (staticOut (Fx::vettaComp, knobs, -20.0f) - staticOut (Fx::vettaComp, knobs, -30.0f)) / 10.0f;
    const float slopeB = (staticOut (Fx::vettaComp, knobs, 0.0f) - staticOut (Fx::vettaComp, knobs, -10.0f)) / 10.0f;
    check (std::abs (1.0f / slopeA - 2.35f) < 0.08f, "ratio 10-20 dB above the threshold", 1.0f / slopeA);
    check (std::abs (1.0f / slopeB - 2.35f) < 0.08f, "ratio 30-40 dB above the threshold", 1.0f / slopeB);
    check (std::abs (staticGain (Fx::vettaComp, knobs, -60.0f)) < 0.1f, "unity below the threshold, dB", staticGain (Fx::vettaComp, knobs, -60.0f));
    const float expected = -40.0f + 30.0f / 2.35f;
    check (std::abs (staticOut (Fx::vettaComp, knobs, -10.0f) - expected) < 1.0f, "threshold where Sensitivity puts it: -10 dB in gives (dB out)",
           staticOut (Fx::vettaComp, knobs, -10.0f));
    check (std::abs (staticGain (Fx::vettaComp, { 0.0f, 0.0f }, -3.0f)) < 0.3f, "Sensitivity 0: no compression of a -3 dB signal, dB",
           staticGain (Fx::vettaComp, { 0.0f, 0.0f }, -3.0f));
    check (std::abs (staticGain (Fx::vettaComp, { 80.0f, 12.0f }, -60.0f) - 12.0f) < 0.1f, "Level adds up to 12 dB",
           staticGain (Fx::vettaComp, { 80.0f, 12.0f }, -60.0f));

    const auto step = stepResponse (Fx::vettaComp, knobs, -50.0f, -6.0f, 1.0);
    std::printf ("    step -50 -> -6 dB: reduction %.1f dB, attack %.0f ms, release %.0f ms\n", step.reductionDb, step.attackMs, step.releaseMs);
    check (step.attackMs <= 30.0 && step.attackMs >= 2.0, "attack (within 2 dB), ms", step.attackMs);
    check (step.releaseMs > 100.0 && step.releaseMs < 600.0, "release (within 2 dB), ms", step.releaseMs);

    std::printf ("  Vetta Juice: Amount sets the ratio above -44 dB\n");
    auto slopeAt = [&] (float amount)
    {
        return (staticOut (Fx::vettaJuice, { amount, 0.0f }, -10.0f) - staticOut (Fx::vettaJuice, { amount, 0.0f }, -30.0f)) / 20.0f;
    };
    check (std::abs (slopeAt (0.0f) - 1.0f) < 0.01f, "Amount 0: 1:1, slope", slopeAt (0.0f));
    check (std::abs (slopeAt (50.0f) - 0.55f) < 0.03f, "Amount 50: slope 0.55 (1.8:1)", slopeAt (50.0f));
    check (std::abs (slopeAt (100.0f) - 0.10f) < 0.03f, "Amount 100: slope 0.10 (10:1)", slopeAt (100.0f));
    check (std::abs (staticGain (Fx::vettaJuice, { 100.0f, 30.0f }, -70.0f) - 30.0f) < 0.2f, "Level has 30 dB of gain",
           staticGain (Fx::vettaJuice, { 100.0f, 30.0f }, -70.0f));
    const float full = staticOut (Fx::vettaJuice, { 100.0f, 30.0f }, -8.0f);
    check (full > -12.0f && full < 0.0f, "Amount 100 + Level 30 dB: a guitar's peak level comes out near where it went in, dB", full);
    const float ceiling = staticOut (Fx::vettaJuice, { 0.0f, 30.0f }, -8.0f, 1000.0) ;
    check (ceiling < 9.1f, "Amount 0 + Level 30 dB stays under the soft ceiling of 2.0 (as an equally loud sine: +9 dB), dB", ceiling);

    const auto juice = stepResponse (Fx::vettaJuice, { 70.0f, 0.0f }, -60.0f, -6.0f, 1.0);
    std::printf ("    step -60 -> -6 dB: reduction %.1f dB, attack %.0f ms, release %.0f ms\n", juice.reductionDb, juice.attackMs, juice.releaseMs);
    check (juice.attackMs <= 15.0, "attack (within 2 dB), ms", juice.attackMs);
    check (juice.releaseMs > 200.0 && juice.releaseMs < 1200.0, "release (within 2 dB), ms", juice.releaseMs);
}

//==============================================================================
void testBoostComp()
{
    std::printf ("  Boost Comp: Drive, Bass, Comp, Treble, Output\n");
    auto gainAt = [&] (std::vector<float> knobs, float inDb, double hz) { return staticGain (Fx::boostComp, knobs, inDb, hz); };

    check (std::abs (gainAt ({ 0.0f, 50.0f, 0.0f, 50.0f, 0.0f }, -20.0f, 1000.0)) < 0.05f, "everything off: unity, dB", gainAt ({ 0.0f, 50.0f, 0.0f, 50.0f, 0.0f }, -20.0f, 1000.0));
    check (std::abs (gainAt ({ 50.0f, 50.0f, 0.0f, 50.0f, 0.0f }, -40.0f, 1000.0) - 13.0f) < 0.1f, "Drive 50: +13 dB", gainAt ({ 50.0f, 50.0f, 0.0f, 50.0f, 0.0f }, -40.0f, 1000.0));
    check (std::abs (gainAt ({ 100.0f, 50.0f, 0.0f, 50.0f, 0.0f }, -40.0f, 1000.0) - 26.0f) < 0.1f, "Drive 100: +26 dB (Micro Amp)", gainAt ({ 100.0f, 50.0f, 0.0f, 50.0f, 0.0f }, -40.0f, 1000.0));

    // a clean boost: a -12 dB sine boosted by 6.5 dB stays well inside the op-amp's headroom
    const auto clean = run (Fx::boostComp, { 25.0f, 50.0f, 0.0f, 50.0f, 0.0f }, steppedSine (1000.0, { { 0.0, -12.0f }, { 1.0, 0.0f } }));
    double fundamental = 0.0, total = 0.0, re = 0.0, im = 0.0;
    const size_t from = (size_t) (0.5 * sampleRate), count = (size_t) (0.4 * sampleRate);
    for (size_t i = 0; i < count; ++i)
    {
        const double ph = 2.0 * fx::pi * 1000.0 * (double) (from + i) / sampleRate;
        re += clean[from + i] * std::cos (ph);  im += clean[from + i] * std::sin (ph);
        total += (double) clean[from + i] * clean[from + i];
    }
    fundamental = 2.0 * (re * re + im * im) / (double) count;
    const double thd = std::sqrt (std::max (0.0, total - fundamental) / fundamental);
    check (thd < 1.0e-4, "clean boost: distortion of a -12 dB sine at Drive 25, %", thd * 100.0);

    const auto hot = run (Fx::boostComp, { 100.0f, 50.0f, 0.0f, 50.0f, 0.0f }, steppedSine (1000.0, { { 0.0, -6.0f }, { 0.5, 0.0f } }));
    check (harness::measure (hot).peak <= 1.6001f, "Drive 100 with a hot input runs into the op-amp's headroom (1.6), peak", harness::measure (hot).peak);

    auto bassLift = [&] (float bass) { return gainAt ({ 0.0f, bass, 0.0f, 50.0f, 0.0f }, -30.0f, 30.0) - gainAt ({ 0.0f, bass, 0.0f, 50.0f, 0.0f }, -30.0f, 1000.0); };
    auto trebleLift = [&] (float treble) { return gainAt ({ 0.0f, 50.0f, 0.0f, treble, 0.0f }, -30.0f, 12000.0) - gainAt ({ 0.0f, 50.0f, 0.0f, treble, 0.0f }, -30.0f, 300.0); };
    check (bassLift (100.0f) > 9.0f && bassLift (100.0f) < 12.5f, "Bass 100: 30 Hz vs 1 kHz, dB", bassLift (100.0f));
    check (bassLift (0.0f) < -7.0f && bassLift (0.0f) > -12.5f, "Bass 0: 30 Hz vs 1 kHz, dB", bassLift (0.0f));
    check (std::abs (bassLift (50.0f)) < 0.05f, "Bass 50: flat, dB", bassLift (50.0f));
    check (trebleLift (100.0f) > 9.0f && trebleLift (100.0f) < 12.5f, "Treble 100: 12 kHz vs 300 Hz, dB", trebleLift (100.0f));
    check (trebleLift (0.0f) < -7.0f && trebleLift (0.0f) > -12.5f, "Treble 0: 12 kHz vs 300 Hz, dB", trebleLift (0.0f));

    const std::vector<float> squash { 0.0f, 50.0f, 100.0f, 50.0f, 0.0f };
    std::printf ("    Comp 100, in dB / out dB:");
    for (float inDb : { -60.0f, -50.0f, -40.0f, -30.0f, -20.0f, -10.0f, 0.0f })
        std::printf ("  %.0f / %.1f", inDb, staticOut (Fx::boostComp, squash, inDb));
    std::printf ("\n");
    check (std::abs (gainAt (squash, -70.0f, 1000.0) - 20.0f) < 0.2f, "Comp 100: quiet notes +20 dB", gainAt (squash, -70.0f, 1000.0));
    const float range = staticOut (Fx::boostComp, squash, -5.0f) - staticOut (Fx::boostComp, squash, -25.0f);
    check (range < 3.5f, "Comp 100: 20 dB more input gives under 3.5 dB more output, dB", range);
    const float half = staticOut (Fx::boostComp, { 0.0f, 50.0f, 50.0f, 50.0f, 0.0f }, -5.0f) - staticOut (Fx::boostComp, { 0.0f, 50.0f, 50.0f, 50.0f, 0.0f }, -25.0f);
    check (half > range + 0.5f && half < 12.0f, "Comp 50 squashes less than Comp 100, dB per 20 dB", half);

    const auto step = stepResponse (Fx::boostComp, squash, -50.0f, -6.0f, 1.0);
    std::printf ("    Comp 100, step -50 -> -6 dB: reduction %.1f dB, attack %.0f ms, release %.0f ms\n", step.reductionDb, step.attackMs, step.releaseMs);
    check (step.attackMs <= 5.0 && step.releaseMs > 500.0, "Dyna-style timing: fast attack, long release (ms)", step.releaseMs);
}

//==============================================================================
/** Knobs jumping between their extremes every 100 ms under a steady low sine: anything that clicks or zippers
    shows up above 4 kHz, where the sine itself has nothing. Returns that residue relative to the output, dB.
    (Measured the same way on a bare gain going -30 <-> +12 dB: switched at once -7 dB; a 30 ms ramp in
    block-sized stairs -20 dB; a 30 ms ramp per sample -70 dB, which is the corners of the ramp.) */
float knobNoise (int variant, const std::vector<float>& a, const std::vector<float>& b)
{
    Fx effect;
    effect.prepare (sampleRate, harness::blockSize);
    std::vector<float> ka = a, kb = b;
    ka.resize (8, 0.0f);  kb.resize (8, 0.0f);
    effect.setModel (variant);
    effect.setParameters (ka.data());
    effect.reset();

    fx::Biquad highPass[2];
    for (auto& h : highPass)
        h.setHighPass (sampleRate, 4000.0, 0.707);

    float left[harness::blockSize], right[harness::blockSize], peakOut = 0.0f, peakHigh = 0.0f;
    const int total = (int) (2.0 * sampleRate);
    for (int pos = 0; pos < total; pos += harness::blockSize)
    {
        effect.setParameters ((pos / (int) (0.1 * sampleRate)) % 2 == 0 ? ka.data() : kb.data());
        for (int i = 0; i < harness::blockSize; ++i)
            left[i] = right[i] = 0.1f * (float) std::sin (2.0 * fx::pi * 110.0 * (double) (pos + i) / sampleRate);
        effect.process (left, right, harness::blockSize);
        for (int i = 0; i < harness::blockSize; ++i)
        {
            peakOut = std::max (peakOut, std::abs (left[i]));
            peakHigh = std::max (peakHigh, std::abs (highPass[1].process (highPass[0].process (left[i]))));
        }
    }
    return db (peakHigh / std::max (1.0e-9f, peakOut));
}

void testKnobMoves()
{
    std::printf ("  Knob jumps under a 110 Hz sine: residue above 4 kHz relative to the output peak\n");
    check (knobNoise (Fx::vettaComp, { 0.0f, -30.0f }, { 0.0f, 12.0f }) < -65.0f, "Vetta Comp, Level alone end to end (the reference case: a bare ramp leaves -70), dB", knobNoise (Fx::vettaComp, { 0.0f, -30.0f }, { 0.0f, 12.0f }));
    check (knobNoise (Fx::tubeComp, { -40.0f, -30.0f }, { 0.0f, 12.0f }) < -40.0f, "Tube Comp, Threshold and Level end to end, dB", knobNoise (Fx::tubeComp, { -40.0f, -30.0f }, { 0.0f, 12.0f }));
    check (knobNoise (Fx::redComp, { 0.0f, -30.0f }, { 100.0f, 12.0f }) < -40.0f, "Red Comp, Sustain and Level, dB", knobNoise (Fx::redComp, { 0.0f, -30.0f }, { 100.0f, 12.0f }));
    check (knobNoise (Fx::blueComp, { 0.0f, -30.0f }, { 100.0f, 12.0f }) < -40.0f, "Blue Comp, Sustain and Level, dB", knobNoise (Fx::blueComp, { 0.0f, -30.0f }, { 100.0f, 12.0f }));
    check (knobNoise (Fx::vettaComp, { 0.0f, -30.0f }, { 100.0f, 12.0f }) < -40.0f, "Vetta Comp, Sensitivity and Level, dB", knobNoise (Fx::vettaComp, { 0.0f, -30.0f }, { 100.0f, 12.0f }));
    check (knobNoise (Fx::vettaJuice, { 0.0f, 0.0f }, { 100.0f, 30.0f }) < -40.0f, "Vetta Juice, Amount and Level, dB", knobNoise (Fx::vettaJuice, { 0.0f, 0.0f }, { 100.0f, 30.0f }));
    check (knobNoise (Fx::boostComp, { 0.0f, 0.0f, 0.0f, 0.0f, -30.0f }, { 60.0f, 100.0f, 100.0f, 100.0f, 0.0f }) < -40.0f, "Boost Comp, all five, dB",
           knobNoise (Fx::boostComp, { 0.0f, 0.0f, 0.0f, 0.0f, -30.0f }, { 60.0f, 100.0f, 100.0f, 100.0f, 0.0f }));
}
//==============================================================================
/** Times and levels must not depend on the sample rate. */
void testOtherRates()
{
    struct Numbers { double values[11]; };
    auto measureAll = []
    {
        const auto in = steppedSine (1000.0, { { 0.0, -40.0f }, { 0.3, -20.0f }, { 0.6, -40.0f }, { 1.0, -60.0f }, { 2.0, 0.0f } });
        const auto gate = gateTiming (gainTrace (in, run (Fx::hardGate, { -30.0f, -50.0f, 100.0f, 200.0f }, in)));
        const auto tube = stepResponse (Fx::tubeComp, { -30.0f, 0.0f }, -50.0f, -6.0f, 1.0);
        const auto red = stepResponse (Fx::redComp, { 50.0f, 0.0f }, -50.0f, -6.0f, 1.0);
        const auto blue = stepResponse (Fx::blueComp, { 50.0f, 0.0f }, -50.0f, -6.0f, 1.0);
        const auto vetta = stepResponse (Fx::vettaComp, { 80.0f, 0.0f }, -50.0f, -6.0f, 1.0);
        return Numbers { { gate.first, gate.second - gate.first, tube.releaseMs, red.releaseMs, blue.releaseMs, vetta.releaseMs,
                           tube.reductionDb, red.reductionDb, blue.reductionDb, vetta.reductionDb,
                           staticOut (Fx::boostComp, { 50.0f, 100.0f, 50.0f, 0.0f, 0.0f }, -20.0f, 100.0) } };
    };
    const char* const names[] = { "gate hold ms", "gate decay ms", "Tube Comp release ms", "Red Comp release ms", "Blue Comp release ms", "Vetta Comp release ms",
                                  "Tube Comp reduction dB", "Red Comp reduction dB", "Blue Comp reduction dB", "Vetta Comp reduction dB",
                                  "Boost Comp (Bass 100, Comp 50) level of a 100 Hz sine dB" };
    const auto reference = measureAll();

    for (double rate : { 96000.0, 192000.0 })
    {
        const AtRate at (rate);
        const auto now = measureAll();
        double worst = 0.0;
        int worstIndex = 0;
        for (int i = 0; i < 11; ++i)
        {
            // times within 4 % (+ 2 ms of measuring grid), levels within 0.3 dB
            const double allowed = i < 6 ? 0.04 * reference.values[i] + 2.0 : 0.3;
            const double error = std::abs (now.values[i] - reference.values[i]) / allowed;
            if (error > worst) { worst = error; worstIndex = i; }
        }
        std::printf ("    %.0f Hz:", rate);
        for (int i = 0; i < 11; ++i)
            std::printf (" %.1f", now.values[i]);
        std::printf ("\n");
        check (worst < 1.0, (std::string ("same times and levels as at 48 kHz; furthest off: ") + names[worstIndex] + ", in units of the tolerance").c_str(), worst);
    }
    std::printf ("    (48000 Hz:");
    for (double v : reference.values)
        std::printf (" %.1f", v);
    std::printf (")\n");
}
} // namespace

int main (int argc, char* argv[])
{
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "test_output/standalone/dynamics";
    testFailures += harness::run<fx::DynamicsFx> ("dynamics", fx::dynamicsModels(), outDir);

    std::printf ("== dynamics: what each model does ==\n");
    testHardGate();
    testTubeComp();
    testRedComp();
    testBlueComp();
    testVetta();
    testBoostComp();
    testBloom();
    testKnobMoves();
    std::printf ("  Other sample rates (gate hold, decay; release of Tube / Red / Blue / Vetta; their reduction; a Boost Comp level):\n");
    testOtherRates();

    std::printf ("%s\n", testFailures == 0 ? "ALL CHECKS PASSED" : (std::to_string (testFailures) + " CHECK(S) FAILED").c_str());
    return testFailures == 0 ? 0 : 1;
}
