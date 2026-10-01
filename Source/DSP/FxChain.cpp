#include "FxChain.h"

namespace fx
{
FxParams defaultBoard()
{
    FxParams p;
    p.slots[0] = makeSlot ("noise_gate", true);
    p.slots[1] = makeSlot ("screamer", true);
    p.slots[2] = makeSlot ("chorus", false);
    p.slots[3] = makeSlot ("analog_delay", false);
    p.slots[4] = makeSlot ("room_reverb", true);
    p.amp = makeAmp ("", defaultCabKey, true);
    p.ampPosition = 2;
    return p;
}

//==============================================================================
Slot::Kind Slot::kindOf (Engine e) noexcept
{
    switch (e)
    {
        case Engine::none:       return Kind::none;
        case Engine::gate:
        case Engine::distortion:
        case Engine::modulation:
        case Engine::delay:
        case Engine::reverb:     return Kind::first;
        case Engine::delayFx:
        case Engine::reverbFx:   return Kind::send;
        default:                 return Kind::insert;
    }
}

void Slot::prepare (double sampleRate, int maxBlockSize)
{
    fs = sampleRate;
    gate.prepare (sampleRate);
    distortion.prepare (sampleRate, maxBlockSize);
    modulation.prepare (sampleRate);
    delay.prepare (sampleRate);
    reverb.prepare (sampleRate, maxBlockSize);

    dynamicsFx.prepare (sampleRate, maxBlockSize); refs[(size_t) Engine::dynamicsFx] = EngineRef::to (dynamicsFx);
    modFx.prepare (sampleRate, maxBlockSize); refs[(size_t) Engine::modFx] = EngineRef::to (modFx);
    filterFx.prepare (sampleRate, maxBlockSize); refs[(size_t) Engine::filterFx] = EngineRef::to (filterFx);
    pitchFx.prepare (sampleRate, maxBlockSize); refs[(size_t) Engine::pitchFx] = EngineRef::to (pitchFx);
    eqFx.prepare (sampleRate, maxBlockSize); refs[(size_t) Engine::eqFx] = EngineRef::to (eqFx);
    delayFx.prepare (sampleRate, maxBlockSize); refs[(size_t) Engine::delayFx] = EngineRef::to (delayFx);
    verbFx.prepare (sampleRate, maxBlockSize); refs[(size_t) Engine::reverbFx] = EngineRef::to (verbFx);
    wahFx.prepare (sampleRate, maxBlockSize); refs[(size_t) Engine::wahFx] = EngineRef::to (wahFx);
    volumeFx.prepare (sampleRate, maxBlockSize);
    refs[(size_t) Engine::volumeFx] = EngineRef::to (volumeFx);

    fade.prepare (sampleRate);
    send.reset (sampleRate, 0.03);
    mix.reset (sampleRate, 0.05);
    active = requested = 0;
    reset();
}

void Slot::reset()
{
    gate.reset();
    distortion.reset();
    modulation.reset();
    delay.reset();
    reverb.reset();

    if (active != requested)
        knobs = pendingKnobs;
    active = requested;
    needsReset = false;
    warmupLeft = 0;
    silentSamples = 0;
    configure();
    resetEngine();
    configure();

    // start in the switched state: no warm-up, no fade
    const auto& info = modelInfo (active);
    const auto kind = kindOf (info.engine);
    const bool audible = kind == Kind::send || (kind == Kind::first && info.trails) ? true : on;
    fade.amount.setCurrentAndTargetValue (kind != Kind::none && audible ? 1.0f : 0.0f);

    running = kind == Kind::send && on;
    send.setCurrentAndTarget (running ? 1.0f : 0.0f);
    if (kind == Kind::send && ref())
        mix.setCurrentAndTarget (ref().getMix (ref().object));
}

void Slot::setParameters (const SlotParams& p)
{
    requested = std::clamp (p.model, 0, numModels() - 1);
    on = p.on;

    // while the old model fades out it keeps its own settings; the new ones wait for it
    if (requested == active)
        knobs = p.knobs;
    else
        pendingKnobs = p.knobs;

    // nothing to fade out: an empty slot, or a delay / reverb that has gone quiet
    const auto activeKind = kindOf (modelInfo (active).engine);
    if (activeKind == Kind::none || (activeKind == Kind::send && ! running && requested != active))
        fade.amount.setCurrentAndTargetValue (0.0f);

    if (requested != active && fade.isOff())
        start();

    configure();

    const auto& info = modelInfo (active);
    const bool same = requested == active;

    switch (kindOf (info.engine))
    {
        case Kind::none:   fade.set (false); break;
        case Kind::first:  fade.set (same && (on || info.trails)); break;
        case Kind::send:   fade.set (same); break; // only fades when another model is picked
        case Kind::insert:
            if (! (same && on))
                fade.set (false);
            else if (! needsReset && warmupLeft == 0)
                fade.set (true); // otherwise process() switches it on once the warm-up is over
            break;
    }
}

void Slot::start()
{
    active = requested;
    knobs = pendingKnobs;
    warmupLeft = 0;
    running = false;
    silentSamples = 0;

    if (kindOf (modelInfo (active).engine) == Kind::first)
    {
        needsReset = false;
        configure();   // the new model's settings...
        resetEngine(); // ...as a fresh start (no smoothing from the last time it was used)
        configure();   // delay / reverb switch themselves back on after their reset
    }
    else
    {
        needsReset = true; // process() resets the engine right before it first runs
    }
}

void Slot::configure()
{
    const auto& info = modelInfo (active);
    const float* k = knobs.data();
    const bool enabled = on && active == requested;

    switch (info.engine)
    {
        case Engine::none:       break;
        case Engine::gate:       gate.setParameters (k[0], k[1]); break;
        case Engine::distortion: distortion.setModel (info.variant); distortion.setParameters (k); break;
        case Engine::modulation: modulation.setParameters (info.variant, k[0], k[1] / 100.0f, k[2] / 100.0f); break;
        case Engine::delay:      delay.setParameters (enabled, k[0], k[2] / 100.0f, k[3] / 100.0f, k[4] / 10.0f); break;
        case Engine::reverb:     reverb.setParameters (enabled, k[0] / 100.0f, k[1] / 100.0f, k[2] / 100.0f, k[3]); break;
        default:
            if (const auto& e = ref())
            {
                e.setModel (e.object, info.variant);
                e.setParameters (e.object, k);
            }
            break;
    }
}

void Slot::resetEngine()
{
    switch (modelInfo (active).engine)
    {
        case Engine::none:       break;
        case Engine::gate:       gate.reset(); break;
        case Engine::distortion: distortion.reset(); break;
        case Engine::modulation: modulation.restart(); break; // the delay lines are kept fed instead
        case Engine::delay:      delay.reset(); break;
        case Engine::reverb:     reverb.reset(); break;
        default:
            if (const auto& e = ref())
                e.reset (e.object);
            break;
    }
}

void Slot::process (float* left, float* right, int n, const SlotScratch& scratch)
{
    const auto& info = modelInfo (active);

    // the original chorus / flanger delay lines always hold the slot's recent input, so picking or switching
    // on one of them fades in on real signal instead of an empty buffer (which would click)
    if (info.engine != Engine::modulation || fade.isOff())
        modulation.feed (left, right, n);

    switch (kindOf (info.engine))
    {
        case Kind::none:   break;
        case Kind::first:  processFirst (left, right, n, scratch); break;
        case Kind::insert: if (ref()) processInsert (left, right, n, scratch); break;
        case Kind::send:   if (ref()) processSend (left, right, n, scratch); break;
    }
}

void Slot::processFirst (float* left, float* right, int n, const SlotScratch& scratch)
{
    const auto& info = modelInfo (active);

    if (fade.isOff())
    {
        needsReset = true;
        return;
    }

    if (needsReset)
    {
        if (info.engine != Engine::modulation)
            resetEngine();
        needsReset = false;
    }

    const bool full = fade.isFullyOn();
    if (! full)
    {
        std::copy (left, left + n, scratch.dryLeft);
        std::copy (right, right + n, scratch.dryRight);
    }

    if (info.stereo)
    {
        switch (info.engine)
        {
            case Engine::modulation: modulation.process (left, right, n); break;
            case Engine::delay:      delay.process (left, right, n); break;
            case Engine::reverb:     reverb.process (left, right, n); break;
            default: break;
        }
    }
    else
    {
        float* m = scratch.mid;
        for (int i = 0; i < n; ++i)
            m[i] = 0.5f * (left[i] + right[i]);

        if (info.engine == Engine::gate)
            gate.process (m, n);
        else
            distortion.process (m, n);

        std::copy (m, m + n, left);
        std::copy (m, m + n, right);
    }

    if (! full)
    {
        for (int i = 0; i < n; ++i)
        {
            const float a = fade.amount.getNextValue();
            left[i]  = scratch.dryLeft[i]  + a * (left[i]  - scratch.dryLeft[i]);
            right[i] = scratch.dryRight[i] + a * (right[i] - scratch.dryRight[i]);
        }
    }
}

void Slot::processInsert (float* left, float* right, int n, const SlotScratch& scratch)
{
    const auto& e = ref();
    const bool wanted = on && requested == active;

    if (! wanted && fade.isOff())
    {
        needsReset = true;
        warmupLeft = 0;
        return;
    }

    if (needsReset)
    {
        e.reset (e.object); // at the settings setParameters() has just applied
        needsReset = false;

        const auto engine = modelInfo (active).engine;
        const bool needsWarmup = engine == Engine::modFx || engine == Engine::filterFx || engine == Engine::pitchFx;
        warmupLeft = needsWarmup ? (int) (0.05 * fs) : 0;
        if (warmupLeft == 0)
            fade.set (wanted);
    }

    if (warmupLeft > 0)
    {
        // run on a copy and throw the result away: the signal stays dry until the effect has settled
        std::copy (left, left + n, scratch.wetLeft);
        std::copy (right, right + n, scratch.wetRight);
        e.process (e.object, scratch.wetLeft, scratch.wetRight, n);

        warmupLeft = std::max (0, warmupLeft - n);
        if (warmupLeft == 0)
            fade.set (wanted);
        return;
    }

    if (fade.isFullyOn())
    {
        e.process (e.object, left, right, n);
        return;
    }

    std::copy (left, left + n, scratch.dryLeft);
    std::copy (right, right + n, scratch.dryRight);
    e.process (e.object, left, right, n);

    for (int i = 0; i < n; ++i)
    {
        const float a = fade.amount.getNextValue();
        left[i]  = scratch.dryLeft[i]  + a * (left[i]  - scratch.dryLeft[i]);
        right[i] = scratch.dryRight[i] + a * (right[i] - scratch.dryRight[i]);
    }
}

void Slot::processSend (float* left, float* right, int n, const SlotScratch& scratch)
{
    const auto& e = ref();
    const bool enabled = on && requested == active;

    if (! running)
    {
        if (! enabled)
            return;

        e.reset (e.object);
        running = true;
        needsReset = false;
        silentSamples = 0;
        send.setCurrentAndTarget (0.0f);
        mix.setCurrentAndTarget (e.getMix (e.object));
    }
    else if (needsReset) // another model was picked while this slot was ringing
    {
        e.reset (e.object);
        needsReset = false;
    }

    send.setTarget (enabled ? 1.0f : 0.0f);
    mix.setTarget (e.getMix (e.object));

    // switched off = the engine's input is muted; what is already in it rings out
    for (int i = 0; i < n; ++i)
    {
        const float g = send.next();
        scratch.gain[i] = g;
        scratch.dryLeft[i]  = left[i];
        scratch.dryRight[i] = right[i];
        scratch.wetLeft[i]  = left[i] * g;
        scratch.wetRight[i] = right[i] * g;
    }

    if (e.processDry != nullptr)
        e.processDry (e.object, scratch.dryLeft, scratch.dryRight, n); // e.g. a tape echo's preamp colours the dry signal too

    e.process (e.object, scratch.wetLeft, scratch.wetRight, n);

    const bool fullFade = fade.isFullyOn();
    float peak = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float m = mix.next();
        const float f = fullFade ? 1.0f : fade.amount.getNextValue(); // only moves while another model is being picked
        const float dryGain = std::min (1.0f, 2.0f - 2.0f * m), wetGain = std::min (1.0f, 2.0f * m);
        const float a = scratch.gain[i] * f;
        const float wl = scratch.wetLeft[i], wr = scratch.wetRight[i];

        left[i]  += a * (scratch.dryLeft[i]  * dryGain - left[i])  + f * wetGain * wl;
        right[i] += a * (scratch.dryRight[i] * dryGain - right[i]) + f * wetGain * wr;
        peak = std::max ({ peak, std::abs (wl), std::abs (wr) });
    }

