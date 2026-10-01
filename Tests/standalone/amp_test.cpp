// Standalone test of the amp engine (see Harness.h): harness::run for all 50 amps, then checks of what defines
// them: the tone stacks' frequency responses, how much each amp distorts and how that follows Drive, Master and the
// playing strength, what the power-amp knobs (Presence, Sag, Bias, Bias X, Hum, Power Amp) do, that all amps are
// about equally loud, that they do not alias, that knobs do not click and that the amps really differ.
#include "Harness.h"
#include "../../Source/DSP/fx/Amp.h"

namespace
{
using harness::Knobs;
using namespace fx::amp_detail;

enum Knob { kDrive = 0, kBass, kMid, kTreble, kPresence, kChVol, kMaster, kSag, kHum, kBias, kBiasX, kPowerAmp };
constexpr double baseRate = harness::testRate;

int ownFailures = 0;
void check (bool ok, const std::string& what)
{
    if (! ok)
    {
        ++ownFailures;
        std::printf ("    FAIL: %s\n", what.c_str());
    }
}

std::string num (double v, int decimals = 1)
{
    char buffer[40];
    std::snprintf (buffer, sizeof (buffer), "%.*f", decimals, v);
    return buffer;
}

double db (double gain) { return 20.0 * std::log10 (std::max (1.0e-15, gain)); }

std::vector<float> sine (double hz, float amplitude, double seconds, double fs = baseRate)
{
    std::vector<float> s ((size_t) std::lround (seconds * fs));
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = amplitude * (float) std::sin (2.0 * fx::pi * hz * (double) i / fs);
    return s;
}

/** Left channel of the engine's output (both channels are the same: the amp is mono). */
std::vector<float> render (int variant, const std::vector<float>& input, const Knobs& knobs, double fs = baseRate,
                           const std::vector<harness::Step>& schedule = {})
{
    fx::AmpFx amp;
    amp.prepare (fs, harness::blockSize);
    const auto stereo = harness::render (amp, variant, input, knobs, schedule);
    std::vector<float> mono (input.size());
    for (size_t i = 0; i < input.size(); ++i)
        mono[i] = stereo[i * 2];
    return mono;
}

/** Amplitude (rms) of the component at `hz` in the last n samples (hz must fit a whole number of cycles in them). */
double component (const std::vector<float>& x, double hz, int n, double fs = baseRate)
{
    const size_t start = x.size() - (size_t) n;
    const double w = 2.0 * fx::pi * hz / fs, cw = std::cos (w), sw = std::sin (w);
    double c = 1.0, s = 0.0, re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i)
    {
        re += x[start + (size_t) i] * c;
        im += x[start + (size_t) i] * s;
        const double nc = c * cw - s * sw;
        s = s * cw + c * sw;
        c = nc;
    }
    return std::sqrt (2.0 * (re * re + im * im)) / n;
}

struct Spectrum
{
    double fundamental = 0.0; // rms
    double thd = 0.0;         // all harmonics together / fundamental
    double h2 = 0.0, h3 = 0.0; // 2nd and 3rd harmonic / fundamental
    double rest = 0.0;        // everything that is neither DC, fundamental nor a harmonic / total (aliasing), in dB
    double rms = 0.0;         // without DC
    double dc = 0.0;          // mean
};

/** Spectrum of the last n samples of a sine's response. */
Spectrum analyse (const std::vector<float>& x, double f0, int n, bool withRest = false, double fs = baseRate)
{
    const size_t start = x.size() - (size_t) n;
    double mean = 0.0, total = 0.0;
    for (int i = 0; i < n; ++i) mean += x[start + (size_t) i];
    mean /= n;
    for (int i = 0; i < n; ++i) { const double v = x[start + (size_t) i] - mean; total += v * v; }
    total /= n;

    Spectrum sp;
    sp.dc = mean;
    sp.rms = std::sqrt (total);
    const double fund = component (x, f0, n, fs);
    sp.fundamental = fund;
    sp.thd = std::sqrt (std::max (0.0, total - fund * fund)) / std::max (1.0e-30, fund);
    sp.h2 = component (x, 2.0 * f0, n, fs) / std::max (1.0e-30, fund);
    sp.h3 = component (x, 3.0 * f0, n, fs) / std::max (1.0e-30, fund);
    if (withRest)
    {
        double harmonics = 0.0;
        for (int k = 1; k * f0 < 0.5 * fs - 1.0; ++k)
        {
            const double a = component (x, k * f0, n, fs);
            harmonics += a * a;
        }
        sp.rest = 10.0 * std::log10 (std::max (1.0e-30, total - harmonics) / std::max (1.0e-30, total));
    }
    return sp;
}

Knobs defaults (int variant, const std::vector<fx::ModelInfo>& models, bool hum = false)
{
    Knobs k = harness::defaultKnobs (models[(size_t) variant]);
    if (! hum)
        k[kHum] = 0.0f;
    return k;
}

Knobs with (Knobs k, int knob, float value) { k[(size_t) knob] = value; return k; }

/** Response of the amp to a 0.6 s sine of the given amplitude: the last 0.2 s are analysed. */
Spectrum play (int variant, const Knobs& k, float amplitude = 0.2f, double hz = 220.0, bool withRest = false)
{
    return analyse (render (variant, sine (hz, amplitude, 0.6), k), hz, 9600, withRest);
}

/** Small-signal gain (linear) at `hz`: a sine far below any clipping. */
double smallGain (int variant, const Knobs& k, double hz)
{
    const float amplitude = 2.0e-5f;
    return component (render (variant, sine (hz, amplitude, 0.3), k), hz, 4800) / (amplitude / std::sqrt (2.0));
}

// what each amp is supposed to be at its default knobs
enum Voicing { clean, edge, crunch, high };
constexpr Voicing voicing[numAmps] = {
    clean, clean, edge, crunch, clean, crunch, crunch, clean, clean, crunch,       // Blackface Double .. Divide 9/15
    edge, edge, crunch, crunch, crunch, crunch, crunch, crunch, crunch, crunch,    // PhD Motorway .. Brit J-800
    high, high, high, high, clean, crunch, high, high, high, clean,                // Bomber Uber .. Flip Top
    high, crunch, crunch, high, high, crunch, high, high, high, high,              // PV Panama .. Line 6 Octone
    clean, crunch, crunch, clean, clean, edge, clean, edge, edge, clean };         // Jazz Rivet .. G Cougar 800
