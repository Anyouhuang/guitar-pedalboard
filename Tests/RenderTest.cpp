// Offline test for the effect chain and tuner.
//  - renders a synthetic guitar riff through every pedal into test_output/*.wav so you can listen
//  - checks every render for NaN/Inf and runaway levels
//  - checks the pitch detector against known frequencies
//
// Usage: PedalTest [output folder]

#include <set>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "DSP/FxChain.h"
#include "DSP/PitchDetector.h"
#include "DSP/SpectrumAnalyzer.h"
#include "DSP/TapTempo.h"
#include "DSP/TestSignal.h"
#include "TestSignalPlayer.h"

namespace
{
constexpr double testRate = 48000.0;
constexpr int blockSize = 256;

//==============================================================================
/** Karplus-Strong plucked string, good enough to exercise the effects. */
void addPluck (std::vector<float>& out, double startSeconds, double frequency, float amplitude, juce::Random& rng)
{
    const int start  = (int) (startSeconds * testRate);
    const int period = std::max (2, (int) std::lround (testRate / frequency));
    std::vector<float> string ((size_t) period);

    for (auto& s : string)
        s = amplitude * (rng.nextFloat() * 2.0f - 1.0f);

    float previous = 0.0f;
    for (int i = start, k = 0; i < (int) out.size(); ++i, ++k)
    {
        const size_t idx = (size_t) (k % period);
        const float current = string[idx];
        string[idx] = 0.996f * 0.5f * (current + previous);
        previous = current;
        out[(size_t) i] += current;
    }
}

std::vector<float> makeRiff()
{
    juce::Random rng (1234);
    std::vector<float> riff ((size_t) (testRate * 7.0), 0.0f);

    // open-E power chord strum, then a little single-note line, then a long open A to hear tails
    const double chord[] = { 82.41, 123.47, 164.81 };
    for (int i = 0; i < 3; ++i)
        addPluck (riff, 0.10 + 0.012 * i, chord[i], 0.30f, rng);

    const double line[] = { 146.83, 164.81, 196.00, 220.00, 246.94, 293.66, 329.63 };
    for (int i = 0; i < 7; ++i)
        addPluck (riff, 1.6 + 0.25 * i, line[i], 0.35f, rng);

    addPluck (riff, 3.6, 110.0, 0.40f, rng);

    // small hum/hiss floor, like a real pickup (about -70 dBFS)
    for (size_t i = 0; i < riff.size(); ++i)
        riff[i] += 0.0003f * (rng.nextFloat() * 2.0f - 1.0f)
                 + 0.0002f * (float) std::sin (2.0 * fx::pi * 60.0 * (double) i / testRate);

    float peak = 0.0f;
    for (auto s : riff) peak = std::max (peak, std::abs (s));
    for (auto& s : riff) s *= 0.4f / peak; // typical interface level: peaks around -8 dBFS

    return riff;
}

//==============================================================================
struct Render
{
    juce::AudioBuffer<float> audio;
    float peak = 0.0f, rms = 0.0f;
    bool finite = true;
};

Render render (const std::vector<float>& input, const fx::FxParams& params,
               std::function<void (fx::FxParams&, double seconds)> automation = {})
{
    fx::FxChain chain;
    chain.prepare (testRate, blockSize);

    Render r;
    r.audio.setSize (2, (int) input.size());
    auto* left  = r.audio.getWritePointer (0);
    auto* right = r.audio.getWritePointer (1);

    // same start-up as the plugin's prepareToPlay(): pedals begin in their switched state
    chain.setParameters (params);
    chain.reset();

    for (int pos = 0; pos < (int) input.size(); pos += blockSize)
    {
        const int n = std::min (blockSize, (int) input.size() - pos);
        auto p = params;
        if (automation)
            automation (p, pos / testRate);

        chain.setParameters (p);
        chain.process (input.data() + pos, left + pos, right + pos, n);
    }

    double sum = 0.0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < r.audio.getNumSamples(); ++i)
        {
            const float s = r.audio.getSample (ch, i);
            r.finite = r.finite && std::isfinite (s);
            r.peak = std::max (r.peak, std::abs (s));
            sum += (double) s * s;
        }

    r.rms = (float) std::sqrt (sum / (2.0 * r.audio.getNumSamples()));
    return r;
}

void writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate (testRate)
                                                   .withNumChannels (audio.getNumChannels())
                                                   .withBitsPerSample (24));
    if (writer != nullptr)
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

fx::FxParams allOff()
{
    fx::FxParams p; // every slot empty
    p.amp = fx::makeAmp ("", fx::defaultCabKey, false); // no amp, this project's original cab, switched off
    p.eqOn = false;
    return p;
}

/** Puts model `key` in slot `s` (default knobs), then applies knob overrides by knob name.
    Tempo-synced times follow 120 BPM. */
fx::SlotParams& put (fx::FxParams& p, int s, const char* key,
                     std::initializer_list<std::pair<const char*, float>> overrides = {}, bool on = true)
{
    auto& slot = p.slots[(size_t) s];
    slot = fx::makeSlot (key, on);
    const auto& knobs = fx::modelInfo (slot.model).knobs;
    for (const auto& [name, value] : overrides)
        for (size_t k = 0; k < knobs.size(); ++k)
            if (juce::String (knobs[k].name) == name)
                slot.knobs[k] = value;

    fx::resolveTempo (slot, 120.0);
    return slot;
}

fx::FxParams defaultBoardAt120()
{
    auto p = fx::defaultBoard();
    for (auto& slot : p.slots)
        fx::resolveTempo (slot, 120.0);
    return p;
}

/** A clearly audible EQ: more bass and treble, scooped mids, low and high cuts engaged. */
fx::EqSettings smileEq()
{
    fx::EqSettings eq;
    eq.lowCutHz = 80.0f;
    eq.bassHz = 120.0f;     eq.bassDb = 6.0f;
    eq.lowMidHz = 500.0f;   eq.lowMidDb = -8.0f;  eq.lowMidQ = 1.2f;
    eq.highMidHz = 2500.0f; eq.highMidDb = 3.0f;  eq.highMidQ = 2.0f;
    eq.trebleHz = 6000.0f;  eq.trebleDb = 5.0f;
    eq.highCutHz = 9000.0f;
    return eq;
}

int failures = 0;

void check (bool ok, const juce::String& what)
{
    if (! ok)
    {
        ++failures;
        std::cout << "  FAIL: " << what << "\n";
    }
}

//==============================================================================
/** The EQ graph draws Equalizer::design(); check that the audio really follows that curve. */
void testEqualizer()
{
    std::cout << "\n== EQ ==\n";

    auto sineGainDb = [] (const fx::FxParams& params, double hz)
    {
        std::vector<float> sine ((size_t) (testRate * 1.0));
        for (size_t i = 0; i < sine.size(); ++i)
            sine[i] = 0.1f * (float) std::sin (2.0 * fx::pi * hz * (double) i / testRate);

        const auto r = render (sine, params);
        double sum = 0.0;
        const int from = r.audio.getNumSamples() / 2; // skip the filter settling time
        for (int i = from; i < r.audio.getNumSamples(); ++i)
            sum += (double) r.audio.getSample (0, i) * r.audio.getSample (0, i);

        const double rms = std::sqrt (sum / (r.audio.getNumSamples() - from));
        return fx::gainToDb ((float) (rms / (0.1 / std::sqrt (2.0))));
    };

    auto p = allOff();
    p.eqOn = true;
    p.eq = smileEq();

    fx::Biquad filters[fx::Equalizer::numBands];
    fx::Equalizer::design (p.eq, testRate, filters);

    float worst = 0.0f;
    for (double hz : { 40.0, 80.0, 120.0, 250.0, 500.0, 1000.0, 2500.0, 4000.0, 6000.0, 9000.0, 14000.0 })
    {
        double curve = 1.0;
        for (const auto& f : filters)
            curve *= f.getMagnitude (hz, testRate);

        const float drawn = fx::gainToDb ((float) curve);
        const float heard = sineGainDb (p, hz);
        worst = std::max (worst, std::abs (drawn - heard));
        std::cout << "  " << juce::String (hz, 0).paddedLeft (' ', 6) << " Hz   graph " << juce::String (drawn, 2).paddedLeft (' ', 7)
                  << " dB   measured " << juce::String (heard, 2).paddedLeft (' ', 7) << " dB\n";
    }
    std::cout << "  worst graph-vs-audio difference: " << juce::String (worst, 3) << " dB\n";
    check (worst < 0.1f, "EQ graph must match the audio");

    // flat EQ must be transparent where a guitar lives
    auto flat = allOff();
    flat.eqOn = true;
    float flatWorst = 0.0f;
    for (double hz : { 80.0, 200.0, 1000.0, 5000.0, 10000.0 })
        flatWorst = std::max (flatWorst, std::abs (sineGainDb (flat, hz)));
    std::cout << "  flat EQ: max deviation 80 Hz - 10 kHz = " << juce::String (flatWorst, 3) << " dB\n";
    check (flatWorst < 0.1f, "flat EQ should be transparent");

    // dragging a band around while playing must not click or zipper
    std::vector<float> sine ((size_t) (testRate * 2.0));
    for (size_t i = 0; i < sine.size(); ++i)
        sine[i] = 0.3f * (float) std::sin (2.0 * fx::pi * 220.0 * (double) i / testRate);

    auto maxStep = [] (const Render& r)
    {
        float step = 0.0f;
        for (int i = 1; i < r.audio.getNumSamples(); ++i)
            step = std::max (step, std::abs (r.audio.getSample (0, i) - r.audio.getSample (0, i - 1)));
        return step;
    };

    auto loud = flat;
    loud.eq.lowMidHz = 220.0f;
    loud.eq.lowMidDb = 15.0f;
    const float steady = maxStep (render (sine, loud));
    const float swept = maxStep (render (sine, flat, [] (fx::FxParams& q, double t)
    {
        // jump the low-mid band around every 100 ms, like fast mouse drags
        const int step = (int) (t / 0.1);
        q.eq.lowMidDb = (step % 2 == 0) ? 15.0f : -15.0f;
        q.eq.lowMidHz = (step % 3 == 0) ? 220.0f : 800.0f;
        q.eq.lowMidQ  = (step % 4 < 2) ? 0.5f : 4.0f;
    }));
    std::cout << "  band dragged while playing: max step " << juce::String (swept, 4)
              << " (steady at +15 dB: " << juce::String (steady, 4) << ")\n";
    check (swept <= steady * 1.15f, "moving an EQ band should not click");
}

