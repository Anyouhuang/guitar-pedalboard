// Standalone test of the speaker-cabinet and microphone engine (see Harness.h).
//
// Beyond harness::run this measures what the engine promises: the magnitude response of every cabinet and every
// microphone (from the impulse response, so the comb, the crossfades and the level stage are all included),
// that "412 Classic" is the old CabSim chain sample for sample, what the Low Cut, Res Level, Thump, Decay and
// E.R. knobs do, that moving knobs does not click, and that everything holds from 44.1 to 192 kHz.
#include "Harness.h"
#include "../../Source/DSP/fx/Cab.h"

namespace
{
using harness::Knobs;
using fx::CabFx;

constexpr double baseRate = harness::testRate;
constexpr int numCabs = fx::cab_detail::numCabs, numMics = fx::cab_detail::numMics;

// third-octave test frequencies, 40 Hz to 12 kHz
constexpr double testHz[] = { 40, 60, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
                              2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12000 };
constexpr int numTestHz = (int) (sizeof (testHz) / sizeof (testHz[0]));

enum { kMic = 0, kRoom, kLowCut, kRes, kThump, kDecay };

Knobs withKnob (Knobs k, int index, float value) { k[(size_t) index] = value; return k; }

/** The engine's output (left channel) for `input`, with the knobs fixed or changing on a schedule. */
std::vector<float> run (int variant, const Knobs& knobs, const std::vector<float>& input, double fs = baseRate,
                        const std::vector<harness::Step>& schedule = {})
{
    CabFx cab;
    cab.prepare (fs, harness::blockSize);
    const auto stereo = harness::render (cab, variant, input, knobs, schedule);
    std::vector<float> mono (input.size());
    for (size_t i = 0; i < input.size(); ++i)
        mono[i] = stereo[i * 2];
    return mono;
}

std::vector<float> impulseResponse (int variant, const Knobs& knobs, double fs = baseRate, double seconds = 0.5)
{
    std::vector<float> impulse ((size_t) (seconds * fs), 0.0f);
    impulse[0] = 1.0f;
    return run (variant, knobs, impulse, fs);
}

/** Magnitude in dB of an impulse response at one frequency (a one-bin Fourier transform). */
double levelDb (const std::vector<float>& h, double hz, double fs = baseRate)
{
    const double w = 2.0 * fx::pi * hz / fs, c = std::cos (w), s = std::sin (w);
    double re = 0.0, im = 0.0, cr = 1.0, ci = 0.0;
    for (float v : h)
    {
        re += v * cr;
        im -= v * ci;
        const double next = cr * c - ci * s;
        ci = cr * s + ci * c;
        cr = next;
    }
    return 10.0 * std::log10 (std::max (1.0e-30, re * re + im * im));
}

std::vector<double> curve (const std::vector<float>& h, double fs = baseRate)
{
    std::vector<double> out;
    for (double hz : testHz)
        out.push_back (levelDb (h, hz, fs));
    return out;
}

/** Loudness of a response for a guitar-like spectrum: equal power per octave from 70 Hz to 1 kHz, falling
    3 dB/octave from there to 10 kHz. This is what the cabinets' and microphones' trims are matched with. */
double loudnessDb (const std::vector<float>& h)
{
    double sum = 0.0, weights = 0.0;
    for (int k = 0; k <= 43; ++k)
    {
        const double hz = 70.0 * std::pow (2.0, k / 6.0), weight = hz <= 1000.0 ? 1.0 : 1000.0 / hz;
        sum += weight * std::pow (10.0, levelDb (h, hz) / 10.0);
        weights += weight;
    }
    return 10.0 * std::log10 (sum / weights);
}

double energy (const std::vector<float>& x, double fromSeconds, double toSeconds, double fs = baseRate)
{
    double sum = 0.0;
    for (size_t i = (size_t) (fromSeconds * fs); i < std::min (x.size(), (size_t) (toSeconds * fs)); ++i)
        sum += (double) x[i] * x[i];
    return sum;
}

/** The chain of Source/DSP/CabSim.h, built here from its numbers. */
struct ClassicReference
{
    fx::Biquad filters[6];

    void prepare (double fs)
    {
        filters[0].setHighPass (fs, 75.0, 0.707);
        filters[1].setPeak     (fs, 120.0, 1.4, 3.0);
        filters[2].setPeak     (fs, 450.0, 1.0, -3.5);
        filters[3].setPeak     (fs, 2300.0, 1.3, 4.0);
        filters[4].setLowPass  (fs, 5000.0, 0.6);
        filters[5].setLowPass  (fs, 6500.0, 0.9);
        for (auto& f : filters)
            f.reset();
    }

    float process (float x) noexcept
    {
        for (auto& f : filters)
            x = f.process (x);
        return x;
    }