const char* const voicingNames[] = { "clean", "edge", "crunch", "high gain" };

//==============================================================================
double stackDb (StackId id, double t, double m, double l, double hz)
{
    double b[4], a[4];
    fmvAnalog (stacks[id], t, m, l, b, a);
    return db (fmvMagnitude (b, a, hz));
}

/** Steady-state gain of the discretised stack at `hz`, measured by running its recursion on a sine. */
double stateSpaceDb (StackId id, double t, double m, double l, double fs, double hz)
{
    double P[9], Q[3], C[3], D = 0.0;
    fmvStateSpace (stacks[id], t, m, l, fs, 1.0, P, Q, C, D);
    const int settle = (int) (0.25 * fs), window = (int) (0.05 * fs);
    double x0 = 0.0, x1 = 0.0, x2 = 0.0, last = 0.0, re = 0.0, im = 0.0;
    for (int i = 0; i < settle + window; ++i)
    {
        const double ph = 2.0 * fx::pi * hz * i / fs, u = std::sin (ph), us = last + u;
        const double n0 = P[0] * x0 + P[1] * x1 + P[2] * x2 + Q[0] * us, n1 = P[3] * x0 + P[4] * x1 + P[5] * x2 + Q[1] * us,
                     n2 = P[6] * x0 + P[7] * x1 + P[8] * x2 + Q[2] * us;
        x0 = n0; x1 = n1; x2 = n2; last = u;
        const double y = C[0] * x0 + C[1] * x1 + C[2] * x2 + D * u;
        if (i >= settle) { re += y * std::cos (ph); im += y * u; }
    }
    return db (2.0 * std::sqrt (re * re + im * im) / window);
}

/** The passive stacks: the known shapes of each family with all knobs at noon, what the three pots do,
    and that the filter the engine runs (state-space, trapezoidal) has the analytic response. */
void checkToneStacks()
{
    std::printf ("-- tone stacks at noon: dB at 80 / 200 / 500 / 1k / 3k / 8k Hz | scoop at, depth | treble, mid, bass knob range | filter error\n");
    const char* const names[numStacks] = { "blackface", "bassman", "jtm45", "plexi", "vox", "recto", "soldano", "hiwatt", "engl", "orange", "pete" };
    double scoopHz[numStacks] {}, depth[numStacks] {};

    for (int s = 0; s < numStacks; ++s)
    {
        const auto id = (StackId) s;
        const double t = 0.5, m = id == stackVox ? 1.0 : midTaper (0.5), l = bassTaper (0.5);

        double lowest = 1.0e9;
        for (double hz = 100.0; hz <= 3000.0; hz *= 1.02)
            if (const double v = stackDb (id, t, m, l, hz); v < lowest) { lowest = v; scoopHz[s] = hz; }
        depth[s] = std::max (stackDb (id, t, m, l, 80.0), stackDb (id, t, m, l, 5000.0)) - lowest;

        const double trebleRange = stackDb (id, 1.0, m, l, 4000.0) - stackDb (id, 0.0, m, l, 4000.0);
        const double midRange = stackDb (id, t, midTaper (1.0), l, 500.0) - stackDb (id, t, midTaper (0.0), l, 500.0);
        const double bassRange = stackDb (id, t, m, bassTaper (1.0), 80.0) - stackDb (id, t, m, bassTaper (0.0), 80.0);

        // the engine's filter against the analytic response (at the frequency the trapezoidal rule warps each test tone to)
        double worst = 0.0;
        for (double fs : { 176400.0, 192000.0, 768000.0 })
            for (int set = 0; set < 4; ++set)
            {
                const double kt[] = { 0.0, 0.5, 1.0, 0.2 }, km[] = { 0.0, 0.5, 1.0, 0.9 }, kl[] = { 0.0, 0.5, 1.0, 0.7 };
                const double mm = id == stackVox ? 1.0 : midTaper (km[set]), ll = bassTaper (kl[set]);
                for (double hz : { 60.0, 240.0, 1000.0, 4000.0, 10000.0 })
                    worst = std::max (worst, std::abs (stateSpaceDb (id, kt[set], mm, ll, fs, hz)
                                                       - stackDb (id, kt[set], mm, ll, fs / fx::pi * std::tan (fx::pi * hz / fs))));
            }

        std::printf ("  %-10s %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f | %5.0f Hz %5.1f dB | %5.1f %5.1f %5.1f dB | %.4f dB\n", names[s],
                     stackDb (id, t, m, l, 80.0), stackDb (id, t, m, l, 200.0), stackDb (id, t, m, l, 500.0), stackDb (id, t, m, l, 1000.0),
                     stackDb (id, t, m, l, 3000.0), stackDb (id, t, m, l, 8000.0), scoopHz[s], depth[s], trebleRange, midRange, bassRange, worst);

        const std::string name = names[s];
        check (worst < 0.01, name + " stack: the state-space filter is " + num (worst, 4) + " dB off the analytic response");
        check (scoopHz[s] > 250.0 && scoopHz[s] < 1200.0 && depth[s] > 2.0 && depth[s] < 25.0, name + " stack: no mid scoop where a passive stack has one");
        check (trebleRange > 8.0, name + " stack: Treble moves 4 kHz by only " + num (trebleRange) + " dB");
        check (bassRange > 8.0, name + " stack: Bass moves 80 Hz by only " + num (bassRange) + " dB");
        if (id != stackVox)
            check (midRange > 4.0, name + " stack: Mid moves 500 Hz by only " + num (midRange) + " dB");
    }

    check (scoopHz[stackBlackface] > 300.0 && scoopHz[stackBlackface] < 600.0 && depth[stackBlackface] > 10.0,
           "blackface stack: the scoop should be deep and sit at 400-500 Hz");
    check (depth[stackBassman] < depth[stackBlackface] - 4.0 && scoopHz[stackBassman] > scoopHz[stackBlackface],
           "bassman stack: should be flatter than the blackface one, with its dip higher up");
    check (depth[stackPlexi] < depth[stackBassman], "plexi stack (33k slope resistor): should have more mids than the bassman one");
    check (depth[stackVox] > 10.0 && scoopHz[stackVox] > scoopHz[stackBlackface], "vox top boost: deep scoop, higher than the blackface one");
    check (depth[stackHiwatt] < depth[stackBlackface] - 5.0, "hiwatt stack: should be much flatter than the blackface one");
    check (depth[stackPete] < depth[stackBlackface] - 2.0, "black panel pete stack: should have more mids than the stock blackface one");
}

