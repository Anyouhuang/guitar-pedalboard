#pragma once

#include "../DspUtils.h"
#include "../ModelTypes.h"

namespace fx
{
/** The HD500X's Pitch models (3). Mono: the input is collapsed to mono and written to both sides.

    Knobs:  Bass Octaver:  Tone, Normal (dry level), Octave (octave-down level)
            Pitch Glide:   Pitch (semitones, -24..+24), Mix
            Smart Harmony: Key, Shift (diatonic interval), Scale, Mix

    Everything here works in the time domain so it can be played live:
      - one tracker (a normalised autocorrelation of the input, decimated to about 8 kHz) follows the played
        note, finds the lag at which the signal repeats best, and reports pick attacks;
      - the pitch shifter is a delay line read at another speed; whenever its read head has to jump it jumps by
        that lag (whole periods of a single note, the best-matching lag of a chord) with a short crossfade, so
        the two heads are in phase and the level stays steady; at a pick attack it goes to just before the
        attack, so that attacks are heard within a few ms, once;
      - the octaver needs no delay at all: like the analog original it flips the polarity of the filtered
        fundamental every other cycle.

    C++ / JavaScript parity: every value that leads to a DECISION (tracker filters, autocorrelation, the
    octaver's filters and comparator, read-head positions, pitch ratios) is kept in `double` and computed with
    + - * / only, in the same order as web/src/dsp/pitch.js, so both versions take bit-identical decisions.
    Only the audio itself (delay-line interpolation, anti-alias filter, mixing) is `float` here. */
namespace pitch_detail
{
inline const char* const pitchKeyNames[]   = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
inline const char* const pitchShiftNames[] = { "-8th", "-7th", "-6th", "-5th", "-4th", "-3rd", "-2nd",
                                               "+2nd", "+3rd", "+4th", "+5th", "+6th", "+7th", "+8th" };
inline const char* const pitchScaleNames[] = { "Major", "Minor", "Pent Major", "Pent Minor", "Harm Minor", "Mel Minor",
                                               "Whole Tone", "Diminished" };
inline constexpr int numKeys = 12, numShifts = 14, numScales = 8;

/** The interval number of each Shift choice (-3 = a third below, 8 = an octave above). */
inline constexpr int shiftIntervals[numShifts] = { -8, -7, -6, -5, -4, -3, -2, 2, 3, 4, 5, 6, 7, 8 };
inline constexpr int scaleSizes[numScales]     = { 7, 7, 5, 5, 7, 7, 6, 8 };
inline constexpr int scaleNotes[numScales][8]  = { { 0, 2, 4, 5, 7, 9, 11, 0 },   // major
                                                   { 0, 2, 3, 5, 7, 8, 10, 0 },   // natural minor
                                                   { 0, 2, 4, 7, 9, 0, 0, 0 },    // major pentatonic
                                                   { 0, 3, 5, 7, 10, 0, 0, 0 },   // minor pentatonic
                                                   { 0, 2, 3, 5, 7, 8, 11, 0 },   // harmonic minor
                                                   { 0, 2, 3, 5, 7, 9, 11, 0 },   // melodic minor (ascending)
                                                   { 0, 2, 4, 6, 8, 10, 0, 0 },   // whole tone
                                                   { 0, 2, 3, 5, 6, 8, 9, 11 } }; // diminished (whole-half)

/** Semitones to shift a played note by so that the harmony stays inside the scale.
    `pitchClass` is relative to the key (0 = the key note), `interval` is one of shiftIntervals.
    Seven-note scales count scale steps (a true diatonic interval). The other scales have no "third" on every
    degree, so they take the scale note whose distance is nearest to the interval (major / perfect first).
    A note outside the scale is shifted like the nearest scale note (the lower one if two are equally near). */
inline int harmonySemitones (int scale, int interval, int pitchClass) noexcept
{
    const int* notes = scaleNotes[scale];
    const int count = scaleSizes[scale];

    auto degreeOf = [notes, count] (int pc) noexcept
    {
        for (int d = 0; d < count; ++d)
            if (notes[d] == pc)
                return d;
        return -1;
    };

    int pc = pitchClass, degree = degreeOf (pc);
    for (int distance = 1; degree < 0 && distance <= 6; ++distance)
    {
        pc = (pitchClass + 12 - distance) % 12;
        degree = degreeOf (pc);
        if (degree < 0)
        {
            pc = (pitchClass + distance) % 12;
            degree = degreeOf (pc);
        }
    }

    const int size = interval < 0 ? -interval : interval, direction = interval < 0 ? -1 : 1;
    if (size >= 8) return 12 * direction;
    if (size <= 1 || degree < 0) return 0;

    if (count == 7)
    {
        const int target = degree + direction * (size - 1);
        const int octave = target >= 7 ? 1 : (target < 0 ? -1 : 0);
        return notes[target - 7 * octave] + 12 * octave - notes[degree];
    }

    static constexpr int preferred[6][4] = { { 2, 1, 3, 0 }, { 4, 3, 5, 2 }, { 5, 6, 4, 7 },
                                             { 7, 6, 8, 5 }, { 9, 8, 10, 7 }, { 11, 10, 9, 12 } };
    for (int i = 0; i < 4; ++i)
    {
        const int shiftBy = direction * preferred[size - 2][i];
        if (degreeOf (((pc + shiftBy) % 12 + 12) % 12) >= 0)
            return shiftBy;
    }
    return direction * preferred[size - 2][0];
}

//==============================================================================
// Maths built from + - * / only, so C++ and JavaScript get the same bits (std::pow / Math.pow may differ).

/** 2^x */
inline double exp2Exact (double x) noexcept
{
    const double whole = std::floor (x + 0.5);
    const double f = (x - whole) * 0.6931471805599453;
    double term = 1.0, sum = 1.0;
    for (int i = 1; i <= 13; ++i)
    {
        term *= f / (double) i;
        sum += term;
    }

    int n = (int) whole;
    for (; n > 0; --n) sum *= 2.0;
    for (; n < 0; ++n) sum *= 0.5;
    return sum;
}

/** log2 (v), v > 0 */
inline double log2Exact (double v) noexcept
{
    double exponent = 0.0;
    while (v >= 1.4142135623730951) { v *= 0.5; exponent += 1.0; }
    while (v <  0.7071067811865476) { v *= 2.0; exponent -= 1.0; }

    const double z = (v - 1.0) / (v + 1.0), z2 = z * z;
    double sum = 0.0;
    for (int k = 12; k >= 0; --k)
        sum = sum * z2 + 1.0 / (double) (2 * k + 1);
    return exponent + 2.0 * z * sum * 1.4426950408889634;
}

/** tan (x) for 0 <= x <= 0.8 */
inline double tanExact (double x) noexcept
{
    const double q = x * x;
    const double s = x * (1.0 - q / 6.0 * (1.0 - q / 20.0 * (1.0 - q / 42.0 * (1.0 - q / 72.0 * (1.0 - q / 110.0 * (1.0 - q / 156.0))))));
    const double c = 1.0 - q / 2.0 * (1.0 - q / 12.0 * (1.0 - q / 30.0 * (1.0 - q / 56.0 * (1.0 - q / 90.0 * (1.0 - q / 132.0 * (1.0 - q / 182.0))))));
    return s / c;
}

inline constexpr double tiny = 1.0e-30; // states below this are set to zero (before they become denormals)

/** Two-pole low-pass (trapezoidal state-variable filter) in double precision: part of the decision path. */
struct Svf
{
    void set (double g, double k) noexcept
    {
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void reset() noexcept { s1 = s2 = 0.0; }

    void flushTiny() noexcept
    {
        if (std::abs (s1) < tiny) s1 = 0.0;
        if (std::abs (s2) < tiny) s2 = 0.0;
    }

    double lowPass (double x) noexcept
    {
        const double v3 = x - s2;
        const double v1 = a1 * s1 + a2 * v3;
        const double v2 = s2 + a2 * s1 + a3 * v3;
        s1 = 2.0 * v1 - s1;
        s2 = 2.0 * v2 - s2;
        return v2;
    }

    double a1 = 1.0, a2 = 0.0, a3 = 0.0, s1 = 0.0, s2 = 0.0;
};

/** The same filter on float audio (the shifter's anti-alias filter). */
struct AudioSvf
{
    void set (double g, double k) noexcept
    {
        const double d = 1.0 / (1.0 + g * (g + k));
        a1 = (float) d;
        a2 = (float) (g * d);
        a3 = (float) (g * g * d);
    }

