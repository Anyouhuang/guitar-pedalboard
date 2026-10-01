# Effect engine contract

How one category of effect models (an "engine") is written for Guitar Pedalboard, so it can be built, tested and
ported on its own and then dropped into the signal chain. Read this whole file, then the example engine:

- `Source/DSP/fx/Volume.h` (C++ engine + model list)
- `Tests/standalone/volume_test.cpp` (its test)
- `web/src/dsp/volume.js` (its JavaScript port)

## What the project is

A guitar multi-effects unit: a JUCE plugin (C++) and a web version (JavaScript in an AudioWorklet) that must sound
the same. The board is a Line 6 POD HD500X-style chain of 8 FX slots plus an amp/cab block. Each slot holds one
**model**; models of one category share one **engine** class. The model names and knobs follow the HD500X list in
`docs/hd500x-models.md`, but the DSP is our own approximation of the original hardware each model is "based on".
Write real DSP that captures what makes each original recognisable (its circuit topology, its typical frequency
response, how its controls interact), not a renamed generic effect. Put a short comment on each model saying what
the original is and which traits you modelled.

## Files you own

For an engine named `<name>` (given in your task) create exactly these and edit nothing else:

| File | Contents |
|---|---|
| `Source/DSP/fx/<Header>.h` | the C++ engine class and `inline std::vector<ModelInfo> <name>Models()` |
| `Tests/standalone/<name>_test.cpp` | `harness::run<...>` plus your own extra checks |
| `web/src/dsp/<name>.js` | the JavaScript port: the class and the `..._MODELS` array |

Shared files are off limits (other engines are being written at the same time and rely on them staying as they
are): `DspUtils.h`, `ModelTypes.h`, `Models.h`, `FxChain.*`, `Tests/standalone/Harness.h`, `build.ps1`,
`web/src/dsp/core.js`, `modeltypes.js`, `chain.js`, `engines.js`, `tools.js`, `web/src/dsp.js`, `web/build.mjs`,
`web/test/*.mjs`, the UI, the README. If you need something changed there, say so in your final report instead.
Do not run the CMake build, `PedalTest` or `web/build.mjs`: the standalone test below is your build.

## The C++ engine

Header-only, no JUCE. Include only `../DspUtils.h`, `../ModelTypes.h` and standard headers.

```cpp
namespace fx
{
class XxxFx
{
public:
    enum Variant { firstModel = 0, /* ... */ numVariants };

    void prepare (double sampleRate, int maxBlockSize); // allocate everything here (may be called again with another rate)
    void reset();                                       // clear ALL audio state, snap smoothed values to their targets
    void setModel (int variant);                        // only selects the model
    void setParameters (const float* knobs);            // plain units in the model's knob order; called before every block
    void process (float* left, float* right, int numSamples); // in place; numSamples <= maxBlockSize
};

inline std::vector<ModelInfo> xxxModels(); // entry i has variant == i
}
```

How the slot drives it:
- When a model is picked or the slot is switched on: `setModel`, `setParameters`, `reset()`, then (modulation,
  filter and pitch engines only) 50 ms of warm-up where `process` runs but its output is thrown away, then a 30 ms
  crossfade from the dry signal to the processed one. Switching off crossfades back to dry and stops calling
  `process`. So an engine never handles bypass, and empty buffers at switch-on are not a click risk.
- `setParameters` is called before **every** block with the current knob values: keep it cheap, never allocate,
  and smooth anything that would zipper or click when a knob moves (gains and mixes over ~30-50 ms with
  `Smoothed`; glide delay times; recompute filter coefficients per block at most).
- After `reset()` the engine must behave exactly like a freshly prepared one at the same settings: clear delay
  lines, filters, envelopes, LFO phases, random seeds, counters. The harness checks this bit for bit.
- `process` always gets both channels. The input is often the same on both sides (mono guitar) but not always.
  HD500X **mono** models collapse the signal: `m = 0.5f * (left + right)`, process `m`, write it to both sides.
  **Stereo** models keep separate state per side and may widen. The "St" column of the HD500X list says which.