/** The tone knobs through the whole engine (small signals, so nothing clips), with the HD500X's remaps. */
void checkToneKnobs (const std::vector<fx::ModelInfo>& models)
{
    std::printf ("-- tone knobs through the amp, 0 -> 100 %% (dB): Bass @60 Hz, Mid @600 Hz / @6 kHz, Treble @8 kHz / @200 Hz, Presence @6 kHz\n");
    for (int v = 0; v < numAmps; ++v)
    {
        const Knobs def = defaults (v, models);
        const std::string name = models[(size_t) v].name;
        const int mode = specs[v].midMode;
        auto range = [&] (int knob, double hz) { return db (smallGain (v, with (def, knob, 100.0f), hz) / smallGain (v, with (def, knob, 0.0f), hz)); };

        const double bass = range (kBass, 60.0), mid = range (kMid, 600.0), midTop = range (kMid, 6000.0), midLow = range (kMid, 200.0);
        const double treble = range (kTreble, 8000.0), trebleTop = range (kTreble, 6000.0), trebleLow = range (kTreble, 200.0), presence = range (kPresence, 6000.0);
        std::printf ("  %-24s %6.1f | %6.1f %6.1f | %6.1f %6.1f | %6.1f   %s\n", models[(size_t) v].key, bass, mid, midTop, treble, trebleLow, presence,
                     mode == midCut ? "(Mid = Cut)" : mode == midTone ? "(Mid = Tone)" : mode == midDivide ? "(Bass = dirty drive, Mid = Tone, Treble = Cut)" : "");

        check (presence > 2.5, name + ": Presence raises 6 kHz by only " + num (presence) + " dB");
        if (mode == midDivide)
        {
            const Knobs dirtyOff = with (def, kBass, 0.0f);
            const double cleanLevel = db (smallGain (v, with (dirtyOff, kDrive, 100.0f), 1000.0) / smallGain (v, with (dirtyOff, kDrive, 0.0f), 1000.0));
            const double dirty = db (smallGain (v, with (def, kBass, 100.0f), 1000.0) / smallGain (v, with (def, kBass, 0.0f), 1000.0));
            check (cleanLevel > 6.0, name + ": Drive (clean-channel level) raises the level by only " + num (cleanLevel) + " dB");
            check (dirty > 6.0, name + ": Bass (dirty-channel drive) raises the level by only " + num (dirty) + " dB");
            check (midTop > 10.0 && std::abs (midLow) < 1.5, name + ": Mid should be the Tone control (treble cut)");
            check (trebleTop > 5.0 && std::abs (trebleLow) < 1.0, name + ": Treble should be the Cut control");
            continue;
        }

        check (bass > 4.0, name + ": Bass raises 60 Hz by only " + num (bass) + " dB");
        check (treble > 5.0, name + ": Treble raises 8 kHz by only " + num (treble) + " dB");
        if (mode == midNormal)
            check (mid > 2.0, name + ": Mid raises 600 Hz by only " + num (mid) + " dB");
        if (mode == midCut)
            check (midTop > 5.0 && std::abs (midLow) < 1.0, name + ": Mid should be the Vox Cut (6 kHz " + num (midTop) + " dB, 200 Hz " + num (midLow) + " dB)");
        if (mode == midTone)
            check (midTop > 10.0 && std::abs (midLow) < 1.5, name + ": Mid should be the single Tone knob (6 kHz " + num (midTop) + " dB, 200 Hz " + num (midLow) + " dB)");
    }

    // the blackface scoop must survive the trip through the whole amp
    for (int v : { 0, 1, 7, 8 })
    {
        const Knobs def = defaults (v, models);
        double lowest = 1.0e9, at = 0.0;
        for (double hz : { 100.0, 150.0, 220.0, 300.0, 400.0, 500.0, 600.0, 800.0, 1000.0, 1500.0, 2000.0, 3000.0 })
            if (const double g = db (smallGain (v, def, hz)); g < lowest) { lowest = g; at = hz; }
        check (at >= 300.0 && at <= 600.0, std::string (models[(size_t) v].name) + ": the response dips at " + num (at, 0) + " Hz instead of 400-500 Hz");
    }
}

//==============================================================================
/** Level change from Master 20 % to 100 % that a perfectly linear power amp would give (Master's own level law). */
double linearMasterRangeDb()
{
    const double cutDb = 36.0 * std::pow (0.8, 1.2);
    return 0.45 * cutDb;
}

