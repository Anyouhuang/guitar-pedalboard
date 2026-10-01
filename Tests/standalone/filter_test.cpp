// Standalone test of the filter / guitar-synth engine (see Harness.h).
//
// After harness::run, the checks below measure what defines each model: frequency responses with multi-tone
// probes (every tone is periodic in the analysis frame, so one DFT bin per tone gives the filter's gain, also
// while the filter moves), the direction and timing of sweeps, steps and triggers, and for the synths the
// pitch, the waveforms' spectra and the silence after the last note.
#include "Harness.h"
#include "../../Source/DSP/fx/Filter.h"

namespace
{
using harness::Knobs;
using V = fx::FilterFx::Variant;
constexpr double fs = harness::testRate;

int failed = 0;

void check (bool ok, const std::string& what)
{
    if (! ok)
    {
        ++failed;
        std::printf ("    FAIL: %s\n", what.c_str());
    }
}

std::string num (double v)
{
    char buffer[40];
    std::snprintf (buffer, sizeof (buffer), "%.4g", v);
    return buffer;
}

bool within (double value, double expected, double tolerance) { return std::abs (value / expected - 1.0) <= tolerance; }

Knobs knobs (std::initializer_list<float> values)
{
    Knobs k {};
    size_t i = 0;
    for (float v : values)
        k[i++] = v;
    return k;
}

int samples (double seconds) { return (int) std::lround (seconds * fs); }

//==============================================================================
struct Stereo { std::vector<float> left, right; };

/** Runs a fresh engine like a slot does (knobs before every block); `change` = knobs from `changeAt` on. */
Stereo run (int variant, const Knobs& k, const std::vector<float>& inLeft, const std::vector<float>& inRight,
            int changeAt = -1, const Knobs* change = nullptr)
{
    fx::FilterFx effect;
    effect.prepare (fs, harness::blockSize);
    effect.setModel (variant);
    effect.setParameters (k.data());
    effect.reset();

    Stereo out { inLeft, inRight };
    for (int pos = 0; pos < (int) inLeft.size(); pos += harness::blockSize)
    {
        const int n = std::min (harness::blockSize, (int) inLeft.size() - pos);
        effect.setParameters ((change != nullptr && pos >= changeAt ? *change : k).data());
        effect.process (out.left.data() + pos, out.right.data() + pos, n);
    }
    return out;
}

Stereo run (int variant, const Knobs& k, const std::vector<float>& input) { return run (variant, k, input, input); }

double rms (const std::vector<float>& x, int start, int length)
{
    double sum = 0.0;
    for (int i = start; i < start + length; ++i)
        sum += (double) x[(size_t) i] * x[(size_t) i];
    return std::sqrt (sum / length);
}

double peak (const std::vector<float>& x, int start, int length)
{
    double p = 0.0;
    for (int i = start; i < start + length; ++i)
        p = std::max (p, (double) std::abs (x[(size_t) i]));
    return p;
}

//==============================================================================
/** A sum of equally loud sines, each periodic in `frame` samples, that repeats a sweep through its band.
    perOctave > 0: spaced evenly on a log axis (fine resolution at any frequency, but the envelope has peaks).
    perOctave < 0: one tone every -perOctave bins with Schroeder phases: a flat envelope, which looks like one
    held note to the attack detector (the log one looks like a series of attacks). */
struct Probe
{
    int frame = 0;
    std::vector<int> bins;
    double amplitude = 0.0; // of each tone
    std::vector<float> signal;

