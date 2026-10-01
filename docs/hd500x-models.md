# Line 6 POD HD500X — complete model list (final firmware 2.62)

Compiled 2026-09-30 for DSP design. The primary source is the **POD HD500X Advanced Guide Rev A** (Line 6): model names, parameter labels, amp/cab/mic "based on", impedance ratings, looper and FX loop. Cross-checked against:
- **POD HD Series Model Gallery Rev E**: FX "based on" and amp background.
- **M5/M9/M13 FX Parameters guide Rev B**: the same FX algorithms (the HD FX come "primarily from the M13"). Knob functions, ranges and screenshots come from here.
- **POD HD500 Advanced Guide v2.10 and Rev F**: the model list is identical to the HD500X Rev A except for Hard Gate (see §12).
- **Line 6 KB**: "All HD amp models in order incl. model packs", the POD HD FAQ, and the v2.62 release notes.
- **podhd-java reverse-engineered preset IDs** (GitHub): an independent check of the FX list.
- **MeAmBobbo (foobazaar) wiki/guide**: per-model stereo class, amp/cab behaviour notes.
- **Line 6 community threads**: model-pack device names.

Legend:
- **St** = stereo class, taken from the MeAmBobbo wiki table (M-series based). The values are Mono, **ST/M** (stereo through, mono processing), **ST/S** (stereo through, stereo effect) and **TS** (true stereo).
- The HD500X guide itself only says: "all EQs, Wahs & Volume, and some Modulations, Filters, Pitches and Delays, as well as the FX Loop preserve stereo; all Dynamics, Distortions, all Amps & Preamps … are mono."
- **gk:** marks general circuit knowledge about the original hardware. It is not stated by Line 6 and is included only as a DSP hint.
- **In-Z** is the "Auto" guitar input impedance the unit applies when that model is first in the chain. It is 1 MΩ unless listed.

## 0. Architecture (context for the port)

- **Layout:** 8 FX blocks and 1 or 2 amp blocks. The amp can sit Pre, Post, or in parallel Paths A/B (two amps). The Mixer block has Level A/B (0 dB = unity) and Pan A/B.
- **Amps are mono.** A mono model "mono-izes" the stereo path after it.
- **Tempo:** mod, filter and delay Speed/Time take either Hz/ms or a note division of Tap Tempo, selected by the per-model "Tempo Sync" control in HD Edit. Tempo itself can be per-preset or global.
- **Trails:** a per-preset On/Off setting that lets delay and reverb tails ring after bypass. It does not spill over across preset changes.
- **Parameter display:** most FX parameters are shown as 0–100%. The exceptions carry units: Time (ms or note), Speed (Hz or note), Pitch (semitones), EQ bands (dB), and Vintage Pre HPF/LPF (Hz/kHz).
- **Presets** store every parameter as a 32-bit float in 0..1 (podhd-java notes).

## 1. Dynamics (9) — all mono

| Model | Based on (Line 6) | HD500X parameters | DSP notes |
|---|---|---|---|
| Noise Gate | — | Threshold, Decay | Simple gate/expander. |
| Hard Gate | — (HD-only, not in M-series) | Open Threshold, Close Threshold, Hold, Decay | Gate with hysteresis and hold. L6: usable for "stuttering and lopped-off power chords". |
| Tube Comp | Teletronix LA-2A | Threshold, Level | Lower Threshold means more compression, with automatic make-up gain tied to Threshold. gk: optical T4 cell with program-dependent 2-stage release. |
| Red Comp | MXR Dyna Comp | Sustain, Level | Sustain = inverse threshold. gk: OTA (CA3080) squash, fast attack. |
| Blue Comp | Boss CS-1, treble switch off | Sustain, Level | |
| Blue Comp Treb | Boss CS-1, treble switch on | Sustain, Level | Same as Blue Comp plus a treble lift. |
| Vetta Comp | Line 6 Vetta II amp | Sensitivity, Level | Fixed ratio 2.35:1, adjustable threshold, up to +12 dB gain on Level. |
| Vetta Juice | Line 6 Vetta II amp | Amount, Level | Amount = variable compression ratio. Level has 30 dB of gain. |
| Boost Comp | Inspired by MXR Micro Amp | Drive, Bass, Comp, Treble, Output | Clean boost ("goose the amp") plus Dyna-Comp-style compression on the Comp knob. |

## 2. Distortion (15) — all mono

Common knobs: Drive, Bass, Mid, Treble, Output (exceptions noted). Setting Drive to minimum gives a near-clean boost.

| Model | Based on | Parameters / In-Z | DSP notes |
|---|---|---|---|
| Tube Drive | Chandler Tube Driver | common | Single 12AX7 stage (L6). Singing sustain. |
| Screamer | Ibanez TS-808 Tube Screamer | Drive, Bass, **Tone**, Treble, Output; In-Z 230k | Tone = the original TS tone knob. gk: op-amp with Si diodes in the feedback loop (soft clip), low-cut before clipping, ~720 Hz mid hump. |
| Overdrive | DOD Overdrive/Preamp 250 | common | The original had only gain and level; Line 6 added the EQ. gk: op-amp gain into diodes to ground (hard clip). |
| Classic Dist | ProCo Rat | Drive, Bass, **Filter**, Treble, Output | Filter works like the Rat's: brighter at low settings, darker at high. gk: LM308 (slew-limited) op-amp, 1N914 hard clip to ground. |
| Heavy Dist | Boss MT-2 Metal Zone | common | "Heavy and scooped." gk: cascaded op-amp gain with diode clipping and an active parametric-mid EQ. |
| Color Drive | Colorsound Overdriver | common; In-Z 136k | Mid at 50% = flat, below = cut, above = boost. gk: 3-transistor discrete overdrive. |
| Buzz Saw | Maestro Fuzz-Tone | common; In-Z 230k | gk: germanium fuzz, sputtery/gated. |
| Facial Fuzz | Arbiter Fuzz Face | common; In-Z **22k** | gk: 2-transistor feedback fuzz, germanium. The low input Z interacts with the pickup and the guitar volume knob. |
| Jumbo Fuzz | Vox Tone Bender | common; In-Z 90k | gk: 3 germanium transistors. |
| Fuzz Pi | Electro-Harmonix Big Muff Pi | common; In-Z **22k** | Known for sustain rather than buzz. gk: 4 transistors, 2 cascaded diode-clip stages, mid-scooped passive tone stack. |
| Jet Fuzz | Roland Jet Phaser (AP-7) | Drive, **Fdbk**, **Tone**, **Speed**, Output | Fuzz followed by a phaser. Fdbk = phaser feedback, Speed = phase rate. |
| Line 6 Drive | Line 6 original, inspired by Colorsound Tone Bender | common | The Mid knob morphs the character: minimum = '70s fuzz clone, ~50% = modern high gain (Rat / Boss Super Distortion), maximum = Tone Bender grit. |
| Line 6 Distortion | Line 6 original | common | "Massive, over the top." |
| Sub Octave Fuzz | Inspired by PAiA Roctave Divider | Drive, Bass, **Sub**, Treble, Output | Fuzz combined with a double-octave-down shift, giving fat square-wave distortion. Sub = amount of sub-octave. gk: flip-flop divider, monophonic. |
| Octave Fuzz | Tycobrahe Octavia | common; In-Z 230k | Output transformer plus two germanium diodes rectify the signal, giving an octave-up fuzz (L6). |

