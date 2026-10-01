#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

namespace fx
{
constexpr double pi = 3.14159265358979323846;

inline float dbToGain (float db) noexcept   { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float gain) noexcept { return gain > 1.0e-9f ? 20.0f * std::log10 (gain) : -180.0f; }

//==============================================================================
/** RBJ-cookbook biquad, transposed direct form II. Coefficient updates never allocate. */
struct Biquad
{
    void reset() noexcept { z1 = z2 = 0.0f; }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setLowPass (double fs, double fc, double q)
    {
        const auto [cosw, alpha] = prewarp (fs, fc, q);
        set ((1.0 - cosw) * 0.5, 1.0 - cosw, (1.0 - cosw) * 0.5, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
    }

    void setHighPass (double fs, double fc, double q)
    {
        const auto [cosw, alpha] = prewarp (fs, fc, q);
        set ((1.0 + cosw) * 0.5, -(1.0 + cosw), (1.0 + cosw) * 0.5, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
    }

    void setPeak (double fs, double fc, double q, double gainDb)
    {
        const auto [cosw, alpha] = prewarp (fs, fc, q);
        const double A = std::pow (10.0, gainDb / 40.0);
        set (1.0 + alpha * A, -2.0 * cosw, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * cosw, 1.0 - alpha / A);
    }

    void setLowShelf (double fs, double fc, double gainDb)
    {
        const auto [cosw, alpha] = prewarp (fs, fc, shelfQ);
        const double A = std::pow (10.0, gainDb / 40.0), k = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1.0) - (A - 1.0) * cosw + k),
             2.0 * A * ((A - 1.0) - (A + 1.0) * cosw),
             A * ((A + 1.0) - (A - 1.0) * cosw - k),
             (A + 1.0) + (A - 1.0) * cosw + k,
             -2.0 * ((A - 1.0) + (A + 1.0) * cosw),
             (A + 1.0) + (A - 1.0) * cosw - k);
    }

    void setHighShelf (double fs, double fc, double gainDb)
    {
        const auto [cosw, alpha] = prewarp (fs, fc, shelfQ);
        const double A = std::pow (10.0, gainDb / 40.0), k = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1.0) + (A - 1.0) * cosw + k),
             -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw),
             A * ((A + 1.0) + (A - 1.0) * cosw - k),
             (A + 1.0) - (A - 1.0) * cosw + k,
             2.0 * ((A - 1.0) - (A + 1.0) * cosw),
             (A + 1.0) - (A - 1.0) * cosw - k);
    }

    /** Linear magnitude response at `freq` (used to draw EQ curves). */
    double getMagnitude (double freq, double fs) const
    {
        const auto zInv1 = std::polar (1.0, -2.0 * pi * freq / fs);
        const auto zInv2 = zInv1 * zInv1;
        const auto num = (double) b0 + (double) b1 * zInv1 + (double) b2 * zInv2;
        const auto den = 1.0 + (double) a1 * zInv1 + (double) a2 * zInv2;
        return std::abs (num) / std::abs (den);
    }

private:
    static constexpr double shelfQ = 0.70710678; // shelf slope S = 1
    static std::pair<double, double> prewarp (double fs, double fc, double q)
    {
        const double w0 = 2.0 * pi * std::clamp (fc, 1.0, 0.49 * fs) / fs;
        return { std::cos (w0), std::sin (w0) / (2.0 * q) };
    }

    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2)
    {
        b0 = float (nb0 / na0);  b1 = float (nb1 / na0);  b2 = float (nb2 / na0);
        a1 = float (na1 / na0);  a2 = float (na2 / na0);
    }

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
};

//==============================================================================
/** Topology-preserving one-pole filter (low-pass and high-pass outputs). */
struct OnePole
{
    void setCutoff (double fs, double fc)
    {
        const double g = std::tan (pi * std::clamp (fc, 1.0, 0.49 * fs) / fs);
        G = float (g / (1.0 + g));
    }

    void reset() noexcept { s = 0.0f; }

