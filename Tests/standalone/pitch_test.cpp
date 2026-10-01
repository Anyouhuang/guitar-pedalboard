// Standalone test of the Pitch engine (see Harness.h): Bass Octaver, Pitch Glide, Smart Harmony.
//
// After the generic harness run, the checks below measure what each model promises: the pitch of the shifted
// voice (sines and plucked strings from 82 to 660 Hz), that its level is steady (no tremolo from the splices),
// that sweeping the Pitch knob does not click, and that Smart Harmony picks the diatonic interval.
#include "Harness.h"
#include "../../Source/DSP/fx/Pitch.h"

namespace
{
int failed = 0;
bool verbose = false;

void check (bool ok, const std::string& what)
{
    if (! ok)
    {
        ++failed;
        std::printf ("    FAIL: %s\n", what.c_str());
    }
}

std::string num (double v, int decimals = 2)
{
    char buffer[40];
    std::snprintf (buffer, sizeof (buffer), "%.*f", decimals, v);
    return buffer;
}

constexpr double testFs = harness::testRate;
enum { octaver = fx::PitchFx::bassOctaver, glide = fx::PitchFx::pitchGlide, harmony = fx::PitchFx::smartHarmony };

//==============================================================================
// test signals
struct Signal
{
    std::vector<float> samples;
    double hz = 0.0; // the real frequency (a plucked string's period is a whole number of samples + 0.5)
    const char* kind = "";
};

Signal sine (double hz, double seconds, float amplitude = 0.3f, double fs = testFs, double startAt = 0.0)
{
    Signal s { std::vector<float> ((size_t) (seconds * fs), 0.0f), hz, "sine" };
    const auto start = (size_t) (startAt * fs), fade = (size_t) (0.003 * fs);
    for (size_t i = start; i < s.samples.size(); ++i)
        s.samples[i] = amplitude * (float) std::min (1.0, (double) (i - start) / (double) fade)
                       * (float) std::sin (2.0 * fx::pi * hz * (double) (i - start) / fs);
    return s;
}

/** The harness's plucked string (Karplus-Strong): all harmonics at first, the high ones decay faster. */
Signal pluck (double hz, double seconds, float amplitude = 0.3f, double fs = testFs, uint32_t seed = 4321u)
{
    harness::Lcg rng { seed + (uint32_t) hz };
    const int period = std::max (2, (int) std::lround (fs / hz - 0.5));
    std::vector<float> string ((size_t) period);
    for (auto& v : string)
        v = rng.next01() * 2.0f - 1.0f;

    Signal s { std::vector<float> ((size_t) (seconds * fs), 0.0f), fs / ((double) period + 0.5), "pluck" };
    float previous = 0.0f, peak = 0.0f;
    for (size_t i = 0; i < s.samples.size(); ++i)
    {
        const size_t idx = i % (size_t) period;
        const float current = string[idx];
        string[idx] = 0.996f * 0.5f * (current + previous);
        previous = current;
        s.samples[i] = current;
        peak = std::max (peak, std::abs (current));
    }
    for (auto& v : s.samples)
        v *= amplitude / peak;
    return s;
}

Signal add (const Signal& a, const Signal& b)
{
    Signal s = a;
    for (size_t i = 0; i < s.samples.size() && i < b.samples.size(); ++i)
        s.samples[i] += b.samples[i];
    return s;
}

harness::Knobs knobs (float a, float b = 0.0f, float c = 0.0f, float d = 0.0f)
{
    harness::Knobs k {};
    k[0] = a; k[1] = b; k[2] = c; k[3] = d;
    return k;
}

/** One channel of the engine's output (both are the same: the models are mono). */
std::vector<float> run (int variant, const std::vector<float>& input, const harness::Knobs& k, double fs = testFs,
                        const std::vector<harness::Step>& schedule = {})
{
    fx::PitchFx effect;
    effect.prepare (fs, harness::blockSize);
    const auto stereo = harness::render (effect, variant, input, k, schedule);
    std::vector<float> mono (input.size());
    for (size_t i = 0; i < mono.size(); ++i)
        mono[i] = stereo[i * 2];
    return mono;
}

//==============================================================================
// measurements
double rmsOf (const std::vector<float>& x, size_t from, size_t to)
{
    double sum = 0.0;
    for (size_t i = from; i < to; ++i)
        sum += (double) x[i] * x[i];
    return std::sqrt (sum / (double) std::max<size_t> (1, to - from));
}

double db (double gain) { return gain > 1.0e-12 ? 20.0 * std::log10 (gain) : -240.0; }
double cents (double hz, double reference) { return 1200.0 * std::log2 (hz / reference); }

/** Normalised autocorrelation (McLeod) of x[from, from + n) at one lag: 1 = the signal repeats after `lag`. */
double nsdfAt (const std::vector<float>& x, size_t from, size_t n, size_t lag)
{
    double r = 0.0, m = 0.0;
    for (size_t i = 0; i + lag < n; ++i)
    {
        const double a = x[from + i], b = x[from + i + lag];
        r += a * b;
        m += a * a + b * b;
    }
    return m > 0.0 ? 2.0 * r / m : 0.0;
}

double parabola (double a, double b, double c)
{
    const double curve = a - 2.0 * b + c;
    return curve < -1.0e-12 ? std::clamp (0.5 * (a - c) / curve, -0.5, 0.5) : 0.0;
}

struct Pitch { double hz = 0.0, clarity = 0.0; };

/** The fundamental of x[from, from + n): the first autocorrelation peak that is within 10 % of the highest
    (so an octave error in the engine is seen as one), then refined over many periods.
    `roughHz` only sets the search range (down to 2.6 periods of it). */
Pitch measurePitch (const std::vector<float>& signal, double fs, size_t from, size_t n, double roughHz)
{
    // without DC (the plucked strings have some, and then the autocorrelation never goes negative)
    std::vector<float> x (signal.size());
    double dc = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        dc += (signal[i] - dc) * (2.0 * fx::pi * 15.0 / fs);
        x[i] = (float) (signal[i] - dc);
    }

