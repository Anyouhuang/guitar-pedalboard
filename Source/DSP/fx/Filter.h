#pragma once

#include <cstdint>

#include "../DspUtils.h"
#include "../ModelTypes.h"

namespace fx
{
// Names of the choice knobs
inline const char* const filterVowelNames[]      = { "A", "E", "I", "O", "U" };
inline const char* const filterAutoNames[]       = { "Sine", "Hold", "Ramp", "Random" };
inline const char* const filterSweepNames[]      = { "Up", "Up-Down" };
inline const char* const filterTypeNames[]       = { "LP", "BP", "HP" };
inline const char* const filterPatternNames[]    = { "Rise", "Fall", "Peak", "Zigzag", "Hi-Lo", "Pairs", "Random 1", "Random 2" };
inline const char* const filterStepNames[]       = { "2", "3", "4", "5", "6", "7", "8", "9" };
inline const char* const filterRangeNames[]      = { "Hi", "Lo" };
inline const char* const filterLfoNames[]        = { "Ramp Up", "Ramp Down", "Triangle", "Square" };
inline const char* const filterModeNames[]       = { "Up", "Down" };
inline const char* const filterSynthWaveNames[]  = { "Saw", "Square", "Pulse", "Triangle", "Saw+Sqr", "Detune", "Octave", "Sub" };
inline const char* const filterAttackWaveNames[] = { "Square", "PWM", "Ramp" };

namespace filter_detail
{
constexpr double twoPi = 2.0 * pi;

/** Formants F1..F3 of the vowels A E I O U (Peterson & Barney's averages for a male voice), the level of
    each formant and the resonators' Q. */
inline constexpr double vowelHz[5][3]   = { { 730.0, 1090.0, 2440.0 }, { 530.0, 1840.0, 2480.0 }, { 270.0, 2290.0, 3010.0 },
                                            { 570.0, 840.0, 2410.0 },  { 300.0, 870.0, 2240.0 } };
inline constexpr double vowelGain[5][3] = { { 1.0, 0.70, 0.25 }, { 1.0, 0.45, 0.30 }, { 1.0, 0.35, 0.25 },
                                            { 1.0, 0.60, 0.15 }, { 1.0, 0.40, 0.12 } };
inline constexpr double vowelQ[3]       = { 6.0, 9.0, 10.0 };

/** The Seeker's step patterns 3..8 (0 = lowest, 1 = highest filter position); "Rise" and "Fall" are computed. */
inline constexpr double seekerTable[6][9] = {
    { 0.00, 0.35, 0.70, 1.00, 0.70, 0.35, 0.00, 0.50, 1.00 },   // Peak
    { 0.00, 0.60, 0.20, 0.80, 0.40, 1.00, 0.30, 0.70, 0.50 },   // Zigzag
    { 0.10, 0.90, 0.10, 0.90, 0.30, 0.70, 0.30, 0.70, 0.50 },   // Hi-Lo
    { 0.00, 0.00, 0.50, 0.50, 1.00, 1.00, 0.50, 0.50, 0.25 },   // Pairs
    { 0.62, 0.11, 0.87, 0.35, 0.05, 0.74, 0.48, 0.96, 0.23 },   // Random 1
    { 0.30, 0.95, 0.55, 0.00, 0.80, 0.18, 0.68, 0.42, 1.00 } }; // Random 2

/** Filter position (0..1) of one step of a Seeker pattern played with `steps` steps. */
inline double seekerPosition (int pattern, int step, int steps) noexcept
{
    if (pattern == 0) return (double) step / (double) (steps - 1);
    if (pattern == 1) return 1.0 - (double) step / (double) (steps - 1);
    return seekerTable[pattern - 2][step];
}

/** Centre frequency of a Seeker filter position: the travel of a wah pedal. */
inline double seekerHz (double position) noexcept { return 300.0 * std::pow (8.0, position); }

inline double smooth01 (double x) noexcept
{
    x = std::clamp (x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

inline int choiceOf (float value, int maxIndex) noexcept { return std::clamp ((int) std::floor (value + 0.5f), 0, maxIndex); }

/** Transparent up to 1.5 (the guitar peaks around 0.4), then bends over towards 4: keeps a resonance that
    is hit dead on from running away. Continuous, so it cannot click. */
inline float softLimit (float x) noexcept
{
    const float a = std::abs (x);
    if (a <= 1.5f)
        return x;

    const float y = 1.5f + 2.5f * std::tanh ((a - 1.5f) * 0.4f);
    return x < 0.0f ? -y : y;
}

/** How much a resonant low-/high-pass is turned down as its peak grows, and the peak gain of the band-pass
    (a wah's resonance is louder than its input, but not by the full Q). `k` = 1 / Q. */
inline double lowPassNorm (double k) noexcept  { return 1.0 / std::sqrt (1.0 + 0.25 / k); }
inline double bandPassNorm (double k) noexcept { return 1.4 * std::sqrt (k); }

//==============================================================================
/** A value that moves in a straight line to a new target during each control interval. */
struct Ramp
{
    float v = 0.0f, dv = 0.0f;

    void aim (double target, float invSteps, bool snap) noexcept
    {
        if (snap) { v = (float) target; dv = 0.0f; }
        else      dv = ((float) target - v) * invSteps;
    }

    float step() noexcept { v += dv; return v; }
};

/** Coefficients of a state-variable filter (g = tan (pi fc / fs), k = 1 / Q), interpolated per sample so
    that sweeps do not zipper. One set can drive the left and the right filter. */
struct SvfCoef
{
    float g = 0.1f, k = 1.0f, dg = 0.0f, dk = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;

    void aim (double gTarget, double kTarget, float invSteps, bool snap) noexcept
    {
        if (snap) { g = (float) gTarget; k = (float) kTarget; dg = dk = 0.0f; }
        else      { dg = ((float) gTarget - g) * invSteps; dk = ((float) kTarget - k) * invSteps; }
    }

    void step() noexcept
    {
        g += dg;
        k += dk;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
};

/** Topology-preserving state-variable filter (stays stable while its frequency is modulated). */
struct Svf
{
    float s1 = 0.0f, s2 = 0.0f, lp = 0.0f, bp = 0.0f, hp = 0.0f;

    void reset() noexcept { s1 = s2 = lp = bp = hp = 0.0f; }

    void process (float x, const SvfCoef& c) noexcept
    {
        const float v3 = x - s2;
        bp = c.a1 * s1 + c.a2 * v3;
        lp = s2 + c.a2 * s1 + c.a3 * v3;
        s1 = 2.0f * bp - s1;
        s2 = 2.0f * lp - s2;
        hp = x - c.k * bp - lp;
    }

    /** Called now and then: a filter ringing out must not end up computing with denormal numbers. */
    void flush() noexcept
    {
        if (std::abs (s1) < 1.0e-20f) s1 = 0.0f;
        if (std::abs (s2) < 1.0e-20f) s2 = 0.0f;
    }
};

/** One-pole smoothing at the control rate. */
struct Slew
{
    double v = 0.0;

    double next (double target, double coef, bool snap) noexcept
    {
        v = snap ? target : v + coef * (target - v);
        return v;
    }
};

/** The project's random numbers (same sequence in the JavaScript port). */
struct Lcg
{
    uint32_t state = 1u;

    double next01() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return (double) (state >> 8) * (1.0 / 16777216.0);
    }
};

//==============================================================================
/** Finds pick attacks: the peak level jumps well above what it has been lately (a follower that rises in
    10 ms and falls in 100 ms). All in double precision with hysteresis and a hold time, so the C++ and the
    JavaScript versions trigger on the same sample. */
struct AttackDetector
{
    void prepare (double fs)
    {
        fastRelease = 1.0 - 1.0 / (0.020 * fs);
        slowRise = 1.0 / (0.010 * fs);
        slowFall = 1.0 / (0.100 * fs);
        holdSamples = (int) std::floor (0.06 * fs);
        reset();
    }

    void reset() noexcept { fast = slow = 0.0; hold = 0; armed = true; }

    /** `a` = |input|. True on the sample where a new note starts. */
    bool process (double a) noexcept
    {
        fast = a > fast ? a : fast * fastRelease;
        if (fast < 1.0e-20) fast = 0.0;
        slow += (fast > slow ? slowRise : slowFall) * (fast - slow);
        if (slow < 1.0e-20) slow = 0.0;

        if (hold > 0)
        {
            --hold;
        }
        else if (armed)
        {
            if (fast > 0.01 && fast > 1.7 * slow)
            {
                armed = false;
                hold = holdSamples;
                return true;
            }
        }
        else if (fast < 1.25 * slow)
        {
            armed = true;
        }
        return false;
    }

    double fast = 0.0, slow = 0.0, fastRelease = 0.999, slowRise = 0.002, slowFall = 0.0002;
    int hold = 0, holdSamples = 2880;
    bool armed = true;
};

/** The playing level for the synths' amplifiers: the highest peak of the last 13.5 ms (one period of the
    lowest note), so it has no ripple at the note's own frequency and still rises at once. */
struct LevelMeter
{
    void prepare (int numBlocks) { ring.assign ((size_t) std::max (1, numBlocks), 0.0); reset(); }
    void reset() noexcept        { std::fill (ring.begin(), ring.end(), 0.0); pos = 0; blockMax = 0.0; }

    void push (double a) noexcept { if (a > blockMax) blockMax = a; }

    /** Once per control interval. */
    double endBlock() noexcept
    {
        ring[(size_t) pos] = blockMax;
        blockMax = 0.0;
        if (++pos >= (int) ring.size())
            pos = 0;

        double level = 0.0;
        for (size_t i = 0; i < ring.size(); ++i)
            if (ring[i] > level)
                level = ring[i];
        return level;
    }

    std::vector<double> ring;
    int pos = 0;
    double blockMax = 0.0;
};

//==============================================================================
/** Monophonic pitch tracker for the guitar synths.

    The input is low-passed and decimated twice: to about 16 kHz ("fine", band-limited to 2.5 kHz) and,
    averaging pairs, to about 8 kHz ("coarse", band-limited to 1 kHz). Every 3 ms the normalised square
    difference function (McLeod's NSDF) of the newest samples is computed: periods up to 4 ms (notes above
    250 Hz) on the fine signal, longer ones on the coarse signal, each over a window as long as the period
    itself but at least 8 ms (shorter windows mistake a repeating detail inside a low note's period for a
    high note). Silence in the window does not count, so a note is found after about two of its periods.
    The first clear peak gives the period; it is then refined on multiples of the period (up to 20 ms apart),
    which puts the oscillators within a cent of the string.

    A new pitch is only taken over when two analyses in a row agree (four for an octave jump away from a
    settled note, the usual tracking error), small changes (bends, vibrato) are followed at once, and a gate
    with hysteresis stops the tracking when nothing is played.

    Everything here is double precision and uses only + - * / and comparisons, and the analysis runs at
    fixed sample counts: the C++ (float audio) and the JavaScript (double audio) versions see the same
    input samples and so take exactly the same decisions. */
class PitchTracker
{
public:
    // The NSDF curve: index i = lag in fine samples up to fineLags - 1, then one point per coarse sample
    // (coarse lag i - fineLags / 2). Periods are counted in fine samples throughout.
    static constexpr int fineLags = 64, curveSize = 172, refineLag = 160, minLag = 9, maxKeys = 24;
    static constexpr int minWindow = 64; // in coarse samples: 8 ms
    static constexpr int fineWindow = 2 * minWindow + fineLags + 1, coarseWindow = 2 * refineLag, fineRingSize = 256, coarseRingSize = 512;
    static constexpr int hop = 48; // fine samples between two analyses
    static constexpr double openLevel = 0.006, closeLevel = 0.003, clarity = 0.7;

    void prepare (double fs)
    {
        decimation = std::max (1, (int) std::floor (fs / 16000.0 + 0.5));
        invDecimation = 1.0 / (double) decimation;
        rate = fs / (double) decimation;
        preCoef = onePole (2500.0, fs);
        lowCoef = onePole (1000.0, rate);
        dcCoef = onePole (45.0, rate);
        fineRing.assign ((size_t) fineRingSize, 0.0);
        coarseRing.assign ((size_t) coarseRingSize, 0.0);
        fineLin.assign ((size_t) fineWindow, 0.0);
        finePrefix.assign ((size_t) fineWindow + 1, 0.0);
        coarseLin.assign ((size_t) coarseWindow, 0.0);
        coarsePrefix.assign ((size_t) coarseWindow + 1, 0.0);
        curve.assign ((size_t) curveSize, 0.0);
        keyIndex.assign ((size_t) maxKeys, 0);
        keyValue.assign ((size_t) maxKeys, 0.0);
        reset();
    }

    void reset() noexcept
    {
        std::fill (fineRing.begin(), fineRing.end(), 0.0);
        std::fill (coarseRing.begin(), coarseRing.end(), 0.0);
        pre1 = pre2 = sum = dc = low1 = low2 = previous = 0.0;
        decimationCount = hopCount = finePos = coarsePos = 0;
        level = 0.0;
        gate = voiced = false;
        period = pendingPeriod = 120.0;
        pendingCount = steadyCount = 0;
        freq = rate / period;
    }

    void process (double x) noexcept
    {
        // Both signals are band-limited well below their sample rates (two poles each), so that the NSDF's
        // peaks are wide enough to be seen at whole lags whatever the string's brightness.
        pre1 += preCoef * (x - pre1) + 1.0e-25; // (the offset keeps the silent filters away from denormals)
        pre2 += preCoef * (pre1 - pre2);
        sum += pre2;
        if (++decimationCount < decimation)
            return;

        decimationCount = 0;
        const double decimated = sum * invDecimation;
        sum = 0.0;
        dc += dcCoef * (decimated - dc);
        const double fine = decimated - dc;
        fineRing[(size_t) finePos] = fine;
        finePos = (finePos + 1) & (fineRingSize - 1);

        low1 += lowCoef * (fine - low1) + 1.0e-25;
        low2 += lowCoef * (low1 - low2);
        if ((finePos & 1) == 0)
        {
            coarseRing[(size_t) coarsePos] = 0.5 * (low2 + previous);
            coarsePos = (coarsePos + 1) & (coarseRingSize - 1);
        }
        previous = low2;

        if (++hopCount < hop)
            return;

        hopCount = 0;
        analyse();
    }

    // set by the owner once per control interval
    double level = 0.0;

    // results
    bool gate = false, voiced = false;
    double freq = 110.0; // Hz, valid while `voiced`

private:
    static double onePole (double fc, double fs) noexcept
    {
        const double w = twoPi * fc / fs;
        return w / (1.0 + w);
    }

    /** NSDF at one lag over the newest samples (`lin`: newest first, `prefix`: their running energy):
        1 = the signal repeats exactly after `lag` samples. The window is one lag long, but not shorter
        than `minLength`. */
    static double nsdfAt (const std::vector<double>& lin, const std::vector<double>& prefix, int lag, int minLength) noexcept
    {
        const int length = lag >= minLength ? lag : minLength;
        const double* a = lin.data();
        const double* b = a + lag;

        // four running sums (the same four in the JavaScript port): about three times as fast as one
        double c0 = 0.0, c1 = 0.0, c2 = 0.0, c3 = 0.0;
        int j = 0;
        for (; j + 3 < length; j += 4)
        {
            c0 += a[j] * b[j];
            c1 += a[j + 1] * b[j + 1];
            c2 += a[j + 2] * b[j + 2];
            c3 += a[j + 3] * b[j + 3];
        }
        for (; j < length; ++j)
            c0 += a[j] * b[j];

        const double cross = (c0 + c1) + (c2 + c3);
        const double energy = prefix[(size_t) length] + prefix[(size_t) (length + lag)] - prefix[(size_t) lag];
        return energy > 1.0e-14 ? 2.0 * cross / energy : 0.0;
    }

    double fineNsdf (int lag) const noexcept   { return nsdfAt (fineLin, finePrefix, lag, 2 * minWindow); }
    double coarseNsdf (int lag) const noexcept { return nsdfAt (coarseLin, coarsePrefix, lag, minWindow); }

    void analyse() noexcept
    {
        if (gate)
        {
            if (level < closeLevel)
            {
                gate = voiced = false;
                pendingCount = steadyCount = 0;
            }
        }
        else if (level > openLevel)
        {
            gate = true;
        }

        if (! gate)
            return;

        // the newest samples first, and their running energy
        finePrefix[0] = 0.0;
        for (int i = 0; i < fineWindow; ++i)
        {
            const double v = fineRing[(size_t) ((finePos - 1 - i) & (fineRingSize - 1))];
            fineLin[(size_t) i] = v;
            finePrefix[(size_t) i + 1] = finePrefix[(size_t) i] + v * v;
        }

        coarsePrefix[0] = 0.0;
        for (int i = 0; i < coarseWindow; ++i)
        {
            const double v = coarseRing[(size_t) ((coarsePos - 1 - i) & (coarseRingSize - 1))];
            coarseLin[(size_t) i] = v;
            coarsePrefix[(size_t) i + 1] = coarsePrefix[(size_t) i] + v * v;
        }

        for (int i = 1; i < fineLags; ++i)
            curve[(size_t) i] = fineNsdf (i);
        for (int i = fineLags; i < curveSize; ++i)
            curve[(size_t) i] = coarseNsdf (i - fineLags / 2);
        // where the two lag grids meet, each side is compared with its own signal's next point
        const double aboveLastFine = fineNsdf (fineLags), belowFirstCoarse = coarseNsdf (fineLags / 2 - 1);

        // "key maxima": the highest point of each positive stretch after the curve first went negative
        int numKeys = 0, best = 0;
        double bestValue = 0.0, highest = 0.0;
        bool seenNegative = false, positive = false;

        for (int i = 1; i < curveSize - 1; ++i)
        {
            const double v = curve[(size_t) i];
            if (! seenNegative)
            {
                seenNegative = v < 0.0;
                continue;
            }

            if (v > 0.0)
            {
                if (! positive)
                {
                    positive = true;
                    best = 0;
                    bestValue = 0.0;
                }
                if (v > bestValue && v >= (i == fineLags ? belowFirstCoarse : curve[(size_t) i - 1])
                                  && v >= (i == fineLags - 1 ? aboveLastFine : curve[(size_t) i + 1]))
                {
                    bestValue = v;
                    best = i;
                }
            }

            if (positive && (v <= 0.0 || i == curveSize - 2))
            {
                positive = false;
                if (best >= minLag && numKeys < maxKeys)
                {
                    keyIndex[(size_t) numKeys] = best;
                    keyValue[(size_t) numKeys] = bestValue;
                    ++numKeys;
                    if (bestValue > highest)
                        highest = bestValue;
                }
            }
        }

        // the first one that is nearly as high as the highest: the shortest period that fits
        int chosen = -1;
        for (int i = 0; i < numKeys && chosen < 0; ++i)
            if (keyValue[(size_t) i] >= 0.85 * highest)
                chosen = i;

        if (chosen < 0 || keyValue[(size_t) chosen] < clarity)
        {
            pendingCount = 0;
            return;
        }

        // parabola through the peak ...
        const int index = keyIndex[(size_t) chosen];
        const double offset = peakOffset (index == fineLags ? belowFirstCoarse : curve[(size_t) index - 1], curve[(size_t) index],
                                          index == fineLags - 1 ? aboveLastFine : curve[(size_t) index + 1]);
        double found = index < fineLags ? (double) index + offset : 2.0 * ((double) (index - fineLags / 2) + offset);
        int multiple = 1;

        // ... then the same on multiples of the period for a finer reading
        for (;;)
        {
            const bool coarse = 2.0 * found >= (double) fineLags - 2.5;
            const int lag = (int) std::floor ((coarse ? found : 2.0 * found) + 0.5);
            if (coarse && lag + 2 > refineLag)
                break;

            const double peak = peakNear (coarse, lag);
            if (peak < 0.0)
                break;

            found = coarse ? 2.0 * peak : peak;
            multiple *= 2;
        }

        const double candidate = found / (double) multiple;

        if (voiced && std::abs (candidate - period) <= 0.06 * period)
        {
            period += 0.4 * (candidate - period); // the same note: follow it, a little smoothed
            pendingCount = 0;
            if (steadyCount < 1000)
                ++steadyCount;
        }
        else
        {
            pendingCount = (pendingCount > 0 && std::abs (candidate - pendingPeriod) <= 0.06 * pendingPeriod) ? pendingCount + 1 : 1;
            pendingPeriod = candidate;

            // a settled note needs more evidence before it jumps an octave (the usual tracking error)
            const double ratio = candidate / period;
            const bool octave = voiced && steadyCount > 8 && ((ratio > 1.88 && ratio < 2.12) || (ratio > 0.47 && ratio < 0.53));
            if (pendingCount >= (octave ? 4 : 2))
            {
                period = candidate;
                voiced = true;
                pendingCount = steadyCount = 0;
            }
        }

        freq = rate / period;
    }

    /** The NSDF peak at `lag` or one step beside it, on the fine or the coarse signal (in that signal's
        samples); -1 if there is no clear peak. */
    double peakNear (bool coarse, int lag) const noexcept
    {
        double before = coarse ? coarseNsdf (lag - 1) : fineNsdf (lag - 1);
        double at     = coarse ? coarseNsdf (lag)     : fineNsdf (lag);
        double after  = coarse ? coarseNsdf (lag + 1) : fineNsdf (lag + 1);

        if (after > at)
        {
            ++lag;
            before = at;
            at = after;
            after = coarse ? coarseNsdf (lag + 1) : fineNsdf (lag + 1);
        }
        else if (before > at)
        {
            --lag;
            after = at;
            at = before;
            before = coarse ? coarseNsdf (lag - 1) : fineNsdf (lag - 1);
        }

        if (at < clarity || at < before || at < after)
            return -1.0;

        return (double) lag + peakOffset (before, at, after);
    }

    static double peakOffset (double before, double at, double after) noexcept
    {
        const double bend = before - 2.0 * at + after;
        return bend < 0.0 ? std::clamp (0.5 * (before - after) / bend, -0.5, 0.5) : 0.0;
    }

    std::vector<double> fineRing, coarseRing, fineLin, finePrefix, coarseLin, coarsePrefix, curve, keyValue;
    std::vector<int> keyIndex;
    int decimation = 3, decimationCount = 0, hopCount = 0, finePos = 0, coarsePos = 0, pendingCount = 0, steadyCount = 0;
    double invDecimation = 1.0 / 3.0, rate = 16000.0, preCoef = 0.2, lowCoef = 0.3, dcCoef = 0.02;
    double pre1 = 0.0, pre2 = 0.0, sum = 0.0, dc = 0.0, low1 = 0.0, low2 = 0.0, previous = 0.0, period = 120.0, pendingPeriod = 120.0;
};

//==============================================================================
// Oscillators: `p` = phase 0..1, `dt` = phase step per sample. PolyBLEP / polyBLAMP round off the steps and
// corners, which keeps the aliasing of a naive oscillator out of the audible range.
inline double polyBlep (double t, double dt) noexcept
{
    if (t < dt)
    {
        const double x = t / dt;
        return x + x - x * x - 1.0;
    }
    if (t > 1.0 - dt)
    {
        const double x = (t - 1.0) / dt;
        return x * x + x + x + 1.0;
    }
    return 0.0;
}

inline double polyBlamp (double t, double dt) noexcept
{
    if (t < dt)
    {
        const double x = t / dt - 1.0;
        return -x * x * x / 3.0;
    }
    if (t > 1.0 - dt)
    {
        const double x = (t - 1.0) / dt + 1.0;
        return x * x * x / 3.0;
    }
    return 0.0;
}

inline double sawWave (double p, double dt) noexcept { return 2.0 * p - 1.0 - polyBlep (p, dt); }

/** Pulse of the given width (0..1), without DC, 2 from top to bottom. */
inline double pulseWave (double p, double dt, double width) noexcept
{
    double p2 = p + 1.0 - width;
    if (p2 >= 1.0)
        p2 -= 1.0;
    return sawWave (p, dt) - sawWave (p2, dt);
}

inline double triangleWave (double p, double dt) noexcept
{
    double p2 = p + 0.5;
    if (p2 >= 1.0)
        p2 -= 1.0;
    return 1.0 - 4.0 * std::abs (p - 0.5) + 8.0 * dt * (polyBlamp (p2, dt) - polyBlamp (p, dt));
}

} // namespace filter_detail

//==============================================================================
/** The HD500X's filter models (16 of 17: the Vocoder needs the hardware's microphone input).

    Filters: every knob change, switch and step is smoothed at a control rate of about 3 kHz and the filter
    coefficients are interpolated per sample, so nothing zippers or clicks. "TS" models filter the left and
    the right side separately (one control signal, from the mono sum, moves both); "ST/M" models make one
    mono effect signal and the Mix knob blends it into the untouched stereo input.

    Synths: a pitch tracker (see PitchTracker) drives band-limited oscillators; their amplifier follows the
    playing level and is closed when nothing is played.

    Knobs (the last one is always Mix):
      Voice Box     Speed, Start, End, Auto          V-Tron        Start, End, Speed, Mode
      Q Filter      Freq, Q, Gain, Type              Seeker        Speed, Freq (pattern), Q, Steps
      Obi Wah       Speed, Freq, Q, Type             Tron Up/Down  Freq, Q, Range, Type
      Throbber      Speed, Freq, Q, Wave             Slow Filter   Freq, Q, Speed, Mode
      Spin Cycle    Speed, Freq, Q, VolSens          Comet Trails  Speed, Freq, Q, Gain
      Octisynth     Speed, Freq, Q, Depth            Synth O Matic Freq, Q, Wave, Pitch
      Attack Synth  Freq, Wave, Speed, Pitch         Synth String  Speed, Freq, Attack, Pitch
      Growler       Speed, Freq, Q, Pitch */
class FilterFx
{
public:
    enum Variant { voiceBox = 0, vTron, qFilter, seeker, obiWah, tronUp, tronDown, throbber, slowFilter, spinCycle,
                   cometTrails, octisynth, synthOMatic, attackSynth, synthString, growler, numVariants };

    static constexpr int numComets = 7;

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        fs = sampleRate;
        ctl = 16 * std::max (1, (int) std::floor (fs / 48000.0 + 0.5));
        invCtl = 1.0f / (float) ctl;
        ctlTime = (double) ctl / fs;
        fcMax = std::min (16000.0, 0.42 * fs);
        c2 = slewCoef (0.002);
        c5 = slewCoef (0.005);
        c10 = slewCoef (0.010);
        c20 = slewCoef (0.020);
        c40 = slewCoef (0.040);
        c120 = slewCoef (0.120);
        envAttack = 1.0 / (1.0 + 0.004 * fs);
        envRelease = 1.0 / (1.0 + 0.120 * fs);
        attack.prepare (fs);
        tracker.prepare (fs);
        meter.prepare ((int) std::ceil (0.0135 / ctlTime));
        reset();
    }

    void reset()
    {
        using namespace filter_detail;
        first = true;
        ctlCount = 0;

        for (int i = 0; i < numComets; ++i)
        {
            svfL[i].reset();
            svfR[i].reset();
            coef[i] = SvfCoef();
        }
        for (auto& r : ramp) r = Ramp();
        for (auto& s : slew) s = Slew();
        mixRamp = Ramp();
        mixSlew = Slew();

        attack.reset();
        tracker.reset();
        meter.reset();
        lcg.state = 20260930u;
        randPrev = lcg.next01();
        randNext = lcg.next01();
        phase = 0.0;
        sweep = 1.0;
        env = 0.0;
        step = 0;
        ph1 = ph2 = 0.0;
        inc1 = inc2 = 0.0;
        oscHz = 110.0;
        width1 = width2 = 0.5;
        amp = fade = 0.0;
        wave = 0;
        wasVoiced = false;
    }

    void setModel (int newVariant) noexcept { variant = std::clamp (newVariant, 0, (int) numVariants - 1); }

    void setParameters (const float* k) noexcept
    {
        for (int i = 0; i < 5; ++i)
            knobs[i] = k[i];

        mixTarget = std::clamp (knobs[4] * 0.01f, 0.0f, 1.0f);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        switch (variant)
        {
            case voiceBox:    processVowel (left, right, numSamples, false); break;
            case vTron:       processVowel (left, right, numSamples, true); break;
            case qFilter:     processSingle (left, right, numSamples, true, false); break;
            case seeker:      processSingle (left, right, numSamples, true, false); break;
            case obiWah:      processSingle (left, right, numSamples, false, false); break;
            case tronUp:      processSingle (left, right, numSamples, true, true); break;
            case tronDown:    processSingle (left, right, numSamples, true, true); break;
            case throbber:    processLowPass (left, right, numSamples, false); break;
            case slowFilter:  processLowPass (left, right, numSamples, true); break;
            case spinCycle:   processSpin (left, right, numSamples); break;
            case cometTrails: processComet (left, right, numSamples); break;
            case octisynth:   processOcti (left, right, numSamples); break;
            default:          processSynth (left, right, numSamples); break;
        }
    }

private:
    using Svf = filter_detail::Svf;
    using SvfCoef = filter_detail::SvfCoef;
    using Ramp = filter_detail::Ramp;
    using Slew = filter_detail::Slew;

    /** Coefficient of a one-pole smoother that runs once per control interval. */
    double slewCoef (double seconds) const noexcept
    {
        const double x = ctlTime / seconds;
        return x / (1.0 + x);
    }

    double gOf (double hz) const noexcept { return std::tan (pi * std::clamp (hz, 20.0, fcMax) / fs); }

    /** True when a control interval starts: time for the model's control code. Moves the Mix knob on the way. */
    bool controlDue() noexcept
    {
        if (ctlCount > 0)
        {
            --ctlCount;
            return false;
        }
        ctlCount = ctl - 1;
        mixRamp.aim (mixSlew.next (mixTarget, c10, first), invCtl, first);
        return true;
    }

    //==============================================================================
    // Voice Box (talk box / vocal tract) and V-Tron (Voice Box triggered like a Mu-Tron).
    // Three parallel formant resonators (F1..F3 of the vowels A E I O U, alternating polarity like a parallel
    // formant synthesiser so there are no holes between the formants) morph from the Start to the End vowel:
    // frequencies along the log axis, levels linearly.
    //   Voice Box: the morph follows one of four automatic patterns at the Speed rate (Sine = there and back,
    //   Hold = rests on each vowel and glides quickly, Ramp = slowly to End and snaps back, Random = glides to
    //   a new random point between the vowels each cycle).
    //   V-Tron: every pick attack restarts a sweep that lasts 1 / Speed seconds: Up = Start -> End and stay,
    //   Up-Down = Start -> End -> Start.
    void controlVowel (bool snap) noexcept
    {
        using namespace filter_detail;
        int start = 0, end = 0;
        double pos = 0.0;

        if (variant == voiceBox)
        {
            start = choiceOf (knobs[1], 4);
            end = choiceOf (knobs[2], 4);
            phase += (double) knobs[0] * ctlTime;
            if (phase >= 1.0)
            {
                phase -= 1.0;
                randPrev = randNext;
                randNext = lcg.next01();
            }

            switch (choiceOf (knobs[3], 3))
            {
                case 0:  pos = 0.5 - 0.5 * std::cos (twoPi * phase); break;
                case 1:  pos = smooth01 ((0.7 - std::abs (2.0 * phase - 1.0)) * 2.5); break;
                case 2:  pos = phase < 0.85 ? phase / 0.85 : (1.0 - phase) / 0.15; break;
                default: pos = randPrev + (randNext - randPrev) * smooth01 (2.0 * phase); break;
            }
        }
        else
        {
            start = choiceOf (knobs[0], 4);
            end = choiceOf (knobs[1], 4);
            if (sweep < 1.0)
                sweep = std::min (1.0, sweep + (double) knobs[2] * ctlTime);

            pos = choiceOf (knobs[3], 1) == 0 ? smooth01 (sweep) : 0.5 - 0.5 * std::cos (twoPi * sweep);
        }

        pos = slew[0].next (pos, c5, snap);

        for (int j = 0; j < 3; ++j)
        {
            const double hz = slew[1 + j].next (vowelHz[start][j] * std::pow (vowelHz[end][j] / vowelHz[start][j], pos), c10, snap);
            const double gain = slew[4 + j].next (vowelGain[start][j] + (vowelGain[end][j] - vowelGain[start][j]) * pos, c10, snap);
            const double k = 1.0 / vowelQ[j];
            coef[j].aim (gOf (hz), k, invCtl, snap);
            ramp[j].aim ((j == 1 ? -vowelMakeUp : vowelMakeUp) * gain * k, invCtl, snap);
        }
    }

    void processVowel (float* left, float* right, int numSamples, bool triggered) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i], inR = right[i];
            const float mono = 0.5f * (inL + inR);

