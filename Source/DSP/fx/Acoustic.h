#pragma once

#include "../DspUtils.h"
#include "../ModelTypes.h"

namespace fx
{
namespace acoustic_detail
{
    /** A band-pass with 0 dB at its centre (RBJ): one resonance of the guitar's body, or one band of its tone.
        Added to the dry signal it lifts that band (by 1 + gain at the centre), subtracted it scoops it; and
        because a band-pass never turns the phase past +-90 degrees, the bands can be mixed in any amounts
        without cancelling each other. That is what lets the knobs move gains only, never coefficients. */
    struct Band
    {
        void set (double fs, double hz, double q)
        {
            const double w0 = 2.0 * pi * std::clamp (hz, 1.0, 0.45 * fs) / fs;
            const double alpha = std::sin (w0) / (2.0 * q), a0 = 1.0 + alpha;
            b0 = float (alpha / a0);
            a1 = float (-2.0 * std::cos (w0) / a0);
            a2 = float ((1.0 - alpha) / a0);
        }

        void reset() noexcept { z1 = z2 = 0.0f; }

        float process (float x) noexcept
        {
            const float y = b0 * x + z1;
            z1 = z2 - a1 * y;       // (b1 = 0)
            z2 = -b0 * x - a2 * y;  // (b2 = -b0)
            return y;
        }

        float b0 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
    };

    /** Per-sample step of a one-pole follower with the time constant `seconds`. */
    inline float stepFor (double seconds, double fs) noexcept
    {
        return (float) (1.0 - std::exp (-1.0 / (seconds * fs)));
    }

    // The bands, fixed: three body resonances of a dreadnought and of a jumbo (the air in the box, the top plate,
    // the back), the midrange a magnetic pickup has too much of, an under-saddle piezo's "quack", presence, air.
    enum BandId { dreadAir = 0, dreadTop, dreadBack, jumboAir, jumboTop, jumboBack, mids, quack, presence, air, numBands };
    struct BandSpec { double hz, q; };
    constexpr BandSpec bandSpecs[numBands] = { { 105.0, 3.5 }, { 205.0, 4.5 }, { 400.0, 5.0 },
                                               { 88.0, 3.5 },  { 175.0, 4.5 }, { 330.0, 5.0 },
                                               { 1200.0, 0.5 }, { 1600.0, 1.6 }, { 3200.0, 1.6 }, { 9000.0, 1.2 } };

    constexpr double lowCutHz = 75.0;   // below the low E's fundamental: handling noise, not guitar
    constexpr double topHz = 7000.0;    // where the string-brilliance shelf turns up

    enum Type { standard = 0, jumbo, enhanced, piezo, numTypes };

    /** One voicing. Gains are what is added to the dry signal (0.8 = +5 dB at the band's centre, -0.6 = -8 dB).
        `body` is scaled by the Body knob, `top` and `air` by the Top knob (both: x 1 at 50 %, x 2 at 100 %). */
    struct Voice
    {
        float body[3];      // air, top plate, back
        bool  bigBody;      // the jumbo's lower resonances instead of the dreadnought's
        float mids, quack, presence;
        float top, air;     // the shelf from 7 kHz, and the band around 9 kHz
        float snap;         // how much brighter the pick attack is than the note that follows
        float trimDb;       // brings the voicing to about the loudness of the input
    };

    constexpr Voice voices[numTypes] =
    {
        //  air    top    back   big    mids    quack  pres   top   air   snap  trim
        { { 0.80f, 0.70f, 0.25f }, false, -0.62f, 0.00f, 0.00f, 1.8f, 0.6f, 1.0f, -3.0f }, // Standard: a dreadnought, balanced
        { { 1.10f, 0.90f, 0.30f }, true,  -0.66f, 0.00f, 0.00f, 1.0f, 0.2f, 0.8f, -0.5f }, // Jumbo: bigger box, deeper and rounder
        { { 0.70f, 0.60f, 0.20f }, false, -0.62f, 0.00f, 0.60f, 2.1f, 0.8f, 2.0f, -6.0f }, // Enhanced: more attack and presence, cuts through a band
        { { 0.30f, 0.35f, 0.15f }, false, -0.35f, 0.60f, 0.00f, 2.3f, 1.0f, 1.5f, -6.0f }, // Piezo: an under-saddle pickup, thin body, bright and quacky
    };

    constexpr double fastAttack = 0.0003, fastRelease = 0.012, slowAttack = 0.025, slowRelease = 0.100, snapSmooth = 0.0015;
    constexpr float  quietest = 2.0e-4f; // (-74 dB) below this the followers no longer count as a note starting