    const size_t maxLag = std::min (n / 2, (size_t) (2.6 * fs / roughHz) + 8);
    std::vector<double> c (maxLag + 2, 0.0);
    for (size_t lag = 1; lag <= maxLag + 1; ++lag)
        c[lag] = nsdfAt (x, from, n, lag);

    size_t t = 1;
    while (t <= maxLag && c[t] > 0.0) ++t;

    std::vector<size_t> peaks;
    double highest = 0.0;
    while (t <= maxLag)
    {
        while (t <= maxLag && c[t] <= 0.0) ++t;
        if (t > maxLag) break;
        size_t top = t;
        for (; t <= maxLag && c[t] > 0.0; ++t)
            if (c[t] > c[top]) top = t;
        if (top < maxLag)
        {
            peaks.push_back (top);
            highest = std::max (highest, c[top]);
        }
    }

    for (size_t top : peaks)
        if (c[top] >= 0.9 * highest)
        {
            double period = (double) top + parabola (c[top - 1], c[top], c[top + 1]);

            // the same peak many periods later pins the period down much better
            const int multiple = (int) std::floor (0.25 * (double) n / period);
            if (multiple > 1)
            {
                const auto centre = (size_t) std::llround (period * multiple);
                double v[5];
                int best = 1;
                for (int j = 0; j < 5; ++j)
                    v[j] = nsdfAt (x, from, n, centre + (size_t) j - 2);
                for (int j = 1; j <= 3; ++j)
                    if (v[j] > v[best]) best = j;
                period = ((double) centre + (double) (best - 2) + parabola (v[best - 1], v[best], v[best + 1])) / multiple;
            }
            return { fs / period, c[top] };
        }
    return {};
}

/** Amplitude of one frequency in x[from, to) (windowed single-bin DFT). */
double toneAmplitude (const std::vector<float>& x, double fs, size_t from, size_t to, double hz)
{
    double re = 0.0, im = 0.0, norm = 0.0;
    const double n = (double) (to - from);
    for (size_t i = from; i < to; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * fx::pi * (double) (i - from) / n);
        const double ph = 2.0 * fx::pi * hz * (double) i / fs;
        re += w * x[i] * std::cos (ph);
        im += w * x[i] * std::sin (ph);
        norm += w;
    }
    return 2.0 * std::sqrt (re * re + im * im) / norm;
}

struct Ripple { double bump = 0.0, spread = 0.0; };

/** How steady the level of x[from, to) is, in dB. The level is taken over frames of whole periods (30 ms or
    more). `bump`: the most a frame differs from the mean of its two neighbours, which shows tremolo, dropouts
    and splice bumps but not a note's smooth decay. `spread`: loudest minus quietest frame. */
Ripple levelRipple (const std::vector<float>& x, double fs, size_t from, size_t to, double hz)
{
    const double period = fs / hz;
    const auto frame = (size_t) std::llround (std::ceil (0.03 * fs / period) * period);
    std::vector<double> levels;
    for (size_t at = from; at + frame <= to; at += frame)
        levels.push_back (db (rmsOf (x, at, at + frame)));

    Ripple r;
    double lo = 1000.0, hi = -1000.0;
    for (size_t i = 0; i < levels.size(); ++i)
    {
        lo = std::min (lo, levels[i]);
        hi = std::max (hi, levels[i]);
        if (i > 0 && i + 1 < levels.size())
            r.bump = std::max (r.bump, std::abs (levels[i] - 0.5 * (levels[i - 1] + levels[i + 1])));
    }
    r.spread = hi - lo;
    return r;
}

/** Lowest and highest momentary amplitude (dB re `reference`) of a sine of known frequency in x[from, to), from
    each sample and the slope around it. Unlike the frames above this shows a wobble at the rate of the splices. */
Ripple sineEnvelope (const std::vector<float>& x, double fs, size_t from, size_t to, double hz, double reference)
{
    const double w = 2.0 * fx::pi * hz / fs;
    double lo = 1.0e9, hi = 0.0;
    for (size_t i = from + 1; i + 1 < to; ++i)
    {
        const double quadrature = ((double) x[i + 1] - (double) x[i - 1]) / (2.0 * std::sin (w));
        const double amplitude = std::sqrt ((double) x[i] * x[i] + quadrature * quadrature);
        lo = std::min (lo, amplitude);
        hi = std::max (hi, amplitude);
    }
    return { db (hi / reference), db (lo / reference) }; // bump = the highest, spread = the lowest
}

struct Line { double hz = 0.0, amplitude = 0.0; };

/** The strongest spectral line within +-`range` Hz of `hz` in x[from, to) (for signals below 3 kHz). */
Line strongestLine (const std::vector<float>& x, double fs, size_t from, size_t to, double hz, double range)
{
    Line best;
    const double n = (double) (to - from);
    for (double f = hz - range; f <= hz + range; f += 0.25)
    {
        double re = 0.0, im = 0.0, norm = 0.0;
        for (size_t i = from; i < to; i += 4)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * fx::pi * (double) (i - from) / n);
            const double ph = 2.0 * fx::pi * f * (double) i / fs;
            re += w * x[i] * std::cos (ph);
            im += w * x[i] * std::sin (ph);
            norm += w;
        }
        const double amplitude = 2.0 * std::sqrt (re * re + im * im) / norm;
        if (amplitude > best.amplitude)
            best = { f, amplitude };
    }
    return best;
}

