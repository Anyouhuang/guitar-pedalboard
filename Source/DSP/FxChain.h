#pragma once

#include <juce_dsp/juce_dsp.h>

#include "AudioTap.h"
#include "Delay.h"
#include "Distortion.h"
#include "Equalizer.h"
#include "Models.h"
#include "Modulation.h"
#include "NoiseGate.h"
#include "NoiseReduction.h"
#include "ReverbFx.h"
#include "fx/Volume.h"

namespace fx
{
/** Everything on the board, in plain units. */
struct FxParams
{
    float inputGainDb = 0.0f, outputGainDb = 0.0f;
    bool  mute = false;

    std::array<SlotParams, numSlots> slots {};

    AmpParams amp;        // amp model + cab + mic
    int  ampPosition = 2; // the amp block sits after this many slots (0..numSlots)

    bool eqOn = true;     // global EQ, at the output
    EqSettings eq;

    int   humMode = HumFilter::off; // mains-hum filter on the input: off / 50 Hz / 60 Hz
    float denoise = 0.0f;           // hiss reduction on the output, 0..100 % (0 = off)
};

/** The board as it first opens: Noise Gate > Screamer > cab > Chorus (off) > Analog Delay (off) > Room Reverb. */
FxParams defaultBoard();

/** Crossfades between the dry and processed signal so switching never clicks. */
struct Fade
{
    juce::SmoothedValue<float> amount;

    void prepare (double fs)         { amount.reset (fs, 0.03); amount.setCurrentAndTargetValue (0.0f); }
    void set (bool on)               { amount.setTargetValue (on ? 1.0f : 0.0f); }
    bool isOff() const noexcept      { return ! amount.isSmoothing() && amount.getCurrentValue() <= 0.0f; }
    bool isFullyOn() const noexcept  { return ! amount.isSmoothing() && amount.getCurrentValue() >= 1.0f; }
};

/** Scratch buffers shared by the slots of one chain. */
struct SlotScratch
{
    float* dryLeft;
    float* dryRight;
    float* mid;
    float* wetLeft;
    float* wetRight;
    float* gain;
};

/** What a slot needs from one of the fx/ engines (docs/engine-contract.md), whichever class it is. */
struct EngineRef
{
    void* object = nullptr;
    void (*setModel) (void*, int) = nullptr;
    void (*setParameters) (void*, const float*) = nullptr;
    void (*reset) (void*) = nullptr;
    void (*process) (void*, float*, float*, int) = nullptr;
    float (*getMix) (void*) = nullptr;                      // delay / reverb engines: they return the wet signal only
    float (*getTailSeconds) (void*) = nullptr;
    void (*processDry) (void*, float*, float*, int) = nullptr; // delay models that colour the dry signal too

    explicit operator bool() const noexcept { return object != nullptr; }

    template <typename Fx>
    static EngineRef to (Fx& fx)
    {
        EngineRef r;
        r.object        = &fx;
        r.setModel      = [] (void* o, int v)                        { static_cast<Fx*> (o)->setModel (v); };
        r.setParameters = [] (void* o, const float* k)               { static_cast<Fx*> (o)->setParameters (k); };
        r.reset         = [] (void* o)                               { static_cast<Fx*> (o)->reset(); };
        r.process       = [] (void* o, float* l, float* rt, int n)   { static_cast<Fx*> (o)->process (l, rt, n); };

        if constexpr (requires (Fx& f) { f.getMix(); f.getTailSeconds(); })
        {
            r.getMix         = [] (void* o) { return static_cast<Fx*> (o)->getMix(); };
            r.getTailSeconds = [] (void* o) { return static_cast<Fx*> (o)->getTailSeconds(); };
        }

        if constexpr (requires (Fx& f, float* p) { f.processDry (p, p, 1); })
            r.processDry = [] (void* o, float* l, float* rt, int n) { static_cast<Fx*> (o)->processDry (l, rt, n); };

        return r;
    }
};

//==============================================================================
/** One FX slot. Owns an instance of every engine, so picking a model never allocates.
    Picking another model fades the old one out, then fades the new one in.

    Three kinds of engine:
    - the first engines (gate, distortion, the original modulation / delay / reverb), driven directly;
    - inserts (dynamics, modulation, filter, pitch, EQ, wah, volume): processed in place, crossfaded with
      the dry signal when switched; modulation, filter and pitch get 50 ms of warm-up first, so their delay
      lines and detectors are full of real signal before they are heard;
    - sends (delay, reverb): return the wet signal only; the slot mixes it with the dry signal and, when
      switched off, only mutes their input, so the echoes and tails ring out. */
class Slot
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void setParameters (const SlotParams&);
    void process (float* left, float* right, int numSamples, const SlotScratch&);