//==============================================================================
/** The EQ panel's noise section: the mains-hum filter on the input and the hiss reduction on the output. */
/** Delays and reverbs switched off must ring out (trails); other effects must stop with the 30 ms fade. */
void testTrails (const std::vector<float>& riff)
{
    std::cout << "\n== Trails: switching off after the playing stops ==\n";

    const int burst = (int) (1.0 * testRate), offAt = burst + (int) (0.02 * testRate);
    std::vector<float> input ((size_t) (3.0 * testRate), 0.0f);
    std::copy (riff.begin(), riff.begin() + burst, input.begin());

    auto tailRms = [&] (const Render& r) // 50..600 ms after switching off
    {
        double sum = 0.0;
        const int from = offAt + (int) (0.05 * testRate), to = offAt + (int) (0.6 * testRate);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = from; i < to; ++i)
                sum += (double) r.audio.getSample (ch, i) * r.audio.getSample (ch, i);
        return (float) std::sqrt (sum / (2.0 * (to - from)) + 1.0e-30);
    };

    float worstTrail = 0.0f;
    juce::String worstName;
    int tested = 0;

    for (int m = 1; m < fx::numModels(); ++m)
    {
        const auto& info = fx::modelInfo (m);
        if (info.engine != fx::Engine::delayFx && info.engine != fx::Engine::reverbFx && info.engine != fx::Engine::modFx)
            continue;

        auto on = allOff();
        put (on, 0, info.key);
        const auto keptOn = render (input, on);
        const auto switchedOff = render (input, on, [&] (fx::FxParams& p, double t) { p.slots[0].on = t * testRate < offAt; });

        const float held = tailRms (keptOn), after = tailRms (switchedOff);

        if (info.trails)
        {
            // the input is silent by then, so muting the send changes nothing: the tail must be the same
            const float change = std::abs (fx::gainToDb (after / held));
            check (held > 1.0e-5f, juce::String (info.key) + " should still ring 50 ms after the playing stops");
            check (change < 0.5f, juce::String (info.key) + " should ring out when switched off (" + juce::String (change, 2) + " dB)");
            if (change >= worstTrail)
            {
                worstTrail = change;
                worstName = info.key;
            }
            ++tested;
        }
        else
        {
            check (after < 1.0e-5f, juce::String (info.key) + " should go quiet when switched off");
        }
    }

    std::cout << "  " << tested << " delay / reverb models ring out when switched off; largest tail change "
              << juce::String (worstTrail, 2) << " dB (" << worstName << ")\n";
}

void testNoiseReduction (const std::vector<float>& riff)
{
    std::cout << "\n== Noise reduction ==\n";

    auto tones = [] (std::initializer_list<std::pair<double, float>> partials, double seconds = 3.0)
    {
        std::vector<float> x ((size_t) (testRate * seconds), 0.0f);
        for (const auto& [hz, amplitude] : partials)
            for (size_t i = 0; i < x.size(); ++i)
                x[i] += amplitude * (float) std::sin (2.0 * fx::pi * hz * (double) i / testRate);
        return x;
    };

    auto lateRms = [] (const Render& r) // the last second, left channel
    {
        double sum = 0.0;
        const int from = r.audio.getNumSamples() - (int) testRate;
        for (int i = from; i < r.audio.getNumSamples(); ++i)
            sum += (double) r.audio.getSample (0, i) * r.audio.getSample (0, i);
        return (float) std::sqrt (sum / testRate);
    };

    // mains hum with its harmonics must go
    for (const auto& [mode, mains] : { std::pair<int, double> { fx::HumFilter::hz60, 60.0 }, { fx::HumFilter::hz50, 50.0 } })
    {
        const auto hum = tones ({ { mains, 0.02f }, { 2.0 * mains, 0.008f }, { 3.0 * mains, 0.012f }, { 5.0 * mains, 0.005f } });
        auto p = allOff();
        const float before = lateRms (render (hum, p));
        p.humMode = mode;
        const float reduction = fx::gainToDb (lateRms (render (hum, p)) / before);
        std::cout << "  " << (int) mains << " Hz hum with harmonics: " << juce::String (reduction, 1) << " dB\n";
        check (reduction < -30.0f, "hum filter should remove " + juce::String ((int) mains) + " Hz hum");

        // ...and notes must stay, even the ones right next to a harmonic
        float worst = 0.0f;
        juce::String worstNote;
        for (const auto& [name, hz] : { std::pair<const char*, double> { "E2", 82.41 }, { "G2", 98.0 }, { "A2", 110.0 }, { "B2", 123.47 },
                                        { "D3", 146.83 }, { "G3", 196.0 }, { "B3", 246.94 }, { "D4", 293.66 }, { "E4", 329.63 } })
        {
            const auto note = tones ({ { hz, 0.1f } });
            const float change = fx::gainToDb (lateRms (render (note, p)) / lateRms (render (note, allOff())));
            if (change < worst) { worst = change; worstNote = name; }
        }
        std::cout << "    open-position notes E2..E4: the most affected is " << worstNote << " at " << juce::String (worst, 2) << " dB\n";
        check (worst > -1.5f, "hum filter should leave the notes alone");
    }

    {
        auto p = allOff();
        p.humMode = fx::HumFilter::hz60;
        const float change = fx::gainToDb (render (riff, p).rms / render (riff, allOff()).rms);
        std::cout << "  riff through the 60 Hz hum filter: " << juce::String (change, 2) << " dB\n";
        check (std::abs (change) < 0.3f, "hum filter should not change the riff's level");
    }

    // hiss alone is pushed down; playing is left alone
    {
        std::vector<float> hiss ((size_t) (testRate * 3.0));
        juce::Random rng (7);
        for (auto& s : hiss)
            s = 0.001f * (rng.nextFloat() * 2.0f - 1.0f); // about -65 dBFS RMS

        for (float amount : { 25.0f, 50.0f, 100.0f })
        {
            auto p = allOff();
            const float before = lateRms (render (hiss, p));
            p.denoise = amount;
            const float reduction = fx::gainToDb (lateRms (render (hiss, p)) / before);
            const float riffChange = fx::gainToDb (render (riff, p).rms / render (riff, allOff()).rms);
            std::cout << "  denoise " << juce::String ((int) amount).paddedLeft (' ', 3) << " %: -65 dB hiss " << juce::String (reduction, 1)
                      << " dB, riff " << juce::String (riffChange, 2) << " dB\n";
            if (amount >= 50.0f)
                check (reduction < -12.0f, "denoise should push hiss down");
            check (std::abs (riffChange) < (amount > 60.0f ? 1.5f : 0.5f), "denoise should leave the playing alone");
        }
    }
}

