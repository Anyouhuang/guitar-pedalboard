// Standalone test of the Preamp+EQ engine (see Harness.h): the harness run, then measured magnitude responses
// against what each band is meant to do, transparency at flat settings, the Studio EQ's soft clipping, the
// Vintage Pre's tube stage, and that moving knobs leave no steps in the signal.
#include "Harness.h"
#include "../../Source/DSP/fx/Eq.h"

namespace
{
using Fx = fx::EqFx;
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
    std::printf ("    %-4s %s (%.3f)\n", ok ? "ok" : "FAIL", what.c_str(), value);
    if (! ok)
        ++testFailures;
}

using Knobs = std::vector<float>;

/** Stereo input through one model at fixed knobs, in blocks like a slot. */
void run (int variant, Knobs knobs, std::vector<float>& left, std::vector<float>& right)
{
    knobs.resize (8, 0.0f);
    Fx effect;
    effect.prepare (sampleRate, harness::blockSize);
    effect.setModel (variant);
    effect.setParameters (knobs.data());
    effect.reset();
    for (size_t pos = 0; pos < left.size(); pos += harness::blockSize)
    {
        const int n = (int) std::min<size_t> (harness::blockSize, left.size() - pos);
        effect.setParameters (knobs.data());
        effect.process (left.data() + pos, right.data() + pos, n);
    }
}

/** Impulse response (left side), scaled back to a unit impulse. `amplitude` small keeps nonlinear models linear. */
std::vector<float> impulseResponse (int variant, const Knobs& knobs, float amplitude = 0.1f)
{
    const size_t length = std::max<size_t> (32768, (size_t) (0.7 * sampleRate));
    std::vector<float> left (length, 0.0f), right (length, 0.0f);
    left[0] = right[0] = amplitude;
    run (variant, knobs, left, right);
    for (auto& s : left)
        s /= amplitude;
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

double gainAt (int variant, const Knobs& knobs, double hz) { return magnitudeDb (impulseResponse (variant, knobs), hz); }

double gainAt48k (int variant, const Knobs& knobs, double hz)
{
    const AtRate reference (harness::testRate);
    return gainAt (variant, knobs, hz);
}

/** Frequency of the largest gain (or, for `sign` = -1, the deepest cut) between 30 Hz and 16 kHz. */
double extremeHz (const std::vector<float>& h, double sign = 1.0)
{
    double best = 30.0, bestDb = -1.0e9;
    for (double hz = 30.0; hz < 16000.0; hz *= std::pow (2.0, 1.0 / 96.0))
        if (const double d = sign * magnitudeDb (h, hz); d > bestDb)
        {
            bestDb = d;
            best = hz;
        }
    return best;
}

/** Width of a boost, as centre / (upper - lower frequency) where the response is `below` dB under the top. */
double measuredQ (const std::vector<float>& h, double centre, double below)
{
    const double top = magnitudeDb (h, centre);
    auto edge = [&] (double direction)
    {
        double inside = centre, outside = centre * std::pow (16.0, direction);
        for (int i = 0; i < 50; ++i)
        {
            const double mid = std::sqrt (inside * outside);
            if (magnitudeDb (h, mid) > top - below) inside = mid; else outside = mid;
        }
        return inside;
    };
    return centre / (edge (1.0) - edge (-1.0));
}

struct Noise
{
    std::vector<float> left, right;
    explicit Noise (size_t length, float amplitude = 0.3f) : left (length), right (length)
    {
        harness::Lcg rng { 42u };
        for (size_t i = 0; i < length; ++i)
        {
            left[i] = amplitude * (rng.next01() * 2.0f - 1.0f);
            right[i] = amplitude * (rng.next01() * 2.0f - 1.0f);
        }
    }
};

bool transparent (int variant, const Knobs& knobs)
{
    const Noise in (48000);
    auto l = in.left, r = in.right;
    run (variant, knobs, l, r);
    return l == in.left && r == in.right;
}

/** Steady sine through a model: distortion (everything that is not the fundamental or DC, relative to the
    fundamental), second and third harmonic (relative), the fundamental's level, the output peak. */
struct SineResult { double thd, h2, h3, fundamentalDb, peak; };

SineResult sineThrough (int variant, const Knobs& knobs, double hz, float amplitude)
{
    const size_t length = (size_t) (2.0 * sampleRate), from = (size_t) sampleRate, count = (size_t) sampleRate; // hz must be a whole number
    std::vector<float> left (length), right;
    for (size_t i = 0; i < length; ++i)
        left[i] = amplitude * (float) std::sin (2.0 * fx::pi * hz * (double) i / sampleRate);
    right = left;
    run (variant, knobs, left, right);

    auto component = [&] (double f)
    {
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < count; ++i)
        {
            const double ph = 2.0 * fx::pi * f * (double) (from + i) / sampleRate;
            re += left[from + i] * std::cos (ph);
            im += left[from + i] * std::sin (ph);
        }
        return 2.0 * std::sqrt (re * re + im * im) / (double) count;
    };

    double total = 0.0, mean = 0.0, peak = 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        mean += left[from + i];
        peak = std::max (peak, (double) std::abs (left[from + i]));
    }
    mean /= (double) count;
    for (size_t i = 0; i < count; ++i)
        total += (left[from + i] - mean) * (left[from + i] - mean);
    total /= (double) count;

    const double a1 = component (hz), power1 = 0.5 * a1 * a1;
    return { std::sqrt (std::max (0.0, total - power1) / power1), component (2.0 * hz) / a1, component (3.0 * hz) / a1,
             20.0 * std::log10 (a1 / amplitude), peak };
}