    // once switched off, stop processing when the tail has died away
    if (! enabled && ! send.isSmoothing())
    {
        silentSamples = peak < 1.0e-5f ? silentSamples + n : 0;
        if ((double) silentSamples > (double) e.getTailSeconds (e.object) * fs)
            running = false;
    }
    else
    {
        silentSamples = 0;
    }
}

//==============================================================================
void AmpBlock::prepare (double sampleRate, int maxBlockSize)
{
    amp.prepare (sampleRate, maxBlockSize);
    cab.prepare (sampleRate, maxBlockSize);
    fade.prepare (sampleRate);
    reset();
}

void AmpBlock::reset()
{
    active = requested;
    configure();
    amp.reset();
    cab.reset();
    needsReset = false;
    fade.amount.setCurrentAndTargetValue (active.on && ! held && (active.amp >= 0 || active.cab >= 0) ? 1.0f : 0.0f);
}

void AmpBlock::setParameters (const AmpParams& p, bool hold)
{
    requested = p;
    held = hold;

    if (sameModels (p, active))
    {
        active = p;
    }
    else if (fade.isOff())
    {
        active = p;
        needsReset = true;
    }

    configure();
    fade.set (p.on && ! hold && sameModels (p, active) && (active.amp >= 0 || active.cab >= 0));
}