    double hz (size_t i) const { return bins[i] * fs / frame; }
};

Probe makeProbe (double lowHz, double highHz, int perOctave, int frame, double seconds, double level, double startSeconds = 0.0)
{
    Probe p;
    p.frame = frame;
    for (int i = 0;; ++i)
    {
        const double f = perOctave > 0 ? lowHz * std::pow (2.0, (double) i / perOctave) : lowHz - (double) (i * perOctave) * fs / frame;
        if (f > highHz)
            break;
        const int bin = (int) std::lround (f * frame / fs);
        if (p.bins.empty() || bin > p.bins.back())
            p.bins.push_back (bin);
    }

    const size_t count = p.bins.size();
    p.amplitude = level * std::sqrt (2.0 / (double) count);
    p.signal.assign ((size_t) samples (seconds), 0.0f);
    const int start = samples (startSeconds);

    double phase = 0.0;
    for (size_t t = 0; t < count; ++t)
    {
        if (t > 0) // each tone's moment in the sweep (its group delay) is proportional to its number
            phase -= 2.0 * fx::pi * ((double) t / (double) count) * (perOctave > 0 ? p.bins[t] - p.bins[t - 1] : 1);
        const double w = 2.0 * fx::pi * p.bins[t] / frame;
        for (int i = start; i < (int) p.signal.size(); ++i)
            p.signal[(size_t) i] += (float) (p.amplitude * std::cos (w * (i - start) + phase));
    }
    return p;
}

/** The gain of every probe tone in out[start, start + frame). */
std::vector<double> gains (const Probe& p, const std::vector<float>& out, int start)
{
    std::vector<double> g;
    for (int bin : p.bins)
    {
        const double c = 2.0 * std::cos (2.0 * fx::pi * bin / p.frame);
        double s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < p.frame; ++i)
        {
            const double s0 = out[(size_t) (start + i)] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        g.push_back (2.0 * std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / p.frame / p.amplitude);
    }
    return g;
}

struct Peak { double hz = 0.0, gain = 0.0; };

/** Parabola through a local maximum of the response (log frequency, dB). */
Peak refine (const Probe& p, const std::vector<double>& g, size_t i)
{
    if (i == 0 || i + 1 >= g.size())
        return { p.hz (i), g[i] };

    const double x0 = std::log (p.hz (i - 1)), x1 = std::log (p.hz (i)), x2 = std::log (p.hz (i + 1));
    const double y0 = 20.0 * std::log10 (g[i - 1] + 1e-12), y1 = 20.0 * std::log10 (g[i] + 1e-12), y2 = 20.0 * std::log10 (g[i + 1] + 1e-12);
    const double d1 = (y1 - y0) / (x1 - x0), d2 = (y2 - y1) / (x2 - x1), curve = (d2 - d1) / (0.5 * (x2 - x0));
    if (curve >= 0.0)
        return { p.hz (i), g[i] };

    const double x = std::clamp (0.5 * (x0 + x1) - d1 / curve, x0, x2);
    const double y = y1 + 0.5 * (d1 + d2) * (x - x1) + 0.5 * curve * (x - x1) * (x - x1);
    return { std::exp (x), std::pow (10.0, y / 20.0) };
}

Peak highest (const Probe& p, const std::vector<double>& g)
{
    return refine (p, g, (size_t) (std::max_element (g.begin(), g.end()) - g.begin()));
}

/** Local maxima that stand at least `prominenceDb` above the dips on both sides, lowest frequency first. */
std::vector<Peak> peaks (const Probe& p, const std::vector<double>& g, double prominenceDb)
{
    std::vector<Peak> found;
    const double ratio = std::pow (10.0, prominenceDb / 20.0);
    for (size_t i = 1; i + 1 < g.size(); ++i)
    {
        if (g[i] < g[i - 1] || g[i] <= g[i + 1])
            continue;

        double leftDip = g[i], rightDip = g[i];
        for (size_t j = i; j-- > 0 && g[j] <= g[i];)      leftDip = std::min (leftDip, g[j]);
        for (size_t j = i + 1; j < g.size() && g[j] <= g[i]; ++j) rightDip = std::min (rightDip, g[j]);
        if (g[i] >= ratio * leftDip && g[i] >= ratio * rightDip)
            found.push_back (refine (p, g, i));
    }
    return found;
}

/** Q of the highest peak from its -3 dB width. */
double measuredQ (const Probe& p, const std::vector<double>& g)
{
    const size_t top = (size_t) (std::max_element (g.begin(), g.end()) - g.begin());
    const Peak pk = refine (p, g, top);
    const double level = pk.gain / std::sqrt (2.0);
    auto crossing = [&] (int direction)
    {
        for (size_t i = top; i > 0 && i + 1 < g.size(); i = (size_t) ((int) i + direction))
        {
            const size_t next = (size_t) ((int) i + direction);
            if (g[next] < level)
            {
                const double t = (std::log (g[i]) - std::log (level)) / (std::log (g[i]) - std::log (g[next]));
                return std::exp (std::log (p.hz (i)) + t * (std::log (p.hz (next)) - std::log (p.hz (i))));
            }
        }
        return 0.0;
    };
    const double low = crossing (-1), high = crossing (1);
    return low > 0.0 && high > 0.0 ? pk.hz / (high - low) : 0.0;
}

double gainAt (const Probe& p, const std::vector<double>& g, double hz)
{
    size_t best = 0;
    for (size_t i = 0; i < g.size(); ++i)
        if (std::abs (std::log (p.hz (i) / hz)) < std::abs (std::log (p.hz (best) / hz)))
            best = i;
    return g[best];
}

//==============================================================================
/** Amplitude of the sine at `hz` in x[start, start + length), Hann window. */
double toneAmplitude (const std::vector<float>& x, int start, int length, double hz)
{
    const double w = 2.0 * fx::pi * hz / fs, cw = std::cos (w), sw = std::sin (w);
    const double v = 2.0 * fx::pi / length, cv = std::cos (v), sv = std::sin (v);
    double c = 1.0, s = 0.0, hc = std::cos (0.5 * v), hs = std::sin (0.5 * v), re = 0.0, im = 0.0, windowSum = 0.0;
    for (int i = 0; i < length; ++i)
    {
        const double window = 0.5 - 0.5 * hc, sample = window * x[(size_t) (start + i)];
        re += sample * c;
        im += sample * s;
        windowSum += window;
        const double c2 = c * cw - s * sw, hc2 = hc * cv - hs * sv;
        s = s * cw + c * sw;
        c = c2;
        hs = hs * cv + hc * sv;
        hc = hc2;
    }
    return 2.0 * std::sqrt (re * re + im * im) / windowSum;
}

/** The frequency of the strongest sine between lowHz and highHz. */
double strongestHz (const std::vector<float>& x, int start, int length, double lowHz, double highHz, double* amplitude = nullptr)
{
    const double coarse = 0.25 * fs / length;
    double best = lowHz, bestAmp = -1.0;
    for (double f = lowHz; f <= highHz; f += coarse)
    {
        const double a = toneAmplitude (x, start, length, f);
        if (a > bestAmp) { bestAmp = a; best = f; }
    }
    for (double step = coarse; step > 0.002 * coarse; step *= 0.5)
    {
        const double down = toneAmplitude (x, start, length, best - step), up = toneAmplitude (x, start, length, best + step);
        if (down > bestAmp)    { bestAmp = down; best -= step; }
        else if (up > bestAmp) { bestAmp = up; best += step; }
    }
    if (amplitude != nullptr)
        *amplitude = bestAmp;
    return best;
}

//==============================================================================
/** One guitar note starting at 50 ms and stopped (10 ms fade) at `stopSeconds`.
    style 0: ten slightly inharmonic partials with a strong fundamental, each decaying at its own rate, plus
    pick noise.  style 1: the same with a weak fundamental (bridge pickup).  style 2: Karplus-Strong string as in
    the harness (its pitch is rate / (period + 0.5), returned in `actualHz`). Hum and hiss at -70 dB as in the harness. */
std::vector<float> guitarNote (double hz, int style, double seconds, double stopSeconds, float amplitude, double* actualHz = nullptr)
{
    harness::Lcg rng { 77u + (uint32_t) (hz * 10.0) + (uint32_t) style };
    std::vector<float> x ((size_t) samples (seconds), 0.0f);
    const int start = samples (0.05);
    double pitch = hz;

    if (style == 2)
    {
        const int period = std::max (2, (int) std::lround (fs / hz));
        pitch = fs / (period + 0.5);
        std::vector<float> string ((size_t) period);
        for (auto& s : string)
            s = rng.next01() * 2.0f - 1.0f;

        float previous = 0.0f;
        for (int i = start, k = 0; i < (int) x.size(); ++i, ++k)
        {
            const size_t idx = (size_t) (k % period);
            const float current = string[idx];
            string[idx] = 0.996f * 0.5f * (current + previous);
            previous = current;
            x[(size_t) i] = current;
        }
    }
    else
    {
        const double levels[2][10] = { { 1.0, 0.5, 0.33, 0.2, 0.15, 0.1, 0.07, 0.05, 0.03, 0.02 },
                                       { 0.35, 1.0, 0.8, 0.5, 0.3, 0.25, 0.15, 0.1, 0.08, 0.05 } };
        for (int h = 1; h <= 10; ++h)
        {
            const double f = hz * h * std::sqrt (1.0 + 0.00005 * h * h);
            if (f > 0.45 * fs)
                break;
            const double phase = 2.0 * fx::pi * rng.next01(), decay = 1.0 + 0.5 * h;
            for (int i = start; i < (int) x.size(); ++i)
            {
                const double t = (i - start) / fs;
                x[(size_t) i] += (float) (levels[style][h - 1] * std::exp (-decay * t) * std::sin (2.0 * fx::pi * f * t + phase));
            }
        }
        const int pick = samples (0.004);
        for (int i = 0; i < pick; ++i)
            x[(size_t) (start + i)] += 0.5f * (rng.next01() * 2.0f - 1.0f) * (1.0f - (float) i / (float) pick);
    }

    float top = 0.0f;
    for (auto s : x) top = std::max (top, std::abs (s));
    for (auto& s : x) s *= amplitude / top;

    for (size_t i = 0; i < x.size(); ++i)
        x[i] += 0.0003f * (rng.next01() * 2.0f - 1.0f) + 0.0002f * (float) std::sin (2.0 * fx::pi * 60.0 * (double) i / fs);

    const int stop = samples (stopSeconds), fade = samples (0.01);
    for (int i = stop; i < (int) x.size(); ++i)
        x[(size_t) i] *= i < stop + fade ? 1.0f - (float) (i - stop) / (float) fade : 0.0f;

    if (actualHz != nullptr)
        *actualHz = pitch;
    return x;
}

std::vector<float> sine (double hz, double amplitude, double seconds, double stopSeconds = 1.0e9)
{
    std::vector<float> x ((size_t) samples (seconds));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (double) i / fs < stopSeconds ? (float) (amplitude * std::sin (2.0 * fx::pi * hz * (double) i / fs)) : 0.0f;
    return x;
}

//==============================================================================
void testQFilter()
{
    std::printf ("Q Filter: centre frequency, Q, slopes, gain\n");
    const auto probe = makeProbe (40.0, 16000.0, 24, 48000, 2.0, 0.03);

    for (float freq : { 20.0f, 50.0f, 80.0f })
        for (float q : { 40.0f, 70.0f, 100.0f })
        {
            const auto out = run (V::qFilter, knobs ({ freq, q, 0.0f, 1.0f, 100.0f }), probe.signal);
            const auto g = gains (probe, out.left, 48000);
            const double wantHz = 80.0 * std::pow (100.0, freq / 100.0), wantQ = 0.5 * std::pow (32.0, q / 100.0);
            const Peak pk = highest (probe, g);
            const double gotQ = measuredQ (probe, g);
            std::printf ("  Freq %3.0f %%  Q %3.0f %%:  centre %7.1f Hz (want %7.1f)   Q %5.2f (want %5.2f)   peak gain %+5.1f dB\n",
                         freq, q, pk.hz, wantHz, gotQ, wantQ, 20.0 * std::log10 (pk.gain));
            check (within (pk.hz, wantHz, 0.03), "Q Filter centre frequency at Freq " + num (freq) + " Q " + num (q));
            check (within (gotQ, wantQ, 0.12), "Q Filter Q at Freq " + num (freq) + " Q " + num (q));
        }

    {
        // low-pass and high-pass at 800 Hz, Q = 1: flat on one side, 12 dB per octave on the other
        const auto lp = gains (probe, run (V::qFilter, knobs ({ 50.0f, 20.0f, 0.0f, 0.0f, 100.0f }), probe.signal).left, 48000);
        const auto hp = gains (probe, run (V::qFilter, knobs ({ 50.0f, 20.0f, 0.0f, 2.0f, 100.0f }), probe.signal).left, 48000);
        const double lpSlope = 20.0 * std::log10 (gainAt (probe, lp, 6400.0) / gainAt (probe, lp, 3200.0));
        const double hpSlope = 20.0 * std::log10 (gainAt (probe, hp, 100.0) / gainAt (probe, hp, 200.0));
        const double lpPass = 20.0 * std::log10 (gainAt (probe, lp, 100.0)), hpPass = 20.0 * std::log10 (gainAt (probe, hp, 8000.0));
        std::printf ("  LP: %+.1f dB at 100 Hz, %+.1f dB/octave above   HP: %+.1f dB at 8 kHz, %+.1f dB/octave below\n", lpPass, lpSlope, hpPass, hpSlope);
        check (lpSlope < -11.0 && lpSlope > -13.5 && hpSlope < -11.0 && hpSlope > -13.5, "Q Filter LP / HP slopes are 12 dB per octave");
        check (std::abs (lpPass) < 3.0 && std::abs (hpPass) < 3.0, "Q Filter LP / HP pass bands are near unity");
    }

    {
        const auto flat = gains (probe, run (V::qFilter, knobs ({ 50.0f, 60.0f, 0.0f, 1.0f, 100.0f }), probe.signal).left, 48000);
        const auto loud = gains (probe, run (V::qFilter, knobs ({ 50.0f, 60.0f, 6.0f, 1.0f, 100.0f }), probe.signal).left, 48000);
        const auto dry  = gains (probe, run (V::qFilter, knobs ({ 50.0f, 60.0f, 0.0f, 1.0f, 0.0f }), probe.signal).left, 48000);
        const double step = 20.0 * std::log10 (highest (probe, loud).gain / highest (probe, flat).gain);
        check (std::abs (step - 6.0) < 0.2, "Q Filter Gain +6 dB raises the output by " + num (step) + " dB");
        check (std::abs (*std::max_element (dry.begin(), dry.end()) - 1.0) < 0.01 && std::abs (*std::min_element (dry.begin(), dry.end()) - 1.0) < 0.01,
               "Q Filter at Mix 0 % is the dry signal");
    }
}

//==============================================================================
void testStereoClasses()
{
    std::printf ("Stereo classes: TS models keep the sides apart, ST/M models add a mono effect to the stereo input\n");
    const auto models = fx::filterModels();
    const auto note = guitarNote (196.0, 0, 1.0, 0.9, 0.3f);
    const std::vector<float> nothing (note.size(), 0.0f);
    const bool trueStereo[] = { false, true, true, true, false, true, true, true, true, true, true, false, false, false, false, false };

    for (int v = 0; v < (int) V::numVariants; ++v)
    {
        auto k = harness::defaultKnobs (models[(size_t) v]);
        k[4] = 50.0f;
        const auto out = run (v, k, note, nothing);
        const int n = (int) note.size();
        if (trueStereo[v])
        {
            check (peak (out.right, 0, n) == 0.0 && rms (out.left, 0, n) > 0.01, std::string (models[(size_t) v].key) + " (TS): a left-only input stays on the left");
        }
        else
        {
            // right = mix * effect, left = (1 - mix) * input + mix * effect
            double worst = 0.0;
            for (int i = 0; i < n; ++i)
                worst = std::max (worst, (double) std::abs (out.left[(size_t) i] - 0.5f * note[(size_t) i] - out.right[(size_t) i]));
            check (worst < 1.0e-5 && rms (out.right, 0, n) > 0.002, std::string (models[(size_t) v].key) + " (ST/M): the same mono effect on both sides, the dry signal stays where it was");
        }
    }

    // Spin Cycle and Comet Trails make a stereo image out of a mono input
    for (int v : { (int) V::spinCycle, (int) V::cometTrails })
    {
        const auto out = run (v, harness::defaultKnobs (models[(size_t) v]), note);
        double diff = 0.0;
        for (size_t i = 0; i < note.size(); ++i)
            diff += std::pow ((double) out.left[i] - out.right[i], 2.0);
        check (std::sqrt (diff / note.size()) > 0.2 * rms (out.left, 0, (int) note.size()), std::string (models[(size_t) v].key) + ": left and right differ");
    }
}

//==============================================================================
void testVowels()
{
    std::printf ("Voice Box / V-Tron: formants\n");
    const auto probe = makeProbe (120.0, 6000.0, 24, 48000, 2.0, 0.03);

    for (int vowel = 0; vowel < 5; ++vowel)
    {
        const auto out = run (V::voiceBox, knobs ({ 1.0f, (float) vowel, (float) vowel, 0.0f, 100.0f }), probe.signal);
        const auto g = gains (probe, out.left, 48000);
        const double top = *std::max_element (g.begin(), g.end());
        std::string text;
        int matched = 0;
        for (const auto& pk : peaks (probe, g, 1.0))
        {
            if (pk.gain < 0.03 * top) // (the little bumps between the formants, 30 dB down)
                continue;
            text += " " + num (std::round (pk.hz)) + " (" + num (std::round (20.0 * std::log10 (pk.gain))) + " dB)";
            for (int f = 0; f < 3; ++f)
                if (within (pk.hz, fx::filter_detail::vowelHz[vowel][f], 0.06))
                    matched |= 1 << f;
        }
        std::printf ("  %s: formant peaks at%s   (want %.0f %.0f %.0f Hz)\n", fx::filterVowelNames[vowel], text.c_str(),
                     fx::filter_detail::vowelHz[vowel][0], fx::filter_detail::vowelHz[vowel][1], fx::filter_detail::vowelHz[vowel][2]);
        check (matched == 7, std::string ("Voice Box vowel ") + fx::filterVowelNames[vowel] + " has its three formants");
    }

    {
        // Sine pattern at 1 Hz from A to I: A at 0 s and 1 s, I at 0.5 s (first formant 730 Hz <-> 270 Hz)
        const auto fast = makeProbe (150.0, 4000.0, 12, 4800, 2.2, 0.05);
        const auto out = run (V::voiceBox, knobs ({ 1.0f, 0.0f, 2.0f, 0.0f, 100.0f }), fast.signal);
        const double atHalf = highest (fast, gains (fast, out.left, samples (0.45))).hz, atOne = highest (fast, gains (fast, out.left, samples (0.95))).hz;
        const double atOneHalf = highest (fast, gains (fast, out.left, samples (1.45))).hz, atTwo = highest (fast, gains (fast, out.left, samples (1.95))).hz;
        std::printf ("  Voice Box A > I > A at 1 Hz: first formant %.0f, %.0f, %.0f, %.0f Hz at 0.5, 1.0, 1.5, 2.0 s\n", atHalf, atOne, atOneHalf, atTwo);
        check (within (atHalf, 270.0, 0.12) && within (atOne, 730.0, 0.08) && within (atOneHalf, 270.0, 0.12) && within (atTwo, 730.0, 0.08),
               "Voice Box morphs from Start to End and back at the Speed rate");

        // the four Auto patterns are four different movements
        std::vector<Stereo> outs;
        for (float pattern : { 0.0f, 1.0f, 2.0f, 3.0f })
            outs.push_back (run (V::voiceBox, knobs ({ 1.0f, 0.0f, 2.0f, pattern, 100.0f }), fast.signal));
        for (size_t a = 0; a < 4; ++a)
            for (size_t b = a + 1; b < 4; ++b)
            {
                double diff = 0.0;
                for (size_t i = 0; i < fast.signal.size(); ++i)
                    diff += std::pow ((double) outs[a].left[i] - outs[b].left[i], 2.0);
                check (std::sqrt (diff / fast.signal.size()) > 0.2 * rms (outs[a].left, 0, (int) fast.signal.size()),
                       "Voice Box Auto patterns " + num ((double) a) + " and " + num ((double) b) + " differ");
            }
    }

    {
        // V-Tron: a note attack at 0.2 s starts the sweep U -> A (first formant 300 -> 730 Hz), 0.5 s long at 2 Hz
        const auto burst = makeProbe (150.0, 4000.0, -2, 4800, 1.4, 0.05, 0.2);
        for (float mode : { 0.0f, 1.0f })
        {
            const auto out = run (V::vTron, knobs ({ 4.0f, 0.0f, 2.0f, mode, 100.0f }), burst.signal);
            const double early = highest (burst, gains (burst, out.left, samples (0.21))).hz;
            const double half  = highest (burst, gains (burst, out.left, samples (0.4))).hz;
            const double late  = highest (burst, gains (burst, out.left, samples (0.85))).hz;
            std::printf ("  V-Tron %s: first formant %.0f Hz just after the attack, %.0f Hz at half the sweep, %.0f Hz after it\n",
                         fx::filterSweepNames[(int) mode], early, half, late);
            check (within (early, 300.0, 0.12), "V-Tron starts at the Start vowel on an attack");
            if (mode == 0.0f) check (half > 400.0 && half < 650.0 && within (late, 730.0, 0.08), "V-Tron Up sweeps to the End vowel and stays");
            else              check (within (half, 730.0, 0.08) && within (late, 300.0, 0.12), "V-Tron Up-Down reaches the End vowel and returns");
        }
    }
}

//==============================================================================
void testSeeker()
{
    std::printf ("Seeker: step sequence\n");
    const auto probe = makeProbe (200.0, 4000.0, 12, 2400, 4.3, 0.05);

    struct Case { float speed; int pattern, steps; };
    for (const auto& c : { Case { 10.0f, 3, 8 }, Case { 10.0f, 0, 5 }, Case { 8.0f, 6, 9 }, Case { 5.0f, 1, 2 }, Case { 10.0f, 7, 3 } })
    {
        const auto out = run (V::seeker, knobs ({ c.speed, (float) c.pattern, 80.0f, (float) (c.steps - 2), 100.0f }), probe.signal);
        const int total = (int) (4.0 * c.speed);
        int wrong = 0;
        std::string text;
        for (int n = 0; n < total; ++n)
        {
            // a 50 ms frame in the middle of step n
            const double centre = highest (probe, gains (probe, out.left, samples ((n + 0.5) / c.speed - 0.025))).hz;
            const double want = fx::filter_detail::seekerHz (fx::filter_detail::seekerPosition (c.pattern, n % c.steps, c.steps));
            if (! within (centre, want, 0.05))
                ++wrong;
            if (n < c.steps)
                text += " " + num (std::round (centre));
        }
        std::printf ("  %-8s %d steps at %4.1f Hz:%s Hz   (%d of %d steps off)\n", fx::filterPatternNames[c.pattern], c.steps, c.speed, text.c_str(), wrong, total);
        check (wrong == 0, std::string ("Seeker pattern ") + fx::filterPatternNames[c.pattern] + " steps through " + num (c.steps) + " positions at the Speed rate");
    }
}

//==============================================================================
void testObiWah()
{
    std::printf ("Obi Wah: sample & hold\n");
    const auto probe = makeProbe (100.0, 8000.0, 12, 2400, 6.2, 0.05);
    const float speed = 4.0f;
    const auto out = run (V::obiWah, knobs ({ speed, 50.0f, 80.0f, 1.0f, 100.0f }), probe.signal);

    std::vector<double> held;
    int notHeld = 0, changes = 0, outside = 0;
    for (int n = 0; n < 24; ++n)
    {
        // two 50 ms frames inside step n: the same value; the next step: another one
        const double first = highest (probe, gains (probe, out.left, samples ((n + 0.2) / speed))).hz;
        const double second = highest (probe, gains (probe, out.left, samples ((n + 0.7) / speed))).hz;
        if (! within (second, first, 0.03)) ++notHeld;
        if (! held.empty() && ! within (first, held.back(), 0.03)) ++changes;
        if (first < 632.0 / 3.0 || first > 632.0 * 3.0) ++outside; // Freq 50 % = 632 Hz, +-1.5 octaves
        held.push_back (first);
    }
    const auto [low, high] = std::minmax_element (held.begin(), held.end());
    std::printf ("  24 steps at %.0f Hz: %d changes, %d not held, range %.0f .. %.0f Hz\n", speed, changes, notHeld, *low, *high);
    check (notHeld == 0, "Obi Wah holds the frequency during a step");
    check (changes >= 21, "Obi Wah jumps to a new frequency at every clock tick (Speed)");
    check (outside == 0 && *high / *low > 3.0, "Obi Wah's random frequencies spread over the range around Freq");

    // twice the speed: a new value every 125 ms
    const auto faster = run (V::obiWah, knobs ({ 8.0f, 50.0f, 80.0f, 1.0f, 100.0f }), probe.signal);
    int fastChanges = 0;
    double previous = 0.0;
    for (int n = 0; n < 48; ++n)
    {
        const double centre = highest (probe, gains (probe, faster.left, samples ((n + 0.5) / 8.0 - 0.025))).hz;
        if (n > 0 && ! within (centre, previous, 0.03)) ++fastChanges;
        previous = centre;
    }
    check (fastChanges >= 42, "Obi Wah at 8 Hz changes 8 times per second (" + num (fastChanges) + " of 47)");
}

//==============================================================================
void testTron()
{
    std::printf ("Tron Up / Tron Down: envelope sweep\n");
    double centre[2][3] {};
    const double levels[3] = { 0.004, 0.04, 0.25 };

    for (int down = 0; down < 2; ++down)
    {
        for (int l = 0; l < 3; ++l)
        {
            const auto probe = makeProbe (60.0, 12000.0, 24, 48000, 2.0, levels[l]);
            const auto out = run (down ? V::tronDown : V::tronUp, knobs ({ 30.0f, 80.0f, 0.0f, 1.0f, 100.0f }), probe.signal);
            centre[down][l] = highest (probe, gains (probe, out.left, 48000)).hz;
        }
        std::printf ("  Tron %s, Range Hi, Freq 30 %%: centre %.0f / %.0f / %.0f Hz at input levels -48 / -28 / -12 dB\n",
                     down ? "Down" : "Up", centre[down][0], centre[down][1], centre[down][2]);
    }

    const double rest = 150.0 * std::pow (10.0, 0.3); // 299 Hz
    check (within (centre[0][0], rest, 0.15), "Tron Up rests at the Freq / Range frequency");
    check (centre[0][1] > 1.25 * centre[0][0] && centre[0][2] > 1.8 * centre[0][1], "Tron Up opens upwards with the input level");
    check (within (centre[1][0], rest * std::pow (2.0, 3.5), 0.15), "Tron Down rests 3.5 octaves above");
    check (centre[1][1] < centre[1][0] / 1.25 && centre[1][2] < centre[1][1] / 1.8, "Tron Down sweeps downwards with the input level");

    {
        const auto probe = makeProbe (60.0, 12000.0, 24, 48000, 2.0, 0.004);
        const auto lo = run (V::tronUp, knobs ({ 30.0f, 80.0f, 1.0f, 1.0f, 100.0f }), probe.signal);
        const double loCentre = highest (probe, gains (probe, lo.left, 48000)).hz;
        std::printf ("  Range Lo: rests at %.0f Hz\n", loCentre);
        check (within (loCentre / centre[0][0], 0.4, 0.1), "Tron Range Lo is a factor 2.5 below Hi");

        // LP passes the lows, HP the highs
        const auto lp = gains (probe, run (V::tronUp, knobs ({ 40.0f, 30.0f, 0.0f, 0.0f, 100.0f }), probe.signal).left, 48000);
        const auto hp = gains (probe, run (V::tronUp, knobs ({ 40.0f, 30.0f, 0.0f, 2.0f, 100.0f }), probe.signal).left, 48000);
        check (gainAt (probe, lp, 80.0) > 10.0 * gainAt (probe, lp, 4000.0) && gainAt (probe, hp, 4000.0) > 10.0 * gainAt (probe, hp, 80.0),
               "Tron Type LP / HP");
    }

    {
        // a plucked note: the filter opens at the attack and closes as the note dies away
        const auto note = guitarNote (110.0, 0, 1.5, 1.4, 0.4f);
        const auto up = run (V::tronUp, knobs ({ 20.0f, 60.0f, 1.0f, 0.0f, 100.0f }), note); // Lo range, rests at 95 Hz
        const double early = toneAmplitude (up.left, samples (0.06), samples (0.1), 880.0) / toneAmplitude (note, samples (0.06), samples (0.1), 880.0);
        const double late  = toneAmplitude (up.left, samples (1.2), samples (0.1), 880.0) / toneAmplitude (note, samples (1.2), samples (0.1), 880.0);
        std::printf ("  Tron Up (LP) on a plucked note: gain at 880 Hz %+.1f dB at the attack, %+.1f dB after 1.2 s\n", 20.0 * std::log10 (early), 20.0 * std::log10 (late));
        check (early > 4.0 * late, "Tron Up is open at the attack and closes with the note");
    }
}

//==============================================================================
void testThrobber()
{
    std::printf ("Throbber: LFO shapes\n");
    const auto probe = makeProbe (100.0, 8000.0, 12, 4800, 7.0, 0.05);
    const double centre = 150.0 * std::pow (20.0, 0.5), speed = 0.25;
    const double phases[] = { 0.15, 0.35, 0.65, 0.85, 1.15, 1.65 };

    for (int wave = 0; wave < 4; ++wave)
    {
        const auto out = run (V::throbber, knobs ({ (float) speed, 50.0f, 90.0f, (float) wave, 100.0f }), probe.signal);
        std::string text;
        bool ok = true;
        for (double phase : phases)
        {
            const double p = phase - std::floor (phase);
            const double lfo = wave == 0 ? 2.0 * p - 1.0 : wave == 1 ? 1.0 - 2.0 * p : wave == 2 ? 1.0 - 4.0 * std::abs (p - 0.5) : (p < 0.5 ? 1.0 : -1.0);
            const double got = highest (probe, gains (probe, out.left, samples (phase / speed - 0.05))).hz;
            ok = ok && within (got, centre * std::pow (2.0, 1.5 * lfo), 0.08);
            text += " " + num (std::round (got));
        }
        std::printf ("  %-9s cutoff at 0.15 0.35 0.65 0.85 1.15 1.65 cycles:%s Hz\n", fx::filterLfoNames[wave], text.c_str());
        check (ok, std::string ("Throbber ") + fx::filterLfoNames[wave] + " follows its LFO shape at the Speed rate, 1.5 octaves either side of Freq");
    }

    {
        // four poles: 24 dB per octave above the cutoff (LFO parked by the square wave's first half: 150 * 2.83 = 424 Hz)
        const auto fine = makeProbe (100.0, 16000.0, 12, 4800, 1.0, 0.05);
        const auto g = gains (fine, run (V::throbber, knobs ({ 0.05f, 0.0f, 0.0f, 3.0f, 100.0f }), fine.signal).left, samples (0.5));
        const double slope = 20.0 * std::log10 (gainAt (fine, g, 3400.0) / gainAt (fine, g, 1700.0));
        check (slope < -21.0 && slope > -30.0, "Throbber's low-pass falls at 24 dB per octave (" + num (slope) + ")");
    }
}

//==============================================================================
void testSlowFilter()
{
    std::printf ("Slow Filter: attack-triggered sweep\n");
    const auto burst = makeProbe (80.0, 12000.0, -2, 4800, 6.0, 0.05, 0.2);
    const double dark = 100.0 * std::pow (20.0, 0.3); // Freq 30 % = 246 Hz

    for (int down = 0; down < 2; ++down)
    {
        // Speed 0.25 Hz: the sweep takes 4 s
        const auto out = run (V::slowFilter, knobs ({ 30.0f, 90.0f, 0.25f, (float) down, 100.0f }), burst.signal);
        std::string text;
        bool ok = true;
        for (double seconds : { 1.0, 2.0, 3.0, 5.0 })
        {
            const double t = std::min (1.0, 0.25 * seconds), want = dark * std::pow (10000.0 / dark, down ? 1.0 - t : t);
            const double got = highest (burst, gains (burst, out.left, samples (0.2 + seconds - 0.05))).hz;
            ok = ok && within (got, want, 0.08);
            text += " " + num (std::round (got));
        }
        std::printf ("  %-4s at 0.25 Hz: cutoff%s Hz at 1, 2, 3 and 5 s after the attack\n", fx::filterModeNames[down], text.c_str());
        check (ok, std::string ("Slow Filter ") + fx::filterModeNames[down] + " sweeps between Freq and 10 kHz in 1 / Speed seconds after an attack");
    }

    {
        // a second attack starts the sweep again: two notes 0.6 s apart, 2 Hz sweep
        auto notes = guitarNote (196.0, 0, 1.4, 0.55, 0.3f);
        const auto second = guitarNote (196.0, 0, 0.8, 0.75, 0.3f);
        for (size_t i = 0; i < second.size(); ++i)
            notes[i + (size_t) samples (0.6)] += second[i];

        const auto out = run (V::slowFilter, knobs ({ 30.0f, 35.0f, 2.0f, 0.0f, 100.0f }), notes);
        auto brightness = [&] (double t) { return toneAmplitude (out.left, samples (t), samples (0.04), 980.0) / toneAmplitude (notes, samples (t), samples (0.04), 980.0); };
        const double a1 = brightness (0.06), b1 = brightness (0.45), a2 = brightness (0.66), b2 = brightness (1.05);
        std::printf ("  two notes: gain at 980 Hz %+.0f dB > %+.0f dB (first note), %+.0f dB > %+.0f dB (second note)\n",
                     20.0 * std::log10 (a1), 20.0 * std::log10 (b1), 20.0 * std::log10 (a2), 20.0 * std::log10 (b2));
        check (b1 > 5.0 * a1 && b2 > 5.0 * a2 && a2 < 0.3 * b1, "Slow Filter restarts dark on every pick attack and opens");
    }
}

//==============================================================================
void testSpinCycle()
{
    std::printf ("Spin Cycle: wah / anti-wah\n");
    const double centre = 300.0 * std::pow (9.0, 0.5);

    {
        const auto probe = makeProbe (150.0, 6000.0, 12, 4800, 5.4, 0.05);
        const auto out = run (V::spinCycle, knobs ({ 0.25f, 50.0f, 80.0f, 0.0f, 100.0f }), probe.signal);
        bool ok = true;
        std::string text;
        for (double phase : { 0.25, 0.5, 0.75, 1.25 })
        {
            const double lfo = 1.2 * std::sin (2.0 * fx::pi * phase);
            const double l = highest (probe, gains (probe, out.left, samples (phase / 0.25 - 0.05))).hz;
            const double r = highest (probe, gains (probe, out.right, samples (phase / 0.25 - 0.05))).hz;
            ok = ok && within (l, centre * std::pow (2.0, lfo), 0.08) && within (r, centre * std::pow (2.0, -lfo), 0.08);
            text += "  " + num (std::round (l)) + "/" + num (std::round (r));
        }
        std::printf ("  left/right centre at 0.25, 0.5, 0.75, 1.25 cycles:%s Hz\n", text.c_str());
        check (ok, "Spin Cycle's two wahs sweep in opposite directions at the Speed rate");
    }

    // VolSens: count the LFO cycles in 5 s at 0.4 Hz for a quiet and a loud input
    double cycles[2][2] {};
    for (int sens = 0; sens < 2; ++sens)
        for (int loud = 0; loud < 2; ++loud)
        {
            const auto probe = makeProbe (150.0, 6000.0, 12, 2400, 5.1, loud ? 0.25 : 0.01);
            const auto out = run (V::spinCycle, knobs ({ 0.4f, 50.0f, 80.0f, sens ? 100.0f : 0.0f, 100.0f }), probe.signal);
            bool above = false;
            for (int frame = 0; frame < 100; ++frame)
            {
                const double hz = highest (probe, gains (probe, out.left, frame * 2400)).hz;
                if (! above && hz > centre * 1.5) { above = true; cycles[sens][loud] += 1.0; }
                if (above && hz < centre / 1.5) above = false;
            }
        }
    std::printf ("  LFO cycles in 5 s at 0.4 Hz: VolSens 0 %%: %.0f quiet, %.0f loud   VolSens 100 %%: %.0f quiet, %.0f loud\n",
                 cycles[0][0], cycles[0][1], cycles[1][0], cycles[1][1]);
    check (cycles[0][0] == 2.0 && cycles[0][1] == 2.0, "Spin Cycle without VolSens runs at the Speed rate at any level");
    check (cycles[1][1] >= 2.5 * cycles[1][0] && cycles[1][0] >= 2.0, "Spin Cycle's VolSens speeds the sweep up with the input level");
}

//==============================================================================
void testCometTrails()
{
    std::printf ("Comet Trails: seven chasing filters\n");
    const auto probe = makeProbe (100.0, 8000.0, 48, 9600, 14.0, 0.05);
    const auto out = run (V::cometTrails, knobs ({ 0.05f, 50.0f, 100.0f, 0.0f, 100.0f }), probe.signal);
    const double centre = 300.0 * std::pow (10.0, 0.5);

    // 0.18 cycles in: the head is near the top, the others are strung out behind it
    const auto rising = peaks (probe, gains (probe, out.left, samples (3.5)), 6.0);
    std::string text;
    bool ok = rising.size() == 7;
    for (size_t i = 0; i < rising.size(); ++i)
    {
        text += " " + num (std::round (rising[i].hz)) + " (" + num (std::round (20.0 * std::log10 (rising[i].gain))) + " dB)";
        if (ok)
            ok = within (rising[i].hz, centre * std::pow (2.0, 1.5 * std::sin (2.0 * fx::pi * (0.18 - 0.06 * (double) (6 - i)))), 0.06);
    }
    std::printf ("  response peaks after 0.18 cycles:%s Hz\n", text.c_str());
    check (ok, "Comet Trails has seven band-passes, each 6 % of a cycle behind the next");

    // half a cycle later the whole train is on its way down: the head is now the lowest
    const auto falling = peaks (probe, gains (probe, out.left, samples (13.5)), 6.0);
    const auto gLeft = gains (probe, out.left, samples (3.5)), gRight = gains (probe, out.right, samples (3.5));
    check (falling.size() == 7 && within (falling.front().hz, centre * std::pow (2.0, 1.5 * std::sin (2.0 * fx::pi * 0.68)), 0.06),
           "Comet Trails: the train of filters sweeps down again half a cycle later");
    check (! rising.empty() && gainAt (probe, gLeft, rising.back().hz) > 1.5 * gainAt (probe, gRight, rising.back().hz),
           "Comet Trails: the head is louder on the left (stereo spread)");

    const auto louder = run (V::cometTrails, knobs ({ 0.05f, 50.0f, 100.0f, 6.0f, 100.0f }), probe.signal);
    const double step = 20.0 * std::log10 (rms (louder.left, samples (3.0), samples (1.0)) / rms (out.left, samples (3.0), samples (1.0)));
    check (std::abs (step - 6.0) < 0.2, "Comet Trails Gain +6 dB raises the output by " + num (step) + " dB");
}

//==============================================================================
void testOctisynth()
{
    std::printf ("Octisynth: level-controlled oscillator, ring modulator, vibrato\n");
    // a 3 kHz input keeps the ring modulator's products (oscillator +- 3 kHz) out of the way of the oscillator itself
    double hz[3] {};
    const double amplitudes[3] = { 0.02, 0.1, 0.4 };
    for (int l = 0; l < 3; ++l)
    {
        const auto out = run (V::octisynth, knobs ({ 5.0f, 0.0f, 0.0f, 0.0f, 100.0f }), sine (3000.0, amplitudes[l], 1.5));
        hz[l] = strongestHz (out.left, samples (0.8), samples (0.5), 60.0, 2000.0);
    }
    std::printf ("  oscillator at %.0f / %.0f / %.0f Hz for input amplitudes 0.02 / 0.1 / 0.4\n", hz[0], hz[1], hz[2]);
    check (hz[1] > 1.8 * hz[0] && hz[2] > 1.8 * hz[1] && hz[0] > 80.0 && hz[2] < 1900.0, "Octisynth: the input level sets the oscillator frequency");

    {
        // ring modulation: a 220 Hz input appears as oscillator +- 220 Hz
        const auto out = run (V::octisynth, knobs ({ 5.0f, 0.0f, 0.0f, 0.0f, 100.0f }), sine (220.0, 0.1, 1.5));
        double carrier = 0.0;
        const double osc = strongestHz (out.left, samples (0.8), samples (0.5), 60.0, 2000.0, &carrier);
        const double lower = toneAmplitude (out.left, samples (0.8), samples (0.5), osc - 220.0), upper = toneAmplitude (out.left, samples (0.8), samples (0.5), osc + 220.0);
        const double centre = toneAmplitude (out.left, samples (0.8), samples (0.5), 0.5 * (osc + osc + 220.0));
        std::printf ("  220 Hz input: strongest line %.0f Hz, lines 220 Hz either side at %+.1f and %+.1f dB\n", osc, 20.0 * std::log10 (lower / carrier), 20.0 * std::log10 (upper / carrier));
        check ((lower > 0.2 * carrier || upper > 0.2 * carrier) && centre < 0.05 * carrier, "Octisynth ring-modulates the guitar with its oscillator");
    }

    {
        // Freq adds the second harmonic
        const auto plain = run (V::octisynth, knobs ({ 5.0f, 0.0f, 0.0f, 0.0f, 100.0f }), sine (3000.0, 0.1, 1.5));
        const auto rich = run (V::octisynth, knobs ({ 5.0f, 100.0f, 0.0f, 0.0f, 100.0f }), sine (3000.0, 0.1, 1.5));
        const double f = strongestHz (plain.left, samples (0.8), samples (0.5), 60.0, 2000.0);
        const double before = toneAmplitude (plain.left, samples (0.8), samples (0.5), 2.0 * f) / toneAmplitude (plain.left, samples (0.8), samples (0.5), f);
        const double after = toneAmplitude (rich.left, samples (0.8), samples (0.5), 2.0 * f) / toneAmplitude (rich.left, samples (0.8), samples (0.5), f);
        std::printf ("  second harmonic: %+.1f dB at Freq 0 %%, %+.1f dB at Freq 100 %% (relative to the fundamental)\n", 20.0 * std::log10 (before + 1e-9), 20.0 * std::log10 (after));
        check (before < 0.03 && after > 0.5, "Octisynth's Freq knob adds the second harmonic");
    }

    {
        // vibrato: the oscillator frequency swings at the Speed rate, +-half an octave at Depth 100 %
        for (float speed : { 2.0f, 4.0f })
        {
            const auto out = run (V::octisynth, knobs ({ speed, 0.0f, 0.0f, 100.0f, 100.0f }), sine (3000.0, 0.1, 3.0));
            const int frame = samples (0.025), count = 80;
            double re = 0.0, im = 0.0, reOther = 0.0, imOther = 0.0, mean = 0.0;
            std::vector<double> octaves;
            for (int n = 0; n < count; ++n)
                octaves.push_back (std::log2 (strongestHz (out.left, samples (0.9) + n * frame, frame, 150.0, 1200.0)));
            for (double o : octaves) mean += o / count;
            for (int n = 0; n < count; ++n)
            {
                const double t = (n + 0.5) * 0.025, other = speed == 2.0f ? 4.0 : 2.0;
                re += (octaves[(size_t) n] - mean) * std::cos (2.0 * fx::pi * speed * t);
                im += (octaves[(size_t) n] - mean) * std::sin (2.0 * fx::pi * speed * t);
                reOther += (octaves[(size_t) n] - mean) * std::cos (2.0 * fx::pi * other * t);
                imOther += (octaves[(size_t) n] - mean) * std::sin (2.0 * fx::pi * other * t);
            }
            const double depth = 2.0 * std::sqrt (re * re + im * im) / count, otherDepth = 2.0 * std::sqrt (reOther * reOther + imOther * imOther) / count;
            std::printf ("  vibrato at %.0f Hz, Depth 100 %%: +-%.2f octaves\n", speed, depth);
            check (depth > 0.35 && depth < 0.6 && otherDepth < 0.1, "Octisynth vibrato at " + num (speed) + " Hz");
        }
    }

    {
        const auto out = run (V::octisynth, knobs ({ 5.0f, 40.0f, 50.0f, 30.0f, 100.0f }), guitarNote (196.0, 0, 2.5, 1.0, 0.3f));
        check (rms (out.left, samples (0.1), samples (0.5)) > 0.01 && peak (out.left, samples (2.0), samples (0.5)) < 1.0e-4, "Octisynth is silent when the input stops");
    }
}

//==============================================================================
void testSynths()
{
    std::printf ("Synths: pitch tracking\n");
    const auto models = fx::filterModels();
    const double notes[] = { 82.41, 110.0, 146.83, 196.0, 329.63, 440.0, 659.26 };

    for (int v = V::synthOMatic; v <= V::growler; ++v)
    {
        const auto& m = models[(size_t) v];
        std::printf ("  %-14s", m.name);
        double worstCents = 0.0, worstOnset = 0.0;
        int bad = 0;

        for (int style : { 0, 2 })
            for (double hz : notes)
                for (float semis : { 0.0f, 12.0f, -12.0f, 7.0f })
                {
                    if (semis != 0.0f && (style != 0 || (hz != 110.0 && hz != 329.63)))
                        continue;

                    double played = hz;
                    const auto note = guitarNote (hz, style, 2.4, 0.9, 0.3f, &played);
                    auto k = harness::defaultKnobs (m);
                    k[3] = semis;
                    k[4] = 100.0f;
                    if (v == V::attackSynth) k[2] = 100.0f; // fastest filter attack: the start of the note is then the tracker's delay
                    if (v == V::synthString) k[2] = 20.0f;  // a quicker attack, so the pad is there within the note
                    // Pulse-width modulation moves the pulses' edges, i.e. the pitch, up and down at the Speed rate (that is
                    // its sound); slowed right down, the spectrum shows the oscillators' own frequency.
                    if (v == V::synthString || v == V::growler) k[0] = 0.05f;
                    const auto out = run (v, k, note);

                    const double want = played * std::pow (2.0, semis / 12.0);
                    double amplitude = 0.0;
                    const double got = strongestHz (out.left, samples (0.35), samples (0.5), want * 0.94, want * 1.06, &amplitude);
                    const double cents = 1200.0 * std::log2 (got / want);
                    const double below = toneAmplitude (out.left, samples (0.35), samples (0.5), 0.5 * got);
                    const double between = toneAmplitude (out.left, samples (0.35), samples (0.5), 1.5 * got);
                    worstCents = std::abs (cents) > std::abs (worstCents) ? cents : worstCents;

                    // (Synth String: two oscillators 7 cents apart, the stronger one pulls the reading)
                    const bool inTune = std::abs (cents) < (v == V::synthString ? 8.0 : 6.0), fundamental = amplitude > 0.003 && below < 0.1 * amplitude && between < 0.1 * amplitude;
                    const bool silent = peak (out.left, samples (2.1), samples (0.3)) < 1.0e-4;
                    if (! inTune || ! fundamental || ! silent)
                    {
                        ++bad;
                        std::printf ("\n    %s: %.2f Hz note (style %d, Pitch %+.0f): %.2f Hz, %+.1f cents, level %.3f, octave below %.3f, tail %.5f",
                                     m.name, played, style, semis, got, cents, amplitude, below, peak (out.left, samples (2.1), samples (0.3)));
                    }

                    if (v != V::synthString && semis == 0.0f)
                    {
                        int onset = samples (0.05);
                        while (onset < (int) out.left.size() && std::abs (out.left[(size_t) onset]) < 0.003f)
                            ++onset;
                        const double latency = onset / fs - 0.05;
                        worstOnset = std::max (worstOnset, latency * hz); // in periods of the note
                        if (latency > 0.012 + 3.0 / hz)
                        {
                            ++bad;
                            std::printf ("\n    %s: %.2f Hz note (style %d) starts after %.1f ms", m.name, played, style, latency * 1000.0);
                        }
                    }
                }

        if (v == V::synthString) std::printf (" worst pitch error %+.1f cents (start not timed: it has its own Attack knob)\n", worstCents);
        else                     std::printf (" worst pitch error %+.1f cents, slowest start %.1f periods of the note\n", worstCents, worstOnset);
        check (bad == 0, std::string (m.name) + " plays the note's pitch (times the Pitch knob) within 6 cents (Synth String: 8), starts within 12 ms + 3 periods and is silent 1.2 s after the note");
    }

    {
        // a line of notes without gaps: every note is found, no wrong pitches in between
        const double line[] = { 146.83, 164.81, 196.0, 220.0, 246.94, 293.66, 329.63, 220.0, 110.0, 82.41 };
        std::vector<float> input ((size_t) samples (3.4), 0.0f);
        for (int n = 0; n < 10; ++n)
        {
            const auto note = guitarNote (line[n], n % 2, 0.4, 0.34, 0.3f);
            for (size_t i = 0; i < note.size(); ++i)
                input[i + (size_t) samples (0.3 * n)] += note[i];
        }
        const auto out = run (V::synthOMatic, knobs ({ 100.0f, 0.0f, 3.0f, 0.0f, 100.0f }), input); // triangle, filter open
        int wrong = 0;
        std::string text;
        for (int n = 0; n < 10; ++n)
        {
            const double got = strongestHz (out.left, samples (0.3 * n + 0.13), samples (0.2), 60.0, 700.0);
            if (std::abs (1200.0 * std::log2 (got / line[n])) > 10.0) ++wrong;
            text += " " + num (std::round (got * 10.0) / 10.0);
        }
        std::printf ("  legato line:%s Hz\n", text.c_str());
        check (wrong == 0, "the tracker follows a line of ten notes (neck and bridge pickup spectra)");
    }

    {
        // a soft note and a bend
        const auto soft = run (V::synthOMatic, harness::defaultKnobs (models[(size_t) V::synthOMatic]), guitarNote (196.0, 0, 1.0, 0.9, 0.02f));
        const double got = strongestHz (soft.left, samples (0.2), samples (0.3), 180.0, 210.0);
        check (std::abs (1200.0 * std::log2 (got / 196.0)) < 6.0, "a soft note (-34 dB) is tracked too");

        std::vector<float> bend ((size_t) samples (1.5));
        double phase = 0.0;
        for (size_t i = 0; i < bend.size(); ++i)
        {
            const double t = (double) i / fs, hz = 220.0 * std::pow (2.0, std::clamp ((t - 0.3) / 0.4, 0.0, 1.0) * 2.0 / 12.0);
            phase += hz / fs;
            bend[i] = (float) (0.3 * std::exp (-1.5 * t) * (std::sin (2.0 * fx::pi * phase) + 0.4 * std::sin (4.0 * fx::pi * phase) + 0.2 * std::sin (6.0 * fx::pi * phase)));
        }
        const auto bent = run (V::synthOMatic, knobs ({ 100.0f, 0.0f, 3.0f, 0.0f, 100.0f }), bend);
        const double before = strongestHz (bent.left, samples (0.1), samples (0.2), 200.0, 260.0), after = strongestHz (bent.left, samples (0.8), samples (0.3), 200.0, 260.0);
        const double mid = strongestHz (bent.left, samples (0.47), samples (0.06), 200.0, 260.0);
        std::printf ("  whole-tone bend from 220 Hz: %.1f Hz before, %.1f Hz half way, %.1f Hz after\n", before, mid, after);
        check (within (before, 220.0, 0.004) && within (after, 246.94, 0.004) && mid > 225.0 && mid < 242.0, "the oscillator follows a string bend");
    }
}

//==============================================================================
/** The pitch tracker on its own, on 300 random notes between E2 and E5 (4 per second, each with one of four
    spectra: strong fundamental, weak fundamental, bright, Karplus-Strong), at several sample rates and under
    harder conditions. Counts the share of time (from 15 ms + 3 periods after each note starts) in which the
    tracker reports the played note within 3 %. */
struct TrackerScore { double right = 0.0, octave = 0.0, latencyMs = 0.0; int lost = 0; };

TrackerScore trackRandomNotes (double rate, int condition) // 0 clean, 1 clipped, 2 noisy (-48 dB), 3 / 4 earlier notes ring on at -24 / -18 dB
{
    harness::Lcg rng { 4242u };
    const int numNotes = 300;
    const double spacing = 0.25, ringDb = condition == 3 ? -24.0 : condition == 4 ? -18.0 : 0.0;
    std::vector<float> x ((size_t) ((numNotes * spacing + 1.0) * rate), 0.0f);
    std::vector<double> pitch ((size_t) numNotes);

    for (int n = 0; n < numNotes; ++n)
    {
        const double hz = 82.41 * std::pow (2.0, std::floor (rng.next01() * 37.0) / 12.0);
        const int profile = (int) (rng.next01() * 4.0) % 4;
        const float amplitude = ringDb < 0.0 ? 0.2f + 0.18f * rng.next01() : 0.08f + 0.3f * rng.next01();
        const int length = (int) ((ringDb < 0.0 ? 1.0 : spacing - 0.01) * rate), start = (int) (n * spacing * rate);
        std::vector<float> note ((size_t) length, 0.0f);
        pitch[(size_t) n] = hz;

        if (profile == 3)
        {
            const int period = std::max (2, (int) std::lround (rate / hz));
            pitch[(size_t) n] = rate / (period + 0.5);
            std::vector<float> string ((size_t) period);
            for (auto& v : string) v = rng.next01() * 2.0f - 1.0f;
            float previous = 0.0f;
            for (int i = 0; i < length; ++i)
            {
                const size_t idx = (size_t) (i % period);
                const float current = string[idx];
                string[idx] = 0.996f * 0.5f * (current + previous);
                previous = current;
                note[(size_t) i] = current;
            }
        }
        else
        {
            const double levels[3][10] = { { 1.0, 0.5, 0.33, 0.2, 0.15, 0.1, 0.07, 0.05, 0.03, 0.02 },
                                           { 0.3, 1.0, 0.8, 0.5, 0.3, 0.25, 0.15, 0.1, 0.08, 0.05 },
                                           { 0.6, 0.7, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.25, 0.2 } };
            for (int h = 1; h <= 10; ++h)
            {
                const double f = hz * h * std::sqrt (1.0 + 0.0001 * h * h);
                if (f > 0.45 * rate)
                    break;
                const double phase = 2.0 * fx::pi * rng.next01(), decay = 1.2 + 0.6 * h;
                for (int i = 0; i < length; ++i)
                    note[(size_t) i] += (float) (levels[profile][h - 1] * std::exp (-decay * i / rate) * std::sin (2.0 * fx::pi * f * i / rate + phase));
            }
            const int pick = (int) (0.004 * rate);
            for (int i = 0; i < pick; ++i)
                note[(size_t) i] += 0.6f * (rng.next01() * 2.0f - 1.0f) * (1.0f - (float) i / (float) pick);
        }

        // a ringing note has dropped by ringDb when the next one starts
        auto energy = [&] (double from, double to) { double e = 1.0e-12; for (int i = (int) (from * rate); i < (int) (to * rate); ++i) e += note[(size_t) i] * note[(size_t) i]; return e; };
        const double extra = ringDb < 0.0 ? std::max (0.0, (-ringDb - 10.0 * std::log10 (energy (0.0, 0.05) / energy (0.25, 0.3))) / 8.686 / spacing) : 0.0;
        float top = 0.0f;
        for (auto v : note) top = std::max (top, std::abs (v));
        for (int i = 0; i < length && start + i < (int) x.size(); ++i)
            x[(size_t) (start + i)] += note[(size_t) i] * amplitude / top * (float) std::exp (-extra * i / rate);
    }

    for (auto& v : x)
    {
        if (condition == 1) v = (float) (0.3 * std::tanh (12.0 * v));
        v += (condition == 2 ? 0.004f : 0.0003f) * (rng.next01() * 2.0f - 1.0f);
    }

    fx::filter_detail::PitchTracker tracker;
    fx::filter_detail::LevelMeter meter;
    const int ctl = 16 * std::max (1, (int) std::floor (rate / 48000.0 + 0.5)); // as the engine runs them
    tracker.prepare (rate);
    meter.prepare ((int) std::ceil (0.0135 / (ctl / rate)));

    TrackerScore score;
    std::vector<bool> found ((size_t) numNotes, false);
    int good = 0, total = 0, octaves = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        tracker.process (x[i]);
        meter.push (std::abs (x[i]));
        if (i % (size_t) ctl != 0)
            continue;

        tracker.level = meter.endBlock();
        const double t = (double) i / rate;
        const int n = (int) (t / spacing);
        if (n >= numNotes)
            break;

        const double since = t - n * spacing, ratio = tracker.freq / pitch[(size_t) n];
        const bool right = tracker.voiced && std::abs (ratio - 1.0) < 0.03;
        if (right && ! found[(size_t) n])
        {
            found[(size_t) n] = true;
            score.latencyMs += 1000.0 * since;
        }
        if (since > 0.015 + 3.0 / pitch[(size_t) n] && since < spacing - 0.02)
        {
            ++total;
            if (right) ++good;
            else if (tracker.voiced && (std::abs (ratio - 2.0) < 0.06 || std::abs (ratio - 0.5) < 0.015)) ++octaves;
        }
    }