/** How much each amp distorts a 220 Hz sine at guitar level, and how that follows the playing strength, Drive and Master. */
void checkDistortion (const std::vector<fx::ModelInfo>& models)
{
    std::printf ("-- distortion (THD %% of a 220 Hz sine): default knobs at 0.2 | soft 0.04 | hard 0.4 | Drive 0 | Drive 100 | h2, h3 | Master 20 -> 100 at 0.4: THD, power-amp compression dB (and at Drive 100)\n");
    for (int v = 0; v < numAmps; ++v)
    {
        const Knobs def = defaults (v, models);
        const std::string name = models[(size_t) v].name;
        const bool divide = specs[v].midMode == midDivide;
        Knobs driveOff = with (def, kDrive, 0.0f);
        if (divide)
            driveOff[kBass] = 0.0f; // the dirty channel's drive is on Bass
        Knobs driveFull = with (def, kDrive, 100.0f);
        if (divide)
            driveFull[kBass] = 100.0f;

        const auto normal = play (v, def, 0.2f), soft = play (v, def, 0.04f), hard = play (v, def, 0.4f);
        const double thd = normal.thd * 100.0, thdSoft = soft.thd * 100.0, thdHard = hard.thd * 100.0;
        const double thdOff = play (v, driveOff, 0.2f).thd * 100.0, thdFull = play (v, driveFull, 0.2f).thd * 100.0;

        const auto m20 = play (v, with (def, kMaster, 20.0f), 0.4f), m100 = play (v, with (def, kMaster, 100.0f), 0.4f);
        const double compression = linearMasterRangeDb() - db (m100.rms / m20.rms);
        const double compressionFull = linearMasterRangeDb() - db (play (v, with (driveFull, kMaster, 100.0f), 0.4f).rms / play (v, with (driveFull, kMaster, 20.0f), 0.4f).rms);

        std::printf ("  %-24s %-9s %6.1f %6.1f %6.1f %6.1f %6.1f | %5.1f %5.1f | %5.1f -> %5.1f  %5.1f dB (%5.1f dB)\n", models[(size_t) v].key, voicingNames[voicing[v]],
                     thd, thdSoft, thdHard, thdOff, thdFull, normal.h2 * 100.0, normal.h3 * 100.0, m20.thd * 100.0, m100.thd * 100.0, compression, compressionFull);

        switch (voicing[v])
        {
            case clean:  check (thd < 4.0 && thdSoft < 1.0, name + " should be clean at its defaults (THD " + num (thd) + " %, soft " + num (thdSoft) + " %)"); break;
            case edge:   check (thd < 9.0 && thdSoft < 2.0, name + " should be clean with an edge at its defaults (THD " + num (thd) + " %, soft " + num (thdSoft) + " %)"); break;
            case crunch: check (thd > 4.0 && thd < 45.0, name + " should crunch at its defaults (THD " + num (thd) + " %)"); break;
            case high:   check (thd > 25.0 && thdSoft > 15.0, name + " should be saturated at its defaults (THD " + num (thd) + " %, soft " + num (thdSoft) + " %)"); break;
        }

        check (thdFull > thdOff + 5.0, name + ": THD should rise with Drive (" + num (thdOff) + " % -> " + num (thdFull) + " %)");
        const auto hardPre = play (v, with (def, kPowerAmp, 1.0f), 0.4f);
        check (std::abs (hard.dc) < 0.01 * hard.rms && std::abs (hardPre.dc) < 0.01 * hardPre.rms, name + ": the asymmetric stages leave DC in the output");
        if (voicing[v] == crunch || voicing[v] == edge)
        {
            check (thdOff < (voicing[v] == crunch ? std::min (8.0, 0.6 * thd) : 5.0), name + " should clean up with Drive turned down (THD " + num (thdOff) + " % at Drive 0)");
            check (thdHard > 1.8 * thdSoft, name + " should break up when played harder (THD " + num (thdSoft) + " % soft, " + num (thdHard) + " % hard)");
        }

        // Master: the power amp must clip (compress) with everything turned up, on every amp...
        check (compressionFull > 1.0, name + ": Master at 100 % does not drive the power amp into clipping (compression " + num (compressionFull) + " dB)");
        // ...and where the preamp is not already a square wave and the power amp is clearly reached at the default Drive, THD must rise
        if (m20.thd < 0.20 && compression > 2.0)
            check (m100.thd > 1.2 * m20.thd, name + ": THD should rise with Master (" + num (m20.thd * 100.0) + " % -> " + num (m100.thd * 100.0) + " %)");
    }
}

//==============================================================================
/** High-gain amps on a low power chord: "fizz" = how much of the output lies above 7 kHz, "flub" = how much lies
    below 55 Hz (the difference tones that the chord's notes make when the bass is clipped together with the rest). */
void checkTightness (const std::vector<fx::ModelInfo>& models)
{
    std::printf ("-- high gain on an E power chord: share of the output above 7 kHz (fizz) and below 55 Hz (flub), dB\n");
    std::vector<float> chord (48000);
    for (size_t i = 0; i < chord.size(); ++i)
    {
        const double t = (double) i / baseRate;
        double v = 0.0;
        for (double hz : { 82.41, 123.47, 164.81 })
            for (int k = 1; k <= 5; ++k)
                v += std::sin (2.0 * fx::pi * hz * k * t) / k;
        chord[i] = (float) (0.1 * v);
    }

    auto band = [] (const std::vector<float>& x, double hz, bool highPass)
    {
        fx::Biquad a, b; // 4th-order Butterworth
        if (highPass) { a.setHighPass (baseRate, hz, 0.5412); b.setHighPass (baseRate, hz, 1.3066); }
        else          { a.setLowPass (baseRate, hz, 0.5412);  b.setLowPass (baseRate, hz, 1.3066); }
        double inBand = 0.0, total = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const float y = b.process (a.process (x[i]));
            if (i >= x.size() / 2) { inBand += (double) y * y; total += (double) x[i] * x[i]; }
        }
        return 10.0 * std::log10 (std::max (1.0e-30, inBand) / total);
    };

    double flub[numAmps] {}, fizz[numAmps] {};
    for (int v = 0; v < numAmps; ++v)
    {
        if (voicing[v] != high)
            continue;
        const auto out = render (v, chord, defaults (v, models));
        fizz[v] = band (out, 7000.0, true);
        flub[v] = band (out, 55.0, false);
        std::printf ("  %-24s %6.1f %6.1f\n", models[(size_t) v].key, fizz[v], flub[v]);
        check (fizz[v] < -18.0, std::string (models[(size_t) v].name) + " is fizzy: " + num (fizz[v]) + " dB of its output is above 7 kHz");
        check (flub[v] < -15.0, std::string (models[(size_t) v].name) + " is flubby: " + num (flub[v]) + " dB of its output is below 55 Hz");
    }
    // by key: the tight ones (Engl, Uberschall, Purge) against the ones voiced for low end (Big Bottom, Doom)
    auto at = [&] (const char* key) { for (int v = 0; v < numAmps; ++v) if (std::string (models[(size_t) v].key) == key) return v; return 0; };
    const double tightest = std::max ({ flub[at ("angel_f_ball")], flub[at ("line6_purge")], flub[at ("bomber_uber")] });
    check (tightest < flub[at ("line6_big_bottom")] - 3.0 && tightest < flub[at ("line6_doom")] - 3.0,
           "the tight amps (F-Ball, Purge, Uber) should carry clearly less sub-bass than Big Bottom and Doom");
    check (fizz[at ("bomber_uber")] < fizz[at ("treadplate")] - 2.0 && fizz[at ("line6_epic")] < fizz[at ("line6_aggro")] - 2.0,
           "the dark amps (Uber, Epic) should have less top than the raw ones (Treadplate, Aggro)");
}

