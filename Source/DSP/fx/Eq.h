#pragma once

#include "../DspUtils.h"
#include "../ModelTypes.h"

namespace fx
{
inline const char* const eqPhaseNames[] = { "0", "180" };

namespace eq_detail
{
    constexpr int tick = 16;        // samples between filter updates (the filters glide in between)
    constexpr int maxSections = 5;
    constexpr int numParams = 5;
    constexpr float tiny = 1.0e-20f;

    // Graphic EQ: the HD500X's five bands
    // (the bands are not evenly spaced: each is as wide as its distance to its neighbours, so that they add up
    // to a plateau without holes)
    constexpr double graphicHz[] = { 80.0, 220.0, 480.0, 1100.0, 2200.0 };
    constexpr double graphicQ[]  = { 0.9, 1.0, 1.3, 1.5, 1.6 };

    // Parametric EQ: fixed shelves, Q knob 0 ... 100 % = 0.4 ... 10
    constexpr double parametricLowHz = 150.0, parametricHighHz = 3500.0, parametricMinQ = 0.4, parametricQRange = 25.0;

    // Studio EQ (API 550B's two mid bands). The output stage is clean up to full scale and rounds off what goes
    // beyond it (it never gets past +6 dBFS).
    constexpr double studioQ = 1.2;
    constexpr float  studioKnee = 1.0f, studioCeiling = 2.0f;

    // 4 Band Shift EQ: band centres at Shift 50 %, and how many octaves each moves per half turn of Shift
    constexpr double shiftLowHz = 120.0, shiftLowMidHz = 400.0, shiftHiMidHz = 1600.0, shiftHiHz = 5000.0, shiftMidQ = 0.9;
    constexpr double shiftLowOctaves = -0.7, shiftLowMidOctaves = 0.7, shiftHiMidOctaves = 0.8, shiftHiOctaves = 0.8;

    // Mid Focus EQ: Q knobs 0 ... 100 % = 0.35 ... 5.6 (25 % = Butterworth)
    constexpr double focusMinQ = 0.35, focusQRange = 16.0;

    // Vintage Pre: the tube stage
    constexpr float preMaxDriveDb = 30.0f, preHeadroom = 2.0f, preBias = 0.2f, preMakeup = 0.7f;

    //==============================================================================
    /** One second-order filter as a state-variable filter: out = m0 * in + m1 * band-pass + m2 * low-pass.
        (Not a direct-form biquad: those lose their precision in 32-bit floats when the frequency is low against
        the sample rate - a 20 Hz high-pass at 96 kHz, say - and this form does not.)
        The responses are the RBJ cookbook's; identity is m0 = 1, m1 = m2 = 0, which passes the input untouched. */
    struct Coefs { float g = 0.1f, k = 1.0f, m0 = 1.0f, m1 = 0.0f, m2 = 0.0f; };

    inline double warp (double fs, double hz) noexcept
    {
        return std::tan (pi * std::clamp (hz, 1.0, 0.49 * fs) / fs);
    }

    inline Coefs makeCoefs (double g, double k, double m0, double m1, double m2) noexcept
    {
        return { (float) g, (float) k, (float) m0, (float) m1, (float) m2 };
    }

    /** Peaking band, boost and cut mirror images of each other; the band gets narrower as the gain grows
        (Q is the width at half the gain in dB). */
    inline Coefs peak (double fs, double hz, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0), k = 1.0 / (q * A);
        return makeCoefs (warp (fs, hz), k, 1.0, k * (A * A - 1.0), 0.0);
    }

    /** Constant-Q peaking band: the poles keep the same Q at every boost (the zeros at every cut), so the width
        of the bump hardly changes with its height. Boost and cut are still mirror images. */
    inline Coefs constantQPeak (double fs, double hz, double q, double gainDb) noexcept
    {
        const double G = std::pow (10.0, gainDb / 20.0), k = G >= 1.0 ? 1.0 / q : 1.0 / (q * G);
        return makeCoefs (warp (fs, hz), k, 1.0, k * (G - 1.0), 0.0);
    }

    constexpr double shelfK = 1.41421356; // 1 / Q of the shelves (slope 1)

    /** `hz` is where the shelf has reached half its gain (in dB). */
    inline Coefs lowShelf (double fs, double hz, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        return makeCoefs (warp (fs, hz) / std::sqrt (A), shelfK, 1.0, shelfK * (A - 1.0), A * A - 1.0);
    }

