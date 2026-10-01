#pragma once

#include "DspUtils.h"

namespace fx
{
/** Rough 4x12 guitar-cabinet voicing. Makes distortion listenable on headphones / monitors.
    Turn it off when you're running into a real guitar amp. */
class CabSim
{
public:
    void prepare (double fs)
    {
        highPass.setHighPass (fs, 75.0, 0.707);
        thump   .setPeak     (fs, 120.0, 1.4, 3.0);
        boxy    .setPeak     (fs, 450.0, 1.0, -3.5);
        presence.setPeak     (fs, 2300.0, 1.3, 4.0);
        lowPass1.setLowPass  (fs, 5000.0, 0.6);
        lowPass2.setLowPass  (fs, 6500.0, 0.9);
        reset();
    }

    void reset()
    {
        for (auto* f : { &highPass, &thump, &boxy, &presence, &lowPass1, &lowPass2 })
            f->reset();
    }

    void process (float* data, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float x = highPass.process (data[i]);
            x = thump.process (x);
            x = boxy.process (x);
            x = presence.process (x);
            x = lowPass1.process (x);
            data[i] = lowPass2.process (x);
        }
    }

private:
    Biquad highPass, thump, boxy, presence, lowPass1, lowPass2;
};

} // namespace fx