    void reset() noexcept { s1 = s2 = 0.0f; }

    float lowPass (float x) noexcept
    {
        const float v3 = x - s2;
        const float v1 = a1 * s1 + a2 * v3;
        const float v2 = s2 + a2 * s1 + a3 * v3;
        s1 = 2.0f * v1 - s1;
        s2 = 2.0f * v2 - s2;
        return v2;
    }

    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, s1 = 0.0f, s2 = 0.0f;
};

inline constexpr double butterworthK1 = 1.8477590650225735, butterworthK2 = 0.7653668647301796; // 1/Q of a 4-pole

//==============================================================================
/** Follows the period of the input.

    The input is band-limited (30 Hz .. 1.2 kHz, which favours the fundamental), decimated to about 8 kHz and
    every 3 ms compared with itself: nsdf (lag) = 2 sum (x[n] x[n - lag]) / sum (x[n]^2 + x[n - lag]^2), the
    normalised autocorrelation of McLeod's pitch method, which stays near 1 at the period while a note decays.
    The window is as long as the lag (at least 4 ms), so a new note is seen after two of its periods.

    Results:
      period / locked / tracking   the fundamental, for the octaver and the harmony: the first peak that is
                                   nearly as high as the highest one, with hysteresis (the current period is
                                   kept while its peak is still good; a new one has to show up twice, clearly)
      jump / jumpCorr              the lag (7..26 ms) at which the signal repeats best, for the shifter's splices:
                                   a few periods of a single note, the common period of a chord if it has one
                                   that short (fifths, octaves, open major chords; not close triads down low)
      onset / onsetAge             a pick attack: the level, or only the treble above 2 kHz (a note picked while
                                   others ring), is suddenly 5 dB above anything in the 36 ms before */
class Tracker
{
public:
    void prepare (double sampleRate)
    {
        decim  = std::max (1, (int) std::floor (sampleRate / 8000.0 + 0.5));
        rate   = sampleRate / (double) decim;
        maxLag = (int) std::floor (rate / 38.0 + 0.5);              // lowest note: about 38 Hz (a bass's low E is 41 Hz)
        minLag = std::max (2, (int) std::floor (rate / 1600.0));    // highest note: about 1.6 kHz
        minWindow  = (int) std::floor (rate * 0.004 + 0.5);
        hop        = (int) std::floor (rate * 0.003 + 0.5);
        jumpMinLag = (int) std::floor (rate * 0.007 + 0.5);
        span = 2 * maxLag;

        ring.assign ((size_t) span * 2, 0.0);
        cumulative.assign ((size_t) span + 1, 0.0);
        nsdf.assign ((size_t) maxLag + 2, 0.0);
        peakLags.assign ((size_t) maxLag + 1, 0);
        bright.assign ((size_t) span * 2, 0.0);
        hopLevels.assign ((size_t) numHopLevels, 0.0);
        hopBrightLevels.assign ((size_t) numHopLevels, 0.0);

        const double wh = 2.0 * pi * 30.0 / sampleRate, wl = 2.0 * pi * 1200.0 / sampleRate, wb = 2.0 * pi * 2000.0 / sampleRate;
        highPassCoef = wh / (1.0 + wh);
        lowPassCoef  = wl / (1.0 + wl);
        brightCoef   = wb / (1.0 + wb);
        defaultPeriod = sampleRate / 110.0;
        defaultJump   = sampleRate * 0.010;
        reset();
    }