//==============================================================================
void testGraphic()
{
    std::printf ("  Graphic EQ\n");
    check (transparent (Fx::graphicEq, { 0, 0, 0, 0, 0 }), "flat: passes noise bit for bit", 0.0);

    const double centres[] = { 80.0, 220.0, 480.0, 1100.0, 2200.0 };
    double worstBoost = 0.0, worstCut = 0.0, neighbour = -100.0;
    for (int b = 0; b < 5; ++b)
    {
        Knobs up (5, 0.0f), down (5, 0.0f);
        up[(size_t) b] = 12.0f;
        down[(size_t) b] = -12.0f;
        const auto h = impulseResponse (Fx::graphicEq, up);
        worstBoost = std::max (worstBoost, std::abs (magnitudeDb (h, centres[b]) - 12.0));
        worstBoost = std::max (worstBoost, 12.0 * std::abs (std::log2 (extremeHz (h) / centres[b]))); // and the top really is at the centre
        worstCut = std::max (worstCut, std::abs (gainAt (Fx::graphicEq, down, centres[b]) + 12.0));
        for (int other = 0; other < 5; ++other)
            if (other != b)
                neighbour = std::max (neighbour, magnitudeDb (h, centres[other]));
    }
    check (worstBoost < 0.15, "each band at +12 dB: +12 dB at its own centre (80 / 220 / 480 / 1.1k / 2.2k), worst error dB", worstBoost);
    check (worstCut < 0.15, "each band at -12 dB: -12 dB at its centre, worst error dB", worstCut);
    check (neighbour < 4.5, "a band at +12 dB lifts its neighbours' centres by less than 4.5 dB", neighbour);

    const auto all = impulseResponse (Fx::graphicEq, { 12, 12, 12, 12, 12 });
    double lowest = 100.0, highest = -100.0;
    for (double hz = 80.0; hz <= 2200.0; hz *= 1.02)
    {
        lowest = std::min (lowest, magnitudeDb (all, hz));
        highest = std::max (highest, magnitudeDb (all, hz));
    }
    std::printf ("    all five at +12 dB: between 80 Hz and 2.2 kHz the curve stays within %+.1f ... %+.1f dB\n", lowest, highest);
    check (lowest > 12.0 && highest < 19.0 && highest - lowest < 6.0, "...a smooth plateau (bands overlap, no holes), ripple dB", highest - lowest);
    const double smile = gainAt (Fx::graphicEq, { 6, 0, -6, 0, 6 }, 480.0);
    check (smile < -4.0 && smile > -7.0, "80Hz +6 / 480Hz -6 / 2.2kHz +6: the mid scoop at 480 Hz, dB", smile);
}