## 3. Modulation (22)

Common parameters:
- **Speed** in Hz or a note value. The M13 screenshots show values such as 0.10–6.33 Hz; the full Hz range is not documented.
- **Depth**, **Fdbk**, and **Mix** (0% dry – 100% wet).
- Script Phase, Pattern Tremolo, AC Flanger and 80A Flanger have **no Mix** (fixed blend, as on the originals).

| Model | Based on | HD500X parameters | St | DSP notes |
|---|---|---|---|---|
| Pattern Tremolo | Inspired by Lightfoot Labs Goatkeeper | Speed, Step 1, Step 2, Step 3, Step 4 | TS | 4-step sequenced tremolo. Each step is 1–16 pulses, MUTE, SKIP, or FULL (M-series doc). |
| Panner | — | Speed, Depth, Shape, VolSens, Mix | ST/S | Shape: min = triangle, 50% = sine, max = square. VolSens: louder input gives a faster rate. Run in mono, it acts as a tremolo. |
| Bias Tremolo | 1960 Vox AC-15 tremolo (varies power-tube bias) | Speed, Level (= depth), Shape, VolSens, Mix | ST/S | Shape runs sine to square. "Deep, 3-D, wide stereo." |
| Opto Tremolo | Optical tremolo of blackface Fender ('64 Deluxe Reverb per Gallery; '65 per M-doc) | Speed, Level (= depth), Shape, VolSens, Mix | ST/M | Light source into a photoresistor. Shape runs smooth to "sci-fi throb". |
| Script Phase | Hand-wired '74 MXR Phase 90 "script" | Speed | TS | Speed only. gk: 4-stage JFET all-pass. |
| Panned Phaser | Ibanez Flying Pan | Speed, Depth, Pan (Left/Center/Right), Pan Spd, Mix | ST/S | 4-stage phaser with a built-in auto-panner. |
| Barberpole Phaser | Modular-synth barberpole phaser | Speed, Fdbk, Mode (Up/Down/Stereo), Mix | TS | Endlessly rising or falling (Shepard-like), not a normal LFO. Stereo mode gives up on one side and down on the other. |
| Dual Phaser | Mu-Tron Bi-Phase | Speed, Depth, Fdbk, LFO Shp (sine→square), Mix | ST/S | "Lush, offset phasing." In-Z 230k. gk: two 6-stage opto phasers. |
| U-Vibe | Uni-Vibe | Speed, Depth, Fdbk, VolSens, Mix | ST/M | "Essentially a four-stage phase shifter." Mix 100% switches to vibrato mode. In-Z 90k. gk: lamp/LDR modulation, unequal stage capacitors. |
| Phaser | Inspired by MXR Phase 90 (4-stage) | Speed, Depth, Fdbk, Stages, Mix | ST/S | Stages is selectable: 4/8/12/16 on the M13. |
| Pitch Vibrato | Boss VB-2 | Speed, Depth, Rise, VolSens, Mix | ST/M | BBD vibrato. Rise ramps the LFO rate up when the effect is engaged; a lower value means a longer rise. |
| Dimension | Roland Dimension D | Sw1, Sw2, Sw3, Sw4, Mix | ST/S | Two delay lines on one oscillator, panned L/R. Four on/off switches (combinations). |
| Analog Chorus | Boss CE-1 Chorus Ensemble | Speed, Depth, Ch Vib (chorus/vibrato), Tone, Mix | ST/S | BBD chorus. In-Z 22k. The MAB guide says the wet signal affects only one side. |
| Tri Chorus | Song Bird / DyTronics Tri-Stereo Chorus | Speed, Depth, Depth2, Depth3, Mix | ST/S | 3 chorus circuits, 12 LFOs, 3 delay lines. |
| Analog Flanger | Inspired by MXR Flanger | Speed, Depth, Fdbk, Manual, Mix | ST/S | BBD flanger, "uniquely shaped waveform". Manual sets the delay time. |
| Jet Flanger | Inspired by A/DA Flanger | Speed, Depth, Fdbk, Manual, Mix | ST/S | 35:1 sweep range and a built-in compressor. More dramatic, with a different wave shape. |
| AC Flanger | MXR Flanger (Reticon BBD re-creation) | Speed, Width, Regen, Manual | ST/S | No Mix. Heard on Van Halen tracks. |
| 80A Flanger | A/DA Flanger (Reticon BBD re-creation) | Speed, Range, Enhance, Manual, Even Odd | Mono | No Mix. "Same knob functions as the classic." |
| Frequency Shifter | Modular-synth frequency shifter | Freq (Hz), Mode (Up/Down/Stereo), Mix | TS | Single-sideband shift (compare with Ring Mod, which gives both sidebands). |
| Ring Modulator | — | Speed (carrier), Depth, Shape (sine→square), AM/FM (blend), Mix | ST/S | |
| Rotary Drum | Fender Vibratone | Speed (Slow/Fast), Depth, Tone, Drive, Mix | ST/S | Rotating styrofoam drum around a 10" speaker. |
| Rotary Drm/Hrn | Leslie 145 | Speed (Slow/Fast), Depth (drum), Horn Dep, Drive, Mix | ST/S | Drum plus horn. Mix 100% gives a "richer Leslie sound". |

