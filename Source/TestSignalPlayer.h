#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "DSP/TestSignal.h"

/** Lets you play the board without a guitar: replaces the live input with a built-in demo riff or a
    looped audio file, and can pluck open strings on top of any source (handy for the tuner).
    process() runs on the audio thread; everything else is called from the message thread. */
class TestSignalPlayer
{
public:
    enum Source { live = 0, demo, file };

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        demoRiff = fx::renderDemoRiff (fs);
        demoPos = 0;

        for (auto& v : voices)
        {
            v.string.prepare (fs);
            v.pendingDelay = -1;
        }
    }

    /** Picking the demo riff or a file also starts it playing. */
    void setSource (Source s) noexcept
    {
        source = s;
        if (s != live)
            setPlaying (true);
    }

    Source getSource() const noexcept  { return source.load(); }

    /** STOP / PLAY for the demo riff or file. Stopping rewinds, so PLAY starts from the top.
        Plucked strings still sound while stopped. */
    void setPlaying (bool shouldPlay) noexcept
    {
        if (! shouldPlay)
            rewindRequested = true;
        playing = shouldPlay;
    }

    bool isPlaying() const noexcept { return playing.load(); }

    void pluck (int stringIndex) noexcept { pluckRequests.fetch_or (1u << (unsigned) stringIndex); }

    /** Down-strums one of fx::basicChords (strings 12 ms apart, low to high; unplayed strings are muted). */
    void strum (int chordIndex) noexcept { chordRequest = chordIndex + 1; }

    //==============================================================================
    /** How CHORD buttons play: a looping arpeggio (eighth notes), a looping down-strum on every beat, or a single strum. */
    enum ChordPattern { arpeggio = 0, strumLoop, singleStrum };

    void setChordPattern (int p) noexcept   { chordPattern = std::clamp (p, 0, 2); }
    int getChordPattern() const noexcept    { return chordPattern.load(); }
    void setChordTempo (float bpm) noexcept { chordTempo = std::clamp (bpm, minTempo, maxTempo); }
    float getChordTempo() const noexcept    { return chordTempo.load(); }

    /** Plays a chord with the current pattern. While a loop is running, the new chord takes over
        on the next eighth note (strum loop: the next beat) and starts again from its bass note. */
    void playChord (int chordIndex) noexcept
    {
        if (chordPattern.load() == singleStrum)
            strum (chordIndex);
        else
            loopRequest = chordIndex;
    }

    /** Stops the chord loop and mutes the strings. */
    void stopChord() noexcept { loopRequest = stopRequest; }

    /** The looping chord (for highlighting its button), or -1. */
    int getLoopingChord() const noexcept { return loopingChordShown.load(); }

    static constexpr float minTempo = 40.0f, maxTempo = 240.0f;

    /** 0..1 position in the demo riff or file, for the progress bar. */
    float getProgress() const noexcept { return progress.load(); }

    /** Message thread only (never touches the audio thread's lock, so it can't cause dropouts). */
    juce::File getFile() const { return currentFile; }

    /** Decodes the whole file (mono, up to 10 minutes). Returns an error message, or empty on success. */
    juce::String loadFile (const juce::File& f)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));

        if (reader == nullptr)
            return "Can't open " + f.getFileName() + " (supported: WAV, AIFF, FLAC, OGG, MP3)";

        const auto length = (int) std::min (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 600.0));
        if (length < 16)
            return f.getFileName() + " is empty";

        juce::AudioBuffer<float> audio ((int) reader->numChannels, length);
        reader->read (&audio, 0, length, 0, true, true);

        auto newFile = std::make_unique<LoadedFile>();
        newFile->sampleRate = reader->sampleRate;
        newFile->samples.assign ((size_t) length, 0.0f);

        const float channelScale = 1.0f / (float) audio.getNumChannels(); // mix to mono
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            for (int i = 0; i < length; ++i)
                newFile->samples[(size_t) i] += audio.getSample (ch, i) * channelScale;

        {
            const juce::SpinLock::ScopedLockType lock (fileLock);
            std::swap (loaded, newFile);
            filePos = 0.0;
        }
        // the previous file (now in newFile) is freed here, on the message thread

        currentFile = f;
        setSource (file);
        return {};
    }

    /** Audio thread: overwrite `io` with the selected test source (unless live), then add plucked strings. */
    void process (float* io, int numSamples) noexcept
    {
        if (rewindRequested.exchange (false))
        {
            demoPos = 0;
            progress = 0.0f;
            const juce::SpinLock::ScopedTryLockType lock (fileLock);
            if (lock.isLocked())
                filePos = 0.0;
            else
                rewindRequested = true; // the file is being swapped: try again next block
        }

        const auto src = source.load();
        if (src != live && ! playing.load())
        {
            std::fill_n (io, numSamples, 0.0f); // stopped: silence, but plucks below still sound
        }
        else
        {
            switch (src)
            {
                case demo: playDemo (io, numSamples); break;
                case file: playFile (io, numSamples); break;
                case live: break;
            }
        }

        if (const int chord = chordRequest.exchange (0); chord > 0 && chord <= fx::numBasicChords)
            startStrum (fx::basicChords[chord - 1]);

        runChordLoop (numSamples);

        if (const auto requests = pluckRequests.exchange (0))
            for (int s = 0; s < 6; ++s)
                if (requests & (1u << (unsigned) s))
                {
                    voices[s].pendingDelay = -1;
                    voices[s].string.pluck (fx::openStringHz[s], 0.35f, 3.5f, 0.75f, rng);
                }

        for (auto& v : voices)
        {
            if (! v.string.isActive() && v.pendingDelay < 0)
                continue;

            for (int i = 0; i < numSamples; ++i)
            {
                if (v.pendingDelay >= 0 && v.pendingDelay-- == 0) // sample-accurate strum timing
                    v.string.pluck (v.pendingHz, v.pendingVelocity, 3.0f, 0.75f, rng);
                io[i] += v.string.next();
            }
        }
    }