/** Level of x in 1 ms frames every 0.25 ms, from `from` on. */
std::vector<double> fineLevels (const std::vector<float>& x, double fs, size_t from, double seconds)
{
    const auto frame = (size_t) (0.001 * fs), step = frame / 4;
    std::vector<double> levels;
    for (size_t at = from; at + frame <= from + (size_t) (seconds * fs) && at + frame <= x.size(); at += step)
        levels.push_back (rmsOf (x, at, at + frame));
    return levels;
}

/** Largest step between two neighbouring samples of x[from, to). */
double maxStep (const std::vector<float>& x, size_t from, size_t to)
{
    double step = 0.0;
    for (size_t i = from + 1; i < to; ++i)
        step = std::max (step, (double) std::abs (x[i] - x[i - 1]));
    return step;
}

const double testNotes[] = { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63, 440.0, 659.26 };
const size_t measureFrom = (size_t) (0.45 * testFs), measureLength = (size_t) (0.30 * testFs);

//==============================================================================
/** Bass Octaver: the octave voice alone is one octave below the played note, as steady as the note itself. */
void testOctaver()
{
    std::printf ("-- Bass Octaver: octave voice alone (Normal 0 %%, Octave 100 %%)\n");
    double worstCents = 0.0, worstBump = 0.0, worstSpread = 0.0, lowestLevel = 100.0, highestLevel = -100.0;

    std::vector<double> notes (std::begin (testNotes), std::end (testNotes));
    notes.insert (notes.begin(), { 41.2, 55.0 }); // a bass's low E and A

    for (double hz : notes)
        for (int kind = 0; kind < 2; ++kind)
        {
            const Signal in = kind == 0 ? sine (hz, 1.2) : pluck (hz, 1.2);
            const auto out = run (octaver, in.samples, knobs (50.0f, 0.0f, 100.0f));
            const size_t from = hz < 60.0 ? (size_t) (0.5 * testFs) : measureFrom, length = hz < 60.0 ? (size_t) (0.6 * testFs) : measureLength;

            const Pitch p = measurePitch (out, testFs, from, length, in.hz * 0.5);
            const double off = p.hz > 0.0 ? cents (p.hz, in.hz * 0.5) : 9999.0;
            const Ripple ripple = levelRipple (out, testFs, (size_t) (0.3 * testFs), (size_t) (hz < 60.0 ? 1.15 * testFs : 0.8 * testFs), in.hz * 0.5);
            const double level = db (rmsOf (out, from, from + length) / rmsOf (in.samples, from, from + length));

            if (verbose)
                std::printf ("   %-5s %7.2f Hz -> %7.2f Hz (%+6.1f cents, clarity %.3f)  level %+5.1f dB  bump %.2f dB  spread %.2f dB\n",
                             in.kind, in.hz, p.hz, off, p.clarity, level, ripple.bump, ripple.spread);

            const std::string what = std::string ("octaver, ") + in.kind + " " + num (in.hz) + " Hz: ";
            check (std::abs (off) < 5.0, what + "octave voice is at " + num (p.hz) + " Hz, expected " + num (in.hz * 0.5));
            check (p.clarity > 0.9, what + "octave voice is not periodic (clarity " + num (p.clarity, 3) + ")");
            check (ripple.bump < 2.0, what + "octave level wobbles by " + num (ripple.bump) + " dB");
            if (kind == 0)
                check (ripple.spread < 2.0, what + "octave level varies by " + num (ripple.spread) + " dB on a steady sine");
            check (level > -15.0 && level < 9.0, what + "octave level is " + num (level) + " dB vs the input");

            worstCents = std::max (worstCents, std::abs (off));
            worstBump = std::max (worstBump, ripple.bump);
            if (kind == 0) worstSpread = std::max (worstSpread, ripple.spread);
            lowestLevel = std::min (lowestLevel, level);
            highestLevel = std::max (highestLevel, level);
        }
    std::printf ("   %d notes 41..659 Hz, sine + pluck: worst pitch error %.2f cents, level bump %.2f dB, spread on sines %.2f dB, level %+.1f..%+.1f dB\n",
                 (int) notes.size(), worstCents, worstBump, worstSpread, lowestLevel, highestLevel);

    // Tone: from a pure sub (only the octave) to a growl (the 3rd harmonic of the octave, a fifth above the note)
    {
        const Signal in = sine (110.0, 1.0);
        double third[2] {};
        for (int i = 0; i < 2; ++i)
        {
            const auto out = run (octaver, in.samples, knobs (i == 0 ? 0.0f : 100.0f, 0.0f, 100.0f));
            third[i] = db (toneAmplitude (out, testFs, measureFrom, measureFrom + measureLength, 165.0)
                           / toneAmplitude (out, testFs, measureFrom, measureFrom + measureLength, 55.0));
        }
        std::printf ("   Tone: 3rd harmonic of the octave voice at %.1f dB (Tone 0 %%) and %.1f dB (Tone 100 %%) vs its fundamental\n", third[0], third[1]);
        check (third[0] < -25.0, "octaver: Tone 0 % should leave a nearly pure sub (3rd harmonic at " + num (third[0]) + " dB)");
        check (third[1] > -8.0 && third[1] - third[0] > 15.0, "octaver: Tone 100 % should bring out the harmonics (3rd at " + num (third[1]) + " dB)");
    }

    // Normal alone is the untouched input; both knobs at 0 is silence
    {
        const Signal in = pluck (146.83, 0.6);
        const auto dry = run (octaver, in.samples, knobs (50.0f, 100.0f, 0.0f));
        const auto none = run (octaver, in.samples, knobs (50.0f, 0.0f, 0.0f));
        double difference = 0.0, left = 0.0;
        for (size_t i = 0; i < dry.size(); ++i)
        {
            difference = std::max (difference, (double) std::abs (dry[i] - in.samples[i]));
            left = std::max (left, (double) std::abs (none[i]));
        }
        check (difference < 1.0e-6, "octaver: Normal 100 % / Octave 0 % should be the dry signal (differs by " + num (difference, 6) + ")");
        check (left < 1.0e-6, "octaver: Normal 0 % / Octave 0 % should be silent");
    }

    // a line of notes: the octave follows every note (the pitch is measured in the second half of each note)
    {
        const double line[] = { 110.0, 164.81, 98.0, 220.0, 130.81, 82.41 };
        Signal in { {}, 0.0, "line" };
        std::vector<double> real;
        for (double hz : line)
        {
            const Signal note = pluck (hz, 0.4);
            in.samples.insert (in.samples.end(), note.samples.begin(), note.samples.end());
            real.push_back (note.hz);
        }
        const auto out = run (octaver, in.samples, knobs (50.0f, 0.0f, 100.0f));
        double worst = 0.0;
        for (size_t i = 0; i < real.size(); ++i)
        {
            const Pitch p = measurePitch (out, testFs, (size_t) ((0.4 * (double) i + 0.15) * testFs), (size_t) (0.24 * testFs), real[i] * 0.5);
            const double off = p.hz > 0.0 ? cents (p.hz, real[i] * 0.5) : 9999.0;
            worst = std::max (worst, std::abs (off));
            check (std::abs (off) < 10.0, "octaver: note " + std::to_string (i) + " of a line (" + num (real[i]) + " Hz) gives " + num (p.hz) + " Hz");
        }
        std::printf ("   line of 6 plucked notes: worst pitch error of the octave %.2f cents\n", worst);
    }
}