    inline Coefs highShelf (double fs, double hz, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        return makeCoefs (warp (fs, hz) * std::sqrt (A), shelfK, A * A, shelfK * (1.0 - A) * A, 1.0 - A * A);
    }

    inline Coefs highPass (double fs, double hz, double q) noexcept
    {
        return makeCoefs (warp (fs, hz), 1.0 / q, 1.0, -1.0 / q, -1.0);
    }

    inline Coefs lowPass (double fs, double hz, double q) noexcept
    {
        return makeCoefs (warp (fs, hz), 1.0 / q, 0.0, 0.0, 1.0);
    }

    //==============================================================================
    /** One filter for both sides. A new setting is not switched in: it glides there over one tick, sample by
        sample, so a moving knob leaves no steps in the signal (and the filter stays stable while it moves). */
    struct Section
    {
        Coefs c, target, step;
        float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f; // from c.g and c.k
        bool gliding = false;
        float ic1[2] {}, ic2[2] {};

        void set (const Coefs& now) noexcept
        {
            c = target = now;
            a1 = 1.0f / (1.0f + c.g * (c.g + c.k));
            a2 = c.g * a1;
            a3 = c.g * a2;
            gliding = false;
        }

        void clear() noexcept { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0f; }

        void glideTo (const Coefs& next) noexcept
        {
            const float scale = 1.0f / (float) tick;
            target = next;
            step = { (next.g - c.g) * scale, (next.k - c.k) * scale, (next.m0 - c.m0) * scale, (next.m1 - c.m1) * scale, (next.m2 - c.m2) * scale };
            gliding = true;
        }

        /** End of a tick: be exactly where the glide was heading. */
        void land() noexcept
        {
            if (gliding)
                set (target);
        }

        void process (float* x, int n, int ch) noexcept
        {
            float s1 = ic1[ch], s2 = ic2[ch];

            if (gliding)
            {
                Coefs now = c;
                for (int i = 0; i < n; ++i)
                {
                    now.g += step.g;  now.k += step.k;  now.m0 += step.m0;  now.m1 += step.m1;  now.m2 += step.m2;
                    const float b1 = 1.0f / (1.0f + now.g * (now.g + now.k));
                    const float b2 = now.g * b1;
                    const float b3 = now.g * b2;

                    const float in = x[i];
                    const float v3 = (in + tiny) - s2;
                    const float v1 = b1 * s1 + b2 * v3;
                    const float v2 = s2 + b2 * s1 + b3 * v3;
                    s1 = 2.0f * v1 - s1;
                    s2 = 2.0f * v2 - s2;
                    x[i] = now.m0 * in + now.m1 * v1 + now.m2 * v2;
                }
            }
            else
            {
                const float m0 = c.m0, m1 = c.m1, m2 = c.m2;
                for (int i = 0; i < n; ++i)
                {
                    const float in = x[i];
                    const float v3 = (in + tiny) - s2;   // (tiny: keeps the states out of the denormals in silence)
                    const float v1 = a1 * s1 + a2 * v3;
                    const float v2 = s2 + a2 * s1 + a3 * v3;
                    s1 = 2.0f * v1 - s1;
                    s2 = 2.0f * v2 - s2;
                    x[i] = m0 * in + m1 * v1 + m2 * v2;
                }
            }

            ic1[ch] = s1;
            ic2[ch] = s2;
        }

        /** After both sides have been processed: move the glide on by `n` samples. */
        void advance (int n) noexcept
        {
            if (gliding)
                for (int i = 0; i < n; ++i)
                {
                    c.g += step.g;  c.k += step.k;  c.m0 += step.m0;  c.m1 += step.m1;  c.m2 += step.m2;
                }
        }
    };

    /** Linear up to `knee`, then bends over smoothly towards `ceiling`. */
    inline float softClip (float v, float knee, float ceiling) noexcept
    {
        const float a = std::abs (v);
        if (a <= knee)
            return v;

        const float range = ceiling - knee;
        const float s = knee + range * std::tanh ((a - knee) / range);
        return v < 0.0f ? -s : s;
    }
}