void AmpBlock::configure()
{
    if (active.amp >= 0)
    {
        amp.setModel (active.amp);
        amp.setParameters (active.ampKnobs.data());
    }

    if (active.cab >= 0)
    {
        cab.setModel (active.cab);
        cab.setParameters (active.cabKnobs.data());
    }
}

void AmpBlock::process (float* left, float* right, int n, const SlotScratch& scratch)
{
    if (fade.isOff())
    {
        needsReset = true;
        return;
    }

    if (needsReset)
    {
        amp.reset();
        cab.reset();
        needsReset = false;
    }

    const bool full = fade.isFullyOn();
    if (! full)
    {
        std::copy (left, left + n, scratch.dryLeft);
        std::copy (right, right + n, scratch.dryRight);
    }

    if (active.amp >= 0)
        amp.process (left, right, n);
    if (active.cab >= 0)
        cab.process (left, right, n);

    if (! full)
    {
        for (int i = 0; i < n; ++i)
        {
            const float a = fade.amount.getNextValue();
            left[i]  = scratch.dryLeft[i]  + a * (left[i]  - scratch.dryLeft[i]);
            right[i] = scratch.dryRight[i] + a * (right[i] - scratch.dryRight[i]);
        }
    }
}

//==============================================================================
void FxChain::prepare (double sampleRate, int maxBlockSize)
{
    maxBlock = std::max (1, maxBlockSize);

    for (auto& slot : slots)
        slot.prepare (sampleRate, maxBlock);

    ampBlock.prepare (sampleRate, maxBlock);
    hum.prepare (sampleRate);
    denoiser.prepare (sampleRate);
    eqLeft.prepare (sampleRate);
    eqRight.prepare (sampleRate);
    eqFade.prepare (sampleRate);

    inputGain.reset (sampleRate, 0.05);
    outputGain.reset (sampleRate, 0.05);

    for (auto* v : { &mono, &dryLeft, &dryRight, &mid, &wetLeft, &wetRight, &gain, &spareRight })
        v->assign ((size_t) maxBlock, 0.0f);

    reset();
}