//==============================================================================
/** Pitch Glide: the wet signal is at the set interval, with a steady level. */
void testGlide()
{
    std::printf ("-- Pitch Glide: wet signal alone (Mix 100 %%)\n");
    for (float shift : { 12.0f, -12.0f, 7.0f, -5.0f, 24.0f, -24.0f, 0.3f })
    {
        const double ratio = std::pow (2.0, (double) shift / 12.0);
        double worstCents = 0.0, worstBump = 0.0, worstSpread = 0.0, lowestLevel = 100.0, highestLevel = -100.0;

        for (double hz : testNotes)
            for (int kind = 0; kind < 2; ++kind)
            {
                const Signal in = kind == 0 ? sine (hz, 1.0) : pluck (hz, 1.0);
                const auto out = run (glide, in.samples, knobs (shift, 100.0f));
                const Pitch p = measurePitch (out, testFs, measureFrom, measureLength, in.hz * ratio);
                const double off = p.hz > 0.0 ? cents (p.hz, in.hz * ratio) : 9999.0;
                const Ripple ripple = levelRipple (out, testFs, (size_t) (0.2 * testFs), out.size() - 2400, in.hz * ratio);
                const double level = db (rmsOf (out, measureFrom, measureFrom + measureLength) / rmsOf (in.samples, measureFrom, measureFrom + measureLength));

                if (verbose)
                    std::printf ("   %+5.1f st %-5s %7.2f Hz -> %8.2f Hz (%+6.1f cents, clarity %.3f)  level %+5.1f dB  bump %.2f dB  spread %.2f dB\n",
                                 shift, in.kind, in.hz, p.hz, off, p.clarity, level, ripple.bump, ripple.spread);

                const std::string what = "glide " + num (shift, 1) + " st, " + in.kind + " " + num (in.hz) + " Hz: ";
                check (std::abs (off) < 5.0, what + "wet signal is at " + num (p.hz) + " Hz, expected " + num (in.hz * ratio));
                check (ripple.bump < 2.0, what + "wet level wobbles by " + num (ripple.bump) + " dB");
                if (kind == 0)
                {
                    // sample by sample: the splices (every few ms) must not show as a tremolo
                    const Ripple envelope = sineEnvelope (out, testFs, (size_t) (0.2 * testFs), out.size() - 2400, in.hz * ratio, 0.3);
                    check (envelope.bump < 1.0 && envelope.spread > -1.0, what + "momentary amplitude of the wet sine moves between "
                                                                            + num (envelope.spread) + " and " + num (envelope.bump) + " dB");
                    check (ripple.spread < 2.0, what + "wet level varies by " + num (ripple.spread) + " dB on a steady sine");
                    check (std::abs (level) < 2.0, what + "wet level is " + num (level) + " dB vs the input");
                    lowestLevel = std::min (lowestLevel, envelope.spread);
                    highestLevel = std::max (highestLevel, envelope.bump);
                    worstSpread = std::max (worstSpread, ripple.spread);
                }

                worstCents = std::max (worstCents, std::abs (off));
                worstBump = std::max (worstBump, ripple.bump);
            }
        std::printf ("   %+5.1f st, 8 notes 82..659 Hz, sine + pluck: worst pitch error %.2f cents, level bump %.2f dB; sines: momentary amplitude %+.2f..%+.2f dB\n",
                     shift, worstCents, worstBump, lowestLevel, highestLevel);
    }

    // chords (as sines, so that every note can be found again): each note arrives at its new pitch, at its
    // level, and the chord's level stays steady. A power chord, and an open E major chord of 6 strings.
    for (int which = 0; which < 2; ++which)
        for (float shift : { 12.0f, 7.0f, -5.0f, -12.0f })
        {
            const double ratio = std::pow (2.0, (double) shift / 12.0);
            const std::vector<double> chord = which == 0 ? std::vector<double> { 110.0, 164.81, 220.0 }
                                                         : std::vector<double> { 82.41, 123.47, 164.81, 207.65, 246.94, 329.63 };
            const float each = 0.45f / (float) chord.size();
            Signal in = sine (chord[0], 2.0, each);
            for (size_t i = 1; i < chord.size(); ++i)
                in = add (in, sine (chord[i], 2.0, each));

            const auto out = run (glide, in.samples, knobs (shift, 100.0f));
            const size_t from = (size_t) (0.4 * testFs), to = out.size() - 2400;
            double weakest = 100.0, detune = 0.0;
            for (double hz : chord)
            {
                const Line line = strongestLine (out, testFs, from, to, hz * ratio, std::min (12.0, 0.2 * hz * ratio));
                weakest = std::min (weakest, db (line.amplitude / each));
                detune = std::max (detune, std::abs (cents (line.hz, hz * ratio)));
            }

            // frames of 60 ms: the chord's own beating is faster than that
            std::vector<double> levels;
            for (size_t at = from; at + 2880 <= to; at += 2880)
                levels.push_back (db (rmsOf (out, at, at + 2880)));
            const double spread = *std::max_element (levels.begin(), levels.end()) - *std::min_element (levels.begin(), levels.end());
            const double level = db (rmsOf (out, from, to) / rmsOf (in.samples, from, to));

            std::printf ("   %+5.1f st, chord of %d sines: notes within %.1f cents, weakest %+.1f dB, level %+.2f dB vs the input, spread %.2f dB\n",
                         shift, (int) chord.size(), detune, weakest, level, spread);
            const std::string what = "glide " + num (shift, 1) + " st, chord of " + std::to_string (chord.size()) + ": ";
            check (detune < 15.0, what + "a note of the chord is " + num (detune) + " cents off");
            check (weakest > -3.0, what + "a note of the chord is " + num (weakest) + " dB down at its new pitch");
            check (std::abs (level) < 2.0, what + "level is " + num (level) + " dB vs the input");
            check (spread < 3.0, what + "level varies by " + num (spread) + " dB");
        }

    // Mix 0 % is the dry signal
    {
        const Signal in = pluck (196.0, 0.5);
        const auto out = run (glide, in.samples, knobs (12.0f, 0.0f));
        double difference = 0.0;
        for (size_t i = 0; i < out.size(); ++i)
            difference = std::max (difference, (double) std::abs (out[i] - in.samples[i]));
        check (difference < 1.0e-6, "glide: Mix 0 % should be the dry signal (differs by " + num (difference, 6) + ")");
    }

    // live playing: the wet signal starts within 25 ms of a note
    {
        const Signal in = sine (220.0, 0.6, 0.3f, testFs, 0.2);
        double worst = 0.0;
        for (float shift : { 12.0f, -12.0f, 0.0f })
        {
            const auto out = run (glide, in.samples, knobs (shift, 100.0f));
            size_t at = 0;
            while (at < out.size() && std::abs (out[at]) < 0.15f) ++at;
            const double delay = ((double) at - 0.2 * testFs) / testFs * 1000.0;
            worst = std::max (worst, delay);
            check (delay >= 0.0 && delay < 25.0, "glide " + num (shift, 1) + " st: the wet signal starts " + num (delay) + " ms after the note");
        }
        std::printf ("   a 220 Hz note reaches half its level in the wet signal after at most %.1f ms\n", worst);
    }
}