    void reset()
    {
        std::fill (ring.begin(), ring.end(), 0.0);
        std::fill (nsdf.begin(), nsdf.end(), 0.0);
        std::fill (bright.begin(), bright.end(), 0.0);
        std::fill (hopLevels.begin(), hopLevels.end(), 0.0);
        std::fill (hopBrightLevels.begin(), hopBrightLevels.end(), 0.0);
        highPass = lowPass1 = lowPass2 = sum = brightLow = brightSum = 0.0;
        phase = hopCount = pos = hopLevelPos = 0;
        sinceOnset = 1000;
        gateOpen = locked = tracking = onset = false;
        onsetAge = 0.0;
        period = defaultPeriod;
        candidate = 0.0;
        candidateCount = disagreed = 0;
        jump = defaultJump;
        jumpCorr = 0.0;
    }

    double getTickRate() const noexcept  { return rate; }

    /** Takes one input sample. Returns 0, 1 (one decimated sample later: a "tick") or 2 (a tick on which the
        results were updated as well). */
    int push (double x) noexcept
    {
        highPass += highPassCoef * (x - highPass);
        lowPass1 += lowPassCoef * (x - highPass - lowPass1);
        lowPass2 += lowPassCoef * (lowPass1 - lowPass2);
        sum += lowPass2;

        // what is above 2 kHz, as energy: a pick attack shows there even while other notes ring on
        brightLow += brightCoef * (x - brightLow);
        brightSum += (x - brightLow) * (x - brightLow);

        if (++phase < decim)
            return 0;

        phase = 0;
        const double v = sum / (double) decim, e = brightSum / (double) decim;
        sum = brightSum = 0.0;
        ring[(size_t) pos] = v;
        ring[(size_t) (pos + span)] = v;
        bright[(size_t) pos] = e;
        bright[(size_t) (pos + span)] = e;
        if (++pos == span)
            pos = 0;

        if (++hopCount < hop)
            return 1;

        hopCount = 0;
        analyse();
        return 2;
    }

    // results (in samples at the full rate)
    double period = 0.0;     // the last fundamental period that was locked
    bool locked = false;     // `period` belongs to the note that is sounding (or has just ended)
    bool tracking = false;   // ... and the latest measurements agree with it
    double jump = 0.0;       // the lag at which the signal repeats best
    double jumpCorr = 0.0;   // how well it repeats there (0..1)
    bool onset = false;      // a note was picked a moment ago (true for one update only)
    double onsetAge = 0.0;   // ... this long ago

private:
    void noPitch() noexcept
    {
        candidateCount = 0;
        if (++disagreed >= 3)
        {
            disagreed = 3;
            locked = false;
        }
        tracking = locked && disagreed < 2;
    }

    /** The peak position between the lags around `lag` (parabola through three points). */
    double refine (int lag) const noexcept
    {
        if (lag <= 1 || lag >= maxLag)
            return (double) lag;

        const double a = nsdf[(size_t) lag - 1], b = nsdf[(size_t) lag], c = nsdf[(size_t) lag + 1];
        const double curve = a - 2.0 * b + c;
        if (curve > -1.0e-12)
            return (double) lag;

        double d = 0.5 * (a - c) / curve;
        if (d > 0.5) d = 0.5;
        if (d < -0.5) d = -0.5;
        return (double) lag + d;
    }

    /** Looks for a sudden rise at the end of `values` (one per tick, values[span - 1] is the newest; `samples`:
        they are samples, not energies): the last 3 ms are 3 times (5 dB) stronger than any 3 ms of the 36 ms
        before. `age`: how many ticks ago it began (the first value that stands out from those 36 ms). */
    bool rise (const double* values, bool samples, double lowest, std::vector<double>& history, int& age) noexcept
    {
        double recent = 0.0;
        for (int k = 0; k < hop; ++k)
        {
            const double v = values[span - 1 - k];
            recent += samples ? v * v : v;
        }
        recent /= (double) hop;

        double loudest = 0.0;
        for (int i = 0; i < numHopLevels; ++i)
            if (history[(size_t) i] > loudest)
                loudest = history[(size_t) i];

        history[(size_t) hopLevelPos] = recent;
        if (recent <= 3.0 * loudest + lowest)
            return false;

        double peak = 0.0;
        for (int k = 2 * hop; k < 14 * hop && k < span; ++k)
        {
            const double v = values[span - 1 - k], e = samples ? v * v : v;
            if (e > peak)
                peak = e;
        }

        const double threshold = std::max (1.5 * peak, 3.0 * loudest + lowest);
        for (int k = 2 * hop - 1; k >= 0; --k)
        {
            const double v = values[span - 1 - k];
            if ((samples ? v * v : v) > threshold)
            {
                age = k;
                break;
            }
        }
        return true;
    }

