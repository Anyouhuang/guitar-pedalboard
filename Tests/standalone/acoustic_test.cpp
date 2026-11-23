// Standalone test of the acoustic simulator (see Harness.h): the harness run, then what makes it an acoustic
// simulator, measured: each Type's frequency response (body resonances up, a magnetic pickup's midrange down,
// the strings' top end up), what the Body, Top and Level knobs do, that the four Types differ in the ways their
// names promise, the brighter pick attack, and that switching the Type or jumping a knob mid-note does not click.
#include "Harness.h"
#include "../../Source/DSP/fx/Acoustic.h"

namespace
{
using Fx = fx::AcousticFx;
int testFailures = 0;

enum Type { standard = 0, jumbo, enhanced, piezo };
const char* const typeNames[] = { "Standard", "Jumbo", "Enhanced", "Piezo" };

struct Settings
{
    int type = standard;
    float body = 50.0f, top = 50.0f, level = 0.0f;
};

void check (bool ok, const std::string& what, double value)
{
    std::printf ("    %-4s %s (%.2f)\n", ok ? "ok" : "FAIL", what.c_str(), value);
    if (! ok)
        ++testFailures;
}

/** Mono input through the effect at fixed knobs, in blocks like a slot. */
std::vector<float> render (const std::vector<float>& input, const Settings& s, double rate = harness::testRate)
{
    Fx effect;
    effect.prepare (rate, harness::blockSize);
    const float knobs[8] = { (float) s.type, s.body, s.top, s.level };
    effect.setModel (0);
    effect.setParameters (knobs);
    effect.reset();

    auto left = input, right = input;
    for (size_t pos = 0; pos < left.size(); pos += harness::blockSize)
    {
        const int n = (int) std::min<size_t> (harness::blockSize, left.size() - pos);
        effect.setParameters (knobs);
        effect.process (left.data() + pos, right.data() + pos, n);
    }
    return left;
}

std::vector<float> sine (double hz, double seconds, float amplitude, double rate = harness::testRate)
{
    std::vector<float> x ((size_t) (seconds * rate));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = amplitude * (float) std::sin (2.0 * fx::pi * hz * (double) i / rate);
    return x;
}

double rmsDb (const std::vector<float>& x, double fromSeconds, double toSeconds, double rate = harness::testRate)
{
    double sum = 0.0;
    const size_t from = (size_t) (fromSeconds * rate), to = std::min (x.size(), (size_t) (toSeconds * rate));
    for (size_t i = from; i < to; ++i)
        sum += (double) x[i] * x[i];
    return 10.0 * std::log10 (std::max (1.0e-30, sum / (double) std::max<size_t> (1, to - from)));
}

/** Gain in dB for a steady sine (the pick-attack lift has long gone by the time it is measured). */
double gainAt (double hz, const Settings& s, double rate = harness::testRate)
{
    const auto in = sine (hz, 0.6, 0.1f, rate);
    return rmsDb (render (in, s, rate), 0.35, 0.6, rate) - rmsDb (in, 0.35, 0.6, rate);
}

//==============================================================================
void testResponses()
{
    const double points[] = { 40.0, 88.0, 105.0, 205.0, 400.0, 700.0, 1200.0, 1600.0, 2500.0, 3000.0, 5000.0, 9000.0, 12000.0 };
    std::printf ("  steady gain in dB at the default knobs\n    %-9s", "Hz");
    for (double hz : points)
        std::printf ("%7.0f", hz);
    std::printf ("\n");

    for (int type = 0; type < 4; ++type)
    {
        std::printf ("    %-9s", typeNames[type]);
        for (double hz : points)
            std::printf ("%7.1f", gainAt (hz, { type }));
        std::printf ("\n");
    }

    // The riff's loudness. Every Type lifts the lows and the top end and takes midrange away; each is trimmed so
    // that the test riff comes out about 2 dB up, and so that switching the Type is not a jump in volume.
    // (The riff is brighter than a real pickup, which has less up there to lift: a real guitar comes out
    // nearer to the level it went in at.)
    {
        const auto riff = harness::makeInput();
        double lowest = 100.0, highest = -100.0;
        std::printf ("  riff level against the input, dB:");
        for (int type = 0; type < 4; ++type)
        {
            const double level = rmsDb (render (riff, { type }), 0.0, 4.0) - rmsDb (riff, 0.0, 4.0);
            std::printf ("  %s %.1f", typeNames[type], level);
            lowest = std::min (lowest, level);
            highest = std::max (highest, level);
        }
        std::printf ("\n");
        check (lowest > 0.5 && highest < 3.5, "every Type: the riff comes out 0.5 to 3.5 dB up; the loudest, dB", highest);
        check (highest - lowest < 1.5, "the Types are within 1.5 dB of each other, dB", highest - lowest);
    }

    // Standard: a dreadnought through a microphone
    const Settings std_ { standard };
    const double mid = gainAt (1200.0, std_);
    check (gainAt (105.0, std_) - mid > 9.0, "Standard: the air resonance (105 Hz) stands above the midrange (1.2 kHz), dB", gainAt (105.0, std_) - mid);
    check (gainAt (205.0, std_) - mid > 8.0, "Standard: so does the top plate's resonance (205 Hz), dB", gainAt (205.0, std_) - mid);
    check (mid - gainAt (400.0, std_) < -4.0 && mid - gainAt (3000.0, std_) < -4.0,
           "Standard: the pickup's midrange is scooped: 1.2 kHz against 400 Hz, dB", mid - gainAt (400.0, std_));
    check (gainAt (2500.0, std_) < gainAt (205.0, std_), "Standard: the pickup's own peak (2.5 kHz) stays below the body, dB", gainAt (2500.0, std_) - gainAt (205.0, std_));
    check (gainAt (5000.0, std_) - mid > 9.0, "Standard: the strings' top end is back: 5 kHz against 1.2 kHz, dB", gainAt (5000.0, std_) - mid);
    check (gainAt (9000.0, std_) - mid > 12.0, "Standard: and 9 kHz against 1.2 kHz, dB", gainAt (9000.0, std_) - mid);
    check (gainAt (40.0, std_) - gainAt (105.0, std_) < -12.0, "Standard: nothing below the low E: 40 Hz against 105 Hz, dB", gainAt (40.0, std_) - gainAt (105.0, std_));

    // a resonance, not just an EQ bump: after a low thump the body keeps ringing near its air resonance
    {
        std::vector<float> thump ((size_t) (0.4 * harness::testRate), 0.0f);
        for (int i = 0; i < 48; ++i)
            thump[(size_t) i] = 0.3f; // 1 ms
        const auto out = render (thump, { standard, 100.0f, 50.0f, 0.0f });
        const auto flat = render (thump, { standard, 0.0f, 50.0f, 0.0f });
        const double ringing = rmsDb (out, 0.02, 0.05), without = rmsDb (flat, 0.02, 0.05);
        check (ringing - without > 12.0, "Standard, Body 100: 20-50 ms after a thump the body still rings, dB above Body 0", ringing - without);

        int crossings = 0;
        const size_t from = (size_t) (0.01 * harness::testRate), to = (size_t) (0.05 * harness::testRate);
        for (size_t i = from + 1; i < to; ++i)
            crossings += (out[i - 1] < 0.0f) != (out[i] < 0.0f) ? 1 : 0;
        const double hz = 0.5 * crossings / 0.04;
        check (hz > 85.0 && hz < 240.0, "and it rings at the body's own pitch, between the air and the top resonance, Hz", hz);
    }
}

void testKnobs()
{
    const auto at = [] (double hz, float body, float top, float level = 0.0f) { return gainAt (hz, { standard, body, top, level }); };

    check (at (105.0, 100.0f, 50.0f) - at (105.0, 0.0f, 50.0f) > 7.0, "Body 0 -> 100 at 105 Hz, dB", at (105.0, 100.0f, 50.0f) - at (105.0, 0.0f, 50.0f));
    check (at (205.0, 100.0f, 50.0f) - at (205.0, 0.0f, 50.0f) > 6.0, "Body 0 -> 100 at 205 Hz, dB", at (205.0, 100.0f, 50.0f) - at (205.0, 0.0f, 50.0f));
    check (std::abs (at (5000.0, 100.0f, 50.0f) - at (5000.0, 0.0f, 50.0f)) < 0.5, "Body leaves 5 kHz alone, dB", at (5000.0, 100.0f, 50.0f) - at (5000.0, 0.0f, 50.0f));

    check (at (9000.0, 50.0f, 100.0f) - at (9000.0, 50.0f, 0.0f) > 12.0, "Top 0 -> 100 at 9 kHz, dB", at (9000.0, 50.0f, 100.0f) - at (9000.0, 50.0f, 0.0f));
    check (at (5000.0, 50.0f, 100.0f) - at (5000.0, 50.0f, 50.0f) > 3.0, "Top 50 -> 100 at 5 kHz, dB", at (5000.0, 50.0f, 100.0f) - at (5000.0, 50.0f, 50.0f));
    check (std::abs (at (205.0, 50.0f, 100.0f) - at (205.0, 50.0f, 0.0f)) < 0.5, "Top leaves 205 Hz alone, dB", at (205.0, 50.0f, 100.0f) - at (205.0, 50.0f, 0.0f));
    check (at (9000.0, 50.0f, 0.0f) < at (9000.0, 50.0f, 50.0f) - 7.0, "Top 0: the lift at 9 kHz is gone, dB below Top 50", at (9000.0, 50.0f, 0.0f) - at (9000.0, 50.0f, 50.0f));

    check (std::abs (at (1000.0, 50.0f, 50.0f, 6.0f) - at (1000.0, 50.0f, 50.0f, 0.0f) - 6.0) < 0.05, "Level +6 dB is +6 dB", at (1000.0, 50.0f, 50.0f, 6.0f) - at (1000.0, 50.0f, 50.0f, 0.0f));
    check (std::abs (at (1000.0, 50.0f, 50.0f, -30.0f) - at (1000.0, 50.0f, 50.0f, 0.0f) + 30.0) < 0.05, "Level -30 dB is -30 dB", at (1000.0, 50.0f, 50.0f, -30.0f) - at (1000.0, 50.0f, 50.0f, 0.0f));
}

void testTypes()
{
    const auto shape = [] (int type, double hz, double reference) { return gainAt (hz, { type }) - gainAt (reference, { type }); };

    // Jumbo: the bigger box resonates lower and louder
    check (shape (jumbo, 88.0, 2500.0) - shape (standard, 88.0, 2500.0) > 3.0, "Jumbo has more at 88 Hz than Standard, dB", shape (jumbo, 88.0, 2500.0) - shape (standard, 88.0, 2500.0));
    check (shape (jumbo, 175.0, 2500.0) > shape (jumbo, 205.0, 2500.0) && shape (standard, 205.0, 2500.0) > shape (standard, 175.0, 2500.0),
           "Jumbo's top plate sits at 175 Hz, Standard's at 205 Hz: Jumbo 175 against 205 Hz, dB", shape (jumbo, 175.0, 2500.0) - shape (jumbo, 205.0, 2500.0));
    check (shape (jumbo, 9000.0, 205.0) < shape (standard, 9000.0, 205.0) - 1.5, "Jumbo is rounder than Standard: 9 kHz against its body, dB less", shape (jumbo, 9000.0, 205.0) - shape (standard, 9000.0, 205.0));

    // Enhanced: presence
    check (shape (enhanced, 3000.0, 205.0) - shape (standard, 3000.0, 205.0) > 1.5, "Enhanced has more presence than Standard: 3 kHz against its body, dB more", shape (enhanced, 3000.0, 205.0) - shape (standard, 3000.0, 205.0));

    // Piezo: thin body, quack
    check (shape (piezo, 105.0, 2500.0) - shape (standard, 105.0, 2500.0) < -3.0, "Piezo has less body (105 Hz) than Standard, dB", shape (piezo, 105.0, 2500.0) - shape (standard, 105.0, 2500.0));
    check (shape (piezo, 1600.0, 205.0) - shape (standard, 1600.0, 205.0) > 4.0, "Piezo quacks: 1.6 kHz against its body, dB more than Standard", shape (piezo, 1600.0, 205.0) - shape (standard, 1600.0, 205.0));
    check (shape (piezo, 9000.0, 205.0) > shape (standard, 9000.0, 205.0) + 1.5, "Piezo is brighter than Standard: 9 kHz against its body, dB more", shape (piezo, 9000.0, 205.0) - shape (standard, 9000.0, 205.0));
}

/** How much louder a suddenly starting tone is in its first milliseconds than once it is steady, in dB. */
double attackLift (double hz, int type, double rate = harness::testRate)
{
    auto in = sine (hz, 0.5, 0.2f, rate);
    in.insert (in.begin(), (size_t) (0.2 * rate), 0.0f); // silence first, so the tone really starts
    const auto out = render (in, { type }, rate);
    return rmsDb (out, 0.202, 0.212, rate) - rmsDb (out, 0.55, 0.7, rate);
}

void testPickAttack()
{
    const double lift = attackLift (6000.0, standard), enhancedLift = attackLift (6000.0, enhanced);
    check (lift > 2.0 && lift < 7.0, "Standard: a 6 kHz tone is brighter in its first 10 ms than later, dB", lift);
    check (enhancedLift > lift + 1.0, "Enhanced: more of that than Standard, dB", enhancedLift - lift);
    check (std::abs (attackLift (150.0, standard)) < 1.5, "a 150 Hz tone gets no such lift, dB", attackLift (150.0, standard));

    // gone within a tenth of a second
    auto in = sine (6000.0, 0.5, 0.2f);
    in.insert (in.begin(), (size_t) (0.2 * harness::testRate), 0.0f);
    const auto out = render (in, { standard });
    check (std::abs (rmsDb (out, 0.30, 0.34) - rmsDb (out, 0.55, 0.7)) < 0.3, "the lift is over 100 ms after the note began, dB left", rmsDb (out, 0.30, 0.34) - rmsDb (out, 0.55, 0.7));

    // the same at another sample rate
    check (std::abs (attackLift (6000.0, standard, 96000.0) - lift) < 0.8, "and it is the same at 96 kHz, dB difference", attackLift (6000.0, standard, 96000.0) - lift);
}

void testOtherRates()
{
    for (double rate : { 44100.0, 96000.0 })
        for (double hz : { 105.0, 1000.0, 9000.0 })
        {
            const double difference = gainAt (hz, { standard }, rate) - gainAt (hz, { standard });
            check (std::abs (difference) < 0.6, "Standard at " + std::to_string ((int) rate) + " Hz: the gain at " + std::to_string ((int) hz) + " Hz is as at 48 kHz, dB difference", difference);
        }
}

/** Biggest sample-to-sample step of a steady 220 Hz sine while the knobs change every 80 ms, against the
    biggest step of any of the steady sounds involved. */
void testSwitching()
{
    const auto in = sine (220.0, 2.0, 0.3f);
    const auto maxStep = [] (const std::vector<float>& x)
    {
        float step = 0.0f;
        for (size_t i = (size_t) (0.1 * harness::testRate) + 1; i < x.size(); ++i)
            step = std::max (step, std::abs (x[i] - x[i - 1]));
        return step;
    };

    const auto switching = [&] (const std::vector<Settings>& states, const std::string& what)
    {
        float steady = 0.0f;
        for (const auto& s : states)
            steady = std::max (steady, maxStep (render (in, s)));

        Fx effect;
        effect.prepare (harness::testRate, harness::blockSize);
        auto left = in, right = in;
        effect.setModel (0);
        float knobs[8] = { (float) states[0].type, states[0].body, states[0].top, states[0].level };
        effect.setParameters (knobs);
        effect.reset();
        for (size_t pos = 0; pos < left.size(); pos += harness::blockSize)
        {
            const auto& s = states[(size_t) ((double) pos / harness::testRate / 0.08) % states.size()];
            knobs[0] = (float) s.type;  knobs[1] = s.body;  knobs[2] = s.top;  knobs[3] = s.level;
            const int n = (int) std::min<size_t> (harness::blockSize, left.size() - pos);
            effect.setParameters (knobs);
            effect.process (left.data() + pos, right.data() + pos, n);
        }
        const float moving = maxStep (left);
        check (moving <= steady * 1.15f + 0.002f, what + ": biggest step while switching against steady", moving / steady);
    };

    switching ({ { standard }, { jumbo }, { enhanced }, { piezo } }, "Type switched every 80 ms");
    switching ({ { standard, 0.0f, 50.0f }, { standard, 100.0f, 50.0f } }, "Body jumping 0 <-> 100");
    switching ({ { standard, 50.0f, 0.0f }, { standard, 50.0f, 100.0f } }, "Top jumping 0 <-> 100");
    switching ({ { piezo, 100.0f, 100.0f, 6.0f }, { jumbo, 0.0f, 0.0f, -6.0f } }, "everything at once");
}

/** Both sides get the same (mono) signal, whatever comes in. */
void testMono()
{
    Fx effect;
    effect.prepare (harness::testRate, harness::blockSize);
    const float knobs[8] = { 0.0f, 50.0f, 50.0f, 0.0f };
    effect.setModel (0);
    effect.setParameters (knobs);
    effect.reset();

    auto left = sine (330.0, 0.2, 0.2f), right = sine (990.0, 0.2, 0.1f);
    float difference = 0.0f;
    for (size_t pos = 0; pos + harness::blockSize <= left.size(); pos += harness::blockSize)
    {
        effect.setParameters (knobs);
        effect.process (left.data() + pos, right.data() + pos, harness::blockSize);
        for (int i = 0; i < harness::blockSize; ++i)
            difference = std::max (difference, std::abs (left[pos + (size_t) i] - right[pos + (size_t) i]));
    }
    check (difference == 0.0f, "mono: left and right come out the same", difference);
}
} // namespace

int main (int argc, char* argv[])
{
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "test_output/standalone/acoustic";
    testFailures += harness::run<fx::AcousticFx> ("acoustic", fx::acousticModels(), outDir);

    std::printf ("== acoustic: what the simulator does ==\n");
    testResponses();
    testKnobs();
    testTypes();
    testPickAttack();
    testOtherRates();
    testSwitching();
    testMono();

    std::printf ("%s\n", testFailures == 0 ? "ALL CHECKS PASSED" : (std::to_string (testFailures) + " CHECK(S) FAILED").c_str());
    return testFailures == 0 ? 0 : 1;
}