void FxChain::reset()
{
    for (auto& slot : slots)
        slot.reset();

    ampPosition = requestedAmpPosition;
    ampBlock.reset();
    hum.reset();
    denoiser.reset();

    eqLeft.reset();
    eqRight.reset();
    eqNeedsReset = false;
    eqFade.amount.setCurrentAndTargetValue (eqFade.amount.getTargetValue());

    inputGain.setCurrentAndTargetValue (inputGain.getTargetValue());
    outputGain.setCurrentAndTargetValue (outputGain.getTargetValue());
    inputPeak = outputPeak = 0.0f;
}

void FxChain::setParameters (const FxParams& p)
{
    inputGain.setTargetValue (dbToGain (p.inputGainDb));
    outputGain.setTargetValue (p.mute ? 0.0f : dbToGain (p.outputGainDb));

    for (size_t s = 0; s < slots.size(); ++s)
        slots[s].setParameters (p.slots[s]);

    // moving the amp block: fade it out, move it, fade it back in
    requestedAmpPosition = std::clamp (p.ampPosition, 0, numSlots);
    if (requestedAmpPosition != ampPosition && ampBlock.isOff())
        ampPosition = requestedAmpPosition;
    ampBlock.setParameters (p.amp, requestedAmpPosition != ampPosition);

    eqFade.set (p.eqOn);
    eqLeft.setParameters (p.eq);
    eqRight.setParameters (p.eq);

    hum.setMode (p.humMode);
    denoiser.setAmount (p.denoise);
}