    // Added to the signal so that filters and followers settle at 1e-20 in silence instead of in the denormals
    constexpr float tiny = 1.0e-20f;
}

inline const char* const acousticTypeNames[] = { "Standard", "Jumbo", "Enhanced", "Piezo" };

/** Acoustic guitar simulator: makes a solid-body electric sound like a steel-string acoustic, in the manner of
    the BOSS AC-2 / AC-3 pedals. This model is our own addition: the HD500X has none. Mono.

    Knobs:  Type (Standard / Jumbo / Enhanced / Piezo), Body, Top, Level

    What separates the two instruments, and what is done about each:
      - a magnetic pickup is loud in the midrange (around 1 kHz) where an acoustic's top is not: scooped;
      - the pickup and cable roll off above 3-5 kHz, where an acoustic's strings shimmer: a shelf from 7 kHz
        and a band around 9 kHz, both on the Top knob;
      - the hollow body rings at its air resonance (about 100 Hz), its top plate (about 200 Hz) and its back
        (about 400 Hz): three resonant bands, on the Body knob;
      - a pick on bronze strings starts every note with a bright click that dies within a few hundredths of a
        second: while a fast level follower is ahead of a slow one, the top end is lifted further.

    Everything is the dry signal plus fixed band-passes mixed in with smoothed gains (see acoustic_detail::Band),
    so turning a knob or switching the Type never clicks. Best with the amp block switched off: a guitar speaker
    takes the top end away again. */
class AcousticFx
{
public:
    enum Variant { acousticSim = 0, numVariants };

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        using namespace acoustic_detail;
        fs = sampleRate;

        lowCut.setHighPass (fs, lowCutHz, 0.7071);
        for (int b = 0; b < numBands; ++b)
        {
            band[b].set (fs, bandSpecs[b].hz, bandSpecs[b].q);
            gain[b].reset (fs, 0.04);
        }
        topFilter.setCutoff (fs, topHz);

        for (auto* s : { &topGain, &snapGain, &level })
            s->reset (fs, 0.04);

        fastUp = stepFor (fastAttack, fs);
        fastDown = stepFor (fastRelease, fs);
        slowUp = stepFor (slowAttack, fs);
        slowDown = stepFor (slowRelease, fs);
        snapStep = stepFor (snapSmooth, fs);

        reset();
    }

    void reset()
    {
        lowCut.reset();
        topFilter.reset();
        for (int b = 0; b < acoustic_detail::numBands; ++b)
        {
            band[b].reset();
            gain[b].setCurrentAndTarget (gain[b].getTarget());
        }
        for (auto* s : { &topGain, &snapGain, &level })
            s->setCurrentAndTarget (s->getTarget());

        fast = slow = snapNow = 0.0f;
    }

    void setModel (int) noexcept {} // one model

    void setParameters (const float* k)
    {
        using namespace acoustic_detail;
        const auto& voice = voices[std::clamp ((int) std::lround (k[0]), 0, (int) numTypes - 1)];
        const float body = 2.0f * std::clamp (k[1] / 100.0f, 0.0f, 1.0f);
        const float top  = 2.0f * std::clamp (k[2] / 100.0f, 0.0f, 1.0f);

        for (int i = 0; i < 3; ++i)
        {
            gain[dreadAir + i].setTarget (voice.bigBody ? 0.0f : voice.body[i] * body);
            gain[jumboAir + i].setTarget (voice.bigBody ? voice.body[i] * body : 0.0f);
        }
        gain[mids].setTarget (voice.mids);
        gain[quack].setTarget (voice.quack);
        gain[presence].setTarget (voice.presence);
        gain[air].setTarget (voice.air * top);

        topGain.setTarget (voice.top * top);
        snapGain.setTarget (voice.snap);
        level.setTarget (dbToGain (k[3] + voice.trimDb));
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        using namespace acoustic_detail;

        for (int i = 0; i < numSamples; ++i)
        {
            const float x = lowCut.process (0.5f * (left[i] + right[i]) + tiny) + tiny;

            // the pick attack: for the few hundredths of a second in which the fast follower is ahead of the slow one
            const float a = std::abs (x) + tiny;
            fast += (a > fast ? fastUp : fastDown) * (a - fast);
            slow += (a > slow ? slowUp : slowDown) * (a - slow);
            const float ahead = std::clamp (2.0f * (fast - slow) / (fast + slow + quietest) - 0.25f, 0.0f, 1.0f);
            snapNow += snapStep * ((ahead + tiny) - snapNow);

            float y = x;
            for (int b = 0; b < numBands; ++b)
                y += gain[b].next() * band[b].process (x);

            y += topGain.next() * (1.0f + snapGain.next() * snapNow) * topFilter.highPass (x);

            left[i] = right[i] = y * level.next();
        }
    }

private:
    double fs = 48000.0;
    Biquad lowCut;
    acoustic_detail::Band band[acoustic_detail::numBands];
    Smoothed gain[acoustic_detail::numBands];
    OnePole topFilter;
    Smoothed topGain { 0.0f }, snapGain { 0.0f }, level { 1.0f };

    float fastUp = 0.0f, fastDown = 0.0f, slowUp = 0.0f, slowDown = 0.0f, snapStep = 0.0f;
    float fast = 0.0f, slow = 0.0f, snapNow = 0.0f;
};

/** In the order of AcousticFx::Variant. */
inline std::vector<ModelInfo> acousticModels()
{
    return {
        { "acoustic_sim", "Acoustic Sim", Category::eq, Engine::acousticFx, AcousticFx::acousticSim,
          "Acoustic simulator pedal (BOSS AC-3 style); our own addition, not an HD500X model",
          { choice ("Type", acousticTypeNames, 4, 0), percent ("Body", 50.0f), percent ("Top", 50.0f), levelDb() } },
    };
}

} // namespace fx