## 4. Filter (17, including 5 synths and the Vocoder)

Common parameters: Freq, Q, Speed (Hz or note), Mix. L6 recommends 100% Mix for most filters.

| Model | Based on | HD500X parameters | St | Drive / DSP notes |
|---|---|---|---|---|
| Voice Box | Inspired by vocoders, vocal tracts and surgical tubing (talk box) | Speed, Start, End, Auto, Mix | ST/M | Formant filter morphing from the Start vowel to the End vowel (A/E/I/O/U). Auto 1–4 are transition patterns; morph time is set by Speed. Pedal-controllable. |
| V-Tron | Voice Box combined with Mu-Tron III | Start, End, Speed, Mode (Up / Up-Down), Mix | TS | **Envelope-triggered** vowel sweep on each pick attack. |
| Q Filter | "Parked wah" | Freq, Q, Gain, Type (LP/BP/HP), Mix | TS | Static resonant filter. |
| Vocoder | Not stated (carried over from the POD X3) | Mic, Input, –, Decay, Mix | ? | **Hardware-dependent:** the XLR mic input is the modulator and the guitar is the carrier. Mic = mic level, Input = guitar level, Decay = band release (forum). Band count undocumented. |
| Seeker | Inspired by Z.Vex Seek Wah | Speed, Freq, Q, Steps (2–9), Mix | TS | **Step-sequenced** parked-wah band-pass filters. Freq selects the pattern. |
| Obi Wah | Oberheim voltage-controlled sample-and-hold filter | Speed, Freq, Q, Type (LP/BP/HP), Mix | ST/M | **Random S&H** filter frequency at the Speed rate. |
| Tron Up | Mu-Tron III, switch Up | Freq, Q, Range (Hi/Lo), Type (LP/BP/HP), Mix | TS | **Envelope follower**, upward sweep. |
| Tron Down | Mu-Tron III, switch Down | Freq, Q, Range, Type, Mix | TS | **Envelope follower**, downward sweep. |
| Throbber | Inspired by Electrix Filter Factory (LFO section) | Speed, Freq, Q, Wave (Ramp Up / Ramp Down / Triangle / Square), Mix | TS | **LFO** filter. |
| Slow Filter | — | Freq, Q, Speed, Mode (Up/Down), Mix | TS | **Attack-triggered** low-pass sweep: dark→bright (Up) or bright→dark (Down). Q adds a resonant peak. |
| Spin Cycle | Inspired by Craig Anderton's Wah/Anti-Wah | Speed, Freq, Q, VolSens, Mix | TS | Two wahs panned L/R sweeping in opposite directions. **LFO-driven with volume sensitivity.** |
| Comet Trails | Line 6 original | Speed, Freq, Q, Gain, Mix | TS | 7 filters "chasing each other" (**LFO**). |
| Octisynth | Line 6 original ("eight-armed denizens") | Speed, Freq, Q, Depth, Mix | ST/M | Synth. Velocity-sensitive ring mod combined with a VCO and vibrato. The guitar volume sets the oscillator frequency. Freq adds 2nd-order harmonics; Speed/Depth control the vibrato. |
| Synth O Matic | Waveforms from vintage analog synths (Moog Modular, Oberheim Synth Expander Module) | Freq, Q, Wave (1–8), Pitch, Mix | ST/M | Synth. Pitch-tracked oscillator into a filter. gk: monophonic tracking. |
| Attack Synth | Korg X911 guitar synth | Freq, Wave (Square/PWM/Ramp), Speed (= attack), Pitch, Mix | ST/M | Synth. Freq = VCF stop frequency. Pitch covers a 2-octave range. |
| Synth String | Roland GR-700 guitar synth | Speed, Freq, Attack, Pitch, Mix | ST/M | Synth. Freq = low-pass tone. Speed = PWM "vibrato" rate. Pitch covers a 2-octave range. |
| Growler | GR-700 tone combined with Mu-Tron III | Speed, Freq, Q, Pitch, Mix | ST/M | Synth with pitch control and PWM (Speed). Pitch covers a 2-octave range. |

## 5. Pitch (3)

| Model | Based on | Parameters | St | Notes |
|---|---|---|---|---|
| Bass Octaver | Inspired by EBS OctaBass | Tone, Normal (dry level), Octave (octave-down level) | ST/M | Clean octave down. Monophonic tracking. |
| Pitch Glide | Inspired by Digitech Whammy | Pitch, Mix | ST/M | Designed for the EXP pedal. Range ±24 semitones (2 octaves) with sub-semitone steps (secondary sources: forums/tutorial). Polyphony not documented. gk: Whammy-style chord-tolerant shifter. |
| Smart Harmony | Inspired by Eventide H3000 | Key, Shift (interval), Scale, Mix | ST/M | Diatonic intelligent harmony. **Monophonic:** "detects your guitar's single-note pitch". Modes other than major are reached by choosing a different Key with Major (the guide includes a table). |

## 6. Preamp + EQ (6)

