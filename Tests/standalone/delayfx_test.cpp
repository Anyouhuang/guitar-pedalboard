// Standalone test of the Delay engine (see Harness.h): harness::run for all 19 models, then checks of what
// defines each model (echo times and levels, ping-pong, reverse playback, darkening, quantising, pitch wobble,
// ducking, swell, head combinations, bounded feedback, click-free time changes, the dry path's preamp).
#include "Harness.h"
#include "../../Source/DSP/fx/DelayFx.h"

#include <functional>

namespace
{
using harness::Knobs;
using Fx = fx::DelayFx;

constexpr double sampleRate = harness::testRate;
int failed = 0;

void check (bool ok, const std::string& what)
{
    if (! ok)
    {
        ++failed;
        std::printf ("    FAIL: %s\n", what.c_str());
    }
}

std::string num (double v) { return harness::jsonNumber (v); }
double db (double ratio) { return ratio > 1.0e-30 ? 10.0 * std::log10 (ratio) : -300.0; } // of an energy ratio

struct Stereo { std::vector<float> left, right; };
using KnobChange = std::function<void (int, Knobs&)>; // (sample position of the block, knobs to edit)

/** Like a slot: model, knobs, reset, then 256-sample blocks with the knobs set before each. */
Stereo render (int variant, Knobs knobs, const std::vector<float>& inLeft, const std::vector<float>& inRight,
               double fs = sampleRate, const KnobChange& change = {})
{
    Fx effect;
    effect.prepare (fs, harness::blockSize);
    effect.setModel (variant);
    effect.setParameters (knobs.data());
    effect.reset();

    Stereo out { inLeft, inRight };
    for (int pos = 0; pos < (int) inLeft.size(); pos += harness::blockSize)
    {
        const int n = std::min (harness::blockSize, (int) inLeft.size() - pos);
        if (change)
            change (pos, knobs);

        effect.setParameters (knobs.data());
        effect.process (out.left.data() + pos, out.right.data() + pos, n);
    }
    return out;
}

Stereo render (int variant, const Knobs& knobs, const std::vector<float>& in, double fs = sampleRate, const KnobChange& change = {})
{
    return render (variant, knobs, in, in, fs, change);
}

void setKnob (Knobs& knobs, const fx::ModelInfo& m, const char* name, float value, bool mustExist = true)
{
    for (size_t i = 0; i < m.knobs.size(); ++i)
        if (std::string (m.knobs[i].name) == name)
        {
            knobs[i] = value;
            return;
        }

    if (mustExist)
        check (false, std::string (m.key) + " has no knob " + name);
}

/** Default knobs with everything that moves or colours the echo time switched off, so times and levels can be measured. */
Knobs plainKnobs (const fx::ModelInfo& m, float timeMs, float fdbk)
{
    Knobs k = harness::defaultKnobs (m);
    setKnob (k, m, "Time", timeMs, false);
    setKnob (k, m, "Fdbk", fdbk, false);
    for (const char* off : { "Wow/Flt", "Depth", "ModDep", "Swp Dep", "Ducking", "Swell" })
        setKnob (k, m, off, 0.0f, false);
    setKnob (k, m, "Tone", 100.0f, false);
    setKnob (k, m, "Res", 18.0f, false); // 24 bit
    return k;
}

std::vector<float> silence (double seconds, double fs = sampleRate) { return std::vector<float> ((size_t) (seconds * fs), 0.0f); }

/** Hann-windowed sine burst. */
void addBurst (std::vector<float>& x, double start, double length, double hz, float amplitude, double fs = sampleRate)
{
    const int first = (int) (start * fs), n = (int) (length * fs);
    for (int i = 0; i < n && first + i < (int) x.size(); ++i)
        x[(size_t) (first + i)] += amplitude * (float) (0.5 - 0.5 * std::cos (2.0 * fx::pi * i / n)) * (float) std::sin (2.0 * fx::pi * hz * i / fs);
}

/** Sine with a hard start and a 5 ms fade at the end. */
void addTone (std::vector<float>& x, double start, double end, double hz, float amplitude, double fs = sampleRate)
{
    const int first = (int) (start * fs), last = std::min ((int) x.size(), (int) (end * fs)), fade = (int) (0.005 * fs);
    for (int i = first; i < last; ++i)
        x[(size_t) i] += amplitude * (float) std::sin (2.0 * fx::pi * hz * (i - first) / fs) * std::min (1.0f, (float) (last - i) / (float) fade);
}

/** Broadband click: a few milliseconds of noise under a Hann window. */
void addNoiseBurst (std::vector<float>& x, double start, double length, float amplitude, double fs = sampleRate)
{
    harness::Lcg rng { 777u };
    const int first = (int) (start * fs), n = (int) (length * fs);
    for (int i = 0; i < n; ++i)
        x[(size_t) (first + i)] += amplitude * (float) (0.5 - 0.5 * std::cos (2.0 * fx::pi * i / n)) * (rng.next01() * 2.0f - 1.0f);
}

double energy (const std::vector<float>& x, double from, double to, double fs = sampleRate)
{
    double sum = 0.0;
    for (int i = std::max (0, (int) (from * fs)); i < std::min ((int) x.size(), (int) (to * fs)); ++i)
        sum += (double) x[(size_t) i] * x[(size_t) i];
    return sum;
}

/** Where the energy of x sits in [from, to), in seconds. */
double centroid (const std::vector<float>& x, double from, double to, double fs = sampleRate)
{
    double sum = 0.0, weighted = 0.0;
    for (int i = std::max (0, (int) (from * fs)); i < std::min ((int) x.size(), (int) (to * fs)); ++i)
    {
        const double e = (double) x[(size_t) i] * x[(size_t) i];
        sum += e;
        weighted += e * i;
    }
    return sum > 0.0 ? weighted / sum / fs : -1.0;
}

float peakOf (const std::vector<float>& x, double from = 0.0, double to = 1.0e9)
{
    float p = 0.0f;
    for (int i = std::max (0, (int) (from * sampleRate)); i < (int) std::min ((double) x.size(), to * sampleRate); ++i)
        p = std::max (p, std::abs (x[(size_t) i]));
    return p;
}

/** Energy of x in [from, to) above (or below) `hz`, in dB (4th-order Butterworth). */
double bandDb (const std::vector<float>& x, double from, double to, double hz, bool above)
{
    fx::Biquad f1, f2;
    if (above) { f1.setHighPass (sampleRate, hz, 0.54); f2.setHighPass (sampleRate, hz, 1.31); }
    else       { f1.setLowPass (sampleRate, hz, 0.54);  f2.setLowPass (sampleRate, hz, 1.31); }

    double band = 0.0;
    for (int i = (int) (from * sampleRate); i < std::min ((int) x.size(), (int) (to * sampleRate)); ++i)
    {
        const float y = f2.process (f1.process (x[(size_t) i]));
        band += (double) y * y;
    }
    return db (band);
}

/** Treble against midrange: energy above 4 kHz over energy below 1 kHz, in dB. */
double tiltDb (const std::vector<float>& x, double from, double to)
{
    return bandDb (x, from, to, 4000.0, true) - bandDb (x, from, to, 1000.0, false);
}

/** Bass against the rest: energy below 150 Hz over energy above 600 Hz, in dB. */
double bassDb (const std::vector<float>& x, double from, double to)
{
    return bandDb (x, from, to, 150.0, false) - bandDb (x, from, to, 600.0, true);
}

/** Amplitude of the `hz` component of x in [from, to). */
double component (const std::vector<float>& x, double from, double to, double hz)
{
    double re = 0.0, im = 0.0;
    const int a = (int) (from * sampleRate), b = std::min ((int) x.size(), (int) (to * sampleRate));
    for (int i = a; i < b; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * fx::pi * (i - a) / (b - a));
        re += w * x[(size_t) i] * std::cos (2.0 * fx::pi * hz * i / sampleRate);
        im += w * x[(size_t) i] * std::sin (2.0 * fx::pi * hz * i / sampleRate);
    }
    return 4.0 * std::sqrt (re * re + im * im) / (b - a);
}

/** 2nd + 3rd harmonic relative to the fundamental. */
double distortion (const std::vector<float>& x, double from, double to, double hz)
{
    const double h2 = component (x, from, to, 2.0 * hz), h3 = component (x, from, to, 3.0 * hz);
    return std::sqrt (h2 * h2 + h3 * h3) / std::max (1.0e-12, component (x, from, to, hz));
}

/** Moving average over `window` samples, in place (the first `window` values are a shorter average). */
void movingAverage (std::vector<double>& x, int window)
{
    std::vector<double> out (x.size());
    double sum = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        sum += x[i];
        if (i >= (size_t) window)
            sum -= x[i - (size_t) window];
        out[i] = sum / window;
    }
    x = out;
}

/** Relative frequency deviation of a sine of `hz` in x, sample by sample over [from, to): quadrature demodulation,
    low-passed by three moving averages over whole periods. `hz` must divide the sample rate. */