    void analyse() noexcept
    {
        if (std::abs (brightLow) < tiny) brightLow = 0.0;
        if (std::abs (highPass) < tiny) highPass = 0.0;
        if (std::abs (lowPass1) < tiny) lowPass1 = 0.0;
        if (std::abs (lowPass2) < tiny) lowPass2 = 0.0;

        const double* a = ring.data() + pos; // a[0] is the oldest of the last `span` samples, a[span - 1] the newest

        double energy = 0.0;
        cumulative[0] = 0.0;
        for (int k = 0; k < span; ++k)
        {
            const double s = a[span - 1 - k];
            energy += s * s;
            cumulative[(size_t) k + 1] = energy;
        }

        // A pick attack: a sudden rise of the level, or of the treble only (a note picked while others ring).
        // Not again within 30 ms: the first periods of a note can look like more attacks.
        {
            int lowAge = hop, brightAge = hop;
            const bool low = rise (a, true, 4.0e-6, hopLevels, lowAge);
            const bool high = rise (bright.data() + pos, false, 1.0e-7, hopBrightLevels, brightAge);
            if (++hopLevelPos == numHopLevels)
                hopLevelPos = 0;

            onset = (low || high) && sinceOnset >= 10;
            sinceOnset = onset ? 0 : (sinceOnset < 1000 ? sinceOnset + 1 : sinceOnset);
            if (onset)
                onsetAge = (double) ((high ? brightAge + 1 : lowAge + 3) * decim); // + the delay of the filters in front
        }

        // nothing to track below about -60 dB (closes again at -66 dB)
        const double level = cumulative[(size_t) maxLag] / (double) maxLag;
        gateOpen = level >= (gateOpen ? 2.5e-7 : 1.0e-6);
        if (! gateOpen)
        {
            noPitch();
            jump = defaultJump;
            jumpCorr = 0.0;
            return;
        }

        for (int lag = 1; lag <= maxLag; ++lag)
        {
            const int n = lag > minWindow ? lag : minWindow;
            const double* u = a + span - n;
            const double* v = u - lag;
            double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
            int i = 0;
            for (; i + 3 < n; i += 4)
            {
                s0 += u[i] * v[i];
                s1 += u[i + 1] * v[i + 1];
                s2 += u[i + 2] * v[i + 2];
                s3 += u[i + 3] * v[i + 3];
            }
            for (; i < n; ++i)
                s0 += u[i] * v[i];

            const double power = cumulative[(size_t) n] + (cumulative[(size_t) (n + lag)] - cumulative[(size_t) lag]);
            nsdf[(size_t) lag] = power > tiny ? 2.0 * ((s0 + s1) + (s2 + s3)) / power : 0.0;
        }

        // the peak of every positive stretch after the one around lag 0
        int t = 1;
        while (t <= maxLag && nsdf[(size_t) t] > 0.0)
            ++t;

        int numPeaks = 0;
        double highest = 0.0;
        while (t <= maxLag)
        {
            while (t <= maxLag && nsdf[(size_t) t] <= 0.0)
                ++t;
            if (t > maxLag)
                break;

            int top = t;
            for (; t <= maxLag && nsdf[(size_t) t] > 0.0; ++t)
                if (nsdf[(size_t) t] > nsdf[(size_t) top])
                    top = t;

            if (top >= minLag && top < maxLag) // at the very end of the range it is not a peak yet
            {
                peakLags[(size_t) numPeaks++] = top;
                if (nsdf[(size_t) top] > highest)
                    highest = nsdf[(size_t) top];
            }
        }

        // ---- where the signal repeats best (at least 7 ms away), for the shifter's splices
        {
            double best = 0.0;
            for (int p = 0; p < numPeaks; ++p)
                if (peakLags[(size_t) p] >= jumpMinLag && nsdf[(size_t) peakLags[(size_t) p]] > best)
                    best = nsdf[(size_t) peakLags[(size_t) p]];

            int chosen = -1;
            for (int p = 0; p < numPeaks && chosen < 0; ++p)
                if (peakLags[(size_t) p] >= jumpMinLag && nsdf[(size_t) peakLags[(size_t) p]] >= 0.97 * best)
                    chosen = peakLags[(size_t) p]; // the shortest of the good ones: least delay

            if (chosen < 0)
            {
                chosen = jumpMinLag;
                for (int lag = jumpMinLag + 1; lag < maxLag; ++lag)
                    if (nsdf[(size_t) lag] > nsdf[(size_t) chosen])
                        chosen = lag;
            }

            // right after a pick attack nothing repeats yet: short jumps smear an attack the least
            if (best < 0.5 && sinceOnset < 10)
                chosen = jumpMinLag;

            const double value = nsdf[(size_t) chosen];
            jump = refine (chosen) * (double) decim;
            jumpCorr = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
        }

        // ---- the fundamental
        if (numPeaks == 0 || highest < 0.5)
        {
            noPitch();
            return;
        }

        int chosen = -1;
        for (int p = 0; p < numPeaks && chosen < 0; ++p)
        {
            const int lag = peakLags[(size_t) p];
            // a shorter period only replaces the current one when it is clearly a period too (an octave up has
            // even harmonics only); the current one stays as long as its peak is reasonable
            const bool current = locked && std::abs (refine (lag) * (double) decim - period) <= 0.06 * period;
            if (nsdf[(size_t) lag] >= (current ? 0.80 : 0.93) * highest)
                chosen = lag;
        }

        if (chosen < 0)
        {
            noPitch();
            return;
        }

        const double measured = refine (chosen) * (double) decim;
        if (locked && std::abs (measured - period) <= 0.06 * period)
        {
            period = measured;
            candidateCount = disagreed = 0;
        }
        else
        {
            candidateCount = candidateCount > 0 && std::abs (measured - candidate) <= 0.03 * candidate ? candidateCount + 1 : 1;
            candidate = measured;
            if (disagreed < 3)
                ++disagreed;

            // seen twice in a row, and clearly periodic: a new note (one period after the attack the peak
            // of a half period can be the highest there is, but it is only that clear for an octave)
            if (candidateCount >= 2 && nsdf[(size_t) chosen] >= 0.8)
            {
                period = measured;
                locked = true;
                candidateCount = disagreed = 0;
            }
        }
        tracking = locked && disagreed < 2;
    }