    double magnitudeDb (double hz, double fs) const
    {
        double gain = 1.0;
        for (const auto& f : filters)
            gain *= f.getMagnitude (hz, fs);
        return 20.0 * std::log10 (gain);
    }
};

void printHeader (const char* title)
{
    std::printf ("\n  %-20s", title);
    for (double hz : testHz)
    {
        char text[16];
        if (hz < 1000.0) std::snprintf (text, sizeof (text), "%.0f", hz);
        else             std::snprintf (text, sizeof (text), "%gk", hz / 1000.0);
        std::printf ("%6s", text);
    }
    std::printf ("\n");
}

void printCurve (const char* name, const std::vector<double>& c, double reference = 0.0)
{
    std::printf ("  %-20s", name);
    for (double v : c)
        std::printf ("%6.1f", v - reference);
    std::printf ("\n");
}

/** What a curve looks like once its overall level is taken out (mean over 60 Hz ... 8 kHz). */
std::vector<double> shape (const std::vector<double>& c)
{
    double mean = 0.0;
    int count = 0;
    for (int i = 0; i < numTestHz; ++i)
        if (testHz[i] >= 60.0 && testHz[i] <= 8000.0) { mean += c[(size_t) i]; ++count; }
    auto out = c;
    for (auto& v : out)
        v -= mean / count;
    return out;
}

struct Difference { double largest = 0.0, rms = 0.0; };

Difference difference (const std::vector<double>& a, const std::vector<double>& b, double fromHz = 60.0, double toHz = 8000.0)
{
    Difference d;
    int count = 0;
    for (int i = 0; i < numTestHz; ++i)
        if (testHz[i] >= fromHz && testHz[i] <= toHz)
        {
            const double e = std::abs (a[(size_t) i] - b[(size_t) i]);
            d.largest = std::max (d.largest, e);
            d.rms += e * e;
            ++count;
        }
    d.rms = std::sqrt (d.rms / count);
    return d;
}

int indexOfHz (double hz)
{
    for (int i = 0; i < numTestHz; ++i)
        if (testHz[i] == hz)
            return i;
    return -1;
}
} // namespace

