// Standalone test of the reverb engine (see Harness.h): the harness run plus checks of what defines each model.
#include "Harness.h"
#include "../../Source/DSP/fx/Verb.h"

namespace
{
using harness::Knobs;
using V = fx::VerbFx;

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

struct Stereo
{
    std::vector<float> l, r;
    size_t size() const { return l.size(); }
};

/** Runs one model like a slot does (mono input on both sides unless `right` is given). */
Stereo render (int variant, const Knobs& knobs, const std::vector<float>& input, double rate = 48000.0,
               const std::vector<harness::Step>& schedule = {}, const std::vector<float>* right = nullptr, int blockSize = harness::blockSize)
{
    V engine;
    engine.prepare (rate, blockSize);
    engine.setModel (variant);
    engine.setParameters (knobs.data());
    engine.reset();

    Stereo out { std::vector<float> (input.size()), std::vector<float> (input.size()) };
    for (int pos = 0; pos < (int) input.size(); pos += blockSize)
    {
        const int n = std::min (blockSize, (int) input.size() - pos);
        const Knobs* k = &knobs;
        for (const auto& s : schedule)
            if (pos >= s.atSample)
                k = &s.knobs;

        engine.setParameters (k->data());
        std::copy (input.begin() + pos, input.begin() + pos + n, out.l.begin() + pos);
        std::copy ((right != nullptr ? *right : input).begin() + pos, (right != nullptr ? *right : input).begin() + pos + n, out.r.begin() + pos);
        engine.process (out.l.data() + pos, out.r.data() + pos, n);
    }
    return out;
}

Knobs knobsOf (const fx::ModelInfo& m) { return harness::defaultKnobs (m); }

Knobs with (Knobs k, int index, float value)
{
    k[(size_t) index] = value;
    return k;
}

std::vector<float> impulseInput (double seconds, double rate = 48000.0, float amplitude = 0.5f)
{
    std::vector<float> x ((size_t) (seconds * rate), 0.0f);
    x[0] = amplitude;
    return x;
}

/** A snare-like hit: 8 ms of decaying noise. */
std::vector<float> snareInput (double seconds, double rate = 48000.0)
{
    std::vector<float> x ((size_t) (seconds * rate), 0.0f);
    harness::Lcg rng { 777u };
    const int n = (int) (0.008 * rate);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = 0.5f * (rng.next01() * 2.0f - 1.0f) * (1.0f - (float) i / (float) n);
    return x;
}

std::vector<float> sineInput (double seconds, double hz, double onSeconds, double rate = 48000.0, float amplitude = 0.25f)
{
    std::vector<float> x ((size_t) (seconds * rate), 0.0f);
    const int on = (int) (onSeconds * rate), fade = (int) (0.01 * rate);
    for (int i = 0; i < on && i < (int) x.size(); ++i)
    {
        const float env = std::min (1.0f, std::min ((float) i / (float) fade, (float) (on - i) / (float) fade));
        x[(size_t) i] = amplitude * env * (float) std::sin (2.0 * fx::pi * hz * i / rate);
    }
    return x;
}

double energy (const Stereo& s, size_t from, size_t to)
{
    double e = 0.0;
    for (size_t i = from; i < std::min (to, s.size()); ++i)
        e += (double) s.l[i] * s.l[i] + (double) s.r[i] * s.r[i];
    return e;
}

double rmsDb (const Stereo& s, double fromSeconds, double toSeconds, double rate = 48000.0)
{
    const size_t a = (size_t) (fromSeconds * rate), b = std::min ((size_t) (toSeconds * rate), s.size());
    return 10.0 * std::log10 (energy (s, a, b) / (2.0 * (double) std::max<size_t> (1, b - a)) + 1.0e-30);
}

float peakOf (const Stereo& s, size_t from = 0, size_t to = (size_t) -1)
{
    float p = 0.0f;
    for (size_t i = from; i < std::min (to, s.size()); ++i)
        p = std::max (p, std::max (std::abs (s.l[i]), std::abs (s.r[i])));
    return p;
}

bool finite (const Stereo& s)
{
    for (size_t i = 0; i < s.size(); ++i)
        if (! std::isfinite (s.l[i]) || ! std::isfinite (s.r[i]))
            return false;
    return true;
}

/** First sample where the response exceeds -60 dB of its peak. */
int onsetSample (const Stereo& s)
{
    const float threshold = 1.0e-3f * peakOf (s);
    for (size_t i = 0; i < s.size(); ++i)
        if (std::abs (s.l[i]) > threshold || std::abs (s.r[i]) > threshold)
            return (int) i;
    return -1;
}

/** Decay time from the energy decay curve (Schroeder integration): the slope between -5 and -25 dB,
    extrapolated to 60 dB. Returns seconds. */
double rt60 (const Stereo& s, double rate = 48000.0)
{
    const size_t n = s.size();
    std::vector<double> edc (n + 1, 0.0);
    for (size_t i = n; i-- > 0;)
        edc[i] = edc[i + 1] + (double) s.l[i] * s.l[i] + (double) s.r[i] * s.r[i];
    if (edc[0] <= 0.0)
        return 0.0;

    double t5 = -1.0, t25 = -1.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double db = 10.0 * std::log10 (edc[i] / edc[0] + 1.0e-30);
        if (t5 < 0.0 && db <= -5.0)  t5 = (double) i / rate;
        if (t25 < 0.0 && db <= -25.0) { t25 = (double) i / rate; break; }
    }
    return t5 >= 0.0 && t25 > t5 ? 3.0 * (t25 - t5) : 0.0;
}

/** Normalised echo density (Abel & Huang): the share of samples outside one standard deviation in a
    window, relative to Gaussian noise. About 1 = fully dense, near 0 = a few separate echoes. */
double echoDensity (const std::vector<float>& x, double fromSeconds, double toSeconds, double rate = 48000.0)
{
    const size_t a = (size_t) (fromSeconds * rate), b = std::min ((size_t) (toSeconds * rate), x.size());
    double sum = 0.0;
    for (size_t i = a; i < b; ++i)
        sum += (double) x[i] * x[i];
    const double sd = std::sqrt (sum / (double) (b - a));
    if (sd <= 0.0)
        return 0.0;
    int outside = 0;
    for (size_t i = a; i < b; ++i)
        if (std::abs ((double) x[i]) > sd)
            ++outside;
    return ((double) outside / (double) (b - a)) / 0.3173;
}

double echoDensity (const Stereo& s, double from, double to, double rate = 48000.0)
{
    return 0.5 * (echoDensity (s.l, from, to, rate) + echoDensity (s.r, from, to, rate));
}

void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const std::complex<double> wl = std::polar (1.0, -2.0 * fx::pi / (double) len);
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k, w *= wl)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