    int decim = 6, maxLag = 210, minLag = 5, minWindow = 32, hop = 24, jumpMinLag = 56, span = 420;
    double rate = 8000.0, highPassCoef = 0.0, lowPassCoef = 0.0, brightCoef = 0.0, defaultPeriod = 436.0, defaultJump = 480.0;
    static constexpr int numHopLevels = 12;
    std::vector<double> ring, bright, cumulative, nsdf, hopLevels, hopBrightLevels;
    std::vector<int> peakLags;

    double highPass = 0.0, lowPass1 = 0.0, lowPass2 = 0.0, sum = 0.0, brightLow = 0.0, brightSum = 0.0, candidate = 0.0;
    int phase = 0, hopCount = 0, pos = 0, hopLevelPos = 0, sinceOnset = 1000, candidateCount = 0, disagreed = 0;
    bool gateOpen = false;
};

//==============================================================================
/** Delay-line pitch shifter. The read head moves through the recent input at `ratio` times the normal speed,
    so its delay shrinks (shift up) or grows (shift down). Before it runs out of room a second head starts a
    whole number of periods away (the tracker's `jump`) and the two are crossfaded: they play the same part of
    the waveform, so nothing cancels. Where the signal does not repeat well (chords, noise) the crossfade is
    made louder by the amount two unrelated signals would lose.
    Pick attacks (Tracker::onset) are neither skipped nor played twice: the head goes to just before the attack
    and the splices keep off it until it has been played.
    Delay: a pick attack is heard after 2..12 ms; a held single note lags 1..19 ms, a chord up to 35 ms. */
class Shifter
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        int size = 64;
        while (size < (int) (0.08 * sampleRate) + 64) // 80 ms: the longest delay is about 55 ms
            size *= 2;

        buffer.assign ((size_t) size, 0.0f);
        mask = size - 1;
        minDelay = std::floor (0.0005 * sampleRate) + 2.0;
        maxDelay = (double) size - 8.0;
        startDelay = minDelay + std::floor (0.005 * sampleRate);
        fadeMin = std::floor (0.001 * sampleRate);
        fadeMax = std::floor (0.010 * sampleRate);
        upDelay = minDelay + std::floor (0.0035 * sampleRate);
        onsetMargin = std::floor (0.0005 * sampleRate);
        onsetRoom = std::floor (0.0025 * sampleRate);
        attackLength = std::floor (0.004 * sampleRate);
        shortJump = std::floor (0.002 * sampleRate);
        longestDelay = std::floor (0.045 * sampleRate);
        reset();
    }

    /** How fast the ratio follows its target (time constant in seconds). */
    void setGlide (double seconds) noexcept { glide = 1.0 / (1.0 + seconds * fs); }
    void setRatio (double newRatio) noexcept { target = newRatio; }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        antiAlias1.reset();
        antiAlias2.reset();
        writePos = 0;
        ratio = target;
        filterRatio = -1.0;
        updateAntiAlias();
        delay = oldDelay = startDelay;
        fadeLeft = 0;
        fadeLength = 1.0;
        fadeLoss = 0.0f;
        onsetPending = onsetForced = false;
        sinceAttack = 1.0e9;
    }