/** The HD500X's Preamp+EQ models. Stereo (each side filtered on its own), except the mono Vintage Pre.

    Graphic EQ        80Hz, 220Hz, 480Hz, 1.1kHz, 2.2kHz           (+-12 dB each)
    Parametric EQ     Lows, Highs, Freq, Q, Gain                   (shelves at 150 Hz / 3.5 kHz and one free band)
    Studio EQ         Low Freq, Low Gain, Hi Freq, Hi Gain, Gain   (two constant-Q bands, output gain that soft-clips)
    4 Band Shift EQ   Low, Low Mid, Hi Mid, Hi, Shift              (Shift spreads the bands: low lower, the others higher)
    Mid Focus EQ      Hi Pass Freq, Hi Pass Q, Low Pass Freq, Low Pass Q, Gain
    Vintage Pre       Gain, Output, Phase, Hi Pass Filter, Lo Pass Filter

    With every gain at 0 dB the four EQs pass the signal bit for bit. Knobs glide for 50 ms and the filters
    follow them sample by sample. */
class EqFx
{
public:
    enum Variant { graphicEq = 0, parametricEq, studioEq, fourBandShiftEq, midFocusEq, vintagePre, numVariants };

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        fs = sampleRate;

        for (auto& p : param)
            p.reset (fs, 0.05);
        for (auto* s : { &outGain, &drive, &makeup })
            s->reset (fs, 0.03);

        oversampler.prepare (eq_detail::tick);
        dc.prepare (fs);
        tubeOffset = std::tanh (eq_detail::preBias);
        tubeNorm = 1.0f / (1.0f - tubeOffset * tubeOffset); // slope 1 for small signals
        reset();
    }

    void reset()
    {
        for (int p = 0; p < eq_detail::numParams; ++p)
        {
            param[p].setCurrentAndTarget (param[p].getTarget());
            value[p] = param[p].getTarget();
        }
        for (auto* s : { &outGain, &drive, &makeup })
            s->setCurrentAndTarget (s->getTarget());

        for (auto& s : section)
        {
            s.set ({});
            s.clear();
        }
        computeSections (false);

        oversampler.reset();
        dc.reset();
        tickCount = 0;
        modelChanged = false;
    }

    void setModel (int newVariant) noexcept
    {
        variant = std::clamp (newVariant, 0, (int) numVariants - 1);
        modelChanged = true;
    }

    void setParameters (const float* k)
    {
        using namespace eq_detail;
        float v[numParams] = { k[0], k[1], k[2], k[3], k[4] };
        const auto logHz = [] (float hz) { return std::log (std::max (1.0f, hz)); }; // frequencies glide in octaves

        switch (variant)
        {
            case parametricEq:
                v[2] = logHz (k[2]);
                v[3] = k[3] * 0.01f;
                break;

            case studioEq:
                v[0] = logHz (k[0]);
                v[2] = logHz (k[2]);
                v[4] = 0.0f;
                outGain.setTarget (dbToGain (k[4]));
                break;

            case fourBandShiftEq:
                v[4] = k[4] * 0.01f;
                break;

            case midFocusEq:
                v[0] = logHz (k[0]);
                v[1] = k[1] * 0.01f;
                v[2] = logHz (k[2]);
                v[3] = k[3] * 0.01f;
                v[4] = 0.0f;
                outGain.setTarget (dbToGain (k[4]));
                break;

            case vintagePre:
            {
                // Gain drives the tube harder; most of the extra level is taken off again behind it, so the knob
                // adds distortion more than loudness. Phase flips the output (through zero, not with a jump).
                const float driveDb = preMaxDriveDb * k[0] * 0.01f;
                drive.setTarget (dbToGain (driveDb) / preHeadroom);
                makeup.setTarget (preHeadroom * dbToGain (-preMakeup * driveDb));
                outGain.setTarget (k[2] >= 0.5f ? -dbToGain (k[1]) : dbToGain (k[1]));
                v[0] = v[1] = v[2] = 0.0f;
                v[3] = logHz (k[3]);
                v[4] = logHz (k[4]);
                break;
            }

            default:
                break;
        }

        for (int p = 0; p < numParams; ++p)
            param[p].setTarget (v[p]);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        using namespace eq_detail;

        for (int pos = 0; pos < numSamples;)
        {
            if (tickCount == 0)
                nextTick();

            const int n = std::min (tickCount, numSamples - pos);
            float* const l = left + pos;
            float* const r = right + pos;

            if (variant == vintagePre)
            {
                processPre (l, r, n);
            }
            else
            {
                const int count = numSections();
                for (int s = 0; s < count; ++s)
                {
                    section[s].process (l, n, 0);
                    section[s].process (r, n, 1);
                    section[s].advance (n);
                }

                if (variant == studioEq)
                {
                    // the output stage: clean up to full scale, rounds the peaks off when pushed beyond it
                    for (int i = 0; i < n; ++i)
                    {
                        const float gain = outGain.next();
                        l[i] = softClip (l[i] * gain, studioKnee, studioCeiling);
                        r[i] = softClip (r[i] * gain, studioKnee, studioCeiling);
                    }
                }
                else if (variant == midFocusEq)
                {
                    for (int i = 0; i < n; ++i)
                    {
                        const float gain = outGain.next();
                        l[i] *= gain;
                        r[i] *= gain;
                    }
                }
            }

            pos += n;
            tickCount -= n;
        }
    }