//==============================================================================
/** The EQ screen's analyser: a sine must show up at its frequency and level, and nowhere else. */
void testAnalyzer()
{
    std::cout << "\n== Spectrum analyser ==\n";
    using SA = fx::SpectrumAnalyzer;

    struct Case { double hz; float amplitude; };
    for (auto c : { Case { 82.41, 0.5f }, Case { 440.0, 0.5f }, Case { 1000.0, 0.25f }, Case { 5000.0, 0.1f } })
    {
        SA analyzer;
        analyzer.setSampleRate (testRate);

        std::vector<float> sine (SA::fftSize * 2);
        for (size_t i = 0; i < sine.size(); ++i)
            sine[i] = c.amplitude * (float) std::sin (2.0 * fx::pi * c.hz * (double) i / testRate);

        analyzer.push (sine.data(), (int) sine.size());
        analyzer.process();

        const float* cols = analyzer.getColumnsDb();
        const int peak = (int) (std::max_element (cols, cols + SA::numColumns) - cols);
        const float peakHz = SA::columnFrequency ((float) peak);
        const float expectedDb = fx::gainToDb (c.amplitude);

        // anything more than an octave away from the tone should be far down
        float farthest = SA::floorDb;
        for (int col = 0; col < SA::numColumns; ++col)
            if (std::abs (std::log2 (SA::columnFrequency ((float) col) / c.hz)) > 1.0)
                farthest = std::max (farthest, cols[col]);

        std::cout << "  " << juce::String (c.hz, 1).paddedLeft (' ', 7) << " Hz @ " << juce::String (expectedDb, 1)
                  << " dBFS -> peak at " << juce::String (peakHz, 1) << " Hz, " << juce::String (cols[peak], 1)
                  << " dBFS; >1 octave away: " << juce::String (farthest, 1) << " dBFS" << "\n";

        check (std::abs (std::log2 (peakHz / c.hz)) < 0.05, "analyser peak frequency");
        check (std::abs (cols[peak] - expectedDb) < 1.6f, "analyser peak level");
        check (farthest < expectedDb - 50.0f, "analyser leakage");
    }
}

//==============================================================================
/** The no-guitar test signals: plucked strings must be in tune (the tuner shows IN TUNE) and
    the demo riff must loop cleanly at a DI-like level. */
void testTestSignals (const juce::File& outDir)
{
    std::cout << "\n== Test signals (no guitar needed) ==\n";

    const char* names[] = { "E2", "A2", "D3", "G3", "B3", "E4" };
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        float worst = 0.0f;
        for (int s = 0; s < 6; ++s)
        {
            fx::PluckedString string;
            string.prepare (rate);
            juce::Random rng (s);
            string.pluck (fx::openStringHz[s], 0.35f, 3.5f, 0.75f, rng);

            std::vector<float> audio ((size_t) (rate * 0.4));
            for (auto& x : audio)
                x = string.next();

            fx::PitchDetector detector;
            detector.prepare (rate);
            detector.pushSamples (audio.data() + (size_t) (rate * 0.1), (int) (rate * 0.3)); // after the pick attack
            const float hz = detector.detect();
            const float cents = hz > 0.0f ? 1200.0f * std::log2 (hz / fx::openStringHz[s]) : 9999.0f;
            worst = std::max (worst, std::abs (cents));

            if (rate == 48000.0)
                std::cout << "  pluck " << names[s] << ": tuner reads " << juce::String (hz, 2) << " Hz ("
                          << juce::String (cents, 2) << " cents)\n";
        }
        std::cout << "  worst @ " << rate << " Hz: " << juce::String (worst, 2) << " cents\n";
        check (worst < 2.0f, "plucked strings must be in tune @ " + juce::String (rate));
    }

    const auto riff = fx::renderDemoRiff (testRate);
    float peak = 0.0f, maxStep = 0.0f;
    for (size_t i = 0; i < riff.size(); ++i)
    {
        peak = std::max (peak, std::abs (riff[i]));
        if (i > 0) maxStep = std::max (maxStep, std::abs (riff[i] - riff[i - 1]));
    }
    const float seamStep = std::abs (riff.front() - riff.back());
    std::cout << "  demo riff: " << juce::String (riff.size() / testRate, 1) << " s, peak "
              << juce::String (fx::gainToDb (peak), 1) << " dBFS, loop seam step " << juce::String (seamStep, 4)
              << " (largest step inside: " << juce::String (maxStep, 4) << ")\n";
    check (std::abs (fx::gainToDb (peak) + 8.0f) < 0.5f, "demo riff level");
    check (seamStep <= maxStep, "demo riff loops without a click");

    juce::AudioBuffer<float> dry (1, (int) riff.size());
    std::copy (riff.begin(), riff.end(), dry.getWritePointer (0));
    writeWav (outDir.getChildFile ("demo_riff_dry.wav"), dry);
}