    /** `event`: what Tracker::push returned for this sample (filter updates are done on its ticks). */
    float process (float x, const Tracker& tracker, int event) noexcept
    {
        if (ratio != target)
        {
            ratio += (target - ratio) * glide;
            if (std::abs (target - ratio) < 1.0e-9)
                ratio = target;
        }

        if (event != 0 && ratio != filterRatio)
            updateAntiAlias();

        writePos = (writePos + 1) & mask;
        buffer[(size_t) writePos] = antiAlias2.lowPass (antiAlias1.lowPass (x));

        const double drift = 1.0 - ratio;
        delay += drift;
        if (fadeLeft > 0)
            oldDelay += drift;

        // A pick attack (the tracker reports it a few ms late): go to just before it, so that it is heard at
        // once and in full. `sinceAttack` then keeps the splices from skipping or repeating it.
        if (sinceAttack < 1.0e8)
            sinceAttack += 1.0;

        if (event == 2 && tracker.onset)
        {
            onsetPending = true;
            onsetForced = false;
            sinceAttack = tracker.onsetAge;

            // Shifting down, in a crossfade to a head that has jumped ahead: if that head was (or will be) still
            // quiet in the middle of the attack's first 2 ms, the attack is as good as skipped, because the old
            // head never gets there. Then finish the crossfade within 1 ms, and go back if half of them is over.
            const double past = sinceAttack - delay;
            if (ratio <= 1.0 && fadeLeft > 0 && (double) fadeLeft + (past - 0.5 * shortJump) / ratio > 0.5 * fadeLength)
            {
                onsetForced = past > 0.5 * shortJump;
                if ((double) fadeLeft > fadeMin)
                {
                    fadeLength *= fadeMin / (double) fadeLeft;
                    fadeLeft = (int) fadeMin;
                }
            }
        }
        if (onsetPending)
        {
            if (fadeLeft == 0)
            {
                const double lead = onsetMargin + ratio * fadeMin; // the head fades in before it gets to the attack
                double wanted = sinceAttack + lead;
                if (wanted < minDelay) wanted = minDelay;
                bool go;

                if (ratio > 1.0)
                {
                    // far enough back that the head does not have to return before the attack is over
                    const double room = upDelay + onsetRoom + (ratio - 1.0) / ratio * (lead + attackLength + 2.0 * shortJump);
                    if (wanted < room) wanted = room;
                    go = delay > sinceAttack && std::abs (delay - wanted) > fadeMin; // (not if it has been played)
                }
                else
                {
                    go = onsetForced || delay > wanted + fadeMin;
                }

                if (go)
                {
                    oldDelay = delay;
                    delay = wanted;
                    startFade (fadeMin, 0.0);
                }
                onsetPending = false;
            }
            else if (sinceAttack > 2.0 * fadeMax)
            {
                onsetPending = false;
            }
        }

        if (fadeLeft > 0)
        {
            // one splice at a time
        }
        else if (ratio > 1.0)
        {
            // shifting up: the head catches up with the input, so it has to go back. It turns 3.5 ms before it
            // gets there: an attack is known that much later, and by then it should not have been played.
            const double speed = ratio - 1.0, lag = tracker.jump;
            const double past = sinceAttack - delay; // how far the head is beyond the start of the last attack
            double room = 0.35 * lag; // what the old head still travels during the crossfade
            if (room > speed * fadeMax) room = speed * fadeMax;
            if (room < speed * fadeMin || past < attackLength) room = speed * fadeMin; // (an attack is played first)

            if (delay <= upDelay + room)
            {
                double back = lag, match = tracker.jumpCorr, length = room / speed;
                if (past > 0.0 && past - back < attackLength)
                {
                    // not as far back: the attack should not be played twice
                    back = past - attackLength;
                    if (back < shortJump) back = shortJump;
                    match = 0.0;
                    if (length > 0.5 * back / speed) length = 0.5 * back / speed;
                    if (length < fadeMin) length = fadeMin;
                }

                oldDelay = delay;
                delay += back;
                startFade (length, match);
            }
        }
        else
        {
            // shifting down: the head falls behind, so it has to skip ahead (not shifting: it stays close);
            // not across an attack, though: that is played to its end first
            const double lag = tracker.jump;
            if (delay >= minDelay + lag && (sinceAttack - delay >= attackLength || delay >= longestDelay))
            {
                double length = ratio < 1.0 ? 0.35 * lag / (1.0 - ratio) : fadeMax;
                if (length > fadeMax) length = fadeMax;
                if (length < fadeMin) length = fadeMin;

                oldDelay = delay;
                delay -= lag;
                startFade (length, tracker.jumpCorr);
            }
        }

        float out = read (delay);
        if (fadeLeft > 0)
        {
            const float t = (float) ((double) fadeLeft / fadeLength);   // the old head's share: 1 -> 0
            const float b = t * t * (3.0f - 2.0f * t), a = 1.0f - b;
            out = (a * out + b * read (oldDelay)) / std::sqrt (1.0f - 2.0f * a * b * fadeLoss);
            --fadeLeft;
        }
        return out;
    }

private:
    void startFade (double length, double correlation) noexcept
    {
        fadeLength = std::max (1.0, std::floor (length));
        fadeLeft = (int) fadeLength;
        fadeLoss = (float) (1.0 - correlation);
    }

    /** Low-pass in front of the delay line: reading faster than the input was written would alias. */
    void updateAntiAlias() noexcept
    {
        filterRatio = ratio;
        const double cutoff = std::min (0.45, 0.40 / std::max (1.0, ratio)); // as a fraction of the sample rate
        const double g = std::tan (pi * cutoff);
        antiAlias1.set (g, butterworthK1);
        antiAlias2.set (g, butterworthK2);
    }

    /** The input `d` samples ago (4-point Hermite). */
    float read (double d) const noexcept
    {
        if (d < 2.0) d = 2.0;
        if (d > maxDelay) d = maxDelay;

        const double p = (double) (writePos + mask + 1) - d;
        const int i = (int) p;
        const float frac = (float) (p - (double) i);
        const float xm1 = buffer[(size_t) ((i - 1) & mask)], x0 = buffer[(size_t) (i & mask)];
        const float x1 = buffer[(size_t) ((i + 1) & mask)], x2 = buffer[(size_t) ((i + 2) & mask)];

        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * frac + c2) * frac + c1) * frac + x0;
    }

    double fs = 48000.0, minDelay = 26.0, maxDelay = 4088.0, startDelay = 266.0, fadeMin = 48.0, fadeMax = 480.0;
    double upDelay = 194.0, onsetMargin = 24.0, onsetRoom = 120.0, attackLength = 192.0, shortJump = 96.0, longestDelay = 2160.0;
    std::vector<float> buffer;
    int mask = 0, writePos = 0;
    AudioSvf antiAlias1, antiAlias2;

    double ratio = 1.0, target = 1.0, glide = 1.0, filterRatio = -1.0;
    double delay = 0.0, oldDelay = 0.0, fadeLength = 1.0, sinceAttack = 1.0e9;
    int fadeLeft = 0;
    float fadeLoss = 0.0f;
    bool onsetPending = false, onsetForced = false;
};

//==============================================================================
/** Analog-style octave divider (EBS OctaBass, Boss OC-2): the input's fundamental is isolated with a low-pass,
    a flip-flop toggles once per cycle, and the fundamental is inverted while the flip-flop is low. That gives
    a wave with twice the period which keeps the dynamics of the playing. A second low-pass (the Tone control)
    smooths it from a pure sub bass to a growl.
    The analog circuit's weak spot is the trigger: harmonics cause extra zero crossings and the octave jumps.
    Here both low-passes follow the tracked note, crossings closer than 0.62 periods are ignored, and the
    voice is faded out while the tracker has no note. */
class Octaver
{
public:
    void prepare (double sampleRate, double tickRate)
    {
        fs = sampleRate;
        const double w = 2.0 * pi * 15.0 / sampleRate;
        highPassCoef = w / (1.0 + w);
        glide = 1.0 / (1.0 + 0.006 * tickRate);
        gateUp = 1.0 / (0.006 * sampleRate);
        gateDown = 1.0 / (0.004 * sampleRate);
        reset();
    }