**Delay and reverb engines return the wet signal only** (after `process`, `left`/`right` hold just the echoes or
the reverb) and add:

```cpp
    float getMix() const;          // 0..1 from the Mix knob (current setParameters value)
    float getTailSeconds() const;  // how long after the input stops the output may still sound (> 0)
```

The slot mixes: dry gain `min (1, 2 - 2 mix)`, wet gain `min (1, 2 mix)` (both at unity at 50 %). When switched
off it fades the engine's *input* to silence and keeps calling `process` until the wet output has been below
-100 dB for `getTailSeconds()`, so echoes and tails ring out. The engine itself ignores the Mix knob otherwise.

Rules:
- No allocation, locks or I/O in `setParameters` / `process`. All buffers sized in `prepare` for the given rate.
- Everything in your header that is not the engine class or the models function goes in a uniquely named nested
  namespace (e.g. `namespace fx::mod_detail`) or is a private nested class: all engine headers end up included
  together. Everything `inline`.
- Must compile cleanly with `/W4`. Work at any sample rate from 44.1 to 192 kHz (derive coefficients from `fs`).
- Float (32-bit) for audio samples and state, like the rest of the code; `double` for phases and coefficient maths.

Building blocks in `DspUtils.h`: `pi`, `dbToGain`, `gainToDb`, `Biquad` (RBJ low/high-pass, peak, shelves,
`getMagnitude`), `OnePole` (TPT low/high-pass), `DcBlocker`, `DelayLine` (Hermite fractional read, `read (d)`
with d >= 1), `Smoothed` (linear or multiplicative ramps), `Oversampler4x` (polyphase IIR, for anything that
clips hard). Anything else you need, write in your own files.

## Models and knobs

`ModelTypes.h` has `ModelInfo`, `KnobSpec` and the knob constructors (`percent`, `decibels`, `millis`, `hertz`,
`freq`, `semitones`, `choice`, `knob10`). Your models function returns the models in variant order with your
`Category` / `Engine` values (given in your task).

- Use the HD500X's model names and knob names and order (`docs/hd500x-models.md`). At most 8 knobs per model.
- The HD500X shows most knobs as 0-100 %: use `percent`. Times are `millis`, LFO speeds `hertz` (give a sensible
  range, e.g. 0.05-10 Hz with the centre around 1 Hz), audio frequencies `freq`, levels/gains `decibels`, pitch
  `semitones`. Switches and modes (LP/BP/HP, Up/Down, wave shapes...) are `choice` knobs: declare the names as
  `inline const char* const xxxNames[] = { ... };` in your header.
- Defaults must sound good and like the effect's typical use, at roughly the input's loudness.
- `key`: snake_case of the model name, unique in the whole project. Already taken: `empty noise_gate tube_drive
  screamer overdrive classic_dist heavy_dist color_drive buzz_saw facial_fuzz jumbo_fuzz fuzz_pi jet_fuzz
  line6_drive line6_dist sub_oct_fuzz octave_fuzz chorus flanger phaser tremolo analog_delay room_reverb
  volume_pedal pan`. If the natural key is taken, append `_hd`.
- `basedOn`: the original hardware as Line 6 names it (or a few words for Line 6 originals).
- Leave `stereo` and `trails` at their defaults except: delay and reverb models set `trails = true`.
- Tempo sync (delay models only): put a `choice ("Note", delayNoteNames, numDelayNotes, 1)` knob right after the
  Time knob and set `timeKnob`, `noteKnob`, `noteBeats = delayNoteBeats`. The engine just reads the Time knob
  (the caller overwrites it with the synced time when Note is not "ms") and ignores the Note knob.

## The JavaScript port

`web/src/dsp/<name>.js`, an ES module that imports only from `./core.js` and `./modeltypes.js`, and exports
exactly two things, each with its own `export` statement: the class (same name as the C++ class) and the models
array (e.g. `export const MOD_MODELS = [ model (...), ... ];`). Everything else stays file-local. Same methods as
the C++ class: `prepare(sampleRate, maxBlock)`, `reset()`, `setModel(variant)`, `setParameters(knobs)`,
`process(left, right, n)` on `Float32Array`s (+ `getMix()`, `getTailSeconds()` for delay / reverb).