private:
    int numSections() const noexcept
    {
        constexpr int counts[] = { 5, 3, 2, 4, 2, 2 };
        return counts[variant];
    }

    /** Filter coefficients for the current (gliding) knob values. */
    void computeSections (bool glide) noexcept
    {
        using namespace eq_detail;
        Coefs c[maxSections];
        const double v[numParams] = { value[0], value[1], value[2], value[3], value[4] };

        switch (variant)
        {
            // Graphic EQ (inspired by the MXR graphic EQs): five gyrator-style bands, boost and cut symmetrical,
            // wide enough to overlap into a smooth curve, +-12 dB.
            case graphicEq:
                for (int b = 0; b < 5; ++b)
                    c[b] = peak (fs, graphicHz[b], graphicQ[b], v[b]);
                break;

            // Parametric EQ: low shelf, high shelf and one band with free frequency, width and gain.
            case parametricEq:
                c[0] = lowShelf (fs, parametricLowHz, v[0]);
                c[1] = highShelf (fs, parametricHighHz, v[1]);
                c[2] = peak (fs, std::exp (v[2]), parametricMinQ * std::pow (parametricQRange, v[3]), v[4]);
                break;

            // Studio EQ (inspired by the API 550B): its two mid bands as the HD500X offers them (Low 75 Hz - 1 kHz,
            // Hi 800 Hz - 12.5 kHz), constant-Q as Line 6 describes the model, reciprocal boost / cut.
            case studioEq:
                c[0] = constantQPeak (fs, std::exp (v[0]), studioQ, v[1]);
                c[1] = constantQPeak (fs, std::exp (v[2]), studioQ, v[3]);
                break;

            // 4 Band Shift EQ: shelves at both ends, two peaking bands between. Shift slides the low band down and
            // the other three up (above 50 %: spread wide for guitar; below: bunched in the low mids for bass).
            case fourBandShiftEq:
            {
                const double turn = 2.0 * v[4] - 1.0;
                c[0] = lowShelf (fs, shiftLowHz * std::pow (2.0, shiftLowOctaves * turn), v[0]);
                c[1] = peak (fs, shiftLowMidHz * std::pow (2.0, shiftLowMidOctaves * turn), shiftMidQ, v[1]);
                c[2] = peak (fs, shiftHiMidHz * std::pow (2.0, shiftHiMidOctaves * turn), shiftMidQ, v[2]);
                c[3] = highShelf (fs, shiftHiHz * std::pow (2.0, shiftHiOctaves * turn), v[3]);
                break;
            }

            // Mid Focus EQ: a 12 dB/octave high-pass and low-pass, each with its own resonance (Q).
            case midFocusEq:
                c[0] = highPass (fs, std::exp (v[0]), focusMinQ * std::pow (focusQRange, v[1]));
                c[1] = lowPass (fs, std::exp (v[2]), focusMinQ * std::pow (focusQRange, v[3]));
                break;

            // Vintage Pre: the filters in front of and behind the tube
            default:
                c[0] = highPass (fs, std::exp (v[3]), 0.70710678);
                c[1] = lowPass (fs, std::exp (v[4]), 0.70710678);
                break;
        }

        const int count = numSections();
        for (int s = 0; s < count; ++s)
        {
            if (glide)  section[s].glideTo (c[s]);
            else        section[s].set (c[s]);
        }
    }

    void nextTick() noexcept
    {
        using namespace eq_detail;
        tickCount = tick;

        for (auto& s : section)
            s.land();

        bool moved = modelChanged;
        for (int p = 0; p < numParams; ++p)
            if (modelChanged || param[p].isSmoothing())
            {
                value[p] = param[p].skip (tick);
                moved = true;
            }

        if (moved)
        {
            modelChanged = false;
            computeSections (true);
        }
    }

    //==============================================================================
    // Vintage Pre (Requisite Y7 tube mic preamp), mono. High-pass, one tube stage, low-pass. The tube curve is a
    // biased tanh: asymmetric, so it adds mostly second harmonic while clean and folds over into real distortion
    // as Gain pushes it. Run at 4x the sample rate, because at full Gain it clips hard.
    void processPre (float* l, float* r, int n) noexcept
    {
        using namespace eq_detail;
        float mono[tick];

        for (int i = 0; i < n; ++i)
            mono[i] = 0.5f * (l[i] + r[i]);

        section[0].process (mono, n, 0);
        section[0].advance (n);

        for (int i = 0; i < n; ++i)
            mono[i] *= drive.next();

        float* const up = oversampler.up (mono, n);
        for (int i = 0; i < 4 * n; ++i)
            up[i] = (std::tanh (up[i] + preBias) - tubeOffset) * tubeNorm;
        oversampler.down (mono, n);

        for (int i = 0; i < n; ++i)
            mono[i] *= makeup.next();

        section[1].process (mono, n, 0);
        section[1].advance (n);

        for (int i = 0; i < n; ++i)
            l[i] = r[i] = dc.process (mono[i]) * outGain.next();
    }

    //==============================================================================
    double fs = 48000.0;
    int variant = graphicEq;

    Smoothed param[eq_detail::numParams];       // the knobs that set filters (gains in dB, frequencies as log Hz)
    float value[eq_detail::numParams] {};       // ...and where their glides are now
    Smoothed outGain { 1.0f }, drive { 1.0f }, makeup { 1.0f };

    eq_detail::Section section[eq_detail::maxSections];
    int tickCount = 0;
    bool modelChanged = true;

    Oversampler4x oversampler;
    DcBlocker dc;
    float tubeOffset = 0.0f, tubeNorm = 1.0f;
};