    float lowPass (float x) noexcept
    {
        const float v  = (x - s) * G;
        const float lp = v + s;
        s = lp + v;
        return lp;
    }

    float highPass (float x) noexcept { return x - lowPass (x); }

private:
    float G = 0.0f, s = 0.0f;
};

//==============================================================================
/** Removes DC offset left behind by asymmetric clipping (~10 Hz high-pass). */
struct DcBlocker
{
    void prepare (double fs) { r = float (1.0 - 2.0 * pi * 10.0 / fs); reset(); }
    void reset() noexcept    { x1 = y1 = 0.0f; }

    float process (float x) noexcept
    {
        const float y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return y;
    }

private:
    float r = 0.999f, x1 = 0.0f, y1 = 0.0f;
};

//==============================================================================
/** Circular buffer delay line with 4-point Hermite interpolation. */
struct DelayLine
{
    void prepare (int maxDelaySamples)
    {
        buffer.assign ((size_t) maxDelaySamples + 4, 0.0f);
        writePos = 0;
    }

    void reset() { std::fill (buffer.begin(), buffer.end(), 0.0f); writePos = 0; }

    int getMaxDelay() const noexcept { return (int) buffer.size() - 4; }

    void push (float x) noexcept
    {
        buffer[(size_t) writePos] = x;
        if (++writePos == (int) buffer.size())
            writePos = 0;
    }

    /** Reads the sample written `delay` samples before the most recent push (delay >= 1). */
    float read (float delay) const noexcept
    {
        delay = std::clamp (delay, 1.0f, (float) getMaxDelay());
        const int   size = (int) buffer.size();
        const int   di   = (int) delay;
        const float frac = delay - (float) di;

        auto at = [&] (int d)
        {
            int idx = writePos - d;
            while (idx < 0) idx += size;
            return buffer[(size_t) idx];
        };

        const float xm1 = at (di - 1 < 1 ? 1 : di - 1);
        const float x0  = at (di);
        const float x1  = at (di + 1);
        const float x2  = at (di + 2);

        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * frac + c2) * frac + c1) * frac + x0;
    }

private:
    std::vector<float> buffer;
    int writePos = 0;
};

//==============================================================================
/** Parameter smoothing with the same stepping rules as juce::SmoothedValue (linear or multiplicative),
    without needing JUCE - so the effect engines can be built and tested on their own. */
class Smoothed
{
public:
    explicit Smoothed (float initial = 0.0f, bool multiplicativeRamp = false) noexcept
        : current (initial), target (initial), multiplicative (multiplicativeRamp) {}

    /** Sets the ramp length; jumps to the current target. */
    void reset (double sampleRate, double rampSeconds) noexcept
    {
        stepsToTarget = (int) std::floor (rampSeconds * sampleRate);
        setCurrentAndTarget (target);
    }

    void setCurrentAndTarget (float v) noexcept { target = current = v; countdown = 0; }

    void setTarget (float v) noexcept
    {
        const float d = std::abs (v - target);
        if (d <= std::numeric_limits<float>::min() || d <= std::numeric_limits<float>::epsilon() * std::max (std::abs (v), std::abs (target)))
            return;

        if (stepsToTarget <= 0)
        {
            setCurrentAndTarget (v);
            return;
        }

        target = v;
        countdown = stepsToTarget;
        step = multiplicative ? std::exp ((std::log (std::abs (target)) - std::log (std::abs (current))) / (float) countdown)
                              : (target - current) / (float) countdown;
    }

    bool isSmoothing() const noexcept { return countdown > 0; }
    float getCurrent() const noexcept { return current; }
    float getTarget() const noexcept  { return target; }

    float next() noexcept
    {
        if (countdown <= 0)
            return target;

        if (--countdown > 0)
        {
            if (multiplicative) current *= step; else current += step;
        }
        else
        {
            current = target;
        }
        return current;
    }

    /** Advances `n` samples at once and returns the value there. */
    float skip (int n) noexcept
    {
        if (n >= countdown)
        {
            setCurrentAndTarget (target);
            return target;
        }

        if (multiplicative) current *= std::pow (step, (float) n); else current += step * (float) n;
        countdown -= n;
        return current;
    }

private:
    float current, target, step = 0.0f;
    int countdown = 0, stepsToTarget = 0;
    bool multiplicative;
};