//==============================================================================
/** Pick attacks: a shifter that jumps around in the recent input can skip an attack (shifting down), play it
    twice (shifting up) or play it late. A quiet note (so that only the click is loud) with a 1.5 ms pick click is played at 12 different
    moments (the read head is somewhere else each time), after silence and while another note rings: the click
    must come out once, at its level, within 15 ms. */
void testAttacks()
{
    std::printf ("-- pick attacks in the wet signal (12 pick times, after silence and over a ringing note)\n");
    struct Case { int variant; harness::Knobs k; const char* name; };
    const Case cases[] = { { glide, knobs (12.0f, 100.0f), "glide +12 st" }, { glide, knobs (7.0f, 100.0f), "glide +7 st" },
                           { glide, knobs (0.0f, 100.0f), "glide 0 st" }, { glide, knobs (-5.0f, 100.0f), "glide -5 st" },
                           { glide, knobs (-12.0f, 100.0f), "glide -12 st" },
                           { harmony, knobs (0.0f, 8.0f, 0.0f, 100.0f), "harmony +3rd" }, { harmony, knobs (0.0f, 5.0f, 0.0f, 100.0f), "harmony -3rd" } };

    for (const auto& c : cases)
    {
        double lowest = 100.0, highest = -100.0, earliest = 1000.0, latest = 0.0;
        int repeats = 0;

        for (int prior = 0; prior < 2; ++prior)
            for (int pick = 0; pick < 12; ++pick)
            {
                const size_t t0 = (size_t) (0.3 * testFs) + (size_t) pick * 97;
                std::vector<float> in ((size_t) (0.5 * testFs), 0.0f);
                harness::Lcg rng { 9u };
                for (auto& v : in)
                    v = 0.0002f * (rng.next01() * 2.0f - 1.0f);
                if (prior != 0)
                    for (size_t i = 0; i < in.size(); ++i)
                        in[i] += 0.02f * (float) (std::sin (2.0 * fx::pi * 196.0 * (double) i / testFs) + 0.5 * std::sin (2.0 * fx::pi * 392.0 * (double) i / testFs));

                for (size_t i = t0; i < in.size(); ++i) // the note: E4 with a second harmonic
                {
                    const double t = (double) (i - t0) / testFs;
                    in[i] += (float) (0.02 * std::exp (-3.0 * t) * (std::sin (2.0 * fx::pi * 329.63 * t) + 0.5 * std::sin (4.0 * fx::pi * 329.63 * t)));
                }
                float lp = 0.0f;
                for (size_t i = 0; i < 72; ++i) // the click: 1.5 ms of noise, about 0.25 rms
                {
                    lp += 0.3f * ((rng.next01() * 2.0f - 1.0f) - lp);
                    in[t0 + i] += 0.9f * lp * (float) std::sin (fx::pi * (double) i / 72.0);
                }

                const auto out = run (c.variant, in, c.k);
                const auto before = fineLevels (in, testFs, t0, 0.010), after = fineLevels (out, testFs, t0, 0.060);
                const double click = *std::max_element (before.begin(), before.end());
                const size_t top = (size_t) (std::max_element (after.begin(), after.end()) - after.begin());

                // anything else that loud, at least 3 ms away, is the click again
                for (size_t i = 1; i + 1 < after.size(); ++i)
                    if (after[i] > 0.5 * after[top] && after[i] >= after[i - 1] && after[i] > after[i + 1] && (i + 12 < top || i > top + 12))
                    {
                        ++repeats;
                        break;
                    }

                const double level = db (after[top] / click), when = 0.25 * (double) top;
                lowest = std::min (lowest, level);
                highest = std::max (highest, level);
                earliest = std::min (earliest, when);
                latest = std::max (latest, when);
            }

        std::printf ("   %-13s click at %+.1f..%+.1f dB of the dry one, after %.1f..%.1f ms, played twice in %d of 24\n",
                     c.name, lowest, highest, earliest, latest, repeats);
        check (lowest > -6.0 && highest < 4.0, std::string (c.name) + ": the pick attack comes out at " + num (lowest) + " .. " + num (highest) + " dB");
        check (latest < 15.0, std::string (c.name) + ": the pick attack comes out after " + num (latest) + " ms");
        check (repeats == 0, std::string (c.name) + ": the pick attack is played twice in " + std::to_string (repeats) + " of 24 cases");
    }
}