//==============================================================================
/** The player the app uses for DEMO RIFF / AUDIO FILE / PLUCK, driven block by block like the audio thread. */
void testPlayer (const juce::File& outDir)
{
    std::cout << "\n== Test signal player (as used by the app) ==\n";

    auto run = [] (TestSignalPlayer& player, int numSamples, float liveInput)
    {
        std::vector<float> out ((size_t) numSamples, 0.0f);
        for (int pos = 0; pos < numSamples; pos += blockSize)
        {
            const int n = std::min (blockSize, numSamples - pos);
            std::fill_n (out.data() + pos, n, liveInput); // pretend this is the interface input
            player.process (out.data() + pos, n);
        }
        return out;
    };

    auto detect = [] (const std::vector<float>& audio, double rate, double fromSeconds)
    {
        fx::PitchDetector d;
        d.prepare (rate);
        const auto from = (size_t) (fromSeconds * rate);
        d.pushSamples (audio.data() + from, (int) (audio.size() - from));
        return d.detect();
    };

    TestSignalPlayer player;
    player.prepare (testRate);

    // LIVE leaves the input untouched
    const auto live = run (player, 4800, 0.25f);
    check (std::all_of (live.begin(), live.end(), [] (float x) { return x == 0.25f; }), "LIVE must pass the input through");

    // DEMO replaces the input with the riff, sample for sample, and ignores the interface
    player.setSource (TestSignalPlayer::demo);
    const auto riff = fx::renderDemoRiff (testRate);
    const auto demo = run (player, (int) riff.size() + 1000, 0.9f);
    bool demoMatches = true;
    for (size_t i = 0; i < demo.size(); ++i)
        demoMatches = demoMatches && demo[i] == riff[i % riff.size()];
    std::cout << "  DEMO RIFF: output == built-in riff, looping: " << (demoMatches ? "yes" : "NO") << "\n";
    check (demoMatches, "DEMO must play the riff and ignore the live input");

    // STOP silences the demo (plucked strings still sound); PLAY starts again from the top
    {
        player.setPlaying (false);
        const auto stopped = run (player, 4800, 0.9f);
        const bool silent = std::all_of (stopped.begin(), stopped.end(), [] (float x) { return x == 0.0f; });
        player.setPlaying (true);
        const auto restarted = run (player, 256, 0.9f);
        std::cout << "  STOP: silent " << (silent ? "yes" : "NO") << ", PLAY restarts from the top: "
                  << (restarted[0] == riff[0] && restarted[255] == riff[255] ? "yes" : "NO") << "\n";
        check (silent, "STOP must silence the demo riff");
        check (restarted[0] == riff[0] && restarted[255] == riff[255], "PLAY must restart the riff from the top");

        TestSignalPlayer stoppedPlayer; // separate player: a ringing string would disturb the tuner test below
        stoppedPlayer.prepare (testRate);
        stoppedPlayer.setSource (TestSignalPlayer::demo);
        stoppedPlayer.setPlaying (false);
        stoppedPlayer.pluck (0);
        const auto plucked = run (stoppedPlayer, 4800, 0.0f);
        check (std::any_of (plucked.begin(), plucked.end(), [] (float x) { return x != 0.0f; }), "plucks must sound while stopped");
        stoppedPlayer.setSource (TestSignalPlayer::demo);
        check (stoppedPlayer.isPlaying(), "choosing DEMO RIFF starts it again");
    }

    // CHORD: every shape must contain exactly its chord's notes, and strumming must sound
    {
        struct Expect { const char* name; std::vector<int> pitchClasses; }; // C = 0
        const Expect expected[] = { { "C", { 0, 4, 7 } }, { "D", { 2, 6, 9 } }, { "E", { 4, 8, 11 } }, { "F", { 5, 9, 0 } },
                                    { "G", { 7, 11, 2 } }, { "A", { 9, 1, 4 } }, { "Am", { 9, 0, 4 } }, { "Dm", { 2, 5, 9 } },
                                    { "Em", { 4, 7, 11 } } };
        const int openMidi[] = { 40, 45, 50, 55, 59, 64 };
        bool allRight = true;
        float worstCents = 0.0f;

        for (int c = 0; c < fx::numBasicChords; ++c)
        {
            const auto& chord = fx::basicChords[c];
            std::set<int> got;
            for (int s = 0; s < 6; ++s)
            {
                if (chord.frets[s] < 0) continue;
                const int midi = openMidi[s] + chord.frets[s];
                got.insert (midi % 12);
                const float equalTempered = 440.0f * std::pow (2.0f, (float) (midi - 69) / 12.0f);
                worstCents = std::max (worstCents, std::abs (1200.0f * std::log2 (fx::fretHz (s, chord.frets[s]) / equalTempered)));
            }
            const std::set<int> want (expected[c].pitchClasses.begin(), expected[c].pitchClasses.end());
            const bool right = juce::String (chord.name) == expected[c].name && got == want;
            allRight = allRight && right;
            check (right, juce::String ("chord shape ") + chord.name);
        }
        std::cout << "  CHORD: all " << fx::numBasicChords << " shapes have the right notes: " << (allRight ? "yes" : "NO")
                  << ", worst tuning " << juce::String (worstCents, 2) << " cents" << "\n";
        check (worstCents < 0.5f, "chord notes in tune");

        TestSignalPlayer strummer;
        strummer.prepare (testRate);
        strummer.strum (4); // G
        const auto strummed = run (strummer, (int) testRate, 0.0f);
        float first = -1.0f, peak = 0.0f;
        for (size_t i = 0; i < strummed.size(); ++i)
        {
            if (first < 0.0f && std::abs (strummed[i]) > 0.0f) first = (float) i;
            peak = std::max (peak, std::abs (strummed[i]));
        }
        std::cout << "  strum G: starts at sample " << first << ", peak " << juce::String (fx::gainToDb (peak), 1) << " dBFS" << "\n";
        check (first == 0.0f && peak > 0.1f && peak < 1.0f, "strummed chord sounds at a sensible level");
    }

    // CHORD loop: notes land exactly on the eighth-note grid at the chosen tempo
    {
        // pluck onsets = bursts of high-frequency energy, found in 2 ms frames
        auto onsets = [] (const std::vector<float>& x)
        {
            const int frame = (int) (0.002 * testRate);
            std::vector<double> e (x.size() / (size_t) frame, 0.0);
            for (size_t k = 0; k < e.size(); ++k)
                for (int i = 1; i < frame; ++i)
                {
                    const double d = x[k * (size_t) frame + (size_t) i] - x[k * (size_t) frame + (size_t) i - 1];
                    e[k] += d * d;
                }
            const double top = *std::max_element (e.begin(), e.end());
            std::vector<double> times;
            for (size_t k = 0; k < e.size(); ++k) // from frame 0: the loop's first note starts at t = 0
            {
                double before = 1.0e-12;
                for (size_t j = k < 5 ? 0 : k - 5; j < k; ++j) before = std::max (before, e[j]);
                if (e[k] > 3.0 * before && e[k] > 0.02 * top && (times.empty() || k * 0.002 - times.back() > 0.08))
                    times.push_back ((double) k * 0.002);
            }
            return times;
        };

        // arpeggio: every eighth note; strum loop: one strum on every beat
        struct Case { const char* name; int pattern; float bpm; int expectedPerBar; double gridBeats; };
        for (const auto& c : { Case { "arpeggio @ 120 BPM", TestSignalPlayer::arpeggio, 120.0f, 8, 0.5 },
                               Case { "arpeggio @ 75 BPM", TestSignalPlayer::arpeggio, 75.0f, 8, 0.5 },
                               Case { "strum loop @ 120 BPM", TestSignalPlayer::strumLoop, 120.0f, 4, 1.0 },
                               Case { "strum loop @ 75 BPM", TestSignalPlayer::strumLoop, 75.0f, 4, 1.0 } })
        {
            TestSignalPlayer p;
            p.prepare (testRate);
            p.setChordPattern (c.pattern);
            p.setChordTempo (c.bpm);
            p.playChord (4); // G
            const double bar = 4.0 * 60.0 / c.bpm;
            const auto audio = run (p, (int) (testRate * bar * 2.0), 0.0f);
            const auto t = onsets (audio);

            // every onset must sit on its grid (eighth notes or beats, within 3 ms)
            const double grid = c.gridBeats * 60.0 / c.bpm;
            double worst = 0.0;
            for (double time : t)
                worst = std::max (worst, std::abs (time - grid * std::round (time / grid)));
            std::cout << "  " << juce::String (c.name).paddedRight (' ', 22) << (int) t.size() << " notes in 2 bars (expected "
                      << 2 * c.expectedPerBar << "), worst timing error " << juce::String (worst * 1000.0, 1) << " ms" << "\n";
            check ((int) t.size() == 2 * c.expectedPerBar, juce::String ("note count: ") + c.name);
            check (worst < 0.003, juce::String ("timing: ") + c.name);
        }

        // a chord change lands on the next eighth note, and STOP silences the strings
        TestSignalPlayer p;
        p.prepare (testRate);
        p.setChordTempo (120.0f);
        p.playChord (0);
        auto a = run (p, (int) (testRate * 0.6), 0.0f);   // 0.6 s: between eighths (0.5 and 0.75)
        p.playChord (4);
        auto b = run (p, (int) (testRate * 0.6), 0.0f);
        a.insert (a.end(), b.begin(), b.end());
        const auto t = onsets (a);
        const bool onGrid = std::none_of (t.begin(), t.end(), [] (double x) { return std::abs (x - 0.6) < 0.05; });
        p.stopChord();
        run (p, (int) (testRate * 0.3), 0.0f);
        const auto after = run (p, (int) (testRate * 0.2), 0.0f);
        float tail = 0.0f;
        for (auto x : after) tail = std::max (tail, std::abs (x));
        std::cout << "  chord change waits for the next eighth: " << (onGrid ? "yes" : "NO")
                  << ", 0.3 s after STOP: " << juce::String (fx::gainToDb (tail), 1) << " dBFS" << "\n";
        check (onGrid, "chord change is quantised to the eighth-note grid");
        check (tail < 1.0e-4f, "STOP mutes the chord loop");

        // strum loop: a chord asked for between beats comes in on the next beat, not on the "and"
        TestSignalPlayer s;
        s.prepare (testRate);
        s.setChordPattern (TestSignalPlayer::strumLoop);
        s.setChordTempo (120.0f);
        s.playChord (0);
        auto sa = run (s, (int) (testRate * 0.6), 0.0f); // beats at 0, 0.5, 1.0 s; the "and" at 0.75 s
        s.playChord (4);
        auto sb = run (s, (int) (testRate * 0.6), 0.0f);
        sa.insert (sa.end(), sb.begin(), sb.end());
        const auto st = onsets (sa);
        const bool offBeat = std::any_of (st.begin(), st.end(), [] (double x) { return x > 0.55 && x < 0.95; });
        const bool onBeat = std::any_of (st.begin(), st.end(), [] (double x) { return std::abs (x - 1.0) < 0.01; });
        std::cout << "  strum loop: chord change comes in on the next beat: " << (onBeat && ! offBeat ? "yes" : "NO") << "\n";
        check (onBeat && ! offBeat, "strum loop: chord change waits for the next beat");
    }

    // PLUCK on silence: the tuner must read an in-tune A
    player.setSource (TestSignalPlayer::live);
    player.pluck (1);
    const auto plucked = run (player, (int) (testRate * 0.5), 0.0f);
    const float hz = detect (plucked, testRate, 0.1);
    std::cout << "  PLUCK A: tuner reads " << juce::String (hz, 2) << " Hz (expected 110.00)\n";
    check (std::abs (1200.0f * std::log2 (hz / 110.0f)) < 2.0f, "PLUCK A must be in tune");

    // AUDIO FILE recorded at 44.1 kHz must keep its pitch when the device runs at 48 kHz
    const auto wavFile = outDir.getChildFile ("player_test_196Hz_44k1.wav");
    {
        juce::AudioBuffer<float> tone (2, 44100 * 2);
        for (int i = 0; i < tone.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                tone.setSample (ch, i, 0.3f * (float) std::sin (2.0 * fx::pi * 196.0 * i / 44100.0));

        wavFile.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (wavFile);
        juce::WavAudioFormat wav;
        if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (44100.0)
                                                                                         .withNumChannels (2).withBitsPerSample (24)))
            writer->writeFromAudioSampleBuffer (tone, 0, tone.getNumSamples());
    }

    TestSignalPlayer filePlayer; // fresh player: the A string plucked above would still be ringing
    filePlayer.prepare (testRate);
    const auto error = filePlayer.loadFile (wavFile);
    check (error.isEmpty(), "loading a WAV: " + error);
    check (filePlayer.getSource() == TestSignalPlayer::file, "loading a file switches to AUDIO FILE");

    const auto filePlayback = run (filePlayer, (int) (testRate * 3.0), 0.9f); // 3 s: loops past the 2 s file end
    const float fileHz = detect (filePlayback, testRate, 2.4);
    float filePeak = 0.0f;
    for (auto x : filePlayback) filePeak = std::max (filePeak, std::abs (x));
    std::cout << "  AUDIO FILE (44.1 kHz file on a 48 kHz device, after looping): " << juce::String (fileHz, 2)
              << " Hz (expected 196.00), peak " << juce::String (filePeak, 3) << " (expected 0.300)\n";
    check (std::abs (1200.0f * std::log2 (fileHz / 196.0f)) < 2.0f, "file playback keeps its pitch");
    check (std::abs (filePeak - 0.3f) < 0.01f, "file playback keeps its level");

    check (player.loadFile (outDir.getChildFile ("does_not_exist.wav")).isNotEmpty(), "missing file reports an error");
    wavFile.deleteFile();
}

