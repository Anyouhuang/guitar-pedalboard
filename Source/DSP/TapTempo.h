#pragma once

#include <array>
#include <cmath>

namespace fx
{
//==============================================================================
/** Turns taps into a beat length: averages the last few intervals, starts over after a pause
    or when the tapping clearly changes speed, and ignores switch bounce. */
class TapTempo
{
public:
    static constexpr double maxGapMs = 2000.0;    // slower than 30 BPM = a new sequence
    static constexpr double minIntervalMs = 100.0; // faster than 600 BPM = a bounce

    /** Registers a tap at `nowMs`. Returns the averaged beat length in ms, or 0 until there are two taps. */
    double tap (double nowMs)
    {
        const double interval = nowMs - lastTapMs;

        if (hasLastTap && interval < minIntervalMs)
            return average();

        if (! hasLastTap || interval > maxGapMs)
        {
            count = 0;
        }
        else
        {
            if (count > 0 && std::abs (interval - average()) > 0.35 * average())
                count = 0; // tempo changed: forget the old intervals

            intervals[(size_t) (next++ % intervals.size())] = interval;
            count = std::min (count + 1, (int) intervals.size());
        }

        hasLastTap = true;
        lastTapMs = nowMs;
        return average();
    }

    double getLastTapMs() const noexcept { return hasLastTap ? lastTapMs : 0.0; }

private:
    double average() const
    {
        if (count == 0)
            return 0.0;

        double sum = 0.0;
        for (int i = 0; i < count; ++i)
            sum += intervals[(size_t) ((next - 1 - i + (int) intervals.size() * 4) % (int) intervals.size())];
        return sum / count;
    }

    std::array<double, 4> intervals {};
    int count = 0, next = 0;
    double lastTapMs = 0.0;
    bool hasLastTap = false;
};

} // namespace fx