void testParametric()
{
    std::printf ("  Parametric EQ\n");
    check (transparent (Fx::parametricEq, { 0, 0, 800, 35, 0 }), "flat: passes noise bit for bit", 0.0);

    const auto lows = impulseResponse (Fx::parametricEq, { 12, 0, 800, 35, 0 });
    check (std::abs (magnitudeDb (lows, 30.0) - 12.0) < 0.5 && std::abs (magnitudeDb (lows, 150.0) - 6.0) < 0.3 && std::abs (magnitudeDb (lows, 3000.0)) < 0.2,
           "Lows +12: low shelf, +12 dB at 30 Hz, half of it at 150 Hz, nothing at 3 kHz; dB at 150 Hz", magnitudeDb (lows, 150.0));
    const auto highs = impulseResponse (Fx::parametricEq, { 0, -12, 800, 35, 0 });
    check (std::abs (magnitudeDb (highs, 16000.0) + 12.0) < 0.5 && std::abs (magnitudeDb (highs, 3500.0) + 6.0) < 0.3 && std::abs (magnitudeDb (highs, 200.0)) < 0.2,
           "Highs -12: high shelf, -12 dB at 16 kHz, half of it at 3.5 kHz, nothing at 200 Hz; dB at 3.5 kHz", magnitudeDb (highs, 3500.0));

    double worstCentre = 0.0, worstGain = 0.0;
    for (float hz : { 80.0f, 250.0f, 800.0f, 2500.0f, 8000.0f })
    {
        const auto h = impulseResponse (Fx::parametricEq, { 0, 0, hz, 50, 12 });
        worstCentre = std::max (worstCentre, std::abs (12.0 * std::log2 (extremeHz (h) / hz)));
        worstGain = std::max (worstGain, std::abs (magnitudeDb (h, hz) - 12.0));
    }
    check (worstCentre < 0.2 && worstGain < 0.1, "Freq 80 Hz ... 8 kHz, Gain +12: the peak is +12 dB and sits at Freq, worst error semitones", worstCentre);

    // Q is the width at half the gain (in dB)
    const double qLow = measuredQ (impulseResponse (Fx::parametricEq, { 0, 0, 1000, 0, 12 }), 1000.0, 6.0);
    const double qMid = measuredQ (impulseResponse (Fx::parametricEq, { 0, 0, 1000, 50, 12 }), 1000.0, 6.0);
    const double qHigh = measuredQ (impulseResponse (Fx::parametricEq, { 0, 0, 1000, 100, 12 }), 1000.0, 6.0);
    std::printf ("    Q knob 0 / 50 / 100 %%: measured Q %.2f / %.2f / %.2f (wanted 0.4 / 2 / 10)\n", qLow, qMid, qHigh);
    check (std::abs (qLow / 0.4 - 1.0) < 0.1 && std::abs (qMid / 2.0 - 1.0) < 0.1 && std::abs (qHigh / 10.0 - 1.0) < 0.1, "Q knob spans 0.4 ... 10, at 50 %", qMid);
    check (std::abs (gainAt (Fx::parametricEq, { 0, 0, 1000, 50, -12 }, 1000.0) + 12.0) < 0.1, "Gain -12: -12 dB at Freq", gainAt (Fx::parametricEq, { 0, 0, 1000, 50, -12 }, 1000.0));
}