//==============================================================================
void testTapTempo()
{
    std::cout << "\n== Tap tempo ==\n";

    auto report = [] (const char* what, double beatMs, double expectedMs)
    {
        std::cout << "  " << juce::String (what).paddedRight (' ', 44) << juce::String (beatMs, 1) << " ms"
                  << (beatMs > 0.0 ? " = " + juce::String (60000.0 / beatMs, 1) + " BPM" : juce::String()) << "\n";
        check (std::abs (beatMs - expectedMs) < 0.5 + 0.02 * expectedMs, juce::String ("tap tempo: ") + what);
    };

    {
        fx::TapTempo t;
        report ("first tap (no tempo yet)", t.tap (1000.0), 0.0);
        double beat = 0.0;
        for (int i = 1; i <= 4; ++i)
            beat = t.tap (1000.0 + 500.0 * i);
        report ("4 more taps every 500 ms", beat, 500.0);
    }
    {
        fx::TapTempo t;
        const double jitter[] = { 0.0, 18.0, -12.0, 9.0, -20.0, 14.0 };
        double beat = 0.0;
        for (int i = 0; i < 6; ++i)
            beat = t.tap (400.0 * i + jitter[i]);
        report ("sloppy taps around 400 ms (+-20 ms)", beat, 400.0);
    }
    {
        fx::TapTempo t;
        double beat = 0.0;
        for (int i = 0; i < 4; ++i) beat = t.tap (600.0 * i);
        beat = t.tap (1800.0 + 3000.0); // long pause: starts a new sequence
        report ("tap after a 3 s pause (starts over)", beat, 0.0);
        beat = t.tap (4800.0 + 300.0);
        report ("...then one more tap 300 ms later", beat, 300.0);
    }
    {
        fx::TapTempo t;
        double beat = 0.0;
        for (int i = 0; i < 4; ++i) beat = t.tap (500.0 * i);
        beat = t.tap (1500.0 + 250.0); // twice as fast: old intervals are dropped
        report ("tempo doubles (500 -> 250 ms)", beat, 250.0);
        beat = t.tap (1750.0 + 30.0); // switch bounce is ignored
        report ("bounce 30 ms later is ignored", beat, 250.0);
    }
}

/** Reverb pre-delay must hold the reverb back by exactly the set time. */
void testPreDelay()
{
    std::cout << "\n== Reverb pre-delay ==\n";

    auto onsetMs = [] (float preDelayMs)
    {
        std::vector<float> click ((size_t) testRate, 0.0f);
        click[100] = 1.0f;

        auto p = allOff();
        put (p, 0, "room_reverb", { { "Mix", 100.0f }, { "Pre-Delay", preDelayMs }, { "Note", 0.0f } });
        const auto r = render (click, p);

        // first sample of reverb (skip the dry click itself)
        for (int i = 102; i < r.audio.getNumSamples(); ++i)
            if (std::abs (r.audio.getSample (0, i)) > 1.0e-4f)
                return (i - 100) * 1000.0 / testRate;
        return -1.0;
    };

    const double base = onsetMs (0.0f); // Freeverb's own comb delay
    for (float pre : { 62.5f, 125.0f, 250.0f, 500.0f })
    {
        const double measured = onsetMs (pre) - base;
        std::cout << "  pre-delay " << juce::String (pre, 1).paddedLeft (' ', 5) << " ms -> reverb starts "
                  << juce::String (measured, 2) << " ms later than without" << "\n";
        check (std::abs (measured - pre) < 0.5, "reverb pre-delay " + juce::String (pre));
    }
}

//==============================================================================
void testTuner()
{
    std::cout << "\n== Tuner (YIN) ==\n";

    struct Case { const char* name; double hz; };
    const Case cases[] = {
        { "Drop D  (D2)",   73.42 }, { "Low E   (E2)",  82.41 }, { "A       (A2)", 110.00 },
        { "D       (D3)",  146.83 }, { "G       (G3)", 196.00 }, { "B       (B3)", 246.94 },
        { "High E  (E4)",  329.63 }, { "12th fret E5", 659.26 }, { "E2 +12 cents", 82.41 * std::pow (2.0, 12.0 / 1200.0) },
        { "A2 -23 cents",  110.0 * std::pow (2.0, -23.0 / 1200.0) },
    };

    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        float worstCents = 0.0f;

        for (const auto& c : cases)
        {
            fx::PitchDetector detector;
            detector.prepare (rate);

            // plucked-string-like tone: rich harmonics, decaying, with a strong 2nd harmonic
            juce::Random rng (7);
            std::vector<float> tone ((size_t) (rate * 0.25));
            for (size_t i = 0; i < tone.size(); ++i)
            {
                const double t = (double) i / rate;
                double v = 0.0;
                for (int h = 1; h <= 12; ++h)
                    v += (h == 2 ? 0.9 : 1.0 / h) * std::sin (2.0 * fx::pi * c.hz * h * t + 0.3 * h);
                tone[i] = (float) (0.2 * v * std::exp (-t * 2.0)) + 0.001f * (rng.nextFloat() - 0.5f);
            }

            detector.pushSamples (tone.data(), (int) tone.size());
            const float hz = detector.detect();

            const float centsError = hz > 0.0f ? 1200.0f * std::log2 (hz / (float) c.hz) : 9999.0f;
            worstCents = std::max (worstCents, std::abs (centsError));

            if (rate == 48000.0)
            {
                const auto note = fx::frequencyToNote (hz);
                std::cout << "  " << c.name << "  expected " << juce::String (c.hz, 2) << " Hz  got "
                          << juce::String (hz, 2) << " Hz  -> " << fx::noteName (note.midiNote)
                          << fx::noteOctave (note.midiNote) << " " << juce::String (note.cents, 1)
                          << " cents  (error " << juce::String (centsError, 2) << " cents)\n";
            }

            check (std::abs (centsError) < 1.0f, juce::String ("tuner accuracy for ") + c.name + " @ " + juce::String (rate));
        }

        std::cout << "  worst error @ " << rate << " Hz: " << juce::String (worstCents, 2) << " cents\n";
    }

    fx::PitchDetector silent;
    silent.prepare (48000.0);
    std::vector<float> quiet (4800, 0.0f);
    silent.pushSamples (quiet.data(), (int) quiet.size());
    check (silent.detect() == 0.0f, "tuner reports nothing for silence");
}

//==============================================================================
/** Reference data for the web version's port: the test input, each static render, the exact
    parameters used, and JUCE's oversampling filter coefficients. web/test/compare.mjs replays the
    same input through the JavaScript DSP and checks it matches sample for sample. */
juce::var paramsToVar (const fx::FxParams& p)
{
    auto* eq = new juce::DynamicObject();
    eq->setProperty ("lowCutHz", p.eq.lowCutHz);
    eq->setProperty ("bassHz", p.eq.bassHz);        eq->setProperty ("bassDb", p.eq.bassDb);
    eq->setProperty ("lowMidHz", p.eq.lowMidHz);    eq->setProperty ("lowMidDb", p.eq.lowMidDb);   eq->setProperty ("lowMidQ", p.eq.lowMidQ);
    eq->setProperty ("highMidHz", p.eq.highMidHz);  eq->setProperty ("highMidDb", p.eq.highMidDb); eq->setProperty ("highMidQ", p.eq.highMidQ);
    eq->setProperty ("trebleHz", p.eq.trebleHz);    eq->setProperty ("trebleDb", p.eq.trebleDb);
    eq->setProperty ("highCutHz", p.eq.highCutHz);

    juce::Array<juce::var> slots;
    for (const auto& slot : p.slots)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("on", slot.on);
        o->setProperty ("model", juce::String (fx::modelInfo (slot.model).key));
        juce::Array<juce::var> knobs;
        for (float k : slot.knobs)
            knobs.add (k);
        o->setProperty ("knobs", knobs);
        slots.add (juce::var (o));
    }

    auto* o = new juce::DynamicObject();
    o->setProperty ("inputGainDb", p.inputGainDb);   o->setProperty ("outputGainDb", p.outputGainDb);
    o->setProperty ("mute", p.mute);

    auto* amp = new juce::DynamicObject();
    amp->setProperty ("on", p.amp.on);
    amp->setProperty ("amp", p.amp.amp >= 0 ? juce::String (fx::amps()[(size_t) p.amp.amp].key) : juce::String());
    amp->setProperty ("cab", p.amp.cab >= 0 ? juce::String (fx::cabs()[(size_t) p.amp.cab].key) : juce::String());
    juce::Array<juce::var> ampKnobs, cabKnobs;
    for (float k : p.amp.ampKnobs) ampKnobs.add (k);
    for (float k : p.amp.cabKnobs) cabKnobs.add (k);
    amp->setProperty ("ampKnobs", ampKnobs);
    amp->setProperty ("cabKnobs", cabKnobs);
    o->setProperty ("amp", juce::var (amp));
    o->setProperty ("ampPosition", p.ampPosition);
    o->setProperty ("humMode", p.humMode);
    o->setProperty ("denoise", p.denoise);
    o->setProperty ("eqOn", p.eqOn);                 o->setProperty ("eq", juce::var (eq));
    o->setProperty ("slots", slots);
    return juce::var (o);
}