//==============================================================================
double onsetToSteadyDb (const std::vector<float>& x)
{
    double onset = 0.0, steady = 0.0;
    for (size_t i = 48; i < 528; ++i) onset += (double) x[i] * x[i];             // 1 .. 11 ms
    for (size_t i = x.size() - 9600; i < x.size(); ++i) steady += (double) x[i] * x[i];
    return 10.0 * std::log10 ((onset / 480.0) / std::max (1.0e-30, steady / 9600.0));
}

/** Presence is checked with the tone knobs; here Sag, Bias, Bias X and the Power Amp switch. */
void checkPowerAmpKnobs (const std::vector<fx::ModelInfo>& models)
{
    std::printf ("-- power amp, driven hard (Drive 100, Master 100, sine 0.4): Sag 0 -> 100 level dB, onset overshoot dB | Bias X 0 -> 100 level dB |"
                 " Bias: small-signal gain cold / class A vs 50 %% dB, h2 %% cold / 50 / class A, THD %% | Pre vs On: 20 Hz dB, difference dB\n");
    const auto riff = harness::makeInput();

    for (int v = 0; v < numAmps; ++v)
    {
        const Knobs def = defaults (v, models);
        const std::string name = models[(size_t) v].name;
        Knobs hot = with (with (def, kDrive, 100.0f), kMaster, 100.0f);
        if (specs[v].midMode == midDivide)
            hot[kBass] = 100.0f;
        const auto loud = sine (220.0, 0.4f, 0.6);

        // Sag: the supply drops under load: a loud sustained note comes out lower, its attack stays
        const auto tight = render (v, loud, with (hot, kSag, 0.0f)), spongy = render (v, loud, with (hot, kSag, 100.0f));
        const double sagDrop = db (analyse (spongy, 220.0, 9600).rms / analyse (tight, 220.0, 9600).rms);
        const double overshoot = onsetToSteadyDb (spongy) - onsetToSteadyDb (tight);
        const bool tubeSupply = specs[v].sagDepth >= 0.05f;
        check (sagDrop < (tubeSupply ? -1.0 : -0.1), name + ": Sag does not compress a hard-driven power amp (" + num (sagDrop) + " dB)");
        check (overshoot > (tubeSupply ? 0.5 : 0.05), name + ": Sag should let the attack through before it gives (" + num (overshoot) + " dB)");
        if (voicing[v] == clean)
        {
            const double softChange = db (play (v, with (def, kSag, 100.0f), 0.01f).rms / play (v, with (def, kSag, 0.0f), 0.01f).rms);
            check (std::abs (softChange) < 0.3, name + ": Sag should leave soft playing alone (" + num (softChange) + " dB)");
        }

        // Bias X: hard drive pushes the bias colder: compression that only happens when the power amp is pushed
        const double biasXDrop = db (analyse (render (v, loud, with (hot, kBiasX, 100.0f)), 220.0, 9600).rms
                                     / analyse (render (v, loud, with (hot, kBiasX, 0.0f)), 220.0, 9600).rms);
        check (biasXDrop < -1.0, name + ": Bias X does not compress a hard-driven power amp (" + num (biasXDrop) + " dB)");
        if (voicing[v] == clean)
        {
            const double softChange = db (play (v, with (def, kBiasX, 100.0f), 0.01f).rms / play (v, with (def, kBiasX, 0.0f), 0.01f).rms);
            check (std::abs (softChange) < 0.3, name + ": Bias X should leave soft playing alone (" + num (softChange) + " dB)");
        }

        // Bias: cold = crossover notch (quiet notes lose gain); towards class A the notch closes and the curve gets softer.
        // Master at 100 so that the power amp is reached; a tiny sine for the gain, soft playing for the harmonics.
        const Knobs quiet = with (def, kMaster, 100.0f);
        const double gainCold = db (smallGain (v, with (quiet, kBias, 0.0f), 1000.0) / smallGain (v, with (quiet, kBias, 50.0f), 1000.0));
        const double gainHot = db (smallGain (v, with (quiet, kBias, 100.0f), 1000.0) / smallGain (v, with (quiet, kBias, 50.0f), 1000.0));
        const auto cold = play (v, with (quiet, kBias, 0.0f), 0.04f), mid = play (v, with (quiet, kBias, 50.0f), 0.04f), classA = play (v, with (quiet, kBias, 100.0f), 0.04f);
        check (gainCold < -1.0, name + ": a cold Bias should open a crossover notch (small-signal gain " + num (gainCold) + " dB)");
        check (std::abs (gainHot) < 1.0, name + ": Bias towards class A should not change the small-signal gain (" + num (gainHot) + " dB)");
        const double spectrumChange = std::max ({ std::abs (cold.h2 - mid.h2), std::abs (cold.h3 - mid.h3), std::abs (classA.h2 - mid.h2), std::abs (classA.h3 - mid.h3) }) * 100.0;
        const double levelChange = std::max (std::abs (db (cold.rms / mid.rms)), std::abs (db (classA.rms / mid.rms)));
        if (voicing[v] != high) // (a saturated preamp feeds the output tubes a square wave: they are flat out at any bias)
            check (spectrumChange > 0.3 || levelChange > 0.3, name + ": Bias changes neither the harmonics (" + num (spectrumChange, 2) + " points) nor the level (" + num (levelChange, 2) + " dB)");
        if (specs[v].match < 0.5f) // single-ended: the bias point sets how lopsided the one tube clips
            check (std::abs (cold.h2 - classA.h2) > 0.02, name + " (single-ended): Bias should change the even harmonics (h2 " + num (cold.h2 * 100.0) + " % cold, "
                                                           + num (classA.h2 * 100.0) + " % class A)");

        // Power Amp Off = the "Pre" model: no output transformer (its bass roll-off is gone), and a different signal
        const Knobs pre = with (def, kPowerAmp, 1.0f);
        const double lowOn = db (smallGain (v, def, 20.0) / smallGain (v, def, 1000.0)), lowPre = db (smallGain (v, pre, 20.0) / smallGain (v, pre, 1000.0));
        const auto on = render (v, riff, def), off = render (v, riff, pre);
        double diff = 0.0, sum = 0.0;
        for (size_t i = 0; i < on.size(); ++i) { diff += ((double) on[i] - off[i]) * ((double) on[i] - off[i]); sum += (double) on[i] * on[i]; }
        const double difference = 10.0 * std::log10 (diff / sum);
        check (lowOn < lowPre - 2.0, name + ": the Pre model should lack the output transformer's bass roll-off (20 Hz: " + num (lowOn) + " dB on, " + num (lowPre) + " dB pre)");
        check (difference > -30.0, name + ": Power Amp Off sounds the same as On (difference " + num (difference) + " dB)");

        std::printf ("  %-24s %6.1f %5.1f | %6.1f | %6.1f %5.1f   %5.1f %5.1f %5.1f   %5.1f %5.1f %5.1f | %6.1f %6.1f\n", models[(size_t) v].key, sagDrop, overshoot, biasXDrop,
                     gainCold, gainHot, cold.h2 * 100.0, mid.h2 * 100.0, classA.h2 * 100.0, cold.thd * 100.0, mid.thd * 100.0, classA.thd * 100.0, lowOn - lowPre, difference);
    }
}