    for (bool f : found)
        if (! f)
            ++score.lost;
    score.right = 100.0 * good / total;
    score.octave = 100.0 * octaves / total;
    score.latencyMs /= std::max (1, numNotes - score.lost);
    return score;
}

void testTracker()
{
    std::printf ("Pitch tracker: 300 random notes E2..E5, four spectra\n");
    struct Case { double rate; int condition; double mustBeRight; const char* name; };
    for (const auto& c : { Case { 48000.0, 0, 99.5, "clean" }, Case { 44100.0, 0, 99.0, "clean" }, Case { 96000.0, 0, 99.5, "clean" }, Case { 192000.0, 0, 99.5, "clean" },
                           Case { 48000.0, 1, 99.5, "clipped hard (after a distortion)" }, Case { 48000.0, 2, 99.5, "hiss at -48 dB" },
                           Case { 48000.0, 3, 98.0, "earlier notes ringing on, 24 dB down" }, Case { 48000.0, 4, 93.0, "earlier notes ringing on, 18 dB down" } })
    {
        const auto score = trackRandomNotes (c.rate, c.condition);
        std::printf ("  %6.0f Hz, %-38s right %6.2f %% of the time, an octave off %5.2f %%, %d notes never found, found after %.1f ms on average\n",
                     c.rate, c.name, score.right, score.octave, score.lost, score.latencyMs);
        check (score.right >= c.mustBeRight, std::string ("pitch tracker, ") + c.name + " at " + num (c.rate) + " Hz: right " + num (score.right) + " % of the time");
    }
}