| Model | Based on | Parameters | St | Notes |
|---|---|---|---|---|
| Graphic EQ | Inspired by MXR 10-band graphic EQ | 80Hz, 220Hz, 480Hz, 1.1kHz, 2.2kHz | TS | ±12 dB per band (M-doc). The third band is labelled **480 Hz** on the HD500X but **440 Hz** in the M-series doc. |
| Parametric EQ | — | Lows (low shelf), Highs (high shelf), Freq, Q, Gain | TS | High shelf, low shelf, and one fully parametric band. |
| Studio EQ | Inspired by API 550B | Low Freq, Low Gain, Hi Freq, Hi Gain, Gain | TS | Constant-Q. Output Gain has **soft clipping**. M13 defaults are 500 Hz and 1500 Hz. |
| 4 Band Shift EQ | — | Low, Low Mid, Hi Mid, Hi, Shift | TS | Shift moves the low band lower and the high bands higher. Above 50% suits guitar; below 50% suits bass. |
| Mid Focus EQ | — | Hi Pass Freq, Hi Pass Q, Low Pass Freq, Low Pass Q, Gain | TS | HPF plus LPF forming a band-pass, with make-up gain. |
| Vintage Pre | Requisite Y7 vintage tube mic preamp | Gain, Output, Phase (0/180), Hi Pass Filter (Hz), Lo Pass Filter (kHz) | Mono (L6: "mono tube mic preamp") | HD-only (not in M-series). High Gain adds tube distortion. |

## 7. Delay (19)

Common parameters:
- **Time** in ms, **max 2000 ms**. Turning past 2000 switches to note values; this is sourced from forums, M13 screens, and the Gallery's "Reverse up to 2 s".
- Fdbk and Mix are shared by all delays.
- The "…Dry" models are identical to the base model, but at Mix 0% the dry path is flat. The base models reproduce the original unit's dry-path colouration. Other docs call these "DryThru" (M-series) or "Studio" (Gallery).

| Model | Based on | HD500X parameters | St | Notes |
|---|---|---|---|---|
| Ping Pong | — | Time, Fdbk, Offset, Spread, Mix | ST/S | Two delays cross-feeding L↔R. Offset = R time as a % of L time. Spread runs mono→wide. Lowest DSP cost (MAB). |
| Dynamic Dly | TC Electronic 2290 | Time, Fdbk, Thresh, Ducking, Mix | TS | Echoes are ducked while you play. Thresh = level above which ducking releases. Ducking = amount. |
| Stereo Delay | Line 6 high-res digital | L Time, L Fdbk, R Time, R Fdbk, Mix | TS | Independent L/R times, each with its own Tempo Sync. |
| Digital Delay | Line 6 original | Time, Fdbk, Bass, Treble, Mix | TS | 32-bit float, true stereo, clean. |
| Dig Dly W/Mod | Line 6 | Time, Fdbk, ModSpd, Depth, Mix | TS | Chorus on the repeats only. |
| Reverse | Line 6 | Time (≤ 2 s), Fdbk, ModSpd, Depth, Mix | ST/M | Reversed chunks of length Time. |
| Lo Res Delay | Line 6 | Time, Fdbk, Tone, Res, Mix | TS | Res = bit depth, **6–24 bits**. Tone affects the repeats only. |
| Tube Echo | '63 Maestro EP-1 Echoplex | Time, Fdbk, Wow/Flt, Drive, Mix | TS | Tape loop with a tube preamp. Drive = tube distortion plus tape saturation. Coloured dry path. |
| Tube Echo Dry | same | same | TS | Flat dry path. |
| Tape Echo | Maestro EP-3 (solid state) | Time, Fdbk, Bass, Treble, Mix | TS | Less distorted than Tube Echo, with tone controls. |
| Tape Echo Dry | same | same | TS | |
| Sweep Echo | Line 6: EP-1 tone plus a sweeping filter on the repeats | Time, Fdbk, Swp Spd, Swp Dep, Mix | TS | |
| Sweep Echo Dry | same | same | TS | |
| Echo Platter | Binson Echorec (magnetic platter) | Time, Fdbk, Wow/Flt, Drive, Mix | TS | Coloured dry path. |
| Echo Platter Dry | same | same | TS | |
| Analog W/Mod | Electro-Harmonix Deluxe Memory Man | Time, Fdbk, ModSpd, Depth, Mix | TS | BBD with chorus on the echoes only. In-Z 90k. |
| Analog Echo | Boss DM-2 | Time, Fdbk, Bass, Treble, Mix | TS | Warm, distorted BBD. In-Z 230k. |
| Auto-Volume Echo | Line 6 | Time, Fdbk, ModDep, Swell, Mix | TS | Auto-swell (attack ramp time = Swell) plus an echo with tape-style wow/flutter (ModDep). |
| Multi-Head | Roland RE-101 Space Echo | Time, Fdbk, Heads 1-2, Heads 3-4, Mix | ST/M | Head on/off combinations for multi-tap. In-Z 22k. |

## 8. Reverb (12)

Common parameters: Decay, PreDelay (20–200 ms per M-series doc), Tone (brightness of the wet signal), Mix. The M-series says "all Reverbs are stereo" (MAB class ST/S).

| Model | Based on / description | Notes |
|---|---|---|
| Plate | Studio plate reverb | Metallic, dense. |
| Room | Classic studio echo-chamber room | Mostly early reflections. |
| Chamber | Elongated space (hallway, stairwell, elevator shaft) | Long decay. |
| Hall | Concert hall / large space | Strong tail. MAB says "mono wet, stereo dry". |
| Echo | Line 6 original | Lush echo plus reverb with distinct repeats. |
| Tile | Tiled room (bathroom, shower) | Bright, discrete early reflections. |
| Cave | Line 6 original | Cavernous echo chamber. |
| Ducking | Hall with ducking | Reverb level drops while you play. |
| Octo | — | Harmonized (octave) dense decay; density follows the Decay knob. Good with swells. |
| Spring | Studio spring reverb | |
| '63 Spring | 1963 "brown" self-contained spring reverb unit | Surf. gk: Fender 6G15 tube reverb. |
| Particle Verb | Line 6 original | Parameters: Dwell (decay), Condition (Stable / Critical / Hazard), Gain, Mix. Stable = lush modulated pad. Critical = pad with a slight pitch rise. Hazard = "all stops removed". |

## 9. Wah (8)