/** In the order of EqFx::Variant. */
inline std::vector<ModelInfo> eqModels()
{
    const auto model = [] (const char* key, const char* name, int variant, const char* basedOn, std::vector<KnobSpec> knobs)
    {
        return ModelInfo { key, name, Category::eq, Engine::eqFx, variant, basedOn, std::move (knobs) };
    };
    const auto gain = [] (const char* name, float def = 0.0f) { return decibels (name, -12.0f, 12.0f, def); };

    return {
        model ("graphic_eq", "Graphic EQ", EqFx::graphicEq, "Inspired by the MXR 10-band graphic EQ",
               { gain ("80Hz"), gain ("220Hz"), gain ("480Hz"), gain ("1.1kHz"), gain ("2.2kHz") }),
        model ("parametric_eq", "Parametric EQ", EqFx::parametricEq, "Low shelf, high shelf and one fully parametric band",
               { gain ("Lows"), gain ("Highs"), freq ("Freq", 80.0f, 8000.0f, 800.0f, 800.0f), percent ("Q", 35.0f), gain ("Gain") }),
        model ("studio_eq", "Studio EQ", EqFx::studioEq, "Inspired by the API 550B",
               { freq ("Low Freq", 75.0f, 1000.0f, 500.0f, 275.0f), gain ("Low Gain"),
                 freq ("Hi Freq", 800.0f, 12500.0f, 1500.0f, 3200.0f), gain ("Hi Gain"), gain ("Gain") }),
        model ("4_band_shift_eq", "4 Band Shift EQ", EqFx::fourBandShiftEq, "Four bands whose frequencies the Shift knob spreads apart",
               { gain ("Low"), gain ("Low Mid"), gain ("Hi Mid"), gain ("Hi"), percent ("Shift", 50.0f) }),
        model ("mid_focus_eq", "Mid Focus EQ", EqFx::midFocusEq, "High-pass and low-pass with resonance, and make-up gain",
               { freq ("Hi Pass Freq", 20.0f, 2000.0f, 130.0f, 200.0f), percent ("Hi Pass Q", 25.0f),
                 freq ("Low Pass Freq", 500.0f, 20000.0f, 4500.0f, 3200.0f), percent ("Low Pass Q", 25.0f), gain ("Gain", 2.0f) }),
        model ("vintage_pre", "Vintage Pre", EqFx::vintagePre, "Requisite Y7 tube mic preamp",
               { percent ("Gain", 30.0f), decibels ("Output", -30.0f, 12.0f, 0.0f), choice ("Phase", eqPhaseNames, 2, 0),
                 freq ("Hi Pass Filter", 20.0f, 1000.0f, 20.0f, 140.0f), freq ("Lo Pass Filter", 1000.0f, 20000.0f, 20000.0f, 4500.0f) }),
    };
}

} // namespace fx