//==============================================================================
/** Pitch Glide: moving the Pitch knob glides without clicks. A sine stays a sine whose frequency is at most
    4x the input's, so no step between two samples may be larger than that sine's steepest slope. */
void testGlideSweep()
{
    std::printf ("-- Pitch Glide: moving the Pitch knob\n");
    const double hz = 220.0, amplitude = 0.3;
    const Signal in = sine (hz, 6.0, (float) amplitude);

    std::vector<harness::Step> schedule;
    auto knobAt = [&] (double seconds, float pitch)
    {
        harness::Step step { (int) (seconds * testFs), knobs (pitch, 100.0f) };
        schedule.push_back (step);
    };

    // heel to toe and back like a pedal: -24 -> +24 in 1.5 s, back in 0.3 s (one knob value per block)
    const double block = harness::blockSize / testFs;
    for (double t = 0.5; t < 2.0; t += block) knobAt (t, (float) (-24.0 + 48.0 * (t - 0.5) / 1.5));
    for (double t = 2.0; t < 2.3; t += block) knobAt (t, (float) (24.0 - 48.0 * (t - 2.0) / 0.3));
    // fast wobble of an octave, 4 times a second
    for (double t = 2.5; t < 4.0; t += block) knobAt (t, (float) (6.0 + 6.0 * std::sin (2.0 * fx::pi * 4.0 * t)));
    // sudden jumps (a switch instead of a pedal)
    const float jumps[] = { 12.0f, -12.0f, 24.0f, 0.0f, -24.0f, 7.0f, 24.0f, -24.0f, 12.0f, 0.1f };
    for (int i = 0; i < 10; ++i) knobAt (4.0 + 0.2 * i, jumps[i]);

    const auto out = run (glide, in.samples, knobs (-24.0f, 100.0f), testFs, schedule);

    const double steepest = amplitude * 2.0 * fx::pi * hz * 4.0 / testFs; // of a sine at 4x the input frequency
    const double step = maxStep (out, (size_t) (0.1 * testFs), out.size());
    const double level = db (rmsOf (out, (size_t) (0.5 * testFs), out.size()) / rmsOf (in.samples, (size_t) (0.5 * testFs), out.size()));

    // the level while gliding, in 20 ms frames
    double lo = 1000.0, hi = -1000.0;
    for (size_t at = (size_t) (0.5 * testFs); at + 960 <= out.size(); at += 960)
    {
        const double frame = db (rmsOf (out, at, at + 960) / (amplitude / std::sqrt (2.0)));
        lo = std::min (lo, frame);
        hi = std::max (hi, frame);
    }

    std::printf ("   sweep, wobble and jumps on a 220 Hz sine: largest step %.4f (a clean 880 Hz sine: %.4f), level %+.2f dB, 20 ms frames %+.2f..%+.2f dB\n",
                 step, steepest, level, lo, hi);
    check (step < 1.25 * steepest, "glide sweep clicks: step of " + num (step, 4) + " between two samples, a clean sine has at most " + num (steepest, 4));
    check (lo > -3.0 && hi < 3.0, "glide sweep: the level moves between " + num (lo) + " and " + num (hi) + " dB");

    // the glide itself: after a jump from 0 to +12 the pitch passes through the interval instead of switching
    {
        std::vector<harness::Step> jump { { (int) (0.5 * testFs), knobs (12.0f, 100.0f) } };
        const auto glided = run (glide, sine (hz, 1.0).samples, knobs (0.0f, 100.0f), testFs, jump);
        const Pitch before = measurePitch (glided, testFs, (size_t) (0.3 * testFs), 4800, hz);
        const Pitch after = measurePitch (glided, testFs, (size_t) (0.7 * testFs), 4800, hz * 2.0);
        check (std::abs (cents (before.hz, hz)) < 5.0 && std::abs (cents (after.hz, hz * 2.0)) < 5.0,
               "glide: jump from 0 to +12 st gives " + num (before.hz) + " Hz then " + num (after.hz) + " Hz");
        std::printf ("   jump 0 -> +12 st: %.2f Hz before, %.2f Hz after\n", before.hz, after.hz);
    }
}