/** Power spectrum of a stretch of one channel (Hann window, 2^order points). Bin k = k * rate / size. */
std::vector<double> spectrum (const std::vector<float>& x, double fromSeconds, int order, double rate = 48000.0)
{
    const size_t n = (size_t) 1 << order, start = (size_t) (fromSeconds * rate);
    std::vector<std::complex<double>> a (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double window = 0.5 - 0.5 * std::cos (2.0 * fx::pi * (double) i / (double) n);
        a[i] = start + i < x.size() ? window * (double) x[start + i] : 0.0;
    }
    fft (a);
    std::vector<double> p (n / 2);
    for (size_t i = 0; i < n / 2; ++i)
        p[i] = std::norm (a[i]);
    return p;
}

double bandEnergy (const std::vector<double>& p, double lowHz, double highHz, double rate = 48000.0)
{
    const double perBin = rate / (2.0 * (double) p.size());
    double e = 0.0;
    for (size_t i = (size_t) (lowHz / perBin); i < std::min (p.size(), (size_t) (highHz / perBin)); ++i)
        e += p[i];
    return e;
}

/** Spectral centroid (Hz) of both channels from `fromSeconds` on. */
double centroid (const Stereo& s, double fromSeconds, int order = 16)
{
    const auto pl = spectrum (s.l, fromSeconds, order), pr = spectrum (s.r, fromSeconds, order);
    const double perBin = 48000.0 / (2.0 * (double) pl.size());
    double num = 0.0, den = 0.0;
    for (size_t i = 1; i < pl.size(); ++i)
    {
        num += (double) i * perBin * (pl[i] + pr[i]);
        den += pl[i] + pr[i];
    }
    return den > 0.0 ? num / den : 0.0;
}

/** How far single partials stand out of the tail's spectrum: the largest bin relative to the mean of its
    third-octave neighbourhood, in dB (white noise gives about 9 .. 11 dB, a metallic ring far more). */
double ringDb (const std::vector<float>& x, double fromSeconds, int order = 15)
{
    const auto p = spectrum (x, fromSeconds, order);
    const double perBin = 48000.0 / (2.0 * (double) p.size());
    double worst = 0.0;
    for (size_t i = (size_t) (200.0 / perBin); i < (size_t) (5000.0 / perBin); ++i)
    {
        const size_t a = (size_t) ((double) i / 1.122), b = (size_t) ((double) i * 1.122) + 1;
        double mean = 0.0;
        for (size_t k = a; k <= b; ++k)
            mean += p[k];
        mean /= (double) (b - a + 1);
        if (mean > 0.0)
            worst = std::max (worst, p[i] / mean);
    }
    return 10.0 * std::log10 (worst + 1.0e-30);
}

/** Flutter: the strongest periodicity of the tail's envelope (1 ms steps) for lags of 5 .. 120 ms,
    as a normalised autocorrelation (0 = none, 1 = a pure repeating echo). */
double flutter (const std::vector<float>& x, double fromSeconds, double toSeconds, double* lagMs = nullptr)
{
    const int hop = 48;
    const size_t a = (size_t) (fromSeconds * 48000.0), b = std::min ((size_t) (toSeconds * 48000.0), x.size());
    std::vector<double> env;
    for (size_t i = a; i + hop <= b; i += hop)
    {
        double e = 0.0;
        for (int k = 0; k < hop; ++k)
            e += (double) x[i + (size_t) k] * x[i + (size_t) k];
        env.push_back (std::log (e + 1.0e-30));
    }

    // take out the decay itself (a straight line in dB)
    const double n = (double) env.size();
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (size_t i = 0; i < env.size(); ++i) { sx += (double) i; sy += env[i]; sxx += (double) i * (double) i; sxy += (double) i * env[i]; }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx), offset = (sy - slope * sx) / n;
    double var = 0.0;
    for (size_t i = 0; i < env.size(); ++i) { env[i] -= offset + slope * (double) i; var += env[i] * env[i]; }

    double worst = 0.0;
    for (size_t lag = 5; lag <= 120 && lag < env.size() / 2; ++lag)
    {
        double c = 0.0;
        for (size_t i = 0; i + lag < env.size(); ++i)
            c += env[i] * env[i + lag];
        if (c / var > worst)
        {
            worst = c / var;
            if (lagMs != nullptr)
                *lagMs = (double) lag;
        }
    }
    return worst;
}

double correlation (const Stereo& s, double fromSeconds, double toSeconds)
{
    const size_t a = (size_t) (fromSeconds * 48000.0), b = std::min ((size_t) (toSeconds * 48000.0), s.size());
    double ll = 0.0, rr = 0.0, lr = 0.0;
    for (size_t i = a; i < b; ++i)
    {
        ll += (double) s.l[i] * s.l[i];
        rr += (double) s.r[i] * s.r[i];
        lr += (double) s.l[i] * s.r[i];
    }
    return lr / std::sqrt (ll * rr + 1.0e-30);
}

/** Level of one frequency (Goertzel) in a stretch of the left channel, dB. */
double toneDb (const std::vector<float>& x, double hz, double fromSeconds, double toSeconds)
{
    const size_t a = (size_t) (fromSeconds * 48000.0), b = std::min ((size_t) (toSeconds * 48000.0), x.size());
    std::complex<double> sum (0.0, 0.0);
    for (size_t i = a; i < b; ++i)
    {
        const double window = 0.5 - 0.5 * std::cos (2.0 * fx::pi * (double) (i - a) / (double) (b - a));
        sum += window * (double) x[i] * std::polar (1.0, -2.0 * fx::pi * hz * (double) i / 48000.0);
    }
    return 20.0 * std::log10 (std::abs (sum) / (double) (b - a) * 4.0 + 1.0e-30);
}

/** Time (s) at which a band of the response has delivered half of its energy inside [from, to]. */
double bandArrival (const std::vector<float>& x, double lowHz, double highHz, double fromSeconds, double toSeconds)
{
    fx::Biquad hp[2], lp[2];
    for (auto& f : hp) f.setHighPass (48000.0, lowHz, 0.7071);
    for (auto& f : lp) f.setLowPass (48000.0, highHz, 0.7071);
    const size_t a = (size_t) (fromSeconds * 48000.0), b = std::min ((size_t) (toSeconds * 48000.0), x.size());
    std::vector<double> e (b, 0.0);
    double total = 0.0;
    for (size_t i = 0; i < b; ++i)
    {
        const float y = lp[1].process (lp[0].process (hp[1].process (hp[0].process (x[i]))));
        if (i >= a)
        {
            total += (double) y * y;
            e[i] = total;
        }
    }
    for (size_t i = a; i < b; ++i)
        if (e[i] >= 0.5 * total)
            return (double) i / 48000.0;
    return toSeconds;
}

/** The strongest repetition in the waveform itself for lags of 10 .. 120 ms (normalised autocorrelation of the
    stretch with its decay taken out): a noise-like tail gives a few hundredths, a bare comb filter far more. */