    int getActiveModel() const noexcept { return active; }

private:
    enum class Kind { none, first, insert, send };
    static Kind kindOf (Engine) noexcept;

    const EngineRef& ref() const noexcept { return refs[(size_t) modelInfo (active).engine]; }
    void configure();
    void start();
    void resetEngine();
    void processFirst (float* left, float* right, int numSamples, const SlotScratch&);
    void processInsert (float* left, float* right, int numSamples, const SlotScratch&);
    void processSend (float* left, float* right, int numSamples, const SlotScratch&);

    // the first engines
    NoiseGate  gate;
    Distortion distortion;
    Modulation modulation;
    Delay      delay;
    ReverbFx   reverb;

    // the fx/ engines
    DynamicsFx dynamicsFx;
    ModFx modFx;
    FilterFx filterFx;
    PitchFx pitchFx;
    EqFx eqFx;
    DelayFx delayFx;
    VerbFx verbFx;
    WahFx wahFx;
    VolumeFx volumeFx;
    std::array<EngineRef, (size_t) Engine::numEngines> refs {};

    double fs = 48000.0;
    Fade fade;
    int active = 0, requested = 0;
    bool on = false, needsReset = false;
    std::array<float, maxKnobs> knobs {};        // the active model's settings
    std::array<float, maxKnobs> pendingKnobs {}; // the requested model's, while the active one fades out

    int warmupLeft = 0;                 // inserts: samples of warm-up still to run
    Smoothed send { 0.0f }, mix { 0.0f }; // sends: input level (0 when switched off) and the Mix knob
    bool running = false;               // sends: still processing (on, or the tail is ringing out)
    int silentSamples = 0;
};

//==============================================================================
/** The amp block: amp model -> speaker cabinet + microphone (both mono; either can be "none").
    Switching it, picking another amp or cab, or moving it in the chain crossfades through the dry signal. */
class AmpBlock
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /** `hold` keeps the block faded out (the chain does that while it moves the block). */
    void setParameters (const AmpParams&, bool hold);
    void process (float* left, float* right, int numSamples, const SlotScratch&);

    bool isOff() const noexcept { return fade.isOff(); }

private:
    void configure();
    static bool sameModels (const AmpParams& a, const AmpParams& b) noexcept { return a.amp == b.amp && a.cab == b.cab; }

    AmpFx amp;
    CabFx cab;
    Fade fade;
    AmpParams active, requested; // `active` keeps its own knobs while it fades out for another amp / cab
    bool needsReset = true, held = false;
};

//==============================================================================
/** input -> hum filter -> 8 slots, with the amp/cab block between them -> global EQ -> hiss reduction -> output.
    Mono in, stereo out: mono models sum the two sides, stereo models (modulation, delay, reverb) keep them. */
class FxChain
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /** Call once per audio block, before process(). */
    void setParameters (const FxParams& params);

    /** Mono in, stereo out. `input` may alias `outLeft`. Pass nullptr for `outRight` to get a mono mix. */
    void process (const float* input, float* outLeft, float* outRight, int numSamples);

    /** Optional: receives the output (after the EQ, before the master level) for the spectrum analyser. */
    void setAnalyzerTap (AudioTap* tap) noexcept { analyzerTap = tap; }

    float getInputPeak() const noexcept  { return inputPeak; }
    float getOutputPeak() const noexcept { return outputPeak; }
    int getActiveModel (int slot) const noexcept { return slots[(size_t) slot].getActiveModel(); }

private:
    void processChunk (const float* input, float* left, float* right, int numSamples);
    void runEq (float* left, float* right, int numSamples);

    int maxBlock = 512;

    std::array<Slot, numSlots> slots;
    AmpBlock ampBlock;
    int ampPosition = 2, requestedAmpPosition = 2;

    HumFilter hum;
    Denoiser denoiser;
    Equalizer eqLeft, eqRight;
    Fade eqFade;
    bool eqNeedsReset = true;

    AudioTap* analyzerTap = nullptr;

    juce::SmoothedValue<float> inputGain { 1.0f }, outputGain { 1.0f };
    std::vector<float> mono, dryLeft, dryRight, mid, wetLeft, wetRight, gain, spareRight;

    float inputPeak = 0.0f, outputPeak = 0.0f;
};

} // namespace fx