//==============================================================================
/** Smart Harmony: every scale degree gets the interval that keeps the harmony in the key. */
void testHarmony()
{
    std::printf ("-- Smart Harmony: wet signal alone (Mix 100 %%)\n");
    using namespace fx::pitch_detail;

    // expected semitones, worked out here by counting scale steps
    const int major[] = { 0, 2, 4, 5, 7, 9, 11 }, minor[] = { 0, 2, 3, 5, 7, 8, 10 };
    auto expected = [&] (int scale, int interval, int degree)
    {
        const int* notes = scale == 0 ? major : minor;
        const int steps = interval > 0 ? interval - 1 : interval + 1, target = degree + steps;
        const int octave = (int) std::floor ((double) target / 7.0);
        return notes[target - 7 * octave] + 12 * octave - notes[degree];
    };
    auto shiftIndex = [] (int interval)
    {
        for (int i = 0; i < numShifts; ++i)
            if (shiftIntervals[i] == interval) return i;
        return -1;
    };

    struct Case { int key, scale, interval; const char* name; };
    const Case cases[] = { { 0, 0, 3, "C major, +3rd" }, { 9, 1, 3, "A minor, +3rd" }, { 7, 0, 5, "G major, +5th" },
                           { 4, 1, -3, "E minor, -3rd" }, { 2, 0, -6, "D major, -6th" }, { 0, 0, 8, "C major, +8th" },
                           { 10, 0, 4, "Bb major, +4th" }, { 9, 1, -4, "A minor, -4th" } };

    for (const auto& c : cases)
    {
        const int* notes = c.scale == 0 ? major : minor;
        double worstCents = 0.0, worstBump = 0.0;
        std::string intervals;

        for (int degree = 0; degree < 7; ++degree)
            for (int kind = 0; kind < 2; ++kind)
            {
                // the scale from the key note upwards, starting between 110 and 208 Hz
                const int midi = 45 + (c.key + 3) % 12 + notes[degree];
                const double hz = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
                const Signal in = kind == 0 ? sine (hz, 0.9) : pluck (hz, 0.9);
                const int want = expected (c.scale, c.interval, degree);
                const double target = in.hz * std::pow (2.0, want / 12.0);

                const auto out = run (harmony, in.samples, knobs ((float) c.key, (float) shiftIndex (c.interval), (float) c.scale, 100.0f));
                const Pitch p = measurePitch (out, testFs, measureFrom, measureLength, target);
                const double off = p.hz > 0.0 ? cents (p.hz, target) : 9999.0;
                const Ripple ripple = levelRipple (out, testFs, (size_t) (0.2 * testFs), out.size() - 2400, target);

                if (verbose)
                    std::printf ("   %-15s degree %d %-5s %7.2f Hz -> %7.2f Hz, want %+3d st (%+6.1f cents)  bump %.2f dB\n",
                                 c.name, degree + 1, in.kind, in.hz, p.hz, want, off, ripple.bump);

                check (std::abs (off) < 5.0, std::string ("harmony ") + c.name + ", degree " + std::to_string (degree + 1) + " (" + in.kind + " "
                                              + num (in.hz) + " Hz): wet is at " + num (p.hz) + " Hz, expected " + std::to_string (want)
                                              + " semitones = " + num (target) + " Hz");
                check (ripple.bump < 2.0, std::string ("harmony ") + c.name + ", degree " + std::to_string (degree + 1) + ": wet level wobbles by " + num (ripple.bump) + " dB");
                worstCents = std::max (worstCents, std::abs (off));
                worstBump = std::max (worstBump, ripple.bump);
                if (kind == 0)
                {
                    intervals += (degree ? " " : "") + std::to_string (want);
                    const Ripple envelope = sineEnvelope (out, testFs, (size_t) (0.2 * testFs), out.size() - 2400, target, 0.3);
                    check (envelope.bump < 1.0 && envelope.spread > -1.0, std::string ("harmony ") + c.name + ", degree " + std::to_string (degree + 1)
                                                                            + ": momentary amplitude of the wet sine moves between " + num (envelope.spread)
                                                                            + " and " + num (envelope.bump) + " dB");
                }
            }
        std::printf ("   %-15s semitones per degree: %-22s worst pitch error %.2f cents, level bump %.2f dB (sine + pluck)\n",
                     c.name, intervals.c_str(), worstCents, worstBump);
    }

    // the scale logic on its own
    {
        auto semis = [&] (int scale, int interval, int pc) { return harmonySemitones (scale, interval, pc); };
        bool ok = true;
        for (int scale = 0; scale < 2; ++scale)
            for (int s = 0; s < numShifts; ++s)
                for (int degree = 0; degree < 7; ++degree)
                    ok = ok && semis (scale, shiftIntervals[s], (scale == 0 ? major : minor)[degree]) == expected (scale, shiftIntervals[s], degree);
        check (ok, "harmony: major / minor intervals differ from counting scale steps");

        // notes outside the scale move like the scale note below them (C# in C major: like C)
        check (semis (0, 3, 1) == 4 && semis (0, 3, 6) == 4 && semis (0, 3, 10) == 3, "harmony: notes outside the scale");
        // harmonic minor: a third above the 7th degree (B in C harmonic minor) is D, above the 6th (Ab) it is C
        check (semis (4, 3, 11) == 3 && semis (4, 3, 8) == 4 && semis (4, 2, 8) == 3, "harmony: harmonic minor");
        // melodic minor: a third above the 4th degree (F) is A
        check (semis (5, 3, 5) == 4 && semis (5, -3, 0) == -3, "harmony: melodic minor");
        // major pentatonic C D E G A, "+3rd": E G G C C (always a scale note, as near to a third as there is)
        check (semis (2, 3, 0) == 4 && semis (2, 3, 2) == 5 && semis (2, 3, 4) == 3 && semis (2, 3, 7) == 5 && semis (2, 3, 9) == 3, "harmony: major pentatonic");
        // minor pentatonic (0 3 5 7 10): a third above the key note and above the fifth is the minor third
        check (semis (3, 3, 0) == 3 && semis (3, 3, 7) == 3 && semis (3, -3, 3) == -3, "harmony: minor pentatonic");
        // whole tone: always a major third; diminished: always a note of the scale
        bool inScale = true;
        for (int pc : { 0, 2, 3, 5, 6, 8, 9, 11 })
        {
            const int target = (pc + semis (7, 3, pc)) % 12;
            inScale = inScale && (target == 0 || target == 2 || target == 3 || target == 5 || target == 6 || target == 8 || target == 9 || target == 11);
        }
        check (semis (6, 3, 4) == 4 && semis (6, 5, 0) == 6 && inScale, "harmony: whole tone / diminished");
        // every scale: the harmony of a scale note is a scale note, and +-8th is an octave
        bool allIn = true;
        for (int scale = 0; scale < numScales; ++scale)
            for (int s = 0; s < numShifts; ++s)
                for (int d = 0; d < scaleSizes[scale]; ++d)
                {
                    const int pc = scaleNotes[scale][d], target = ((pc + semis (scale, shiftIntervals[s], pc)) % 12 + 12) % 12;
                    bool found = false;
                    for (int e = 0; e < scaleSizes[scale]; ++e)
                        found = found || scaleNotes[scale][e] == target;
                    allIn = allIn && found;
                    if (std::abs (shiftIntervals[s]) == 8)
                        allIn = allIn && std::abs (semis (scale, shiftIntervals[s], pc)) == 12;
                }
        check (allIn, "harmony: a harmony note falls outside its scale");
    }

    // a melody: the interval changes from note to note (C major, +3rd: E -> G is 3 semitones, F -> A is 4)
    {
        const double line[] = { 164.81, 174.61, 196.0, 220.0, 246.94, 261.63 }; // E F G A B C
        const int want[] = { 3, 4, 4, 3, 3, 4 };
        Signal in { {}, 0.0, "line" };
        std::vector<double> real;
        for (double hz : line)
        {
            const Signal note = pluck (hz, 0.4);
            in.samples.insert (in.samples.end(), note.samples.begin(), note.samples.end());
            real.push_back (note.hz);
        }
        const auto out = run (harmony, in.samples, knobs (0.0f, (float) shiftIndex (3), 0.0f, 100.0f));
        double worst = 0.0;
        for (size_t i = 0; i < real.size(); ++i)
        {
            const double target = real[i] * std::pow (2.0, want[i] / 12.0);
            const Pitch p = measurePitch (out, testFs, (size_t) ((0.4 * (double) i + 0.15) * testFs), (size_t) (0.24 * testFs), target);
            const double off = p.hz > 0.0 ? cents (p.hz, target) : 9999.0;
            worst = std::max (worst, std::abs (off));
            check (std::abs (off) < 10.0, "harmony: note " + std::to_string (i) + " of a melody (" + num (real[i]) + " Hz) gives " + num (p.hz)
                                           + " Hz, expected +" + std::to_string (want[i]) + " semitones");
        }
        std::printf ("   melody E F G A B C in C major, +3rd: worst pitch error of the harmony %.2f cents\n", worst);
    }
}