//==============================================================================
/** Hum: nothing at all at 0 %, 60 Hz heater hum with its 3rd harmonic otherwise, and 120 Hz ripple on the supply
    (which shows as sidebands around a note, since a push-pull stage cancels the ripple itself). */
void checkHum (const std::vector<fx::ModelInfo>& models)
{
    const std::vector<float> silence (48000, 0.0f);
    double leastHum = 1.0e9, mostHum = 0.0, leastRipple = 1.0e9, mostDefault = 0.0;

    for (int v = 0; v < numAmps; ++v)
    {
        const std::string name = models[(size_t) v].name;
        for (float powerAmp : { 0.0f, 1.0f })
        {
            const Knobs k = with (harness::defaultKnobs (models[(size_t) v]), kPowerAmp, powerAmp);
            const std::string which = name + (powerAmp > 0.5f ? " (Pre)" : "");

            float peak = 0.0f;
            for (float s : render (v, silence, with (k, kHum, 0.0f))) peak = std::max (peak, std::abs (s));
            check (peak == 0.0f, which + ": Hum at 0 is not completely silent (peak " + harness::jsonNumber (peak) + ")");

            const auto full = render (v, silence, with (k, kHum, 100.0f)), half = render (v, silence, with (k, kHum, 50.0f));
            const double hum60 = component (full, 60.0, 24000), hum180 = component (full, 180.0, 24000), half60 = component (half, 60.0, 24000);
            leastHum = std::min (leastHum, hum60);
            mostHum = std::max (mostHum, hum60);
            check (hum60 > 1.0e-4 && hum60 < 3.0e-3, which + ": 60 Hz hum at Hum 100 is " + harness::jsonNumber (hum60));
            check (hum180 > 0.15 * hum60 && hum180 < hum60, which + ": heater hum should carry some 180 Hz (" + harness::jsonNumber (hum180 / hum60) + " of the 60 Hz)");
            check (half60 > 0.2 * hum60 && half60 < 0.3 * hum60, which + ": Hum 50 should give a quarter of the hum of Hum 100");

            float defaultPeak = 0.0f;
            for (float s : render (v, silence, k)) defaultPeak = std::max (defaultPeak, std::abs (s));
            mostDefault = std::max (mostDefault, (double) defaultPeak);
        }

        // ripple: 120 Hz sidebands around a 1 kHz note, only with Hum up
        const Knobs def = harness::defaultKnobs (models[(size_t) v]);
        const auto tone = sine (1000.0, 0.1f, 0.6);
        const auto clean = render (v, tone, with (def, kHum, 0.0f)), rippled = render (v, tone, with (def, kHum, 100.0f));
        const double sideband = component (rippled, 1120.0, 9600) / component (rippled, 1000.0, 9600);
        const double without = component (clean, 1120.0, 9600) / component (clean, 1000.0, 9600);
        leastRipple = std::min (leastRipple, sideband);
        check (sideband > 0.005 && without < 1.0e-4, name + ": Hum should put 120 Hz ripple on the power amp (sideband " + harness::jsonNumber (sideband) + ", without Hum "
                                                     + harness::jsonNumber (without) + ")");
    }
    std::printf ("-- hum: silent at 0 %%; 60 Hz at Hum 100 between %.5f and %.5f rms; ripple sidebands at least %.1f dB; loudest default hum peak %.5f\n",
                 leastHum, mostHum, db (leastRipple), mostDefault);
}

//==============================================================================
/** Ch Vol's calibration: every amp (and its Pre version) at its default knobs is about as loud as the input. */
void checkLoudness (const std::vector<fx::ModelInfo>& models)
{
    const auto riff = harness::makeInput();
    const double inputRms = harness::measure (riff).rms;
    double lo = 1.0e9, hi = -1.0e9, loPre = 1.0e9, hiPre = -1.0e9;
    for (int v = 0; v < numAmps; ++v)
    {
        const Knobs def = harness::defaultKnobs (models[(size_t) v]);
        const double level = db (harness::measure (render (v, riff, def)).rms / inputRms);
        const double levelPre = db (harness::measure (render (v, riff, with (def, kPowerAmp, 1.0f))).rms / inputRms);
        lo = std::min (lo, level); hi = std::max (hi, level);
        loPre = std::min (loPre, levelPre); hiPre = std::max (hiPre, levelPre);
        check (std::abs (level) < 1.5, std::string (models[(size_t) v].name) + ": default level is " + num (level) + " dB from the input's");
        check (std::abs (levelPre) < 1.5, std::string (models[(size_t) v].name) + " (Pre): default level is " + num (levelPre) + " dB from the input's");

        // Ch Vol is a clean level control: +-6 dB of knob gives +-6 dB of output, whatever the amp is doing
        const auto up = play (v, with (defaults (v, models), kChVol, 75.0f)), down = play (v, with (defaults (v, models), kChVol, 25.0f)), mid = play (v, defaults (v, models));
        const double expectUp = 1.585 * db (1.5), expectDown = 1.585 * db (0.5);
        check (std::abs (db (up.rms / mid.rms) - expectUp) < 0.1 && std::abs (db (down.rms / mid.rms) - expectDown) < 0.1 && std::abs (up.thd - down.thd) < 0.002,
               std::string (models[(size_t) v].name) + ": Ch Vol should only change the level");
    }
    std::printf ("-- loudness at default knobs vs the input: %.1f .. %.1f dB (full amps), %.1f .. %.1f dB (Pre models)\n", lo, hi, loPre, hiPre);
}