    /** 0..1: the octave voice's low-pass, from 1x to 5x the octave's frequency. */
    void setTone (double tone01) noexcept { toneTarget = exp2Exact (tone01 * 2.321928094887362); }

    void reset()
    {
        extract1.reset(); extract2.reset(); tone1.reset(); tone2.reset();
        highPass = envelope = previous = gate = gateTarget = 0.0;
        sign = 1.0;
        armed = false;
        sinceToggle = 0;
        frequency = frequencyTarget = 110.0;
        tone = toneTarget;
        holdOff = 0.62 * fs / 110.0;
        envelopeDecay = 1.0 - 110.0 / (2.0 * fs);
        updateFilters();
    }

    /** Once per tracker tick; `analysed`: the tracker has new results. */
    void tick (const Tracker& tracker, bool analysed) noexcept
    {
        if (analysed)
        {
            if (tracker.locked)
            {
                frequencyTarget = fs / tracker.period;
                holdOff = 0.62 * tracker.period;
                envelopeDecay = 1.0 - 1.0 / (2.0 * tracker.period);
            }
            gateTarget = tracker.tracking ? 1.0 : 0.0;

            if (std::abs (highPass) < tiny) highPass = 0.0;
            if (std::abs (envelope) < tiny) envelope = 0.0;
            extract1.flushTiny(); extract2.flushTiny(); tone1.flushTiny(); tone2.flushTiny();
        }

        if (frequency != frequencyTarget)
        {
            frequency += (frequencyTarget - frequency) * glide;
            if (std::abs (frequencyTarget - frequency) < 1.0e-6)
                frequency = frequencyTarget;
        }
        if (tone != toneTarget)
        {
            tone += (toneTarget - tone) * glide;
            if (std::abs (toneTarget - tone) < 1.0e-6)
                tone = toneTarget;
        }
        if (frequency != filterFrequency || tone != filterTone)
            updateFilters();
    }

    double process (double x) noexcept
    {
        highPass += highPassCoef * (x - highPass);
        const double y = extract2.lowPass (extract1.lowPass (x - highPass));

        const double magnitude = std::abs (y);
        envelope = magnitude > envelope ? magnitude : envelope * envelopeDecay;

        if (sinceToggle < 1000000000)
            ++sinceToggle;

        // Schmitt trigger: a rising zero crossing counts once the signal has been clearly negative
        if (previous < 0.0 && y >= 0.0)
        {
            if (armed && (double) sinceToggle >= holdOff)
            {
                sign = -sign;
                sinceToggle = 0;
            }
            armed = false;
        }
        if (y < -0.2 * envelope - 1.0e-9)
            armed = true;
        previous = y;

        if (gate < gateTarget)      { gate += gateUp;   if (gate > gateTarget) gate = gateTarget; }
        else if (gate > gateTarget) { gate -= gateDown; if (gate < gateTarget) gate = gateTarget; }

        return gate * tone2.lowPass (tone1.lowPass (sign * y));
    }

private:
    void updateFilters() noexcept
    {
        filterFrequency = frequency;
        filterTone = tone;

        const double limit = 0.2 * fs;
        double cutoff = 1.25 * frequency; // keeps the fundamental, 16 dB less of the second harmonic
        if (cutoff > limit) cutoff = limit;
        double g = tanExact (pi * cutoff / fs);
        extract1.set (g, butterworthK1);
        extract2.set (g, butterworthK2);

        cutoff = tone * 0.5 * frequency;
        if (cutoff > limit) cutoff = limit;
        g = tanExact (pi * cutoff / fs);
        tone1.set (g, butterworthK1);
        tone2.set (g, butterworthK2);
    }

    double fs = 48000.0, highPassCoef = 0.0, glide = 1.0, gateUp = 1.0, gateDown = 1.0;
    Svf extract1, extract2, tone1, tone2;
    double highPass = 0.0, envelope = 0.0, envelopeDecay = 0.999, previous = 0.0, sign = 1.0, holdOff = 0.0;
    double gate = 0.0, gateTarget = 0.0;
    double frequency = 110.0, frequencyTarget = 110.0, tone = 2.0, toneTarget = 2.0, filterFrequency = 0.0, filterTone = 0.0;
    int sinceToggle = 0;
    bool armed = false;
};

} // namespace pitch_detail