void testStudio()
{
    std::printf ("  Studio EQ\n");
    check (transparent (Fx::studioEq, { 500, 0, 1500, 0, 0 }), "flat: passes noise bit for bit", 0.0);

    const auto low = impulseResponse (Fx::studioEq, { 500, 12, 1500, 0, 0 });
    const auto high = impulseResponse (Fx::studioEq, { 500, 0, 1500, -12, 0 });
    check (std::abs (magnitudeDb (low, 500.0) - 12.0) < 0.1 && std::abs (12.0 * std::log2 (extremeHz (low) / 500.0)) < 0.2, "Low Gain +12 at 500 Hz: +12 dB there", magnitudeDb (low, 500.0));
    check (std::abs (magnitudeDb (high, 1500.0) + 12.0) < 0.1 && std::abs (12.0 * std::log2 (extremeHz (high, -1.0) / 1500.0)) < 0.2, "Hi Gain -12 at 1.5 kHz: -12 dB there", magnitudeDb (high, 1500.0));
    const auto ends = impulseResponse (Fx::studioEq, { 75, 12, 12500, 12, 0 });
    check (std::abs (magnitudeDb (ends, 75.0) - 12.0) < 0.2 && std::abs (magnitudeDb (ends, 12500.0) - 12.0) < 0.2, "the ends of the ranges: +12 dB at 75 Hz and at 12.5 kHz", magnitudeDb (ends, 12500.0));
    const auto mirror = impulseResponse (Fx::studioEq, { 500, 12, 500, 0, 0 });
    const auto mirrorCut = impulseResponse (Fx::studioEq, { 500, -12, 500, 0, 0 });
    double asymmetry = 0.0;
    for (double hz = 100.0; hz < 3000.0; hz *= 1.1)
        asymmetry = std::max (asymmetry, std::abs (magnitudeDb (mirror, hz) + magnitudeDb (mirrorCut, hz)));
    check (asymmetry < 0.05, "boost and cut are mirror images (reciprocal), worst dB", asymmetry);

    // constant-Q: the width 3 dB under the top hardly depends on the amount of boost (the Parametric EQ's does)
    auto width = [] (int variant, const Knobs& knobs, double hz) { return measuredQ (impulseResponse (variant, knobs), hz, 3.0); };
    const double studio12 = width (Fx::studioEq, { 500, 12, 1500, 0, 0 }, 500.0), studio6 = width (Fx::studioEq, { 500, 6, 1500, 0, 0 }, 500.0);
    const double param12 = width (Fx::parametricEq, { 0, 0, 500, 34.16f, 12 }, 500.0), param6 = width (Fx::parametricEq, { 0, 0, 500, 34.16f, 6 }, 500.0);
    std::printf ("    width 3 dB under the top, as Q, at +6 / +12 dB: Studio %.2f / %.2f, Parametric (Q knob at 1.2) %.2f / %.2f\n", studio6, studio12, param6, param12);
    check (studio12 / studio6 < 1.4, "Studio EQ: constant-Q, +12 dB is less than 1.4x narrower than +6 dB", studio12 / studio6);
    check (param12 / param6 > 1.6, "...while the Parametric EQ's band narrows with gain, ratio", param12 / param6);

    check (std::abs (gainAt (Fx::studioEq, { 500, 0, 1500, 0, 12 }, 1000.0) - 12.0) < 0.05, "Gain +12 (small signal): +12 dB", gainAt (Fx::studioEq, { 500, 0, 1500, 0, 12 }, 1000.0));
    const auto clean = sineThrough (Fx::studioEq, { 500, 0, 1500, 0, 0 }, 1000.0, 0.95f);
    const auto boosted = sineThrough (Fx::studioEq, { 500, 0, 1500, 0, 6 }, 1000.0, 0.4f);
    const auto pushed = sineThrough (Fx::studioEq, { 500, 0, 1500, 0, 12 }, 1000.0, 0.5f);
    const auto slammed = sineThrough (Fx::studioEq, { 500, 12, 1500, 0, 12 }, 500.0, 1.0f);
    std::printf ("    0.95 sine at Gain 0: THD %.4f %%;  0.4 sine at Gain +6: THD %.4f %%;  0.5 sine at Gain +12: THD %.1f %%, peak %.2f (unclipped it would be 1.99)\n",
                 100.0 * clean.thd, 100.0 * boosted.thd, 100.0 * pushed.thd, pushed.peak);
    check (clean.thd < 1.0e-5 && boosted.thd < 1.0e-5, "output stage is clean up to full scale, THD %", 100.0 * std::max (clean.thd, boosted.thd));
    check (pushed.peak > 1.5 && pushed.peak < 1.85 && pushed.thd > 0.01 && pushed.thd < 0.12 && pushed.h3 > 5.0 * pushed.h2,
           "Gain +12 on a 0.5 sine: soft clipping - peak rounded off, a few % of odd harmonics, THD %", 100.0 * pushed.thd);
    check (slammed.peak <= 2.0001, "even +24 dB on a full-scale sine stays under the ceiling of 2.0, peak", slammed.peak);
}

void testShift()
{
    std::printf ("  4 Band Shift EQ\n");
    check (transparent (Fx::fourBandShiftEq, { 0, 0, 0, 0, 50 }), "flat: passes noise bit for bit", 0.0);
    check (transparent (Fx::fourBandShiftEq, { 0, 0, 0, 0, 100 }), "flat at any Shift", 0.0);

    // the shelves' frequency: where the gain is half of the shelf's
    auto shelfHz = [] (const std::vector<float>& h, double halfDb)
    {
        double best = 30.0, error = 1.0e9;
        for (double hz = 30.0; hz < 16000.0; hz *= std::pow (2.0, 1.0 / 96.0))
            if (const double e = std::abs (magnitudeDb (h, hz) - halfDb); e < error)
            {
                error = e;
                best = hz;
            }
        return best;
    };

    std::printf ("    Shift     Low shelf   Low Mid    Hi Mid    Hi shelf   (each band alone at +12 dB)\n");
    double f[3][4];
    int row = 0;
    for (float shift : { 0.0f, 50.0f, 100.0f })
    {
        f[row][0] = shelfHz (impulseResponse (Fx::fourBandShiftEq, { 12, 0, 0, 0, shift }), 6.0);
        f[row][1] = extremeHz (impulseResponse (Fx::fourBandShiftEq, { 0, 12, 0, 0, shift }));
        f[row][2] = extremeHz (impulseResponse (Fx::fourBandShiftEq, { 0, 0, 12, 0, shift }));
        f[row][3] = shelfHz (impulseResponse (Fx::fourBandShiftEq, { 0, 0, 0, 12, shift }), 6.0);
        std::printf ("    %3.0f %%   %7.0f Hz  %7.0f Hz %7.0f Hz  %7.0f Hz\n", shift, f[row][0], f[row][1], f[row][2], f[row][3]);
        ++row;
    }
    auto near = [] (double got, double want) { return std::abs (got / want - 1.0) < 0.03; };
    check (near (f[1][0], 120.0) && near (f[1][1], 400.0) && near (f[1][2], 1600.0) && near (f[1][3], 5000.0), "Shift 50: 120 Hz / 400 Hz / 1.6 kHz / 5 kHz", f[1][1]);
    check (near (f[2][0], 120.0 * std::pow (2.0, -0.7)) && near (f[2][1], 400.0 * std::pow (2.0, 0.7)) && near (f[2][2], 1600.0 * std::pow (2.0, 0.8)) && near (f[2][3], 5000.0 * std::pow (2.0, 0.8)),
           "Shift 100: the low band 0.7 octave lower, the others 0.7 - 0.8 octave higher; Low Mid Hz", f[2][1]);
    check (near (f[0][0], 120.0 * std::pow (2.0, 0.7)) && near (f[0][1], 400.0 * std::pow (2.0, -0.7)) && near (f[0][2], 1600.0 * std::pow (2.0, -0.8)) && near (f[0][3], 5000.0 * std::pow (2.0, -0.8)),
           "Shift 0: the other way round (for bass); Low Mid Hz", f[0][1]);

    const auto mids = impulseResponse (Fx::fourBandShiftEq, { 0, 12, -12, 0, 50 });
    check (std::abs (magnitudeDb (mids, 400.0) - 12.0) < 1.5 && std::abs (magnitudeDb (mids, 1600.0) + 12.0) < 1.5, "Low Mid +12 / Hi Mid -12: +-12 dB at their centres, less the other band's skirt (~1 dB); at 400 Hz", magnitudeDb (mids, 400.0));
    const auto shelves = impulseResponse (Fx::fourBandShiftEq, { -12, 0, 0, 12, 50 });
    check (std::abs (magnitudeDb (shelves, 30.0) + 12.0) < 0.6 && std::abs (magnitudeDb (shelves, 16000.0) - 12.0) < 0.6, "Low -12 / Hi +12: -12 dB at 30 Hz, +12 dB at 16 kHz", magnitudeDb (shelves, 16000.0));
}

