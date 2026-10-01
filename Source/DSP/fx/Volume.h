#pragma once

#include "../DspUtils.h"
#include "../ModelTypes.h"

namespace fx
{
/** The HD500X's Volume/Pan models (2). Stereo: each side keeps its own signal.
    Knobs:  Volume Pedal: Volume (100 % = unity)   |   Pan: Pan (0 % = left, 50 % = centre, 100 % = right) */
class VolumeFx
{
public:
    enum Variant { volumePedal = 0, pan, numVariants };

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        gainLeft.reset (sampleRate, 0.03);
        gainRight.reset (sampleRate, 0.03);
        reset();
    }

    void reset()
    {
        gainLeft.setCurrentAndTarget (gainLeft.getTarget());
        gainRight.setCurrentAndTarget (gainRight.getTarget());
    }

    void setModel (int newVariant) noexcept { variant = std::clamp (newVariant, 0, (int) numVariants - 1); }

    void setParameters (const float* k)
    {
        const float position = k[0] / 100.0f;

        if (variant == volumePedal)
        {
            // audio taper, like a volume pedal's pot
            gainLeft.setTarget (position * position);
            gainRight.setTarget (position * position);
        }
        else
        {
            // balance: both sides at unity in the centre
            gainLeft.setTarget (std::min (1.0f, 2.0f - 2.0f * position));
            gainRight.setTarget (std::min (1.0f, 2.0f * position));
        }
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            left[i]  *= gainLeft.next();
            right[i] *= gainRight.next();
        }
    }

private:
    int variant = volumePedal;
    Smoothed gainLeft { 1.0f }, gainRight { 1.0f };
};

/** In the order of VolumeFx::Variant. */
inline std::vector<ModelInfo> volumeModels()
{
    return {
        { "volume_pedal", "Volume Pedal", Category::volume, Engine::volumeFx, VolumeFx::volumePedal,
          "Volume pedal (100 % = unity)", { percent ("Volume", 100.0f) } },
        { "pan", "Pan", Category::volume, Engine::volumeFx, VolumeFx::pan,
          "Pan / balance (50 % = centre)", { percent ("Pan", 50.0f) } },
    };
}

} // namespace fx