//==============================================================================
/** Aliasing: for a high sine, everything in the output that is not a harmonic of it has folded back from above. */
void checkAliasing (const std::vector<fx::ModelInfo>& models)
{
    std::printf ("-- aliasing (non-harmonic part of the output, dB): 1245 Hz / 3515 Hz sine at the default knobs | with Drive and Master at 100\n");
    double worst[4] = { -999.0, -999.0, -999.0, -999.0 };
    for (int v = 0; v < numAmps; ++v)
    {
        const Knobs def = defaults (v, models);
        Knobs hot = with (with (def, kDrive, 100.0f), kMaster, 100.0f);
        if (specs[v].midMode == midDivide)
            hot[kBass] = 100.0f;
        const double r[4] = { play (v, def, 0.3f, 1245.0, true).rest, play (v, def, 0.3f, 3515.0, true).rest,
                              play (v, hot, 0.3f, 1245.0, true).rest, play (v, hot, 0.3f, 3515.0, true).rest };
        for (int i = 0; i < 4; ++i) worst[i] = std::max (worst[i], r[i]);
        if (voicing[v] == high || v % 8 == 0)
            std::printf ("  %-24s %7.1f %7.1f | %7.1f %7.1f\n", models[(size_t) v].key, r[0], r[1], r[2], r[3]);
        const std::string name = models[(size_t) v].name;
        check (r[0] < -60.0 && r[1] < -50.0, name + ": aliasing at the default knobs is " + num (r[0]) + " dB (1245 Hz), " + num (r[1]) + " dB (3515 Hz)");
        check (r[2] < -55.0 && r[3] < -45.0, name + ": aliasing with everything turned up is " + num (r[2]) + " dB (1245 Hz), " + num (r[3]) + " dB (3515 Hz)");
    }
    std::printf ("  worst of all 50:         %7.1f %7.1f | %7.1f %7.1f\n", worst[0], worst[1], worst[2], worst[3]);
}

//==============================================================================
/** Clicks and zipper noise: every knob jumps between its ends four times under a steady note. The largest
    sample-to-sample step of the output must stay near that of the steady signal at either end (Drive and Master
    get more room: on the way they pass settings where the wave has steeper edges than at both ends). */
void checkKnobMoves (const std::vector<fx::ModelInfo>& models)
{
    std::printf ("-- knob jumps 0 <-> 100 under a 220 Hz note: largest output step / largest step of the steady signal, per knob\n");
    const auto tone = sine (220.0, 0.2f, 1.2);
    auto maxStep = [] (const std::vector<float>& x) { float m = 0.0f; for (size_t i = 4800; i < x.size(); ++i) m = std::max (m, std::abs (x[i] - x[i - 1])); return m; };

    // one of each kind: blackface stack, single Tone, two channels, Vox stack + Cut, bright-cap Marshall, master-volume
    // Marshall, high gain without feedback, interactive presence, Baxandall bass amp, solid-state with shelving EQ
    for (int v : { 0, 3, 9, 12, 16, 19, 21, 23, 29, 46 })
    {
        const Knobs def = defaults (v, models);
        std::printf ("  %-24s", models[(size_t) v].key);
        for (int knob = 0; knob < numKnobs; ++knob)
        {
            const Knobs lo = with (def, knob, 0.0f), hi = with (def, knob, knob == kPowerAmp ? 1.0f : 100.0f);
            std::vector<harness::Step> schedule;
            for (int n = 0; n < 4; ++n)
                schedule.push_back ({ 9600 + n * 9600, n % 2 == 0 ? hi : lo });
            const float steady = std::max (maxStep (render (v, tone, lo)), maxStep (render (v, tone, hi)));
            const float ratio = maxStep (render (v, tone, lo, baseRate, schedule)) / std::max (1.0e-9f, steady);
            std::printf (" %5.2f", ratio);
            check (ratio < (knob == kDrive || knob == kMaster ? 4.0f : 1.6f),
                   std::string (models[(size_t) v].name) + ": moving " + models[(size_t) v].knobs[(size_t) knob].name + " clicks (step ratio " + num (ratio, 2) + ")");
        }
        std::printf ("\n");
    }
}