//==============================================================================
/** 4x oversampling for one channel, with two polyphase IIR half-band stages: the same structure and
    coefficients as juce::dsp::Oversampling (order 2, filterHalfBandPolyphaseIIR, max quality), JUCE-free.
    Usage: float* x = os.up (in, n);  ...process 4 * n samples of x in place...;  os.down (out, n); */
class Oversampler4x
{
public:
    void prepare (int maxBlockSize)
    {
        buffer2.assign ((size_t) maxBlockSize * 2, 0.0f);
        buffer4.assign ((size_t) maxBlockSize * 4, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : stages)
            s = {};
    }

    /** Latency of up + down together, in samples at the base rate (approximate, as JUCE reports it). */
    static constexpr float latencySamples = 4.43f;

    float* up (const float* input, int n) noexcept
    {
        processUp (0, input, n, buffer2.data());
        processUp (1, buffer2.data(), n * 2, buffer4.data());
        return buffer4.data();
    }

    void down (float* output, int n) noexcept
    {
        processDown (1, buffer4.data(), n * 2, buffer2.data());
        processDown (0, buffer2.data(), n, output);
    }

private:
    struct Coefficients { const float* values; int count; };
    struct State { float up[8] {}, down[8] {}; float delayDown = 0.0f; };

    static Coefficients upCoefficients (int stage) noexcept
    {
        static constexpr float s0[] = { 0.0457281470f, 0.332501113f, 0.663202047f, 0.933855832f, 0.168087542f, 0.504485726f, 0.803780854f };
        static constexpr float s1[] = { 0.0542307794f, 0.398796976f, 0.862917840f, 0.199699581f, 0.621096849f };
        return stage == 0 ? Coefficients { s0, 7 } : Coefficients { s1, 5 };
    }

    static Coefficients downCoefficients (int stage) noexcept
    {
        static constexpr float s0[] = { 0.0542175248f, 0.383087337f, 0.748720944f, 0.196797967f, 0.573136389f, 0.914293706f };
        static constexpr float s1[] = { 0.0707659498f, 0.513167560f, 0.257853091f, 0.817317367f };
        return stage == 0 ? Coefficients { s0, 6 } : Coefficients { s1, 4 };
    }

    void processUp (int stage, const float* input, int n, float* out) noexcept
    {
        const auto c = upCoefficients (stage);
        float* v = stages[stage].up;
        const int direct = c.count - (c.count >> 1);

        for (int i = 0; i < n; ++i)
        {
            float x = input[i];
            for (int k = 0; k < direct; ++k)       { const float y = c.values[k] * x + v[k]; v[k] = x - c.values[k] * y; x = y; }
            out[i << 1] = x;

            x = input[i];
            for (int k = direct; k < c.count; ++k) { const float y = c.values[k] * x + v[k]; v[k] = x - c.values[k] * y; x = y; }
            out[(i << 1) + 1] = x;
        }
    }

    void processDown (int stage, const float* buffer, int n, float* out) noexcept
    {
        const auto c = downCoefficients (stage);
        float* v = stages[stage].down;
        const int direct = c.count - (c.count >> 1);
        float delay = stages[stage].delayDown;

        for (int i = 0; i < n; ++i)
        {
            float x = buffer[i << 1];
            for (int k = 0; k < direct; ++k)       { const float y = c.values[k] * x + v[k]; v[k] = x - c.values[k] * y; x = y; }
            const float directOut = x;

            x = buffer[(i << 1) + 1];
            for (int k = direct; k < c.count; ++k) { const float y = c.values[k] * x + v[k]; v[k] = x - c.values[k] * y; x = y; }

            out[i] = (delay + directOut) * 0.5f;
            delay = x;
        }

        stages[stage].delayDown = delay;
    }

    State stages[2];
    std::vector<float> buffer2, buffer4;
};

} // namespace fx