            if (triggered && attack.process (std::abs ((double) mono)))
                sweep = 0.0;

            if (controlDue())
            {
                controlVowel (first);
                first = false;
                for (int j = 0; j < 3; ++j) { svfL[j].flush(); svfR[j].flush(); }
            }

            coef[0].step();
            coef[1].step();
            coef[2].step();
            const float w0 = ramp[0].step(), w1 = ramp[1].step(), w2 = ramp[2].step();
            const float mixNow = mixRamp.step();

            if (triggered) // V-Tron: true stereo
            {
                svfL[0].process (inL, coef[0]);
                svfL[1].process (inL, coef[1]);
                svfL[2].process (inL, coef[2]);
                svfR[0].process (inR, coef[0]);
                svfR[1].process (inR, coef[1]);
                svfR[2].process (inR, coef[2]);
                const float wetL = filter_detail::softLimit (w0 * svfL[0].bp + w1 * svfL[1].bp + w2 * svfL[2].bp);
                const float wetR = filter_detail::softLimit (w0 * svfR[0].bp + w1 * svfR[1].bp + w2 * svfR[2].bp);
                left[i]  = inL + mixNow * (wetL - inL);
                right[i] = inR + mixNow * (wetR - inR);
            }
            else // Voice Box: mono effect
            {
                svfL[0].process (mono, coef[0]);
                svfL[1].process (mono, coef[1]);
                svfL[2].process (mono, coef[2]);
                const float wet = filter_detail::softLimit (w0 * svfL[0].bp + w1 * svfL[1].bp + w2 * svfL[2].bp);
                left[i]  = inL + mixNow * (wet - inL);
                right[i] = inR + mixNow * (wet - inR);
            }
        }
    }

    //==============================================================================
    // The models built on one two-pole state-variable filter with low-, band- and high-pass outputs.
    //   Q Filter: a parked wah. Freq 80 Hz .. 8 kHz, Q 0.5 .. 16, Gain in dB, LP / BP / HP.
    //   Seeker (Z.Vex Seek Wah): a sequencer steps a wah's band-pass through 2..9 parked positions at Speed
    //   steps per second; the Freq knob picks one of eight patterns. The steps glide over a few ms like the
    //   Seek Wah's lamp/LDR.
    //   Obi Wah (Oberheim VCF with sample & hold): at every clock tick (Speed) a random voltage is sampled and
    //   held: the filter jumps to a random frequency within 1.5 octaves either side of Freq.
    //   Tron Up / Tron Down (Mu-Tron III): an envelope follower (fast attack, slow lamp/LDR-like release)
    //   sweeps the state-variable filter up from, or down to, the frequency set by Freq and Range (Lo is a
    //   factor 2.5 lower, like the Mu-Tron's range capacitors); harder picking sweeps further, up to 3.5 octaves.
    void controlSingle (bool snap) noexcept
    {
        using namespace filter_detail;
        double hz = 1000.0, q = 1.0, gain = 1.0;
        int type = 1;

        switch (variant)
        {
            case qFilter:
                hz = 80.0 * std::pow (100.0, slew[0].next (knobs[0] * 0.01, c20, snap));
                q = 0.5 * std::pow (32.0, slew[1].next (knobs[1] * 0.01, c20, snap));
                gain = slew[2].next (std::pow (10.0, (double) knobs[2] * 0.05), c20, snap);
                type = choiceOf (knobs[3], 2);
                break;

            case seeker:
            {
                const int steps = 2 + choiceOf (knobs[3], 7);
                phase += (double) knobs[0] * ctlTime;
                if (phase >= 1.0)
                {
                    phase -= 1.0;
                    ++step;
                }
                if (step >= steps)
                    step = 0;

                hz = seekerHz (slew[0].next (seekerPosition (choiceOf (knobs[1], 7), step, steps), c5, snap));
                q = 1.5 * std::pow (10.0, slew[1].next (knobs[2] * 0.01, c20, snap));
                break;
            }

            case obiWah:
                phase += (double) knobs[0] * ctlTime;
                if (phase >= 1.0)
                {
                    phase -= 1.0;
                    randPrev = lcg.next01();
                }
                hz = 200.0 * std::pow (10.0, slew[3].next (knobs[1] * 0.01, c20, snap)) * std::pow (8.0, slew[0].next (randPrev - 0.5, c5, snap));
                q = 1.5 * std::pow (10.0, slew[1].next (knobs[2] * 0.01, c20, snap));
                type = choiceOf (knobs[3], 2);
                break;

            default: // Tron Up / Tron Down
            {
                const double drive = env / (env + 0.2); // 0..1: how hard the strings are picked
                const double bottom = slew[3].next (choiceOf (knobs[2], 1) == 0 ? 150.0 : 60.0, c20, snap)
                                        * std::pow (10.0, slew[0].next (knobs[0] * 0.01, c20, snap));
                hz = bottom * std::pow (2.0, 3.5 * (variant == tronUp ? drive : 1.0 - drive));
                q = 0.7 * std::pow (20.0, slew[1].next (knobs[1] * 0.01, c20, snap));
                type = choiceOf (knobs[3], 2);
                break;
            }
        }

        const double k = 1.0 / q;
        coef[0].aim (gOf (hz), k, invCtl, snap);
        ramp[0].aim (slew[4].next (type == 0 ? 1.0 : 0.0, c10, snap) * lowPassNorm (k) * gain, invCtl, snap);
        ramp[1].aim (slew[5].next (type == 1 ? 1.0 : 0.0, c10, snap) * bandPassNorm (k) * gain, invCtl, snap);
        ramp[2].aim (slew[6].next (type == 2 ? 1.0 : 0.0, c10, snap) * lowPassNorm (k) * gain, invCtl, snap);
    }

    void processSingle (float* left, float* right, int numSamples, bool stereo, bool follow) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i], inR = right[i];
            const float mono = 0.5f * (inL + inR);

            if (follow)
            {
                const double a = std::abs ((double) mono);
                env += (a > env ? envAttack : envRelease) * (a - env);
            }

            if (controlDue())
            {
                controlSingle (first);
                first = false;
                svfL[0].flush();
                svfR[0].flush();
                if (env < 1.0e-20) env = 0.0;
            }

            coef[0].step();
            const float wl = ramp[0].step(), wb = ramp[1].step(), wh = ramp[2].step();
            const float mixNow = mixRamp.step();

            if (stereo)
            {
                svfL[0].process (inL, coef[0]);
                svfR[0].process (inR, coef[0]);
                const float wetL = filter_detail::softLimit (wl * svfL[0].lp + wb * svfL[0].bp + wh * svfL[0].hp);
                const float wetR = filter_detail::softLimit (wl * svfR[0].lp + wb * svfR[0].bp + wh * svfR[0].hp);
                left[i]  = inL + mixNow * (wetL - inL);
                right[i] = inR + mixNow * (wetR - inR);
            }
            else
            {
                svfL[0].process (mono, coef[0]);
                const float wet = filter_detail::softLimit (wl * svfL[0].lp + wb * svfL[0].bp + wh * svfL[0].hp);
                left[i]  = inL + mixNow * (wet - inL);
                right[i] = inR + mixNow * (wet - inR);
            }
        }
    }

    //==============================================================================
    // The two four-pole low-pass models (a resonant two-pole stage and a plain one at the same frequency).
    //   Throbber (Electrix Filter Factory's LFO section): an LFO with four shapes (ramp up, ramp down, triangle,
    //   square) moves the cutoff 1.5 octaves either side of Freq.
    //   Slow Filter: every pick attack restarts a sweep of 1 / Speed seconds between Freq (the dark end,
    //   100 Hz .. 2 kHz) and fully open (10 kHz): Up = dark -> bright, Down = bright -> dark.
    void controlLowPass (bool snap) noexcept
    {
        using namespace filter_detail;
        double hz = 1000.0;

        if (variant == throbber)
        {
            phase += (double) knobs[0] * ctlTime;
            if (phase >= 1.0)
                phase -= 1.0;

            double lfo = 0.0;
            switch (choiceOf (knobs[3], 3))
            {
                case 0:  lfo = 2.0 * phase - 1.0; break;
                case 1:  lfo = 1.0 - 2.0 * phase; break;
                case 2:  lfo = 1.0 - 4.0 * std::abs (phase - 0.5); break;
                default: lfo = phase < 0.5 ? 1.0 : -1.0; break;
            }

            hz = 150.0 * std::pow (20.0, slew[0].next (knobs[1] * 0.01, c20, snap)) * std::pow (2.0, 1.5 * slew[2].next (lfo, c5, snap));
        }
        else
        {
            if (sweep < 1.0)
                sweep = std::min (1.0, sweep + (double) knobs[2] * ctlTime);

            const double pos = slew[2].next (sweep, c10, snap);
            const double up = slew[3].next (choiceOf (knobs[3], 1) == 0 ? 1.0 : 0.0, c20, snap);
            const double dark = 100.0 * std::pow (20.0, slew[0].next (knobs[0] * 0.01, c20, snap));
            hz = dark * std::pow (10000.0 / dark, up * pos + (1.0 - up) * (1.0 - pos));
        }

        const double k = 1.0 / (0.7 * std::pow (16.0, slew[1].next (knobs[variant == throbber ? 2 : 1] * 0.01, c20, snap)));
        const double g = gOf (hz);
        coef[0].aim (g, k, invCtl, snap);
        coef[1].aim (g, 1.4142, invCtl, snap);
        ramp[0].aim (lowPassNorm (k), invCtl, snap);
    }

    void processLowPass (float* left, float* right, int numSamples, bool triggered) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i], inR = right[i];

            if (triggered && attack.process (std::abs ((double) (0.5f * (inL + inR)))))
                sweep = 0.0;

            if (controlDue())
            {
                controlLowPass (first);
                first = false;
                for (int j = 0; j < 2; ++j) { svfL[j].flush(); svfR[j].flush(); }
            }

            coef[0].step();
            coef[1].step();
            const float gain = ramp[0].step();
            const float mixNow = mixRamp.step();

            svfL[0].process (inL, coef[0]);
            svfL[1].process (svfL[0].lp, coef[1]);
            svfR[0].process (inR, coef[0]);
            svfR[1].process (svfR[0].lp, coef[1]);
            const float wetL = filter_detail::softLimit (gain * svfL[1].lp);
            const float wetR = filter_detail::softLimit (gain * svfR[1].lp);
            left[i]  = inL + mixNow * (wetL - inL);
            right[i] = inR + mixNow * (wetR - inR);
        }
    }

    //==============================================================================
    // Spin Cycle (Craig Anderton's Wah/Anti-Wah): two wah band-passes, one on the left and one on the right,
    // swept by one LFO in opposite directions (1.2 octaves either side of Freq). VolSens speeds the LFO up
    // while the strings are hit hard (up to 7 times at full VolSens).
    void controlSpin (bool snap) noexcept
    {
        using namespace filter_detail;
        phase += (double) knobs[0] * (1.0 + 0.06 * (double) knobs[3] * env / (env + 0.1)) * ctlTime;
        if (phase >= 1.0)
            phase -= 1.0;

        const double lfo = 1.2 * std::sin (twoPi * phase);
        const double centre = 300.0 * std::pow (9.0, slew[0].next (knobs[1] * 0.01, c20, snap));
        const double k = 1.0 / (1.5 * std::pow (10.0, slew[1].next (knobs[2] * 0.01, c20, snap)));
        coef[0].aim (gOf (centre * std::pow (2.0, lfo)), k, invCtl, snap);
        coef[1].aim (gOf (centre * std::pow (2.0, -lfo)), k, invCtl, snap);
        ramp[0].aim (bandPassNorm (k), invCtl, snap);
    }

    void processSpin (float* left, float* right, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i], inR = right[i];
            const double a = std::abs ((double) (0.5f * (inL + inR)));
            env += (a > env ? envAttack : envRelease) * (a - env);

            if (controlDue())
            {
                controlSpin (first);
                first = false;
                svfL[0].flush();
                svfR[0].flush();
                if (env < 1.0e-20) env = 0.0;
            }

            coef[0].step();
            coef[1].step();
            const float gain = ramp[0].step();
            const float mixNow = mixRamp.step();

            svfL[0].process (inL, coef[0]);
            svfR[0].process (inR, coef[1]);
            const float wetL = filter_detail::softLimit (gain * svfL[0].bp);
            const float wetR = filter_detail::softLimit (gain * svfR[0].bp);
            left[i]  = inL + mixNow * (wetL - inL);
            right[i] = inR + mixNow * (wetR - inR);
        }
    }

    //==============================================================================
    // Comet Trails (Line 6 original): seven band-passes chase each other along the same sine sweep (1.5 octaves
    // either side of Freq), each 6 % of a cycle behind the one in front and a little quieter: a head with a
    // tail. Odd and even filters lean to opposite sides, which spreads the tail across the stereo field.
    static constexpr float cometLeft[numComets]  = { 1.0f, 0.43f, 0.72f, 0.31f, 0.52f, 0.22f, 0.38f };
    static constexpr float cometRight[numComets] = { 0.5f, 0.85f, 0.36f, 0.61f, 0.26f, 0.44f, 0.19f };

    void controlComet (bool snap) noexcept
    {
        using namespace filter_detail;
        phase += (double) knobs[0] * ctlTime;
        if (phase >= 1.0)
            phase -= 1.0;

        const double centre = 300.0 * std::pow (10.0, slew[0].next (knobs[1] * 0.01, c20, snap));
        const double k = 1.0 / (2.0 * std::pow (10.0, slew[1].next (knobs[2] * 0.01, c20, snap)));
        for (int j = 0; j < numComets; ++j)
            coef[j].aim (gOf (centre * std::pow (2.0, 1.5 * std::sin (twoPi * (phase - 0.06 * (double) j)))), k, invCtl, snap);

        ramp[0].aim (cometMakeUp * bandPassNorm (k) * slew[2].next (std::pow (10.0, (double) knobs[3] * 0.05), c20, snap), invCtl, snap);
    }

    void processComet (float* left, float* right, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i], inR = right[i];

            if (controlDue())
            {
                controlComet (first);
                first = false;
                for (int j = 0; j < numComets; ++j) { svfL[j].flush(); svfR[j].flush(); }
            }

            float sumL = 0.0f, sumR = 0.0f;
            for (int j = 0; j < numComets; ++j)
            {
                coef[j].step();
                svfL[j].process (inL, coef[j]);
                svfR[j].process (inR, coef[j]);
                sumL += cometLeft[j] * svfL[j].bp;
                sumR += cometRight[j] * svfR[j].bp;
            }

            const float gain = ramp[0].step();
            const float mixNow = mixRamp.step();
            const float wetL = filter_detail::softLimit (gain * sumL);
            const float wetR = filter_detail::softLimit (gain * sumR);
            left[i]  = inL + mixNow * (wetL - inL);
            right[i] = inR + mixNow * (wetR - inR);
        }
    }

    //==============================================================================
    // Octisynth (Line 6 original): no pitch tracking. The playing level sets the frequency of an oscillator
    // (80 Hz when the note has died away, about 1 kHz on a hard attack, so every note swoops down as it
    // decays); the guitar is ring-modulated with it and the oscillator itself is mixed in at the playing
    // level. Speed / Depth: vibrato of the oscillator (up to half an octave either way). Freq adds the
    // oscillator's second harmonic. A resonant low-pass follows the oscillator at twice its frequency (Q).
    void controlOcti (bool snap) noexcept
    {
        using namespace filter_detail;
        phase += (double) knobs[0] * ctlTime;
        if (phase >= 1.0)
            phase -= 1.0;

        const double drive = env / (env + 0.1);
        const double hz = 80.0 * std::pow (2.0, 4.5 * drive + 0.005 * (double) knobs[3] * std::sin (twoPi * phase));
        inc1 = std::min (0.2, hz / fs);

        const double k = 1.0 / (0.7 * std::pow (16.0, slew[1].next (knobs[2] * 0.01, c20, snap)));
        coef[0].aim (gOf (2.0 * hz), k, invCtl, snap);
        ramp[0].aim (octiMakeUp * lowPassNorm (k) * drive * smooth01 ((env - 0.002) / 0.006), invCtl, snap); // the oscillator's level
        ramp[1].aim (slew[0].next (knobs[1] * 0.01, c20, snap), invCtl, snap);                                 // second harmonic
        ramp[2].aim (octiMakeUp * lowPassNorm (k) * 6.0, invCtl, snap);                                        // ring modulator
    }

    void processOcti (float* left, float* right, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i], inR = right[i];
            const float mono = 0.5f * (inL + inR);
            const double a = std::abs ((double) mono);
            env += (a > env ? envAttack : envRelease) * (a - env);

            if (controlDue())
            {
                controlOcti (first);
                first = false;
                svfL[0].flush();
                if (env < 1.0e-20) env = 0.0;
            }

            ph1 += inc1;
            if (ph1 >= 1.0)
                ph1 -= 1.0;

            const float level = ramp[0].step(), second = ramp[1].step(), ring = ramp[2].step();
            const float osc = (float) std::sin (filter_detail::twoPi * ph1) + second * (float) std::sin (2.0 * filter_detail::twoPi * ph1);
            coef[0].step();
            svfL[0].process (osc * (level + ring * mono), coef[0]);

            const float wet = filter_detail::softLimit (svfL[0].lp);
            const float mixNow = mixRamp.step();
            left[i]  = inL + mixNow * (wet - inL);
            right[i] = inR + mixNow * (wet - inR);
        }
    }

    //==============================================================================
    // The four pitch-tracked synths. Oscillators at the played pitch (times the Pitch knob, +-12 semitones),
    // gated by the playing level.
    //   Synth O Matic (Moog modular / Oberheim SEM waveforms): one of eight oscillator set-ups (saw, square,
    //   narrow pulse, triangle, saw + square, two detuned saws, saw + octave, square + sub-octave) into a
    //   resonant two-pole low-pass (Freq 100 Hz .. 8 kHz, Q). The amplifier follows the guitar's envelope.
    //   Attack Synth (Korg X911): square, pulse-width-modulated or ramp wave into a four-pole low-pass that
    //   opens on every pick attack from 90 Hz to the stop frequency (Freq); Speed is how fast (1.5 s .. 5 ms).
    //   Synth String (Roland GR-700 strings): two pulse waves 7 cents apart, their widths swept by an LFO at the
    //   Speed rate (a moving pulse edge is a moving pitch: the ensemble "vibrato" of a string machine), a
    //   low-pass for the tone (Freq) and a slow amplifier attack (Attack, 5 ms .. 2 s) with a pad-like release.
    //   Growler (GR-700 tone through a Mu-Tron III): pulse (width swept at Speed) plus saw into a resonant
    //   low-pass that an envelope follower opens up to 4 octaves above Freq.
    void controlSynth (bool snap) noexcept
    {
        using namespace filter_detail;
        const double level = meter.endBlock();
        tracker.level = level;

        const bool voiced = tracker.voiced;
        const double target = tracker.freq * std::pow (2.0, (double) knobs[3] / 12.0);
        if (voiced)
            oscHz = (snap || ! wasVoiced) ? target : oscHz + c5 * (target - oscHz);
        wasVoiced = voiced;

        const double open = smooth01 ((level - 0.003) / 0.009); // fades out before the tracker's gate closes
        double wanted = voiced ? open * level / (level + 0.1) : 0.0, rise = c2, fall = c40, gain = 1.0;
        int waveKnob = -1;

        switch (variant)
        {
            case synthOMatic:
            {
                const double k = 1.0 / (0.7 * std::pow (16.0, slew[1].next (knobs[1] * 0.01, c20, snap)));
                coef[0].aim (gOf (100.0 * std::pow (80.0, slew[0].next (knobs[0] * 0.01, c20, snap))), k, invCtl, snap);
                gain = synthMakeUp * lowPassNorm (k);
                waveKnob = choiceOf (knobs[2], 7);
                break;
            }

            case attackSynth:
            {
                if (! voiced)
                    sweep = 0.0;
                else if (sweep < 1.0)
                    sweep = std::min (1.0, sweep + ctlTime / (1.5 * std::pow (1.0 / 300.0, (double) knobs[2] * 0.01)));

                const double stop = 200.0 * std::pow (40.0, slew[0].next (knobs[0] * 0.01, c20, snap));
                const double g = gOf (90.0 * std::pow (stop / 90.0, slew[1].next (sweep, c5, snap)));
                coef[0].aim (g, 0.4, invCtl, snap);
                coef[1].aim (g, 1.4142, invCtl, snap);
                gain = attackMakeUp;
                waveKnob = choiceOf (knobs[1], 2);

                phase += 0.8 * ctlTime;
                if (phase >= 1.0)
                    phase -= 1.0;
                width1 = waveKnob == 1 ? 0.5 + 0.4 * std::sin (twoPi * phase) : 0.5;
                break;
            }

            case synthString:
            {
                phase += (double) knobs[0] * ctlTime;
                if (phase >= 1.0)
                    phase -= 1.0;

                width1 = 0.5 + 0.38 * std::sin (twoPi * phase);
                width2 = 0.5 + 0.38 * std::sin (twoPi * phase + 1.9);
                coef[0].aim (gOf (300.0 * std::pow (27.0, slew[0].next (knobs[1] * 0.01, c20, snap))), 1.25, invCtl, snap);

                const double x = ctlTime / (0.005 * std::pow (400.0, (double) knobs[2] * 0.01));
                rise = x / (1.0 + x);
                fall = c120;
                wanted = voiced ? open * level / (level + 0.04) : 0.0;
                gain = stringMakeUp;
                break;
            }

            default: // Growler
            {
                phase += (double) knobs[0] * ctlTime;
                if (phase >= 1.0)
                    phase -= 1.0;

                width1 = 0.5 + 0.42 * std::sin (twoPi * phase);
                const double k = 1.0 / (0.7 * std::pow (20.0, slew[1].next (knobs[2] * 0.01, c20, snap)));
                coef[0].aim (gOf (80.0 * std::pow (10.0, slew[0].next (knobs[1] * 0.01, c20, snap)) * std::pow (16.0, env / (env + 0.1))), k, invCtl, snap);
                gain = growlerMakeUp * lowPassNorm (k);
                break;
            }
        }

        // a wave switch fades the oscillator out, changes it and fades back in
        if (waveKnob >= 0)
        {
            if (snap)                  { wave = waveKnob; fade = 1.0; }
            else if (waveKnob != wave) { fade += c2 * (0.0 - fade); if (fade < 0.003) wave = waveKnob; }
            else                       fade += c2 * (1.0 - fade);
            gain *= fade;
        }

        amp = snap ? wanted : amp + (wanted > amp ? rise : fall) * (wanted - amp);
        if (amp < 1.0e-20) amp = 0.0;
        ramp[0].aim (amp * gain, invCtl, snap);

        inc1 = std::min (0.2, oscHz / fs);
        inc2 = inc1;
        if (variant == synthOMatic)      inc2 = wave == 5 ? 1.007 * inc1 : 0.5 * inc1;
        else if (variant == synthString) { inc1 *= 0.998; inc2 *= 1.002; }
    }

    void processSynth (float* left, float* right, int numSamples) noexcept
    {
        using namespace filter_detail;

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i], inR = right[i];
            const float mono = 0.5f * (inL + inR);
            const double a = std::abs ((double) mono);
            tracker.process ((double) mono);
            meter.push (a);

            if (variant == attackSynth)
            {
                if (attack.process (a))
                    sweep = 0.0;
            }
            else if (variant == growler)
            {
                env += (a > env ? envAttack : envRelease) * (a - env);
            }

            if (controlDue())
            {
                controlSynth (first);
                first = false;
                svfL[0].flush();
                svfL[1].flush();
                if (env < 1.0e-20) env = 0.0;
            }

            ph1 += inc1;
            if (ph1 >= 1.0) ph1 -= 1.0;
            ph2 += inc2;
            if (ph2 >= 1.0) ph2 -= 1.0;

            double osc = 0.0;
            if (variant == synthOMatic)
            {
                switch (wave)
                {
                    case 0:  osc = sawWave (ph1, inc1); break;
                    case 1:  osc = pulseWave (ph1, inc1, 0.5); break;
                    case 2:  osc = pulseWave (ph1, inc1, 0.2); break;
                    case 3:  osc = 1.6 * triangleWave (ph1, inc1); break;
                    case 4:  osc = 0.6 * (sawWave (ph1, inc1) + pulseWave (ph1, inc1, 0.5)); break;
                    case 5:  osc = 0.6 * (sawWave (ph1, inc1) + sawWave (ph2, inc2)); break;
                    case 6:
                    {
                        double octave = ph1 + ph1;
                        if (octave >= 1.0) octave -= 1.0;
                        osc = 0.65 * sawWave (ph1, inc1) + 0.5 * sawWave (octave, inc1 + inc1);
                        break;
                    }
                    default: osc = 0.6 * (pulseWave (ph1, inc1, 0.5) + pulseWave (ph2, inc2, 0.5)); break;
                }
            }
            else if (variant == attackSynth)
            {
                osc = wave == 2 ? sawWave (ph1, inc1) : pulseWave (ph1, inc1, width1);
            }
            else if (variant == synthString)
            {
                osc = 0.6 * (pulseWave (ph1, inc1, width1) + pulseWave (ph2, inc2, width2));
            }
            else
            {
                osc = 0.6 * pulseWave (ph1, inc1, width1) + 0.5 * sawWave (ph1, inc1);
            }

            coef[0].step();
            svfL[0].process ((float) osc * ramp[0].step(), coef[0]);
            float out = svfL[0].lp;

            if (variant == attackSynth)
            {
                coef[1].step();
                svfL[1].process (out, coef[1]);
                out = svfL[1].lp;
            }

            const float wet = softLimit (out);
            const float mixNow = mixRamp.step();
            left[i]  = inL + mixNow * (wet - inL);
            right[i] = inR + mixNow * (wet - inR);
        }
    }

    //==============================================================================
    // make-up gains: every model about as loud as its input at the default knobs
    static constexpr double vowelMakeUp = 3.0, cometMakeUp = 0.7, octiMakeUp = 0.15, synthMakeUp = 0.35,
                            attackMakeUp = 0.17, stringMakeUp = 0.22, growlerMakeUp = 0.28;

    double fs = 48000.0, ctlTime = 16.0 / 48000.0, fcMax = 16000.0;
    double c2 = 0.1, c5 = 0.05, c10 = 0.03, c20 = 0.02, c40 = 0.01, c120 = 0.003, envAttack = 0.005, envRelease = 0.0002;
    int variant = voiceBox, ctl = 16, ctlCount = 0;
    float invCtl = 1.0f / 16.0f;
    bool first = true;
    float knobs[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 100.0f }, mixTarget = 1.0f;

    Svf svfL[numComets], svfR[numComets];
    SvfCoef coef[numComets];
    Ramp ramp[3], mixRamp;
    Slew slew[7], mixSlew;

    filter_detail::AttackDetector attack;
    filter_detail::PitchTracker tracker;
    filter_detail::LevelMeter meter;
    filter_detail::Lcg lcg;

    double phase = 0.0, sweep = 1.0, env = 0.0, randPrev = 0.0, randNext = 0.0;
    double ph1 = 0.0, ph2 = 0.0, inc1 = 0.0, inc2 = 0.0, oscHz = 110.0, width1 = 0.5, width2 = 0.5, amp = 0.0, fade = 0.0;
    int step = 0, wave = 0;
    bool wasVoiced = false;
};