//==============================================================================
class PitchFx
{
public:
    enum Variant { bassOctaver = 0, pitchGlide, smartHarmony, numVariants };

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        fs = sampleRate;
        tracker.prepare (sampleRate);
        shifter.prepare (sampleRate);
        octaver.prepare (sampleRate, tracker.getTickRate());
        dryGain.reset (sampleRate, 0.03);
        wetGain.reset (sampleRate, 0.03);
        reset();
    }

    void reset()
    {
        tracker.reset();
        octaver.reset();
        note = 0;
        haveNote = false;
        pitchClass = 0;
        if (variant == smartHarmony)
            shifter.setRatio (ratios[0]);
        shifter.reset();
        dryGain.setCurrentAndTarget (dryGain.getTarget());
        wetGain.setCurrentAndTarget (wetGain.getTarget());
    }

    void setModel (int newVariant) noexcept { variant = std::clamp (newVariant, 0, (int) numVariants - 1); }

    void setParameters (const float* k)
    {
        if (variant == bassOctaver)
        {
            // Bass Octaver - EBS OctaBass: an analog octave divider for bass. Modelled: the flip-flop divider on
            // the low-passed fundamental (the octave follows the playing dynamics, one note at a time), separate
            // Normal and Octave levels, and the octave's tone filter (the original's High / Mid / Low switch).
            const float tone = std::clamp (k[0], 0.0f, 100.0f) / 100.0f;
            const float normal = std::clamp (k[1], 0.0f, 100.0f) / 100.0f, octave = std::clamp (k[2], 0.0f, 100.0f) / 100.0f;
            octaver.setTone ((double) tone);
            dryGain.setTarget (normal * normal);
            wetGain.setTarget (2.4f * octave * octave);
            return;
        }

        float mix;
        if (variant == pitchGlide)
        {
            // Pitch Glide - Digitech Whammy: one shifted voice whose interval is swept with a pedal. Modelled:
            // the continuous glide between any two intervals within +-2 octaves, and splicing that copes with
            // chords (the head jumps to where the signal repeats best instead of at a fixed rate). Like the
            // original it is cleanest on single notes, fifths and octaves; the third of a low, close chord warbles.
            const double pitch = (double) std::clamp (k[0], -24.0f, 24.0f);
            shifter.setGlide (0.012);
            shifter.setRatio (pitch_detail::exp2Exact (pitch / 12.0));
            mix = k[1];
        }
        else
        {
            // Smart Harmony - Eventide H3000 diatonic shift: detects the played note and picks the interval that
            // keeps the harmony in the chosen key and scale (a third above is 4 semitones on C but 3 on D in
            // C major). Modelled: monophonic note detection with hysteresis, the scale logic, a quick slide of
            // the shifted voice from one interval to the next, pitch-synchronous splicing.
            const int key   = std::clamp ((int) std::floor (k[0] + 0.5f), 0, pitch_detail::numKeys - 1);
            const int shift = std::clamp ((int) std::floor (k[1] + 0.5f), 0, pitch_detail::numShifts - 1);
            const int scale = std::clamp ((int) std::floor (k[2] + 0.5f), 0, pitch_detail::numScales - 1);

            if (key != harmonyKey || shift != harmonyShift || scale != harmonyScale)
            {
                harmonyKey = key; harmonyShift = shift; harmonyScale = scale;
                for (int pc = 0; pc < 12; ++pc)
                    ratios[pc] = pitch_detail::exp2Exact ((double) pitch_detail::harmonySemitones (scale, pitch_detail::shiftIntervals[shift], pc) / 12.0);
                pitchClass = haveNote ? ((note - key) % 12 + 12) % 12 : 0;
            }
            shifter.setGlide (0.008);
            shifter.setRatio (ratios[pitchClass]);
            mix = k[3];
        }

        // equal-power mix: a harmony and the dry note together stay at about the input's loudness
        const float angle = std::clamp (mix, 0.0f, 100.0f) / 100.0f * (float) (0.5 * pi);
        dryGain.setTarget (std::cos (angle));
        wetGain.setTarget (std::sin (angle));
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        if (variant == bassOctaver)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float in = 0.5f * (left[i] + right[i]);
                const int event = tracker.push ((double) in);
                if (event != 0)
                    octaver.tick (tracker, event == 2);

                const double octave = octaver.process ((double) in);
                left[i] = right[i] = dryGain.next() * in + (float) ((double) wetGain.next() * octave);
            }
            return;
        }

        const bool harmony = variant == smartHarmony;
        for (int i = 0; i < numSamples; ++i)
        {
            const float in = 0.5f * (left[i] + right[i]);
            const int event = tracker.push ((double) in);
            if (harmony && event == 2)
                followNote();

            const float wet = shifter.process (in, tracker, event);
            left[i] = right[i] = dryGain.next() * in + wetGain.next() * wet;
        }
    }

private:
    /** Smart Harmony: which note is being played (A = 440 Hz), with hysteresis so a bend or a slightly flat
        string does not flip between two notes. */
    void followNote() noexcept
    {
        if (! tracker.tracking)
            return;

        const double midi = 69.0 + 12.0 * pitch_detail::log2Exact (fs / (tracker.period * 440.0));
        if (! haveNote || std::abs (midi - (double) note) > 0.6)
        {
            note = (int) std::floor (midi + 0.5);
            haveNote = true;
            pitchClass = ((note - harmonyKey) % 12 + 12) % 12;
            shifter.setRatio (ratios[pitchClass]);
        }
    }

    int variant = bassOctaver;
    double fs = 48000.0;
    pitch_detail::Tracker tracker;
    pitch_detail::Shifter shifter;
    pitch_detail::Octaver octaver;
    Smoothed dryGain { 1.0f }, wetGain { 1.0f };

    int harmonyKey = -1, harmonyShift = -1, harmonyScale = -1;
    double ratios[12] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
    int note = 0, pitchClass = 0;
    bool haveNote = false;
};

/** In the order of PitchFx::Variant. */
inline std::vector<ModelInfo> pitchModels()
{
    using namespace pitch_detail;
    return {
        { "bass_octaver", "Bass Octaver", Category::pitch, Engine::pitchFx, PitchFx::bassOctaver, "EBS OctaBass",
          { percent ("Tone", 50.0f), percent ("Normal", 100.0f), percent ("Octave", 70.0f) } },
        { "pitch_glide", "Pitch Glide", Category::pitch, Engine::pitchFx, PitchFx::pitchGlide, "Digitech Whammy",
          { semitones ("Pitch", -24.0f, 24.0f, 12.0f, 0.1f), percent ("Mix", 100.0f) } },
        { "smart_harmony", "Smart Harmony", Category::pitch, Engine::pitchFx, PitchFx::smartHarmony, "Eventide H3000",
          { choice ("Key", pitchKeyNames, numKeys, 0), choice ("Shift", pitchShiftNames, numShifts, 8),
            choice ("Scale", pitchScaleNames, numScales, 0), percent ("Mix", 50.0f) } },
    };
}

} // namespace fx