//==============================================================================
void testSynthSounds()
{
    std::printf ("Synths: waveforms, filters, envelopes\n");
    const auto note = guitarNote (196.0, 0, 2.0, 1.6, 0.3f);
    auto harmonic = [] (const std::vector<float>& x, double t, double hz) { return toneAmplitude (x, samples (t), samples (0.4), hz); };

    {
        // Synth O Matic: the eight waves through the open filter. Harmonics 2, 3 and the octave below, relative to the fundamental.
        std::printf ("  Synth O Matic waves (harmonic 2, harmonic 3, sub-octave, in dB re the fundamental):\n");
        double h2[8], h3[8], sub[8];
        for (int wave = 0; wave < 8; ++wave)
        {
            const auto out = run (V::synthOMatic, knobs ({ 100.0f, 0.0f, (float) wave, 0.0f, 100.0f }), note);
            const double f = strongestHz (out.left, samples (0.4), samples (0.4), 190.0, 202.0);
            const double base = harmonic (out.left, 0.4, f);
            h2[wave] = harmonic (out.left, 0.4, 2.0 * f) / base;
            h3[wave] = harmonic (out.left, 0.4, 3.0 * f) / base;
            sub[wave] = harmonic (out.left, 0.4, 0.5 * f) / base;
            std::printf ("    %-9s %+6.1f %+6.1f %+6.1f\n", fx::filterSynthWaveNames[wave], 20.0 * std::log10 (h2[wave] + 1e-6), 20.0 * std::log10 (h3[wave] + 1e-6), 20.0 * std::log10 (sub[wave] + 1e-6));
        }
        check (within (h2[0], 0.5, 0.15) && within (h3[0], 0.333, 0.15), "Saw: harmonics fall as 1/n");
        check (h2[1] < 0.03 && within (h3[1], 0.333, 0.15), "Square: odd harmonics only");
        check (h2[2] > 0.6 && h3[2] > 0.4, "Pulse: even and odd harmonics, brighter than the square");
        check (h2[3] < 0.03 && within (h3[3], 0.111, 0.2), "Triangle: odd harmonics falling as 1/n^2");
        check (within (h2[4], 1.0 / 6.0, 0.15) && within (h3[4], 0.333, 0.15), "Saw+Sqr: the square doubles the odd harmonics (harmonic 2 at 1/6)");
        check (h2[6] > 0.9 && sub[6] < 0.03, "Octave: a second saw an octave up");
        check (sub[7] > 0.7 && sub[0] < 0.03, "Sub: a square an octave below");

        // Detune: two saws 12 cents apart beat at 196 * 0.007 = 1.4 Hz
        const auto detuned = run (V::synthOMatic, knobs ({ 100.0f, 0.0f, 5.0f, 0.0f, 100.0f }), sine (196.0, 0.3, 3.0));
        double low = 1e9, high = 0.0;
        for (int n = 0; n < 40; ++n)
        {
            const double a = toneAmplitude (detuned.left, samples (0.5 + 0.05 * n), samples (0.05), 196.7);
            low = std::min (low, a);
            high = std::max (high, a);
        }
        check (high > 4.0 * low, "Detune: two oscillators beating");
    }

    {
        // Aliasing: a saw at 1318.5 Hz (E6, the highest fretted note) and an octave above, filter wide open. The
        // harmonics above 24 kHz fold back to 48000 - k f; a naive saw has them at 1/k (-25 .. -35 dB).
        for (float semis : { 0.0f, 12.0f })
        {
            const auto out = run (V::synthOMatic, knobs ({ 100.0f, 0.0f, 0.0f, semis, 100.0f }), sine (1318.5, 0.3, 1.5));
            double fundamental = 0.0, worst = 0.0;
            const double f = strongestHz (out.left, samples (0.5), samples (0.5), 1300.0 * (1.0 + semis / 12.0), 1340.0 * (1.0 + semis / 12.0), &fundamental);
            for (int k = (int) (24000.0 / f) + 1; k * f < 47000.0; ++k)
                if (fs - k * f < 12000.0)
                    worst = std::max (worst, toneAmplitude (out.left, samples (0.5), samples (0.5), fs - k * f));
            std::printf ("  saw at %.1f Hz: strongest alias below 12 kHz at %.1f dB re the fundamental\n", f, 20.0 * std::log10 (worst / fundamental));
            check (fundamental > 0.01 && worst < 0.004 * fundamental, "the oscillators' aliases stay 48 dB below the fundamental");
        }
    }

    {
        // Synth O Matic's filter: Freq is the cutoff, Q the resonance
        const auto dark = run (V::synthOMatic, knobs ({ 30.0f, 0.0f, 0.0f, 0.0f, 100.0f }), note);   // 372 Hz
        const auto open = run (V::synthOMatic, knobs ({ 100.0f, 0.0f, 0.0f, 0.0f, 100.0f }), note);
        const auto peaky = run (V::synthOMatic, knobs ({ 58.0f, 100.0f, 0.0f, 0.0f, 100.0f }), note); // 1270 Hz
        const auto flat = run (V::synthOMatic, knobs ({ 58.0f, 0.0f, 0.0f, 0.0f, 100.0f }), note);
        const double f = 196.0;
        const double darkRatio = harmonic (dark.left, 0.4, 6.0 * f) / harmonic (open.left, 0.4, 6.0 * f) * harmonic (open.left, 0.4, f) / harmonic (dark.left, 0.4, f);
        const double resonance = harmonic (peaky.left, 0.4, 6.0 * f) / harmonic (peaky.left, 0.4, f) / (harmonic (flat.left, 0.4, 6.0 * f) / harmonic (flat.left, 0.4, f));
        std::printf ("  Synth O Matic filter: Freq 30 %% takes harmonic 6 down by %.1f dB; Q 100 %% lifts the harmonic at the cutoff by %.1f dB\n",
                     -20.0 * std::log10 (darkRatio), 20.0 * std::log10 (resonance));
        check (darkRatio < 0.15 && resonance > 5.0, "Synth O Matic: low-pass with resonance");
    }

    {
        // Attack Synth: the filter opens after the attack, faster with more Speed; three waves
        auto brightness = [&] (float speed, double t)
        {
            const auto out = run (V::attackSynth, knobs ({ 80.0f, 2.0f, speed, 0.0f, 100.0f }), note);
            return toneAmplitude (out.left, samples (0.05 + t), samples (0.05), 5.0 * 196.0);
        };
        const double slowEarly = brightness (25.0f, 0.04), slowLate = brightness (25.0f, 1.0), fastEarly = brightness (100.0f, 0.04), fastLate = brightness (100.0f, 1.0);
        std::printf ("  Attack Synth, harmonic 5 at 40 ms / 1 s after the attack: Speed 25 %%: %.4f / %.4f   Speed 100 %%: %.4f / %.4f\n", slowEarly, slowLate, fastEarly, fastLate);
        check (slowLate > 20.0 * slowEarly && fastEarly > 0.3 * fastLate, "Attack Synth: the filter opens at the rate set by Speed");

        const auto low = run (V::attackSynth, knobs ({ 20.0f, 2.0f, 100.0f, 0.0f, 100.0f }), note); // stops at 418 Hz
        const auto high = run (V::attackSynth, knobs ({ 100.0f, 2.0f, 100.0f, 0.0f, 100.0f }), note);
        check (harmonic (low.left, 0.6, 5.0 * 196.0) < 0.05 * harmonic (high.left, 0.6, 5.0 * 196.0), "Attack Synth: Freq is where the filter stops");

        const auto square = run (V::attackSynth, knobs ({ 100.0f, 0.0f, 100.0f, 0.0f, 100.0f }), note);
        const auto ramp = run (V::attackSynth, knobs ({ 100.0f, 2.0f, 100.0f, 0.0f, 100.0f }), note);
        const auto pwm = run (V::attackSynth, knobs ({ 100.0f, 1.0f, 100.0f, 0.0f, 100.0f }), sine (196.0, 0.3, 3.0));
        double low2 = 1e9, high2 = 0.0;
        for (int n = 0; n < 30; ++n)
        {
            const double a = toneAmplitude (pwm.left, samples (0.5 + 0.08 * n), samples (0.08), 392.0);
            low2 = std::min (low2, a);
            high2 = std::max (high2, a);
        }
        check (harmonic (square.left, 0.4, 392.0) < 0.03 * harmonic (square.left, 0.4, 196.0) && harmonic (ramp.left, 0.4, 392.0) > 0.35 * harmonic (ramp.left, 0.4, 196.0) && high2 > 5.0 * low2,
               "Attack Synth waves: square (odd harmonics), PWM (harmonic 2 comes and goes), ramp (all harmonics)");
    }

    {
        // Synth String: Attack sets the rise time; Speed moves the pulse widths (harmonic 2 comes and goes) and the pitch
        const auto held = sine (196.0, 0.3, 3.0, 2.0);
        const auto quick = run (V::synthString, knobs ({ 3.0f, 60.0f, 0.0f, 0.0f, 100.0f }), held);
        const auto slow = run (V::synthString, knobs ({ 3.0f, 60.0f, 80.0f, 0.0f, 100.0f }), held); // 0.6 s
        const double quickEarly = rms (quick.left, samples (0.1), samples (0.1)), quickLate = rms (quick.left, samples (1.5), samples (0.3));
        const double slowEarly = rms (slow.left, samples (0.1), samples (0.1)), slowMid = rms (slow.left, samples (0.6), samples (0.1)), slowLate = rms (slow.left, samples (1.7), samples (0.3));
        std::printf ("  Synth String level at 0.15 s / 1.7 s: Attack 0 %%: %.3f / %.3f   Attack 80 %%: %.3f / %.3f\n", quickEarly, quickLate, slowEarly, slowLate);
        check (quickEarly > 0.7 * quickLate && slowEarly < 0.3 * slowLate && slowMid > 0.45 * slowLate && slowMid < 0.85 * slowLate, "Synth String: Attack sets the rise time");
        check (rms (quick.left, samples (2.1), samples (0.1)) > 0.2 * quickLate && peak (quick.left, samples (2.85), samples (0.15)) < 0.01 * quickLate, "Synth String: pad-like release, then silence");

        // Pulse-width modulation at the Speed rate puts side lines at multiples of Speed next to each oscillator's
        // lines (the String's two oscillators play 440 Hz -0.2 % and +0.2 %): 2 Hz below at Speed 2, 5 Hz below at Speed 5.
        for (int v : { (int) V::synthString, (int) V::growler })
            for (float speed : { 2.0f, 5.0f })
            {
                const auto out = run (v, knobs ({ speed, 100.0f, v == V::growler ? 50.0f : 0.0f, 0.0f, 100.0f }), sine (440.0, 0.3, 5.2));
                const double base = v == V::synthString ? 440.0 * 0.998 : 440.0;
                const double line = toneAmplitude (out.left, samples (1.0), samples (4.0), base);
                const double at2 = toneAmplitude (out.left, samples (1.0), samples (4.0), base - 2.0), at5 = toneAmplitude (out.left, samples (1.0), samples (4.0), base - 5.0);
                std::printf ("  %s Speed %.0f Hz: side lines at -2 Hz %+.1f dB, at -5 Hz %+.1f dB (re the oscillator's line)\n",
                             v == V::growler ? "Growler" : "Synth String", speed, 20.0 * std::log10 (at2 / line), 20.0 * std::log10 (at5 / line));
                check (speed == 2.0f ? (at2 > 0.2 * line && at5 < 0.25 * at2) : (at5 > 0.2 * line && at2 < 0.25 * at5),
                       std::string (v == V::growler ? "Growler" : "Synth String") + ": the pulse width moves at the Speed rate (" + num (speed) + " Hz)");
            }

        const auto bright = run (V::synthString, knobs ({ 3.0f, 100.0f, 0.0f, 0.0f, 100.0f }), held);
        const auto mellow = run (V::synthString, knobs ({ 3.0f, 10.0f, 0.0f, 0.0f, 100.0f }), held); // 417 Hz
        check (rms (mellow.left, samples (1.0), samples (0.5)) > 0.02 && toneAmplitude (mellow.left, samples (1.0), samples (0.5), 7.0 * 196.0) < 0.2 * toneAmplitude (bright.left, samples (1.0), samples (0.5), 7.0 * 196.0),
               "Synth String: Freq is the tone control");
    }

    {
        // Growler: the envelope opens the filter; louder playing = brighter
        const auto loud = run (V::growler, knobs ({ 1.5f, 35.0f, 65.0f, 0.0f, 100.0f }), sine (110.0, 0.4, 1.5));
        const auto soft = run (V::growler, knobs ({ 1.5f, 35.0f, 65.0f, 0.0f, 100.0f }), sine (110.0, 0.03, 1.5));
        auto tilt = [&] (const Stereo& s) { return rms (s.left, samples (0.5), samples (0.9)); };
        auto high = [&] (const Stereo& s)
        {
            double sum = 0.0;
            for (int h = 8; h <= 14; ++h)
                sum += std::pow (toneAmplitude (s.left, samples (0.5), samples (0.9), 110.0 * h), 2.0);
            return std::sqrt (sum);
        };
        const double loudBright = high (loud) / tilt (loud), softBright = high (soft) / tilt (soft);
        std::printf ("  Growler: harmonics 8..14 are %.1f dB below the whole signal when played hard, %.1f dB when played softly\n",
                     -20.0 * std::log10 (loudBright), -20.0 * std::log10 (softBright));
        check (loudBright > 5.0 * softBright, "Growler: the envelope filter opens with the playing level");

        const auto plucked = run (V::growler, knobs ({ 1.5f, 35.0f, 65.0f, 0.0f, 100.0f }), guitarNote (110.0, 0, 2.0, 1.6, 0.4f));
        const double early = toneAmplitude (plucked.left, samples (0.1), samples (0.1), 990.0), late = toneAmplitude (plucked.left, samples (1.3), samples (0.1), 990.0);
        check (early > 8.0 * late, "Growler closes as the note dies away");
    }
}