std::vector<double> pitchDeviation (const std::vector<float>& x, double from, double to, double hz)
{
    const int a = (int) (from * sampleRate), b = std::min ((int) x.size(), (int) (to * sampleRate)), window = (int) std::lround (sampleRate / hz) * 4;
    std::vector<double> re (x.size()), im (x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        re[i] = x[i] * std::cos (2.0 * fx::pi * hz * (double) i / sampleRate);
        im[i] = -x[i] * std::sin (2.0 * fx::pi * hz * (double) i / sampleRate);
    }

    for (int pass = 0; pass < 3; ++pass)
    {
        movingAverage (re, window);
        movingAverage (im, window);
    }

    std::vector<double> deviation;
    for (int i = std::max (a, 3 * window) + 1; i < b; ++i)
    {
        double step = std::atan2 (im[(size_t) i], re[(size_t) i]) - std::atan2 (im[(size_t) i - 1], re[(size_t) i - 1]);
        while (step > fx::pi) step -= 2.0 * fx::pi;
        while (step < -fx::pi) step += 2.0 * fx::pi;
        deviation.push_back (step * sampleRate / (2.0 * fx::pi * hz));
    }
    return deviation;
}

double peakAbs (const std::vector<double>& x)
{
    double p = 0.0;
    for (double v : x) p = std::max (p, std::abs (v));
    return p;
}

/** Largest sample-to-sample step of x from `from` seconds on. */
float maxStep (const std::vector<float>& x, double from = 0.0, double to = 1.0e9)
{
    float step = 0.0f;
    for (size_t i = (size_t) (from * sampleRate) + 1; i < (size_t) std::min ((double) x.size(), to * sampleRate); ++i)
        step = std::max (step, std::abs (x[i] - x[i - 1]));
    return step;
}

bool identical (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
}

bool isTapeOrAnalog (int v)
{
    return v >= Fx::tubeEcho && v != Fx::reverse && v != Fx::loRes;
}

bool hasFixedWow (int v)
{
    return v == Fx::tapeEcho || v == Fx::tapeEchoDry || v == Fx::sweepEcho || v == Fx::sweepEchoDry || v == Fx::multiHead;
}
} // namespace