All wahs have Position and Mix. Position is auto-assigned to EXP 1 and can be parked. MAB class TS.

| Model | Based on (Gallery / M-doc) | Notes |
|---|---|---|
| Fassel | Cry Baby Super / Jen "Super Cry Baby" with Fasel inductor | |
| Conductor | Maestro Boomerang | "wow-wow" |
| Throaty | RMC Real McCoy 1 (Gallery) / "RMC Real McCoy Custom" (M-doc) | Clone of the Vox Clyde McCoy "picture" wah. |
| Colorful | Colorsound Wah-Fuzz(-Swell), wah section only | **Inductor-less** resonant circuit. |
| Vetta Wah | Line 6 original (Vetta II) | |
| Chrome | Vox V847 | |
| Chrome Custom | Modded V847 | Retuned first-stage gain, aftermarket inductor, wider Q, 470k pot. |
| Weeper | Arbiter Cry Baby (Gallery) / Dunlop GCB-95 Cry Baby (M-doc) | In-Z 90k. |

## 10. Volume/Pan (2)

| Model | Parameter | Notes |
|---|---|---|
| Volume Pedal | Volume (100% = unity) | Auto-assigned to EXP 2. Stereo. |
| Pan | Pan (0% = L, 50% = C, 100% = R) | Auto-assigned to EXP 2. A mono model placed later re-monos the path. |

## 11. Hardware / system blocks (no DSP model to port, or hardware-only)

- **FX Loop** (inserted as an FX block):
  - Parameters: Send, Return, Mix. Mix 100% = the full signal goes out the Send; 0% = the loop is bypassed.
  - Stereo returns. A rear LINE/STOMP switch sets the level. Honours Trails.
  - **Hardware-only.**
- **Looper:**
  - 24 s at normal speed or 48 s at ½ speed. Mono.
  - Controls: Rec/Overdub, Play/Stop, Play Once, Undo, Pre/Post position, ½ Speed (an octave down), Reverse.
  - Settings: Playback level, Overdub level (decay per pass), Hi Cut, Lo Cut.
  - Hardware and footswitch feature.
- **Vocoder mic input:** XLR, no phantom power. Needed for the Vocoder.
- **Global EQ** (added in v2.62; not on the L6 LINK output): Low Cut, Low, Mid, High (3 fully parametric bands), High Cut.
- **Output modes:**
  - **Studio/Direct:** "Studio" cabs plus mic plus an "AIR" convolution.
  - **Combo Front / Stack Front:** "Live" cabs, no mic, plus Lows / Focus / Highs.
  - **Combo Power Amp / Stack Power Amp.**
  - The cab Res Level, Thump and Decay controls only act in Studio/Direct.
- **Guitar In-Z:** Auto, 22k, 32k, 70k, 90k, 136k, 230k, 1M, 3.5M. This is analog hardware; to emulate it, apply pickup loading.
- **AC Frequency:** 50/60 Hz global setting for the amp hum simulation.
- **DT50/DT25 settings (L6 LINK only):** Class A/AB, Topology I–IV, Triode/Pentode per amp.
- **Tuner.**

## 12. Amps — stock (30 full models plus a "Pre" preamp-only version of each = 60 selections), all mono

The **"Pre"** versions exist for every stock amp. They have no power-amp emulation, and the amp deep-edit page is hidden.

Front-panel **tone knobs**: DRIVE, BASS, MID, TREBLE, PRES (Presence), VOL (Channel Volume). When the amp is bypassed, VOL becomes the bypass volume.

**Power-amp deep edit** (full models only, 0–100%):
- **Master:** power-amp distortion; interacts with everything. At 100% the model matches the original non-master amp.
- **Sag:** low = tight; high = touch dynamics and sustain.
- **Hum:** heater hum *and* AC ripple combined into **one** knob. **There is no separate "Ripple" on the HD.**
- **Bias:** minimum = cold class AB; maximum = class A.
- **Bias X (bias excursion):** low = tight; high = more tube compression. Reacts strongly with Drive and Master.

**Cab/mic deep edit** (amps and preamps):
- E.R. (early reflections / room).
- Low Cut (HPF, Hz).
- Mic select.
- Res Level (speaker resonance level; raises overall level).
- Thump (low-frequency resonance).
- Decay (resonance decay: tight to loose cone).
- Thump and Decay do nothing when Res Level is at minimum.

**Knob remaps:**
- **Super O:** MID = the Supro's single "Tone" knob. Bass, Treble and Presence were invented by Line 6.
- **Divide 9/15:** DRIVE = clean-channel level, BASS = dirty-channel drive, MID = Tone, TREBLE = Cut.
- **Class A-15 and Class A-30 TB:** MID acts as **Cut** (counter-clockwise cuts highs).

