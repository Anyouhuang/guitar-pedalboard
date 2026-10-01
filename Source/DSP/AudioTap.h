#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace fx
{
/** Lock-free single-producer / single-consumer sample queue: the audio thread pushes,
    the UI pulls (for the tuner and the spectrum analyser). If the UI isn't reading, samples are dropped. */
class AudioTap
{
public:
    explicit AudioTap (int capacity = 32768) : fifo (capacity), buffer ((size_t) capacity) {}

    void push (const float* data, int numSamples) noexcept
    {
        const auto scope = fifo.write (numSamples);

        if (scope.blockSize1 > 0)
            std::copy_n (data, scope.blockSize1, buffer.data() + scope.startIndex1);

        if (scope.blockSize2 > 0)
            std::copy_n (data + scope.blockSize1, scope.blockSize2, buffer.data() + scope.startIndex2);
    }

    int pull (float* dest, int maxSamples)
    {
        const auto scope = fifo.read (std::min (maxSamples, fifo.getNumReady()));

        if (scope.blockSize1 > 0)
            std::copy_n (buffer.data() + scope.startIndex1, scope.blockSize1, dest);

        if (scope.blockSize2 > 0)
            std::copy_n (buffer.data() + scope.startIndex2, scope.blockSize2, dest + scope.blockSize1);

        return scope.blockSize1 + scope.blockSize2;
    }

private:
    juce::AbstractFifo fifo;
    std::vector<float> buffer;
};

} // namespace fx