void testMidFocus()
{
    std::printf ("  Mid Focus EQ\n");
    const auto h = impulseResponse (Fx::midFocusEq, { 200, 25, 3000, 25, 0 });
    std::printf ("    HP 200 Hz / LP 3 kHz, Q 25 %% (0.7):  50 Hz %+.1f  100 Hz %+.1f  200 Hz %+.1f  800 Hz %+.1f  3 kHz %+.1f  6 kHz %+.1f  12 kHz %+.1f dB\n",
                 magnitudeDb (h, 50.0), magnitudeDb (h, 100.0), magnitudeDb (h, 200.0), magnitudeDb (h, 800.0), magnitudeDb (h, 3000.0), magnitudeDb (h, 6000.0), magnitudeDb (h, 12000.0));
    check (std::abs (magnitudeDb (h, 200.0) + 3.0) < 0.3 && std::abs (magnitudeDb (h, 3000.0) + 3.0) < 0.3, "-3 dB at both corner frequencies; at 200 Hz", magnitudeDb (h, 200.0));
    check (std::abs (magnitudeDb (h, 800.0)) < 0.3, "flat in between; at 800 Hz dB", magnitudeDb (h, 800.0));
    check (std::abs (magnitudeDb (h, 50.0) + 24.0) < 1.0 && magnitudeDb (h, 12000.0) < -23.0, "12 dB/octave outside: two octaves out is 24 dB down; at 50 Hz", magnitudeDb (h, 50.0));

    const auto resonant = impulseResponse (Fx::midFocusEq, { 200, 100, 3000, 100, 0 });
    check (std::abs (magnitudeDb (resonant, 200.0) - 15.0) < 0.5 && std::abs (magnitudeDb (resonant, 3000.0) - 15.0) < 0.5, "Q 100 % (5.6): +15 dB resonances at the corners; at 3 kHz", magnitudeDb (resonant, 3000.0));
    const auto soft = impulseResponse (Fx::midFocusEq, { 200, 0, 3000, 0, 0 });
    check (std::abs (magnitudeDb (soft, 200.0) + 9.1) < 0.4, "Q 0 % (0.35): a gentle corner, -9 dB at the corner frequency", magnitudeDb (soft, 200.0));
    check (std::abs (gainAt (Fx::midFocusEq, { 200, 25, 3000, 25, 12 }, 800.0) - 12.0) < 0.3 && std::abs (gainAt (Fx::midFocusEq, { 200, 25, 3000, 25, -12 }, 800.0) + 12.0) < 0.3,
           "Gain +-12: the pass band moves by +-12 dB; at +12", gainAt (Fx::midFocusEq, { 200, 25, 3000, 25, 12 }, 800.0));
    const auto ends = impulseResponse (Fx::midFocusEq, { 2000, 25, 500, 25, 0 });
    check (magnitudeDb (ends, 1000.0) < -10.0, "corners crossed (HP 2 kHz above LP 500 Hz): what is left at 1 kHz, dB", magnitudeDb (ends, 1000.0));
}

