// Standalone test of the Wah engine (see Harness.h): the harness run, then each wah's measured resonance
// (frequency, Q, gain, bass passed) at Position 0 / 50 / 100, that the eight models differ, and that sweeping
// the Position knob - however fast - leaves no steps in the output.
#include "Harness.h"
#include "../../Source/DSP/fx/Wah.h"

namespace
{
using Fx = fx::WahFx;
double sampleRate = harness::testRate; // (the checks at other rates change it for a while)
int testFailures = 0;

struct AtRate
{
    explicit AtRate (double rate) : previous (sampleRate) { sampleRate = rate; }
    ~AtRate() { sampleRate = previous; }
    double previous;
};

void check (bool ok, const std::string& what, double value)
{
    std::printf ("    %-4s %s (%.2f)\n", ok ? "ok" : "FAIL", what.c_str(), value);
    if (! ok)
        ++testFailures;
}

/** Stereo input through one model at fixed knobs, in blocks like a slot. */
void run (Fx& effect, int variant, float position, float mix, std::vector<float>& left, std::vector<float>& right)
{
    const float knobs[8] = { position, mix };
    effect.setModel (variant);
    effect.setParameters (knobs);
    effect.reset();
    for (size_t pos = 0; pos < left.size(); pos += harness::blockSize)
    {
        const int n = (int) std::min<size_t> (harness::blockSize, left.size() - pos);
        effect.setParameters (knobs);
        effect.process (left.data() + pos, right.data() + pos, n);
    }
}

std::vector<float> impulseResponse (int variant, float position, float mix = 100.0f)
{
    Fx effect;
    effect.prepare (sampleRate, harness::blockSize);
    std::vector<float> left (16384, 0.0f), right (16384, 0.0f);
    left[0] = right[0] = 1.0f;
    run (effect, variant, position, mix, left, right);
    return left;
}

double magnitudeDb (const std::vector<float>& h, double hz)
{
    double re = 0.0, im = 0.0;
    const double w = 2.0 * fx::pi * hz / sampleRate;
    for (size_t i = 0; i < h.size(); ++i)
    {
        re += h[i] * std::cos (w * (double) i);
        im -= h[i] * std::sin (w * (double) i);
    }
    return 20.0 * std::log10 (std::max (1.0e-12, std::sqrt (re * re + im * im)));
}

struct Resonance { double hz, q, peakDb, bassDb; };

/** Peak frequency, Q from the -3 dB width, gain at the peak and at the guitar's low A (110 Hz). */
Resonance measure (int variant, float position)
{
    const auto h = impulseResponse (variant, position);

    double best = 150.0, bestDb = -200.0;
    for (double hz = 150.0; hz < 6000.0; hz *= std::pow (2.0, 1.0 / 48.0))
        if (const double d = magnitudeDb (h, hz); d > bestDb)
        {
            bestDb = d;
            best = hz;
        }

    double lo = best / std::pow (2.0, 1.0 / 48.0), hi = best * std::pow (2.0, 1.0 / 48.0);
    for (int i = 0; i < 40; ++i) // ternary search for the top
    {
        const double a = lo + (hi - lo) / 3.0, b = hi - (hi - lo) / 3.0;
        if (magnitudeDb (h, a) < magnitudeDb (h, b)) lo = a; else hi = b;
    }
    const double peakHz = 0.5 * (lo + hi), peakDb = magnitudeDb (h, peakHz);

    auto edge = [&] (double direction)
    {
        double inside = peakHz, outside = peakHz * std::pow (4.0, direction);
        for (int i = 0; i < 50; ++i)
        {
            const double mid = std::sqrt (inside * outside);
            if (magnitudeDb (h, mid) > peakDb - 3.0103) inside = mid; else outside = mid;
        }
        return inside;
    };
    return { peakHz, peakHz / (edge (1.0) - edge (-1.0)), peakDb, magnitudeDb (h, 110.0) };
}

/** What the analogue prototype of a voice gives at `hz`: coupling cap, then low-pass + band-pass of one resonator. */
double prototypeDb (const fx::wah_detail::Voice& v, double pedal, double hz)
{
    const double t = std::pow (pedal, v.taper);
    const double f0 = v.heelHz * std::pow (v.toeHz / v.heelHz, t), q = v.heelQ * std::pow (v.toeQ / v.heelQ, t);
    const double peak = std::pow (10.0, (v.heelPeakDb + (v.toePeakDb - v.heelPeakDb) * t) / 20.0);
    const double low = v.lowPassPath ? std::pow (10.0, (v.heelLowDb + (v.toeLowDb - v.heelLowDb) * t) / 20.0) : 0.0;
    const double band = std::sqrt (std::max (0.0, (peak / q) * (peak / q) - low * low));

    const std::complex<double> s (0.0, hz / f0), sc (0.0, hz / v.couplingHz);
    const auto resonator = (low + band * s) / (s * s + s / q + 1.0);
    return 20.0 * std::log10 (std::abs (resonator * sc / (1.0 + sc)));
}

//==============================================================================
void testVoices()
{
    const auto models = fx::wahModels();
    Resonance measured[Fx::numVariants][3];

    std::printf ("  Resonance at Position 0 / 50 / 100 (frequency Hz, Q, peak dB, level at 110 Hz dB):\n");
    for (int m = 0; m < Fx::numVariants; ++m)
    {
        const auto& v = fx::wah_detail::voices[m];
        std::printf ("    %-14s", models[(size_t) m].name);
        bool frequencyOk = true, qOk = true, peakOk = true, shapeOk = true;
        double worstShape = 0.0;

        for (int p = 0; p < 3; ++p)
        {
            const double pedal = 0.5 * p, t = std::pow (pedal, v.taper);
            const auto r = measured[m][p] = measure (m, 50.0f * (float) p);
            std::printf ("   %5.0f Hz  Q %4.1f  %+5.1f  %+6.1f", r.hz, r.q, r.peakDb, r.bassDb);

            const double f0 = v.heelHz * std::pow (v.toeHz / v.heelHz, t), q = v.heelQ * std::pow (v.toeQ / v.heelQ, t);
            frequencyOk = frequencyOk && std::abs (r.hz / f0 - 1.0) < 0.05;
            qOk = qOk && std::abs (r.q / q - 1.0) < 0.20;
            peakOk = peakOk && std::abs (r.peakDb - prototypeDb (v, pedal, r.hz)) < 0.5;

            // the whole curve against the analogue prototype, from the low E to well above the sweep
            const auto h = impulseResponse (m, 50.0f * (float) p);
            for (double hz : { 82.0, 110.0, 220.0, 440.0, 880.0, 1760.0, 3520.0 })
                worstShape = std::max (worstShape, std::abs (magnitudeDb (h, hz) - prototypeDb (v, pedal, hz)));
            shapeOk = worstShape < 1.0;
        }
        std::printf ("\n");
        const std::string name = models[(size_t) m].name;
        check (frequencyOk, name + ": peak frequencies within 5 % of the voice's sweep", measured[m][2].hz / measured[m][0].hz);
        check (qOk, name + ": Q (from the -3 dB width) within 20 % of the voice's, heel / toe", measured[m][0].q / measured[m][2].q);
        check (peakOk && shapeOk, name + ": response matches the analogue prototype from 82 Hz to 3.5 kHz, worst dB", worstShape);
    }

    // ---- the characters
    auto sweepOctaves = [&] (int m) { return std::log2 (measured[m][2].hz / measured[m][0].hz); };
    check (measured[Fx::colorful][2].q > 2.5 * measured[Fx::colorful][0].q, "Colorful (no inductor): Q grows along the sweep, toe / heel",
           measured[Fx::colorful][2].q / measured[Fx::colorful][0].q);
    check (measured[Fx::colorful][2].peakDb > measured[Fx::colorful][0].peakDb + 6.0, "Colorful: and so does the gain, dB",
           measured[Fx::colorful][2].peakDb - measured[Fx::colorful][0].peakDb);
    bool inductorsNarrowAtHeel = true, colorfulThinnest = true;
    for (int m = 0; m < Fx::numVariants; ++m)
        if (m != Fx::colorful)
        {
            inductorsNarrowAtHeel = inductorsNarrowAtHeel && (m == Fx::vettaWah || measured[m][0].q > measured[m][2].q * 1.2);
            colorfulThinnest = colorfulThinnest && (measured[m][2].bassDb - measured[m][2].peakDb) > (measured[Fx::colorful][2].bassDb - measured[Fx::colorful][2].peakDb) + 3.0;
        }
    check (inductorsNarrowAtHeel, "the six inductor wahs: the peak is sharper at the heel than at the toe", 1.0);
    check (colorfulThinnest, "Colorful passes the least bass under its toe peak (pure band-pass), 110 Hz below peak dB",
           measured[Fx::colorful][2].bassDb - measured[Fx::colorful][2].peakDb);
    check (sweepOctaves (Fx::chrome) < sweepOctaves (Fx::weeper) - 0.5, "Chrome (V847) has the short sweep, octaves", sweepOctaves (Fx::chrome));
    check (sweepOctaves (Fx::chromeCustom) > sweepOctaves (Fx::chrome) + 0.5 && measured[Fx::chromeCustom][1].q < 0.75 * measured[Fx::chrome][1].q
               && measured[Fx::chromeCustom][1].bassDb > measured[Fx::chrome][1].bassDb + 2.0,
           "Chrome Custom: longer sweep, wider peak and more bass than the Chrome, octaves", sweepOctaves (Fx::chromeCustom));
    check (measured[Fx::conductor][2].hz < 1500.0 && measured[Fx::conductor][0].hz < 330.0, "Conductor (Boomerang) sits lowest: toe peak Hz", measured[Fx::conductor][2].hz);
    check (measured[Fx::weeper][0].q > measured[Fx::fassel][0].q * 1.15 && measured[Fx::weeper][1].bassDb < measured[Fx::fassel][1].bassDb - 2.0,
           "Weeper: sharper and thinner than the Fassel, heel Q", measured[Fx::weeper][0].q);
    check (std::abs (measured[Fx::vettaWah][0].q / measured[Fx::vettaWah][2].q - 1.0) < 0.1 && sweepOctaves (Fx::vettaWah) > 3.0,
           "Vetta Wah: constant Q over the widest sweep, octaves", sweepOctaves (Fx::vettaWah));
    check (measured[Fx::throaty][0].hz < measured[Fx::fassel][0].hz * 0.9 && measured[Fx::throaty][2].hz < measured[Fx::fassel][2].hz * 0.9,
           "Throaty sits lower than the Fassel at both ends, heel Hz", measured[Fx::throaty][0].hz);

    // ---- every pair differs audibly somewhere: peak frequency by > 0.1 octave, Q by > 15 %, peak or bass by > 2 dB
    double closest = 1.0e9;
    int closestA = 0, closestB = 0;
    for (int a = 0; a < Fx::numVariants; ++a)
        for (int b = a + 1; b < Fx::numVariants; ++b)
        {
            double difference = 0.0;
            for (int p = 0; p < 3; ++p)
            {
                const auto& x = measured[a][p];
                const auto& y = measured[b][p];
                difference = std::max ({ difference, std::abs (std::log2 (x.hz / y.hz)) / 0.1, std::abs (std::log (x.q / y.q)) / 0.14,
                                         std::abs (x.peakDb - y.peakDb) / 2.0, std::abs (x.bassDb - y.bassDb) / 2.0 });
            }
            if (difference < closest) { closest = difference; closestA = a; closestB = b; }
        }
    check (closest > 1.5, std::string ("all eight differ; the closest pair is ") + models[(size_t) closestA].name + " / " + models[(size_t) closestB].name
                              + ", in units of a clearly audible difference", closest);
}

//==============================================================================
/** The sweep must be where it is at 48 kHz whatever the sample rate. */
void testOtherRates()
{
    std::printf ("  Other sample rates: peak frequency / Q / gain against the voice, Position 0 / 50 / 100, all eight models\n");
    for (double rate : { 44100.0, 96000.0, 192000.0 })
    {
        const AtRate at (rate);
        double worstHz = 0.0, worstQ = 0.0, worstDb = 0.0;
        for (int m = 0; m < Fx::numVariants; ++m)
            for (int p = 0; p < 3; ++p)
            {
                const auto& v = fx::wah_detail::voices[m];
                const double pedal = 0.5 * p, t = std::pow (pedal, v.taper);
                const auto r = measure (m, 50.0f * (float) p);
                worstHz = std::max (worstHz, std::abs (r.hz / (v.heelHz * std::pow (v.toeHz / v.heelHz, t)) - 1.0));
                worstQ = std::max (worstQ, std::abs (r.q / (v.heelQ * std::pow (v.toeQ / v.heelQ, t)) - 1.0));
                worstDb = std::max (worstDb, std::abs (r.peakDb - prototypeDb (v, pedal, r.hz)));
            }
        std::printf ("    %.0f Hz: worst frequency error %.1f %%, worst Q error %.1f %%, worst peak gain error %.2f dB\n", rate, 100.0 * worstHz, 100.0 * worstQ, worstDb);
        check (worstHz < 0.05 && worstQ < 0.20 && worstDb < 0.5, "same sweep as at 48 kHz (within 5 % / 20 % / 0.5 dB), worst frequency error %", 100.0 * worstHz);
    }
}

//==============================================================================
void testStereoAndMix()
{
    std::printf ("  Stereo and Mix:\n");
    harness::Lcg rng { 7u };
    std::vector<float> noiseL (24000), noiseR (24000);
    for (auto& s : noiseL) s = 0.2f * (rng.next01() * 2.0f - 1.0f);
    for (auto& s : noiseR) s = 0.2f * (rng.next01() * 2.0f - 1.0f);

    Fx effect;
    effect.prepare (sampleRate, harness::blockSize);

    auto l = noiseL, r = noiseR;
    run (effect, Fx::fassel, 40.0f, 100.0f, l, r);
    auto onlyL = noiseL, silent = std::vector<float> (noiseL.size(), 0.0f);
    run (effect, Fx::fassel, 40.0f, 100.0f, onlyL, silent);
    auto onlyR = noiseR, copyR = noiseR;
    run (effect, Fx::fassel, 40.0f, 100.0f, onlyR, copyR);

    float leak = 0.0f, differenceL = 0.0f, differenceR = 0.0f;
    for (size_t i = 0; i < l.size(); ++i)
    {
        leak = std::max (leak, std::abs (silent[i]));
        differenceL = std::max (differenceL, std::abs (l[i] - onlyL[i]));
        differenceR = std::max (differenceR, std::abs (r[i] - onlyR[i]));
    }
    check (leak < 1.0e-12f, "a signal on the left only leaves the right silent, peak", leak);
    check (differenceL == 0.0f && differenceR == 0.0f, "each side is filtered on its own (same as processing it alone), max difference", differenceL + differenceR);

    auto dryL = noiseL, dryR = noiseR;
    run (effect, Fx::weeper, 70.0f, 0.0f, dryL, dryR);
    check (dryL == noiseL && dryR == noiseR, "Mix 0: the dry signal, bit for bit", 0.0);

    auto halfL = noiseL, halfR = noiseR, wetL = noiseL, wetR = noiseR;
    run (effect, Fx::weeper, 70.0f, 50.0f, halfL, halfR);
    run (effect, Fx::weeper, 70.0f, 100.0f, wetL, wetR);
    float mixError = 0.0f;
    for (size_t i = 0; i < halfL.size(); ++i)
        mixError = std::max (mixError, std::abs (halfL[i] - 0.5f * (noiseL[i] + wetL[i])));
    check (mixError < 1.0e-5f, "Mix 50: half dry, half wet, max error", mixError);
}

//==============================================================================
/** Sine through the wah while Position is moved by `positionAt (seconds)` (set once per block, like host
    automation). Returns what is left above `highPassHz` relative to the output's peak, in dB: a smooth sweep of a
    sine only makes sidebands close to it, steps in the coefficients (zipper) or in the signal (clicks) splatter. */
template <typename Move>
float sweepResidue (int variant, double sineHz, double highPassHz, Move positionAt, double seconds = 2.0)
{
    Fx effect;
    effect.prepare (sampleRate, harness::blockSize);
    float knobs[8] = { positionAt (0.0), 100.0f };
    effect.setModel (variant);
    effect.setParameters (knobs);
    effect.reset();

    fx::Biquad highPass[3];
    for (auto& h : highPass)
        h.setHighPass (sampleRate, highPassHz, 0.707);

    float left[harness::blockSize], right[harness::blockSize], peakOut = 0.0f, peakHigh = 0.0f;
    const int total = (int) (seconds * sampleRate);
    for (int pos = 0; pos < total; pos += harness::blockSize)
    {
        knobs[0] = positionAt ((double) pos / sampleRate);
        effect.setParameters (knobs);
        for (int i = 0; i < harness::blockSize; ++i)
            left[i] = right[i] = 0.2f * (float) std::sin (2.0 * fx::pi * sineHz * (double) (pos + i) / sampleRate);
        effect.process (left, right, harness::blockSize);
        for (int i = 0; i < harness::blockSize; ++i)
        {
            const float high = highPass[2].process (highPass[1].process (highPass[0].process (left[i])));
            if (pos > (int) (0.15 * sampleRate)) // (not the sine's own switch-on)
            {
                peakOut = std::max (peakOut, std::abs (left[i]));
                peakHigh = std::max (peakHigh, std::abs (high));
            }
        }
    }
    return harness::toDb (peakHigh / std::max (1.0e-9f, peakOut));
}

void testSweep()
{
    std::printf ("  Moving Position under a steady sine: residue far above the sine, relative to the output peak\n");
    auto jumps    = [] (double t) { return ((int) (t / 0.1)) % 2 == 0 ? 0.0f : 100.0f; };                 // heel <-> toe every 100 ms
    auto rocking  = [] (double t) { const double ph = std::fmod (t * 6.0, 1.0); return (float) (100.0 * (ph < 0.5 ? 2.0 * ph : 2.0 - 2.0 * ph)); }; // 6 Hz
    auto parked   = [] (double) { return 50.0f; };

    float worstJump = -200.0f, worstRock = -200.0f, worstParked = -200.0f;
    for (int m = 0; m < Fx::numVariants; ++m)
    {
        worstParked = std::max ({ worstParked, sweepResidue (m, 110.0, 5000.0, parked), sweepResidue (m, 700.0, 12000.0, parked) });
        worstJump = std::max ({ worstJump, sweepResidue (m, 110.0, 5000.0, jumps), sweepResidue (m, 700.0, 12000.0, jumps) });
        worstRock = std::max ({ worstRock, sweepResidue (m, 110.0, 5000.0, rocking), sweepResidue (m, 700.0, 12000.0, rocking) });
    }
    std::printf ("    (parked at 50: %.1f dB - the measurement floor)\n", worstParked);
    check (worstJump < -70.0f, "jumping heel <-> toe every 100 ms, worst of the 8 models, dB", worstJump);
    check (worstRock < -70.0f, "rocking at 6 Hz (set once per 256-sample block), worst of the 8 models, dB", worstRock);

    // and the sweep really is fast: 40 ms after a jump to the toe the filter is there
    Fx effect;
    effect.prepare (sampleRate, harness::blockSize);
    std::vector<float> left ((size_t) (0.5 * sampleRate)), right;
    for (size_t i = 0; i < left.size(); ++i)
        left[i] = 0.2f * (float) std::sin (2.0 * fx::pi * 2200.0 * (double) i / sampleRate);
    right = left;
    float knobs[8] = { 0.0f, 100.0f };
    effect.setModel (Fx::weeper);
    effect.setParameters (knobs);
    effect.reset();
    float before = 0.0f, after = 0.0f;
    for (size_t pos = 0; pos + harness::blockSize <= left.size(); pos += harness::blockSize)
    {
        const double t = (double) pos / sampleRate;
        knobs[0] = t < 0.25 ? 0.0f : 100.0f;
        effect.setParameters (knobs);
        effect.process (left.data() + pos, right.data() + pos, harness::blockSize);
        for (int i = 0; i < harness::blockSize; ++i)
        {
            if (t > 0.2 && t < 0.24)  before = std::max (before, std::abs (left[pos + (size_t) i]));
            if (t > 0.30 && t < 0.34) after = std::max (after, std::abs (left[pos + (size_t) i]));
        }
    }
    check (harness::toDb (after / before) > 25.0f, "Weeper, 2.2 kHz sine: 50 ms after the jump to the toe the peak has arrived, dB louder", harness::toDb (after / before));
}
} // namespace

int main (int argc, char* argv[])
{
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "test_output/standalone/wah";
    testFailures += harness::run<fx::WahFx> ("wah", fx::wahModels(), outDir);

    std::printf ("== wah: what each model does ==\n");
    testVoices();
    testOtherRates();
    testStereoAndMix();
    testSweep();

    std::printf ("%s\n", testFailures == 0 ? "ALL CHECKS PASSED" : (std::to_string (testFailures) + " CHECK(S) FAILED").c_str());
    return testFailures == 0 ? 0 : 1;
}