double periodicity (const std::vector<float>& x, double fromSeconds, double toSeconds, double rt, double* lagMs = nullptr)
{
    // undo the decay first, so that the whole stretch counts and not just its loud beginning
    const size_t a = (size_t) (fromSeconds * 48000.0), b = std::min ((size_t) (toSeconds * 48000.0), x.size());
    std::vector<double> y (b - a);
    for (size_t i = a; i < b; ++i)
        y[i - a] = ((double) x[i] - (double) x[i - 1]) * std::pow (10.0, 3.0 * (double) (i - a) / (48000.0 * rt)); // first difference: a flatter spectrum

    double worst = 0.0;
    for (size_t lag = 480; lag <= std::min<size_t> (5760, y.size() / 2); lag += 2)
    {
        double c = 0.0, e1 = 0.0, e2 = 0.0;
        for (size_t i = 0; i + lag < y.size(); ++i)
        {
            c += y[i] * y[i + lag];
            e1 += y[i] * y[i];
            e2 += y[i + lag] * y[i + lag];
        }
        const double r = std::abs (c) / std::sqrt (e1 * e2 + 1.0e-30);
        if (r > worst)
        {
            worst = r;
            if (lagMs != nullptr)
                *lagMs = (double) lag / 48.0;
        }
    }
    return worst;
}

/** Spectral balance: energy in 2 .. 8 kHz against 100 .. 800 Hz, dB. */
double brightness (const Stereo& s)
{
    const auto p = spectrum (s.l, 0.0, 16);
    return 10.0 * std::log10 (bandEnergy (p, 2000.0, 8000.0) / (bandEnergy (p, 100.0, 800.0) + 1.0e-30) + 1.0e-30);
}

/** The largest jump in the waveform's curvature (second difference) in a stretch: a click shows up here. */
double roughness (const std::vector<float>& x, double fromSeconds, double toSeconds)
{
    double worst = 0.0;
    for (size_t i = std::max<size_t> (2, (size_t) (fromSeconds * 48000.0)); i < std::min ((size_t) (toSeconds * 48000.0), x.size()); ++i)
        worst = std::max (worst, std::abs ((double) x[i] - 2.0 * x[i - 1] + x[i - 2]));
    return worst;
}

/** How far the roughest 5 ms frame of a stretch stands above its neighbours (the median of the three frames
    before and after it): about 1 for a steady or slowly changing sound, large for a click. */
double spikiness (const std::vector<float>& x, double fromSeconds, double toSeconds)
{
    std::vector<double> frames;
    for (double t = fromSeconds; t + 0.005 <= toSeconds; t += 0.005)
        frames.push_back (roughness (x, t, t + 0.005));

    double worst = 0.0;
    for (size_t f = 3; f + 3 < frames.size(); ++f)
    {
        std::vector<double> around { frames[f - 3], frames[f - 2], frames[f - 1], frames[f + 1], frames[f + 2], frames[f + 3] };
        std::sort (around.begin(), around.end());
        worst = std::max (worst, frames[f] / (0.5 * (around[2] + around[3]) + 1.0e-30));
    }
    return worst;
}

/** Decay time of one frequency band of a response. */
double bandRt60 (const Stereo& s, double lowHz, double highHz)
{
    Stereo band { s.l, s.r };
    for (auto* x : { &band.l, &band.r })
    {
        fx::Biquad hp[2], lp[2];
        for (auto& f : hp) f.setHighPass (48000.0, lowHz, 0.7071);
        for (auto& f : lp) f.setLowPass (48000.0, highHz, 0.7071);
        for (auto& v : *x)
            v = lp[1].process (lp[0].process (hp[1].process (hp[0].process (v))));
    }
    return rt60 (band);
}

/** When the response is at its loudest: the middle of the 20 ms stretch with the most energy (seconds). */
double loudestAt (const Stereo& s)
{
    double best = 0.0, at = 0.0;
    for (size_t i = 0; i + 960 <= std::min (s.size(), (size_t) 48000); i += 96)
    {
        const double e = energy (s, i, i + 960);
        if (e > best)
        {
            best = e;
            at = (double) (i + 480) / 48000.0;
        }
    }
    return at;
}

/** Energy of a band of the spectrum of x[from ..], dB. */
double bandDb (const std::vector<float>& x, double fromSeconds, double lowHz, double highHz, int order = 15)
{
    return 10.0 * std::log10 (bandEnergy (spectrum (x, fromSeconds, order), lowHz, highHz) + 1.0e-30);
}

const char* m_key (const std::vector<fx::ModelInfo>& models, int v) { return models[(size_t) v].key; }

std::string num (double v, int digits = 2)
{
    char buffer[40];
    std::snprintf (buffer, sizeof (buffer), "%.*f", digits, v);
    return buffer;
}

} // namespace