void testVintagePre()
{
    std::printf ("  Vintage Pre\n");
    const Knobs clean { 0, 0, 0, 20, 20000 };
    const auto h = impulseResponse (Fx::vintagePre, clean, 0.001f);
    check (std::abs (magnitudeDb (h, 1000.0)) < 0.1 && std::abs (magnitudeDb (h, 100.0)) < 0.2 && std::abs (magnitudeDb (h, 8000.0)) < 0.3,
           "Gain 0, filters open, small signal: unity from 100 Hz to 8 kHz; dB at 1 kHz", magnitudeDb (h, 1000.0));

    std::printf ("    0.3 sine at 220 Hz:  Gain    level     THD      2nd      3rd     peak\n");
    double previousThd = 0.0;
    bool rising = true, bounded = true;
    SineResult results[5];
    int index = 0;
    for (float gain : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
    {
        const auto r = results[index++] = sineThrough (Fx::vintagePre, { gain, 0, 0, 20, 20000 }, 220.0, 0.3f);
        std::printf ("                         %3.0f %%  %+5.1f dB  %5.1f %%  %5.1f %%  %5.1f %%   %.2f\n", gain, r.fundamentalDb, 100.0 * r.thd, 100.0 * r.h2, 100.0 * r.h3, r.peak);
        rising = rising && r.thd > previousThd * 1.3;
        bounded = bounded && r.peak < 1.0 && r.fundamentalDb > -6.0 && r.fundamentalDb < 6.0;
        previousThd = r.thd;
    }
    check (rising, "Gain adds tube distortion: THD rises with every quarter turn; at 100 %, %", 100.0 * previousThd);
    check (results[0].thd < 0.03 && results[0].h2 > 3.0 * results[0].h3, "Gain 0: nearly clean, and what there is is second harmonic (asymmetric tube curve), THD %", 100.0 * results[0].thd);
    check (results[4].thd > 0.25, "Gain 100: clips hard, THD %", 100.0 * results[4].thd);
    check (bounded, "the level stays within +-6 dB of the input over the whole Gain range; at 100 %, dB", results[4].fundamentalDb);

    // oversampling: what a clipped 1301 Hz sine leaves between its own harmonics (aliases land there)
    {
        const double hz = 1301.0;
        const size_t length = (size_t) (2.0 * sampleRate), from = (size_t) sampleRate, count = (size_t) sampleRate;
        std::vector<float> left (length), right;
        for (size_t i = 0; i < length; ++i)
            left[i] = 0.3f * (float) std::sin (2.0 * fx::pi * hz * (double) i / sampleRate);
        right = left;
        run (Fx::vintagePre, { 100, 0, 0, 20, 20000 }, left, right);

        double total = 0.0, harmonic = 0.0, mean = 0.0;
        for (size_t i = 0; i < count; ++i) mean += left[from + i];
        mean /= (double) count;
        for (size_t i = 0; i < count; ++i) total += (left[from + i] - mean) * (left[from + i] - mean);
        total /= (double) count;
        for (int k = 1; k * hz < 0.5 * sampleRate; ++k)
        {
            double re = 0.0, im = 0.0;
            for (size_t i = 0; i < count; ++i)
            {
                const double ph = 2.0 * fx::pi * k * hz * (double) (from + i) / sampleRate;
                re += left[from + i] * std::cos (ph);
                im += left[from + i] * std::sin (ph);
            }
            harmonic += 2.0 * (re * re + im * im) / ((double) count * (double) count);
        }
        const double aliasDb = 10.0 * std::log10 (std::max (1.0e-20, total - harmonic) / total);
        check (aliasDb < -50.0, "Gain 100 on a 1301 Hz sine: everything that is not a harmonic (aliasing), dB below the signal", aliasDb);
    }

    // Phase
    {
        const Noise in (24000, 0.05f);
        auto l0 = in.left, r0 = in.right, l180 = in.left, r180 = in.right;
        run (Fx::vintagePre, { 30, 0, 0, 20, 20000 }, l0, r0);
        run (Fx::vintagePre, { 30, 0, 1, 20, 20000 }, l180, r180);
        bool inverted = true, mono = true;
        double dot = 0.0;
        for (size_t i = 0; i < l0.size(); ++i)
        {
            inverted = inverted && l180[i] == -l0[i];
            mono = mono && l0[i] == r0[i];
            dot += (double) l0[i] * (0.5 * (in.left[i] + in.right[i]));
        }
        check (inverted, "Phase 180: exactly the Phase 0 output upside down", 0.0);
        check (dot > 0.0, "Phase 0 keeps the input's polarity", dot);
        check (mono, "mono: both sides carry the same signal even with different inputs", 0.0);
    }

    // the filters
    const auto filtered = impulseResponse (Fx::vintagePre, { 0, 0, 0, 200, 3000 }, 0.001f);
    check (std::abs (magnitudeDb (filtered, 200.0) + 3.0) < 0.3 && std::abs (magnitudeDb (filtered, 50.0) + 24.0) < 1.0, "Hi Pass Filter 200 Hz: -3 dB there, 12 dB/octave below", magnitudeDb (filtered, 200.0));
    check (std::abs (magnitudeDb (filtered, 3000.0) + 3.0) < 0.3 && magnitudeDb (filtered, 12000.0) < -23.0, "Lo Pass Filter 3 kHz: -3 dB there, 12 dB/octave above", magnitudeDb (filtered, 3000.0));
    check (std::abs (gainAt (Fx::vintagePre, { 0, -12, 0, 20, 20000 }, 1000.0) + 12.0) < 0.5, "Output -12 dB", gainAt (Fx::vintagePre, { 0, -12, 0, 20, 20000 }, 1000.0));
}

//==============================================================================
/** The curves must be the same at every sample rate - also the filters that sit very low against the rate. */
void testOtherRates()
{
    std::printf ("  Other sample rates:\n");
    for (double rate : { 44100.0, 96000.0, 192000.0 })
    {
        const AtRate at (rate);
        std::printf ("    %.0f Hz\n", rate);
        check (transparent (Fx::graphicEq, { 0, 0, 0, 0, 0 }) && transparent (Fx::fourBandShiftEq, { 0, 0, 0, 0, 30 }), "flat: bit for bit", 0.0);

        const auto low = impulseResponse (Fx::graphicEq, { 12, 0, 0, 0, 0 });
        const auto high = impulseResponse (Fx::graphicEq, { 0, 0, 0, 0, -12 });
        check (std::abs (magnitudeDb (low, 80.0) - 12.0) < 0.15 && std::abs (12.0 * std::log2 (extremeHz (low) / 80.0)) < 0.3
                   && std::abs (magnitudeDb (high, 2200.0) + 12.0) < 0.15 && std::abs (magnitudeDb (low, 220.0) - gainAt48k (Fx::graphicEq, { 12, 0, 0, 0, 0 }, 220.0)) < 0.15,
               "Graphic EQ: +12 dB at 80 Hz, -12 dB at 2.2 kHz, same skirt as at 48 kHz; dB at 80 Hz", magnitudeDb (low, 80.0));

        const auto band = impulseResponse (Fx::parametricEq, { 0, 0, 8000, 50, 12 });
        check (std::abs (magnitudeDb (band, 8000.0) - 12.0) < 0.1 && std::abs (12.0 * std::log2 (extremeHz (band) / 8000.0)) < 0.2, "Parametric EQ: +12 dB band at 8 kHz; dB there", magnitudeDb (band, 8000.0));

        const auto focus = impulseResponse (Fx::midFocusEq, { 20, 25, 20000, 25, 0 });
        check (std::abs (magnitudeDb (focus, 20.0) + 3.0) < 0.3 && std::abs (magnitudeDb (focus, 10.0) + 12.3) < 0.7 && std::abs (magnitudeDb (focus, 1000.0)) < 0.1
                   && std::abs (magnitudeDb (focus, 20000.0) + 3.0) < 0.3,
               "Mid Focus EQ at the ends of its ranges: -3 dB at 20 Hz and at 20 kHz, flat at 1 kHz; dB at 20 Hz", magnitudeDb (focus, 20.0));

        const auto pre = impulseResponse (Fx::vintagePre, { 0, 0, 0, 20, 20000 }, 0.001f);
        check (std::abs (magnitudeDb (pre, 1000.0)) < 0.1 && std::abs (magnitudeDb (pre, 20.0) + 4.0) < 0.4 && std::abs (magnitudeDb (pre, 10000.0)) < 0.5,
               "Vintage Pre, small signal: unity at 1 kHz and 10 kHz, -4 dB at 20 Hz (its high-pass -3, the DC blocker -1); dB at 20 Hz", magnitudeDb (pre, 20.0));
        const auto driven = sineThrough (Fx::vintagePre, { 60, 0, 0, 20, 20000 }, 220.0, 0.3f);
        const auto driven48 = [] { const AtRate reference (harness::testRate); return sineThrough (Fx::vintagePre, { 60, 0, 0, 20, 20000 }, 220.0, 0.3f); }();
        check (std::abs (driven.thd / driven48.thd - 1.0) < 0.03 && std::abs (driven.fundamentalDb - driven48.fundamentalDb) < 0.1, "Vintage Pre, Gain 60: same distortion as at 48 kHz, THD %", 100.0 * driven.thd);
    }
}

//==============================================================================
/** Two knob settings swapped every 100 ms under a steady 110 Hz sine. Returns what is left above 5 kHz relative
    to the output's peak, in dB: gliding filters only make sidebands next to the sine, steps would splatter.
    (Measured the same way on one of these shelves, 150 Hz going -12 <-> +12 dB: switched at once -9 dB; a 50 ms
    glide with the filter set once per 256-sample block -20 dB, once per 16 samples -42 dB, and recomputed
    exactly on every sample -70 dB, which is the corners of the glide itself.) */
float knobNoise (int variant, Knobs a, Knobs b)
{
    a.resize (8, 0.0f);
    b.resize (8, 0.0f);
    Fx effect;
    effect.prepare (sampleRate, harness::blockSize);
    effect.setModel (variant);
    effect.setParameters (a.data());
    effect.reset();

    fx::Biquad highPass[3];
    for (auto& h : highPass)
        h.setHighPass (sampleRate, 5000.0, 0.707);

    float left[harness::blockSize], right[harness::blockSize], peakOut = 0.0f, peakHigh = 0.0f;
    const int total = (int) (2.0 * sampleRate);
    for (int pos = 0; pos < total; pos += harness::blockSize)
    {
        effect.setParameters ((pos / (int) (0.1 * sampleRate)) % 2 == 0 ? a.data() : b.data());
        for (int i = 0; i < harness::blockSize; ++i)
            left[i] = right[i] = 0.2f * (float) std::sin (2.0 * fx::pi * 110.0 * (double) (pos + i) / sampleRate);
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

void testKnobMoves()
{
    std::printf ("  Knobs swapped between two settings every 100 ms under a 110 Hz sine: residue above 5 kHz, dB re output peak\n");
    auto test = [] (const char* what, int variant, const Knobs& a, const Knobs& b, float limit = -55.0f)
    {
        const float noise = knobNoise (variant, a, b);
        check (noise < limit, what, noise);
    };
    test ("Parametric EQ, Lows alone -12 <-> +12 (the reference case: as good as recomputing every sample, -70)", Fx::parametricEq, { -12, 0, 800, 35, 0 }, { 12, 0, 800, 35, 0 }, -68.0f);
    test ("Graphic EQ, all bands -12 <-> +12", Fx::graphicEq, { -12, -12, -12, -12, -12 }, { 12, 12, 12, 12, 12 }, -65.0f);
    test ("Parametric EQ, shelves and band end to end, Freq 80 <-> 8000, Q 0 <-> 100", Fx::parametricEq, { -12, -12, 80, 0, -12 }, { 12, 12, 8000, 100, 12 });
    test ("Parametric EQ, narrow +12 dB band sweeping across the sine (80 <-> 300 Hz)", Fx::parametricEq, { 0, 0, 80, 100, 12 }, { 0, 0, 300, 100, 12 });
    test ("Studio EQ, both bands and the output gain end to end", Fx::studioEq, { 75, -12, 800, -12, -12 }, { 1000, 12, 12500, 12, 0 });
    test ("4 Band Shift EQ, all gains and Shift end to end", Fx::fourBandShiftEq, { -12, -12, -12, -12, 0 }, { 12, 12, 12, 12, 100 });
    test ("Mid Focus EQ, corners, resonances and gain end to end", Fx::midFocusEq, { 20, 0, 500, 0, -12 }, { 2000, 100, 20000, 100, 12 });
    test ("Mid Focus EQ, resonant high-pass sweeping across the sine (60 <-> 200 Hz)", Fx::midFocusEq, { 60, 100, 4500, 25, 0 }, { 200, 100, 4500, 25, 0 });
    test ("Vintage Pre, Output, Phase and both filters (Gain 0)", Fx::vintagePre, { 0, -30, 0, 20, 1000 }, { 0, 12, 1, 1000, 20000 });
}
} // namespace

int main (int argc, char* argv[])
{
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "test_output/standalone/eq";
    testFailures += harness::run<fx::EqFx> ("eq", fx::eqModels(), outDir);

    std::printf ("== eq: what each model does ==\n");
    testGraphic();
    testParametric();
    testStudio();
    testShift();
    testMidFocus();
    testVintagePre();
    testOtherRates();
    testKnobMoves();

    std::printf ("%s\n", testFailures == 0 ? "ALL CHECKS PASSED" : (std::to_string (testFailures) + " CHECK(S) FAILED").c_str());
    return testFailures == 0 ? 0 : 1;
}