//==============================================================================
/** The pitch is right at every sample rate (the tracker and the delay times are derived from it). */
void testSampleRates()
{
    std::printf ("-- other sample rates\n");
    for (double fs : { 44100.0, 88200.0, 96000.0, 192000.0 })
    {
        double worst = 0.0;
        for (double hz : { 82.41, 246.94, 659.26 })
            for (int variant : { (int) octaver, (int) glide, (int) harmony })
            {
                const Signal in = pluck (hz, 0.8, 0.3f, fs);
                // C major +3rd: E (82.41, 659.26) -> +3, B (246.94) -> +3
                const double ratio = variant == octaver ? 0.5 : (variant == glide ? std::pow (2.0, 7.0 / 12.0) : std::pow (2.0, 3.0 / 12.0));
                const auto k = variant == octaver ? knobs (50.0f, 0.0f, 100.0f) : (variant == glide ? knobs (7.0f, 100.0f) : knobs (0.0f, 8.0f, 0.0f, 100.0f));
                const auto out = run (variant, in.samples, k, fs);
                const Pitch p = measurePitch (out, fs, (size_t) (0.4 * fs), (size_t) (0.3 * fs), in.hz * ratio);
                const double off = p.hz > 0.0 ? cents (p.hz, in.hz * ratio) : 9999.0;
                const Ripple ripple = levelRipple (out, fs, (size_t) (0.3 * fs), out.size() - (size_t) (0.05 * fs), in.hz * ratio);
                worst = std::max (worst, std::abs (off));
                check (std::abs (off) < 5.0 && ripple.bump < 2.0, "at " + num (fs, 0) + " Hz, model " + std::to_string (variant) + ", " + num (in.hz) + " Hz: "
                                                                     + num (p.hz) + " Hz (" + num (off) + " cents), level bump " + num (ripple.bump) + " dB");
            }
        std::printf ("   %6.0f Hz: worst pitch error %.2f cents (3 notes x 3 models)\n", fs, worst);
    }
}

} // namespace

int main (int argc, char* argv[])
{
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "test_output/standalone/pitch";
    verbose = argc > 2;

    failed += harness::run<fx::PitchFx> ("pitch", fx::pitchModels(), outDir);

    testOctaver();
    testGlide();
    testAttacks();
    testGlideSweep();
    testHarmony();
    testSampleRates();

    std::printf ("%s\n", failed == 0 ? "ALL CHECKS PASSED" : (std::to_string (failed) + " CHECK(S) FAILED").c_str());
    return failed == 0 ? 0 : 1;
}