It must produce the same samples as the C++ (the test requires every case to differ by less than -60 dB; aim for
-80 dB or better). Port line by line, same order of operations. What matters:

- C++ stores `float`s; JS numbers are doubles. Wrap values that C++ keeps in `float` members and computes once
  per block (coefficients, smoothing targets, gains) in `f32()` (`Math.fround`). Per-sample maths may stay in
  doubles; if a recursive path with a lot of feedback drifts, keep its state in a `Float32Array` or `f32()` it.
- `Smoothed` in JS: `new Smoothed(initial, multiplicative)`, `.reset(fs, seconds)`, `.setTarget(v)`,
  `.setCurrentAndTarget(v)`, `.next()`, `.skip(n)`, `.isSmoothing()`, fields `.current` / `.target`.
  `Oversampler4x` in JS: `new Oversampler4x(maxBlock)`, `.reset()`, `.up(data, n)` returns the 4n-sample
  buffer to process in place, `.down(out, n)`.
- Decisions must not hinge on the last bit of a float: comparators, zero-crossing and pitch detectors,
  sample-and-hold triggers, envelope gates need hysteresis or integer counters, or the two versions will take
  different branches. Random numbers: the harness's LCG (`state = state * 1664525 + 1013904223` on uint32; in JS
  `state = (Math.imul(state, 1664525) + 1013904223) >>> 0`; value `(state >>> 8) / 16777216`), seeded in `reset()`.
- `Math.sin` / `std::sin` on doubles agree closely enough; keep LFO phases in doubles on both sides.
- Allocate typed arrays in `prepare`, not in `process` (the AudioWorklet must not trigger garbage collection).

## Testing (both must print ALL CHECKS PASSED)

```
powershell -ExecutionPolicy Bypass -File Tests\standalone\build.ps1 -Name <name>
node web/test/standalone.mjs <name> <ClassName> <MODELS_EXPORT>
```

The first compiles `Tests/standalone/<name>_test.cpp` (a few seconds) and runs it. For every model
`harness::run` renders a 5 s plucked-string riff (peaks at 0.4, silent from 4 s on) and checks: level at default
knobs (-20..+12 dB vs the input by default; delay/reverb: not silent), no NaN/Inf, every knob at both ends, knobs
jumping to random values every 0.2 s, silence in -> silence out, 44.1 and 96 kHz, that `reset()` equals a fresh
engine, and speed. It writes reference renders to `test_output/standalone/<name>/`, which the second command
replays through the JavaScript class and compares sample by sample. `harness::Options` lets you widen a limit
where a model genuinely needs it (say why in a comment).

The harness cannot tell whether a model does what its name promises. **Add your own checks** to your
`<name>_test.cpp` `main` (after `harness::run`, counting failures the same way) that measure the defining
behaviour of each model: a filter's frequency response, a tremolo's rate and depth, a delay's time from an
impulse, a pitch shifter's output frequency, a compressor's gain reduction, that two "different" models really
differ. Treat these like unit tests of the sound; they are the only ears this project has.

Speed matters: up to 8 slots plus the amp run at once, in JavaScript, possibly on a phone. Aim for 50x realtime
or more per model in C++ and 20x or more in node; the tests fail below 8x and 4x.

## Working in this environment

- Windows. Use PowerShell for the build script; Git Bash for `node`, `grep` etc. The Bash tool un-escapes
  backslashes inside heredocs, so create and edit source files with the Write / Edit tools, not with shell
  heredocs.
- The user may be using this PC: never play audio, never open GUI apps, never take screenshots.
- No network access is needed; everything you need about the HD500X is in `docs/hd500x-models.md`.

## Final report

When both tests pass, reply with: the models and their knobs (name, range, default); the harness table (levels,
speed) and the worst JavaScript difference; what each of your own checks measured; every place where you
simplified or deviated from the HD500X list and why; and anything you need changed in shared code.