//==============================================================================
int main (int argc, char* argv[])
{
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "test_output/standalone/verb";
    verbose = argc > 2;
    const auto models = fx::verbModels();

    harness::Options options;
    options.wetOnly = true;
    failed += harness::run<V> ("verb", models, outDir, options);

    const int numRooms = 11; // the models with Decay, PreDelay, Tone, Mix (all but Particle Verb)
    enum { kDecay = 0, kPre = 1, kTone = 2, kCondition = 1, kGain = 2 };
    auto key = [&] (int v) { return std::string (models[(size_t) v].key); };
    auto dry = [&] (int v) { return v < numRooms ? with (knobsOf (models[(size_t) v]), kPre, 0.0f) : knobsOf (models[(size_t) v]); }; // default knobs, no pre-delay

    //==========================================================================
    std::printf ("== decay time (RT60 from the energy decay curve of an impulse) against the Decay knob ==\n");
    std::printf ("  model             0 %%    25 %%    50 %%    75 %%   100 %%   default   tail()\n");
    double defaultRt[12] {};
    for (int v = 0; v < 12; ++v)
    {
        const auto& m = models[(size_t) v];
        std::printf ("  %-14s", m.key);
        double previous = 0.0;
        bool rising = true;
        for (float decay : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
        {
            const double rt = rt60 (render (v, with (dry (v), kDecay, decay), impulseInput (decay > 60.0f ? 30.0 : 12.0)));
            std::printf (" %6.2f ", rt);
            rising = rising && rt > previous * 1.15;
            previous = rt;
        }
        check (rising, key (v) + ": the decay time does not rise with the Decay knob");

        V engine;
        engine.prepare (48000.0, 256);
        engine.setModel (v);
        engine.setParameters (dry (v).data());
        const double tail = engine.getTailSeconds();
        defaultRt[v] = rt60 (render (v, dry (v), impulseInput (20.0)));
        std::printf ("  %6.2f   %6.2f\n", defaultRt[v], tail);
        // falling 100 dB takes 100 / 60 of the RT60
        check (tail >= 0.5 && tail <= 20.5 && tail > 1.1 * defaultRt[v] && tail < 2.6 * defaultRt[v] + 0.5, key (v) + ": getTailSeconds() does not fit the measured decay");
    }

    // plausible for each space
    const double rtLow[12]  = { 0.8, 0.15, 0.7, 1.8, 1.5, 0.3, 3.0, 1.8, 3.0, 1.0, 1.2, 4.0 };
    const double rtHigh[12] = { 2.5, 0.7,  2.2, 4.0, 4.5, 1.0, 8.0, 5.0, 10.0, 2.5, 3.5, 12.0 };
    for (int v = 0; v < 12; ++v)
        check (defaultRt[v] >= rtLow[v] && defaultRt[v] <= rtHigh[v], key (v) + ": decay time at the default knobs is " + num (defaultRt[v]) + " s");
    check (defaultRt[V::room] < defaultRt[V::tile] && defaultRt[V::tile] < defaultRt[V::plate] && defaultRt[V::plate] < defaultRt[V::hall]
           && defaultRt[V::hall] < defaultRt[V::cave], "decay times should be ordered room < tile < plate < hall < cave");

    //==========================================================================
    std::printf ("== impulse response at the default knobs (PreDelay 0) ==\n");
    std::printf ("  model          onset      first 80 ms   echo density at 5-25 / 30-60 / 80-120 / 250-300 ms   centroid   L/R corr   L/R dB   DC\n");
    double early[12] {}, density[12][4] {}, cen[12] {}, bright[12] {};
    for (int v = 0; v < 12; ++v)
    {
        const auto ir = render (v, dry (v), impulseInput (8.0));
        const int onset = onsetSample (ir);
        const double t0 = (double) onset / 48000.0;
        early[v] = energy (ir, 0, (size_t) onset + 3840) / energy (ir, 0, ir.size());
        const double from[4] = { 0.005, 0.03, 0.08, 0.25 }, to[4] = { 0.025, 0.06, 0.12, 0.3 };
        for (int i = 0; i < 4; ++i)
            density[v][i] = echoDensity (ir, t0 + from[i], t0 + to[i]);
        cen[v] = centroid (ir, 0.0);
        bright[v] = brightness (ir);
        const double corr = correlation (ir, t0 + 0.05, t0 + 2.0);

        double mean = 0.0, el = 0.0, er = 0.0;
        for (size_t i = 0; i < ir.size(); ++i)
        {
            mean += (double) ir.l[i] + ir.r[i];
            el += (double) ir.l[i] * ir.l[i];
            er += (double) ir.r[i] * ir.r[i];
        }
        const double dc = std::abs (mean) / std::sqrt ((el + er) * (double) ir.size() * 2.0 + 1.0e-30);
        const double balance = 10.0 * std::log10 (el / er);

        std::printf ("  %-14s %5.2f ms   %5.1f %%       %5.2f   %5.2f   %5.2f   %5.2f                            %6.0f Hz   %5.2f     %5.1f   %7.5f\n",
                     m_key (models, v), 1000.0 * t0, 100.0 * early[v], density[v][0], density[v][1], density[v][2], density[v][3], cen[v], corr, balance, dc);

        if (v == V::spring || v == V::spring63)
            check (t0 > 0.02 && t0 < 0.045, key (v) + ": the first sound should arrive after the spring's transit time (about 30 ms), not after " + num (1000.0 * t0) + " ms");
        else
            check (onset >= 0 && t0 < 0.001, key (v) + ": the reverb starts " + num (1000.0 * t0) + " ms after the input at PreDelay 0");

        check (std::abs (corr) < 0.3, key (v) + ": left and right tails are correlated (" + num (corr) + ")");
        check (std::abs (balance) < 3.0, key (v) + ": left and right differ by " + num (balance) + " dB");
        check (dc < 0.01, key (v) + ": DC in the response (" + num (dc, 4) + ")");
        // dense after a quarter of a second at the latest (no sparse, separate repeats left)
        check (density[v][3] > 0.75, key (v) + ": echo density 250 ms in is only " + num (density[v][3]));
        if (v != V::spring && v != V::spring63) // a spring's first repeats are separate chirps
            check (density[v][2] > 0.7, key (v) + ": echo density 100 ms in is only " + num (density[v][2]));
    }

    // what sets the spaces apart
    check (density[V::plate][0] > 0.6, "plate: should be dense from the first milliseconds");
    check (density[V::plate][0] > density[V::hall][0] + 0.2, "plate should build up its density faster than hall");
    check (density[V::room][0] < 0.45 && density[V::tile][0] < 0.55, "room and tile should start with separate early reflections");
    check (early[V::room] > 0.85 && early[V::tile] > 0.8, "room and tile: most of the energy should arrive in the first 80 ms");
    check (early[V::hall] < 0.4 && early[V::cave] < 0.25 && early[V::hall] < early[V::chamber], "hall and cave should build up slowly");
    check (bright[V::tile] > bright[V::room] + 1.5 && bright[V::plate] > bright[V::hall] + 1.5, "tile should be brighter than room, plate brighter than hall");
    for (int v = 0; v < numRooms; ++v)
        if (v != V::cave && v != V::spring && v != V::spring63)
            check (bright[V::cave] < bright[v] - 6.0, "cave should be much darker than " + key (v));

    // no two of the plain spaces may measure alike: decay time, early share, brightness
    for (int a : { (int) V::plate, (int) V::room, (int) V::chamber, (int) V::hall, (int) V::tile, (int) V::cave, (int) V::spring, (int) V::spring63, (int) V::particleVerb })
        for (int b : { (int) V::plate, (int) V::room, (int) V::chamber, (int) V::hall, (int) V::tile, (int) V::cave, (int) V::spring, (int) V::spring63, (int) V::particleVerb })
            if (a < b)
                check (std::abs (std::log (defaultRt[a] / defaultRt[b])) > 0.25 || std::abs (early[a] - early[b]) > 0.15 || std::abs (bright[a] - bright[b]) > 3.0,
                       key (a) + " and " + key (b) + " measure alike");

    //==========================================================================
    std::printf ("== build-up and decay per band (150-400 Hz / 0.8-2 kHz / 4-9 kHz, springs 2.8-4 kHz) at the default knobs ==\n");
    {
        double peakAt[12] {}, low[12] {}, mid[12] {}, high[12] {};
        for (int v = 0; v < 12; ++v)
        {
            const auto ir = render (v, dry (v), impulseInput (16.0));
            peakAt[v] = loudestAt (ir) - (double) onsetSample (ir) / 48000.0;
            low[v] = bandRt60 (ir, 150.0, 400.0);
            mid[v] = bandRt60 (ir, 800.0, 2000.0);
            high[v] = v == V::spring || v == V::spring63 ? bandRt60 (ir, 2800.0, 4000.0) : bandRt60 (ir, 4000.0, 9000.0); // a spring ends at 4 kHz
            std::printf ("  %-14s loudest %5.0f ms after the onset; RT60 %5.2f / %5.2f / %5.2f s\n", m_key (models, v), 1000.0 * peakAt[v], low[v], mid[v], high[v]);
        }
        check (peakAt[V::plate] < 0.03 && peakAt[V::room] < 0.03 && peakAt[V::tile] < 0.03, "plate, room and tile should be at their loudest right away");
        check (peakAt[V::hall] > peakAt[V::plate] + 0.02 && peakAt[V::cave] > peakAt[V::hall], "hall and cave should take time to build up");
        check (peakAt[V::particleVerb] > 0.08, "particle_verb should swell in (a pad)");
        // the damping inside the loops: highs die first; in the big spaces and the plate the lows ring longest
        for (int v : { (int) V::plate, (int) V::chamber, (int) V::hall, (int) V::echo, (int) V::cave, (int) V::ducking })
            check (low[v] > 1.05 * mid[v] && mid[v] > (v == V::plate ? 1.05 : 1.2) * high[v], // (the plate is bright: its highs last)
                   key (v) + ": decay per band should be lows > mids > highs");
        check (low[V::room] < 1.05 * mid[V::room] && low[V::tile] < 1.05 * mid[V::tile], "room and tile: the lows should not ring longer than the mids (small rooms)");
        check (high[V::tile] / mid[V::tile] > high[V::hall] / mid[V::hall] + 0.15, "tile should keep its highs longer than hall");
        check (low[V::cave] / high[V::cave] > 2.5, "cave: the lows should ring far longer than the highs");
        check (low[V::spring] > 1.25 * high[V::spring] && low[V::spring63] > 1.25 * high[V::spring63], "springs should lose their highs first");
    }

    //==========================================================================
    std::printf ("== smoothness of the tail after a snare-like hit ==\n");
    std::printf ("  model          envelope flutter (lag)   waveform periodicity   loudest partial over its third octave\n");
    {
        // what the measures say about ideal and about bad material
        harness::Lcg rng { 5u };
        std::vector<float> noise ((size_t) (48000 * 4)), comb (noise.size(), 0.0f);
        for (size_t i = 0; i < noise.size(); ++i)
        {
            double g = 0.0;
            for (int k = 0; k < 6; ++k)
                g += rng.next01() - 0.5;
            noise[i] = (float) (g * std::pow (10.0, -3.0 * (double) i / (48000.0 * 1.5)));
        }
        const auto hit = snareInput (4.0);
        for (int d : { 1687, 1601, 2053, 2251 })
        {
            std::vector<float> y (comb.size(), 0.0f);
            for (size_t i = 0; i < y.size(); ++i)
            {
                y[i] = hit[i] + (i >= (size_t) d ? 0.9f * y[i - (size_t) d] : 0.0f);
                comb[i] += y[i];
            }
        }
        const double f1 = flutter (noise, 0.2, 1.2), p1 = periodicity (noise, 0.2, 0.9, 1.5), f2 = flutter (comb, 0.2, 1.2), p2 = periodicity (comb, 0.2, 0.9, 2.3);
        std::printf ("  (decaying noise  %4.2f                     %4.2f                   %4.1f dB)\n", f1, p1, ringDb (noise, 0.3, 12));
        std::printf ("  (four bare combs %4.2f                     %4.2f                   %4.1f dB)\n", f2, p2, ringDb (comb, 0.3, 12));
        check (f1 < 0.15 && p1 < 0.1 && (f2 > 0.3 || p2 > 0.3), "the smoothness measures do not tell noise from comb filters");
    }
    for (int v = 0; v < 12; ++v)
    {
        const auto hit = render (v, dry (v), snareInput (8.0));
        const double t0 = (double) onsetSample (hit) / 48000.0, start = t0 + std::max (0.15, 0.15 * defaultRt[v]);
        double lag = 0.0;
        const double flut = std::max (flutter (hit.l, start, start + std::max (0.5, 0.6 * defaultRt[v]), &lag), flutter (hit.r, start, start + std::max (0.5, 0.6 * defaultRt[v])));
        const double span = std::clamp (1.5 * defaultRt[v], 0.4, 0.7);
        double lagL = 0.0, lagR = 0.0;
        const double periodL = periodicity (hit.l, start, start + span, defaultRt[v], &lagL), periodR = periodicity (hit.r, start, start + span, defaultRt[v], &lagR);
        const double period = std::max (periodL, periodR), periodLag = periodL > periodR ? lagL : lagR;
        const double ring = std::max (ringDb (hit.l, t0 + std::max (0.08, 0.3 * defaultRt[v]), 12), ringDb (hit.r, t0 + std::max (0.08, 0.3 * defaultRt[v]), 12));
        std::printf ("  %-14s   %4.2f (%3.0f ms)            %4.2f (%5.1f ms)        %4.1f dB\n", m_key (models, v), flut, lag, period, periodLag, ring);

        if (v == V::echo) // its repeats are meant to be heard: only the tail between them has to be smooth
            continue;
        check (flut < 0.25, key (v) + ": the tail flutters (" + num (flut) + " at " + num (lag, 0) + " ms)");
        // (a spring is a recirculating delay: its waveform does come round again, blurred)
        check (period < (v == V::spring || v == V::spring63 ? 0.3 : 0.2), key (v) + ": the tail repeats itself / rings (" + num (period) + ")");
        check (ring < 13.0, key (v) + ": a partial stands " + num (ring, 1) + " dB out of the tail");
    }

    //==========================================================================
    std::printf ("== pre-delay: onset of the response against the knob ==\n");
    for (int v = 0; v < numRooms; ++v)
    {
        const int base = onsetSample (render (v, dry (v), impulseInput (1.0)));
        std::printf ("  %-14s", m_key (models, v));
        for (float ms : { 1.0f, 20.0f, 50.0f, 125.0f, 200.0f })
        {
            const int onset = onsetSample (render (v, with (dry (v), kPre, ms), impulseInput (1.0)));
            const double measured = 1000.0 * (double) (onset - base) / 48000.0;
            std::printf ("  %3.0f -> %6.2f ms", ms, measured);
            check (std::abs (measured - (double) ms) < 1.0, key (v) + ": PreDelay " + num (ms, 0) + " ms delays the reverb by " + num (measured) + " ms");
        }
        std::printf ("\n");
    }

    //==========================================================================
    std::printf ("== Tone: 2-8 kHz against 100-800 Hz in the response ==\n");
    for (int v = 0; v < numRooms; ++v)
    {
        double b[3] {};
        int i = 0;
        for (float tone : { 0.0f, 50.0f, 100.0f })
            b[i++] = brightness (render (v, with (dry (v), kTone, tone), impulseInput (6.0)));
        std::printf ("  %-14s Tone 0 %%: %6.1f dB   50 %%: %6.1f dB   100 %%: %6.1f dB\n", m_key (models, v), b[0], b[1], b[2]);
        check (b[1] > b[0] + 3.0 && b[2] > b[1] + 1.0 && b[2] > b[0] + 8.0, key (v) + ": Tone does not tilt the spectrum of the wet signal");
    }

    //==========================================================================
    std::printf ("== Echo: separate repeats in front of the tail ==\n");
    {
        // PreDelay 90 ms: repeats every 100 + 2 * 90 = 280 ms, left first
        const auto ir = render (V::echo, with (knobsOf (models[V::echo]), kPre, 90.0f), impulseInput (3.0));
        const double first = 0.09 + 0.28, second = first + 0.28;
        auto standsOut = [&] (const std::vector<float>& x, double at) // peak at the echo time over the RMS of the 20 ms before it
        {
            double peak = 0.0, before = 0.0;
            for (size_t i = (size_t) ((at - 0.001) * 48000.0); i < (size_t) ((at + 0.002) * 48000.0); ++i) peak = std::max (peak, std::abs ((double) x[i]));
            for (size_t i = (size_t) ((at - 0.022) * 48000.0); i < (size_t) ((at - 0.002) * 48000.0); ++i) before += (double) x[i] * x[i] / 960.0;
            return 20.0 * std::log10 (peak / std::sqrt (before + 1.0e-30));
        };
        const double l1 = standsOut (ir.l, first), r1 = standsOut (ir.r, first), l2 = standsOut (ir.l, second), r2 = standsOut (ir.r, second);
        std::printf ("  repeat at %.0f ms: left %+.1f dB, right %+.1f dB over the tail; at %.0f ms: left %+.1f dB, right %+.1f dB\n", 1000.0 * first, l1, r1, 1000.0 * second, l2, r2);
        check (l1 > 20.0 && r2 > 20.0, "echo_verb: no separate repeats at the echo time");
        check (l1 > r1 + 6.0 && r2 > l2 + 6.0, "echo_verb: the repeats should alternate between left and right");
        const auto hall = render (V::hall, with (knobsOf (models[V::hall]), kPre, 90.0f), impulseInput (3.0));
        std::printf ("  hall at %.0f ms: %+.1f dB (noise-like: about 10 dB)\n", 1000.0 * first, standsOut (hall.l, first));
        check (standsOut (hall.l, first) < 14.0, "hall should not have a separate repeat there");
    }

    //==========================================================================
    std::printf ("== Octo: octaves above a 440 Hz note in the tail ==\n");
    {
        const auto note = sineInput (4.0, 440.0, 0.6);
        for (int v : { (int) V::octo, (int) V::hall })
        {
            const auto y = render (v, dry (v), note);
            // a semitone either side of each octave (the shifter's grains spread the line a little)
            const double f0 = bandDb (y.l, 1.0, 415.0, 466.0), f1 = bandDb (y.l, 1.0, 830.0, 932.0), f2 = bandDb (y.l, 1.0, 1660.0, 1864.0);
            const double later = bandDb (y.l, 2.5, 830.0, 932.0) - bandDb (y.l, 2.5, 415.0, 466.0);
            std::printf ("  %-14s 1 .. 1.7 s: around 440 Hz %6.1f dB, 880 Hz %+6.1f dB, 1760 Hz %+6.1f dB against it; 880 Hz from 2.5 s on: %+6.1f dB\n",
                         m_key (models, v), f0, f1 - f0, f2 - f0, later);
            if (v == V::octo)
            {
                check (f1 > f0 - 12.0 && f2 > f0 - 24.0, "octo: the tail has no clear octave (880 Hz) and double octave above the note");
                check (later > f1 - f0, "octo: the octave should take over as the tail goes on");
            }
            else
                check (f1 < f0 - 40.0, "hall: should not add an octave");
        }
    }

    //==========================================================================
    std::printf ("== Ducking: wet level while playing and after ==\n");
    {
        // two seconds of a loud note, then silence
        const auto note = sineInput (6.0, 220.0, 2.0, 48000.0, 0.3f);
        for (int v : { (int) V::ducking, (int) V::hall })
        {
            const auto y = render (v, dry (v), note);
            const double playing = rmsDb (y, 1.0, 1.9), after = rmsDb (y, 2.3, 2.7);
            std::printf ("  %-14s while playing %6.1f dB, 0.3 .. 0.7 s after the note %6.1f dB (%+.1f)\n", m_key (models, v), playing, after, after - playing);
            if (v == V::ducking)
                check (after > playing + 3.0, "ducking: the reverb should swell when the playing stops");
            else
                check (after < playing - 1.0, "hall: should simply decay");
        }
        // a quiet note is not ducked: compare the gain of the wet signal for a loud and a 40 dB quieter note
        const auto loud = render (V::ducking, dry (V::ducking), sineInput (3.0, 220.0, 2.0, 48000.0, 0.3f));
        const auto quiet = render (V::ducking, dry (V::ducking), sineInput (3.0, 220.0, 2.0, 48000.0, 0.003f));
        const double reduction = (rmsDb (quiet, 1.0, 1.9) + 40.0) - rmsDb (loud, 1.0, 1.9);
        std::printf ("  gain reduction on a loud note: %.1f dB\n", reduction);
        check (reduction > 8.0 && reduction < 24.0, "ducking: the wet signal should be 8 .. 24 dB lower while playing loud");
    }

    //==========================================================================
    std::printf ("== springs: dispersion (arrival of two bands in the first echo) ==\n");
    {
        // half of each band's energy in the 25 ms after the onset (the first trip along the springs)
        double late[2] {};
        int i = 0;
        for (int v : { (int) V::spring, (int) V::spring63 })
        {
            const auto ir = render (v, dry (v), impulseInput (1.0, 48000.0, 0.05f));
            const double t0 = (double) onsetSample (ir) / 48000.0;
            const double low = bandArrival (ir.l, 500.0, 1200.0, t0 - 0.002, t0 + 0.025) - t0, high = bandArrival (ir.l, 2600.0, 3600.0, t0 - 0.002, t0 + 0.025) - t0;
            late[i++] = 1000.0 * (high - low);
            std::printf ("  %-14s 0.5-1.2 kHz after %5.2f ms, 2.6-3.6 kHz after %5.2f ms\n", m_key (models, v), 1000.0 * low, 1000.0 * high);
        }
        check (late[0] > 1.5 && late[1] > 4.0, "springs: the highs should arrive milliseconds after the lows (chirp)");
        check (late[1] > late[0] + 2.0, "'63 spring should disperse more than the studio spring");

        // tube grit of the '63: harmonics of a loud note that the studio spring does not make
        for (int v : { (int) V::spring, (int) V::spring63 })
        {
            const auto y = render (v, dry (v), sineInput (2.0, 330.0, 1.5, 48000.0, 0.4f));
            const double h = std::max (toneDb (y.l, 660.0, 0.7, 1.4), toneDb (y.l, 990.0, 0.7, 1.4)) - toneDb (y.l, 330.0, 0.7, 1.4);
            std::printf ("  %-14s harmonics of a loud 330 Hz note: %.1f dB\n", m_key (models, v), h);
            check (v == V::spring ? h < -50.0 : (h > -45.0 && h < -15.0), key (v) + ": harmonic distortion is " + num (h, 1) + " dB");
        }
        // bandwidth: nothing much above the tank's 5 kHz
        for (int v : { (int) V::spring, (int) V::spring63 })
        {
            const auto p = spectrum (render (v, with (dry (v), kTone, 100.0f), impulseInput (3.0)).l, 0.0, 16);
            const double top = 10.0 * std::log10 (bandEnergy (p, 6500.0, 20000.0) / bandEnergy (p, 300.0, 4000.0));
            std::printf ("  %-14s above 6.5 kHz: %.1f dB\n", m_key (models, v), top);
            check (top < -25.0, key (v) + ": the spring should be band-limited");
        }
    }

    //==========================================================================
    std::printf ("== Particle Verb: a 440 Hz note, where its pitch goes ==\n");
    {
        const auto note = sineInput (6.0, 440.0, 0.5);
        double drift[3] {}, scattered[3] {};
        for (int condition = 0; condition < 3; ++condition)
        {
            const auto y = render (V::particleVerb, with (knobsOf (models[V::particleVerb]), kCondition, (float) condition), note);
            auto pitchAt = [&] (double from) // centre of the energy between 300 and 900 Hz
            {
                const auto p = spectrum (y.l, from, 15);
                const double perBin = 48000.0 / (2.0 * (double) p.size());
                double n = 0.0, d = 0.0;
                for (size_t k = (size_t) (300.0 / perBin); k < (size_t) (900.0 / perBin); ++k) { n += (double) k * perBin * p[k]; d += p[k]; }
                return n / d;
            };
            const double early440 = pitchAt (0.6), late440 = pitchAt (3.0);
            const auto p = spectrum (y.l, 2.0, 16);
            scattered[condition] = 1.0 - bandEnergy (p, 415.0, 466.0) / bandEnergy (p, 20.0, 20000.0); // share outside a semitone around the note
            drift[condition] = 1200.0 * std::log2 (late440 / early440);
            std::printf ("  %-10s 0.6 s: %6.1f Hz, 3 s: %6.1f Hz (%+.0f cents); energy away from the note after 2 s: %4.1f %%; level 4 .. 5 s %6.1f dB\n",
                         fx::verbConditionNames[condition], early440, late440, drift[condition], 100.0 * scattered[condition], rmsDb (y, 4.0, 5.0));
        }
        check (std::abs (drift[0]) < 30.0 && scattered[0] < 0.2, "particle_verb Stable: the pad should stay on the note");
        check (drift[1] > 40.0, "particle_verb Critical: the pitch should rise slowly");
        check (scattered[2] > 0.6 && scattered[2] > scattered[1], "particle_verb Hazard: the pitch should run wild");

        // slow attack: a pad, not an echo of the pick
        const auto ir = render (V::particleVerb, knobsOf (models[V::particleVerb]), impulseInput (8.0));
        check (early[V::particleVerb] < 0.2, "particle_verb: should swell in slowly");
        // Gain drives the pad: +12 dB in gives less than +12 dB out (soft limiting), -12 dB simply 12 dB less
        const auto riff = harness::makeInput();
        const double mid = rmsDb (render (V::particleVerb, knobsOf (models[V::particleVerb]), riff), 0.1, 5.0);
        const double hot = rmsDb (render (V::particleVerb, with (knobsOf (models[V::particleVerb]), kGain, 100.0f), riff), 0.1, 5.0);
        const double low = rmsDb (render (V::particleVerb, with (knobsOf (models[V::particleVerb]), kGain, 0.0f), riff), 0.1, 5.0);
        std::printf ("  Gain 0 / 50 / 100 %%: %.1f / %.1f / %.1f dB\n", low, mid, hot);
        check (std::abs ((mid - low) - 12.0) < 1.0 && hot - mid > 6.0 && hot - mid < 12.5, "particle_verb: Gain should cover about +-12 dB");
    }

    //==========================================================================
    std::printf ("== every knob at its maximum, 20 s of playing, then 10 s of silence ==\n");
    {
        std::vector<float> playing;
        const auto riff = harness::makeInput();
        for (int i = 0; i < 4; ++i)
            playing.insert (playing.end(), riff.begin(), riff.end());
        playing.resize (playing.size() + (size_t) (10 * 48000), 0.0f);

        for (int v = 0; v < 12; ++v)
            for (int condition = 0; condition < (v == V::particleVerb ? 3 : 1); ++condition)
            {
                Knobs k = with (knobsOf (models[(size_t) v]), kDecay, 100.0f);
                if (v == V::particleVerb)
                    k = with (with (k, kCondition, (float) condition), kGain, 100.0f);
                else
                    k = with (k, kTone, 100.0f);

                const auto y = render (v, k, playing);
                const float p1 = peakOf (y, 5 * 48000, 10 * 48000), p2 = peakOf (y, 15 * 48000, 20 * 48000);
                const double atStop = rmsDb (y, 19.0, 20.0), atEnd = rmsDb (y, 29.0, 30.0);
                std::printf ("  %-14s%s peak %.2f in 5 .. 10 s, %.2f in 15 .. 20 s; level %6.1f dB at the end of the playing, %6.1f dB 10 s later\n",
                             m_key (models, v), v == V::particleVerb ? (std::string (" ") + fx::verbConditionNames[condition]).c_str() : "", p1, p2, atStop, atEnd);
                check (finite (y) && peakOf (y) < 2.0f, key (v) + ": not bounded at maximum Decay");
                check (p2 < 1.5f * p1 + 0.01f, key (v) + ": the level keeps growing at maximum Decay");
                check (atEnd < atStop - 3.0, key (v) + ": does not decay at maximum Decay");
            }
    }

    //==========================================================================
    std::printf ("== knobs moved while two notes ring: roughness (curvature) during the move against the steady states, and (its spike) ==\n");
    {
        // two steady notes: the wet signal is smooth, so a click stands out in its second difference
        std::vector<float> notes ((size_t) (4 * 48000));
        for (size_t i = 0; i < notes.size(); ++i)
            notes[i] = 0.15f * (float) (std::sin (2.0 * fx::pi * 196.0 * (double) i / 48000.0) + std::sin (2.0 * fx::pi * 294.0 * (double) i / 48000.0))
                       * std::min (1.0f, (float) i / 2400.0f);

        struct Move { int knob; float from, to; const char* name; };
        const Move moves[] = { { kDecay, 20.0f, 100.0f, "Decay up" }, { kDecay, 100.0f, 0.0f, "Decay down" }, { kPre, 0.0f, 200.0f, "PreDelay up" },
                               { kPre, 120.0f, 10.0f, "PreDelay down" }, { kTone, 0.0f, 100.0f, "Tone up" }, { kTone, 100.0f, 0.0f, "Tone down" } };
        for (int v = 0; v < 12; ++v)
        {
            std::printf ("  %-14s", m_key (models, v));
            for (const auto& move : moves)
            {
                int knob = move.knob;
                float from = move.from, to = move.to;
                if (v == V::particleVerb)
                {
                    if (move.knob == kPre) continue;
                    if (move.knob == kTone) { knob = kGain; }
                }
                // the curvature during the move against the larger of before and after it has settled
                // (brightness and level may change; a click is a burst that belongs to neither)
                const Knobs before = with (dry (v), knob, from);
                const auto y = render (v, before, notes, 48000.0, { { 72000, with (before, knob, to) } });
                auto rougher = [] (const std::vector<float>& x) { return roughness (x, 1.5, 1.9) / std::max (roughness (x, 1.0, 1.5), roughness (x, 3.0, 3.5)); };
                const double ratio = std::max (rougher (y.l), rougher (y.r)), spike = std::max (spikiness (y.l, 1.45, 1.95), spikiness (y.r, 1.45, 1.95));
                std::printf ("  %s %.2f (%.1f)", v == V::particleVerb && knob == kGain ? (to > from ? "Gain up" : "Gain down") : move.name, ratio, spike);
                // a click: rougher than both steady states and confined to a few milliseconds; zipper noise: far rougher
                check (! (ratio > 2.0 && spike > 2.5) && ratio < 4.0, key (v) + ": " + move.name + " clicks (" + num (ratio) + ", spike " + num (spike) + ")");
            }
            std::printf ("\n");
        }

        // Particle Verb's Condition switch
        std::printf ("  particle_verb ");
        for (int from = 0; from < 3; ++from)
        {
            const int to = (from + 1) % 3;
            const Knobs before = with (knobsOf (models[V::particleVerb]), kCondition, (float) from);
            const auto y = render (V::particleVerb, before, notes, 48000.0, { { 72000, with (before, kCondition, (float) to) } });
            const double spike = std::max (spikiness (y.l, 1.45, 1.95), spikiness (y.r, 1.45, 1.95));
            std::printf ("  %s -> %s (%.1f)", fx::verbConditionNames[from], fx::verbConditionNames[to], spike);
            check (spike < 2.5, std::string ("particle_verb: switching from ") + fx::verbConditionNames[from] + " clicks");
        }
        std::printf ("\n");
    }

    //==========================================================================
    std::printf ("== one-sided input, DC at the input, and the tail time ==\n");
    {
        const auto riff = harness::makeInput (48000.0, 26.0);
        const std::vector<float> nothing (riff.size(), 0.0f);
        std::vector<float> offset (riff.size());
        for (size_t i = 0; i < offset.size(); ++i)
            offset[i] = i < 4 * 48000 ? 0.2f + riff[i] : 0.0f;

        for (int v = 0; v < 12; ++v)
        {
            const auto& m = models[(size_t) v];

            // the guitar on the left only: the reverb must still fill both sides
            const auto one = render (v, knobsOf (m), riff, 48000.0, {}, &nothing);
            const double balance = rmsDb ({ one.l, one.l }, 0.5, 4.0) - rmsDb ({ one.r, one.r }, 0.5, 4.0), corr = correlation (one, 0.5, 4.0);

            // 0.2 of DC under the riff: none of it may come out (mean of 3 .. 4 s against the RMS)
            const auto y = render (v, knobsOf (m), offset);
            double mean = 0.0;
            for (size_t i = 3 * 48000; i < 4 * 48000; ++i)
                mean += ((double) y.l[i] + y.r[i]) / 96000.0;
            const double dcDb = 20.0 * std::log10 (std::abs (mean) + 1.0e-12) - rmsDb (y, 3.0, 4.0);

            // the tail: how far the level has dropped getTailSeconds() after the playing stops
            V engine;
            engine.prepare (48000.0, 256);
            engine.setModel (v);
            engine.setParameters (knobsOf (m).data());
            const double tail = engine.getTailSeconds();
            const auto played = render (v, knobsOf (m), riff);
            const double drop = rmsDb (played, 3.9, 4.0) - rmsDb (played, 4.05 + tail, 4.15 + tail);

            std::printf ("  %-14s left only: L - R %+5.1f dB, correlation %5.2f; DC %6.1f dB; %5.2f s after the playing: %6.1f dB down\n", m.key, balance, corr, dcDb, tail, drop);
            check (finite (one) && std::abs (balance) < 6.0 && std::abs (corr) < 0.5, key (v) + ": a one-sided input should still give a wide reverb on both sides");
            check (dcDb < -30.0, key (v) + ": DC at the input comes out (" + num (dcDb, 1) + " dB)");
            check (drop > 70.0, key (v) + ": " + num (drop, 0) + " dB down after getTailSeconds(), expected about 100");
        }
    }

    //==========================================================================
    // the host's block size must not matter (smoothing and LFOs run on the engine's own 32-sample clock)
    {
        const auto riff = harness::makeInput (48000.0, 2.0);
        bool same = true;
        for (int v = 0; v < 12; ++v)
        {
            const auto a = render (v, knobsOf (models[(size_t) v]), riff), b = render (v, knobsOf (models[(size_t) v]), riff, 48000.0, {}, nullptr, 37);
            same = same && std::memcmp (a.l.data(), b.l.data(), a.l.size() * sizeof (float)) == 0 && std::memcmp (a.r.data(), b.r.data(), a.r.size() * sizeof (float)) == 0;
        }
        std::printf ("== blocks of 256 and of 37 samples give %s output ==\n", same ? "the same" : "DIFFERENT");
        check (same, "the output depends on the block size");
    }

    //==========================================================================
    std::printf ("== other sample rates: decay time at the default knobs, peak of the riff ==\n");
    for (int v = 0; v < 12; ++v)
    {
        std::printf ("  %-14s 48 kHz %5.2f s ", m_key (models, v), defaultRt[v]);
        for (double rate : { 44100.0, 96000.0, 192000.0 })
        {
            const double rt = rt60 (render (v, dry (v), impulseInput (16.0, rate), rate), rate);
            // (the riff with everything at its maximum: Decay and Tone, or Dwell, Hazard and Gain)
            const Knobs top = v == V::particleVerb ? Knobs { 100.0f, 2.0f, 100.0f, 50.0f } : with (with (knobsOf (models[(size_t) v]), kDecay, 100.0f), kTone, 100.0f);
            const auto y = render (v, top, harness::makeInput (rate, 6.0), rate);
            std::printf ("  %3.0f kHz %5.2f s, peak %.2f", rate / 1000.0, rt, peakOf (y));
            check (finite (y) && peakOf (y) < 2.0f, key (v) + " misbehaves at " + num (rate, 0) + " Hz");
            check (std::abs (rt / defaultRt[v] - 1.0) < 0.25, key (v) + ": decay time at " + num (rate, 0) + " Hz is " + num (rt) + " s, not " + num (defaultRt[v]) + " s");
        }
        std::printf ("\n");
    }

    if (verbose)
    {
        // level of the wet signal while the riff plays, and the program level that would put it at -6 dB
        const auto riff = harness::makeInput (48000.0, 12.0);
        std::vector<float> quiet (riff);
        for (auto& x : quiet)
            x *= 0.01f;
        for (int v = 0; v < 12; ++v)
        {
            const bool duck = v == V::ducking;
            const auto y = render (v, knobsOf (models[(size_t) v]), duck ? quiet : riff);
            const double level = rmsDb (y, 0.1, 4.0) - rmsDb ({ riff, riff }, 0.1, 4.0) + (duck ? 40.0 : 0.0);
            std::printf ("  %-14s level while playing %6.1f dB (level %.3f -> %.3f), tail 4.2 s %6.1f, 5 s %6.1f, 7 s %6.1f, 10 s %6.1f dB, peak %.2f\n", m_key (models, v),
                         level, fx::verb_detail::programs[v].level, fx::verb_detail::programs[v].level * std::pow (10.0, (-6.0 - level) / 20.0),
                         rmsDb (y, 4.2, 4.4), rmsDb (y, 5.0, 5.2), rmsDb (y, 7.0, 7.2), rmsDb (y, 10.0, 10.2), peakOf (y));
        }
    }

    std::printf ("%s\n", failed == 0 ? "ALL CHECKS PASSED" : (std::to_string (failed) + " CHECK(S) FAILED").c_str());
    return failed == 0 ? 0 : 1;
}