//==============================================================================
int main (int argc, char* argv[])
{
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "test_output/standalone/delayfx";
    const auto models = fx::delayFxModels();

    harness::Options options;
    options.wetOnly = true;
    failed += harness::run<Fx> ("delayfx", models, outDir, options);

    std::printf ("== delayfx: model checks ==\n");
    check ((int) models.size() == (int) Fx::numVariants, "19 models");
    for (const auto& m : models)
        check (m.trails && m.timeKnob == 0 && m.noteKnob == 1 && m.noteBeats == fx::delayNoteBeats
               && m.category == fx::Category::delay && m.engine == fx::Engine::delayFx, std::string (m.key) + ": tempo sync / trails fields");
    check (models[Fx::stereoDelay].timeKnob2 == 3 && models[Fx::stereoDelay].noteKnob2 == 4, "stereo_delay: second tempo sync");

    //==========================================================================
    // 1. Echo time and level: a 1 kHz burst, Time 250 ms, Fdbk 50 %. The first echo must sit at Time, the next
    //    ones at multiples, each 6 dB below the one before.
    std::printf ("  echo times and levels (Time 250 ms, Fdbk 50 %%)\n");
    std::printf ("    model                 echo 1 ms   echo 2 ms   echo 3 ms   step 1>2 dB  step 2>3 dB\n");
    for (const auto& m : models)
    {
        const int v = m.variant;
        if (v == Fx::pingPong || v == Fx::stereoDelay || v == Fx::reverse || v == Fx::multiHead)
            continue; // their own tests below

        const bool swell = v == Fx::autoVolume;
        const double length = swell ? 0.1 : 0.008, centre = 0.05 + 0.5 * length, time = 0.25;
        auto in = silence (1.4);
        addBurst (in, 0.05, length, 1000.0, 0.05f);

        Knobs k = plainKnobs (m, 250.0f, 50.0f);
        if (swell)
            setKnob (k, m, "Mix", 50.0f); // swelled signal and echoes both at unity; the swell (20 ms here) shapes both alike

        const auto out = render (v, k, in);
        const double reference = swell ? centroid (out.left, centre - 0.125, centre + 0.125) : centroid (in, 0.0, 0.2);
        double at[3], level[3];
        for (int e = 0; e < 3; ++e)
        {
            const double from = centre + (e + 1) * time - 0.125, to = from + 0.25;
            at[e] = (centroid (out.left, from, to) - reference) * 1000.0;
            level[e] = energy (out.left, from, to);
        }

        const bool coloured = isTapeOrAnalog (v);
        const double firstTolerance = coloured ? 1.0 : 0.05, laterTolerance = hasFixedWow (v) ? 4.0 : firstTolerance; // ms
        const double levelTolerance = coloured ? 1.5 : 0.05; // dB; tape and BBD loops are not flat
        const double step12 = db (level[1] / level[0]), step23 = db (level[2] / level[1]);
        std::printf ("    %-20s %9.3f   %9.3f   %9.3f   %9.2f    %9.2f\n", m.key, at[0], at[1], at[2], step12, step23);

        check (std::abs (at[0] - 250.0) < firstTolerance, std::string (m.key) + ": first echo at " + num (at[0]) + " ms instead of 250");
        check (std::abs (at[1] - 500.0) < laterTolerance && std::abs (at[2] - 750.0) < laterTolerance, std::string (m.key) + ": later echoes not at multiples of Time");
        check (std::abs (step12 + 6.02) < levelTolerance && std::abs (step23 + 6.02) < levelTolerance,
               std::string (m.key) + ": echo level steps " + num (step12) + " / " + num (step23) + " dB, Fdbk 50 % means -6");
        check (energy (out.left, 0.0, centre + 0.1) < (swell ? 1.0e30 : 1.0e-12), std::string (m.key) + ": wet signal contains the dry signal");
        check (identical (out.left, out.right), std::string (m.key) + ": same input on both sides must give the same echoes");
    }

    // Fdbk 0 % = one echo only; the feedback knob's level at another setting
    {
        const auto& m = models[Fx::digitalDelay];
        auto in = silence (1.4);
        addBurst (in, 0.05, 0.008, 1000.0, 0.2f);
        const auto once = render (m.variant, plainKnobs (m, 250.0f, 0.0f), in);
        check (energy (once.left, 0.2, 0.4) > 1.0e-4 && energy (once.left, 0.45, 1.4) == 0.0, "digital_delay: Fdbk 0 % must give exactly one echo");
        const auto more = render (m.variant, plainKnobs (m, 250.0f, 80.0f), in);
        const double step = db (energy (more.left, 0.429, 0.679) / energy (more.left, 0.179, 0.429));
        check (std::abs (step - 20.0 * std::log10 (0.8)) < 0.05, "digital_delay: Fdbk 80 % gives " + num (step) + " dB per repeat");

        // the first echo is the input, bit for bit
        bool exact = true;
        for (size_t i = 0; i < 9600; ++i)
            exact = exact && once.left[i + 12000] == in[i];
        check (exact, "digital_delay: the first echo is not a clean copy of the input");
    }

    //==========================================================================
    // 2. Stereo Delay: independent times and feedbacks; true stereo (left input never reaches the right output)
    {
        const auto& m = models[Fx::stereoDelay];
        auto in = silence (1.5);
        addBurst (in, 0.05, 0.008, 1000.0, 0.2f);
        Knobs k = harness::defaultKnobs (m);
        k[0] = 200.0f; k[2] = 50.0f; k[3] = 330.0f; k[5] = 25.0f;
        const auto out = render (m.variant, k, in);
        const double l1 = (centroid (out.left, 0.154, 0.354) - 0.054) * 1000.0, r1 = (centroid (out.right, 0.254, 0.514) - 0.054) * 1000.0;
        const double lStep = db (energy (out.left, 0.354, 0.554) / energy (out.left, 0.154, 0.354));
        const double rStep = db (energy (out.right, 0.549, 0.879) / energy (out.right, 0.219, 0.549));
        std::printf ("  stereo delay: L echo %.3f ms (%.2f dB per repeat), R echo %.3f ms (%.2f dB per repeat)\n", l1, lStep, r1, rStep);
        check (std::abs (l1 - 200.0) < 0.05 && std::abs (r1 - 330.0) < 0.05, "stereo_delay: L / R times are not independent");
        check (std::abs (lStep + 6.02) < 0.05 && std::abs (rStep + 12.04) < 0.05, "stereo_delay: L / R feedbacks are not independent");
    }

    for (const auto& m : models)
    {
        auto in = silence (1.5);
        addBurst (in, 0.05, 0.1, 1000.0, 0.2f);
        const auto out = render (m.variant, harness::defaultKnobs (m), in, silence (1.5));
        const bool monoEffect = m.variant == Fx::pingPong || m.variant == Fx::reverse || m.variant == Fx::multiHead;
        const double l = energy (out.left, 0.0, 1.5), r = energy (out.right, 0.0, 1.5);
        if (monoEffect)
            check (l > 1.0e-5 && r > 1.0e-5, std::string (m.key) + ": works on the mono sum, so a left-only input must reach both sides");
        else
            check (l > 1.0e-5 && r == 0.0, std::string (m.key) + ": true stereo, a left-only input must leave the right side silent");
    }

    //==========================================================================
    // 3. Ping Pong: left, right, left, right; Offset moves the right echo; Spread 0 % is mono
    {
        const auto& m = models[Fx::pingPong];
        auto in = silence (2.0);
        addBurst (in, 0.05, 0.008, 1000.0, 0.2f);
        const double c = 0.054;
        Knobs k = harness::defaultKnobs (m);
        k[0] = 200.0f; k[2] = 50.0f; k[3] = 100.0f; k[4] = 100.0f;

        auto side = [&] (const Stereo& s, double at) // +1 = all left, -1 = all right
        {
            const double l = energy (s.left, c + at - 0.05, c + at + 0.05), r = energy (s.right, c + at - 0.05, c + at + 0.05);
            return l + r > 1.0e-9 ? (l - r) / (l + r) : 0.0;
        };
        auto total = [&] (const Stereo& s, double at)
        {
            return energy (s.left, c + at - 0.05, c + at + 0.05) + energy (s.right, c + at - 0.05, c + at + 0.05);
        };

        const auto even = render (m.variant, k, in);
        std::printf ("  ping pong (200 ms, Offset 100 %%): sides %+.2f %+.2f %+.2f %+.2f, levels %.2f %.2f %.2f dB\n",
                     side (even, 0.2), side (even, 0.4), side (even, 0.6), side (even, 0.8),
                     db (total (even, 0.4) / total (even, 0.2)), db (total (even, 0.6) / total (even, 0.4)), db (total (even, 0.8) / total (even, 0.6)));
        check (side (even, 0.2) > 0.999 && side (even, 0.4) < -0.999 && side (even, 0.6) > 0.999 && side (even, 0.8) < -0.999,
               "ping_pong: echoes do not alternate left / right");
        check (std::abs ((centroid (even.left, c + 0.1, c + 0.3) - c) - 0.2) < 0.00005 && std::abs ((centroid (even.right, c + 0.3, c + 0.5) - c) - 0.4) < 0.00005,
               "ping_pong: ping at Time, pong one right-delay later");
        check (std::abs (db (total (even, 0.4) / total (even, 0.2))) < 0.05, "ping_pong: ping and pong of one bounce must be equally loud");
        check (std::abs (db (total (even, 0.6) / total (even, 0.2)) + 6.02) < 0.05 && std::abs (db (total (even, 0.8) / total (even, 0.4)) + 6.02) < 0.05,
               "ping_pong: each round trip must drop by Fdbk");

        k[3] = 50.0f; // right delay 100 ms: L at 200, R at 300, L at 500, R at 600
        const auto swing = render (m.variant, k, in);
        check (side (swing, 0.2) > 0.999 && side (swing, 0.3) < -0.999 && side (swing, 0.5) > 0.999 && side (swing, 0.6) < -0.999 && total (swing, 0.4) < 1.0e-9,
               "ping_pong: Offset 50 % must put the right echo half a Time after the left one");

        k[3] = 100.0f; k[4] = 0.0f;
        const auto mono = render (m.variant, k, in);
        check (std::abs (side (mono, 0.2)) < 1.0e-6 && std::abs (side (mono, 0.4)) < 1.0e-6, "ping_pong: Spread 0 % must be mono");
        k[4] = 50.0f;
        const auto half = render (m.variant, k, in);
        check (side (half, 0.2) > 0.3 && side (half, 0.2) < 0.95, "ping_pong: Spread 50 % must sit between mono and wide");
    }

    //==========================================================================
    // 4. Reverse: every chunk of the input comes back reversed, and the joins do not click
    {
        const auto& m = models[Fx::reverse];
        const int n = (int) (0.2 * sampleRate), fade = (int) (0.02 * sampleRate); // Time 200 ms
        auto in = harness::makeInput (sampleRate, 2.0);
        Knobs k = plainKnobs (m, 200.0f, 0.0f);
        const auto out = render (m.variant, k, in);

        bool backwards = true;
        for (int chunk = 1; chunk < 9; ++chunk)
            for (int p = fade; p < n; ++p)
                backwards = backwards && out.left[(size_t) (chunk * n + p)] == in[(size_t) (chunk * n - 2 - p)];
        check (backwards, "reverse: output chunks are not the input chunks played backwards");

        // and it is not a forwards delay: compare with the input shifted by one chunk
        double same = 0.0, all = 0.0;
        for (int i = n; i < (int) in.size(); ++i)
        {
            same += (double) out.left[(size_t) i] * in[(size_t) (i - n)];
            all += (double) in[(size_t) (i - n)] * in[(size_t) (i - n)];
        }
        check (std::abs (same / all) < 0.3, "reverse: output correlates with the forwards input");

        // smooth joins: a steady sine through the chunk boundaries keeps its level and never jumps
        auto sine = silence (2.0);
        addTone (sine, 0.0, 2.0, 220.0, 0.3f);
        const auto joined = render (m.variant, k, sine);
        const float normalStep = 0.3f * (float) (2.0 * fx::pi * 220.0 / sampleRate);
        double lowest = 1.0e9;
        for (int chunk = 2; chunk < 9; ++chunk)
            lowest = std::min (lowest, energy (joined.left, chunk * 0.2 - 0.005, chunk * 0.2 + 0.025) / (0.03 * sampleRate));
        std::printf ("  reverse: joins: largest step %.4f (a clean sine: %.4f), lowest level across a join %.1f dB\n",
                     maxStep (joined.left, 0.21, 1.9), normalStep, db (lowest / (0.5 * 0.3 * 0.3)));
        check (maxStep (joined.left, 0.21, 1.9) < 1.6f * normalStep, "reverse: click at a chunk join");
        check (db (lowest / (0.5 * 0.3 * 0.3)) > -4.0, "reverse: level dips at the chunk joins");

        // feedback: the reversed chunk is reversed again (forwards) one chunk later, at the Fdbk level
        k = plainKnobs (m, 200.0f, 50.0f);
        auto burst = silence (1.5);
        addBurst (burst, 0.25, 0.05, 1000.0, 0.2f); // inside chunk 1 (0.2..0.4 s): comes back in chunk 2, again in chunk 3
        const auto fed = render (m.variant, k, burst);
        const double step = db (energy (fed.left, 0.6, 0.8) / energy (fed.left, 0.4, 0.6));
        check (std::abs (step + 6.02) < 0.3, "reverse: Fdbk 50 % repeats the chunk at " + num (step) + " dB");

        // ModSpd / Depth move the read head (chorus)
        Knobs modulated = plainKnobs (m, 200.0f, 0.0f);
        setKnob (modulated, m, "Depth", 100.0f);
        setKnob (modulated, m, "ModSpd", 2.0f);
        auto sine240 = silence (2.0);
        addTone (sine240, 0.0, 2.0, 240.0, 0.3f);
        const auto still = render (m.variant, plainKnobs (m, 200.0f, 0.0f), sine240), wobbly = render (m.variant, modulated, sine240);
        const double steady = peakAbs (pitchDeviation (still.left, 1.08, 1.19, 240.0)), moving = peakAbs (pitchDeviation (wobbly.left, 1.08, 1.19, 240.0));
        std::printf ("  reverse: pitch deviation inside a chunk %.4f %% (Depth 0), %.3f %% (Depth 100 %% at 2 Hz)\n", 100.0 * steady, 100.0 * moving);
        check (steady < 1.0e-4 && moving > 0.003 && moving < 0.02, "reverse: Depth must modulate the pitch");
    }

    //==========================================================================
    // 5. Tone of the repeats: a noise click, Fdbk 60 %. Treble against midrange (energy above 4 kHz over energy
    //    below 1 kHz) in echo 1 and echo 3: unchanged on the digital models, falling on tape, platter and BBD.
    std::printf ("  darkening (treble against mids in echo 1 -> echo 3) and bass against the rest in echo 3\n");
    double bass3[Fx::numVariants] {}, tilt1[Fx::numVariants] {};
    for (const auto& m : models)
    {
        const int v = m.variant;
        if (v == Fx::pingPong || v == Fx::stereoDelay || v == Fx::reverse)
            continue;

        auto in = silence (1.4);
        if (v == Fx::autoVolume) addNoiseBurst (in, 0.05, 0.08, 0.1f);
        else                     addNoiseBurst (in, 0.05, 0.004, 0.2f);

        Knobs k = plainKnobs (m, 300.0f, 60.0f);
        if (v == Fx::multiHead) { setKnob (k, m, "Heads 1-2", 0.0f); setKnob (k, m, "Heads 3-4", 2.0f); } // head 4 only
        if (v == Fx::autoVolume) setKnob (k, m, "Mix", 100.0f);

        const auto out = render (v, k, in);
        const double first = tiltDb (out.left, 0.3, 0.5), third = tiltDb (out.left, 0.9, 1.1);
        bass3[v] = bassDb (out.left, 0.9, 1.1);
        tilt1[v] = first - tiltDb (in, 0.0, 0.2);
        std::printf ("    %-20s %6.1f dB -> %6.1f dB    bass %6.1f dB\n", m.key, tilt1[v], third - tiltDb (in, 0.0, 0.2), bass3[v] - bassDb (in, 0.0, 0.2));

        if (isTapeOrAnalog (v))
            check (third < first - 4.0, std::string (m.key) + ": repeats must get darker (" + num (first) + " -> " + num (third) + " dB)");
        else
            check (std::abs (third - first) < 0.1 && std::abs (tilt1[v]) < 0.1, std::string (m.key) + ": digital repeats must keep their tone");
    }
    check (tilt1[Fx::analogEcho] < tilt1[Fx::analogMod] - 2.0 && tilt1[Fx::analogMod] < tilt1[Fx::tapeEcho] - 2.0 && tilt1[Fx::tubeEcho] < tilt1[Fx::tapeEcho] - 2.0,
           "the DM-2 must be darker than the Memory Man, the BBDs darker than tape, the EP-1 darker than the EP-3");
    check (bass3[Fx::echoPlatter] < bass3[Fx::tubeEcho] - 6.0, "echo_platter: the drum's repeats must be thinner in the bass than tape");

    // grit: a 220 Hz note; harmonics of echo 1 and echo 2
    std::printf ("  distortion of the repeats (220 Hz at 0.3): echo 1, echo 2\n");
    double grit[Fx::numVariants] {};
    for (int v : { (int) Fx::digitalDelay, (int) Fx::tapeEcho, (int) Fx::tubeEcho, (int) Fx::echoPlatter, (int) Fx::analogMod, (int) Fx::analogEcho, (int) Fx::multiHead })
    {
        const auto& m = models[(size_t) v];
        auto in = silence (1.6);
        addBurst (in, 0.05, 0.3, 220.0, 0.3f);
        Knobs k = plainKnobs (m, 400.0f, 70.0f);
        if (v == Fx::multiHead) { setKnob (k, m, "Heads 1-2", 0.0f); setKnob (k, m, "Heads 3-4", 2.0f); }
        const auto out = render (v, k, in);
        const double first = distortion (out.left, 0.5, 0.7, 220.0), second = distortion (out.left, 0.9, 1.1, 220.0);
        grit[v] = first;
        std::printf ("    %-20s %6.2f %%  %6.2f %%\n", m.key, 100.0 * first, 100.0 * second);
        if (v == Fx::digitalDelay)
            check (first < 1.0e-4 && second < 1.0e-4, "digital_delay: repeats must be clean");
        else
            check (first > 0.002 && second > 1.2 * first, std::string (m.key) + ": saturation must build up over the repeats");
    }
    check (grit[Fx::analogEcho] > grit[Fx::analogMod] && grit[Fx::tubeEcho] > 2.0 * grit[Fx::tapeEcho],
           "the DM-2 must be grittier than the Memory Man, the tube EP-1 dirtier than the solid-state EP-3");

    // Drive (Tube Echo, Echo Platter) adds distortion; Bass / Treble shape the repeats
    for (int v : { (int) Fx::tubeEcho, (int) Fx::echoPlatter })
    {
        const auto& m = models[(size_t) v];
        auto in = silence (1.0);
        addBurst (in, 0.05, 0.3, 220.0, 0.3f);
        Knobs k = plainKnobs (m, 400.0f, 0.0f);
        setKnob (k, m, "Drive", 0.0f);
        const double clean = distortion (render (v, k, in).left, 0.5, 0.7, 220.0);
        setKnob (k, m, "Drive", 100.0f);
        const double driven = distortion (render (v, k, in).left, 0.5, 0.7, 220.0);
        check (driven > 3.0 * clean && driven > 0.05, std::string (m.key) + ": Drive 0 -> 100 % gives " + num (100.0 * clean) + " -> " + num (100.0 * driven) + " % distortion");
    }

    for (int v : { (int) Fx::digitalDelay, (int) Fx::tapeEcho, (int) Fx::analogEcho })
    {
        const auto& m = models[(size_t) v];
        auto in = silence (1.4);
        addNoiseBurst (in, 0.05, 0.004, 0.2f);
        auto shape = [&] (float bass, float treble) // treble of echo 1 and echo 3, bass of echo 1, level step echo 1 -> 2
        {
            Knobs k = plainKnobs (m, 300.0f, 60.0f);
            setKnob (k, m, "Bass", bass);
            setKnob (k, m, "Treble", treble);
            const auto out = render (v, k, in);
            return std::array<double, 4> { bandDb (out.left, 0.3, 0.5, 6000.0, true), bandDb (out.left, 0.9, 1.1, 6000.0, true),
                                           bandDb (out.left, 0.3, 0.5, 100.0, false), db (energy (out.left, 0.6, 0.8) / energy (out.left, 0.3, 0.5)) };
        };
        const auto flat = shape (50.0f, 50.0f), dark = shape (50.0f, 0.0f), bright = shape (50.0f, 100.0f), fat = shape (100.0f, 50.0f), thin = shape (0.0f, 50.0f);
        std::printf ("  %-14s Treble 0 / 100 %%: %+.1f / %+.1f dB above 6 kHz (echo 3: %+.1f / %+.1f); Bass 0 / 100 %%: %+.1f / %+.1f dB below 100 Hz\n", m.key,
                     dark[0] - flat[0], bright[0] - flat[0], dark[1] - flat[1], bright[1] - flat[1], thin[2] - flat[2], fat[2] - flat[2]);
        check (dark[0] - flat[0] < -6.0 && bright[0] - flat[0] > 6.0, std::string (m.key) + ": Treble must cut / boost the highs of the repeats");
        check (dark[1] - flat[1] < 2.0 * (dark[0] - flat[0]), std::string (m.key) + ": Treble cut must add up from repeat to repeat");
        check (std::abs ((bright[1] - flat[1]) - (bright[0] - flat[0])) < 1.5 || v == Fx::analogEcho, std::string (m.key) + ": a Treble boost must be the same on every repeat");
        check (fat[2] - flat[2] > 5.0 && thin[2] - flat[2] < -5.0, std::string (m.key) + ": Bass must boost / cut the lows of the repeats");
        check (bright[3] < -4.3 && fat[3] < -4.3 && dark[3] < -4.3 && thin[3] < -4.3, std::string (m.key) + ": Bass / Treble must not raise the loop gain above Fdbk");
        if (v == Fx::digitalDelay) // (tape and BBD repeats lose level through their own filters)
            check (bright[3] > -4.6 && fat[3] > -4.6, "digital_delay: a boost must not shorten the repeats");
    }

    //==========================================================================
    // 6. Lo Res Delay: the line holds multiples of one quantising step
    {
        const auto& m = models[Fx::loRes];
        const auto in = harness::makeInput (sampleRate, 2.0);
        auto grid = [&] (float resIndex, double stepSize)
        {
            Knobs k = plainKnobs (m, 250.0f, 50.0f);
            setKnob (k, m, "Res", resIndex);
            const auto out = render (m.variant, k, in);
            bool onGrid = true;
            double worst = 0.0;
            for (size_t i = 0; i < out.left.size(); ++i)
            {
                const double steps = (double) out.left[i] / stepSize;
                onGrid = onGrid && steps == std::floor (steps);
                if (i >= 12000 && i < 24000) // the first echo alone: within half a step of the input
                    worst = std::max (worst, std::abs ((double) out.left[i] - (double) in[i - 12000]));
            }
            return std::pair<bool, double> { onGrid, worst / stepSize };
        };

        const auto six = grid (0.0f, 1.0 / 32.0), ten = grid (4.0f, 1.0 / 512.0), full = grid (18.0f, 1.0 / 8388608.0);
        std::printf ("  lo res: 6 bit on grid %d (error %.2f steps), 10 bit on grid %d (%.2f), 24 bit on grid %d (%.2f)\n",
                     (int) six.first, six.second, (int) ten.first, ten.second, (int) full.first, full.second);
        check (six.first && ten.first && full.first, "lo_res_delay: output is not quantised to the Res word length");
        check (six.second <= 0.5 && six.second > 0.4 && ten.second <= 0.5 && full.second <= 0.5, "lo_res_delay: quantiser must round to the nearest step");

        // Tone darkens the repeats, more with every repeat
        auto click = silence (1.4);
        addNoiseBurst (click, 0.05, 0.004, 0.2f);
        Knobs k = plainKnobs (m, 300.0f, 60.0f);
        const auto open = render (m.variant, k, click);
        setKnob (k, m, "Tone", 30.0f);
        const auto dark = render (m.variant, k, click);
        const double o1 = tiltDb (open.left, 0.3, 0.5), d1 = tiltDb (dark.left, 0.3, 0.5), d3 = tiltDb (dark.left, 0.9, 1.1);
        check (d1 < o1 - 6.0 && d3 < d1 - 6.0, "lo_res_delay: Tone must darken the repeats (" + num (o1) + ", " + num (d1) + ", " + num (d3) + " dB)");
    }

    //==========================================================================
    // 7. Pitch wobble of the repeats: a steady 1 kHz sine in, one echo out, its frequency deviation measured
    std::printf ("  pitch wobble of the echo (peak deviation of a 1 kHz sine)\n");
    {
        auto sine = silence (4.0);
        addTone (sine, 0.0, 4.0, 1000.0, 0.2f);

        struct Case { int variant; const char* knob; float value; const char* speedKnob; float speed; double min, max; };
        const Case cases[] = {
            { Fx::digitalDelay,  nullptr,   0.0f,   nullptr,  0.0f, 0.0,    1.0e-6 },
            { Fx::dynamicDly,    nullptr,   0.0f,   nullptr,  0.0f, 0.0,    1.0e-6 },
            { Fx::digMod,        "Depth",   0.0f,   "ModSpd", 1.0f, 0.0,    1.0e-6 },
            { Fx::digMod,        "Depth",   100.0f, "ModSpd", 1.0f, 0.010,  0.014 },
            { Fx::digMod,        "Depth",   50.0f,  "ModSpd", 1.0f, 0.005,  0.007 },
            { Fx::digMod,        "Depth",   100.0f, "ModSpd", 4.0f, 0.010,  0.014 },
            { Fx::analogMod,     "Depth",   0.0f,   "ModSpd", 1.0f, 0.0,    1.0e-5 },
            { Fx::analogMod,     "Depth",   100.0f, "ModSpd", 1.0f, 0.010,  0.014 },
            { Fx::analogEcho,    nullptr,   0.0f,   nullptr,  0.0f, 0.0,    1.0e-5 },
            { Fx::tubeEcho,      "Wow/Flt", 0.0f,   nullptr,  0.0f, 0.0,    1.0e-5 },
            { Fx::tubeEcho,      "Wow/Flt", 35.0f,  nullptr,  0.0f, 0.004,  0.014 },
            { Fx::tubeEcho,      "Wow/Flt", 100.0f, nullptr,  0.0f, 0.015,  0.035 },
            { Fx::echoPlatter,   "Wow/Flt", 0.0f,   nullptr,  0.0f, 0.0,    1.0e-5 },
            { Fx::echoPlatter,   "Wow/Flt", 100.0f, nullptr,  0.0f, 0.010,  0.030 },
            { Fx::tapeEcho,      nullptr,   0.0f,   nullptr,  0.0f, 0.0015, 0.008 },
            { Fx::sweepEcho,     "Swp Dep", 0.0f,   nullptr,  0.0f, 0.001,  0.007 },
            { Fx::autoVolume,    "ModDep",  0.0f,   nullptr,  0.0f, 0.0,    1.0e-5 },
            { Fx::autoVolume,    "ModDep",  100.0f, nullptr,  0.0f, 0.015,  0.035 },
            { Fx::multiHead,     nullptr,   0.0f,   nullptr,  0.0f, 0.0015, 0.009 },
        };

        for (const auto& c : cases)
        {
            const auto& m = models[(size_t) c.variant];
            Knobs k = plainKnobs (m, 350.0f, 0.0f);
            if (c.knob != nullptr) setKnob (k, m, c.knob, c.value);
            if (c.speedKnob != nullptr) setKnob (k, m, c.speedKnob, c.speed);
            if (c.variant == Fx::multiHead) { setKnob (k, m, "Heads 1-2", 0.0f); setKnob (k, m, "Heads 3-4", 2.0f); }
            if (c.variant == Fx::autoVolume) setKnob (k, m, "Mix", 100.0f);

            const auto out = render (c.variant, k, sine);
            const auto deviation = pitchDeviation (out.left, 1.0, 4.0, 1000.0);
            const double peak = peakAbs (deviation);

            int crossings = 0; // of the smoothed deviation: two per LFO cycle
            if (c.speedKnob != nullptr && c.value > 0.0f)
            {
                const double hysteresis = 0.3 * peak;
                int sign = 0;
                for (double d : deviation)
                {
                    const int now = d > hysteresis ? 1 : (d < -hysteresis ? -1 : sign);
                    if (now != sign && sign != 0) ++crossings;
                    sign = now;
                }
                check (std::abs (crossings - (int) (2.0f * c.speed * 3.0f)) <= 1,
                       std::string (m.key) + ": LFO speed, " + std::to_string (crossings) + " half cycles in 3 s at " + num (c.speed) + " Hz");
            }

            std::printf ("    %-20s %-8s %5.0f   %.3f %%\n", m.key, c.knob != nullptr ? c.knob : "", c.value, 100.0 * peak);
            check (peak >= c.min && peak <= c.max, std::string (m.key) + ": pitch deviation " + num (100.0 * peak) + " % outside "
                                                     + num (100.0 * c.min) + " .. " + num (100.0 * c.max) + " %");
        }

        // Dig Dly W/Mod: the two sides are modulated a quarter cycle apart (stereo chorus); Analog W/Mod: together
        for (int v : { (int) Fx::digMod, (int) Fx::analogMod })
        {
            const auto& m = models[(size_t) v];
            Knobs k = plainKnobs (m, 350.0f, 0.0f);
            setKnob (k, m, "Depth", 100.0f);
            setKnob (k, m, "ModSpd", 1.0f);
            const auto out = render (v, k, sine);
            const auto l = pitchDeviation (out.left, 1.0, 4.0, 1000.0), r = pitchDeviation (out.right, 1.0, 4.0, 1000.0);
            double lr = 0.0, ll = 0.0;
            for (size_t i = 0; i < l.size(); ++i) { lr += l[i] * r[i]; ll += l[i] * l[i]; }
            if (v == Fx::digMod) check (std::abs (lr / ll) < 0.1, "dig_dly_w_mod: left and right must be modulated in quadrature");
            else                 check (lr / ll > 0.99, "analog_w_mod: one BBD clock, both sides must move together");
        }
    }

    //==========================================================================
    // 8. Dynamic Dly: the echoes are down while playing and bloom afterwards
    {
        const auto& m = models[Fx::dynamicDly];
        auto in = silence (4.0);
        addTone (in, 0.0, 1.5, 220.0, 0.3f);
        auto levels = [&] (float thresh, float ducking)
        {
            Knobs k = harness::defaultKnobs (m);
            setKnob (k, m, "Time", 300.0f); setKnob (k, m, "Fdbk", 80.0f);
            setKnob (k, m, "Thresh", thresh); setKnob (k, m, "Ducking", ducking);
            const auto out = render (m.variant, k, in);
            return std::array<double, 3> { energy (out.left, 0.9, 1.4), energy (out.left, 2.4, 3.0), energy (out.left, 3.4, 4.0) };
        };

        const auto open = levels (50.0f, 0.0f), half = levels (50.0f, 50.0f), full = levels (50.0f, 100.0f), high = levels (100.0f, 100.0f), low = levels (0.0f, 100.0f);
        std::printf ("  dynamic dly: echoes while playing %.1f dB (Ducking 50 %%), %.1f dB (100 %%); after stopping %.1f dB, %.1f dB\n",
                     db (half[0] / open[0]), db (full[0] / open[0]), db (half[1] / open[1]), db (full[1] / open[1]));
        check (std::abs (db (half[0] / open[0]) + 12.04) < 1.0, "dynamic_dly: Ducking 50 % must turn the echoes down by 12 dB while playing");
        check (db (full[0] / open[0]) < -60.0, "dynamic_dly: Ducking 100 % must mute the echoes while playing");
        check (db (full[1] / open[1]) > -1.0 && db (half[1] / open[1]) > -1.0, "dynamic_dly: the echoes must come back after the playing stops");
        check (std::abs (db (high[0] / open[0])) < 0.1, "dynamic_dly: a signal below Thresh must not duck the echoes");
        check (db (low[1] / open[1]) < db (full[1] / open[1]) - 1.0 && db (low[2] / open[2]) > -1.0,
               "dynamic_dly: a lower Thresh must hold the echoes down for longer, but they must still come back in silence");

        // the bloom takes a few hundred milliseconds, it does not snap
        Knobs k = harness::defaultKnobs (m);
        setKnob (k, m, "Time", 300.0f); setKnob (k, m, "Fdbk", 100.0f); setKnob (k, m, "Ducking", 100.0f);
        const auto out = render (m.variant, k, in);
        const double early = energy (out.left, 1.55, 1.65), later = energy (out.left, 2.15, 2.25), end = energy (out.left, 3.35, 3.45);
        check (db (early / end) < -10.0 && db (later / end) > -3.0, "dynamic_dly: bloom time (" + num (db (early / end)) + " dB after 0.1 s, " + num (db (later / end)) + " dB after 0.7 s)");
    }

    //==========================================================================
    // 9. Auto-Volume Echo: every note fades in over the Swell time, and is echoed
    {
        const auto& m = models[Fx::autoVolume];
        auto in = silence (4.0);
        addTone (in, 0.1, 1.3, 330.0, 0.3f);
        addTone (in, 1.6, 2.8, 440.0, 0.3f); // a second note after a gap
        const double full = 0.5 * 0.3 * 0.3 * 0.02 * sampleRate; // energy of 20 ms of the note

        auto swell = [&] (float swellPercent, float mix)
        {
            Knobs k = harness::defaultKnobs (m);
            setKnob (k, m, "Time", 500.0f); setKnob (k, m, "Fdbk", 0.0f); setKnob (k, m, "ModDep", 0.0f);
            setKnob (k, m, "Swell", swellPercent); setKnob (k, m, "Mix", mix);
            return render (m.variant, k, in);
        };

        const auto medium = swell (50.0f, 0.0f); // Mix 0 %: the swelled signal alone. Swell 50 % = 200 ms
        const double at10 = db (energy (medium.left, 0.10, 0.12) / full), at100 = db (energy (medium.left, 0.19, 0.21) / full), at300 = db (energy (medium.left, 0.39, 0.41) / full);
        const double second10 = db (energy (medium.left, 1.60, 1.62) / full), second300 = db (energy (medium.left, 1.89, 1.91) / full);
        std::printf ("  auto-volume (Swell 200 ms): level %.1f dB at 10 ms, %.1f dB at 100 ms, %.1f dB at 300 ms; second note %.1f dB, %.1f dB\n",
                     at10, at100, at300, second10, second300);
        check (at10 < -40.0 && at100 > -16.0 && at100 < -8.0 && std::abs (at300) < 0.5, "auto_volume_echo: a note must fade in over the Swell time");
        check (second10 < -40.0 && std::abs (second300) < 0.5, "auto_volume_echo: the next note must fade in again");
        check (maxStep (medium.left) < 1.5f * 0.3f * (float) (2.0 * fx::pi * 440.0 / sampleRate), "auto_volume_echo: the swell clicks");

        const auto slow = swell (100.0f, 0.0f), fast = swell (0.0f, 0.0f); // 2 s and 20 ms
        check (db (energy (slow.left, 0.59, 0.61) / full) < -20.0 && db (energy (slow.left, 0.59, 0.61) / full) > -28.0, "auto_volume_echo: Swell 100 % must take 2 s");
        check (std::abs (db (energy (fast.left, 0.14, 0.16) / full)) < 0.5 && db (energy (fast.left, 0.100, 0.104) / (0.2 * full)) < -12.0, "auto_volume_echo: Swell 0 % must take 20 ms");

        // Mix balances the swelled signal against its echoes; the slot is told "all wet" so the unswelled dry signal stays out
        const auto echoes = swell (50.0f, 100.0f), both = swell (50.0f, 50.0f);
        check (energy (echoes.left, 0.0, 0.59) == 0.0 && std::abs (db (energy (echoes.left, 0.89, 0.91) / full)) < 1.0, "auto_volume_echo: Mix 100 % must leave the echo of the swelled note only");
        check (std::abs (db (energy (both.left, 0.39, 0.41) / full)) < 0.5, "auto_volume_echo: Mix 50 % must have the swelled signal at unity");

        Fx effect;
        effect.prepare (sampleRate, 256);
        for (const auto& other : models)
        {
            Knobs k = harness::defaultKnobs (other);
            setKnob (k, other, "Mix", 30.0f);
            effect.setModel (other.variant);
            effect.setParameters (k.data());
            check (std::abs (effect.getMix() - (other.variant == Fx::autoVolume ? 1.0f : 0.3f)) < 1.0e-6f, std::string (other.key) + ": getMix()");
        }
    }

    //==========================================================================
    // 10. Sweep Echo: the repeats' tone moves up and down at Swp Spd; Swp Dep 0 % leaves plain EP-1 echoes
    {
        const auto& m = models[Fx::sweepEcho];
        auto in = silence (5.0);
        harness::Lcg rng { 4242u };
        for (auto& s : in) s = 0.1f * (rng.next01() * 2.0f - 1.0f);

        auto tilt = [&] (float speed, float depth) // brightness of the echoes in 50 ms frames
        {
            Knobs k = plainKnobs (m, 300.0f, 0.0f);
            setKnob (k, m, "Swp Spd", speed); setKnob (k, m, "Swp Dep", depth);
            const auto out = render (m.variant, k, in);
            std::vector<double> frames;
            for (double t = 1.0; t < 4.95; t += 0.05)
                frames.push_back (bandDb (out.left, t, t + 0.05, 1500.0, true) - bandDb (out.left, t, t + 0.05, 400.0, false));
            return frames;
        };
        auto range = [] (const std::vector<double>& x) { return *std::max_element (x.begin(), x.end()) - *std::min_element (x.begin(), x.end()); };
        auto cycles = [] (const std::vector<double>& x) // upward crossings of the mean
        {
            double mean = 0.0;
            for (double v : x) mean += v / (double) x.size();
            int count = 0;
            for (size_t i = 1; i < x.size(); ++i)
                count += x[i - 1] < mean && x[i] >= mean ? 1 : 0;
            return count;
        };

        const auto still = tilt (1.0f, 0.0f), swept = tilt (1.0f, 100.0f), half = tilt (1.0f, 40.0f), quick = tilt (2.5f, 100.0f);
        std::printf ("  sweep echo: brightness range %.1f dB (Swp Dep 0 %%), %.1f dB (40 %%), %.1f dB (100 %%); %d and %d sweeps in 3.95 s at 1 and 2.5 Hz\n",
                     range (still), range (half), range (swept), cycles (swept), cycles (quick));
        // (the noise itself makes the 50 ms frames vary by a few dB)
        check (range (still) < 8.0 && range (swept) > 25.0 && range (half) > 12.0 && range (half) < range (swept), "sweep_echo: Swp Dep must set how far the filter sweeps");
        check (std::abs (cycles (swept) - 4) <= 1 && std::abs (cycles (quick) - 10) <= 1, "sweep_echo: Swp Spd must set how fast it sweeps");
    }

    //==========================================================================
    // 11. Multi-Head: heads at 1/4, 2/4, 3/4 and 4/4 of Time; all 16 switch combinations
    {
        const auto& m = models[Fx::multiHead];
        auto in = silence (1.0);
        addBurst (in, 0.05, 0.008, 1000.0, 0.2f);
        const double inputEnergy = energy (in, 0.0, 0.2);
        bool tapsRight = true, levelsRight = true, timesRight = true;

        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b)
            {
                Knobs k = harness::defaultKnobs (m);
                setKnob (k, m, "Time", 400.0f); setKnob (k, m, "Fdbk", 0.0f);
                setKnob (k, m, "Heads 1-2", (float) a); setKnob (k, m, "Heads 3-4", (float) b);
                const auto out = render (m.variant, k, in);
                const bool on[4] = { (a & 1) != 0, (a & 2) != 0, (b & 1) != 0, (b & 2) != 0 };
                const int count = (int) on[0] + (int) on[1] + (int) on[2] + (int) on[3];

                for (int h = 0; h < 4; ++h)
                {
                    const double centre = 0.054 + 0.1 * (h + 1), e = energy (out.left, centre - 0.04, centre + 0.04);
                    tapsRight = tapsRight && (on[h] ? e > 0.1 * inputEnergy / count : e < 1.0e-6 * inputEnergy);
                    if (on[h])
                    {
                        levelsRight = levelsRight && std::abs (db (e * count / inputEnergy)) < 2.0; // each head at 1 / sqrt (heads)
                        timesRight = timesRight && std::abs (centroid (out.left, centre - 0.04, centre + 0.04) - centre) < 0.001;
                    }
                }
                tapsRight = tapsRight && identical (out.left, out.right);
            }

        check (tapsRight, "multi_head: the head switches do not give the expected taps");
        check (timesRight, "multi_head: heads must sit at 1/4, 2/4, 3/4 and 4/4 of Time");
        check (levelsRight, "multi_head: head levels");

        // the mix of the heads is fed back: with heads 2 and 4 (200 and 400 ms) the taps at 400 ms and 600 ms get regenerated
        Knobs k = harness::defaultKnobs (m);
        setKnob (k, m, "Time", 400.0f); setKnob (k, m, "Fdbk", 50.0f);
        const auto out = render (m.variant, k, in);
        check (energy (out.left, 0.62, 0.69) > 1.0e-3 * inputEnergy && energy (out.left, 0.72, 0.79) < 1.0e-6 * inputEnergy,
               "multi_head: feedback must repeat the head pattern");
    }

    //==========================================================================
    // 12. 100 % feedback for 20 s of playing, at the default, the shortest and the longest Time
    {
        const auto riff = harness::makeInput (sampleRate, 4.0);
        std::vector<float> in;
        for (int i = 0; i < 5; ++i)
            in.insert (in.end(), riff.begin(), riff.end());

        float worst = 0.0f;
        std::string worstKey;
        for (const auto& m : models)
            for (float timeMs : { 0.0f, 20.0f, 2000.0f })
            {
                Knobs k = harness::defaultKnobs (m);
                for (size_t i = 0; i < m.knobs.size(); ++i)
                {
                    const std::string name = m.knobs[i].name;
                    if (name.find ("Fdbk") != std::string::npos) k[i] = 100.0f;
                    if (name.find ("Time") != std::string::npos && timeMs > 0.0f) k[i] = timeMs;
                    if (name == "Heads 1-2" || name == "Heads 3-4") k[i] = 3.0f; // all four heads: the hottest loop
                    if (name == "Drive" || name == "Treble") k[i] = 100.0f;
                }

                const auto stats = harness::measure (render (m.variant, k, in).left);
                if (stats.peak > worst) { worst = stats.peak; worstKey = m.key; }
                check (stats.finite && stats.peak < 5.0f, std::string (m.key) + ": Fdbk 100 % for 20 s peaks at " + num (stats.peak)); // (the loop itself stays below 1.5; Treble adds 9 dB)
            }
        std::printf ("  Fdbk 100 %% for 20 s: highest peak %.2f (%s)\n", worst, worstKey.c_str());

        // What Fdbk 100 % means once the playing stops (11 s of silence after the riff):
        //   digital models hold the repeats (loop gain exactly 1), tape / platter / BBD run away into saturation
        //   like the originals, Auto-Volume Echo just holds on. At 80 % everything dies away.
        auto once = silence (16.0);
        std::copy (riff.begin(), riff.end(), once.begin());
        std::printf ("  after the playing stops: level 11 s later against the first second, at Fdbk 80 %% and 100 %%\n");
        for (const auto& m : models)
        {
            auto later = [&] (float fdbk, float& peak)
            {
                Knobs k = harness::defaultKnobs (m);
                for (size_t i = 0; i < m.knobs.size(); ++i)
                    if (std::string (m.knobs[i].name).find ("Fdbk") != std::string::npos)
                        k[i] = fdbk;
                const auto out = render (m.variant, k, once);
                peak = peakOf (out.left, 5.0, 16.0);
                return db ((energy (out.left, 15.0, 16.0) + energy (out.right, 15.0, 16.0)) / (energy (out.left, 4.1, 5.1) + energy (out.right, 4.1, 5.1)));
            };

            float peak80 = 0.0f, peak100 = 0.0f;
            const double at80 = later (80.0f, peak80), at100 = later (100.0f, peak100);
            const int v = m.variant;
            const bool regenerates = v >= Fx::tubeEcho && v != Fx::autoVolume;
            std::printf ("    %-20s %7.1f dB  %6.1f dB (peak %.2f)\n", m.key, at80, at100, peak100);

            check (at80 < -20.0, std::string (m.key) + ": Fdbk 80 % must die away");
            if (regenerates)
                check (at100 > 0.0 && peak100 > 0.3 && peak100 < 1.5, std::string (m.key) + ": Fdbk 100 % must run away into (bounded) self-oscillation");
            else if (v == Fx::loRes || v == Fx::autoVolume || v == Fx::dynamicDly) // (requantising / filtered loop / still coming up in the first second)
                check (at100 > -12.0 && at100 < 3.0, std::string (m.key) + ": Fdbk 100 % must hold the repeats");
            else
                check (std::abs (at100) < 1.0, std::string (m.key) + ": Fdbk 100 % must hold the repeats at their level");
        }
    }

    //==========================================================================
    // 13. Changing Time while a note rings must not click: glide (tape, BBD) or crossfade (digital)
    {
        auto sine = silence (3.2);
        addTone (sine, 0.0, 3.2, 220.0, 0.3f);
        const float cleanStep = (float) (2.0 * fx::pi * 220.0 / sampleRate);
        float worst = 0.0f;
        std::string worstKey;

        for (const auto& m : models)
        {
            Knobs k = plainKnobs (m, 300.0f, 40.0f);
            if (m.variant == Fx::autoVolume) setKnob (k, m, "Mix", 100.0f);
            auto change = [&] (int pos, Knobs& knobs)
            {
                const double t = pos / sampleRate;
                const float timeMs = t < 1.0 ? 300.0f : (t < 1.6 ? 450.0f : (t < 2.2 ? 120.0f : (t < 2.6 ? 1000.0f : 60.0f)));
                for (size_t i = 0; i < m.knobs.size(); ++i)
                    if (std::string (m.knobs[i].name).find ("Time") != std::string::npos)
                        knobs[i] = timeMs;
            };

            const auto out = render (m.variant, k, sine, sampleRate, change);
            // relative to the echoes' level: a sine moves by 2 pi f / fs of its peak per sample, a click by about its peak.
            // A glide plays the line at up to twice its speed (and the tape models add their own harmonics), hence the factor.
            const float ratio = maxStep (out.left, 0.5) / (cleanStep * std::max (0.3f, peakOf (out.left)));
            if (ratio > worst) { worst = ratio; worstKey = m.key; }
            check (ratio < (isTapeOrAnalog (m.variant) ? 4.0f : 2.0f), std::string (m.key) + ": Time change clicks (step " + num (ratio) + " times a clean sine's)");
            check (energy (out.left, 3.0, 3.2) > 1.0e-3, std::string (m.key) + ": no echoes after the Time changes");
        }
        std::printf ("  Time changes: largest step %.2f times a clean sine's (%s)\n", worst, worstKey.c_str());

        // tape-style: the pitch bends while the time glides; digital: it does not
        auto sine1k = silence (3.0);
        addTone (sine1k, 0.0, 3.0, 1000.0, 0.2f);
        for (int v : { (int) Fx::digitalDelay, (int) Fx::tubeEcho, (int) Fx::analogEcho })
        {
            const auto& m = models[(size_t) v];
            Knobs k = plainKnobs (m, 300.0f, 0.0f);
            auto change = [&] (int pos, Knobs& knobs) { knobs[0] = pos < (int) sampleRate ? 300.0f : 360.0f; };
            const auto out = render (v, k, sine1k, sampleRate, change);
            const double bend = peakAbs (pitchDeviation (out.left, 0.9, 2.5, 1000.0));
            if (v == Fx::digitalDelay) check (bend < 0.002 || bend > 0.2, "digital_delay: a Time change must crossfade, not bend the pitch");
            else                       check (bend > 0.003 && bend < 0.5, std::string (m.key) + ": a Time change must bend the pitch (" + num (bend) + ")");

            // and the new time is reached
            auto tail = silence (3.0);
            addBurst (tail, 2.0, 0.008, 1000.0, 0.05f);
            const auto late = render (v, k, tail, sampleRate, change);
            check (std::abs (centroid (late.left, 2.2, 2.6) - 2.004 - 0.36) < 0.001, std::string (m.key) + ": new Time not reached");
        }
    }

    // other knobs jumping between their ends while a note rings: no clicks either
    {
        auto sine = silence (3.0);
        addTone (sine, 0.0, 3.0, 220.0, 0.3f);
        const float cleanStep = (float) (2.0 * fx::pi * 220.0 / sampleRate);
        float worst = 0.0f;
        std::string worstKey;

        for (const auto& m : models)
            for (size_t knob = 2; knob < m.knobs.size(); ++knob)
            {
                const std::string name = m.knobs[knob].name;
                if (name.find ("Time") != std::string::npos || name.find ("Note") != std::string::npos || name == "Res" || (name == "Mix" && m.variant != Fx::autoVolume))
                    continue; // Time: above. Res switches the word length (its steps are the effect). Mix is the slot's, except in Auto-Volume Echo.

                Knobs k = harness::defaultKnobs (m);
                if (name != "Depth" && name != "Wow/Flt" && name != "ModDep")
                    k = plainKnobs (m, 300.0f, 40.0f);
                if (m.variant == Fx::loRes) setKnob (k, m, "Res", 18.0f);
                auto change = [&] (int pos, Knobs& knobs)
                {
                    knobs[knob] = ((int) (pos / (0.25 * sampleRate)) & 1) ? m.knobs[knob].max : m.knobs[knob].min;
                };

                const auto out = render (m.variant, k, sine, sampleRate, change);
                const float ratio = std::max (maxStep (out.left, 0.5), maxStep (out.right, 0.5)) / (cleanStep * std::max (0.3f, std::max (peakOf (out.left), peakOf (out.right))));
                if (ratio > worst) { worst = ratio; worstKey = std::string (m.key) + " " + name; }
                check (ratio < 2.5f, std::string (m.key) + ": knob " + name + " clicks (step " + num (ratio) + " times a clean sine's)");
            }
        std::printf ("  other knobs jumping between their ends: largest step %.2f times a clean sine's (%s)\n", worst, worstKey.c_str());
    }

    //==========================================================================
    // 14. The longest time at every sample rate, every model: 2000 ms at 44.1, 48, 96 and 192 kHz
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const auto& m : models)
        {
            const int v = m.variant;
            const bool swell = v == Fx::autoVolume;
            auto in = silence (4.6, fs);
            addBurst (in, 0.05, swell ? 0.1 : 0.008, 1000.0, 0.05f, fs);
            Knobs k = plainKnobs (m, 2000.0f, 0.0f);
            if (v == Fx::stereoDelay) { k[0] = 2000.0f; k[3] = 2000.0f; }
            if (v == Fx::multiHead) { setKnob (k, m, "Heads 1-2", 0.0f); setKnob (k, m, "Heads 3-4", 2.0f); }
            if (swell) setKnob (k, m, "Mix", 50.0f); // the swelled burst itself is the reference
            const auto out = render (v, k, in, fs);

            if (v == Fx::reverse)
            {
                // the burst sits 54 ms into chunk 0, so it comes back 54 ms before the end of chunk 1
                const double at = centroid (out.left, 3.5, 4.5, fs);
                check (std::abs (at - (4.0 - 0.054)) < 0.001, "reverse: a 2000 ms chunk at " + num (fs) + " Hz comes back at " + num (at) + " s");
            }
            else
            {
                const double at = centroid (out.left, 1.5, 2.6, fs) - (swell ? centroid (out.left, 0.0, 0.5, fs) : 0.054);
                check (std::abs (at - 2.0) < (isTapeOrAnalog (v) ? 0.003 : 0.00005), std::string (m.key) + ": 2000 ms at " + num (fs) + " Hz gives " + num (at * 1000.0) + " ms");
            }
            check (harness::measure (out.left).finite, std::string (m.key) + ": NaN at " + num (fs) + " Hz");
        }

    // a new sample rate without new knob values (prepare called again): the times must follow
    for (int v : { (int) Fx::digitalDelay, (int) Fx::tubeEcho, (int) Fx::reverse, (int) Fx::pingPong, (int) Fx::multiHead })
    {
        const auto& m = models[(size_t) v];
        Knobs k = plainKnobs (m, 2000.0f, 0.0f);
        if (v == Fx::multiHead) { setKnob (k, m, "Heads 1-2", 0.0f); setKnob (k, m, "Heads 3-4", 2.0f); }
        Fx effect;
        effect.prepare (192000.0, 256);
        effect.setModel (v);
        effect.setParameters (k.data());
        effect.reset();
        effect.prepare (44100.0, 256);

        auto l = silence (4.6, 44100.0);
        addBurst (l, 0.05, 0.008, 1000.0, 0.05f, 44100.0);
        auto r = l;
        for (int pos = 0; pos < (int) l.size(); pos += 256)
            effect.process (l.data() + pos, r.data() + pos, std::min (256, (int) l.size() - pos));

        const double at = v == Fx::reverse ? centroid (l, 3.5, 4.5, 44100.0) - (4.0 - 0.054) + 2.0 : centroid (l, 1.5, 2.6, 44100.0) - 0.054;
        check (std::abs (at - 2.0) < 0.003, std::string (m.key) + ": after prepare() at another rate, 2000 ms became " + num (at * 1000.0) + " ms");
    }

    // shortest time, and the models' tone does not depend on the sample rate
    for (double fs : { 44100.0, 96000.0, 192000.0 })
    {
        const auto& m = models[Fx::analogEcho];
        auto in = silence (0.5, fs);
        addBurst (in, 0.05, 0.004, 1000.0, 0.05f, fs);
        const auto out = render (m.variant, plainKnobs (m, 20.0f, 0.0f), in, fs);
        const double at = centroid (out.left, 0.06, 0.09, fs) - 0.052;
        check (std::abs (at - 0.02) < 0.001, "analog_echo: 20 ms at " + num (fs) + " Hz gives " + num (at * 1000.0) + " ms");
    }

    //==========================================================================
    // 15. getTailSeconds(): about the time the repeats need to die away
    {
        Fx effect;
        effect.prepare (sampleRate, 256);
        bool inRange = true;
        for (const auto& m : models)
            for (float fdbk : { 0.0f, 50.0f, 100.0f })
                for (float timeMs : { 20.0f, 500.0f, 2000.0f })
                {
                    Knobs k = plainKnobs (m, timeMs, fdbk);
                    if (m.variant == Fx::stereoDelay) { k[0] = k[3] = timeMs; k[2] = k[5] = fdbk; }
                    effect.setModel (m.variant);
                    effect.setParameters (k.data());
                    const float tail = effect.getTailSeconds();
                    inRange = inRange && tail >= timeMs * 0.001f + 0.1f - 1.0e-4f && tail <= 20.0f && (fdbk < 100.0f || tail == 20.0f);
                }
        check (inRange, "getTailSeconds() must be at least Time + 0.1 s, at most 20 s, and 20 s at Fdbk 100 %");

        // where the tape / BBD models regenerate past unity (Fdbk above about 90 %), the tail is "for ever" at any Time
        Knobs runaway = plainKnobs (models[Fx::tubeEcho], 20.0f, 95.0f);
        effect.setModel (Fx::tubeEcho);
        effect.setParameters (runaway.data());
        check (effect.getTailSeconds() == 20.0f, "tube_echo: getTailSeconds() at Fdbk 95 % (self-oscillating) must be 20 s");

        for (int v : { (int) Fx::digitalDelay, (int) Fx::tubeEcho, (int) Fx::analogEcho, (int) Fx::pingPong })
        {
            const auto& m = models[(size_t) v];
            auto in = silence (14.0);
            addBurst (in, 0.05, 0.02, 1000.0, 0.2f);
            Knobs k = plainKnobs (m, 400.0f, 60.0f);
            effect.setModel (v);
            effect.setParameters (k.data());
            const auto out = render (v, k, in);
            const float first = std::max (peakOf (out.left, 0.4, 0.6), peakOf (out.right, 0.4, 0.95));
            double lastLoud = 0.0; // last moment above -60 dB of the first echo
            for (size_t i = 0; i < out.left.size(); ++i)
                if (std::max (std::abs (out.left[i]), std::abs (out.right[i])) > 0.001f * first)
                    lastLoud = (double) i / sampleRate - 0.06;
            std::printf ("  tail of %-16s measured %.2f s, getTailSeconds() %.2f s\n", m.key, lastLoud, effect.getTailSeconds());
            check (effect.getTailSeconds() > 0.6 * lastLoud && effect.getTailSeconds() < 1.6 * lastLoud + 0.2, std::string (m.key) + ": getTailSeconds() is far from the measured tail");
        }
    }

    //==========================================================================
    // 16. processDry: the original's preamp on the dry signal, for Tube Echo, Tape Echo, Sweep Echo and Echo Platter only.
    //     Their "Dry" twins return the same echoes and leave the dry signal alone.
    {
        // the riff without the plucks' DC offsets, which would hide everything else behind a high-pass
        auto in = harness::makeInput (sampleRate, 2.0);
        {
            fx::Biquad f1, f2;
            f1.setHighPass (sampleRate, 40.0, 0.54);
            f2.setHighPass (sampleRate, 40.0, 1.31);
            for (auto& v : in) v = f2.process (f1.process (v));
        }
        const double inputEnergy = energy (in, 0.0, 2.0);
        harness::writeFloats (outDir / "dry_input.f32", in); // with the dry_<key>.f32 below: a reference for the JavaScript port's processDry
        std::printf ("  processDry: level, and tone against the untouched signal\n");

        auto dryPath = [&] (Fx& effect, const fx::ModelInfo& m, Knobs k)
        {
            effect.setModel (m.variant);
            effect.setParameters (k.data());
            effect.reset();
            Stereo out { in, in };
            for (int pos = 0; pos < (int) in.size(); pos += 256)
            {
                effect.setParameters (k.data());
                effect.processDry (out.left.data() + pos, out.right.data() + pos, std::min (256, (int) in.size() - pos));
            }
            return out;
        };

        Fx shared;
        shared.prepare (sampleRate, 256);
        for (const auto& m : models)
        {
            const int v = m.variant;
            const bool coloured = v == Fx::tubeEcho || v == Fx::tapeEcho || v == Fx::sweepEcho || v == Fx::echoPlatter;
            const Knobs k = harness::defaultKnobs (m);
            const auto out = dryPath (shared, m, k);

            if (! coloured)
            {
                check (identical (out.left, in) && identical (out.right, in), std::string (m.key) + ": processDry must leave the signal untouched");
                continue;
            }

            const double level = db (energy (out.left, 0.0, 2.0) / inputEnergy);
            const double highs = bandDb (out.left, 0.0, 2.0, 3000.0, true) - bandDb (in, 0.0, 2.0, 3000.0, true);
            const double lows = bandDb (out.left, 0.0, 2.0, 100.0, false) - bandDb (in, 0.0, 2.0, 100.0, false);

            // harmonics the preamp adds to a 220 Hz note at guitar level
            auto tone = silence (0.5);
            addTone (tone, 0.0, 0.5, 220.0, 0.3f);
            auto harmonics = [&] (const Knobs& knobs)
            {
                Fx e;
                e.prepare (sampleRate, 256);
                e.setModel (v);
                e.setParameters (knobs.data());
                e.reset();
                auto l = tone, r = tone;
                for (int pos = 0; pos < (int) l.size(); pos += 256)
                    e.processDry (l.data() + pos, r.data() + pos, std::min (256, (int) l.size() - pos));
                return distortion (l, 0.2, 0.45, 220.0);
            };
            const double thd = harmonics (k);

            std::printf ("    %-20s level %+.2f dB, above 3 kHz %+.2f dB, below 100 Hz %+.2f dB, distortion of a 220 Hz note %.2f %%\n", m.key, level, highs, lows, 100.0 * thd);
            check (std::abs (highs) > 0.5 || std::abs (lows) > 0.5 || thd > 0.003, std::string (m.key) + ": processDry must colour the dry signal");
            check (std::abs (highs) < 4.0 && std::abs (lows) < 6.0 && thd < 0.05, std::string (m.key) + ": processDry must colour the dry signal, not wreck it");
            check (std::abs (level) < 1.0, std::string (m.key) + ": processDry changes the level by " + num (level) + " dB");
            check (identical (out.left, out.right) && harness::measure (out.left).finite, std::string (m.key) + ": processDry sides differ");

            Fx fresh;
            fresh.prepare (sampleRate, 256);
            check (identical (dryPath (fresh, m, k).left, out.left), std::string (m.key) + ": reset() does not clear processDry's state");
            harness::writeFloats (outDir / (std::string ("dry_") + m.key + ".f32"), out.left);

            if (v == Fx::tapeEcho)    check (highs > 0.5, "tape_echo: the EP-3 preamp must brighten the dry signal");
            if (v == Fx::echoPlatter) check (lows < -1.0 && highs < 0.0, "echo_platter: the Echorec must thin out the dry signal's lows and soften its top");
            if (v == Fx::tubeEcho)    check (highs < -0.5 && thd > 0.005, "tube_echo: the EP-1's tube stage must warm up the dry signal");
            if (v == Fx::tubeEcho || v == Fx::echoPlatter)
            {
                Knobs clean = k, driven = k;
                setKnob (clean, m, "Drive", 0.0f);
                setKnob (driven, m, "Drive", 100.0f);
                check (harmonics (driven) > 1.5 * harmonics (clean) && harmonics (clean) > 0.002, std::string (m.key) + ": Drive must push the dry signal's tube stage too");
            }

            // the twin: same echoes, untouched dry signal
            const auto& twin = models[(size_t) v + 1];
            const auto wet = render (v, k, in), wetTwin = render (twin.variant, k, in);
            check (identical (wet.left, wetTwin.left) && identical (wet.right, wetTwin.right), std::string (twin.key) + ": echoes must equal those of " + m.key);
        }
    }

    // models that share knobs must still differ from each other
    {
        const auto in = harness::makeInput (sampleRate, 2.0);
        const int family[] = { Fx::tubeEcho, Fx::echoPlatter, Fx::tapeEcho, Fx::analogEcho, Fx::digitalDelay, Fx::analogMod, Fx::digMod };
        std::vector<Stereo> outs;
        for (int v : family)
        {
            Knobs k = harness::defaultKnobs (models[(size_t) v]);
            k[0] = 300.0f;
            k[2] = 50.0f;
            outs.push_back (render (v, k, in));
        }

        for (size_t a = 0; a < outs.size(); ++a)
            for (size_t b = a + 1; b < outs.size(); ++b)
            {
                double difference = 0.0;
                for (size_t i = 0; i < in.size(); ++i)
                    difference += ((double) outs[a].left[i] - outs[b].left[i]) * ((double) outs[a].left[i] - outs[b].left[i]);
                check (db (difference / energy (outs[a].left, 0.0, 2.0)) > -25.0, std::string (models[(size_t) family[a]].key) + " and " + models[(size_t) family[b]].key + " sound the same");
            }
    }

    std::printf ("%s\n", failed == 0 ? "ALL CHECKS PASSED" : (std::to_string (failed) + " CHECK(S) FAILED").c_str());
    return failed == 0 ? 0 : 1;
}