//==============================================================================
/** The harness measures the level on a riff with a chord and notes ringing into each other, which a monophonic
    synth cannot follow. This is the level on what the synths are made for, a line of single notes. */
void testLevels()
{
    std::printf ("Level at the default knobs on a line of single notes (dB re the input)\n ");
    const auto models = fx::filterModels();
    const double line[] = { 146.83, 164.81, 196.0, 220.0, 246.94, 293.66, 329.63, 220.0, 110.0, 82.41 };
    std::vector<float> input ((size_t) samples (3.4), 0.0f);
    for (int n = 0; n < 10; ++n)
    {
        const auto note = guitarNote (line[n], n % 2, 0.4, 0.34, 0.4f);
        for (size_t i = 0; i < note.size(); ++i)
            input[i + (size_t) samples (0.3 * n)] += note[i];
    }

    for (const auto& m : models)
    {
        const auto out = run (m.variant, harness::defaultKnobs (m), input);
        const double level = 20.0 * std::log10 (rms (out.left, 0, (int) input.size()) / rms (input, 0, (int) input.size()));
        std::printf (" %s %+.1f ", m.key, level);
        check (level > -7.0 && level < 4.0, std::string (m.key) + ": " + num (level) + " dB re the input on a single-note line");
    }
    std::printf ("\n");
}