private:
    struct Voice
    {
        fx::PluckedString string;
        int pendingDelay = -1; // samples until this string gets plucked by a strum, -1 = none
        float pendingHz = 0.0f, pendingVelocity = 0.0f;
    };

    //==============================================================================
    static constexpr int noRequest = -2, stopRequest = -1;

    void runChordLoop (int numSamples) noexcept
    {
        const int request = loopRequest.exchange (noRequest);
        if (request == stopRequest)
        {
            loopChord = nextLoopChord = -1;
            for (auto& v : voices)
            {
                v.pendingDelay = -1;
                v.string.damp();
            }
        }
        else if (request >= 0 && request < fx::numBasicChords)
        {
            if (loopChord < 0)
            {
                loopChord = request;
                loopStep = 0;
                samplesToStep = 0; // start right away
            }
            else
            {
                nextLoopChord = request; // change on the next eighth note
            }
        }

        const int pattern = chordPattern.load();
        if (pattern == singleStrum)
            loopChord = nextLoopChord = -1;

        if (loopChord >= 0)
        {
            const int stepLength = std::max (1, (int) std::lround (fs * 30.0 / chordTempo.load())); // eighth notes
            while (samplesToStep < numSamples)
            {
                // a new chord takes over on the next eighth (the strum loop: on the next beat, so it stays on the beat)
                if (nextLoopChord >= 0 && (pattern != strumLoop || loopStep % 2 == 0))
                {
                    loopChord = nextLoopChord;
                    nextLoopChord = -1;
                    loopStep = 0;
                }
                playStep (fx::basicChords[loopChord], pattern, loopStep, samplesToStep);
                loopStep = (loopStep + 1) % 8;
                samplesToStep += stepLength;
            }
            samplesToStep -= numSamples;
        }

        loopingChordShown = nextLoopChord >= 0 ? nextLoopChord : loopChord;
    }

    /** One eighth note of the pattern, starting `offset` samples into this block. */
    void playStep (const fx::Chord& chord, int pattern, int step, int offset) noexcept
    {
        int played[6], count = 0;
        for (int s = 0; s < 6; ++s)
            if (chord.frets[s] >= 0)
                played[count++] = s;

        const auto pluckAt = [&] (int s, int delay, float velocity)
        {
            auto& v = voices[s];
            v.pendingDelay = delay;
            v.pendingHz = fx::fretHz (s, chord.frets[s]);
            v.pendingVelocity = velocity;
        };
        const auto muteUnplayed = [&]
        {
            for (int s = 0; s < 6; ++s)
                if (chord.frets[s] < 0)
                    voices[s].string.damp();
        };

        if (pattern == arpeggio)
        {
            // bass, G, B, e, alternate bass, G, B, e
            static constexpr int upper[8] = { -1, 3, 4, 5, -1, 3, 4, 5 };
            const int bass = played[0];
            const int altBass = (count > 1 && played[1] <= 2) ? played[1] : bass;
            const int s = step == 0 ? bass : (step == 4 ? altBass : upper[step]);

            if (step == 0)
                muteUnplayed();
            pluckAt (s, offset, step == 0 ? 0.3f : (step == 4 ? 0.26f : 0.2f));
        }
        else if (step % 2 == 0)
        {
            // one down-strum per beat (quarter notes), beat 1 a little stronger
            muteUnplayed();
            const int spacing = (int) (0.009 * fs);
            for (int k = 0; k < count; ++k)
                pluckAt (played[k], offset + k * spacing, (step == 0 ? 0.19f : 0.16f) * (1.0f - 0.04f * (float) k));
        }
    }

    void startStrum (const fx::Chord& chord) noexcept
    {
        const int spacing = (int) (0.012 * fs);
        int order = 0;
        for (int s = 0; s < 6; ++s)
        {
            auto& v = voices[s];
            if (chord.frets[s] < 0)
            {
                v.pendingDelay = -1;
                v.string.damp(); // e.g. the low E left over from a G chord under a C chord
                continue;
            }
            v.pendingDelay = spacing * order;
            v.pendingHz = fx::fretHz (s, chord.frets[s]);
            v.pendingVelocity = 0.19f * (1.0f - 0.04f * (float) order); // six strings together peak near -8 dBFS, like the demo riff
            ++order;
        }
    }

    struct LoadedFile
    {
        double sampleRate = 44100.0;
        std::vector<float> samples;
    };

    void playDemo (float* io, int n) noexcept
    {
        const int size = (int) demoRiff.size();
        if (size == 0)
        {
            std::fill_n (io, n, 0.0f);
            return;
        }

        for (int i = 0; i < n; ++i)
        {
            io[i] = demoRiff[(size_t) demoPos];
            if (++demoPos >= size)
                demoPos = 0;
        }
        progress = (float) demoPos / (float) size;
    }

    void playFile (float* io, int n) noexcept
    {
        const juce::SpinLock::ScopedTryLockType lock (fileLock);

        if (! lock.isLocked() || loaded == nullptr)
        {
            std::fill_n (io, n, 0.0f); // file being swapped (or none loaded): a block of silence
            return;
        }

        const auto& s = loaded->samples;
        const int size = (int) s.size();
        const double step = loaded->sampleRate / fs; // plays at the right speed whatever the device rate
        auto at = [&] (int i) { return s[(size_t) ((i % size + size) % size)]; };

        for (int i = 0; i < n; ++i)
        {
            const int i0 = (int) filePos;
            const float t = (float) (filePos - i0);
            const float xm1 = at (i0 - 1), x0 = at (i0), x1 = at (i0 + 1), x2 = at (i0 + 2);

            // 4-point Hermite interpolation
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            io[i] = ((c3 * t + c2) * t + c1) * t + x0;

            filePos += step;
            if (filePos >= size)
                filePos -= size;
        }
        progress = (float) (filePos / size);
    }

    double fs = 48000.0;
    std::atomic<Source> source { live };
    std::atomic<float> progress { 0.0f };
    std::atomic<unsigned> pluckRequests { 0 };
    std::atomic<int> chordRequest { 0 }, loopRequest { noRequest }, chordPattern { arpeggio }, loopingChordShown { -1 };
    std::atomic<float> chordTempo { 90.0f };
    int loopChord = -1, nextLoopChord = -1, loopStep = 0, samplesToStep = 0; // audio thread only
    std::atomic<bool> playing { true }, rewindRequested { false };

    std::vector<float> demoRiff;
    int demoPos = 0;

    juce::SpinLock fileLock;
    std::unique_ptr<LoadedFile> loaded;
    juce::File currentFile;
    double filePos = 0.0;

    Voice voices[6];
    juce::Random rng;
};