/** A model list, so the web version can check that its copy matches. */
juce::var modelsToVar (const std::vector<fx::ModelInfo>& models)
{
    juce::Array<juce::var> list;
    for (const auto& m : models)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("key", juce::String (m.key));
        o->setProperty ("name", juce::String (m.name));
        o->setProperty ("category", (int) m.category);
        o->setProperty ("engine", (int) m.engine);
        o->setProperty ("variant", m.variant);
        o->setProperty ("basedOn", juce::String (m.basedOn));
        o->setProperty ("timeKnob", m.timeKnob);
        o->setProperty ("noteKnob", m.noteKnob);
        o->setProperty ("timeKnob2", m.timeKnob2);
        o->setProperty ("noteKnob2", m.noteKnob2);
        o->setProperty ("stereo", m.stereo);
        o->setProperty ("trails", m.trails);

        juce::Array<juce::var> knobs;
        for (const auto& k : m.knobs)
        {
            auto* ko = new juce::DynamicObject();
            ko->setProperty ("name", juce::String (k.name));
            ko->setProperty ("min", k.min);
            ko->setProperty ("max", k.max);
            ko->setProperty ("def", k.def);
            ko->setProperty ("centre", k.centre);
            ko->setProperty ("step", k.step);
            ko->setProperty ("unit", (int) k.unit);
            juce::Array<juce::var> choices;
            if (k.unit == fx::Unit::choice)
                for (int c = 0; c <= (int) k.max; ++c)
                    choices.add (juce::String (k.choices[c]));
            ko->setProperty ("choices", choices);
            knobs.add (juce::var (ko));
        }
        o->setProperty ("knobs", knobs);
        list.add (juce::var (o));
    }
    return list;
}

juce::var oversamplerCoefficients (float transitionWidth, float stopbandDb)
{
    const auto design = juce::dsp::FilterDesign<float>::designIIRLowpassHalfBandPolyphaseAllpassMethod (transitionWidth, stopbandDb);
    juce::Array<juce::var> coefficients; // same order the JUCE stage uses: direct path, then delayed path minus its pure delay
    for (int i = 0; i < design.directPath.size(); ++i)
        coefficients.add (design.directPath.getObjectPointer (i)->coefficients[0]);
    for (int i = 1; i < design.delayedPath.size(); ++i)
        coefficients.add (design.delayedPath.getObjectPointer (i)->coefficients[0]);
    return coefficients;
}

void writeFloats (const juce::File& file, const float* data, size_t count)
{
    file.deleteFile();
    juce::FileOutputStream out (file);
    out.write (data, count * sizeof (float));
}

} // namespace