void FxChain::process (const float* input, float* outLeft, float* outRight, int numSamples)
{
    inputPeak = outputPeak = 0.0f;

    // Hosts may send blocks larger than promised, so work through them in prepared-size chunks.
    for (int offset = 0; offset < numSamples; offset += maxBlock)
    {
        const int n = std::min (maxBlock, numSamples - offset);
        float* right = outRight != nullptr ? outRight + offset : spareRight.data();

        processChunk (input + offset, outLeft + offset, right, n);

        if (outRight == nullptr)
            for (int i = 0; i < n; ++i)
                outLeft[offset + i] = 0.5f * (outLeft[offset + i] + right[i]);
    }
}

void FxChain::processChunk (const float* input, float* left, float* right, int n)
{
    // Copy first: `input` may be the same memory as `left`.
    for (int i = 0; i < n; ++i)
    {
        mono[(size_t) i] = input[i] * inputGain.getNextValue();
        inputPeak = std::max (inputPeak, std::abs (mono[(size_t) i]));
    }

    hum.process (mono.data(), n); // before anything can amplify the hum

    std::copy (mono.begin(), mono.begin() + n, left);
    std::copy (mono.begin(), mono.begin() + n, right);

    const SlotScratch scratch { dryLeft.data(), dryRight.data(), mid.data(), wetLeft.data(), wetRight.data(), gain.data() };
    for (int s = 0; s < numSlots; ++s)
    {
        if (s == ampPosition)
            ampBlock.process (left, right, n, scratch);
        slots[(size_t) s].process (left, right, n, scratch);
    }
    if (ampPosition >= numSlots)
        ampBlock.process (left, right, n, scratch);

    runEq (left, right, n);
    denoiser.process (left, right, n);

    if (analyzerTap != nullptr)
    {
        for (int i = 0; i < n; ++i)
            mid[(size_t) i] = 0.5f * (left[i] + right[i]);
        analyzerTap->push (mid.data(), n);
    }

    for (int i = 0; i < n; ++i)
    {
        const float g = outputGain.getNextValue();
        // Safety net only: normal playing never gets near this.
        left[i]  = std::clamp (left[i]  * g, -2.0f, 2.0f);
        right[i] = std::clamp (right[i] * g, -2.0f, 2.0f);
        outputPeak = std::max ({ outputPeak, std::abs (left[i]), std::abs (right[i]) });
    }
}

void FxChain::runEq (float* left, float* right, int n)
{
    if (eqFade.isOff())
    {
        eqNeedsReset = true;
        return;
    }

    if (eqNeedsReset)
    {
        eqLeft.reset();
        eqRight.reset();
        eqNeedsReset = false;
    }

    if (eqFade.isFullyOn())
    {
        eqLeft.process (left, n);
        eqRight.process (right, n);
        return;
    }

    std::copy (left, left + n, dryLeft.data());
    std::copy (right, right + n, dryRight.data());
    eqLeft.process (left, n);
    eqRight.process (right, n);

    for (int i = 0; i < n; ++i)
    {
        const float a = eqFade.amount.getNextValue();
        left[i]  = dryLeft[(size_t) i]  + a * (left[i]  - dryLeft[(size_t) i]);
        right[i] = dryRight[(size_t) i] + a * (right[i] - dryRight[(size_t) i]);
    }
}

} // namespace fx