/** In the order of FilterFx::Variant. */
inline std::vector<ModelInfo> filterModels()
{
    const auto speed = [] (float def) { return hertz ("Speed", 0.05f, 10.0f, def, 1.0f); };
    const auto mix   = [] { return percent ("Mix", 100.0f); };
    const auto pitch = [] { return semitones ("Pitch", -12.0f, 12.0f, 0.0f); };
    const auto type  = [] (int def) { return choice ("Type", filterTypeNames, 3, def); };
    const auto make  = [] (const char* key, const char* name, int variant, const char* basedOn, std::vector<KnobSpec> knobs)
    {
        return ModelInfo { key, name, Category::filter, Engine::filterFx, variant, basedOn, std::move (knobs) };
    };

    return {
        make ("voice_box", "Voice Box", FilterFx::voiceBox, "Talk box: vocoders, vocal tracts and surgical tubing",
              { speed (1.5f), choice ("Start", filterVowelNames, 5, 0), choice ("End", filterVowelNames, 5, 2),
                choice ("Auto", filterAutoNames, 4, 0), mix() }),
        make ("v_tron", "V-Tron", FilterFx::vTron, "Voice Box triggered like a Mu-Tron III",
              { choice ("Start", filterVowelNames, 5, 4), choice ("End", filterVowelNames, 5, 0),
                hertz ("Speed", 0.2f, 20.0f, 5.0f, 3.0f), choice ("Mode", filterSweepNames, 2, 0), mix() }),
        make ("q_filter", "Q Filter", FilterFx::qFilter, "Parked wah",
              { percent ("Freq", 50.0f), percent ("Q", 60.0f), decibels ("Gain", -12.0f, 12.0f, 0.0f), type (1), mix() }),
        make ("seeker", "Seeker", FilterFx::seeker, "Z.Vex Seek Wah",
              { hertz ("Speed", 0.5f, 20.0f, 6.0f, 5.0f), choice ("Freq", filterPatternNames, 8, 3), percent ("Q", 60.0f),
                choice ("Steps", filterStepNames, 8, 6), mix() }),
        make ("obi_wah", "Obi Wah", FilterFx::obiWah, "Oberheim voltage-controlled sample & hold filter",
              { hertz ("Speed", 0.5f, 20.0f, 6.0f, 5.0f), percent ("Freq", 50.0f), percent ("Q", 60.0f), type (1), mix() }),
        make ("tron_up", "Tron Up", FilterFx::tronUp, "Mu-Tron III, drive switch up",
              { percent ("Freq", 30.0f), percent ("Q", 60.0f), choice ("Range", filterRangeNames, 2, 0), type (1), mix() }),
        make ("tron_down", "Tron Down", FilterFx::tronDown, "Mu-Tron III, drive switch down",
              { percent ("Freq", 30.0f), percent ("Q", 60.0f), choice ("Range", filterRangeNames, 2, 0), type (1), mix() }),
        make ("throbber", "Throbber", FilterFx::throbber, "Electrix Filter Factory, LFO section",
              { speed (2.0f), percent ("Freq", 50.0f), percent ("Q", 50.0f), choice ("Wave", filterLfoNames, 4, 2), mix() }),
        make ("slow_filter", "Slow Filter", FilterFx::slowFilter, "Line 6 original: attack-triggered low-pass sweep",
              { percent ("Freq", 30.0f), percent ("Q", 35.0f), hertz ("Speed", 0.1f, 10.0f, 1.5f, 1.5f),
                choice ("Mode", filterModeNames, 2, 0), mix() }),
        make ("spin_cycle", "Spin Cycle", FilterFx::spinCycle, "Craig Anderton's Wah/Anti-Wah",
              { speed (1.0f), percent ("Freq", 50.0f), percent ("Q", 60.0f), percent ("VolSens", 30.0f), mix() }),
        make ("comet_trails", "Comet Trails", FilterFx::cometTrails, "Line 6 original: seven filters chasing each other",
              { speed (0.5f), percent ("Freq", 50.0f), percent ("Q", 70.0f), decibels ("Gain", -12.0f, 12.0f, 0.0f), mix() }),
        make ("octisynth", "Octisynth", FilterFx::octisynth, "Line 6 original: level-controlled oscillator, ring modulator and vibrato",
              { hertz ("Speed", 0.1f, 15.0f, 5.0f, 3.0f), percent ("Freq", 40.0f), percent ("Q", 50.0f), percent ("Depth", 30.0f), mix() }),
        make ("synth_o_matic", "Synth O Matic", FilterFx::synthOMatic, "Moog modular and Oberheim SEM waveforms",
              { percent ("Freq", 60.0f), percent ("Q", 45.0f), choice ("Wave", filterSynthWaveNames, 8, 0), pitch(), mix() }),
        make ("attack_synth", "Attack Synth", FilterFx::attackSynth, "Korg X911 guitar synthesizer",
              { percent ("Freq", 65.0f), choice ("Wave", filterAttackWaveNames, 3, 0), percent ("Speed", 50.0f), pitch(), mix() }),
        make ("synth_string", "Synth String", FilterFx::synthString, "Roland GR-700 guitar synthesizer (strings)",
              { speed (3.0f), percent ("Freq", 60.0f), percent ("Attack", 50.0f), pitch(), mix() }),
        make ("growler", "Growler", FilterFx::growler, "Roland GR-700 tone into a Mu-Tron III",
              { speed (1.5f), percent ("Freq", 35.0f), percent ("Q", 65.0f), pitch(), mix() }),
    };
}

} // namespace fx
