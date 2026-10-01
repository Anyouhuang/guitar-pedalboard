// Standalone test of the modulation engine (see Harness.h): the harness run, then checks of what defines each
// of the 22 models (rates, depths, wave shapes, notches, pitch deviation, sidebands, rotor inertia, stereo).
#include "Harness.h"
#include "../../Source/DSP/fx/Mod.h"

#include <complex>
#include <cstdarg>
#include <functional>

namespace
{
using harness::Knobs;
using V = fx::ModFx;

constexpr double fs = 48000.0;
int failedChecks = 0;

std::string text (const char* format, ...)
{
    char buffer[400];
    va_list args;
    va_start (args, format);
    std::vsnprintf (buffer, sizeof (buffer), format, args);
    va_end (args);
    return buffer;
}

void expect (bool ok, const std::string& what)
{
    if (! ok)
        ++failedChecks;
    std::printf ("    %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

const std::vector<fx::ModelInfo>& models()
{
    static const auto list = fx::modModels();
    return list;
}

//==============================================================================
struct Setup { int variant = 0; Knobs knobs {}; };

/** A model at its default knobs, with the named knobs changed. */
Setup setup (int variant, std::initializer_list<std::pair<const char*, double>> values = {})
{
    const auto& m = models()[(size_t) variant];
    Setup s { variant, harness::defaultKnobs (m) };
    for (const auto& v : values)
    {
        bool found = false;
        for (size_t i = 0; i < m.knobs.size(); ++i)
            if (std::string (m.knobs[i].name) == v.first)
            {
                s.knobs[i] = (float) v.second;
                found = true;
            }
        if (! found)
        {
            std::printf ("test error: %s has no knob %s\n", m.key, v.first);
            std::exit (3);
        }
    }
    return s;
}

using Signal = std::function<float (int)>;
Signal dc (float level)                      { return [=] (int) { return level; }; }
Signal sine (double hz, double amplitude, double rate = fs) { return [=] (int i) { return (float) (amplitude * std::sin (2.0 * fx::pi * hz * i / rate)); }; }
Signal impulses (int period, float amplitude) { return [=] (int i) { return i % period == 0 ? amplitude : 0.0f; }; }

struct Stereo { std::vector<float> l, r; };
struct Change { double at; Setup setup; };

/** Runs the engine like a slot does (256-sample blocks, knobs before every block). */
Stereo render (const Setup& s, double seconds, const Signal& in, const std::vector<Change>& changes = {},
               bool leftOnly = false, double rate = fs)
{
    fx::ModFx effect;
    effect.prepare (rate, 256);
    effect.setModel (s.variant);
    effect.setParameters (s.knobs.data());
    effect.reset();

    const int total = (int) (seconds * rate);
    Stereo out;
    out.l.resize ((size_t) total);
    out.r.resize ((size_t) total);

    for (int pos = 0; pos < total; pos += 256)
    {
        const int n = std::min (256, total - pos);
        const Knobs* k = &s.knobs;
        for (const auto& c : changes)
            if (pos >= (int) (c.at * rate))
                k = &c.setup.knobs;
        effect.setParameters (k->data());

        for (int i = 0; i < n; ++i)
        {
            const float x = in (pos + i);
            out.l[(size_t) (pos + i)] = x;
            out.r[(size_t) (pos + i)] = leftOnly ? 0.0f : x;
        }
        effect.process (out.l.data() + pos, out.r.data() + pos, n);
    }
    return out;
}

//==============================================================================
/** A measured quantity over time. */
struct Track
{
    std::vector<double> t;
    std::vector<float> v;

    float min() const { return *std::min_element (v.begin(), v.end()); }
    float max() const { return *std::max_element (v.begin(), v.end()); }
    float mean() const { double s = 0.0; for (float x : v) s += x; return (float) (s / (double) std::max<size_t> (1, v.size())); }
    float median() const { auto c = v; std::sort (c.begin(), c.end()); return c[c.size() / 2]; }
    double rate() const { return (double) (t.size() - 1) / (t.back() - t.front()); }
};

Track track (const std::vector<float>& x, double scale = 1.0)
{
    Track out;
    for (size_t i = 0; i < x.size(); ++i)
    {
        out.t.push_back ((double) i / fs);
        out.v.push_back ((float) (scale * x[i]));
    }
    return out;
}

Track cut (const Track& in, double t0, double t1)
{
    Track out;
    for (size_t i = 0; i < in.t.size(); ++i)
        if (in.t[i] >= t0 && in.t[i] < t1)
        {
            out.t.push_back (in.t[i]);
            out.v.push_back (in.v[i]);
        }
    return out;
}

/** Times at which the track crosses `level` (default: halfway between its extremes), with hysteresis. */
std::vector<double> crossings (const Track& x, bool rising = true, float level = std::numeric_limits<float>::quiet_NaN())
{
    const float lo = x.min(), hi = x.max(), mid = std::isnan (level) ? 0.5f * (lo + hi) : level, hyst = (std::isnan (level) ? 0.15f : 0.03f) * (hi - lo);
    const float sign = rising ? 1.0f : -1.0f;
    std::vector<double> times;
    bool armed = false;
    for (size_t i = 1; i < x.v.size(); ++i)
    {
        const float a = sign * (x.v[i - 1] - mid), b = sign * (x.v[i] - mid);
        if (b < -hyst)
            armed = true;
        if (armed && a < 0.0f && b >= 0.0f)
        {
            times.push_back (x.t[i - 1] + (x.t[i] - x.t[i - 1]) * (double) (-a / (b - a)));
            armed = false;
        }
    }
    return times;
}

/** Cycles per second of a track that swings once per cycle. */
double cycleRate (const Track& x)
{
    const auto c = crossings (x);
    return c.size() < 2 ? 0.0 : (double) (c.size() - 1) / (c.back() - c.front());
}

/** RMS over consecutive windows. */
Track envelope (const std::vector<float>& x, int window)
{
    Track out;
    for (size_t pos = 0; pos + (size_t) window <= x.size(); pos += (size_t) window)
    {
        double sum = 0.0;
        for (int i = 0; i < window; ++i)
            sum += (double) x[pos + (size_t) i] * x[pos + (size_t) i];
        out.t.push_back (((double) pos + 0.5 * window) / fs);
        out.v.push_back ((float) std::sqrt (sum / window));
    }
    return out;
}

/** Frequency of a sine-like signal over time: `cycles` periods between rising zero crossings at a time. */
Track frequencyTrack (const std::vector<float>& x, int cycles = 4)
{
    std::vector<double> zero;
    for (size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            zero.push_back (((double) (i - 1) + (double) (-x[i - 1] / (x[i] - x[i - 1]))) / fs);

    Track out;
    for (size_t i = 0; i + (size_t) cycles < zero.size(); ++i)
    {
        out.t.push_back (0.5 * (zero[i] + zero[i + (size_t) cycles]));
        out.v.push_back ((float) (cycles / (zero[i + (size_t) cycles] - zero[i])));
    }
    return out;
}

/** Average frequency between the first and the last rising zero crossing in [t0, t1). */
double averageFrequency (const std::vector<float>& x, double t0, double t1)
{
    double first = -1.0, last = -1.0;
    int count = 0;
    for (size_t i = std::max<size_t> (1, (size_t) (t0 * fs)); i < std::min (x.size(), (size_t) (t1 * fs)); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            last = ((double) (i - 1) + (double) (-x[i - 1] / (x[i] - x[i - 1]))) / fs;
            if (first < 0.0)
                first = last;
            ++count;
        }
    return count < 2 ? 0.0 : (count - 1) / (last - first);
}

/** Amplitude of the component at `hz` in [t0, t1) (Hann window). */
double amplitudeAt (const std::vector<float>& x, double hz, double t0, double t1)
{
    const size_t from = (size_t) (t0 * fs), to = std::min (x.size(), (size_t) (t1 * fs));
    std::complex<double> sum = 0.0;
    double weight = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * fx::pi * (double) (i - from) / (double) (to - from));
        sum += w * (double) x[i] * std::polar (1.0, -2.0 * fx::pi * hz * (double) i / fs);
        weight += w;
    }
    return 2.0 * std::abs (sum) / weight;
}

double db (double ratio) { return ratio > 1.0e-12 ? 20.0 * std::log10 (ratio) : -240.0; }

double rms (const std::vector<float>& x, double t0 = 0.0, double t1 = 1.0e9)
{
    const size_t from = (size_t) (t0 * fs), to = std::min (x.size(), (size_t) std::min (t1 * fs, 4.0e9));
    double sum = 0.0;
    for (size_t i = from; i < to; ++i)
        sum += (double) x[i] * x[i];
    return std::sqrt (sum / (double) std::max<size_t> (1, to - from));
}

/** How far apart the two sides are: RMS of (L - R) over RMS of (L + R). */
double sideRatio (const Stereo& s, double t0 = 0.0)
{
    double side = 0.0, mid = 0.0;
    for (size_t i = (size_t) (t0 * fs); i < s.l.size(); ++i)
    {
        side += (double) (s.l[i] - s.r[i]) * (s.l[i] - s.r[i]);
        mid += (double) (s.l[i] + s.r[i]) * (s.l[i] + s.r[i]);
    }
    return std::sqrt (side / std::max (1.0e-30, mid));
}

bool identical (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
}

/** RMS of (a - b) relative to a, in dB, both sides. */
double differenceDb (const Stereo& a, const Stereo& b)
{
    double diff = 0.0, sum = 0.0;
    for (size_t i = 0; i < a.l.size(); ++i)
    {
        diff += (double) (a.l[i] - b.l[i]) * (a.l[i] - b.l[i]) + (double) (a.r[i] - b.r[i]) * (a.r[i] - b.r[i]);
        sum += (double) a.l[i] * a.l[i] + (double) a.r[i] * a.r[i];
    }
    return db (std::sqrt (diff / std::max (1.0e-30, sum)));
}

/** How badly a uniformly sampled track fails to repeat after `lag` seconds (0 = repeats exactly). */
double repeatError (const Track& e, double lag)
{
    const size_t shift = (size_t) std::lround (lag * e.rate());
    const float range = e.max() - e.min();
    double sum = 0.0;
    size_t count = 0;
    for (size_t i = 0; i + shift < e.v.size(); ++i, ++count)
        sum += std::abs (e.v[i] - e.v[i + shift]);
    return sum / (double) std::max<size_t> (1, count) / std::max (1.0e-9f, range);
}

/** The shortest time after which a uniformly sampled track repeats. */
double repeatPeriod (const Track& e, double shortest, double longest)
{
    const double step = 1.0 / e.rate();
    double best = 1.0e9;
    for (double lag = shortest; lag <= longest; lag += step)
        best = std::min (best, repeatError (e, lag));
    for (double lag = shortest + step; lag <= longest - step; lag += step)
    {
        const double here = repeatError (e, lag);
        if (here < best + 0.04 && here <= repeatError (e, lag - step) && here <= repeatError (e, lag + step))
            return lag;
    }
    return 0.0;
}

/** Delay of the strongest (positive or negative) peak after every impulse of an impulse train, in ms. */
Track delayTrack (const std::vector<float>& y, int period, double fromMs, double toMs, bool positive = true)
{
    Track out;
    const float sign = positive ? 1.0f : -1.0f;
    for (size_t start = 0; start + (size_t) period <= y.size(); start += (size_t) period)
    {
        const int from = (int) (fromMs * 0.001 * fs), to = std::min (period - 2, (int) (toMs * 0.001 * fs));
        int best = from;
        for (int i = from; i <= to; ++i)
            if (sign * y[start + (size_t) i] > sign * y[start + (size_t) best])
                best = i;
        double offset = 0.0;
        if (best > from && best < to)
        {
            const double a = sign * y[start + (size_t) best - 1], b = sign * y[start + (size_t) best], c = sign * y[start + (size_t) best + 1];
            if (a - 2.0 * b + c < 0.0)
                offset = 0.5 * (a - c) / (a - 2.0 * b + c);
        }
        out.t.push_back ((double) start / fs);
        out.v.push_back ((float) (((double) best + offset) * 1000.0 / fs));
    }
    return out;
}

/** Amplitude and phase (cycles) of the component at `hz` in a uniformly sampled track, over whole cycles. */
std::pair<double, double> component (const Track& x, double hz)
{
    const double duration = std::floor ((x.t.back() - x.t.front()) * hz) / hz;
    std::complex<double> sum = 0.0;
    int count = 0;
    const float mean = x.mean();
    for (size_t i = 0; i < x.t.size() && x.t[i] - x.t.front() < duration; ++i, ++count)
        sum += (double) (x.v[i] - mean) * std::polar (1.0, -2.0 * fx::pi * hz * x.t[i]);
    return { 2.0 * std::abs (sum) / std::max (1, count), std::arg (sum) / (2.0 * fx::pi) };
}

//==============================================================================
void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const auto w = std::polar (1.0, -2.0 * fx::pi / (double) len);
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> wn = 1.0;
            for (size_t k = 0; k < len / 2; ++k, wn *= w)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * wn;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

constexpr int fftSize = 16384;
double binHz (double bin) { return bin * fs / fftSize; }
int hzBin (double hz) { return (int) std::lround (hz * fftSize / fs); }

/** Magnitude responses (dB per FFT bin) to impulses fired at the given times while the effect keeps running. */
std::vector<std::vector<double>> responses (const Setup& s, const std::vector<double>& times, bool right = false, int capture = 4096)
{
    std::vector<int> at;
    for (double t : times)
        at.push_back ((int) std::lround (t * fs));
    const auto out = render (s, times.back() + (capture + 16) / fs, [&] (int i) { return std::find (at.begin(), at.end(), i) != at.end() ? 0.5f : 0.0f; });

    std::vector<std::vector<double>> result;
    for (int start : at)
    {
        std::vector<std::complex<double>> a ((size_t) fftSize);
        for (int i = 0; i < capture; ++i)
        {
            const double fade = i < capture / 2 ? 1.0 : 0.5 + 0.5 * std::cos (fx::pi * (double) (i - capture / 2) / (capture / 2));
            a[(size_t) i] = 2.0 * fade * (right ? out.r : out.l)[(size_t) (start + i)];
        }
        fft (a);
        std::vector<double> mag ((size_t) fftSize / 2);
        for (size_t b = 0; b < mag.size(); ++b)
            mag[b] = db (std::abs (a[b]));
        result.push_back (mag);
    }
    return result;
}

std::vector<double> response (const Setup& s, double time = 0.1, bool right = false) { return responses (s, { time }, right)[0]; }

double maxDb (const std::vector<double>& mag, double fromHz, double toHz)
{
    return *std::max_element (mag.begin() + hzBin (fromHz), mag.begin() + hzBin (toHz) + 1);
}

double minDb (const std::vector<double>& mag, double fromHz, double toHz)
{
    return *std::min_element (mag.begin() + hzBin (fromHz), mag.begin() + hzBin (toHz) + 1);
}

/** Frequencies of the notches between fromHz and toHz: local minima at least depthDb under the highest point. */
std::vector<double> notches (const std::vector<double>& mag, double fromHz, double toHz, double depthDb = 12.0)
{
    const double reference = maxDb (mag, fromHz, toHz);
    std::vector<double> found;
    for (int b = hzBin (fromHz); b <= hzBin (toHz); ++b)
    {
        if (mag[(size_t) b] > reference - depthDb)
            continue;
        bool lowest = true;
        const int reach = std::max (2, (int) (0.08 * b));
        for (int o = std::max (1, b - reach); o <= std::min (fftSize / 2 - 1, b + reach); ++o)
            lowest = lowest && (mag[(size_t) b] < mag[(size_t) o] || o == b || (mag[(size_t) b] == mag[(size_t) o] && o > b));
        if (lowest)
            found.push_back (binHz (b));
    }
    return found;
}

std::string list (const std::vector<double>& values)
{
    std::string out;
    for (double v : values)
        out += text (out.empty() ? "%.0f" : ", %.0f", v);
    return out.empty() ? "none" : out;
}

//==============================================================================
void testPatternTremolo()
{
    std::printf ("  Pattern Tremolo\n");
    // 2 steps per second: 3 pulses, Mute, Full, Skip -> a pattern of three steps (1.5 s)
    auto out = render (setup (V::patternTremolo, { { "Speed", 2.0 }, { "Step 1", 2 }, { "Step 2", 16 }, { "Step 3", 18 }, { "Step 4", 17 } }), 3.2, dc (0.25f));
    const auto gain = track (out.l, 4.0);
    auto pulses = [&] (double t0, double t1) { return (int) crossings (cut (gain, t0, t1), false, 0.5f).size(); };
    expect (pulses (0.0, 0.5) == 3, text ("step of 3 pulses chops the sound 3 times in its half second (%d)", pulses (0.0, 0.5)));
    expect (cut (gain, 0.53, 0.98).max() < 0.02f, text ("Mute step is silent (gain %.4f)", cut (gain, 0.53, 0.98).max()));
    expect (cut (gain, 1.03, 1.48).min() > 0.98f, text ("Full step passes the sound (gain %.4f)", cut (gain, 1.03, 1.48).min()));
    expect (pulses (1.5, 2.0) == 3 && cut (gain, 2.03, 2.48).max() < 0.02f, text ("Skip step is left out: the pattern restarts after 1.5 s (%d pulses)", pulses (1.5, 2.0)));
    expect (gain.max() < 1.001f && gain.min() > -0.001f, "gain stays between 0 and 1");

    out = render (setup (V::patternTremolo, { { "Speed", 1.0 }, { "Step 1", 15 }, { "Step 2", 0 }, { "Step 3", 18 }, { "Step 4", 18 } }), 2.0, dc (0.25f));
    const auto gain2 = track (out.l, 4.0);
    const int sixteen = (int) crossings (cut (gain2, 0.0, 1.0), false, 0.5f).size(), one = (int) crossings (cut (gain2, 1.0, 2.0), false, 0.5f).size();
    expect (sixteen == 16 && one == 1, text ("steps of 16 pulses and of 1 pulse (%d, %d)", sixteen, one));
    const auto edge = cut (gain2, 1.49, 1.52);
    float steepest = 0.0f;
    for (size_t i = 1; i < edge.v.size(); ++i)
        steepest = std::max (steepest, std::abs (edge.v[i] - edge.v[i - 1]));
    expect (steepest < 0.03f, text ("pulse edges are rounded (largest step per sample %.4f of full level)", steepest));

    out = render (setup (V::patternTremolo), 1.0, sine (440.0, 0.3), {}, true);
    expect (rms (out.r) == 0.0 && rms (out.l) > 0.05, "true stereo: a signal on the left only stays on the left");
}

void testPanner()
{
    std::printf ("  Panner\n");
    auto out = render (setup (V::panner, { { "Speed", 2.0 } }), 4.0, dc (0.25f));
    const auto gl = track (out.l, 4.0), gr = track (out.r, 4.0);
    expect (std::abs (cycleRate (gl) - 2.0) < 0.02, text ("pan rate follows Speed: %.3f Hz at 2 Hz", cycleRate (gl)));
    float worstPower = 0.0f;
    for (size_t i = 4800; i < gl.v.size(); ++i)
        worstPower = std::max (worstPower, std::abs (gl.v[i] * gl.v[i] + gr.v[i] * gr.v[i] - 2.0f));
    expect (worstPower < 0.01f, text ("constant power: L^2 + R^2 stays at 2 (off by %.4f at most)", worstPower));
    expect (gl.min() < 0.01f && gr.min() < 0.01f && gl.max() > 1.40f, text ("full depth pans hard left and right (gain %.3f .. %.3f)", gl.min(), gl.max()));

    out = render (setup (V::panner, { { "Speed", 2.0 }, { "Depth", 50 } }), 2.0, dc (0.25f));
    expect (track (out.l, 4.0).min() > 0.5f, text ("Depth 50 %% pans half way (lowest gain %.3f)", track (out.l, 4.0).min()));

    // wave shape: share of the time the pan position is near the middle (gain between 0.4 and 1.3)
    double share[3] {};
    for (int shape = 0; shape < 3; ++shape)
    {
        out = render (setup (V::panner, { { "Speed", 2.0 }, { "Shape", 50.0 * shape } }), 4.0, dc (0.25f));
        int count = 0;
        for (float v : out.l)
            count += (v * 4.0f > 0.4f && v * 4.0f < 1.3f) ? 1 : 0;
        share[shape] = (double) count / (double) out.l.size();
    }
    expect (share[0] > share[1] + 0.05 && share[1] > share[2] + 0.2 && share[2] < 0.1,
            text ("Shape: triangle, sine, square spend %.2f, %.2f, %.2f of the time near the centre", share[0], share[1], share[2]));

    const auto loud = render (setup (V::panner, { { "Speed", 1.0 }, { "VolSens", 100 } }), 6.0, dc (0.4f));
    const auto quiet = render (setup (V::panner, { { "Speed", 1.0 }, { "VolSens", 100 } }), 6.0, dc (0.004f));
    const double fast = cycleRate (cut (track (loud.l), 1.0, 6.0)), slow = cycleRate (cut (track (quiet.l), 1.0, 6.0));
    expect (fast > 3.8 && fast < 4.1 && slow < 1.1, text ("VolSens: %.2f Hz when played loud, %.2f Hz when quiet (Speed 1 Hz)", fast, slow));

    out = render (setup (V::panner, { { "Mix", 0 } }), 1.0, sine (330.0, 0.3));
    expect (identical (out.l, out.r) && std::abs (rms (out.l) - 0.3 / std::sqrt (2.0)) < 0.001, "Mix 0 % leaves the signal alone");
}

/** Fall and rise time of a tremolo's gain, between 10 % and 90 % of its swing (seconds, averaged). */
std::pair<double, double> edgeTimes (const Track& gain)
{
    const float lo = gain.min(), hi = gain.max(), low = lo + 0.1f * (hi - lo), high = lo + 0.9f * (hi - lo);
    const auto downHigh = crossings (gain, false, high), downLow = crossings (gain, false, low);
    const auto upLow = crossings (gain, true, low), upHigh = crossings (gain, true, high);
    auto average = [] (const std::vector<double>& starts, const std::vector<double>& ends)
    {
        double sum = 0.0;
        int count = 0;
        for (double s : starts)
        {
            const auto e = std::upper_bound (ends.begin(), ends.end(), s);
            if (e != ends.end())
            {
                sum += *e - s;
                ++count;
            }
        }
        return count > 0 ? sum / count : 0.0;
    };
    return { average (downHigh, downLow), average (upLow, upHigh) };
}

void testTremolos()
{
    std::printf ("  Bias Tremolo\n");
    auto out = render (setup (V::biasTremolo, { { "Speed", 4.0 }, { "Level", 100 } }), 3.0, dc (0.1f));
    auto gain = cut (track (out.l, 10.0), 0.5, 3.0);
    expect (std::abs (cycleRate (gain) - 4.0) < 0.04, text ("rate follows Speed: %.3f Hz at 4 Hz", cycleRate (gain)));
    expect (gain.max() > 0.97f && gain.max() < 1.03f && db (gain.min()) < -25.0, text ("Level 100 %%: gain swings from %.2f down to %.1f dB", gain.max(), db (gain.min())));
    auto edges = edgeTimes (gain);
    expect (edges.second / edges.first > 0.8 && edges.second / edges.first < 1.25, text ("smooth, even throb: fall %.1f ms, rise %.1f ms", 1000.0 * edges.first, 1000.0 * edges.second));
    const auto gainR = cut (track (out.r, 10.0), 0.5, 3.0);
    const double lag = 4.0 * (crossings (gain)[1] - crossings (gainR)[1]);
    expect (std::abs (lag - std::floor (lag) - 0.25) < 0.02, text ("stereo: the right side's tremolo is a quarter cycle ahead (%.2f cycles)", lag - std::floor (lag)));

    const auto small = render (setup (V::biasTremolo, { { "Speed", 4.0 }, { "Level", 60 } }), 2.0, dc (0.02f));
    const auto large = render (setup (V::biasTremolo, { { "Speed", 4.0 }, { "Level", 60 } }), 2.0, dc (0.5f));
    const double troughSmall = db (cut (track (small.l, 50.0), 0.5, 2.0).min()), troughLarge = db (cut (track (large.l, 2.0), 0.5, 2.0).min());
    expect (troughLarge > troughSmall + 1.0 && troughLarge < troughSmall + 6.0,
            text ("depends on the playing level: at Level 60 %% the trough is %.1f dB for a quiet note, %.1f dB for a loud one", troughSmall, troughLarge));

    const auto loudBias = render (setup (V::biasTremolo, { { "Speed", 1.0 }, { "VolSens", 100 } }), 6.0, dc (0.4f));
    const auto quietBias = render (setup (V::biasTremolo, { { "Speed", 1.0 }, { "VolSens", 100 } }), 6.0, dc (0.004f));
    const double fastBias = cycleRate (cut (track (loudBias.l), 1.0, 6.0)), slowBias = cycleRate (cut (track (quietBias.l), 1.0, 6.0));
    expect (fastBias > 3.8 && fastBias < 4.1 && slowBias < 1.1, text ("VolSens: %.2f Hz when played loud, %.2f Hz when quiet (Speed 1 Hz)", fastBias, slowBias));

    out = render (setup (V::biasTremolo, { { "Level", 0 } }), 1.0, dc (0.1f));
    expect (track (out.l, 10.0).max() - track (out.l, 10.0).min() < 0.001f, "Level 0 %: no tremolo");
    out = render (setup (V::biasTremolo, { { "Speed", 4.0 }, { "Level", 100 }, { "Shape", 100 } }), 3.0, dc (0.1f));
    const auto square = edgeTimes (cut (track (out.l, 10.0), 0.5, 3.0));
    expect (square.first < 0.4 * edges.first, text ("Shape 100 %%: square, edges of %.1f ms instead of %.1f ms", 1000.0 * square.first, 1000.0 * edges.first));

    std::printf ("  Opto Tremolo\n");
    out = render (setup (V::optoTremolo, { { "Speed", 4.0 }, { "Level", 100 }, { "Shape", 0 } }), 3.0, dc (0.1f));
    gain = cut (track (out.l, 10.0), 0.5, 3.0);
    const auto opto = edgeTimes (gain);
    expect (std::abs (cycleRate (gain) - 4.0) < 0.04, text ("rate follows Speed: %.3f Hz at 4 Hz", cycleRate (gain)));
    expect (opto.second > 2.5 * opto.first, text ("lopsided lamp and photocell: the volume falls in %.1f ms and recovers in %.1f ms", 1000.0 * opto.first, 1000.0 * opto.second));
    expect (db (gain.min()) < -20.0 && gain.max() > 0.9f, text ("Level 100 %%: gain %.2f down to %.1f dB", gain.max(), db (gain.min())));
    expect (identical (out.l, out.r), "one photocell for both sides");
    const auto fastOpto = render (setup (V::optoTremolo, { { "Speed", 10.0 }, { "Level", 100 }, { "Shape", 0 } }), 2.0, dc (0.1f));
    expect (cut (track (fastOpto.l, 10.0), 0.5, 2.0).max() < gain.max() - 0.1f, text ("the cell cannot keep up at 10 Hz: the gain only recovers to %.2f", cut (track (fastOpto.l, 10.0), 0.5, 2.0).max()));
    const auto throb = render (setup (V::optoTremolo, { { "Speed", 4.0 }, { "Level", 100 }, { "Shape", 100 } }), 3.0, dc (0.1f));
    const auto throbEdges = edgeTimes (cut (track (throb.l, 10.0), 0.5, 3.0));
    expect (throbEdges.first < 0.6 * opto.first, text ("Shape 100 %%: the lamp switches hard (fall %.1f ms)", 1000.0 * throbEdges.first));
    out = render (setup (V::optoTremolo), 1.0, sine (440.0, 0.3), {}, true);
    expect (rms (out.r) == 0.0, "stereo through: a signal on the left only stays on the left");

    const auto loud = render (setup (V::optoTremolo, { { "Speed", 1.0 }, { "VolSens", 50 } }), 6.0, dc (0.4f));
    expect (std::abs (cycleRate (cut (track (loud.l), 1.0, 6.0)) - 2.5) < 0.1, text ("VolSens 50 %%: %.2f Hz when played loud (Speed 1 Hz)", cycleRate (cut (track (loud.l), 1.0, 6.0))));
}

//==============================================================================
void testPhasers()
{
    std::printf ("  Script Phase\n");
    {
        // Speed 0.05 Hz: the sweep takes 10 s from the bottom to the top
        const auto mags = responses (setup (V::scriptPhase, { { "Speed", 0.05 } }), { 0.1, 2.5, 5.0, 7.5, 9.9 });
        bool two = true, ratio = true;
        std::vector<double> lowNotch;
        for (const auto& mag : mags)
        {
            const auto n = notches (mag, 40.0, 12000.0);
            two = two && n.size() == 2;
            if (n.size() == 2)
            {
                ratio = ratio && std::abs (n[1] / n[0] - 5.83) < 0.5;
                lowNotch.push_back (n[0]);
            }
        }
        expect (two && ratio, "four stages: two notches, 5.8 times apart in frequency");
        expect (lowNotch.size() == 5 && lowNotch.front() < 95.0 && lowNotch.back() > 650.0 && std::is_sorted (lowNotch.begin(), lowNotch.end()),
                text ("the notches sweep: the lower one at %s Hz during the upward half", list (lowNotch).c_str()));
        expect (maxDb (mags[2], 40.0, 12000.0) < 2.0 && maxDb (mags[2], 40.0, 12000.0) > 0.5, text ("peaks at %.1f dB (dry + effect 1:1, a little boost)", maxDb (mags[2], 40.0, 12000.0)));

        const auto out = render (setup (V::scriptPhase, { { "Speed", 2.0 } }), 4.0, sine (300.0, 0.3));
        const auto env = envelope (out.l, 160);
        const double period = repeatPeriod (env, 0.1, 0.8);
        expect (std::abs (period - 0.5) < 0.01, text ("sweep rate follows Speed: repeats every %.3f s at 2 Hz", period));
        const auto left = render (setup (V::scriptPhase), 1.0, sine (440.0, 0.3), {}, true);
        expect (rms (left.r) == 0.0, "true stereo: a signal on the left only stays on the left");
    }

    std::printf ("  Phaser\n");
    {
        std::string counts;
        bool ok = true;
        for (int stages = 0; stages < 4; ++stages)
        {
            const auto n = notches (response (setup (V::phaser, { { "Depth", 0 }, { "Fdbk", 0 }, { "Stages", stages } })), 30.0, 15000.0);
            ok = ok && (int) n.size() == 2 * (stages + 1);
            counts += text ("%s%d", stages ? ", " : "", (int) n.size());
        }
        expect (ok, "Stages 4 / 8 / 12 / 16 give " + counts + " notches");

        const auto plain = response (setup (V::phaser, { { "Depth", 0 }, { "Fdbk", 0 } })), resonant = response (setup (V::phaser, { { "Depth", 0 }, { "Fdbk", 100 } }));
        const double lift = maxDb (resonant, 300.0, 3000.0) - minDb (resonant, 100.0, 3000.0);
        expect (maxDb (resonant, 300.0, 3000.0) > maxDb (plain, 300.0, 3000.0) + 4.0 && notches (resonant, 40.0, 12000.0, 20.0).size() == 2,
                text ("Fdbk adds a resonant peak between the notches (%.1f dB against %.1f dB without; notch to peak %.0f dB)",
                      maxDb (resonant, 300.0, 3000.0), maxDb (plain, 300.0, 3000.0), lift));

        const auto moving = responses (setup (V::phaser, { { "Speed", 0.05 }, { "Depth", 100 }, { "Fdbk", 0 } }), { 0.1, 5.0, 15.0 });
        const auto n0 = notches (moving[0], 30.0, 15000.0), n1 = notches (moving[1], 30.0, 15000.0), n2 = notches (moving[2], 30.0, 15000.0);
        expect (n0.size() == 2 && n1.size() == 2 && n2.size() == 2 && n1[0] / n2[0] > 7.0 && n1[0] / n2[0] < 11.0,
                text ("Depth 100 %%: the lower notch sweeps between %.0f and %.0f Hz (3.2 octaves)", n2[0], n1[0]));
        const auto still = responses (setup (V::phaser, { { "Speed", 0.05 }, { "Depth", 0 }, { "Fdbk", 0 } }), { 0.1, 5.0 });
        expect (std::abs (notches (still[0], 30.0, 15000.0)[0] - notches (still[1], 30.0, 15000.0)[0]) < 3.0, "Depth 0 %: the notches stand still");

        const auto rightSide = notches (response (setup (V::phaser, { { "Speed", 0.05 }, { "Depth", 100 }, { "Fdbk", 0 } }), 0.1, true), 30.0, 15000.0);
        expect (rightSide.size() == 2 && rightSide[0] / n0[0] > 2.5, text ("stereo: the right side's sweep is a quarter cycle ahead (lower notch at %.0f Hz left, %.0f Hz right)", n0[0], rightSide[0]));

        const auto out = render (setup (V::phaser, { { "Speed", 2.0 }, { "Depth", 100 } }), 4.0, sine (300.0, 0.3));
        const double period = repeatPeriod (envelope (out.l, 160), 0.1, 0.8);
        expect (std::abs (period - 0.5) < 0.01, text ("sweep rate follows Speed: repeats every %.3f s at 2 Hz", period));
    }

    std::printf ("  Panned Phaser\n");
    {
        const auto left = setup (V::pannedPhaser, { { "Speed", 0.05 }, { "Pan", 0 }, { "Pan Spd", 0 } });
        const auto magL = response (left), magR = response (left, 0.1, true);
        expect (notches (magL, 40.0, 15000.0).size() == 2, text ("four stages: two notches (%s Hz)", list (notches (magL, 40.0, 15000.0)).c_str()));
        expect (maxDb (magR, 40.0, 15000.0) - minDb (magR, 40.0, 15000.0) < 0.1, "Pan = Left, Pan Spd 0: the phasing sits on the left, the right side is dry");
        const auto centre = render (setup (V::pannedPhaser, { { "Pan", 1 }, { "Pan Spd", 0 } }), 1.0, sine (500.0, 0.3));
        expect (identical (centre.l, centre.r), "Pan = Center, Pan Spd 0: both sides alike");

        auto out = render (setup (V::pannedPhaser, { { "Depth", 0 }, { "Pan", 1 }, { "Pan Spd", 2.0 }, { "Mix", 100 } }), 4.0, sine (1000.0, 0.3));
        auto envL = cut (envelope (out.l, 96), 0.5, 4.0), envR = cut (envelope (out.r, 96), 0.5, 4.0);
        expect (std::abs (cycleRate (envL) - 2.0) < 0.03, text ("the panner's rate follows Pan Spd: %.3f Hz at 2 Hz", cycleRate (envL)));
        expect (envL.min() < 0.02f * envL.max() && envR.min() < 0.02f * envR.max(), "Pan = Center: the effect travels fully left and fully right");
        out = render (setup (V::pannedPhaser, { { "Depth", 0 }, { "Pan", 2 }, { "Pan Spd", 2.0 }, { "Mix", 100 } }), 4.0, sine (1000.0, 0.3));
        envL = cut (envelope (out.l, 96), 0.5, 4.0);
        envR = cut (envelope (out.r, 96), 0.5, 4.0);
        expect (envR.min() > 0.97f * envR.max() && envL.min() < 0.02f * envL.max() && envL.max() > 0.97f * envR.max(),
                text ("Pan = Right: the effect moves between the centre and the right (left %.3f .. %.3f, right %.3f .. %.3f)", envL.min(), envL.max(), envR.min(), envR.max()));
    }

    std::printf ("  Dual Phaser\n");
    {
        // a quarter of the way into the cycle phasor A is at the top of its sweep and B at the bottom
        const auto s = setup (V::dualPhaser, { { "Speed", 0.05 }, { "Depth", 100 }, { "Fdbk", 0 } });
        const auto a = notches (response (s, 5.0), 20.0, 16000.0), b = notches (response (s, 5.0, true), 20.0, 16000.0);
        expect (a.size() == 3 && b.size() == 3, text ("six stages per phasor: three notches each (left %s Hz, right %s Hz)", list (a).c_str(), list (b).c_str()));
        expect (a.size() == 3 && b.size() == 3 && a[1] / b[1] > 8.0 && a[1] / b[1] < 13.0, "the two sweeps run against each other (3.4 octaves apart at the extremes)");
        const auto later = notches (response (s, 15.0), 20.0, 16000.0);
        expect (later.size() == 3 && b.size() == 3 && std::abs (later[1] / b[1] - 1.0) < 0.1, "half a cycle later they have swapped places");

        // LFO Shp: sine glides, square jumps between two positions
        double dwell[2] {};
        for (int shape = 0; shape < 2; ++shape)
        {
            std::vector<double> times;
            for (int i = 0; i < 40; ++i)
                times.push_back (1.0 + 0.05 * i);
            const auto mags = responses (setup (V::dualPhaser, { { "Speed", 1.0 }, { "Depth", 100 }, { "Fdbk", 0 }, { "LFO Shp", 100.0 * shape } }), times, false, 2048);
            int atEnds = 0;
            for (const auto& mag : mags)
            {
                const auto n = notches (mag, 20.0, 16000.0);
                const double middle = n.size() == 3 ? n[1] : 0.0;
                atEnds += (middle > 0.0 && (middle < 129.0 * 1.2 || middle > 1365.0 / 1.2)) ? 1 : 0;
            }
            dwell[shape] = atEnds / 40.0;
        }
        expect (dwell[0] < 0.5 && dwell[1] > 0.8, text ("LFO Shp: the sweep is at one of its two ends %.0f %% of the time as a sine, %.0f %% as a square", 100.0 * dwell[0], 100.0 * dwell[1]));

        const auto plain = response (setup (V::dualPhaser, { { "Speed", 0.05 }, { "Depth", 0 }, { "Fdbk", 0 } }));
        const auto resonant = response (setup (V::dualPhaser, { { "Speed", 0.05 }, { "Depth", 0 }, { "Fdbk", 100 } }));
        expect (maxDb (resonant, 150.0, 3000.0) > maxDb (plain, 150.0, 3000.0) + 4.0, text ("Fdbk makes the peaks resonant (%.1f dB against %.1f dB)", maxDb (resonant, 150.0, 3000.0), maxDb (plain, 150.0, 3000.0)));
        const auto out = render (setup (V::dualPhaser, { { "Speed", 2.0 } }), 4.0, sine (200.0, 0.3));
        const double period = repeatPeriod (envelope (out.l, 240), 0.1, 0.8);
        expect (std::abs (period - 0.5) < 0.012, text ("sweep rate follows Speed: repeats every %.3f s at 2 Hz", period));
    }

    std::printf ("  U-Vibe\n");
    {
        // lamp bright after 5 s, dark after 15 s
        const auto mags = responses (setup (V::uVibe, { { "Speed", 0.05 }, { "Depth", 100 } }), { 0.2, 5.0, 15.0 });
        const auto start = notches (mags[0], 20.0, 20000.0, 10.0), bright = notches (mags[1], 20.0, 20000.0, 10.0), dark = notches (mags[2], 20.0, 20000.0, 10.0);
        const auto script = notches (response (setup (V::scriptPhase, { { "Speed", 0.05 } }), 5.0), 20.0, 20000.0, 10.0);
        expect (bright.size() == 2 && bright[1] / bright[0] > 30.0 && dark.size() == 1 && start.size() == 2 && start[1] / start[0] > 30.0 && script.size() == 2 && script[1] / script[0] < 6.5,
                "staggered stages: one dip in the bass and one far above it (lamp bright: " + list (bright) + " Hz; half way: " + list (start) + " Hz; dark: " + list (dark)
                + " Hz) where the Script Phase has a pair 5.8 times apart (" + list (script) + " Hz)");

        const auto vibrato = response (setup (V::uVibe, { { "Speed", 0.05 }, { "Depth", 100 }, { "Mix", 100 } }), 5.0);
        expect (maxDb (vibrato, 60.0, 15000.0) - minDb (vibrato, 60.0, 15000.0) < 0.5, text ("Mix 100 %% = vibrato: no dips left (response flat within %.2f dB)", maxDb (vibrato, 60.0, 15000.0) - minDb (vibrato, 60.0, 15000.0)));
        auto out = render (setup (V::uVibe, { { "Speed", 5.0 }, { "Depth", 100 }, { "Mix", 100 } }), 3.0, sine (1000.0, 0.3));
        auto pitch = cut (frequencyTrack (out.l, 8), 1.0, 3.0);
        expect (pitch.max() - pitch.min() > 2.0f && std::abs (cycleRate (pitch) - 5.0) < 0.1,
                text ("... and the pitch wobbles at the Speed (%.2f Hz, 1 kHz moves %.1f .. %.1f Hz)", cycleRate (pitch), pitch.min(), pitch.max()));

        // lamp: the sweep lingers at the dark (low) end and rises faster than it falls
        std::vector<double> times;
        for (int i = 0; i < 40; ++i)
            times.push_back (1.0 + 0.05 * i);
        const auto sweep = responses (setup (V::uVibe, { { "Speed", 0.5 }, { "Depth", 60 } }), times, false, 2048);
        std::vector<double> dips;
        for (const auto& mag : sweep)
        {
            const auto n = notches (mag, 150.0, 20000.0, 8.0);
            if (n.size() == 1)
                dips.push_back (std::log (n[0]));
        }
        const double lowest = *std::min_element (dips.begin(), dips.end()), highest = *std::max_element (dips.begin(), dips.end());
        int below = 0;
        for (double d : dips)
            below += d < 0.5 * (lowest + highest) ? 1 : 0;
        expect (dips.size() >= 36 && (double) below / (double) dips.size() > 0.56,
                text ("lamp-shaped sweep: the dip moves %.0f .. %.0f Hz and spends %.0f %% of the time in the lower half", std::exp (lowest), std::exp (highest), 100.0 * below / (double) dips.size()));

        const auto loud = render (setup (V::uVibe, { { "Speed", 1.0 }, { "VolSens", 100 }, { "Mix", 100 } }), 6.0, sine (1000.0, 0.4));
        const auto quiet = render (setup (V::uVibe, { { "Speed", 1.0 }, { "VolSens", 100 }, { "Mix", 100 } }), 6.0, sine (1000.0, 0.004));
        const double fast = cycleRate (cut (frequencyTrack (loud.l, 8), 1.0, 6.0)), slow = cycleRate (cut (frequencyTrack (quiet.l, 8), 1.0, 6.0));
        expect (fast > 3.7 && fast < 4.1 && slow < 1.1, text ("VolSens: %.2f Hz when played loud, %.2f Hz when quiet (Speed 1 Hz)", fast, slow));
        out = render (setup (V::uVibe), 1.0, sine (500.0, 0.3));
        expect (identical (out.l, out.r), "one effect for both sides");
    }

    std::printf ("  Barberpole Phaser\n");
    {
        // the time at which the notch passes a 500 Hz and a 600 Hz tone
        auto passes = [] (int mode, bool right, double hz)
        {
            const auto out = render (setup (V::barberpolePhaser, { { "Speed", 1.0 }, { "Fdbk", 0 }, { "Mode", mode } }), 4.0, sine (hz, 0.3));
            const auto env = cut (envelope (right ? out.r : out.l, 480), 1.0, 4.0);
            const auto falls = crossings (env, false), rises = crossings (env, true);
            const auto next = std::upper_bound (rises.begin(), rises.end(), falls[0]);
            return std::make_tuple (0.5 * (falls[0] + *next), cycleRate (env), (double) (env.min() / env.max()));
        };
        const auto [t500, rate500, depth500] = passes (0, false, 500.0);
        const auto [t600, rate600, depth600] = passes (0, false, 600.0);
        auto later = [] (double a, double b) { const double d = b - a - std::floor (b - a); return d; };  // b after a, in cycles of 1 s
        expect (std::abs (rate500 - 1.0) < 0.01 && std::abs (rate600 - 1.0) < 0.01, text ("a notch passes every tone once per 1 / Speed (%.3f per second at 1 Hz)", rate500));
        expect (depth500 < 0.1 && depth600 < 0.1, text ("the notches are deep (a tone drops to %.3f of its level)", depth500));
        expect (later (t500, t600) > 0.02 && later (t500, t600) < 0.45, text ("Up: the notch reaches 600 Hz %.0f ms after 500 Hz, every cycle - an endless rise", 1000.0 * later (t500, t600)));
        const double down500 = std::get<0> (passes (1, false, 500.0)), down600 = std::get<0> (passes (1, false, 600.0));
        expect (later (down600, down500) > 0.02 && later (down600, down500) < 0.45, text ("Down: it reaches 500 Hz %.0f ms after 600 Hz", 1000.0 * later (down600, down500)));
        const double sl500 = std::get<0> (passes (2, false, 500.0)), sl600 = std::get<0> (passes (2, false, 600.0));
        const double sr500 = std::get<0> (passes (2, true, 500.0)), sr600 = std::get<0> (passes (2, true, 600.0));
        expect (later (sl500, sl600) < 0.45 && later (sr600, sr500) < 0.45 && later (sl500, sl600) > 0.02 && later (sr600, sr500) > 0.02, "Stereo: up on the left, down on the right");

        const auto slow = render (setup (V::barberpolePhaser, { { "Speed", 0.25 }, { "Fdbk", 0 } }), 10.0, sine (500.0, 0.3));
        expect (std::abs (cycleRate (cut (envelope (slow.l, 480), 1.0, 10.0)) - 0.25) < 0.005, text ("... %.3f per second at 0.25 Hz", cycleRate (cut (envelope (slow.l, 480), 1.0, 10.0))));

        // a snapshot of the spectrum: several notches at once, and resonant peaks with feedback
        const auto n = notches (response (setup (V::barberpolePhaser, { { "Speed", 0.05 }, { "Fdbk", 0 } }), 3.0), 60.0, 12000.0);
        expect (n.size() >= 4 && n.size() <= 7, text ("%d notches between 60 Hz and 12 kHz at any moment (%s Hz)", (int) n.size(), list (n).c_str()));
        const auto plain = response (setup (V::barberpolePhaser, { { "Speed", 0.05 }, { "Fdbk", 0 } }), 3.0);
        const auto resonant = response (setup (V::barberpolePhaser, { { "Speed", 0.05 }, { "Fdbk", 100 } }), 3.0);
        expect (maxDb (resonant, 150.0, 5000.0) > maxDb (plain, 150.0, 5000.0) + 3.0, text ("Fdbk: resonant peaks (%.1f dB against %.1f dB)", maxDb (resonant, 150.0, 5000.0), maxDb (plain, 150.0, 5000.0)));
        const auto left = render (setup (V::barberpolePhaser), 1.0, sine (440.0, 0.3), {}, true);
        expect (rms (left.r) == 0.0, "true stereo: a signal on the left only stays on the left");
    }
}

//==============================================================================
void testVibratoAndChorus()
{
    std::printf ("  Pitch Vibrato\n");
    {
        // the line's delay swings +-1.6 ms at Depth 100 %: a 1 kHz tone moves by 2 pi * Speed * 1.6 ms * 1 kHz
        auto deviation = [] (double speed, double depth, double rise, double t0, double t1, double* rate = nullptr)
        {
            const auto out = render (setup (V::pitchVibrato, { { "Speed", speed }, { "Depth", depth }, { "Rise", rise } }), t1, sine (1000.0, 0.3));
            const auto pitch = cut (frequencyTrack (out.l, 4), t0, t1);
            if (rate != nullptr)
                *rate = cycleRate (pitch);
            return 0.5 * (double) (pitch.max() - pitch.min());
        };
        double rate = 0.0;
        const double full = deviation (5.0, 100, 100, 1.0, 3.0, &rate), half = deviation (5.0, 50, 100, 1.0, 3.0), slow = deviation (2.5, 100, 100, 1.0, 3.0);
        expect (std::abs (rate - 5.0) < 0.05, text ("vibrato rate follows Speed: %.3f Hz at 5 Hz", rate));
        expect (std::abs (full / 50.27 - 1.0) < 0.06, text ("Depth 100 %% at 5 Hz: 1 kHz swings +-%.1f Hz (%.0f cents; +-1.6 ms of delay gives 50.3 Hz)", full, 1200.0 * std::log2 (1.0 + full / 1000.0)));
        expect (std::abs (half / full - 0.5) < 0.04 && std::abs (slow / full - 0.5) < 0.04, text ("half the Depth or half the Speed halves it (+-%.1f Hz, +-%.1f Hz), as with a bucket-brigade clock", half, slow));

        const double early = deviation (5.0, 100, 0, 0.2, 0.7), late = deviation (5.0, 100, 0, 3.6, 4.6), instant = deviation (5.0, 100, 100, 0.2, 0.7);
        expect (early < 0.2 * late && std::abs (late / full - 1.0) < 0.06 && instant > 0.9 * full,
                text ("Rise 0 %%: +-%.1f Hz in the first half second, +-%.1f Hz after 3.6 s; Rise 100 %%: +-%.1f Hz at once", early, late, instant));

        const auto out = render (setup (V::pitchVibrato, { { "Depth", 100 } }), 2.0, sine (1000.0, 0.3));
        const auto env = cut (envelope (out.l, 480), 1.0, 2.0);
        expect (identical (out.l, out.r) && env.min() > 0.9f * env.max(), "Mix 100 %: vibrato only, no dry signal beating against it; one effect for both sides");
        auto sensed = [] (double amplitude)
        {
            const auto o = render (setup (V::pitchVibrato, { { "Speed", 1.0 }, { "Depth", 100 }, { "Rise", 100 }, { "VolSens", 100 } }), 6.0, sine (1000.0, amplitude));
            return cycleRate (cut (frequencyTrack (o.l, 8), 1.0, 6.0));
        };
        expect (sensed (0.4) > 3.7 && sensed (0.4) < 4.1 && sensed (0.004) < 1.1, text ("VolSens: %.2f Hz when played loud, %.2f Hz when quiet (Speed 1 Hz)", sensed (0.4), sensed (0.004)));
        const auto top = response (setup (V::pitchVibrato, { { "Depth", 0 } }));
        expect (maxDb (top, 9000.0, 9100.0) < maxDb (top, 300.0, 400.0) - 8.0, text ("bucket-brigade treble loss: %.1f dB at 9 kHz", maxDb (top, 9000.0, 9100.0) - maxDb (top, 300.0, 400.0)));
    }

    std::printf ("  Dimension\n");
    {
        const char* names[] = { "Sw1", "Sw2", "Sw3", "Sw4" };
        double swing[4] {}, rates[4] {};
        bool triangle = true, opposite = true;
        for (int mode = 0; mode < 4; ++mode)
        {
            const auto s = setup (V::dimension, { { "Sw3", 0 }, { "Mix", 100 }, { names[mode], 1 } });
            const auto out = render (s, 14.0, impulses (960, 0.5f));
            const auto a = cut (delayTrack (out.l, 960, 5.0, 11.0), 1.0, 13.0), b = cut (delayTrack (out.r, 960, 5.0, 11.0), 1.0, 13.0);
            swing[mode] = 0.5 * (a.max() - a.min());
            rates[mode] = cycleRate (a);
            double acc = 0.0;
            for (float v : a.v)
                acc += (double) (v - a.mean()) * (v - a.mean());
            triangle = triangle && std::abs (std::sqrt (acc / (double) a.v.size()) / swing[mode] - 0.577) < 0.03;
            for (size_t i = 0; i < a.v.size(); ++i)
                opposite = opposite && std::abs (a.v[i] + b.v[i] - 2.0f * a.mean()) < 0.03f;
        }
        expect (swing[0] > 0.3 && swing[0] < swing[1] && swing[1] < swing[2] && swing[2] < swing[3] && swing[3] < 1.6,
                text ("buttons 1 to 4 modulate more and more: +-%.2f, %.2f, %.2f, %.2f ms around 8 ms", swing[0], swing[1], swing[2], swing[3]));
        expect (std::abs (rates[0] - 0.25) < 0.01 && std::abs (rates[2] - 0.25) < 0.01 && std::abs (rates[3] - 0.5) < 0.01,
                text ("slow LFO: %.3f Hz, %.3f Hz with button 4", rates[0], rates[3]));
        expect (triangle && opposite, "triangle LFO; the two lines move in opposite directions");

        auto out = render (setup (V::dimension, { { "Sw1", 1 }, { "Sw2", 1 }, { "Sw3", 1 }, { "Sw4", 1 }, { "Mix", 100 } }), 6.0, impulses (960, 0.5f));
        const auto all = cut (delayTrack (out.l, 960, 5.0, 11.0), 1.0, 6.0);
        expect (0.5 * (all.max() - all.min()) > swing[3] + 0.2, text ("the buttons combine: all four give +-%.2f ms", 0.5 * (all.max() - all.min())));
        const auto other = cut (delayTrack (out.l, 960, 5.0, 11.0, false), 1.0, 6.0), rightLine = cut (delayTrack (out.r, 960, 5.0, 11.0), 1.0, 6.0);
        bool cross = true;
        for (size_t i = 0; i < other.v.size(); ++i)
            cross = cross && std::abs (other.v[i] - rightLine.v[i]) < 0.06f;
        expect (cross, "each side also gets the other side's line, inverted");

        out = render (setup (V::dimension, { { "Sw3", 0 } }), 1.0, sine (440.0, 0.3));
        const auto in = render (setup (V::dimension, { { "Sw3", 0 }, { "Mix", 0 } }), 1.0, sine (440.0, 0.3));
        expect (identical (out.l, in.l) && identical (out.r, in.r), "no button pressed: dry");
        const auto low = response (setup (V::dimension, { { "Mix", 100 } }));
        expect (maxDb (low, 50.0, 60.0) < maxDb (low, 1000.0, 1100.0) - 8.0, text ("the lines leave the bass alone (%.1f dB at 55 Hz)", maxDb (low, 50.0, 60.0) - maxDb (low, 1000.0, 1100.0)));
    }

    std::printf ("  Analog Chorus\n");
    {
        auto shape = [] (const Track& d) { double acc = 0.0; for (float v : d.v) acc += (double) (v - d.mean()) * (v - d.mean()); return std::sqrt (acc / (double) d.v.size()) / (0.5 * (d.max() - d.min())); };
        auto out = render (setup (V::analogChorus, { { "Speed", 0.5 }, { "Depth", 100 }, { "Mix", 100 } }), 9.0, impulses (960, 0.5f));
        const auto chorus = cut (delayTrack (out.l, 960, 2.0, 12.0), 1.0, 9.0);
        expect (std::abs (cycleRate (chorus) - 0.5) < 0.01 && std::abs (shape (chorus) - 0.577) < 0.03,
                text ("chorus: triangle LFO at the Speed (%.3f Hz at 0.5 Hz), delay %.1f .. %.1f ms", cycleRate (chorus), chorus.min(), chorus.max()));
        out = render (setup (V::analogChorus), 1.0, sine (440.0, 0.3));
        const auto in = render (setup (V::analogChorus, { { "Mix", 0 } }), 1.0, sine (440.0, 0.3));
        expect (identical (out.r, in.r) && ! identical (out.l, in.l), "chorus: the effect is on the left output only, the right output carries the untouched signal");

        out = render (setup (V::analogChorus, { { "Speed", 4.0 }, { "Depth", 100 }, { "Ch Vib", 1 } }), 4.0, impulses (480, 0.5f));
        const auto vibrato = cut (delayTrack (out.l, 480, 1.0, 9.0), 1.0, 4.0);
        float dryPart = 0.0f;
        for (size_t i = 48000; i < out.l.size(); i += 480)
            dryPart = std::max (dryPart, std::abs (out.l[i]));
        expect (std::abs (cycleRate (vibrato) - 4.0) < 0.05 && std::abs (shape (vibrato) - 0.707) < 0.03,
                text ("vibrato: sine LFO (%.3f Hz at 4 Hz), delay %.1f .. %.1f ms", cycleRate (vibrato), vibrato.min(), vibrato.max()));
        expect (dryPart < 0.001f && identical (out.l, out.r), "vibrato: no dry signal, both sides alike");

        out = render (setup (V::analogChorus, { { "Speed", 0.6 }, { "Depth", 50 }, { "Mix", 100 } }), 5.0, sine (1000.0, 0.3));
        const auto pitch = cut (frequencyTrack (out.l, 16), 1.0, 5.0);
        expect (std::abs (0.5 * (pitch.max() - pitch.min()) / 3.84 - 1.0) < 0.12,
                text ("chorus detune at the default Depth: +-%.2f Hz at 1 kHz (%.1f cents)", 0.5 * (pitch.max() - pitch.min()), 1200.0 * std::log2 (1.0 + 0.5 * (pitch.max() - pitch.min()) / 1000.0)));

        const auto dark = response (setup (V::analogChorus, { { "Depth", 0 }, { "Tone", 0 }, { "Mix", 100 } }));
        const auto brightest = response (setup (V::analogChorus, { { "Depth", 0 }, { "Tone", 100 }, { "Mix", 100 } }));
        const double at6k[] = { maxDb (dark, 6000.0, 6100.0) - maxDb (dark, 200.0, 300.0), maxDb (brightest, 6000.0, 6100.0) - maxDb (brightest, 200.0, 300.0) };
        expect (at6k[0] < at6k[1] - 8.0 && maxDb (brightest, 12000.0, 12100.0) < maxDb (brightest, 200.0, 300.0) - 8.0,
                text ("Tone: %.1f .. %.1f dB at 6 kHz; band-limited line: %.1f dB at 12 kHz even at full Tone", at6k[0], at6k[1], maxDb (brightest, 12000.0, 12100.0) - maxDb (brightest, 200.0, 300.0)));
    }

    std::printf ("  Tri Chorus\n");
    {
        auto out = render (setup (V::triChorus, { { "Speed", 0.5 }, { "Depth", 30 }, { "Depth2", 30 }, { "Depth3", 30 }, { "Mix", 100 } }), 9.0, impulses (960, 0.5f));
        const Track lines[] = { cut (delayTrack (out.l, 960, 5.2, 6.8), 1.0, 9.0), cut (delayTrack (out.l, 960, 7.2, 8.8), 1.0, 9.0), cut (delayTrack (out.r, 960, 9.7, 11.3), 1.0, 9.0) };
        const auto c1 = component (lines[0], 0.5), c2 = component (lines[1], 0.5), c3 = component (lines[2], 0.5);
        auto apart = [] (double a, double b) { const double d = b - a - std::floor (b - a); return std::min (d, 1.0 - d); };
        expect (std::abs (lines[0].mean() - 6.0) < 0.15 && std::abs (lines[1].mean() - 8.0) < 0.15 && std::abs (lines[2].mean() - 10.5) < 0.15,
                text ("three delay lines: %.2f ms (left), %.2f ms (both sides), %.2f ms (right)", lines[0].mean(), lines[1].mean(), lines[2].mean()));
        expect (std::abs (apart (c2.second, c1.second) - 1.0 / 3.0) < 0.03 && std::abs (apart (c3.second, c2.second) - 1.0 / 3.0) < 0.03,
                text ("their LFOs are a third of a cycle apart (%.0f and %.0f degrees), at the Speed", 360.0 * apart (c2.second, c1.second), 360.0 * apart (c3.second, c2.second)));
        expect (std::abs (c1.first / 0.66 - 1.0) < 0.06 && std::abs (c2.first / 0.66 - 1.0) < 0.06 && std::abs (c3.first / 0.66 - 1.0) < 0.06,
                text ("main LFO: +-%.2f ms at Depth 30 %%", c1.first));
        const auto shimmer = component (lines[0], 2.65);
        expect (std::abs (shimmer.first / 0.036 - 1.0) < 0.25, text ("a second, faster LFO on every line (%.3f ms at 2.65 Hz)", shimmer.first));

        out = render (setup (V::triChorus, { { "Speed", 0.5 }, { "Depth", 30 }, { "Depth2", 0 }, { "Depth3", 30 }, { "Mix", 100 } }), 5.0, impulses (960, 0.5f));
        const auto still = cut (delayTrack (out.l, 960, 7.2, 8.8), 1.0, 5.0), moving = cut (delayTrack (out.l, 960, 5.2, 6.8), 1.0, 5.0);
        expect (still.max() - still.min() < 0.01f && moving.max() - moving.min() > 1.0f, "Depth, Depth2 and Depth3 set the three lines' modulation separately");
        float steepest = 0.0f;
        for (size_t i = 1; i < lines[0].v.size(); ++i)
            steepest = std::max (steepest, std::abs (lines[0].v[i] - lines[0].v[i - 1]) / 20.0f);  // ms of delay per ms
        expect (steepest > 0.002f && steepest < 0.0035f, text ("detune of a line at Depth 30 %%: up to %.1f cents", 1200.0 * std::log2 (1.0 + steepest)));
    }
}

//==============================================================================
void testFlangers()
{
    struct Flanger { int variant; const char* name; const char* depth; const char* feedback; bool hasMix; double shortest, longest, middle; };
    const Flanger flangers[] = {
        { V::analogFlanger, "Analog Flanger", "Depth", "Fdbk",    true,  0.5,  10.0,  0.952 },
        { V::jetFlanger,    "Jet Flanger",    "Depth", "Fdbk",    true,  0.35, 12.25, 2.071 },
        { V::acFlanger,     "AC Flanger",     "Width", "Regen",   false, 0.5,  10.0,  0.952 },
        { V::flanger80A,    "80A Flanger",    "Range", "Enhance", false, 0.35, 12.25, 2.071 },
    };

    for (const auto& f : flangers)
    {
        std::printf ("  %s\n", f.name);
        auto make = [&] (double speed, double depth, double feedback, double manual, double mix = 100.0)
        {
            auto s = setup (f.variant, { { "Speed", speed }, { f.depth, depth }, { f.feedback, feedback }, { "Manual", manual } });
            if (f.hasMix)
                s.knobs[4] = (float) mix;
            return s;
        };

        // the delay over time, from an impulse every 30 ms
        auto out = render (make (0.5, 100, 0, 50), 9.0, impulses (1440, 0.5f));
        const auto delay = cut (delayTrack (out.l, 1440, 0.2, 14.0), 1.0, 9.0);
        expect (std::abs (delay.min() / f.shortest - 1.0) < 0.12 && std::abs (delay.max() / f.longest - 1.0) < 0.05,
                text ("full sweep: %.2f .. %.2f ms (%.0f : 1)", delay.min(), delay.max(), delay.max() / delay.min()));
        expect (std::abs (delay.median() / f.middle - 1.0) < 0.1,
                text ("sweep law: half the time below %.2f ms (%s)", delay.median(), f.middle < 1.0 ? "clock linear in the LFO: notches move evenly in Hz" : "exponential: notches move evenly in octaves"));
        Track clock = delay;
        for (auto& v : clock.v)
            v = std::log (v);
        expect (std::abs (cycleRate (clock) - 0.5) < 0.01, text ("sweep rate follows Speed: %.3f Hz at 0.5 Hz", cycleRate (clock)));

        const auto longest = cut (delayTrack (render (make (0.5, 0, 0, 0), 2.0, impulses (1440, 0.5f)).l, 1440, 0.2, 14.0), 1.0, 2.0);
        const auto shortest = cut (delayTrack (render (make (0.5, 0, 0, 100), 2.0, impulses (1440, 0.5f)).l, 1440, 0.2, 14.0), 1.0, 2.0);
        expect (longest.max() - longest.min() < 0.01f && std::abs (longest.mean() / f.longest - 1.0) < 0.05 && std::abs (shortest.mean() / f.shortest - 1.0) < 0.12,
                text ("%s 0 %%: no sweep, Manual sets the delay (%.2f ms .. %.2f ms)", f.depth, longest.mean(), shortest.mean()));

        // regeneration: the comb's peaks grow
        const auto plain = response (make (0.5, 0, 0, 50, 50)), resonant = response (make (0.5, 0, 100, 50, 50));
        expect (maxDb (resonant, 200.0, 3000.0) > maxDb (plain, 200.0, 3000.0) + 5.0 && maxDb (plain, 200.0, 3000.0) - minDb (plain, 200.0, 3000.0) > 18.0,
                text ("comb filter with %.0f dB deep notches; %s raises its peaks from %.1f to %.1f dB", maxDb (plain, 200.0, 3000.0) - minDb (plain, 200.0, 3000.0), f.feedback,
                      maxDb (plain, 200.0, 3000.0), maxDb (resonant, 200.0, 3000.0)));
        const double top = maxDb (plain, 9000.0, 11000.0) - minDb (plain, 9000.0, 11000.0);
        std::printf ("         (notch depth around 10 kHz: %.1f dB - the line's filters)\n", top);

        out = render (make (0.5, 100, 0, 50), 4.0, impulses (1440, 0.5f));
        if (f.variant == V::flanger80A)
        {
            expect (identical (out.l, out.r), "mono: both sides alike");
            const auto left = render (setup (f.variant), 1.0, sine (440.0, 0.3), {}, true);
            expect (identical (left.l, left.r) && rms (left.r) > 0.02, "mono: a signal on the left only comes out on both sides");
        }
        else
        {
            const auto dl = cut (delayTrack (out.l, 1440, 0.2, 14.0), 1.0, 4.0), dr = cut (delayTrack (out.r, 1440, 0.2, 14.0), 1.0, 4.0);
            double apart = 0.0;
            for (size_t i = 0; i < dl.v.size(); ++i)
                apart += std::abs (std::log (dl.v[i] / dr.v[i])) / (double) dl.v.size();
            expect (apart > 0.3, text ("stereo: the right side's line sweeps out of step (a factor of %.1f apart on average)", std::exp (apart)));
        }
    }

    std::printf ("  flanger voicings\n");
    {
        // the compressor in front of the A/DA's line: a quiet note is delayed at a higher level than a loud one
        auto wetLevel = [] (int variant, double amplitude)
        {
            auto s = setup (variant, { { "Speed", 0.5 }, { variant == V::flanger80A ? "Range" : "Depth", 0 }, { variant == V::flanger80A ? "Enhance" : "Fdbk", 0 } });
            // dry + wet at a frequency where the comb has a peak: both add
            const auto out = render (s, 2.0, sine (2.0 * 482.9, amplitude));
            return rms (out.l, 1.0, 2.0) / (amplitude / std::sqrt (2.0));
        };
        expect (db (wetLevel (V::jetFlanger, 0.01) / wetLevel (V::jetFlanger, 0.4)) > 1.5 && db (wetLevel (V::flanger80A, 0.01) / wetLevel (V::flanger80A, 0.4)) > 1.5,
                text ("Jet and 80A: compressor in front of the line (quiet notes come out %.1f dB and %.1f dB stronger)",
                      db (wetLevel (V::jetFlanger, 0.01) / wetLevel (V::jetFlanger, 0.4)), db (wetLevel (V::flanger80A, 0.01) / wetLevel (V::flanger80A, 0.4))));

        // Even / Odd: with the line at 2.07 ms the even harmonics are at multiples of 483 Hz
        const auto even = response (setup (V::flanger80A, { { "Range", 0 }, { "Manual", 50 }, { "Enhance", 80 }, { "Even Odd", 0 } }));
        const auto odd = response (setup (V::flanger80A, { { "Range", 0 }, { "Manual", 50 }, { "Enhance", 80 }, { "Even Odd", 1 } }));
        const double evenAtEven = maxDb (even, 440.0, 500.0), evenAtOdd = maxDb (even, 690.0, 740.0), oddAtEven = maxDb (odd, 440.0, 500.0), oddAtOdd = maxDb (odd, 680.0, 760.0);
        expect (evenAtEven > evenAtOdd + 10.0 && oddAtOdd > oddAtEven + 3.0,
                text ("80A Even: peak at 483 Hz (%.1f dB; %.1f dB at 724 Hz). Odd: peak at 724 Hz (%.1f dB; %.1f dB at 483 Hz)", evenAtEven, evenAtOdd, oddAtOdd, oddAtEven));

        const auto analog = response (setup (V::analogFlanger, { { "Depth", 0 }, { "Fdbk", 0 }, { "Mix", 100 } }));
        const auto ac = response (setup (V::acFlanger, { { "Width", 0 }, { "Regen", 0 } }));
        const double analogTop = maxDb (analog, 8000.0, 8100.0) - maxDb (analog, 300.0, 400.0);
        expect (analogTop > -6.0, text ("Analog Flanger: a cleaner line (%.1f dB at 8 kHz)", analogTop));
        const double combTop = maxDb (ac, 7000.0, 9000.0) - minDb (ac, 7000.0, 9000.0), analogComb = [&] { const auto a = response (setup (V::analogFlanger, { { "Depth", 0 }, { "Fdbk", 0 } })); return maxDb (a, 7000.0, 9000.0) - minDb (a, 7000.0, 9000.0); }();
        expect (combTop < analogComb - 3.0, text ("AC Flanger: the Reticon line is darker, so the comb is shallower up high (%.1f dB against %.1f dB around 8 kHz)", combTop, analogComb));
    }
}

//==============================================================================
void testShifterAndRing()
{
    std::printf ("  Frequency Shifter\n");
    {
        auto shifted = [] (double hz, double shift, int mode, bool right = false)
        {
            const auto out = render (setup (V::frequencyShifter, { { "Freq", shift }, { "Mode", mode }, { "Mix", 100 } }), 5.0, sine (hz, 0.3));
            const auto& y = right ? out.r : out.l;
            const double wanted = amplitudeAt (y, mode == 1 || (mode == 2 && right) ? hz - shift : hz + shift, 1.0, 5.0);
            const double image = amplitudeAt (y, mode == 1 || (mode == 2 && right) ? hz + shift : hz - shift, 1.0, 5.0);
            return std::make_tuple (averageFrequency (y, 1.0, 5.0), db (wanted / 0.3), db (image / wanted), db (amplitudeAt (y, hz, 1.0, 5.0) / wanted));
        };
        const auto [up, upLevel, upImage, upCarrier] = shifted (1000.0, 100.0, 0);
        expect (std::abs (up - 1100.0) < 0.01 && std::abs (upLevel) < 0.2, text ("Up 100 Hz: 1000 Hz comes out at %.3f Hz, level %.2f dB", up, upLevel));
        expect (upImage < -50.0 && upCarrier < -50.0, text ("single sideband: the other sideband (900 Hz) is at %.0f dB, the original at %.0f dB", upImage, upCarrier));
        const auto [down, downLevel, downImage, downCarrier] = shifted (1000.0, 100.0, 1);
        expect (std::abs (down - 900.0) < 0.01 && downImage < -50.0, text ("Down 100 Hz: %.3f Hz (other sideband %.0f dB)", down, downImage));
        const auto [fine, fineLevel, fineImage, fineCarrier] = shifted (1000.0, 3.3, 0);
        expect (std::abs (fine - 1003.3) < 0.01, text ("Up 3.3 Hz: %.3f Hz", fine));
        const auto [low, lowLevel, lowImage, lowCarrier] = shifted (82.41, 50.0, 0);
        expect (std::abs (low - 132.41) < 0.01 && lowImage < -45.0, text ("the low E string (82.41 Hz) up 50 Hz: %.3f Hz, other sideband %.0f dB", low, lowImage));
        const auto [high, highLevel, highImage, highCarrier] = shifted (6000.0, 1500.0, 0);
        expect (std::abs (high - 7500.0) < 0.05 && highImage < -50.0, text ("6 kHz up 1500 Hz: %.2f Hz, other sideband %.0f dB", high, highImage));
        const auto [sl, slLevel, slImage, slCarrier] = shifted (1000.0, 100.0, 2, false);
        const auto [sr, srLevel, srImage, srCarrier] = shifted (1000.0, 100.0, 2, true);
        expect (std::abs (sl - 1100.0) < 0.01 && std::abs (sr - 900.0) < 0.01, text ("Stereo: %.2f Hz on the left, %.2f Hz on the right", sl, sr));

        // harmonics stop being harmonic: 200 + 400 Hz become 250 + 450 Hz
        const auto out = render (setup (V::frequencyShifter, { { "Freq", 50.0 }, { "Mix", 100 } }), 3.0, [] (int i) { return (float) (0.2 * std::sin (2.0 * fx::pi * 200.0 * i / fs) + 0.2 * std::sin (2.0 * fx::pi * 400.0 * i / fs)); });
        expect (db (amplitudeAt (out.l, 250.0, 1.0, 3.0) / 0.2) > -0.3 && db (amplitudeAt (out.l, 450.0, 1.0, 3.0) / 0.2) > -0.3 && db (amplitudeAt (out.l, 500.0, 1.0, 3.0) / 0.2) < -50.0,
                "every partial moves by the same number of Hz (200 + 400 Hz become 250 + 450 Hz, not 250 + 500)");
        for (double rate : { 44100.0, 96000.0, 192000.0 })
        {
            const auto o = render (setup (V::frequencyShifter, { { "Freq", 100.0 }, { "Mix", 100 } }), 2.0, sine (110.0, 0.3, rate), {}, false, rate);
            std::complex<double> wanted = 0.0, image = 0.0;
            for (size_t i = (size_t) rate; i < o.l.size(); ++i)
            {
                wanted += (double) o.l[i] * std::polar (1.0, -2.0 * fx::pi * 210.0 * (double) i / rate);
                image += (double) o.l[i] * std::polar (1.0, -2.0 * fx::pi * 10.0 * (double) i / rate);
            }
            expect (db (std::abs (image) / std::abs (wanted)) < -40.0, text ("at %.0f Hz sample rate: 110 Hz up 100 Hz, other sideband %.0f dB", rate, db (std::abs (image) / std::abs (wanted))));
        }
        const auto left = render (setup (V::frequencyShifter), 1.0, sine (440.0, 0.3), {}, true);
        expect (rms (left.r) == 0.0, "true stereo: a signal on the left only stays on the left");
    }

    std::printf ("  Ring Modulator\n");
    {
        auto out = render (setup (V::ringModulator, { { "Speed", 150.0 }, { "Mix", 100 } }), 3.0, sine (1000.0, 0.3));
        const double lower = db (amplitudeAt (out.l, 850.0, 1.0, 3.0) / 0.3), upper = db (amplitudeAt (out.l, 1150.0, 1.0, 3.0) / 0.3), original = db (amplitudeAt (out.l, 1000.0, 1.0, 3.0) / 0.3);
        expect (std::abs (lower + 6.02) < 0.1 && std::abs (upper + 6.02) < 0.1 && original < -60.0,
                text ("carrier 150 Hz on 1 kHz: both sidebands, 850 Hz at %.2f dB and 1150 Hz at %.2f dB, the original gone (%.0f dB)", lower, upper, original));
        expect (std::abs (db (amplitudeAt (out.r, 850.0, 1.0, 3.0) / 0.3) + 6.02) < 0.1 && sideRatio (out, 1.0) > 0.5, text ("stereo: same sidebands on the right, from a carrier 90 degrees ahead (side / mid %.2f)", sideRatio (out, 1.0)));

        out = render (setup (V::ringModulator, { { "Speed", 150.0 }, { "Depth", 50 }, { "Mix", 100 } }), 3.0, sine (1000.0, 0.3));
        expect (std::abs (db (amplitudeAt (out.l, 1000.0, 1.0, 3.0) / 0.3) + 6.02) < 0.1 && std::abs (db (amplitudeAt (out.l, 850.0, 1.0, 3.0) / 0.3) + 12.04) < 0.1,
                "Depth 50 %: amplitude modulation, the original comes back (-6 dB, sidebands -12 dB)");
        out = render (setup (V::ringModulator, { { "Speed", 150.0 }, { "Depth", 0 }, { "Mix", 100 } }), 1.0, sine (1000.0, 0.3));
        expect (std::abs (rms (out.l, 0.5, 1.0) - 0.3 / std::sqrt (2.0)) < 0.0005 && amplitudeAt (out.l, 850.0, 0.5, 1.0) < 0.0003, "Depth 0 %: nothing happens");

        out = render (setup (V::ringModulator, { { "Speed", 150.0 }, { "Shape", 100 }, { "Mix", 100 } }), 3.0, sine (1000.0, 0.3));
        const double third = db (amplitudeAt (out.l, 1450.0, 1.0, 3.0) / amplitudeAt (out.l, 1150.0, 1.0, 3.0));
        const auto plain = render (setup (V::ringModulator, { { "Speed", 150.0 }, { "Shape", 0 }, { "Mix", 100 } }), 3.0, sine (1000.0, 0.3));
        expect (third > -14.0 && third < -8.0 && db (amplitudeAt (plain.l, 1450.0, 1.0, 3.0) / 0.3) < -60.0,
                text ("Shape 100 %%: a square carrier adds sidebands at its odd harmonics (1450 Hz at %.1f dB below 1150 Hz)", -third));

        out = render (setup (V::ringModulator, { { "Speed", 150.0 }, { "AM/FM", 100 }, { "Mix", 100 } }), 3.0, sine (1000.0, 0.3));
        const auto env = cut (envelope (out.l, 480), 1.0, 3.0);
        const double fmOriginal = db (amplitudeAt (out.l, 1000.0, 1.0, 3.0) / 0.3), fmSide = db (amplitudeAt (out.l, 1150.0, 1.0, 3.0) / 0.3), fmSecond = db (amplitudeAt (out.l, 1300.0, 1.0, 3.0) / 0.3);
        expect (std::abs (db (rms (out.l, 1.0, 3.0) / (0.3 / std::sqrt (2.0)))) < 0.2 && env.min() > 0.97f * env.max() && fmSide > -12.0 && fmSecond > -20.0,
                text ("AM/FM 100 %%: frequency modulation - constant level, sidebands at multiples of the carrier (1000 / 1150 / 1300 Hz at %.1f / %.1f / %.1f dB)", fmOriginal, fmSide, fmSecond));

        out = render (setup (V::ringModulator, { { "Speed", 4.0 }, { "Mix", 100 } }), 4.0, dc (0.25f));
        expect (std::abs (cycleRate (cut (track (out.l), 1.0, 4.0)) - 4.0) < 0.02, text ("carrier frequency follows Speed (%.3f Hz at 4 Hz)", cycleRate (cut (track (out.l), 1.0, 4.0))));
    }
}

//==============================================================================
/** Rotor speed over time from the loudness swell of a steady tone: (time, revolutions per second). */
Track rotorSpeed (const std::vector<float>& y, int window)
{
    const auto env = envelope (y, window);
    const auto peaks = crossings (env);
    Track out;
    for (size_t i = 1; i < peaks.size(); ++i)
    {
        out.t.push_back (0.5 * (peaks[i] + peaks[i - 1]));
        out.v.push_back ((float) (1.0 / (peaks[i] - peaks[i - 1])));
    }
    return out;
}

void testRotary()
{
    std::printf ("  Rotary Drum\n");
    {
        const auto slow = setup (V::rotaryDrum, { { "Speed", 0 }, { "Depth", 100 }, { "Mix", 100 }, { "Drive", 0 } });
        const auto fast = setup (V::rotaryDrum, { { "Speed", 1 }, { "Depth", 100 }, { "Mix", 100 }, { "Drive", 0 } });
        auto out = render (slow, 12.0, sine (1000.0, 0.2), { { 6.0, fast } });
        const auto speed = rotorSpeed (out.l, 480);
        const float before = cut (speed, 1.0, 5.5).mean(), during = cut (speed, 6.1, 6.6).mean(), after = cut (speed, 10.0, 12.0).mean();
        expect (std::abs (before - 0.67f) < 0.02f && std::abs (after - 5.7f) < 0.1f, text ("Slow %.2f, Fast %.2f revolutions per second (40 and 342 rpm)", before, after));
        const auto ramp = cut (speed, 6.0, 8.5);
        expect (during > 1.5f && during < 5.0f && std::is_sorted (ramp.v.begin(), ramp.v.end(), [] (float a, float b) { return a < b - 0.05f; }),
                text ("the drum takes its time to speed up: %.1f rev/s half a second after the switch", during));
        out = render (fast, 8.0, sine (1000.0, 0.2), { { 3.0, slow } });
        const auto slowing = rotorSpeed (out.l, 480);
        expect (cut (slowing, 3.2, 4.0).mean() > 1.5f && cut (slowing, 3.2, 4.0).mean() < 5.0f && std::abs (slowing.v.back() - 0.67f) < 0.1f,
                text ("... and to slow down: %.1f rev/s during the first second", cut (slowing, 3.2, 4.0).mean()));

        out = render (fast, 3.0, sine (1000.0, 0.2));
        const auto pitch = cut (frequencyTrack (out.l, 4), 1.0, 3.0);
        const double doppler = 0.5 * (pitch.max() - pitch.min());
        expect (std::abs (doppler / 12.5 - 1.0) < 0.15 && std::abs (cycleRate (pitch) - 5.7) < 0.1, text ("Doppler: 1 kHz swings +-%.1f Hz at fast speed (%.0f cents)", doppler, 1200.0 * std::log2 (1.0 + doppler / 1000.0)));
        const auto lowTone = render (fast, 3.0, sine (300.0, 0.2)), highTone = render (fast, 3.0, sine (4000.0, 0.2));
        const auto lowEnv = cut (envelope (lowTone.l, 480), 1.0, 3.0), highEnv = cut (envelope (highTone.l, 480), 1.0, 3.0);
        expect (db (lowEnv.max() / lowEnv.min()) > 3.0 && db (highEnv.max() / highEnv.min()) > db (lowEnv.max() / lowEnv.min()) + 6.0,
                text ("loudness and tone follow the drum: 300 Hz swells by %.1f dB, 4 kHz by %.1f dB", db (lowEnv.max() / lowEnv.min()), db (highEnv.max() / highEnv.min())));
        const auto envL = cut (envelope (out.l, 480), 1.0, 3.0), envR = cut (envelope (out.r, 480), 1.0, 3.0);
        const double turn = (crossings (envL)[2] - crossings (envR)[2]) * 5.7;
        expect (std::abs (std::abs (turn - std::round (turn)) - 0.25) < 0.04, text ("stereo: the two mics hear the drum a quarter turn apart (%.2f turns)", turn - std::round (turn)));

        const auto still = render (setup (V::rotaryDrum, { { "Depth", 0 }, { "Mix", 100 }, { "Drive", 0 } }), 2.0, sine (1000.0, 0.2));
        const auto stillEnv = cut (envelope (still.l, 480), 1.0, 2.0);
        expect (stillEnv.min() > 0.99f * stillEnv.max(), "Depth 0 %: no modulation");
        const auto dark = response (setup (V::rotaryDrum, { { "Depth", 0 }, { "Mix", 100 }, { "Tone", 0 } })), brightest = response (setup (V::rotaryDrum, { { "Depth", 0 }, { "Mix", 100 }, { "Tone", 100 } }));
        expect (maxDb (dark, 4000.0, 4100.0) < maxDb (brightest, 4000.0, 4100.0) - 5.0, text ("Tone: %.1f .. %.1f dB at 4 kHz", maxDb (dark, 4000.0, 4100.0) - maxDb (dark, 300.0, 400.0), maxDb (brightest, 4000.0, 4100.0) - maxDb (brightest, 300.0, 400.0)));
        const auto clean = render (setup (V::rotaryDrum, { { "Depth", 0 }, { "Mix", 100 }, { "Drive", 0 } }), 2.0, sine (200.0, 0.4));
        const auto driven = render (setup (V::rotaryDrum, { { "Depth", 0 }, { "Mix", 100 }, { "Drive", 100 } }), 2.0, sine (200.0, 0.4));
        const double cleanThird = db (amplitudeAt (clean.l, 600.0, 1.0, 2.0) / amplitudeAt (clean.l, 200.0, 1.0, 2.0)), drivenThird = db (amplitudeAt (driven.l, 600.0, 1.0, 2.0) / amplitudeAt (driven.l, 200.0, 1.0, 2.0));
        expect (cleanThird < -30.0 && drivenThird > -16.0 && std::abs (db (rms (driven.l, 1.0, 2.0) / rms (clean.l, 1.0, 2.0))) < 4.0,
                text ("Drive: third harmonic from %.0f dB to %.0f dB, level change %.1f dB", cleanThird, drivenThird, db (rms (driven.l, 1.0, 2.0) / rms (clean.l, 1.0, 2.0))));
    }

    std::printf ("  Rotary Drm/Hrn\n");
    {
        const auto slow = setup (V::rotaryDrumHorn, { { "Speed", 0 }, { "Depth", 100 }, { "Horn Dep", 100 }, { "Drive", 0 } });
        const auto fast = setup (V::rotaryDrumHorn, { { "Speed", 1 }, { "Depth", 100 }, { "Horn Dep", 100 }, { "Drive", 0 } });
        // a 3 kHz tone is on the horn, a 150 Hz tone on the drum
        const auto hornOut = render (slow, 16.0, sine (3000.0, 0.2), { { 6.0, fast } }), drumOut = render (slow, 16.0, sine (150.0, 0.2), { { 6.0, fast } });
        const auto horn = rotorSpeed (hornOut.l, 480), drum = rotorSpeed (drumOut.l, 640);
        expect (std::abs (cut (horn, 1.0, 5.5).mean() - 0.8f) < 0.02f && std::abs (cut (horn, 13.0, 16.0).mean() - 6.8f) < 0.1f,
                text ("horn: Slow %.2f, Fast %.2f revolutions per second (48 and 408 rpm)", cut (horn, 1.0, 5.5).mean(), cut (horn, 13.0, 16.0).mean()));
        expect (std::abs (cut (drum, 1.0, 5.5).mean() - 0.67f) < 0.03f && std::abs (cut (drum, 14.0, 16.0).mean() - 5.7f) < 0.1f,
                text ("drum: Slow %.2f, Fast %.2f revolutions per second (40 and 342 rpm)", cut (drum, 1.0, 5.5).mean(), cut (drum, 14.0, 16.0).mean()));
        const float hornAt1 = cut (horn, 6.8, 7.2).mean(), drumAt1 = cut (drum, 6.7, 7.3).mean(), drumAt3 = cut (drum, 8.7, 9.3).mean();
        expect (hornAt1 > 0.9f * 6.8f && drumAt1 > 1.2f && drumAt1 < 0.6f * 5.7f && drumAt3 > drumAt1 && drumAt3 < 0.95f * 5.7f,
                text ("inertia: one second after switching to Fast the horn turns at %.1f rev/s, the drum at %.1f (and at %.1f after three seconds)", hornAt1, drumAt1, drumAt3));
        const auto drumRamp = cut (drum, 6.0, 12.0);
        double off = 0.0;
        for (size_t i = 0; i < drumRamp.v.size(); ++i)
            off = std::max (off, std::abs (drumRamp.v[i] - (5.7 - 5.03 * std::exp (-(drumRamp.t[i] - 6.0) / 1.5))));
        expect (std::is_sorted (drumRamp.v.begin(), drumRamp.v.end(), [] (float a, float b) { return a < b - 0.05f - 0.03f * b; }) && off < 0.35,
                text ("the drum's speed follows an exponential approach with a 1.5 s time constant (within %.2f rev/s, turn by turn)", off));
        const auto back = render (fast, 12.0, sine (3000.0, 0.2), { { 3.0, slow } });
        const auto hornBack = rotorSpeed (back.l, 480);
        expect (cut (hornBack, 3.3, 3.8).mean() > 1.5f && cut (hornBack, 3.3, 3.8).mean() < 5.5f && std::abs (hornBack.v.back() - 0.8f) < 0.1f,
                text ("back to Slow: the horn is at %.1f rev/s half a second after the switch", cut (hornBack, 3.3, 3.8).mean()));

        const auto hornFast = render (fast, 3.0, sine (3000.0, 0.2));
        const auto pitch = cut (frequencyTrack (hornFast.l, 12), 1.0, 3.0);
        const double doppler = 0.5 * (pitch.max() - pitch.min()) / 3000.0;
        expect (std::abs (doppler / 0.0214 - 1.0) < 0.15, text ("horn Doppler at fast speed: +-%.2f %% of pitch (%.0f cents)", 100.0 * doppler, 1200.0 * std::log2 (1.0 + doppler)));
        const auto hornEnv = cut (envelope (hornFast.l, 480), 1.0, 3.0);
        const auto drumFast = render (fast, 3.0, sine (150.0, 0.2));
        const auto drumEnv = cut (envelope (drumFast.l, 640), 1.0, 3.0);
        const auto drumPitch = cut (frequencyTrack (drumFast.l, 2), 1.0, 3.0);
        expect (db (hornEnv.max() / hornEnv.min()) > 8.0 && db (drumEnv.max() / drumEnv.min()) > 2.0 && db (drumEnv.max() / drumEnv.min()) < db (hornEnv.max() / hornEnv.min()),
                text ("the horn swells by %.1f dB, the drum by %.1f dB (Doppler +-%.2f %%)", db (hornEnv.max() / hornEnv.min()), db (drumEnv.max() / drumEnv.min()),
                      50.0 * (drumPitch.max() - drumPitch.min()) / 150.0));

        // crossover: the two depth knobs act on their own band
        auto swell = [&] (double hz, double drumDepth, double hornDepth)
        {
            const auto o = render (setup (V::rotaryDrumHorn, { { "Speed", 1 }, { "Depth", drumDepth }, { "Horn Dep", hornDepth }, { "Drive", 0 } }), 3.0, sine (hz, 0.2));
            const auto e = cut (envelope (o.l, hz < 1000.0 ? 640 : 480), 1.0, 3.0);
            return db (e.max() / e.min());
        };
        expect (swell (3000.0, 100, 0) < 1.5 && swell (3000.0, 0, 100) > 8.0 && swell (150.0, 0, 100) < 0.5 && swell (150.0, 100, 0) > 2.0,
                text ("800 Hz crossover: Depth moves the bass (%.1f dB at 150 Hz, %.1f dB at 3 kHz), Horn Dep the treble (%.1f dB and %.1f dB)",
                      swell (150.0, 100, 0), swell (3000.0, 100, 0), swell (150.0, 0, 100), swell (3000.0, 0, 100)));
        const auto flat = response (setup (V::rotaryDrumHorn, { { "Depth", 0 }, { "Horn Dep", 0 }, { "Drive", 0 } }));
        expect (maxDb (flat, 100.0, 3000.0) - minDb (flat, 100.0, 3000.0) < 3.0, text ("drum and horn add up again across the crossover (within %.1f dB from 100 Hz to 3 kHz)", maxDb (flat, 100.0, 3000.0) - minDb (flat, 100.0, 3000.0)));
        const auto envR = cut (envelope (hornFast.r, 480), 1.0, 3.0);
        const double turn = (crossings (hornEnv)[2] - crossings (envR)[2]) * 6.8;
        expect (std::abs (std::abs (turn - std::round (turn)) - 0.25) < 0.04, text ("stereo: the two mics hear the horn a quarter turn apart (%.2f turns)", turn - std::round (turn)));
    }
}

//==============================================================================
void testAllModels()
{
    std::printf ("  all models\n");
    const auto riff = harness::makeInput();
    const Signal play = [&] (int i) { return riff[(size_t) i]; };

    // stereo class
    {
        std::string wide, narrow;
        bool ok = true;
        for (const auto& m : models())
        {
            auto s = setup (m.variant);
            if (m.variant == V::barberpolePhaser || m.variant == V::frequencyShifter)
                s = setup (m.variant, { { "Mode", 2 } });
            const auto out = render (s, 4.0, play);
            const double side = sideRatio (out, 0.05);
            const bool same = m.variant == V::patternTremolo || m.variant == V::optoTremolo || m.variant == V::scriptPhase || m.variant == V::uVibe
                           || m.variant == V::pitchVibrato || m.variant == V::flanger80A;
            if (same)
            {
                ok = ok && identical (out.l, out.r);
                narrow += std::string (narrow.empty() ? "" : ", ") + m.name;
            }
            else
            {
                ok = ok && side > 0.1;
                wide += text ("%s%s %.2f", wide.empty() ? "" : ", ", m.name, side);
            }
        }
        expect (ok, "mono guitar in: both sides identical for " + narrow);
        expect (ok, "side / mid level of the stereo models (Barberpole and Frequency Shifter in Stereo mode): " + wide);
    }

    // Mix 0 % = the untouched signal, on each side
    {
        bool ok = true;
        std::string which;
        for (const auto& m : models())
            for (size_t k = 0; k < m.knobs.size(); ++k)
                if (std::string (m.knobs[k].name) == "Mix")
                {
                    const auto out = render (setup (m.variant, { { "Mix", 0 } }), 2.0, play, {}, true);
                    bool same = true;
                    for (size_t i = 0; i < out.l.size(); ++i)
                        same = same && out.l[i] == riff[i] && out.r[i] == 0.0f;
                    ok = ok && same;
                    if (! same)
                        which += std::string (" ") + m.key;
                }
        expect (ok, "Mix 0 %: the output is the input, bit for bit, on each side" + which);
    }

    // no clicks: every knob jumps from one end to the other while a low note sounds
    {
        std::string clicks;
        double worst = 0.0;
        std::string worstName;
        for (const auto& m : models())
            for (size_t k = 0; k < m.knobs.size(); ++k)
                for (int direction = 0; direction < 2; ++direction)
                {
                    auto a = setup (m.variant), b = a;
                    a.knobs[k] = direction == 0 ? m.knobs[k].min : m.knobs[k].max;
                    b.knobs[k] = direction == 0 ? m.knobs[k].max : m.knobs[k].min;
                    const auto out = render (a, 9.0, sine (110.0, 0.3), { { 4.0, b } });
                    auto steepest = [&] (double t0, double t1)
                    {
                        float d = 0.0f;
                        for (size_t i = (size_t) (t0 * fs); i < (size_t) (t1 * fs); ++i)
                            d = std::max ({ d, std::abs (out.l[i] - out.l[i - 1]), std::abs (out.r[i] - out.r[i - 1]) });
                        return (double) d;
                    };
                    const double usual = std::max ({ steepest (0.5, 4.0), steepest (5.0, 9.0), 0.0043 }), moment = steepest (4.0, 4.4);
                    if (moment / usual > worst)
                    {
                        worst = moment / usual;
                        worstName = std::string (m.name) + " / " + m.knobs[k].name;
                    }
                    if (moment > 2.0 * usual)
                        clicks += text (" [%s / %s: step %.4f, usually %.4f]", m.name, m.knobs[k].name, moment, usual);
                }
        expect (clicks.empty(), text ("no clicks when a knob jumps from end to end (largest sample-to-sample step %.2f times the steady one: %s)", worst, worstName.c_str()) + clicks);
    }

    // every knob at both ends, all at once, at 44.1 and 192 kHz
    {
        std::string bad;
        for (double rate : { 44100.0, 192000.0 })
        {
            const auto input = harness::makeInput (rate, 3.0);
            for (const auto& m : models())
                for (int end = 0; end < 2; ++end)
                {
                    auto s = setup (m.variant);
                    for (size_t k = 0; k < m.knobs.size(); ++k)
                        s.knobs[k] = end == 0 ? m.knobs[k].min : m.knobs[k].max;
                    const auto out = render (s, 3.0, [&] (int i) { return input[(size_t) i]; }, {}, false, rate);
                    float peak = 0.0f;
                    bool finite = true;
                    for (size_t i = 0; i < out.l.size(); ++i)
                    {
                        finite = finite && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
                        peak = std::max ({ peak, std::abs (out.l[i]), std::abs (out.r[i]) });
                    }
                    if (! finite || peak > 4.0f)
                        bad += text (" [%s, all knobs at %s, %.0f Hz: peak %.2f]", m.key, end == 0 ? "min" : "max", rate, peak);
                }
        }
        expect (bad.empty(), "stable with all knobs at their lowest and at their highest, at 44.1 and 192 kHz" + bad);
    }

    // the same sound at 96 and 192 kHz: the modulation is timed in seconds and Hz, not in samples
    {
        auto rateOf = [] (int variant, double sampleRate)
        {
            fx::ModFx effect;
            effect.prepare (sampleRate, 256);
            const auto s = setup (variant, { { "Speed", 3.0 }, { "Mix", 100 } });
            effect.setModel (variant);
            effect.setParameters (s.knobs.data());
            effect.reset();
            std::vector<float> l ((size_t) (3.0 * sampleRate), 0.25f), r = l;
            for (size_t pos = 0; pos + 256 <= l.size(); pos += 256)
            {
                effect.setParameters (s.knobs.data());
                effect.process (l.data() + pos, r.data() + pos, 256);
            }
            Track g;
            for (size_t i = 0; i + 256 < l.size(); ++i)
            {
                g.t.push_back ((double) i / sampleRate);
                g.v.push_back (l[i]);
            }
            return cycleRate (g);
        };
        expect (std::abs (rateOf (V::panner, 44100.0) - 3.0) < 0.02 && std::abs (rateOf (V::panner, 192000.0) - 3.0) < 0.02 && std::abs (rateOf (V::optoTremolo, 192000.0) - 3.0) < 0.02,
                text ("LFO rates do not depend on the sample rate (3 Hz: %.3f at 44.1 kHz, %.3f at 192 kHz)", rateOf (V::panner, 44100.0), rateOf (V::panner, 192000.0)));
    }

    // models that should differ, at the same knob settings
    {
        auto flanger = [&] (int variant)
        {
            const bool adaNames = variant == V::flanger80A, mxrNames = variant == V::acFlanger;
            return render (setup (variant, { { "Speed", 0.3 }, { adaNames ? "Range" : mxrNames ? "Width" : "Depth", 70 }, { adaNames ? "Enhance" : mxrNames ? "Regen" : "Fdbk", 50 }, { "Manual", 30 } }), 4.0, play);
        };
        auto tremolo = [&] (int variant) { return render (setup (variant, { { "Speed", 4.0 }, { "Level", 70 }, { "Shape", 0 } }), 4.0, play); };
        const double pairs[] = {
            differenceDb (render (setup (V::scriptPhase, { { "Speed", 0.5 } }), 4.0, play), render (setup (V::phaser, { { "Speed", 0.5 }, { "Depth", 100 }, { "Fdbk", 0 } }), 4.0, play)),
            differenceDb (render (setup (V::scriptPhase, { { "Speed", 0.5 } }), 4.0, play), render (setup (V::uVibe, { { "Speed", 0.5 }, { "Depth", 100 } }), 4.0, play)),
            differenceDb (flanger (V::analogFlanger), flanger (V::acFlanger)),
            differenceDb (flanger (V::jetFlanger), flanger (V::flanger80A)),
            differenceDb (flanger (V::analogFlanger), flanger (V::jetFlanger)),
            differenceDb (tremolo (V::biasTremolo), tremolo (V::optoTremolo)),
            differenceDb (render (setup (V::rotaryDrum, { { "Speed", 1 }, { "Mix", 100 } }), 4.0, play), render (setup (V::rotaryDrumHorn, { { "Speed", 1 }, { "Mix", 100 } }), 4.0, play)),
            differenceDb (render (setup (V::analogChorus), 4.0, play), render (setup (V::triChorus), 4.0, play)),
            differenceDb (render (setup (V::analogChorus), 4.0, play), render (setup (V::dimension), 4.0, play)),
            differenceDb (render (setup (V::frequencyShifter, { { "Freq", 120.0 } }), 4.0, play), render (setup (V::ringModulator), 4.0, play)),
        };
        bool ok = true;
        for (double d : pairs)
            ok = ok && d > -12.0;
        expect (ok, text ("different models differ at the same settings: Script Phase / Phaser %.0f dB, Script Phase / U-Vibe %.0f dB, Analog / AC Flanger %.0f dB, "
                          "Jet / 80A Flanger %.0f dB, Analog / Jet Flanger %.0f dB, Bias / Opto Tremolo %.0f dB, Rotary Drum / Drm+Hrn %.0f dB, "
                          "Analog / Tri Chorus %.0f dB, Analog Chorus / Dimension %.0f dB, Frequency Shifter / Ring Modulator %.0f dB",
                          pairs[0], pairs[1], pairs[2], pairs[3], pairs[4], pairs[5], pairs[6], pairs[7], pairs[8], pairs[9]));
    }
}
} // namespace

int main (int argc, char* argv[])
{
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "test_output/standalone/mod";
    const int harnessFailures = harness::run<fx::ModFx> ("mod", fx::modModels(), outDir);

    std::printf ("== mod: what defines each model ==\n");
    testPatternTremolo();
    testPanner();
    testTremolos();
    testPhasers();
    testVibratoAndChorus();
    testFlangers();
    testShifterAndRing();
    testRotary();
    testAllModels();

    std::printf ("%s\n", failedChecks + harnessFailures == 0 ? "ALL CHECKS PASSED" : (std::to_string (failedChecks + harnessFailures) + " CHECK(S) FAILED").c_str());
    return failedChecks + harnessFailures == 0 ? 0 : 1;
}