//==============================================================================
/** Moving a knob or a switch while a steady tone plays must not leave a step in the output: the largest
    second difference (a click of height h gives about h) stays close to that of the tone itself. */
void testClicks()
{
    std::printf ("Knob and switch changes: no clicks\n");
    const auto models = fx::filterModels();
    std::vector<float> tone ((size_t) samples (1.0));
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = (float) (0.2 * std::sin (2.0 * fx::pi * 220.0 * (double) i / fs) + 0.1 * std::sin (2.0 * fx::pi * 331.0 * (double) i / fs));

    auto roughness = [] (const std::vector<float>& x, int start, int length)
    {
        double worst = 0.0;
        for (int i = start + 2; i < start + length; ++i)
            worst = std::max (worst, (double) std::abs (x[(size_t) i] - 2.0f * x[(size_t) i - 1] + x[(size_t) i - 2]));
        return worst;
    };

    double worstAll = 0.0;
    for (int v = V::voiceBox; v <= V::cometTrails; ++v)
    {
        const auto& m = models[(size_t) v];
        for (size_t knob = 0; knob < m.knobs.size(); ++knob)
            for (int direction = 0; direction < 2; ++direction)
            {
                auto before = harness::defaultKnobs (m), after = before;
                before[knob] = direction ? m.knobs[knob].max : m.knobs[knob].min;
                after[knob] = direction ? m.knobs[knob].min : m.knobs[knob].max;
                const auto out = run (v, before, tone, tone, samples (0.5), &after);
                const double click = std::max (roughness (out.left, samples (0.45), samples (0.3)), roughness (out.right, samples (0.45), samples (0.3)));
                worstAll = std::max (worstAll, click);
                check (click < 0.012, std::string (m.name) + ": moving " + m.knobs[knob].name + " clicks (" + num (click) + ")");
            }
    }
    std::printf ("  largest step after any knob jump on the filter models: %.4f (the input tone alone: %.4f)\n", worstAll, roughness (tone, 0, (int) tone.size()));

    // the synths: a wave switch or a Mix / Freq jump must not produce a step larger than the oscillator's own edges
    for (int v = V::octisynth; v <= V::growler; ++v)
    {
        const auto& m = models[(size_t) v];
        const auto reference = run (v, harness::defaultKnobs (m), tone);
        const double normal = roughness (reference.left, samples (0.3), samples (0.6));
        for (size_t knob = 0; knob < m.knobs.size(); ++knob)
        {
            if (m.knobs[knob].unit == fx::Unit::semitones)
                continue;
            auto before = harness::defaultKnobs (m), after = before;
            before[knob] = m.knobs[knob].min;
            after[knob] = m.knobs[knob].max;
            const auto out = run (v, before, tone, tone, samples (0.5), &after);
            const auto there = run (v, after, tone);
            const double limit = 1.5 * std::max ({ normal, roughness (there.left, samples (0.3), samples (0.6)), roughness (run (v, before, tone).left, samples (0.3), samples (0.6)) }) + 0.01;
            const double click = roughness (out.left, samples (0.45), samples (0.3));
            check (click < limit, std::string (m.name) + ": moving " + m.knobs[knob].name + " clicks (" + num (click) + ", limit " + num (limit) + ")");
        }
    }
}