int main (int argc, char* argv[])
{
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "test_output/standalone/cab";
    const auto models = fx::cabModels();
    int failures = harness::run<CabFx> ("cab", models, outDir);

    auto check = [&] (bool ok, const std::string& what)
    {
        if (! ok)
        {
            ++failures;
            std::printf ("    FAIL: %s\n", what.c_str());
        }
    };
    auto at = [] (const std::vector<double>& c, double hz) { return c[(size_t) indexOfHz (hz)]; };
    auto name = [&] (int cab) { return std::string (models[(size_t) cab].name); };
    auto text = [] (double v) { char b[32]; std::snprintf (b, sizeof (b), "%.2f", v); return std::string (b); };

    std::vector<Knobs> defaults;
    for (const auto& m : models)
        defaults.push_back (harness::defaultKnobs (m));

    check ((int) models.size() == numCabs && numCabs == (int) CabFx::numVariants, "22 cabinets");
    for (const auto& m : models)
        check (m.knobs.size() == 6 && (int) m.knobs[0].max == numMics - 1, std::string (m.key) + " has the six cab knobs and 14 microphones");

    //==============================================================================
    std::printf ("\n== 412 Classic = the old CabSim chain ==\n");
    {
        const int classic = CabFx::classic412;
        const Knobs k = defaults[(size_t) classic];

        for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            const auto input = harness::makeInput (fs, 2.0);
            const auto out = run (classic, k, input, fs);
            ClassicReference reference;
            reference.prepare (fs);
            size_t different = 0;
            float worst = 0.0f;
            for (size_t i = 0; i < input.size(); ++i)
            {
                const float expected = reference.process (input[i]);
                different += out[i] != expected ? 1 : 0;
                worst = std::max (worst, std::abs (out[i] - expected));
            }
            std::printf ("  %6.0f Hz: %zu of %zu samples differ from CabSim (largest difference %g)\n", fs, different, input.size(), (double) worst);
            check (different == 0, "412 Classic at its defaults is CabSim sample for sample at " + text (fs) + " Hz");
        }

        ClassicReference reference;
        reference.prepare (baseRate);
        const auto measured = curve (impulseResponse (classic, k));
        std::vector<double> expected;
        double worst = 0.0;
        for (int i = 0; i < numTestHz; ++i)
        {
            expected.push_back (reference.magnitudeDb (testHz[i], baseRate));
            worst = std::max (worst, std::abs (measured[(size_t) i] - expected[(size_t) i]));
        }
        printHeader ("dB at Hz");
        printCurve ("CabSim (computed)", expected);
        printCurve ("412 Classic", measured);
        std::printf ("  largest difference %.4f dB\n", worst);
        check (worst < 0.1, "412 Classic's response matches CabSim's within 0.1 dB (" + text (worst) + ")");
    }

    //==============================================================================
    // Every cabinet through the first microphone (57 On Xs), no early reflections, the other knobs at their defaults.
    std::printf ("\n== cabinets (Mic = 57 On Xs, E.R. = 0): response in dB relative to 1 kHz ==\n");
    std::vector<std::vector<double>> cabCurves, cabShapes;
    struct Traits { double lowEdge, highEdge, peakHz, topDb, at12k, slope, bass60, scoop; };
    std::vector<Traits> traits;
    {
        printHeader ("cabinet");
        for (int cab = 0; cab < numCabs; ++cab)
        {
            const auto h = impulseResponse (cab, withKnob (withKnob (defaults[(size_t) cab], kMic, 0.0f), kRoom, 0.0f));
            cabCurves.push_back (curve (h));
            cabShapes.push_back (shape (cabCurves.back()));
            printCurve (models[(size_t) cab].name, cabCurves.back(), at (cabCurves.back(), 1000.0));

            // a finer look (1/12 octave, 30 Hz ... 20 kHz) for the edges and the break-up peak
            std::vector<double> hz, db;
            for (int k = 0; k <= 112; ++k)
            {
                hz.push_back (30.0 * std::pow (2.0, k / 12.0));
                db.push_back (levelDb (h, hz.back()));
            }
            auto mean = [&] (double from, double to)
            {
                double sum = 0.0; int count = 0;
                for (size_t i = 0; i < hz.size(); ++i)
                    if (hz[i] >= from && hz[i] <= to) { sum += db[i]; ++count; }
                return sum / count;
            };
            auto interpolate = [&] (double f)
            {
                for (size_t i = 1; i < hz.size(); ++i)
                    if (hz[i] >= f)
                        return db[i - 1] + (db[i] - db[i - 1]) * std::log (f / hz[i - 1]) / std::log (hz[i] / hz[i - 1]);
                return db.back();
            };

            Traits t {};
            const double mids = mean (200.0, 1000.0);
            t.topDb = -1000.0;
            for (size_t i = 0; i < hz.size(); ++i)
                if (hz[i] >= 1200.0 && hz[i] <= 7000.0 && db[i] > t.topDb) { t.topDb = db[i]; t.peakHz = hz[i]; }
            auto crossing = [&] (size_t inside, size_t outside, double level) // where the curve passes `level` between two grid points
            {
                return hz[inside] * std::pow (hz[outside] / hz[inside], (db[inside] - level) / (db[inside] - db[outside]));
            };
            for (size_t i = 1; i < hz.size(); ++i)      // the lowest frequency within 6 dB of the mids
                if (db[i] >= mids - 6.0) { t.lowEdge = crossing (i, i - 1, mids - 6.0); break; }
            for (size_t i = hz.size() - 1; i-- > 0;)    // the highest frequency within 12 dB of the break-up peak
                if (db[i] >= t.topDb - 12.0) { t.highEdge = crossing (i, i + 1, t.topDb - 12.0); break; }
            t.at12k = levelDb (h, 12000.0) - t.topDb;
            t.slope = interpolate (2.0 * t.highEdge) - interpolate (t.highEdge); // dB over the octave above the edge
            t.bass60 = levelDb (h, 60.0) - mids;
            t.scoop = mean (400.0, 900.0) - 0.5 * (mean (90.0, 200.0) + mean (2000.0, 4000.0));
            traits.push_back (t);
        }

        std::printf ("\n  %-20s  low edge  break-up peak  high edge  octave above it   12 kHz   60 Hz   mid scoop\n", "cabinet");
        for (int cab = 0; cab < numCabs; ++cab)
        {
            const auto& t = traits[(size_t) cab];
            std::printf ("  %-20s %6.0f Hz   %6.0f Hz     %6.0f Hz     %6.1f dB    %6.1f dB %6.1f dB %6.1f dB\n",
                         models[(size_t) cab].name, t.lowEdge, t.peakHz, t.highEdge, t.slope, t.at12k, t.bass60, t.scoop);
        }

        const bool isBass[numCabs] = { false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                                       true, false, false, true, true, false };
        for (int cab = 0; cab < numCabs; ++cab)
        {
            const auto& t = traits[(size_t) cab];
            // steep roll-off above the speaker's range: at least 18 dB over the octave above its upper edge,
            // and far down by 12 kHz (the horn of the 410 Rhino reaches furthest)
            check (t.slope <= -18.0, name (cab) + " falls " + text (-t.slope) + " dB in the octave above its upper edge (want 18 or more)");
            check (t.at12k <= (cab == CabFx::rhino410 ? -20.0 : -30.0), name (cab) + " is only " + text (-t.at12k) + " dB down at 12 kHz");
            check (t.highEdge >= 3000.0 && t.highEdge <= 9500.0, name (cab) + ": upper edge at " + text (t.highEdge) + " Hz");

            // bass cabinets reach lower than every guitar cabinet
            if (isBass[cab])
                for (int other = 0; other < numCabs; ++other)
                    if (! isBass[other])
                        check (t.lowEdge < traits[(size_t) other].lowEdge && t.bass60 > traits[(size_t) other].bass60,
                               name (cab) + " reaches lower than " + name (other));
        }

        auto edgeBelow = [&] (int a, int b)
        {
            check (traits[(size_t) a].highEdge < traits[(size_t) b].highEdge, name (a) + " rolls off earlier than " + name (b));
        };
        edgeBelow (CabFx::greenback412, CabFx::blackback412);      // G12M < G12H30 < G12T-75
        edgeBelow (CabFx::blackback412, CabFx::britT75412);
        edgeBelow (CabFx::treadV30412, CabFx::britT75412);         // Vintage 30 < G12T-75
        edgeBelow (CabFx::greenback412, CabFx::blueBell112);       // Greenback < Alnico Blue
        edgeBelow (CabFx::treadV30412, CabFx::blackfaceDouble212); // Vintage 30 < Jensen C12N
        edgeBelow (CabFx::bfLux112, CabFx::blackfaceDouble212);    // Oxford < Jensen
        edgeBelow (CabFx::fieldCoil112, CabFx::bfLux112);          // the 1939 field coil is the darkest 12"
        edgeBelow (CabFx::superO6x9, CabFx::greenback412);
        edgeBelow (CabFx::svBeast810, CabFx::rhino410);            // the Rhino has a horn
        for (int cab = 0; cab < numCabs; ++cab)
        {
            if (cab != CabFx::flipTop115)
                edgeBelow (CabFx::flipTop115, cab);                // a 15" without a tweeter is the darkest of all
            if (cab != CabFx::jazzRivet212 && ! isBass[cab])
                edgeBelow (cab, CabFx::jazzRivet212);              // the brightest guitar cabinet
        }

        // the small speakers have no bass; the big closed and ported boxes have the most
        for (int cab = 0; cab < numCabs; ++cab)
            if (cab != CabFx::superO6x9 && cab != CabFx::smallTweed108)
            {
                check (traits[(size_t) cab].lowEdge < traits[(size_t) CabFx::superO6x9].lowEdge, "6x9 Super O has less bass than " + name (cab));
                check (traits[(size_t) cab].lowEdge < traits[(size_t) CabFx::smallTweed108].lowEdge, "108 Small Tweed has less bass than " + name (cab));
            }
        check (traits[(size_t) CabFx::uber412].bass60 > traits[(size_t) CabFx::greenback412].bass60, "412 Uber (oversized) has more deep bass than 412 Greenback 25");
        check (traits[(size_t) CabFx::phdPorted212].lowEdge < traits[(size_t) CabFx::silverBell212].lowEdge, "the ported 2x12 reaches lower than the open-back 2x12");
        check (traits[(size_t) CabFx::bfLux112].bass60 < traits[(size_t) CabFx::greenback412].bass60, "a small open-back combo has less deep bass than a closed 4x12");

        // mids: the G12T-75 is the scooped Marshall speaker, the Supro and the field coil are mid-heavy
        check (traits[(size_t) CabFx::britT75412].scoop < traits[(size_t) CabFx::greenback412].scoop - 2.0, "412 Brit T-75 is more scooped than 412 Greenback 25");
        check (traits[(size_t) CabFx::superO6x9].scoop > 0.0 && traits[(size_t) CabFx::fieldCoil112].scoop > traits[(size_t) CabFx::bfLux112].scoop + 2.0,
               "6x9 Super O and 112 Field Coil are mid-heavy");
        check (traits[(size_t) CabFx::britT75412].peakHz > traits[(size_t) CabFx::treadV30412].peakHz, "the T-75's break-up peak is above the Vintage 30's");
        check (traits[(size_t) CabFx::treadV30412].peakHz > 1900.0 && traits[(size_t) CabFx::treadV30412].peakHz < 2800.0, "the Vintage 30's spike is near 2.3 kHz");

        // every pair of cabinets must differ in shape (levels taken out), 60 Hz ... 8 kHz
        Difference closest { 1000.0, 1000.0 };
        int closestA = 0, closestB = 0;
        for (int a = 0; a < numCabs; ++a)
            for (int b = a + 1; b < numCabs; ++b)
            {
                const auto d = difference (cabShapes[(size_t) a], cabShapes[(size_t) b]);
                if (d.rms < closest.rms) { closest = d; closestA = a; closestB = b; }
                check (d.largest >= 3.0 && d.rms >= 1.0, name (a) + " and " + name (b) + " are too alike (largest difference "
                                                           + text (d.largest) + " dB, rms " + text (d.rms) + " dB)");
            }
        std::printf ("  closest pair: %s / %s, largest difference %.1f dB, rms %.1f dB\n",
                     models[(size_t) closestA].name, models[(size_t) closestB].name, closest.largest, closest.rms);
    }

    //==============================================================================
    // Every microphone on one cabinet, as the difference to the 57 On Xs (with the loudness trims: a microphone
    // with a lot of bass or presence is turned down a little, so its mids come out below the 57's).
    std::printf ("\n== microphones on 412 Greenback 25 (E.R. = 0): dB relative to 57 On Xs ==\n");
    {
        const int cab = CabFx::greenback412;
        const Knobs base = withKnob (defaults[(size_t) cab], kRoom, 0.0f);
        const Knobs classicBase = defaults[(size_t) CabFx::classic412];
        std::vector<std::vector<double>> relative, absolute;
        std::vector<double> first, firstClassic;

        // the 57's own response, from the numbers in Cab.h (the engine has no "no microphone" setting to measure it with)
        std::vector<double> mic57;
        for (double hz : testHz)
        {
            double gain = 1.0;
            for (const auto& s : fx::cab_detail::micDefs[0].stages)
                if (s.type != fx::cab_detail::unused)
                {
                    fx::Biquad b;
                    fx::cab_detail::setStage (b, baseRate, s, 1.0);
                    gain *= b.getMagnitude (hz, baseRate);
                }
            mic57.push_back (20.0 * std::log10 (gain));
        }

        printHeader ("microphone");
        double worstClassic = 0.0;
        for (int mic = 0; mic < numMics; ++mic)
        {
            const auto c = curve (impulseResponse (cab, withKnob (base, kMic, (float) mic)));
            const auto onClassic = curve (impulseResponse (CabFx::classic412, withKnob (classicBase, kMic, (float) mic)));
            if (mic == 0) { first = c; firstClassic = onClassic; }

            std::vector<double> rel, abs;
            for (int i = 0; i < numTestHz; ++i)
            {
                rel.push_back (c[(size_t) i] - first[(size_t) i]);
                abs.push_back (rel.back() + mic57[(size_t) i]);
                worstClassic = std::max (worstClassic, std::abs (rel.back() - (onClassic[(size_t) i] - firstClassic[(size_t) i])));
            }
            relative.push_back (rel);
            absolute.push_back (abs);
            printCurve (fx::cabMicNames[mic], rel);
        }

        std::printf ("\n  the microphones' own responses (the above plus the 57's curve from Cab.h), dB:\n");
        printHeader ("microphone");
        for (int mic = 0; mic < numMics; ++mic)
            printCurve (fx::cabMicNames[mic], absolute[(size_t) mic]);

        std::printf ("  on 412 Classic the microphones differ from the 57 On Xs by the same curves within %.3f dB\n", worstClassic);
        check (worstClassic < 0.1, "the Mic knob does the same on 412 Classic as on the other cabinets, relative to the 57 On Xs");

        Difference closest { 1000.0, 1000.0 };
        int closestA = 0, closestB = 0;
        for (int a = 0; a < numMics; ++a)
            for (int b = a + 1; b < numMics; ++b)
            {
                const auto d = difference (shape (relative[(size_t) a]), shape (relative[(size_t) b]), 40.0, 12000.0);
                if (d.largest < closest.largest) { closest = d; closestA = a; closestB = b; }
                check (d.largest >= 2.0 && d.rms >= 0.7, std::string (fx::cabMicNames[a]) + " and " + fx::cabMicNames[b] + " are too alike (largest difference "
                                                           + text (d.largest) + " dB, rms " + text (d.rms) + " dB)");
            }
        std::printf ("  closest pair: %s / %s, largest difference %.1f dB, rms %.1f dB\n",
                     fx::cabMicNames[closestA], fx::cabMicNames[closestB], closest.largest, closest.rms);

        enum { m57On = 0, m57Off, m409, m421, m4038, m121, m67, m87, m12, m112, m20, m7, m40, m47 };
        // the microphone's own response, relative to its 1 kHz
        auto own = [&] (int mic, double hz) { return at (absolute[(size_t) mic], hz) - at (absolute[(size_t) mic], 1000.0); };
        auto spread = [&] (int mic)
        {
            double low = 1000.0, high = -1000.0;
            for (int i = 0; i < numTestHz; ++i)
                if (testHz[i] >= 80.0 && testHz[i] <= 10000.0)
                {
                    low = std::min (low, absolute[(size_t) mic][(size_t) i]);
                    high = std::max (high, absolute[(size_t) mic][(size_t) i]);
                }
            return high - low;
        };

        std::printf ("  spread 80 Hz ... 10 kHz:");
        for (int mic = 0; mic < numMics; ++mic)
            std::printf (" %s %.1f%s", fx::cabMicNames[mic], spread (mic), mic + 1 < numMics ? "," : " dB\n");

        check (own (m57On, 6300.0) >= 3.5 && own (m57On, 100.0) <= -1.5, "57 On Xs: presence peak near 6 kHz and rolled-off lows");
        // ...and compared with the 57 On Xs (levels matched at 1 kHz)
        auto versus57 = [&] (int mic, double hz) { return at (relative[(size_t) mic], hz) - at (relative[(size_t) mic], 1000.0); };
        check (versus57 (m57Off, 6300.0) <= -4.0 && versus57 (m57Off, 8000.0) <= -4.0 && versus57 (m57Off, 100.0) >= 0.5,
               "57 Off Xs is duller than on axis, with a little more bass");
        check (own (m421, 400.0) <= -2.0 && own (m421, 4000.0) >= 3.5 && own (m421, 80.0) >= 1.0, "421 Dyn: scooped low mids, strong presence, solid lows");
        for (int ribbon : { m4038, m121 })
            check (own (ribbon, 100.0) >= 3.0 && own (ribbon, 8000.0) <= -2.0 && versus57 (ribbon, 100.0) >= 5.0 && versus57 (ribbon, 6300.0) <= -5.0,
                   std::string (fx::cabMicNames[ribbon]) + ": proximity bass and a dark top");
        check (own (m4038, 8000.0) < own (m121, 8000.0) - 1.0, "the Coles ribbon is darker than the Royer");
        for (int condenser : { m67, m87 })
        {
            check (spread (condenser) <= 4.5, std::string (fx::cabMicNames[condenser]) + " is flat (spread " + text (spread (condenser)) + " dB)");
            for (int dynamic : { m57On, m421, m12, m112 })
                check (spread (condenser) < spread (dynamic) - 2.0, std::string (fx::cabMicNames[condenser]) + " is flatter than " + fx::cabMicNames[dynamic]);
        }
        check (own (m87, 10000.0) > 1.5 && own (m87, 10000.0) > own (m67, 10000.0) + 2.0, "87 Cond has the lifted, extended top, 67 Cond the soft one");
        check (spread (m20) <= 4.0, "20 Dyn (RE20) is the flat dynamic (spread " + text (spread (m20)) + " dB)");
        for (int kick : { m12, m112 })
            check (std::max (own (kick, 80.0), own (kick, 100.0)) - std::min (own (kick, 400.0), own (kick, 500.0)) >= 6.0,
                   std::string (fx::cabMicNames[kick]) + ": kick-drum microphone, bass bump and scooped low mids");
        check (own (m112, 4000.0) >= 5.0 && own (m112, 4000.0) > own (m12, 4000.0) + 2.0, "112 Dyn has the strong click peak at 4 kHz");
        check (own (m47, 100.0) >= 2.0 && own (m47, 4000.0) >= 2.0, "47 Cond: rich lows and upper-mid presence");
        check (own (m40, 5000.0) >= 2.5 && own (m40, 10000.0) >= 2.0 && own (m40, 60.0) >= 1.0, "40 Dyn: deep lows, a broad presence rise and an open top");
        check (own (m7, 100.0) >= 1.5 && own (m7, 12000.0) < own (m7, 6300.0) - 3.0, "7 Dyn: warm lows, soft top");
        check (own (m409, 315.0) >= 1.0 && own (m409, 4000.0) >= 2.0 && own (m409, 60.0) < own (m409, 315.0) - 1.5 && own (m409, 4000.0) < own (m421, 4000.0),
               "409 Dyn: warm low mids, a smoother presence lift than the 421");
    }

    //==============================================================================
    std::printf ("\n== knobs ==\n");
    {
        const int cab = CabFx::greenback412;
        const Knobs base = withKnob (withKnob (defaults[(size_t) cab], kMic, 0.0f), kRoom, 0.0f);
        const auto& def = fx::cab_detail::cabDefs[cab];

        // ---- Low Cut: a 12 dB/octave high-pass; 20 Hz = off
        {
            const auto off = impulseResponse (cab, withKnob (base, kLowCut, 20.0f));
            std::printf ("  Low Cut (dB relative to off)     at 50 Hz  100 Hz  200 Hz  500 Hz   2 kHz\n");
            double previous100 = 1.0;
            for (float cut : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f })
            {
                const auto h = impulseResponse (cab, withKnob (base, kLowCut, cut));
                auto rel = [&] (double hz) { return levelDb (h, hz) - levelDb (off, hz); };
                std::printf ("    Low Cut %3.0f Hz:                  %6.1f  %6.1f  %6.1f  %6.1f  %6.1f\n", cut, rel (50), rel (100), rel (200), rel (500), rel (2000));
                if (cut > 20.0f)
                {
                    check (std::abs (rel (cut) + 3.0) < 0.3, "Low Cut " + text (cut) + " Hz is 3 dB down at that frequency (" + text (rel (cut)) + ")");
                    check (std::abs (rel (cut / 2.0) + 12.3) < 0.6, "Low Cut " + text (cut) + " Hz falls at 12 dB/octave (" + text (rel (cut / 2.0)) + " an octave below)");
                    check (std::abs (rel (cut * 8.0)) < 0.2, "Low Cut " + text (cut) + " Hz leaves the range above it alone");
                }
                check (rel (100) < previous100, "Low Cut: a higher setting cuts more at 100 Hz");
                previous100 = rel (100) + 1.0e-9;
            }
            // off really is off: 412 Classic has no other high-pass, so with Low Cut at 20 Hz it passes 5 Hz
            const Knobs classicOff = withKnob (defaults[(size_t) CabFx::classic412], kLowCut, 20.0f);
            const auto classicDc = impulseResponse (CabFx::classic412, classicOff, baseRate, 2.0);
            check (levelDb (classicDc, 5.0) > -1.0, "Low Cut at 20 Hz is off (412 Classic then passes 5 Hz: " + text (levelDb (classicDc, 5.0)) + " dB)");
            check (levelDb (impulseResponse (CabFx::classic412, defaults[(size_t) CabFx::classic412]), 5.0) < -40.0, "Low Cut at 75 Hz removes 5 Hz");
        }

        // ---- Res Level, Thump, Decay: the speaker's bass resonance
        {
            auto knobs = [&] (float res, float thump, float decay) { return withKnob (withKnob (withKnob (base, kRes, res), kThump, thump), kDecay, decay); };
            const auto none = impulseResponse (cab, knobs (0.0f, 50.0f, 50.0f));

            // at Res Level 0 Thump and Decay do nothing at all
            bool identical = true;
            for (float thump : { 0.0f, 100.0f })
                for (float decay : { 0.0f, 100.0f })
                    identical = identical && impulseResponse (cab, knobs (0.0f, thump, decay)) == none;
            check (identical, "Thump and Decay do nothing when Res Level is 0 (identical output)");

            std::printf ("  resonance of %s at %.0f Hz (dB relative to Res Level 0)   overall level   peak at the resonance   half an octave off   1 kHz\n", models[(size_t) cab].name, def.resHz);
            auto report = [&] (float res, float thump, float decay)
            {
                const auto h = impulseResponse (cab, knobs (res, thump, decay));
                auto change = [&] (double hz) { return levelDb (h, hz) - levelDb (none, hz); };
                struct R { double level, peak, side, mid; } r;
                r.level = change (8000.0);                    // far above the resonance: the overall level
                r.peak  = change (def.resHz) - r.level;       // the peak itself, with the level taken out
                r.side  = 0.5 * (change (def.resHz * 1.414) + change (def.resHz / 1.414)) - r.level;
                r.mid   = change (1000.0);
                std::printf ("    Res Level %3.0f  Thump %3.0f  Decay %3.0f:                          %6.1f           %6.1f                %6.1f          %6.1f\n",
                             res, thump, decay, r.level, r.peak, r.side, r.mid);
                return r;
            };

            const auto r50 = report (50.0f, 50.0f, 50.0f), r100 = report (100.0f, 50.0f, 50.0f), r25 = report (25.0f, 50.0f, 50.0f);
            check (std::abs (r50.peak - def.resDb) < 0.3, "Res Level 50 / Thump 50 gives the cabinet's own resonance peak");
            check (std::abs (r100.peak - 2.0 * def.resDb) < 0.4 && std::abs (r25.peak - 0.5 * def.resDb) < 0.3, "Res Level scales the resonance peak");
            check (std::abs (r100.level - 3.0) < 0.1 && std::abs (r50.level - 1.5) < 0.1, "Res Level raises the overall level (3 dB over its range)");

            const auto t0 = report (50.0f, 0.0f, 50.0f), t100 = report (50.0f, 100.0f, 50.0f);
            check (std::abs (t0.peak) < 0.1 && std::abs (t100.peak - 2.0 * def.resDb) < 0.4, "Thump sets the height of the low resonance (none at 0, double at 100)");
            check (std::abs (t0.mid - r50.level) < 0.1 && std::abs (t100.mid - r50.level) < 0.1 && std::abs (t100.level - r50.level) < 0.01,
                   "Thump leaves the level at 1 kHz and above alone");

            const auto d0 = report (100.0f, 100.0f, 0.0f), d100 = report (100.0f, 100.0f, 100.0f);
            check (std::abs (d0.peak - d100.peak) < 0.2 && std::abs (d0.level - d100.level) < 0.1, "Decay leaves the height of the resonance and the level alone");
            check (d100.side < d0.side - 4.0, "Decay narrows the resonance (higher Q)");

            // ...and a narrower resonance rings on for longer: the energy left in the impulse response after 25 ms
            const double tight = energy (impulseResponse (cab, knobs (100.0f, 100.0f, 0.0f)), 0.025, 0.5);
            const double medium = energy (impulseResponse (cab, knobs (100.0f, 100.0f, 50.0f)), 0.025, 0.5);
            const double loose = energy (impulseResponse (cab, knobs (100.0f, 100.0f, 100.0f)), 0.025, 0.5);
            std::printf ("    ringing after 25 ms: Decay 0 %.1f dB, Decay 50 %.1f dB, Decay 100 %.1f dB (re Decay 0)\n",
                         0.0, 10.0 * std::log10 (medium / tight), 10.0 * std::log10 (loose / tight));
            check (medium > 10.0 * tight && loose > 10.0 * medium, "Decay: tight to loose cone (longer ringing)");
        }

        // ---- E.R.: nothing at 0, short reflections above
        {
            const auto dry = impulseResponse (cab, withKnob (base, kRoom, 0.0f));
            auto reflections = [&] (float amount)
            {
                auto h = impulseResponse (cab, withKnob (base, kRoom, amount));
                for (size_t i = 0; i < h.size(); ++i)
                    h[i] -= dry[i];
                return h;
            };
            const auto r1 = reflections (1.0f), r50 = reflections (50.0f), r100 = reflections (100.0f);
            const double direct = energy (dry, 0.0, 0.5);
            const double e1 = energy (r1, 0.0, 0.5), e50 = energy (r50, 0.0, 0.5), e100 = energy (r100, 0.0, 0.5);
            std::printf ("  E.R.: reflections relative to the direct sound: 1 %% %.1f dB, 50 %% %.1f dB, 100 %% %.1f dB\n",
                         10.0 * std::log10 (e1 / direct), 10.0 * std::log10 (e50 / direct), 10.0 * std::log10 (e100 / direct));
            std::printf ("        at 100 %%: before 5 ms %.2g, 5-45 ms %.4g, after 70 ms %.2g (energy)\n",
                         energy (r100, 0.0, 0.005), energy (r100, 0.005, 0.045), energy (r100, 0.07, 0.5));

            // at 0 there is nothing after the cabinet's own short ringing; with E.R. up there is
            const double lateDry = energy (dry, 0.03, 0.5) / direct, lateRoom = energy (impulseResponse (cab, withKnob (base, kRoom, 50.0f)), 0.03, 0.5) / direct;
            std::printf ("        energy arriving after 30 ms: E.R. 0 %.1f dB, E.R. 50 %.1f dB (re the whole response)\n",
                         10.0 * std::log10 (lateDry), 10.0 * std::log10 (lateRoom));
            check (lateDry < 1.0e-5 && lateRoom > 100.0 * lateDry, "E.R. at 0 adds nothing: no sound arrives late");

            check (energy (r100, 0.0, 0.005) == 0.0, "E.R. leaves the direct sound alone (nothing before the first reflection at 5.3 ms)");
            check (energy (r100, 0.005, 0.045) > 0.9 * e100, "E.R.: the reflections arrive between 5 and 45 ms");
            check (energy (r100, 0.07, 0.5) < 1.0e-4 * e100, "E.R.: the reflections are short (over after 70 ms)");
            check (10.0 * std::log10 (e100 / direct) > -12.0 && 10.0 * std::log10 (e100 / direct) < -3.0, "E.R. at 100 % is a clearly audible room");
            check (std::abs (e100 / e50 - 4.0) < 0.2 && std::abs (e100 / e1 - 10000.0) < 500.0, "E.R. scales the reflections evenly down to nothing at 0");

            // six separate reflections: the envelope of the difference has six bursts at the tap times
            int found = 0;
            for (double ms : fx::cab_detail::roomTapMs)
                found += energy (r100, ms * 0.001 - 0.0002, ms * 0.001 + 0.003) > 0.02 * e100 ? 1 : 0;
            check (found == 6, "E.R.: six reflections at the tap times");
        }
    }

    //==============================================================================
    // No clicks or zipper noise when knobs move: a steady tone goes in, one knob jumps from one end to the other.
    // Any step in the output (a click, or the staircase of zipper noise) or kink shows up in its second
    // difference, which for the steady tone is tiny. What the change adds to it must stay 70 dB below the tone
    // (with the smoothing taken out of the engine, these moves give -2 to -65 dB on the low tone).
    std::printf ("\n== moving knobs: largest second difference during the change, as a ratio to the steady tone's and as\n"
                 "   the step it adds relative to the tone's amplitude ==\n");
    {
        const int cab = CabFx::treadV30412;
        struct Move { const char* what; int knob; float from, to; };
        const Move moves[] = { { "Mic 57 On > 4038 Rbn", kMic, 0.0f, 4.0f },    { "Mic 112 Dyn > 57 Off", kMic, 9.0f, 1.0f },
                               { "E.R. 0 > 100", kRoom, 0.0f, 100.0f },         { "E.R. 100 > 0", kRoom, 100.0f, 0.0f },
                               { "Low Cut 20 > 500", kLowCut, 20.0f, 500.0f },  { "Low Cut 500 > 20", kLowCut, 500.0f, 20.0f },
                               { "Low Cut 60 > 300", kLowCut, 60.0f, 300.0f },
                               { "Res Level 0 > 100", kRes, 0.0f, 100.0f },     { "Res Level 100 > 0", kRes, 100.0f, 0.0f },
                               { "Thump 0 > 100", kThump, 0.0f, 100.0f },       { "Thump 100 > 0", kThump, 100.0f, 0.0f },
                               { "Decay 0 > 100", kDecay, 0.0f, 100.0f },       { "Decay 100 > 0", kDecay, 100.0f, 0.0f } };

        for (double toneHz : { 104.0, 2300.0 })
        {
            std::vector<float> tone ((size_t) baseRate);
            for (size_t i = 0; i < tone.size(); ++i)
                tone[i] = 0.3f * (float) std::sin (2.0 * fx::pi * toneHz * (double) i / baseRate);

            for (const auto& move : moves)
            {
                Knobs from = withKnob (defaults[(size_t) cab], move.knob, move.from);
                if (move.knob == kThump || move.knob == kDecay)
                    from = withKnob (from, kRes, 100.0f);
                const Knobs to = withKnob (from, move.knob, move.to);
                const int changeAt = harness::blockSize * 94; // just after 0.5 s, on a block boundary
                const auto out = run (cab, from, tone, baseRate, { { changeAt, to } });

                auto curvature = [&] (double fromSeconds, double toSeconds)
                {
                    double largest = 0.0;
                    for (size_t i = std::max<size_t> (2, (size_t) (fromSeconds * baseRate)); i < (size_t) (toSeconds * baseRate); ++i)
                        largest = std::max (largest, std::abs ((double) out[i] - 2.0 * out[i - 1] + out[i - 2]));
                    return largest;
                };
                const double before = curvature (0.3, 0.5), during = curvature (0.5, 0.8), after = curvature (0.8, 1.0);
                double amplitude = 0.0;
                for (size_t i = (size_t) (0.3 * baseRate); i < out.size(); ++i)
                    amplitude = std::max (amplitude, (double) std::abs (out[i]));
                const double ratio = during / std::max (1.0e-7, std::max (before, after));
                const double added = std::max (1.0e-9, during - std::max (before, after)) / amplitude;
                std::printf ("  %5.0f Hz tone, %-22s %5.2f  %7.1f dB\n", toneHz, move.what, ratio, 20.0 * std::log10 (added));
                check (added < 3.16e-4, std::string (move.what) + " clicks or zippers on a " + text (toneHz) + " Hz tone ("
                                         + text (20.0 * std::log10 (added)) + " dB re the tone)");
            }
        }
    }

    //==============================================================================
    std::printf ("\n== loudness at the default knobs (guitar-like spectrum), dB relative to 412 Classic ==\n");
    {
        const double reference = loudnessDb (impulseResponse (CabFx::classic412, defaults[(size_t) CabFx::classic412]));
        for (int cab = 0; cab < numCabs; ++cab)
        {
            const double level = loudnessDb (impulseResponse (cab, defaults[(size_t) cab])) - reference;
            std::printf ("  %-20s %5.2f   (trim %5.1f dB, microphone %s)\n", models[(size_t) cab].name, level, fx::cab_detail::cabDefs[cab].trimDb,
                         fx::cabMicNames[(int) defaults[(size_t) cab][kMic]]);
            check (std::abs (level) < 0.5, name (cab) + " is " + text (level) + " dB louder than 412 Classic at its defaults");
        }

        std::printf ("  changing the microphone (average over the cabinets, dB relative to 57 On Xs):\n");
        for (int mic = 0; mic < numMics; ++mic)
        {
            double sum = 0.0, largest = 0.0;
            for (int cab = 0; cab < numCabs; ++cab)
            {
                const Knobs k = withKnob (defaults[(size_t) cab], kRoom, 0.0f);
                const double d = loudnessDb (impulseResponse (cab, withKnob (k, kMic, (float) mic))) - loudnessDb (impulseResponse (cab, withKnob (k, kMic, 0.0f)));
                sum += d;
                largest = std::max (largest, std::abs (d));
            }
            std::printf ("    %-10s %5.2f (largest %4.2f, trim %4.1f dB)\n", fx::cabMicNames[mic], sum / numCabs, largest, fx::cab_detail::micDefs[mic].trimDb);
            check (std::abs (sum / numCabs) < 0.3 && largest < 2.5, std::string (fx::cabMicNames[mic]) + " changes the loudness");
        }
    }

    //==============================================================================
    std::printf ("\n== sample rates ==\n");
    {
        double worst = 0.0;
        int worstCab = 0;
        for (int cab = 0; cab < numCabs; ++cab)
        {
            const Knobs k = withKnob (withKnob (defaults[(size_t) cab], kMic, 0.0f), kRoom, 0.0f);
            for (double fs : { 44100.0, 96000.0, 192000.0 })
            {
                const auto h = impulseResponse (cab, k, fs);
                const double top = *std::max_element (cabCurves[(size_t) cab].begin(), cabCurves[(size_t) cab].end());
                for (int i = 0; i < numTestHz; ++i)
                    if (testHz[i] <= 5000.0 && cabCurves[(size_t) cab][(size_t) i] > top - 15.0) // where the cabinet is not far into its roll-off
                    {
                        const double d = std::abs (levelDb (h, testHz[i], fs) - cabCurves[(size_t) cab][(size_t) i]);
                        if (d > worst) { worst = d; worstCab = cab; }
                    }
            }

            // every knob at its extreme, at the highest rate
            const auto riff = harness::makeInput (192000.0, 1.5);
            for (float lowCut : { 20.0f, 500.0f })
                for (float decay : { 0.0f, 100.0f })
                {
                    Knobs extreme {};
                    extreme[kMic] = 9.0f; extreme[kRoom] = 100.0f; extreme[kLowCut] = lowCut; extreme[kRes] = 100.0f; extreme[kThump] = 100.0f; extreme[kDecay] = decay;
                    const auto s = harness::measure (run (cab, extreme, riff, 192000.0));
                    check (s.finite && s.peak < 8.0f, name (cab) + " misbehaves at 192 kHz with its knobs at the extremes (peak " + text (s.peak) + ")");
                }
        }
        std::printf ("  44.1 / 96 / 192 kHz vs 48 kHz, 40 Hz ... 5 kHz within 15 dB of the peak: largest difference %.2f dB (%s)\n", worst, models[(size_t) worstCab].name);
        check (worst < 1.0, "the cabinets sound the same at every sample rate (" + text (worst) + " dB)");
    }

    std::printf ("\n%s\n", failures == 0 ? "ALL CHECKS PASSED" : (std::to_string (failures) + " CHECK(S) FAILED").c_str());
    return failures == 0 ? 0 : 1;
}