| # | Model (guide name; device abbreviations such as "Nrm/Brt/Vib") | Based on (Advanced Guide; Gallery/KB in brackets) | Voicing / family (gk = general circuit knowledge) |
|---|---|---|---|
| 1 | Blackface Double Normal | '65 "Blackface" Fender Twin Reverb, Normal ch. | Clean. Fender blackface TMB stack, 4×6L6 ~100 W, very high headroom. |
| 2 | Blackface Double Vibrato | '65 Fender Twin Reverb, Vibrato ch. | Extra 12AX7 stage gives different clipping. |
| 3 | Hiway 100 | Hiwatt Custom 100 ['73 Hiwatt DR-103] | Clean to crunch. 4×EL34, huge headroom, very linear. |
| 4 | Super O | '60s Supro S6616 | Crunch. Single-ended 6V6, one Tone control, 6×9 oval speaker. |
| 5 | Gibtone 185 | Gibson EH-185 [1939] | Clean/edge. Octal preamp, 2×6L6, 12" field-coil speaker. |
| 6 | Tweed B-Man Normal | '59 Fender Tweed Bassman, Normal ch. [KB/Gallery: 1958; 5F6-A] | Crunch. Cathode-follower TMB stack, 5AR4 rectifier, no master volume. |
| 7 | Tweed B-Man Bright | '59 Bassman, Bright ch. | Brighter: bright cap across the volume and the other half of the first tube. |
| 8 | Blackface 'Lux Normal | Fender "Blackface" Deluxe Reverb, Normal ch. [1964] | Clean/edge. 2×6V6GT class AB 22 W, Oxford 12K5-6 speaker. |
| 9 | Blackface 'Lux Vibrato | Deluxe Reverb, Vibrato ch. | Extra 12AX7 stage plus bright cap. |
| 10 | Divide 9/15 | Divided by 13 JRT 9/15 | Boutique. Two blendable 5879-pentode channels; power amp 2×6V6 class A 9 W or 2×EL84 AB 15 W. |
| 11 | PhD Motorway | Dr. Z Route 66 | Boutique crunch. EF86 pentode preamp, 2×KT66, ultra-linear output transformer. |
| 12 | Class A-15 | '61 "Fawn" Vox AC-15 [Gallery: 1960] | Chime/crunch. EF86 channel, 2×EL84 cathode-biased with no negative feedback, EZ81 rectifier. Vox Cut. |
| 13 | Class A-30 TB | Vox AC-30 "Top Boost" [Gallery: 1967] | Chime/crunch. 4×EL84 ~36 W, Top Boost tone circuit (Treble/Bass plus Cut). |
| 14 | Brit J-45 Normal | '65 Marshall JTM-45 MkII, Normal ch. | Crunch. Marshall TMB stack (Bassman-derived), KT66. |
| 15 | Brit J-45 Bright | JTM-45, Bright ch. | Brighter via an inter-stage high-shelf. |
| 16 | Plexi Lead 100 Normal | '59 Marshall "Plexi" Super Lead 100 (model 1959), Normal ch. | Crunch. 4×EL34, Marshall TMB. |
| 17 | Plexi Lead 100 Bright | Super Lead 100, Bright ch. | Brighter. |
| 18 | Brit P-75 Normal | Park 75, Normal ch. [KB: '71] | Crunch/hot plexi. KT88 output, more front-end gain. |
| 19 | Brit P-75 Bright | Park 75, Bright ch. | Brighter via an inter-stage high-shelf. |
| 20 | Brit J-800 | Marshall JCM-800 [Gallery: 1982, 2204 50 W] | Hi-gain '80s. Master volume, 2×EL34. |
| 21 | Bomber Uber | 2002 Bogner Uberschall | High gain. 4×12AX7 of preamp gain, 4×EL34, heavy low end. |
| 22 | Treadplate | Mesa/Boogie Dual Rectifier [Gallery "Cali Tread": 2001 Dual Rectifier Solo] | High gain. 6L6, Mesa stack, lots of bass. |
| 23 | Angel F-Ball | Engl Fireball 100 [2009] | High gain. 4×6L6, 4×12AX7. |
| 24 | Line 6 Elektrik | Line 6 original | High gain. Interactive Presence and Mid. |
| 25 | Solo 100 Clean | '93 Soldano SLO-100, Normal ch., Clean | Clean. |
| 26 | Solo 100 Crunch | SLO-100, Normal ch., Crunch | Crunch. |
| 27 | Solo 100 OD | SLO-100, Overdrive ch. | High gain. |
| 28 | Line 6 Doom | Line 6: modded JCM800 preamp into a Hiwatt power amp | Doom/sludge. Sag-heavy, lots of bass. |
| 29 | Line 6 Epic | Line 6 original | High gain, "sustain at any playing level". |
| 30 | Flip Top | Ampeg B-15NF Portaflex (bass) | Bass. 30 W, 1×15 CTS; loads the 115 Flip Top cab and the bass mics. |

Also available: "Amp Disabled" (null block).

History: Solo 100 ×3, Doom and Epic were added in v2.1, bringing the total to "30 HD amps". The KB FAQ counts "22 full HD guitar amp models", counting base amps rather than channels.

## 13. Amps — optional HD Model Packs (v2.60+)

The packs were sold separately. Since 2019, new HD500X units registered with Line 6 get the "HD Fully Loaded Bundle" of all three packs free.

Device names are from the KB list and forums. Pre versions exist for at least PV Panama ("PV Panama Pre" appears in CustomTone preset metadata); the others are assumed.

| Pack | Device name | Based on |
|---|---|---|
| Metal | PV Panama | Peavey 5150 (block logo) |
| Metal | Mahadeva | Bogner Shiva |
| Metal | Brit 2204 | "Remastered" Marshall JCM800 (model 2204) |
| Metal | Line 6 Insane | Line 6 original (high gain) |
| Metal | Line 6 Big Bottom | Line 6 original (bass-heavy high gain) |
| Metal | Line 6 Variac'ed Plexi | Line 6 original (variac-sagged Plexi idea) |
| Metal | Line 6 Purge | Line 6 original |
| Metal | Line 6 Aggro | Line 6 original |
| Metal | Line 6 Smash | Line 6 original |
| Metal | Line 6 Octone | Line 6 original |
| Vintage | Jazz Rivet | Roland JC-120 (amp only, no built-in chorus) |
| Vintage | Small Tweed | Fender Champ (tweed) |
| Vintage | Mandarin 80 | Orange OR80 |
| Vintage | A30 Fawn Nrm | Vox AC30 "Fawn", Normal ch. |
| Vintage | A30 Fawn Brt | Vox AC30 "Fawn", Bright ch. |
| Vintage | Black Panel Pete | Pete Anderson custom amp |
| Vintage | Line 6 Acoustic | Line 6 original (acoustic simulation). A reported bug: it shows in HD Edit but not on the device. |
| Bass | SVT Nrm | Ampeg SVT, Normal ch. |
| Bass | SVT Brt | Ampeg SVT, Bright ch. |
| Bass | G Cougar 800 | Gallien-Krueger 800RB |

## 14. Cabinets (stock: 17 plus "No Cab"; packs: +4)

"No Cab" bypasses the speaker, mic and early-reflection processing.

| Cab | Based on | Speakers |
|---|---|---|
| 212 Blackface Double | Fender Blackface Twin Reverb combo | 2×12 Jensen |
| 412 Hiway | Hiwatt cabinet | 4×12 Fane 12287 (50 W) |
| 6x9 Super O | Supro S6616 combo | 1× 6"×9" oval |
| 112 Field Coil | Gibson EH-185 combo | 1×12 field-coil |
| 410 Tweed | '59 Fender Tweed Bassman combo | 4×10 Jensen alnico |
| 112 BF 'Lux | Fender Blackface Deluxe Reverb combo | 1×12 Oxford 12K5-6 |
| 112 Celest 12-H | Divided by 13 9/15 combo | 1×12 Celestion G12H Heritage (70th anniversary) |
| 212 PhD Ported | Dr. Z "Z Best" cabinet (ported) | 2×12: 1× G12H Heritage plus 1× Vintage 30 |
| 112 Blue Bell | '61 "Fawn" Vox AC-15 combo | 1×12 Celestion Alnico Blue |
| 212 Silver Bell | Vox AC-30 Top Boost | 2×12 Celestion Alnico Silver Bell |
| 412 Greenback 25 | Marshall cabinet | 4×12 Celestion G12M "Greenback" (25 W) |
| 412 Blackback 30 | Marshall cabinet | 4×12 Celestion Rola G12H30 "Blackback" |
| 412 Brit T-75 | Marshall cabinet | 4×12 Celestion G12T-75 |
| 412 Uber | Bogner Uberschall cabinet | 4×12: 2× G12T-75 plus 2× Vintage 30 |
| 412 Tread V-30 | Mesa/Boogie cabinet | 4×12 Celestion Vintage 30 |
| 412 XXL V-30 | Engl Pro cabinet | 4×12 Celestion Vintage 30 |
| 115 Flip Top (bass) | Ampeg B-15 "Custom Design" | 1×15 CTS |
| *Vintage Pack* | Roland JC-120 cab (device name unconfirmed) | 2×12 |
| *Vintage Pack* | Fender Champ cab (device name unconfirmed) | 1×8 |
| *Bass Pack:* 810 SV Beast | Ampeg SVT 8×10 cab | 8×10 |
| *Bass Pack:* 410 Rhino | Ampeg SVT 410HLF cab | 4×10 (plus horn in the original) |

## 15. Microphones (8 guitar plus 8 bass = 16 entries, 14 distinct types)

Any guitar cab offers the 8 guitar mics. The 115 Flip Top cab offers the 8 bass mics. The bass-pack cabs were used with bass mics in forum reports; this is not officially documented.

| Guitar-cab mics | Based on | Bass-cab mics | Based on |
|---|---|---|---|
| 57 On Xs | Shure SM57, on axis | 57 On Xs | Shure SM57, on axis |
| 57 Off Xs | Shure SM57, off axis | 421 Dyn | Sennheiser MD 421 |
| 409 Dyn | Sennheiser MD 409 | 12 Dyn | AKG D12 |
| 421 Dyn | Sennheiser MD 421 | 112 Dyn | AKG D112 |
| 4038 Rbn | Coles 4038 ribbon | 20 Dyn | Electro-Voice RE20 |
| 121 Rbn | Royer R-121 ribbon | 7 Dyn | Shure SM7B |
| 67 Cond | Neumann U67 | 40 Dyn | Heil PR40 |
| 87 Cond | Neumann U87 | 47 Cond | Neumann U47 |

## 16. Bass-related content (summary)

| Type | Stock | Model packs |
|---|---|---|
| Amps | Flip Top (Ampeg B-15NF) and Flip Top Pre | Bass Pack: SVT Nrm, SVT Brt, G Cougar 800 |
| Cabs | 115 Flip Top | 810 SV Beast, 410 Rhino |
| Mics | 8 bass mics (§15) | — |
| Bass-friendly FX | Bass Octaver, Sub Octave Fuzz, 4 Band Shift EQ (Shift below 50%), Vintage Pre | — |

## 17. Totals

| Category | Count |
|---|---|
| Dynamics | 9 |
| Distortion | 15 |
| Modulation | 22 |
| Filter | 17 (incl. 5 synths and the Vocoder) |
| Pitch | 3 |
| Preamp+EQ | 6 (5 EQ plus Vintage Pre) |
| Delay | 19 |
| Reverb | 12 |
| Wah | 8 |
| Volume/Pan | 2 |
| **FX models total** | **113**, plus the FX Loop block (hardware) and the Looper (hardware) |
| Amps, stock | 30 full plus 30 Pre (29 guitar plus 1 bass) |
| Amps, model packs | 20 (Metal 10, Vintage 7, Bass 3), with Pre versions likely |
| Cabs | 17 stock (16 guitar plus 1 bass) plus No Cab; +4 from packs = 21 |
| Mics | 16 entries (8 guitar plus 8 bass; 14 distinct) |

## 18. Unconfirmed items and conflicts

1. **Dynamics count:** the Line 6 KB FAQ says "Dynamics: 8". The HD500X guide Rev A, the HD500 v2.10 guide and the preset IDs list 9. Hard Gate is missing from the older HD500 Rev F guide, so the FAQ count is probably stale.
2. **Stereo Delay vs Dynamic Dly parameters:** the HD500X guide Rev A table gives Stereo Delay "Time/Fdbk/Thresh/Ducking" and Dynamic Dly "L Time/L Fdbk/R Time/R Fdbk". This is swapped relative to the M-series doc, the Gallery descriptions and the HD500X Edit guide (Stereo Delay has 2 Tempo Syncs). Treat Rev A as an erratum.
3. **Per-model stereo class** comes from one fan source (the MeAmBobbo wiki, M-series based). Conflicts:
   - The MAB guide calls Hall "mono wet / stereo dry" and says Analog Chorus affects only one side.
   - A third-party copy of the HD500 manual says EQs and Wahs are mono. The HD500X Rev A and HD500 Rev F guides say they are stereo.
   - The Vocoder's status is undocumented. Hard Gate and Vintage Pre are not in the MAB table; mono is assumed.
4. **Graphic EQ third band:** 480 Hz (HD500X guide) vs 440 Hz (M-series doc).
5. **Wah "based on" conflicts:**
   - Weeper: Arbiter Cry Baby (Gallery) vs Dunlop GCB-95 (M-doc).
   - Throaty: RMC Real McCoy 1 vs Real McCoy Custom.
   - Fassel: Cry Baby Super vs Jen Super Cry Baby.
6. **Amp year conflicts:**
   - Tweed B-Man: '59 (guide) vs 1958 (Gallery, KB).
   - Class A-15: '61 (guide, KB) vs 1960 (Gallery).
   - Opto Tremolo: '64 (Gallery) vs '65 Deluxe Reverb (M-doc).
7. **Amp name conflicts:**
   - Treadplate is called "Cali Tread" in the Gallery.
   - Hiway 100 is called "Hiway 100 Custom" in the Gallery.
   - Device abbreviations differ from the guide names (e.g., "Blackface Dbl Nrm", "Solo-100 Overdrive").
8. **Ranges not documented in any source found:**
   - Modulation/filter Speed Hz range and the tempo-sync note list.
   - EQ frequency and gain ranges (except Graphic ±12 dB).
   - Cab Low Cut range.
   - Pitch Glide ±24 semitones comes only from a secondary source (forum/tutorial).
   - Delay max 2000 ms is from forum and M13 evidence, not the HD guide.
9. **Model packs:**
   - Exact device names of the two Vintage Pack cabs were not found.
   - Pre versions are confirmed only for PV Panama.
   - The Line 6 originals (Insane, Big Bottom, Variac'ed Plexi, Purge, Aggro, Smash, Octone, Acoustic) have no official voicing descriptions for the HD. They derive from older POD XT/X3 "Metal Shop"-era models.
   - The Line 6 shop pages that listed the packs are offline.
10. **Vocoder** parameter meanings (Mic, Input, Decay) come from forum posts, not a Line 6 document. The band count is unknown.
11. Circuit details marked **gk:** (clipping topology, tube complements not quoted above, Bi-Phase stage count, etc.) are general knowledge about the original hardware. They are not Line 6 statements about how the HD models were built.

## Sources

- POD HD500X Advanced Guide Rev A: https://line6.com/data/6/0a06434c6fb051e03e8ab63dc/application/pdf/POD%20HD500X%20Advanced%20Guide%20-%20English%20(%20Rev%20A%20).pdf
- POD HD500 Advanced Guide v2.10: https://line6.com/data/6/0a06434dc1a55085c5d6532bf/application/pdf/POD%20HD500%20Advanced%20Guide%20v2.10%20-%20English%20(%20Rev%20A%20).pdf
- POD HD500 Advanced Guide Rev F: https://line6.com/data/6/0a060b316ac34f05939099360/application/pdf/POD%20HD500%20Advanced%20Guide%20-%20English%20(%20Rev%20F%20).pdf
- POD HD Series Model Gallery Rev E: https://line6.com/data/6/0a06434c883751e6fd5654eee/application/pdf/POD%20HD%20Series%20Model%20Gallery%20-%20English%20(%20Rev%20E%20).pdf
- M5/M9/M13 FX Parameters Rev B: https://line6.com/data/6/0a060b316ac34f0593fabe278/application/pdf/M13/M9/M5%20FX%20Parameters%20-%20English%20(%20Rev%20B%20).pdf
- M13 v2.0 Advanced Guide: https://line6.com/data/l/0a0600729b4f4af8b8d2e9557/application/pdf/M13_Advanced_Users_Guide.pdf
- POD HD500X Edit Pilot's Guide Rev B: https://l6c-acdn2.line6.net/data/6/0a06434c11f852169d6dcd6b2/application/pdf/POD%20HD500X%20Edit%20Pilot's%20Guide%20(%20Rev%20B%20).pdf
- KB, all HD amp models incl. packs: https://kb.line6.com/all-hd-amp-models-in-order-including-hd-model-packs
- KB, POD HD FAQ: https://kb.line6.com/pod-hd500x-hd500-pod-hd-pod-hd-pro-faq
- KB, v2.62 release notes: https://kb.line6.com/pod-hd500-hd-desktop-hd-pro-v2-62-release-notes
- KB, installing model packs: https://kb.line6.com/installing-hd-model-packs-on-hd500x-500-hd-hd-pro-hd-pro-x
- HD Fully Loaded promo (2019): https://l6c-acdn2.line6.net/data/6/0a020a3f163135da4e95e1ebd8/application/pdf/POD%20HD%20Complete
- Pack contents: https://brianmhall.wordpress.com/2015/04/02/line-6-has-released-3-new-model-packs-for-the-hd-series/
- Pack contents: https://line6.com/support/topic/12862-hd-model-packs-list-not-all-new-amps-are-there/
- Bass pack device names: https://line6.com/support/topic/56347-hd-bass-model-pack/
- v2.1 news: https://www.kvraudio.com/news/line-6-releases-v2-1-firmware-update-for-pod-hd500-pod-hd-pro-and-pod-hd-multi-effects-20287
- Preset effect IDs: https://github.com/johanneszab/podhd-java (docs/reverse_podhdeffects.txt)
- MeAmBobbo effects wiki: https://foobazaar.com/wiki/index.php?title=Line_6_Pod_HD_Effects
- MeAmBobbo amp guide: https://foobazaar.com/podhd/toneGuide/ampTone
- MeAmBobbo cab/mic guide: https://foobazaar.com/podhd/toneGuide/cabsMics
- Helix-to-HD500 amp map: https://medias.audiofanzine.com/files/helix-to-hd500-models-479031.pdf
- Vocoder forum thread: https://line6.com/support/topic/3475-using-the-vocoder-effect-on-pod-x3-pro/
- Pitch Glide tutorial: https://line6podhditalia.wordpress.com/2017/10/05/line-6-pod-hd-tutorial-pitch-glide-per-whammy-detuner/