//==============================================================================
/** Extremes and other sample rates beyond the harness: 192 kHz, and every model with all knobs at once at
    their minimum and maximum. */
void testExtremes()
{
    std::printf ("Extremes: 192 kHz, all knobs at minimum / maximum\n");
    const auto models = fx::filterModels();

    for (double rate : { 44100.0, 192000.0 })
    {
        const auto input = harness::makeInput (rate, 3.0);
        for (const auto& m : models)
            for (int end = 0; end < 3; ++end)
            {
                fx::FilterFx effect;
                effect.prepare (rate, 512);
                Knobs k = harness::defaultKnobs (m);
                for (size_t i = 0; i < m.knobs.size(); ++i)
                    if (end > 0)
                        k[i] = end == 1 ? m.knobs[i].min : m.knobs[i].max;
                k[4] = 100.0f;
                effect.setModel (m.variant);
                effect.setParameters (k.data());
                effect.reset();

                auto left = input, right = input;
                for (int pos = 0; pos < (int) input.size(); pos += 512)
                {
                    effect.setParameters (k.data());
                    effect.process (left.data() + pos, right.data() + pos, std::min (512, (int) input.size() - pos));
                }
                const auto sl = harness::measure (left), sr = harness::measure (right);
                check (sl.finite && sr.finite && sl.peak < 8.0f && sr.peak < 8.0f,
                       std::string (m.key) + " at " + num (rate) + " Hz, knobs " + (end == 0 ? "default" : end == 1 ? "min" : "max") + ": peak " + num (std::max (sl.peak, sr.peak)));
            }
    }

    // a full-scale sine right on a resonance at the highest Q must stay bounded (soft limit at 4)
    const auto hot = sine (800.0, 1.0, 1.0);
    const auto out = run (V::qFilter, knobs ({ 50.0f, 100.0f, 12.0f, 0.0f, 100.0f }), hot);
    check (peak (out.left, 0, (int) hot.size()) <= 4.0, "a full-scale sine on the resonance stays below 4");
}

} // namespace

//==============================================================================
int main (int argc, char* argv[])
{
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "test_output/standalone/filter";
    failed = harness::run<fx::FilterFx> ("filter", fx::filterModels(), outDir);

    std::printf ("\n== filter: the models' own checks ==\n");
    testLevels();
    testQFilter();
    testStereoClasses();
    testVowels();
    testSeeker();
    testObiWah();
    testTron();
    testThrobber();
    testSlowFilter();
    testSpinCycle();
    testCometTrails();
    testOctisynth();
    testTracker();
    testSynths();
    testSynthSounds();
    testClicks();
    testExtremes();

    std::printf ("%s\n", failed == 0 ? "ALL CHECKS PASSED" : (std::to_string (failed) + " CHECK(S) FAILED").c_str());
    return failed == 0 ? 0 : 1;
}