//==============================================================================
/** 192 kHz, all knobs at their ends at once, and odd block sizes. */
void checkRatesAndExtremes (const std::vector<fx::ModelInfo>& models)
{
    // three decaying notes with six harmonics each: the same signal at any sample rate
    auto notes = [] (double fs)
    {
        std::vector<float> x ((size_t) (2.0 * fs));
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = (double) i / fs;
            double v = 0.0;
            for (double hz : { 110.0, 164.81, 277.18 })
                for (int k = 1; k <= 6; ++k)
                    v += std::sin (2.0 * fx::pi * hz * k * t) / k;
            x[i] = (float) (0.09 * v * std::exp (-1.5 * t));
        }
        return x;
    };
    const auto riff48 = notes (48000.0), riff192 = notes (192000.0);
    const double in48 = harness::measure (riff48).rms, in192 = harness::measure (riff192).rms;
    double worstRate = 0.0, worstBlocks = -999.0;
    float worstPeak = 0.0f;

    for (int v = 0; v < numAmps; ++v)
    {
        const std::string name = models[(size_t) v].name;
        const Knobs def = harness::defaultKnobs (models[(size_t) v]);

        const auto ref = render (v, riff48, def);
        const auto high = harness::measure (render (v, riff192, def, 192000.0));
        const double difference = db (high.rms / in192) - db (harness::measure (ref).rms / in48);
        worstRate = std::max (worstRate, std::abs (difference));
        check (high.finite && std::abs (difference) < 1.0, name + ": level at 192 kHz differs by " + num (difference) + " dB from 48 kHz");

        for (float value : { 0.0f, 100.0f })
            for (float powerAmp : { 0.0f, 1.0f })
            {
                Knobs k {};
                for (int i = 0; i < kPowerAmp; ++i) k[(size_t) i] = value;
                k[kPowerAmp] = powerAmp;
                const auto s = harness::measure (render (v, riff48, k));
                worstPeak = std::max (worstPeak, s.peak);
                check (s.finite && s.peak < 8.0f, name + ": all knobs at " + num (value, 0) + " gives peak " + num (s.peak, 2));
            }

        // blocks of 1 .. 97 samples instead of 256: the same output (nothing is moving, so the chunking must not matter)
        fx::AmpFx amp;
        amp.prepare (baseRate, 256);
        amp.setModel (v);
        amp.setParameters (def.data());
        amp.reset();
        double err = 0.0, sum = 0.0;
        float left[256], right[256];
        harness::Lcg rng { 7u + (uint32_t) v };
        for (int pos = 0; pos < (int) riff48.size();)
        {
            const int n = std::min (1 + (int) (rng.next01() * 96.0f), (int) riff48.size() - pos);
            amp.setParameters (def.data());
            std::copy (riff48.begin() + pos, riff48.begin() + pos + n, left);
            std::copy (riff48.begin() + pos, riff48.begin() + pos + n, right);
            amp.process (left, right, n);
            for (int i = 0; i < n; ++i)
            {
                err += ((double) left[i] - ref[(size_t) (pos + i)]) * ((double) left[i] - ref[(size_t) (pos + i)]);
                sum += (double) ref[(size_t) (pos + i)] * ref[(size_t) (pos + i)];
            }
            pos += n;
        }
        const double blocks = 10.0 * std::log10 (std::max (1.0e-30, err) / sum);
        worstBlocks = std::max (worstBlocks, blocks);
        check (blocks < -100.0, name + ": output depends on the block size (" + num (blocks) + " dB)");
    }
    std::printf ("-- 192 kHz: level within %.1f dB of 48 kHz; all knobs at 0 or 100: peak at most %.2f; random block sizes: at most %.1f dB off\n",
                 worstRate, worstPeak, worstBlocks);
}

//==============================================================================
/** The amps must differ in voicing, headroom and feel, not only in gain: with the same knob positions on all of them,
    every pair must differ clearly in frequency response, in how much it distorts, or in how it compresses. */
void checkAmpsDiffer (const std::vector<fx::ModelInfo>& models)
{
    constexpr int numFeatures = 9;
    static double f[numAmps][numFeatures];
    const Knobs same { 50.0f, 50.0f, 50.0f, 50.0f, 50.0f, 50.0f, 70.0f, 50.0f, 0.0f, 50.0f, 50.0f, 0.0f };

    for (int v = 0; v < numAmps; ++v)
    {
        const double g1k = smallGain (v, same, 1000.0);
        int n = 0;
        for (double hz : { 100.0, 300.0, 3000.0, 6000.0 })
            f[v][n++] = db (smallGain (v, same, hz) / g1k);                          // voicing: 1 dB counts
        f[v][n++] = db (g1k) / 3.0;                                                   // gain: 3 dB counts
        const auto normal = play (v, same, 0.2f), soft = play (v, same, 0.04f), hard = play (v, same, 0.4f);
        f[v][n++] = std::log (std::max (0.002, normal.thd)) / std::log (1.3);         // distortion: a factor of 1.3 counts
        f[v][n++] = std::log (std::max (0.002, soft.thd)) / std::log (1.3);
        f[v][n++] = db (hard.rms / soft.rms);                                         // headroom / compression: 1 dB counts
        f[v][n++] = db (std::max (1.0e-4, normal.h2) / std::max (1.0e-4, normal.h3)) / 3.0; // even vs odd harmonics: 3 dB counts
    }

    struct Pair { double distance; int a, b; };
    std::vector<Pair> pairs;
    for (int a = 0; a < numAmps; ++a)
        for (int b = a + 1; b < numAmps; ++b)
        {
            double distance = 0.0;
            for (int i = 0; i < numFeatures; ++i)
                distance = std::max (distance, std::abs (f[a][i] - f[b][i]));
            pairs.push_back ({ distance, a, b });
            check (distance > 1.5, std::string (models[(size_t) a].name) + " and " + models[(size_t) b].name + " are nearly the same amp (largest difference "
                                   + num (distance, 2) + " units)");
        }
    std::sort (pairs.begin(), pairs.end(), [] (const Pair& x, const Pair& y) { return x.distance < y.distance; });
    std::printf ("-- amps differ (same knob positions on all; 1 unit = 1 dB of response or compression, a factor 1.3 of THD, 3 dB of gain). Closest pairs:\n");
    for (int i = 0; i < 6; ++i)
        std::printf ("  %-24s %-24s %5.2f\n", models[(size_t) pairs[(size_t) i].a].key, models[(size_t) pairs[(size_t) i].b].key, pairs[(size_t) i].distance);
}
} // namespace

int main (int argc, char* argv[])
{
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "test_output/standalone/amp";
    harness::Options options;
    options.maxKnobs = 12; // the amp block has its own page of knobs: six tone knobs, five power-amp knobs and the Pre switch
    // maxSilencePeak stays at the harness default (1e-3): the default Hum (20..50 %) gives at most about 0.0006 of hum.
    int failed = harness::run<fx::AmpFx> ("amp", fx::ampModels(), outDir, options);

    const auto models = fx::ampModels();
    std::printf ("\n== amp: own checks ==\n");
    checkToneStacks();
    checkToneKnobs (models);
    checkDistortion (models);
    checkTightness (models);
    checkPowerAmpKnobs (models);
    checkHum (models);
    checkLoudness (models);
    checkAliasing (models);
    checkKnobMoves (models);
    checkRatesAndExtremes (models);
    checkAmpsDiffer (models);

    failed += ownFailures;
    std::printf ("%s\n", failed == 0 ? "ALL CHECKS PASSED" : (std::to_string (failed) + " CHECK(S) FAILED").c_str());
    return failed == 0 ? 0 : 1;
}