//==============================================================================
int main (int argc, char* argv[])
{
    juce::ScopedNoDenormals noDenormals;

    const auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "test_output");
    outDir.createDirectory();

    const auto riff = makeRiff();

    // Presets store models by key: every key must be there exactly once in its list.
    // Menus show names: two models of one category must not share a name either.
    for (const auto* list : { &fx::models(), &fx::amps(), &fx::cabs() })
        for (size_t i = 0; i < list->size(); ++i)
        {
            check (fx::findIn (*list, (*list)[i].key) == (int) i, juce::String ("model key is used twice: ") + (*list)[i].key);
            for (size_t j = 0; j < i; ++j)
                check ((*list)[j].category != (*list)[i].category || juce::String ((*list)[j].name) != (*list)[i].name,
                       juce::String ("two models are called ") + (*list)[i].name);
        }

    // A case either has fixed parameters, an automation (C++ only), or a schedule of parameter
    // changes (exported, so the web version can replay the same switching sequence).
    struct Scheduled { int atSample; fx::FxParams params; };
    struct Case
    {
        juce::String name;
        fx::FxParams params;
        std::function<void (fx::FxParams&, double)> automation;
        std::vector<Scheduled> schedule;
    };
    std::vector<Case> cases;

    auto add = [&] (juce::String name, std::function<void (fx::FxParams&)> setup, std::function<void (fx::FxParams&, double)> automation = {})
    {
        auto p = allOff();
        setup (p);
        cases.push_back ({ name, p, automation, {} });
    };

    add ("00_clean",    [] (auto&) {});
    add ("01_cab_only", [] (auto& p) { p.amp.on = true; });

    // every distortion model at its default settings, as on the board: gate > model > cab
    for (const auto& m : fx::models())
        if (m.category == fx::Category::distortion)
            add (juce::String ("drv_") + m.key, [key = m.key] (auto& p) { p.amp.on = true; put (p, 0, "noise_gate"); put (p, 1, key); });

    // every model of the fx/ engines at its default settings, alone in the first slot
    for (const auto& m : fx::models())
        if ((int) m.engine >= (int) fx::Engine::dynamicsFx)
            add (juce::String ("fx_") + m.key, [key = m.key] (auto& p) { put (p, 0, key); });

    // every amp through the cabinet it is usually played with, and every cabinet on its own
    for (const auto& m : fx::amps())
        add (juce::String ("amp_") + m.key, [key = m.key] (auto& p)
        {
            const auto usual = fx::defaultCabFor (key);
            p.amp = fx::makeAmp (key, usual.empty() ? std::string_view (fx::defaultCabKey) : usual, true);
        });

    for (const auto& m : fx::cabs())
        add (juce::String (m.key), [key = m.key] (auto& p) { p.amp = fx::makeAmp ("", key, true); });

    auto driven = [] (const char* key, std::initializer_list<std::pair<const char*, float>> knobs)
    {
        return [key, knobs = std::vector<std::pair<const char*, float>> (knobs)] (fx::FxParams& p)
        {
            p.amp.on = true;
            put (p, 0, "noise_gate");
            auto& slot = put (p, 1, key);
            const auto& specs = fx::modelInfo (slot.model).knobs;
            for (const auto& [name, value] : knobs)
                for (size_t k = 0; k < specs.size(); ++k)
                    if (juce::String (specs[k].name) == name)
                        slot.knobs[k] = value;
        };
    };
    add ("max_screamer",     driven ("screamer",     { { "Drive", 100.0f }, { "Tone", 100.0f } }));
    add ("max_rat",          driven ("classic_dist", { { "Drive", 100.0f }, { "Filter", 0.0f } }));
    add ("max_heavy",        driven ("heavy_dist",   { { "Drive", 100.0f }, { "Bass", 100.0f }, { "Mid", 0.0f }, { "Treble", 100.0f } }));
    add ("max_fuzz_pi",      driven ("fuzz_pi",      { { "Drive", 100.0f }, { "Mid", 0.0f } }));
    add ("max_facial",       driven ("facial_fuzz",  { { "Drive", 100.0f }, { "Output", 12.0f } }));
    add ("max_jet",          driven ("jet_fuzz",     { { "Drive", 100.0f }, { "Speed", 4.0f }, { "Fdbk", 100.0f }, { "Tone", 100.0f } }));
    add ("max_sub_octave",   driven ("sub_oct_fuzz", { { "Drive", 30.0f }, { "Sub", 100.0f } }));
    add ("line6_drive_fuzz", driven ("line6_drive",  { { "Mid", 0.0f } }));
    add ("line6_drive_grit", driven ("line6_drive",  { { "Mid", 100.0f }, { "Treble", 80.0f } }));
    add ("min_drive_eq",     driven ("overdrive",    { { "Drive", 0.0f }, { "Bass", 0.0f }, { "Mid", 100.0f }, { "Treble", 0.0f } }));

    add ("mod_chorus",   [] (auto& p) { put (p, 0, "chorus"); });
    add ("mod_flanger",  [] (auto& p) { put (p, 0, "flanger", { { "Speed", 0.3f }, { "Depth", 80.0f } }); });
    add ("mod_phaser",   [] (auto& p) { put (p, 0, "phaser",  { { "Depth", 80.0f } }); });
    add ("mod_tremolo",  [] (auto& p) { put (p, 0, "tremolo", { { "Speed", 5.0f }, { "Depth", 80.0f } }); });
    add ("dly_analog",   [] (auto& p) { put (p, 0, "analog_delay"); }); // 1/4 at 120 BPM = 500 ms
    add ("dly_max_fb",   [] (auto& p) { put (p, 0, "analog_delay", { { "Note", 0.0f }, { "Time", 120.0f }, { "Feedback", 95.0f },
                                                                      { "Mix", 100.0f }, { "Tone", 10.0f } }); });
    add ("rev_room",     [] (auto& p) { put (p, 0, "room_reverb"); });
    add ("rev_max",      [] (auto& p) { put (p, 0, "room_reverb", { { "Size", 100.0f }, { "Mix", 100.0f }, { "Damp", 0.0f } }); });
    add ("rev_predelay", [] (auto& p) { put (p, 0, "room_reverb", { { "Pre-Delay", 125.0f }, { "Mix", 60.0f } }); });

    add ("default_board", [] (auto& p) { p = defaultBoardAt120(); });
    add ("noise_hum_60", [] (auto& p) { p.humMode = fx::HumFilter::hz60; p.amp.on = true; put (p, 0, "classic_dist"); });
    add ("noise_hum_50", [] (auto& p) { p.humMode = fx::HumFilter::hz50; });
    add ("noise_denoise", [] (auto& p) { p.denoise = 60.0f; p.amp.on = true; put (p, 0, "heavy_dist"); });
    add ("full_board", [] (auto& p)
    {
        p.amp.on = true;
        put (p, 0, "noise_gate");
        put (p, 1, "classic_dist");
        put (p, 2, "chorus");
        put (p, 3, "analog_delay");
        put (p, 4, "room_reverb");
    });
    add ("eq_smile", [] (auto& p) { p.amp.on = true; p.eqOn = true; put (p, 0, "noise_gate"); put (p, 1, "classic_dist"); p.eq = smileEq(); });
    add ("overdrive_hot", [] (auto& p)
    {
        p.inputGainDb = 6.0f;
        p.outputGainDb = -2.0f;
        p.eqOn = true;
        p.eq.highMidDb = 4.0f;
        put (p, 0, "screamer", { { "Drive", 80.0f }, { "Tone", 30.0f }, { "Output", -3.0f } });
    });
    add ("amp_first",        [] (auto& p) { p.amp.on = true; p.ampPosition = 0; put (p, 0, "screamer"); });
    add ("stereo_into_mono", [] (auto& p) { put (p, 0, "chorus"); put (p, 1, "fuzz_pi"); });
    add ("last_slot",        [] (auto& p) { p.amp.on = true; p.ampPosition = 8; put (p, 7, "room_reverb"); put (p, 6, "tube_drive"); });

    add ("stomping", [] (auto& p) { p.amp.on = true; put (p, 0, "noise_gate"); },
         [] (fx::FxParams& p, double t)
         {
             // flip slots on and off, swap models and sweep the delay time while playing, to catch clicks/glitches
             const int step = (int) (t / 0.35);
             const char* drives[] = { "screamer", "fuzz_pi", "heavy_dist" };
             const char* mods[] = { "chorus", "flanger", "phaser", "tremolo" };
             put (p, 1, drives[step % 3], {}, (step % 2) == 1);
             put (p, 2, mods[step % 4], {}, (step % 3) == 1);
             put (p, 3, "analog_delay", { { "Note", 0.0f }, { "Time", 100.0f + 400.0f * (float) (0.5 + 0.5 * std::sin (t)) } }, (step % 4) < 2);
             put (p, 4, "room_reverb", {}, (step % 5) < 3);
         });

    {
        // A scripted session: models swapped, slots toggled and the amp block moved, every 0.25 s.
        Case c { "switching", {}, {}, {} };
        c.params = allOff();
        c.params.amp.on = true;
        put (c.params, 0, "noise_gate");

        const char* sequence[] = { "screamer", "fuzz_pi", "chorus", "analog_delay", "empty", "heavy_dist", "room_reverb",
                                   "sub_oct_fuzz", "phaser", "jet_fuzz", "tremolo", "octave_fuzz", "flanger", "buzz_saw" };
        for (int step = 1; step * 0.25 < 7.0; ++step)
        {
            auto p = c.params;
            put (p, 1, sequence[step % (int) std::size (sequence)], {}, step % 5 != 3);
            put (p, 2, sequence[(step + 5) % (int) std::size (sequence)], {}, step % 3 != 0);
            p.ampPosition = step % 7 == 2 ? 0 : 2;
            p.amp.on = step % 11 != 6;
            c.schedule.push_back ({ (int) (step * 0.25 * testRate), p });
        }

        c.automation = [schedule = c.schedule] (fx::FxParams& p, double t)
        {
            for (const auto& s : schedule)
                if (t * testRate >= s.atSample)
                    p = s.params;
        };
        cases.push_back (c);
    }

    std::cout << "== Effect renders (" << outDir.getFullPathName() << ") ==\n";
    std::cout << "  name                 peak dBFS   rms dBFS\n";

    const auto refDir = outDir.getChildFile ("reference");
    refDir.createDirectory();
    for (const auto& old : refDir.findChildFiles (juce::File::findFiles, false, "*.f32"))
        old.deleteFile();
    writeFloats (refDir.getChildFile ("input.f32"), riff.data(), riff.size());
    juce::Array<juce::var> referenceCases;

    float cleanRms = 1.0f;
    std::vector<std::pair<juce::String, float>> driveLevels, ampLevels;
    for (auto& c : cases)
    {
        auto r = render (riff, c.params, c.automation);
        writeWav (outDir.getChildFile (c.name + ".wav"), r.audio);

        if (! c.automation || ! c.schedule.empty())
        {
            // the per-model cases only keep their first 4 seconds as reference data (there are many of them)
            const bool perModel = c.name.startsWith ("fx_") || c.name.startsWith ("amp_") || c.name.startsWith ("cab_");
            const int kept = perModel ? std::min (r.audio.getNumSamples(), (int) (4.0 * testRate)) : r.audio.getNumSamples();
            std::vector<float> interleaved ((size_t) kept * 2);
            for (int i = 0; i < kept; ++i)
            {
                interleaved[(size_t) i * 2]     = r.audio.getSample (0, i);
                interleaved[(size_t) i * 2 + 1] = r.audio.getSample (1, i);
            }
            writeFloats (refDir.getChildFile (c.name + ".f32"), interleaved.data(), interleaved.size());

            auto* entry = new juce::DynamicObject();
            entry->setProperty ("name", c.name);
            entry->setProperty ("params", paramsToVar (c.params));
            if (! c.schedule.empty())
            {
                juce::Array<juce::var> schedule;
                for (const auto& s : c.schedule)
                {
                    auto* e = new juce::DynamicObject();
                    e->setProperty ("atSample", s.atSample);
                    e->setProperty ("params", paramsToVar (s.params));
                    schedule.add (juce::var (e));
                }
                entry->setProperty ("schedule", schedule);
            }
            referenceCases.add (juce::var (entry));
        }

        if (c.name == "00_clean")
            cleanRms = r.rms;

        const float vsClean = fx::gainToDb (r.rms / cleanRms);
        if (c.name.startsWith ("drv_"))
            driveLevels.push_back ({ c.name, vsClean });
        if (c.name.startsWith ("amp_"))
            ampLevels.push_back ({ c.name, vsClean });

        std::cout << "  " << c.name.paddedRight (' ', 20) << " "
                  << juce::String (fx::gainToDb (r.peak), 1).paddedLeft (' ', 9) << "  "
                  << juce::String (fx::gainToDb (r.rms), 1).paddedLeft (' ', 9)
                  << "   (" << juce::String (vsClean, 1) << " dB vs clean)\n";

        check (r.finite, c.name + " produced NaN/Inf");
        check (r.peak < 1.99f, c.name + " hit the safety clamp");
        check (r.rms > 1.0e-4f, c.name + " is silent");
    }

    // At their default settings, the distortion models should all be about equally loud
    // (so switching models doesn't jump in volume) and a little louder than the clean guitar.
    {
        std::vector<float> levels;
        for (const auto& [name, level] : driveLevels)
            levels.push_back (level);
        std::sort (levels.begin(), levels.end());
        const float median = levels[levels.size() / 2];
        std::cout << "\n== Distortion levels at default settings (median " << juce::String (median, 1) << " dB vs clean) ==\n";
        for (const auto& [name, level] : driveLevels)
        {
            const bool ok = std::abs (level - median) <= 3.0f;
            if (! ok)
                std::cout << "  " << name << ": " << juce::String (level, 1) << " dB\n";
            check (ok, name + " should be within 3 dB of the other distortion models");
        }
        check (median > 2.0f && median < 9.0f, "distortion models should be a few dB louder than clean");
    }

    // The amps, each at its own default settings, should come out about equally loud too
    {
        std::vector<float> levels;
        for (const auto& [name, level] : ampLevels)
            levels.push_back (level);
        std::sort (levels.begin(), levels.end());
        const float median = levels[levels.size() / 2];
        std::cout << "\n== Amp levels at default settings (median " << juce::String (median, 1) << " dB vs clean) ==\n";
        for (const auto& [name, level] : ampLevels)
        {
            const bool ok = std::abs (level - median) <= 4.0f;
            if (! ok)
                std::cout << "  " << name << ": " << juce::String (level, 1) << " dB\n";
            check (ok, name + " should be within 4 dB of the other amps");
        }
    }

    // Loudness across the Drive knob (informational, and checked: turning Drive should not jump the volume)
    {
        std::cout << "\n== Distortion loudness vs Drive (dB vs clean, gate > model > cab) ==\n"
                     "  model               0%    25%    50%    75%   100%\n";
        for (const auto& m : fx::models())
        {
            if (m.category != fx::Category::distortion)
                continue;
            juce::String line = "  " + juce::String (m.key).paddedRight (' ', 16);
            float lo = 1000.0f, hi = -1000.0f;
            for (float d : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
            {
                auto p = allOff();
                p.amp.on = true;
                put (p, 0, "noise_gate");
                put (p, 1, m.key, { { "Drive", d } });
                const float level = fx::gainToDb (render (riff, p).rms / cleanRms);
                lo = std::min (lo, level);
                hi = std::max (hi, level);
                line << juce::String (level, 1).paddedLeft (' ', 7);
            }
            std::cout << line << "\n";
            check (hi - lo < 9.0f, juce::String (m.key) + ": volume changes too much across the Drive knob");
        }
    }

    {
        auto* manifest = new juce::DynamicObject();
        manifest->setProperty ("sampleRate", testRate);
        manifest->setProperty ("blockSize", blockSize);
        manifest->setProperty ("cases", referenceCases);
        manifest->setProperty ("models", modelsToVar (fx::models()));
        manifest->setProperty ("amps", modelsToVar (fx::amps()));
        manifest->setProperty ("cabs", modelsToVar (fx::cabs()));

        // stage 0 (1x <-> 2x) and stage 1 (2x <-> 4x), exactly as juce::dsp::Oversampling(1, 2, polyphaseIIR, maxQuality)
        auto* os = new juce::DynamicObject();
        os->setProperty ("up",   juce::Array<juce::var> { oversamplerCoefficients (0.05f, -90.0f), oversamplerCoefficients (0.10f, -80.0f) });
        os->setProperty ("down", juce::Array<juce::var> { oversamplerCoefficients (0.06f, -75.0f), oversamplerCoefficients (0.12f, -65.0f) });
        manifest->setProperty ("oversampler", juce::var (os));

        refDir.getChildFile ("manifest.json").replaceWithText (juce::JSON::toString (juce::var (manifest)));
        std::cout << "  reference data for the web version: " << refDir.getFullPathName() << "\n";
    }

    // Noise gate: pickup hiss alone must be removed, a played note must pass.
    {
        std::vector<float> hiss ((size_t) testRate, 0.0f);
        juce::Random rng (99);
        for (auto& s : hiss) s = 0.0003f * (rng.nextFloat() * 2.0f - 1.0f);

        auto p = allOff();
        put (p, 0, "noise_gate");
        auto gated = render (hiss, p);
        std::cout << "\n== Noise gate ==\n  hiss in: -70 dBFS, out: " << juce::String (fx::gainToDb (gated.peak), 1) << " dBFS peak\n";
        check (gated.peak < 1.0e-5f, "gate should silence -70 dB hiss");

        auto played = render (riff, p);
        auto clean  = render (riff, allOff());
        std::cout << "  riff through gate: " << juce::String (fx::gainToDb (played.rms / clean.rms), 2) << " dB vs clean\n";
        check (std::abs (fx::gainToDb (played.rms / clean.rms)) < 0.5f, "gate should pass the riff untouched");
    }

    // Switching must not click: on a steady sine, the biggest sample-to-sample step while
    // switching may not exceed the biggest step of the steady sounds on either side.
    {
        std::cout << "\n== Click test (switched every 250 ms on a 220 Hz sine) ==\n";
        std::vector<float> sine ((size_t) (testRate * 3.0));
        for (size_t i = 0; i < sine.size(); ++i)
            sine[i] = 0.3f * (float) std::sin (2.0 * fx::pi * 220.0 * (double) i / testRate);

        auto maxStep = [] (const Render& r)
        {
            float step = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 1; i < r.audio.getNumSamples(); ++i)
                    step = std::max (step, std::abs (r.audio.getSample (ch, i) - r.audio.getSample (ch, i - 1)));
            return step;
        };

        auto report = [&] (const juce::String& name, const fx::FxParams& a, const fx::FxParams& b)
        {
            const float steady = std::max (maxStep (render (sine, a)), maxStep (render (sine, b)));
            const float toggled = maxStep (render (sine, a, [&] (fx::FxParams& p, double t) { if (((int) (t / 0.25) % 2) == 1) p = b; }));
            std::cout << "  " << name.paddedRight (' ', 28) << " steady max step " << juce::String (steady, 4)
                      << ", while switching " << juce::String (toggled, 4) << "\n";
            check (toggled <= steady * 1.15f + 0.002f, name + " clicks when switched");
        };

        // every model switched on and off
        for (const auto& m : fx::models())
        {
            if (m.engine == fx::Engine::none)
                continue;
            auto off = allOff();
            put (off, 0, m.key, {}, false);
            auto on = off;
            on.slots[0].on = true;
            report (juce::String (m.key) + " on/off", off, on);
        }

        auto off = allOff();
        auto cabOn = off;  cabOn.amp.on = true;
        auto eqOn = off;   eqOn.eqOn = true; eqOn.eq = smileEq();
        auto eqOff = eqOn; eqOff.eqOn = false;
        report ("cab on/off", off, cabOn);
        report ("eq on/off", eqOff, eqOn);

        auto hum50 = off, hum60 = off, denoised = off;
        hum50.humMode = fx::HumFilter::hz50;
        hum60.humMode = fx::HumFilter::hz60;
        denoised.denoise = 100.0f;
        report ("hum filter on/off", off, hum60);
        report ("hum filter 50 <-> 60 Hz", hum50, hum60);
        report ("denoise on/off", off, denoised);

        // picking another model in a slot that is on
        const std::pair<const char*, const char*> swaps[] = {
            { "screamer", "fuzz_pi" }, { "chorus", "flanger" }, { "phaser", "tremolo" }, { "analog_delay", "room_reverb" },
            { "noise_gate", "heavy_dist" }, { "empty", "chorus" }, { "sub_oct_fuzz", "jet_fuzz" }, { "octave_fuzz", "room_reverb" } };
        for (const auto& [a, b] : swaps)
        {
            auto pa = allOff(), pb = allOff();
            put (pa, 0, a);
            put (pb, 0, b);
            report (juce::String (a) + " <-> " + b, pa, pb);
        }

        // the amp block: picking another amp or cabinet while it is on
        {
            const auto& a = fx::amps();
            const auto& c = fx::cabs();
            auto first = allOff(), otherAmp = allOff(), otherCab = allOff(), none = allOff();
            first.amp    = fx::makeAmp (a.front().key, c.front().key, true);
            otherAmp.amp = fx::makeAmp (a.back().key, c.front().key, true);
            otherCab.amp = fx::makeAmp (a.front().key, c.back().key, true);
            none.amp     = fx::makeAmp (a.front().key, c.front().key, false);
            report ("amp block on/off", none, first);
            report ("amp model swapped", first, otherAmp);
            report ("cab model swapped", first, otherCab);
        }

        // moving the amp/cab block in front of / behind a drive
        auto before = allOff();
        before.amp.on = true;
        put (before, 0, "screamer");
        auto after = before;
        before.ampPosition = 0;
        after.ampPosition = 1;
        report ("amp block moved", before, after);
    }

    // Each distortion slot adds its oversampling delay
    {
        fx::Distortion d;
        d.prepare (testRate, blockSize);
        std::cout << "\n== Latency ==\n  " << juce::String (d.getLatencySamples(), 2) << " samples ("
                  << juce::String (1000.0 * d.getLatencySamples() / testRate, 2) << " ms) per distortion slot, from oversampling\n";
    }

    testEqualizer();
    testNoiseReduction (riff);
    testTrails (riff);
    testTestSignals (outDir);
    testPlayer (outDir);
    testTapTempo();
    testPreDelay();
    testAnalyzer();
    testTuner();

    std::cout << "\n" << (failures == 0 ? "ALL CHECKS PASSED" : juce::String (failures) + " CHECK(S) FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
