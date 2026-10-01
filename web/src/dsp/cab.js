// The speaker cabinet and its microphone (Source/DSP/fx/Cab.h): the HD500X's 17 cabinets, the 4 model-pack
// cabinets and this project's original voicing ("412 Classic"), each through one of 14 microphones. Mono.
//
// What it models:  Low Cut (12 dB/octave high-pass, off at 20 Hz)  ->  the speaker's bass resonance (Res Level /
//                  Thump / Decay)  ->  the cabinet's biquads  ->  the comb of a second speaker and of the back wave
//                  ->  the microphone's biquads  ->  E.R. (six early reflections)  ->  level
//
// The order the filters run in is a different one, which gives the same response (they are all linear): every
// biquad below 500 Hz first (Low Cut, resonance, the cabinet's low stages, the microphone's low stages), then
// the rest. Float rounding only matters in those low biquads (a 40 Hz high-pass at 48 kHz has its poles a hair
// from z = 1): the C++ keeps its samples and filter states in floats, and in doubles these biquads would come
// out 70 to 90 dB below the signal apart from it. So that first part imitates float arithmetic exactly here
// (runLow: every operation rounded with f32, on input that is still bit-identical to the C++'s), and the rest
// runs in plain doubles (runHigh), five times faster. Nothing here allocates after prepare().
import { PI, clamp, f32, Smoothed, Biquad, OnePole } from "./core.js";
import { CATEGORY, ENGINE, model, percent, freq, choice } from "./modeltypes.js";

/** The Mic knob: the HD500X's 8 guitar-cab microphones, then the bass-cab microphones that are not among them. */
const MIC_NAMES = ["57 On Xs", "57 Off Xs", "409 Dyn", "421 Dyn", "4038 Rbn", "121 Rbn", "67 Cond", "87 Cond",
                   "12 Dyn", "112 Dyn", "20 Dyn", "7 Dyn", "40 Dyn", "47 Cond"];

const NUM_CABS = 22, NUM_MICS = 14;
const MAX_CAB_STAGES = 8, MAX_BANK_STAGES = 8;
const CHUNK = 32;                 // filter coefficients follow a moving knob in steps of this many samples
const NUM_ROOM_TAPS = 6;
const MAX_RESONANCE_DB = 16;
const LOW_STAGE_HZ = 500;         // biquads below this frequency run first, in float arithmetic
const CLASSIC = 21;               // the variant of "412 Classic"

// stage types: [type, hz, q, dB] is one biquad of a cabinet's or a microphone's response
const HP = 1, LP = 2, PK = 3, LS = 4, HS = 5;

// A cabinet, built from what is known about the speaker and the box (there are no measured responses in here):
// - resHz / resQ / resDb: the bass resonance of the speaker in its box, as a peak the Res Level, Thump and
//   Decay knobs scale (the values here are what 50 % / 50 % / 50 % gives)
// - tap1: a second speaker reaching the microphone late (comb ripple); tap2: the back wave, coming out of an
//   open back in opposite polarity or bouncing off the back panel of a closed box. Both only below tapLowPassHz.
// - stages: the box's low roll-off, the body and box dips, the cone break-up peaks with the dips around them and
//   the steep roll-off above the speaker's range (two 12 dB/octave low-passes)
// - trimDb: brings every cabinet to the same loudness at its default knobs
const CAB_DEFS = [
  // 212 Blackface Double: Fender Twin Reverb, open back, 2 x Jensen C12N. Loose, not very deep bass (open back),
  // a big combo's thump at 130 Hz, mids scooped at 480 Hz, a glassy break-up peak at 3.3 kHz, the most
  // extended top of the Fenders.
  { resHz: 95, resQ: 1.6, resDb: 2.5, tap1Ms: 1.1, tap1Gain: 0.16, tap2Ms: 1.6, tap2Gain: -0.2, tapLowPassHz: 1300, trimDb: -1.1,
    stages: [[HP, 80, 0.7, 0], [PK, 130, 1, 1.5], [PK, 480, 0.9, -2.5], [PK, 1300, 1.2, 1.5],
             [PK, 3300, 1.3, 4.5], [LP, 5300, 0.7, 0], [LP, 6500, 0.9, 0]] },

  // 412 Hiway: Hiwatt 4x12, closed back, Fane 12287. Tight and even: no box dip, full mids around 900 Hz,
  // a modest break-up peak and a clear top that reaches further than a Celestion's.
  { resHz: 105, resQ: 1.3, resDb: 2.5, tap1Ms: 0.9, tap1Gain: 0.14, tap2Ms: 1.8, tap2Gain: 0.1, tapLowPassHz: 1200, trimDb: -1.8,
    stages: [[HP, 78, 0.8, 0], [PK, 900, 0.8, 2.5], [PK, 2700, 1.2, 2], [PK, 4500, 1.8, 2],
             [LP, 5600, 0.65, 0], [LP, 6700, 0.9, 0]] },

  // 6x9 Super O: Supro S6616, one small oval speaker in a little open box. No bass to speak of (resonance at
  // 140 Hz), a nasal honk around 1 kHz and an early roll-off.
  { resHz: 140, resQ: 2, resDb: 3.5, tap1Ms: 1, tap1Gain: 0, tap2Ms: 1.1, tap2Gain: -0.25, tapLowPassHz: 2000, trimDb: -0.3,
    stages: [[HP, 125, 0.8, 0], [PK, 330, 1.2, -2], [PK, 950, 1.1, 4], [PK, 1700, 2.5, -1.5],
             [PK, 2400, 1.6, 3], [LP, 3300, 0.7, 0], [LP, 4000, 1, 0]] },

  // 112 Field Coil: Gibson EH-185, a 1939 field-coil 12" in an open-back combo. Mid-heavy and dark: a broad
  // 750 Hz hump, a low break-up peak at 2 kHz and the earliest, softest roll-off of the 12" speakers.
  { resHz: 92, resQ: 1.8, resDb: 3, tap1Ms: 1, tap1Gain: 0, tap2Ms: 1.7, tap2Gain: -0.22, tapLowPassHz: 1500, trimDb: -1.8,
    stages: [[HP, 85, 0.7, 0], [PK, 240, 0.9, 1.5], [PK, 750, 0.8, 3], [PK, 2000, 1.5, 2.5],
             [LP, 3100, 0.6, 0], [LP, 4200, 0.9, 0]] },

  // 410 Tweed: '59 Bassman, open back, 4 x 10" Jensen alnico. The tens resonate higher (112 Hz) and punch at
  // 180 Hz, forward mids, a bright break-up peak at 2.9 kHz.
  { resHz: 112, resQ: 1.5, resDb: 3, tap1Ms: 0.8, tap1Gain: 0.18, tap2Ms: 2.6, tap2Gain: -0.18, tapLowPassHz: 1600, trimDb: -2.8,
    stages: [[HP, 95, 0.75, 0], [PK, 180, 0.9, 2], [PK, 1100, 0.8, 2], [PK, 2900, 1.5, 4],
             [PK, 4200, 2.2, 1.5], [LP, 5000, 0.7, 0], [LP, 6100, 0.95, 0]] },

  // 112 BF 'Lux: Deluxe Reverb, small open-back combo, Oxford 12K5-6. Soft, scooped mids, a gentle break-up
  // peak at 3 kHz and a smooth top that gives up before 5 kHz.
  { resHz: 100, resQ: 1.7, resDb: 2.5, tap1Ms: 1, tap1Gain: 0, tap2Ms: 1.3, tap2Gain: -0.24, tapLowPassHz: 1500, trimDb: 0.4,
    stages: [[HP, 92, 0.7, 0], [PK, 250, 0.9, 1.5], [PK, 600, 0.8, -3.5], [PK, 1500, 1.5, -1],
             [PK, 3000, 1.3, 3.5], [LP, 4300, 0.65, 0], [LP, 5400, 0.9, 0]] },

  // 112 Celest 12-H: Divided by 13 combo, open back, Celestion G12H Heritage. The heavy-magnet speaker: a low
  // resonance and thick low mids, no mid scoop, a bark at 1.4 kHz and a strong break-up peak at 2.8 kHz.
  { resHz: 85, resQ: 1.5, resDb: 3, tap1Ms: 1, tap1Gain: 0, tap2Ms: 1.6, tap2Gain: -0.2, tapLowPassHz: 1500, trimDb: -3.1,
    stages: [[HP, 75, 0.7, 0], [PK, 200, 0.8, 2.5], [PK, 700, 1, 1], [PK, 1400, 0.9, 2.5],
             [PK, 2800, 1.5, 4.5], [PK, 4100, 2.5, 1.5], [LP, 4900, 0.7, 0], [LP, 6000, 0.95, 0]] },

  // 212 PhD Ported: Dr. Z "Z Best", a ported 2x12 with one G12H Heritage and one Vintage 30. The port keeps the
  // bass flat to 70 Hz and then drops at 24 dB/octave; full low mids, and the two different speakers give
  // two break-up peaks (2.2 and 3.3 kHz).
  { resHz: 88, resQ: 1.2, resDb: 2, tap1Ms: 0.7, tap1Gain: 0.15, tap2Ms: 1.6, tap2Gain: 0.1, tapLowPassHz: 1300, trimDb: -3.3,
    stages: [[HP, 70, 1.1, 0], [HP, 55, 0.6, 0], [PK, 280, 0.7, 2.5], [PK, 1300, 0.9, 2],
             [PK, 2200, 1.4, 3], [PK, 3300, 1.6, 4], [LP, 5000, 0.7, 0], [LP, 6000, 0.95, 0]] },

  // 112 Blue Bell: '61 Vox AC-15, open back, Celestion Alnico Blue. Soft lows, relaxed mids and the chime:
  // break-up peaks at 2.6 and 4.4 kHz with a top that stays open to 6 kHz.
  { resHz: 90, resQ: 1.6, resDb: 2, tap1Ms: 1, tap1Gain: 0, tap2Ms: 1.5, tap2Gain: -0.24, tapLowPassHz: 1500, trimDb: -1.6,
    stages: [[HP, 88, 0.7, 0], [PK, 300, 0.8, 1.5], [PK, 800, 1, -2], [PK, 2600, 1.3, 4],
             [PK, 4400, 1.8, 3], [LP, 5400, 0.7, 0], [LP, 6500, 1, 0]] },

  // 212 Silver Bell: Vox AC-30, open back, 2 x Celestion Alnico Silver. The same chime from a bigger box:
  // a lower resonance, fuller low mids, the Vox's forward upper mids and the ripple of two speakers.
  { resHz: 84, resQ: 1.5, resDb: 2.5, tap1Ms: 0.85, tap1Gain: 0.16, tap2Ms: 1.3, tap2Gain: -0.2, tapLowPassHz: 1400, trimDb: -2.5,
    stages: [[HP, 78, 0.7, 0], [PK, 200, 0.8, 2], [PK, 1400, 0.9, 2], [PK, 2500, 1.2, 3],
             [PK, 4200, 1.6, 2.5], [LP, 5500, 0.7, 0], [LP, 6600, 1, 0]] },

  // 412 Greenback 25: Marshall 4x12, closed back, Celestion G12M. The closed box pushes the resonance up to a
  // 118 Hz thump; warm low mids with only a shallow box dip, woody mids at 850 Hz, break-up peaks at 2.3 and
  // 3.5 kHz and the earliest roll-off of the Celestions.
  { resHz: 118, resQ: 1.4, resDb: 3, tap1Ms: 1, tap1Gain: 0.18, tap2Ms: 2.05, tap2Gain: 0.14, tapLowPassHz: 1200, trimDb: -2.2,
    stages: [[HP, 85, 0.8, 0], [PK, 220, 0.9, 1.5], [PK, 400, 1.1, -1], [PK, 850, 0.8, 2],
             [PK, 2300, 1.4, 4], [PK, 3500, 2, 2.5], [LP, 4300, 0.65, 0], [LP, 5400, 0.9, 0]] },

  // 412 Blackback 30: Marshall 4x12, Celestion G12H30. Tighter and deeper than the Greenback, a stronger and
  // higher break-up peak (3 kHz), more top.
  { resHz: 105, resQ: 1.3, resDb: 3.5, tap1Ms: 1, tap1Gain: 0.18, tap2Ms: 2.05, tap2Gain: 0.14, tapLowPassHz: 1200, trimDb: -1.8,
    stages: [[HP, 72, 0.85, 0], [PK, 450, 1, -2.5], [PK, 1200, 1, 1], [PK, 3000, 1.5, 5.5],
             [PK, 4400, 2.2, 2.5], [LP, 5100, 0.7, 0], [LP, 6000, 0.95, 0]] },

  // 412 Brit T-75: Marshall 4x12, Celestion G12T-75. Big lows, scooped mids and the fizzy, extended top: the
  // break-up peaks sit at 3.9 and 5.3 kHz and the roll-off starts near 6 kHz.
  { resHz: 112, resQ: 1.2, resDb: 3.5, tap1Ms: 1, tap1Gain: 0.18, tap2Ms: 2.05, tap2Gain: 0.14, tapLowPassHz: 1200, trimDb: 0.2,
    stages: [[HP, 84, 0.8, 0], [PK, 650, 0.7, -4.5], [PK, 1500, 1.5, -1], [PK, 3900, 1.4, 2.5],
             [PK, 5300, 2.2, 2], [LP, 5800, 0.7, 0], [LP, 6600, 1, 0]] },

  // 412 Uber: Bogner Uberkab, an oversized closed 4x12 with two G12T-75 and two Vintage 30. The deepest guitar
  // bass, a wide mid scoop, the V30's 2.2 kHz peak and the T-75's 4.3 kHz one with a dip between them.
  { resHz: 95, resQ: 1.2, resDb: 4.5, tap1Ms: 1.1, tap1Gain: 0.2, tap2Ms: 2.3, tap2Gain: 0.15, tapLowPassHz: 1200, trimDb: -1.5,
    stages: [[HP, 68, 0.9, 0], [PK, 500, 0.9, -3], [PK, 2200, 1.5, 3.5], [PK, 3100, 2.5, -1.5],
             [PK, 4300, 1.7, 3], [LP, 5400, 0.7, 0], [LP, 6300, 1, 0]] },

  // 412 Tread V-30: Mesa/Boogie Rectifier 4x12, oversized, Celestion Vintage 30. Big low end, a deep box dip,
  // the Vintage 30's pushed mids and its spike at 2.3 kHz, a notch above it, a second peak at 4.1 kHz, gone above 5 kHz.
  { resHz: 104, resQ: 1.3, resDb: 4, tap1Ms: 1.05, tap1Gain: 0.18, tap2Ms: 2.2, tap2Gain: 0.15, tapLowPassHz: 1200, trimDb: -2,
    stages: [[HP, 76, 0.9, 0], [PK, 420, 1, -3.5], [PK, 1300, 0.9, 2.5], [PK, 2300, 1.6, 4.5],
             [PK, 3300, 2.5, -2.5], [PK, 4100, 2, 2.5], [LP, 4800, 0.7, 0], [LP, 5700, 0.95, 0]] },

  // 412 XXL V-30: Engl Pro 4x12, Vintage 30. Tighter than the Mesa: less sub-bass, lean low mids (a dip at
  // 300 Hz), focused mids at 1.6 kHz, the Vintage 30 peaks at 2.6 and 4.2 kHz, a smooth roll-off.
  { resHz: 115, resQ: 1.1, resDb: 2.5, tap1Ms: 0.95, tap1Gain: 0.16, tap2Ms: 1.9, tap2Gain: 0.12, tapLowPassHz: 1200, trimDb: -2,
    stages: [[HP, 95, 0.8, 0], [PK, 300, 1, -3], [PK, 1600, 0.8, 3.5], [PK, 2600, 1.7, 4],
             [PK, 4200, 2, 3], [LP, 5200, 0.65, 0], [LP, 6300, 0.9, 0]] },

  // 115 Flip Top: Ampeg B-15, one 15" CTS in a closed double-baffle box. Bass down to 45 Hz with a round bump
  // at 70 Hz, a 15" cone's break-up at 1.5 kHz and nothing above 3 kHz.
  { resHz: 70, resQ: 1.2, resDb: 4, tap1Ms: 1, tap1Gain: 0, tap2Ms: 2.3, tap2Gain: 0.12, tapLowPassHz: 900, trimDb: -1.3,
    stages: [[HP, 42, 0.8, 0], [PK, 160, 0.8, 2], [PK, 450, 0.9, -2], [PK, 1500, 1.2, 3],
             [LP, 2400, 0.7, 0], [LP, 3300, 1, 0]] },

  // 212 Jazz Rivet: Roland JC-120's open-back 2x12. Stiff, clean, hi-fi speakers: even mids, little resonance
  // (a solid-state amp damps it) and the brightest, most extended top of the guitar cabinets.
  { resHz: 92, resQ: 1.2, resDb: 1.5, tap1Ms: 1.2, tap1Gain: 0.16, tap2Ms: 1.9, tap2Gain: -0.18, tapLowPassHz: 1400, trimDb: -0.6,
    stages: [[HP, 85, 0.7, 0], [PK, 400, 1, 1], [PK, 1500, 1, -1.5], [PK, 3600, 1.2, 2.5],
             [PK, 5600, 1.8, 2.5], [LP, 6300, 0.7, 0], [LP, 7400, 0.95, 0]] },

  // 108 Small Tweed: tweed Fender Champ, one 8" speaker in a tiny open box. Resonance at 150 Hz, a boxy 600 Hz
  // hump, a break-up peak at 2.8 kHz.
  { resHz: 150, resQ: 1.8, resDb: 3, tap1Ms: 1, tap1Gain: 0, tap2Ms: 0.95, tap2Gain: -0.26, tapLowPassHz: 2200, trimDb: -0.5,
    stages: [[HP, 135, 0.75, 0], [PK, 600, 0.9, 3], [PK, 1400, 1.5, -1.5], [PK, 2800, 1.4, 4],
             [LP, 4000, 0.7, 0], [LP, 5000, 0.95, 0]] },

  // 810 SV Beast: Ampeg SVT 8x10, sealed. Deep bass with the low-mid punch of eight tens, their grind at
  // 2.4 kHz, rolled off above 4 kHz.
  { resHz: 78, resQ: 1.1, resDb: 3.5, tap1Ms: 0.8, tap1Gain: 0.22, tap2Ms: 2.4, tap2Gain: 0.14, tapLowPassHz: 1100, trimDb: -2.5,
    stages: [[HP, 52, 0.75, 0], [PK, 200, 0.8, 2.5], [PK, 600, 1, -2], [PK, 2400, 1.3, 4],
             [LP, 3700, 0.7, 0], [LP, 4600, 0.95, 0]] },

  // 410 Rhino: Ampeg SVT-410HLF, ported and tuned low, with a horn. The deepest bass (24 dB/octave below 38 Hz),
  // scooped low mids and the horn's top, which makes it the most extended cabinet here.
  { resHz: 58, resQ: 1, resDb: 3.5, tap1Ms: 0.8, tap1Gain: 0.2, tap2Ms: 2.6, tap2Gain: 0.12, tapLowPassHz: 1100, trimDb: -1.2,
    stages: [[HP, 38, 1, 0], [HP, 32, 0.6, 0], [PK, 350, 0.8, -3], [PK, 2000, 1.2, 2.5],
             [PK, 5000, 1.2, 3], [LP, 6300, 0.7, 0], [LP, 7400, 0.9, 0]] },

  // 412 Classic: this project's original cab voicing (Source/DSP/CabSim.h). With Low Cut at 75 Hz and Res Level,
  // Thump and Decay at 50 % the chain is exactly CabSim's: HP 75 Hz, +3 dB at 120 Hz, -3.5 dB at 450 Hz,
  // +4 dB at 2.3 kHz, low-passes at 5 and 6.5 kHz. No comb, no trim; its microphone is handled in setBank().
  { resHz: 120, resQ: 1.4, resDb: 3, tap1Ms: 1, tap1Gain: 0, tap2Ms: 1, tap2Gain: 0, tapLowPassHz: 1000, trimDb: 0,
    stages: [[PK, 450, 1, -3.5], [PK, 2300, 1.3, 4], [LP, 5000, 0.6, 0], [LP, 6500, 0.9, 0]] },
];

// A microphone in front of the speaker: shelves and peaks only, so a response can be undone exactly
// (the 412 Classic needs that). trimDb keeps the loudness the same when the microphone is changed.
const MIC_DEFS = [
  // 57 On Xs: Shure SM57 on axis. Thin lows, a rise from 3 kHz into the presence peak near 6 kHz.
  { trimDb: 0, stages: [[LS, 160, 0, -2.5], [PK, 3200, 0.7, 1.5], [PK, 5800, 1.3, 4.5]] },
  // 57 Off Xs: the SM57 turned away from the cone: most of the presence peak is gone, the top is dull and
  // there is a cancellation dip at 2.6 kHz.
  { trimDb: 0.6, stages: [[LS, 160, 0, -1], [PK, 2600, 1.5, -2], [PK, 5000, 1, 1.5], [HS, 3800, 0, -5]] },
  // 409 Dyn: Sennheiser MD 409. Warm low mids without deep bass, a smooth presence lift at 3.6 kHz, a soft top.
  { trimDb: -1.1, stages: [[LS, 100, 0, -1], [PK, 300, 0.7, 2], [PK, 3600, 1, 3.5], [HS, 7500, 0, -3]] },
  // 421 Dyn: Sennheiser MD 421. Scooped clarity: solid lows, a dip at 420 Hz, a strong peak at 4.2 kHz, open top.
  { trimDb: 0, stages: [[LS, 110, 0, 2], [PK, 420, 0.8, -3.5], [PK, 4200, 1.2, 5], [HS, 8500, 0, 1]] },
  // 4038 Rbn: Coles 4038 ribbon. A figure-8's big proximity bass, no presence peak, the darkest top.
  { trimDb: -1.8, stages: [[LS, 190, 0, 5], [PK, 3000, 0.8, -1.5], [HS, 4600, 0, -5]] },
  // 121 Rbn: Royer R-121 ribbon. Proximity bass, a slight push at 2.9 kHz, a smooth top that falls less than the Coles'.
  { trimDb: -1.6, stages: [[LS, 160, 0, 4], [PK, 2900, 0.8, 1.5], [HS, 6500, 0, -3]] },
  // 67 Cond: Neumann U67. Nearly flat: slightly warm lows, a very broad +1 dB around 5 kHz, a soft top.
  { trimDb: -0.4, stages: [[LS, 120, 0, 1.5], [PK, 5200, 0.6, 1], [HS, 10000, 0, -2]] },
  // 87 Cond: Neumann U87. Flat lows and mids, a little forward at 3.6 kHz, a lifted, extended top.
  { trimDb: -0.2, stages: [[LS, 90, 0, -1], [PK, 3600, 0.7, 1.5], [HS, 7500, 0, 3.5]] },
  // 12 Dyn: AKG D12, the vintage kick-drum microphone. A bass-chamber bump at 85 Hz, scooped at 400 Hz,
  // presence at 3 kHz, a rolled-off top.
  { trimDb: -1.4, stages: [[PK, 85, 0.9, 5], [PK, 400, 0.8, -3], [PK, 3000, 1, 3], [HS, 7500, 0, -4]] },
  // 112 Dyn: AKG D112. A bump at 100 Hz, the deepest mid scoop, a narrow, strong click peak at 4 kHz.
  { trimDb: -0.7, stages: [[PK, 100, 1, 4], [PK, 500, 0.7, -4.5], [PK, 4000, 1.6, 6], [HS, 9000, 0, -3]] },
  // 20 Dyn: Electro-Voice RE20. Variable-D: no proximity boost (the leanest lows here), and the flattest
  // dynamic; a small lift at 7 kHz before the top falls away.
  { trimDb: 0.7, stages: [[LS, 100, 0, -2], [PK, 380, 0.8, -1.5], [PK, 7000, 1, 2], [HS, 11000, 0, -3]] },
  // 7 Dyn: Shure SM7B. Warm lows, a dip at 3.2 kHz, a presence lift at 6.5 kHz, a soft top.
  { trimDb: -0.8, stages: [[LS, 150, 0, 3], [PK, 3200, 1.2, -2], [PK, 6500, 1.2, 2.5], [HS, 10000, 0, -4]] },
  // 40 Dyn: Heil PR40. Deep, tight lows, flat mids, a broad rise around 5.5 kHz that stays open to the top.
  { trimDb: -0.6, stages: [[LS, 80, 0, 2.5], [PK, 5500, 0.6, 3.5], [HS, 9000, 0, 2]] },
  // 47 Cond: Neumann U47. Rich lows, slightly recessed mids, a broad upper-mid presence at 3.8 kHz, a smooth top.
  { trimDb: -1.5, stages: [[LS, 150, 0, 4], [PK, 700, 0.7, -1.5], [PK, 3800, 0.8, 4.5], [HS, 12000, 0, -1]] },
];

// E.R.: six early reflections of a small room, alternating in polarity (so they add no bass) and darker than the direct sound.
const ROOM_TAP_MS = [5.3, 9.1, 13.7, 19.9, 26.3, 34.1];
const ROOM_TAP_GAIN = Float32Array.from([0.42, -0.36, 0.30, -0.24, 0.19, -0.15]);

// ---- biquad coefficients: the formulas of core.js's Biquad (DspUtils.h), written out so that nothing is allocated
function setCoefficients(bq, b0, b1, b2, a0, a1, a2) {
  bq.b0 = f32(b0 / a0); bq.b1 = f32(b1 / a0); bq.b2 = f32(b2 / a0);
  bq.a1 = f32(a1 / a0); bq.a2 = f32(a2 / a0);
}
function setLowPass(bq, fs, fc, q) {
  const w0 = (2 * PI * clamp(fc, 1, 0.49 * fs)) / fs, c = Math.cos(w0), a = Math.sin(w0) / (2 * q);
  setCoefficients(bq, (1 - c) * 0.5, 1 - c, (1 - c) * 0.5, 1 + a, -2 * c, 1 - a);
}
function setHighPass(bq, fs, fc, q) {
  const w0 = (2 * PI * clamp(fc, 1, 0.49 * fs)) / fs, c = Math.cos(w0), a = Math.sin(w0) / (2 * q);
  setCoefficients(bq, (1 + c) * 0.5, -(1 + c), (1 + c) * 0.5, 1 + a, -2 * c, 1 - a);
}
function setPeak(bq, fs, fc, q, gainDb) {
  const w0 = (2 * PI * clamp(fc, 1, 0.49 * fs)) / fs, c = Math.cos(w0), a = Math.sin(w0) / (2 * q);
  const A = Math.pow(10, gainDb / 40);
  setCoefficients(bq, 1 + a * A, -2 * c, 1 - a * A, 1 + a / A, -2 * c, 1 - a / A);
}
function setLowShelf(bq, fs, fc, gainDb) {
  const w0 = (2 * PI * clamp(fc, 1, 0.49 * fs)) / fs, c = Math.cos(w0), a = Math.sin(w0) / (2 * 0.70710678);
  const A = Math.pow(10, gainDb / 40), k = 2 * Math.sqrt(A) * a;
  setCoefficients(bq, A * ((A + 1) - (A - 1) * c + k), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - k),
                  (A + 1) + (A - 1) * c + k, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - k);
}
function setHighShelf(bq, fs, fc, gainDb) {
  const w0 = (2 * PI * clamp(fc, 1, 0.49 * fs)) / fs, c = Math.cos(w0), a = Math.sin(w0) / (2 * 0.70710678);
  const A = Math.pow(10, gainDb / 40), k = 2 * Math.sqrt(A) * a;
  setCoefficients(bq, A * ((A + 1) + (A - 1) * c + k), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - k),
                  (A + 1) - (A - 1) * c + k, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - k);
}
function setStage(bq, fs, s, gainSign) {
  switch (s[0]) {
    case HP: setHighPass(bq, fs, s[1], s[2]); break;
    case LP: setLowPass(bq, fs, s[1], s[2]); break;
    case PK: setPeak(bq, fs, s[1], s[2], gainSign * s[3]); break;
    case LS: setLowShelf(bq, fs, s[1], gainSign * s[3]); break;
    case HS: setHighShelf(bq, fs, s[1], gainSign * s[3]); break;
    default: break;
  }
}

/** Sets up the stages of `list` that lie below (low = true) or at and above LOW_STAGE_HZ, in their listed order. */
function addStages(filters, count, fs, list, low, gainSign) {
  for (let i = 0; i < list.length; ++i)
    if ((list[i][1] < LOW_STAGE_HZ) === low) setStage(filters[count++], fs, list[i], gainSign);
  return count;
}

function powerOfTwoAbove(n) {
  let size = 64;
  while (size < n) size *= 2;
  return size;
}

/** Biquad::process over a buffer, in float arithmetic like the C++: for the low biquads. */
function runLow(bq, x, n) {
  const b0 = bq.b0, b1 = bq.b1, b2 = bq.b2, a1 = bq.a1, a2 = bq.a2;
  let z1 = bq.z1, z2 = bq.z2;
  for (let i = 0; i < n; ++i) {
    const v = x[i];
    const y = f32(f32(b0 * v) + z1);
    z1 = f32(f32(f32(b1 * v) - f32(a1 * y)) + z2);
    z2 = f32(f32(b2 * v) - f32(a2 * y));
    x[i] = y;
  }
  bq.z1 = z1; bq.z2 = z2;
}

/** Biquad::process over a buffer, in doubles. */
function runHigh(bq, x, n) {
  const b0 = bq.b0, b1 = bq.b1, b2 = bq.b2, a1 = bq.a1, a2 = bq.a2;
  let z1 = bq.z1, z2 = bq.z2;
  for (let i = 0; i < n; ++i) {
    const v = x[i];
    const y = b0 * v + z1;
    z1 = b1 * v - a1 * y + z2;
    z2 = b2 * v - a2 * y;
    x[i] = y;
  }
  bq.z1 = z1; bq.z2 = z2;
}

/** 0..1 -> 0..1 with no kink at either end: the shape of the Mic and Low Cut fades (in float arithmetic). */
function smoothStep(t) { return f32(f32(t * t) * f32(3 - 2 * t)); }

/** One microphone: its low stages first (`low` of them), then the others, then its loudness trim. */
function makeBank() {
  const stages = [];
  for (let i = 0; i < MAX_BANK_STAGES; ++i) stages.push(new Biquad());
  return { stages, low: 0, count: 0, gain: 1 };
}

function runBank(bank, x, n) {
  for (let s = 0; s < bank.low; ++s) runLow(bank.stages[s], x, n);
  for (let s = bank.low; s < bank.count; ++s) runHigh(bank.stages[s], x, n);
  const gain = bank.gain;
  for (let i = 0; i < n; ++i) x[i] = x[i] * gain;
}

/** A knob value gliding to its target in steps of one chunk. Doubles and an integer counter only, so that it
    arrives at exactly the C++'s values: the coefficients of a biquad at 50 Hz turn on the last bit. */
class KnobRamp {
  constructor(initial) { this.start = this.target = this.current = initial; this.length = 1; this.left = 0; }
  prepare(lengthInChunks) { this.length = Math.max(1, lengthInChunks); this.snap(); }
  snap() { this.start = this.current = this.target; this.left = 0; }
  isMoving() { return this.left > 0; }
  setTarget(v) {
    if (v !== this.target) { this.start = this.current; this.target = v; this.left = this.length; }
  }
  /** One chunk further. */
  next() {
    if (this.left > 0) {
      --this.left;
      this.current = this.left === 0 ? this.target : this.target + (this.start - this.target) * (this.left / this.length);
    }
    return this.current;
  }
}

// Knobs: Mic | E.R. | Low Cut | Res Level | Thump | Decay
//   - Res Level scales the speaker's bass resonance peak and raises the overall level (0.03 dB per %);
//     Thump scales the peak's height, Decay its Q (how long the cone rings on). Peak height in dB =
//     the cabinet's own value x Res Level / 50 x Thump / 50, so Thump and Decay do nothing at Res Level 0.
//   - the Mic switch crossfades over 30 ms, Low Cut fades in and out (at 20 Hz) over 30 ms; the other knobs
//     glide over 50 ms.
export class CabFx {
  constructor() {
    this.fs = 48000;
    this.variant = CLASSIC; this.micTarget = 0; this.micActive = 0; this.micFading = 0;
    this.modelChanged = true;

    // knob values, moving in steps of one chunk (the filter coefficients are computed from them)
    this.lowCutHz = new KnobRamp(20); this.resLevel = new KnobRamp(50); this.thump = new KnobRamp(50); this.decay = new KnobRamp(50);
    // per sample
    this.roomGain = new Smoothed(0); this.outGain = new Smoothed(1);

    this.lowCut = new Biquad(); this.resonance = new Biquad();
    this.stages = [];
    for (let i = 0; i < MAX_CAB_STAGES; ++i) this.stages.push(new Biquad());
    this.numLowStages = 0; this.numStages = 0;
    this.lowCutOn = false; this.lowCutRunning = false;
    this.lowCutFade = 0; // 0 = out ... fadeLength = fully in

    this.combOn = false;
    this.combBuffer = new Float32Array(64);
    this.combMask = 63; this.combPos = 0; this.combDelay1 = 1; this.combDelay2 = 1;
    this.combGain1 = 0; this.combGain2 = 0;
    this.combLowPass = new OnePole();

    this.bankA = makeBank(); this.bankB = makeBank();
    this.fadeLeft = 0; this.fadeLength = 1; // fadeLength: 30 ms, for the microphones and for Low Cut

    this.roomBuffer = new Float32Array(64);
    this.roomMask = 63; this.roomPos = 0;
    this.roomDelay = new Int32Array(NUM_ROOM_TAPS);
    this.roomLowPass = new OnePole();

    this.work = new Float32Array(CHUNK); this.work2 = new Float32Array(CHUNK);
  }

  prepare(sampleRate, maxBlock) {
    const fs = this.fs = sampleRate;

    const rampChunks = Math.round((0.05 * fs) / CHUNK);
    this.lowCutHz.prepare(rampChunks); this.resLevel.prepare(rampChunks); this.thump.prepare(rampChunks); this.decay.prepare(rampChunks);

    this.roomGain.reset(fs, 0.05);
    this.outGain.reset(fs, 0.05);
    this.fadeLength = Math.max(1, Math.floor(0.03 * fs));

    this.combBuffer = new Float32Array(powerOfTwoAbove(Math.floor(0.004 * fs) + 8));
    this.combMask = this.combBuffer.length - 1;
    this.roomBuffer = new Float32Array(powerOfTwoAbove(Math.floor(0.04 * fs) + 8));
    this.roomMask = this.roomBuffer.length - 1;

    for (let k = 0; k < NUM_ROOM_TAPS; ++k)
      this.roomDelay[k] = Math.max(1, Math.round(ROOM_TAP_MS[k] * 0.001 * fs));
    this.roomLowPass.setCutoff(fs, 4200);

    this.reset();
  }

  reset() {
    this.lowCutHz.snap(); this.resLevel.snap(); this.thump.snap(); this.decay.snap();
    this.roomGain.setCurrentAndTarget(this.roomGain.target); this.outGain.setCurrentAndTarget(this.outGain.target);
    this.lowCutFade = this.lowCutOn ? this.fadeLength : 0;

    this.micActive = this.micFading = this.micTarget;
    this.fadeLeft = 0;
    this.configure();

    this.lowCut.reset();
    this.resonance.reset();
    for (let i = 0; i < MAX_CAB_STAGES; ++i) this.stages[i].reset();
    for (let i = 0; i < MAX_BANK_STAGES; ++i) { this.bankA.stages[i].reset(); this.bankB.stages[i].reset(); }
    this.combLowPass.reset();
    this.roomLowPass.reset();
    this.combBuffer.fill(0);
    this.roomBuffer.fill(0);
    this.combPos = this.roomPos = 0;
    this.lowCutRunning = this.lowCutOn;
  }

  setModel(variant) {
    const newVariant = clamp(variant | 0, 0, NUM_CABS - 1);
    this.modelChanged = this.modelChanged || newVariant !== this.variant;
    this.variant = newVariant;
  }

  setParameters(k) {
    this.micTarget = clamp(Math.round(k[0]), 0, NUM_MICS - 1);
    this.roomGain.setTarget(f32(f32(0.007) * clamp(k[1], 0, 100)));

    const cut = clamp(k[2], 20, 500);
    this.lowCutHz.setTarget(cut);
    this.lowCutOn = cut > 20.5; // 20 Hz = off

    const res = clamp(k[3], 0, 100);
    this.resLevel.setTarget(res);
    this.thump.setTarget(clamp(k[4], 0, 100));
    this.decay.setTarget(clamp(k[5], 0, 100));
    const db = f32(f32(CAB_DEFS[this.variant].trimDb) + f32(f32(0.03) * f32(res - 50)));
    this.outGain.setTarget(f32(Math.pow(10, f32(db * f32(0.05))))); // dbToGain on floats
  }

  process(left, right, numSamples) {
    const x = this.work;

    for (let pos = 0; pos < numSamples; pos += CHUNK) {
      const n = Math.min(CHUNK, numSamples - pos);
      this.updateChunk();

      for (let i = 0; i < n; ++i) x[i] = 0.5 * (left[pos + i] + right[pos + i]);

      // ---- the low biquads, in float arithmetic: Low Cut ...
      if (this.lowCutRunning) {
        if (this.lowCutOn && this.lowCutFade >= this.fadeLength) {
          runLow(this.lowCut, x, n);
        } else { // fading in or out
          const bq = this.lowCut, b0 = bq.b0, b1 = bq.b1, b2 = bq.b2, a1 = bq.a1, a2 = bq.a2;
          const fadeLength = this.fadeLength, direction = this.lowCutOn ? 1 : -1;
          let z1 = bq.z1, z2 = bq.z2, fade = this.lowCutFade;
          for (let i = 0; i < n; ++i) {
            fade = clamp(fade + direction, 0, fadeLength);
            const v = x[i];
            const cut = f32(f32(b0 * v) + z1);
            z1 = f32(f32(f32(b1 * v) - f32(a1 * cut)) + z2);
            z2 = f32(f32(b2 * v) - f32(a2 * cut));
            x[i] = v + f32(smoothStep(f32(fade / fadeLength)) * f32(cut - v));
          }
          bq.z1 = z1; bq.z2 = z2; this.lowCutFade = fade;
          this.lowCutRunning = this.lowCutOn || fade > 0;
        }
      }

      // ... the speaker's bass resonance and the cabinet's low stages ...
      runLow(this.resonance, x, n);
      for (let s = 0; s < this.numLowStages; ++s) runLow(this.stages[s], x, n);

      // ... and the microphone (two of them while the Mic switch crossfades)
      if (this.fadeLeft > 0) {
        const y = this.work2, fadeLength = this.fadeLength, done = fadeLength - this.fadeLeft;
        for (let i = 0; i < n; ++i) y[i] = x[i];

        runBank(this.bankA, x, n);
        runBank(this.bankB, y, n);

        for (let i = 0; i < n; ++i) {
          const t = Math.min(1, f32((done + i + 1) / fadeLength));
          x[i] = x[i] + smoothStep(t) * (y[i] - x[i]);
        }

        this.fadeLeft -= n;
        if (this.fadeLeft <= 0) {
          this.fadeLeft = 0;
          const swap = this.bankA; this.bankA = this.bankB; this.bankB = swap;
          this.micActive = this.micFading;
        }
      } else {
        runBank(this.bankA, x, n);
      }

      // ---- the rest of the cabinet, in doubles
      for (let s = this.numLowStages; s < this.numStages; ++s) runHigh(this.stages[s], x, n);

      // the second speaker and the back wave, arriving late
      if (this.combOn) {
        const buffer = this.combBuffer, mask = this.combMask, d1 = this.combDelay1, d2 = this.combDelay2;
        const g1 = this.combGain1, g2 = this.combGain2, G = this.combLowPass.G;
        let p = this.combPos, state = this.combLowPass.s;
        for (let i = 0; i < n; ++i) {
          const v = x[i];
          const late = g1 * buffer[(p - d1) & mask] + g2 * buffer[(p - d2) & mask];
          buffer[p] = v;
          p = (p + 1) & mask;
          const step = (late - state) * G, lp = step + state;   // OnePole::lowPass
          state = lp + step;
          x[i] = v + lp;
        }
        this.combPos = p; this.combLowPass.s = state;
      }

      // E.R.
      {
        const buffer = this.roomBuffer, mask = this.roomMask, gain = this.roomGain;
        let p = this.roomPos;
        if (gain.isSmoothing() || gain.target > 0) {
          const delay = this.roomDelay, tap = ROOM_TAP_GAIN, G = this.roomLowPass.G;
          const d0 = delay[0], d1 = delay[1], d2 = delay[2], d3 = delay[3], d4 = delay[4], d5 = delay[5];
          const g0 = tap[0], g1 = tap[1], g2 = tap[2], g3 = tap[3], g4 = tap[4], g5 = tap[5];
          let state = this.roomLowPass.s;
          for (let i = 0; i < n; ++i) {
            const v = x[i];
            buffer[p] = v;
            const sum = g0 * buffer[(p - d0) & mask] + g1 * buffer[(p - d1) & mask] + g2 * buffer[(p - d2) & mask]
                      + g3 * buffer[(p - d3) & mask] + g4 * buffer[(p - d4) & mask] + g5 * buffer[(p - d5) & mask];
            p = (p + 1) & mask;
            const step = (sum - state) * G, lp = step + state;   // OnePole::lowPass
            state = lp + step;
            x[i] = v + gain.next() * lp;
          }
          this.roomLowPass.s = state;
        } else {
          for (let i = 0; i < n; ++i) {
            buffer[p] = x[i];
            p = (p + 1) & mask;
          }
        }
        this.roomPos = p;
      }

      const level = this.outGain;
      for (let i = 0; i < n; ++i) {
        const out = f32(x[i] * level.next());
        left[pos + i] = out;
        right[pos + i] = out;
      }
    }
  }

  // The microphone's filters. On the 412 Classic, whose voicing already is a finished, mic'd sound, the
  // microphones act relative to the 57 On Xs: that one is exactly flat (no filters at all), the others are
  // their own response with the 57's taken out (the same shelves and peaks with the opposite gain).
  setBank(bank, mic) {
    const m = MIC_DEFS[mic], fs = this.fs, relative = this.variant === CLASSIC;
    bank.low = bank.count = 0;
    bank.gain = f32(Math.pow(10, f32(f32(m.trimDb) * f32(0.05)))); // dbToGain on floats

    if (relative && mic === 0) return;

    bank.count = addStages(bank.stages, bank.count, fs, m.stages, true, 1);
    if (relative) bank.count = addStages(bank.stages, bank.count, fs, MIC_DEFS[0].stages, true, -1);
    bank.low = bank.count;
    bank.count = addStages(bank.stages, bank.count, fs, m.stages, false, 1);
    if (relative) bank.count = addStages(bank.stages, bank.count, fs, MIC_DEFS[0].stages, false, -1);
  }

  setResonance(res, thumpAmount, decayAmount) {
    const c = CAB_DEFS[this.variant];
    const gainDb = Math.min(MAX_RESONANCE_DB, c.resDb * (res / 50) * (thumpAmount / 50));
    const q = c.resQ * Math.pow(2, (decayAmount - 50) * 0.03); // 0.35 to 2.8 times the cabinet's Q
    setPeak(this.resonance, this.fs, c.resHz, q, gainDb);
  }

  /** Every coefficient for the model and the current knob values (audio state is left alone). */
  configure() {
    const c = CAB_DEFS[this.variant], fs = this.fs;

    this.numLowStages = addStages(this.stages, 0, fs, c.stages, true, 1);
    this.numStages = addStages(this.stages, this.numLowStages, fs, c.stages, false, 1);

    this.combOn = c.tap1Gain !== 0 || c.tap2Gain !== 0;
    this.combDelay1 = Math.max(1, Math.round(c.tap1Ms * 0.001 * fs));
    this.combDelay2 = Math.max(1, Math.round(c.tap2Ms * 0.001 * fs));
    this.combGain1 = f32(c.tap1Gain);
    this.combGain2 = f32(c.tap2Gain);
    this.combLowPass.setCutoff(fs, c.tapLowPassHz);

    setHighPass(this.lowCut, fs, this.lowCutHz.current, 0.707);
    this.setResonance(this.resLevel.current, this.thump.current, this.decay.current);
    this.setBank(this.bankA, this.micActive);
    if (this.fadeLeft > 0) this.setBank(this.bankB, this.micFading);
    this.modelChanged = false;
  }

  /** Once per chunk: lets the filters follow the knobs. */
  updateChunk() {
    if (this.modelChanged) this.configure(); // a model picked without reset(): the slot always resets, so this is only a safety net

    if (this.lowCutHz.isMoving()) setHighPass(this.lowCut, this.fs, this.lowCutHz.next(), 0.707);

    if (this.lowCutOn && !this.lowCutRunning) {
      this.lowCut.reset();
      this.lowCutRunning = true;
    }

    if (this.resLevel.isMoving() || this.thump.isMoving() || this.decay.isMoving()) {
      const r = this.resLevel.next(), t = this.thump.next(), d = this.decay.next();
      this.setResonance(r, t, d);
    }

    if (this.fadeLeft <= 0 && this.micTarget !== this.micActive) {
      this.micFading = this.micTarget;
      this.setBank(this.bankB, this.micFading);
      for (let i = 0; i < MAX_BANK_STAGES; ++i) this.bankB.stages[i].reset();
      this.fadeLeft = this.fadeLength;
    }
  }
}

// In the order of the variants. Knobs: Mic, E.R., Low Cut (20 Hz = off), Res Level, Thump, Decay.
const knobs = (mic, room, lowCut, res, thump, decay) =>
  [choice("Mic", MIC_NAMES, mic), percent("E.R.", room), freq("Low Cut", 20, 500, lowCut, 100), percent("Res Level", res),
   percent("Thump", thump), percent("Decay", decay)];
const cab = (key, name, variant, basedOn, k) => model(key, name, CATEGORY.cab, ENGINE.cabFx, variant, basedOn, k);

export const CAB_MODELS = [
  cab("cab_212_blackface", "212 Blackface Double", 0, "Fender Blackface Twin Reverb combo, 2x12 Jensen", knobs(0, 20, 20, 50, 50, 50)),
  cab("cab_412_hiway", "412 Hiway", 1, "Hiwatt cabinet, 4x12 Fane 12287", knobs(3, 12, 20, 50, 50, 45)),
  cab("cab_6x9_super_o", "6x9 Super O", 2, "Supro S6616 combo, 6x9 oval speaker", knobs(5, 20, 20, 50, 50, 55)),
  cab("cab_112_field_coil", "112 Field Coil", 3, "Gibson EH-185 combo, 1x12 field-coil speaker", knobs(6, 20, 20, 50, 50, 55)),
  cab("cab_410_tweed", "410 Tweed", 4, "'59 Fender Tweed Bassman combo, 4x10 Jensen alnico", knobs(0, 20, 20, 50, 50, 50)),
  cab("cab_112_bf_lux", "112 BF 'Lux", 5, "Fender Blackface Deluxe Reverb combo, 1x12 Oxford 12K5-6", knobs(0, 20, 20, 50, 50, 50)),
  cab("cab_112_celest_12h", "112 Celest 12-H", 6, "Divided by 13 9/15 combo, 1x12 Celestion G12H Heritage", knobs(0, 20, 20, 50, 50, 50)),
  cab("cab_212_phd_ported", "212 PhD Ported", 7, "Dr. Z Z Best ported cabinet, G12H Heritage + Vintage 30", knobs(0, 15, 20, 50, 55, 45)),
  cab("cab_112_blue_bell", "112 Blue Bell", 8, "'61 Vox AC-15 combo, 1x12 Celestion Alnico Blue", knobs(0, 20, 20, 50, 50, 50)),
  cab("cab_212_silver_bell", "212 Silver Bell", 9, "Vox AC-30 Top Boost, 2x12 Celestion Alnico Silver", knobs(2, 20, 20, 50, 50, 50)),
  cab("cab_412_greenback", "412 Greenback 25", 10, "Marshall cabinet, 4x12 Celestion G12M Greenback", knobs(0, 12, 20, 50, 50, 50)),
  cab("cab_412_blackback", "412 Blackback 30", 11, "Marshall cabinet, 4x12 Celestion Rola G12H30 Blackback", knobs(0, 12, 20, 50, 50, 50)),
  cab("cab_412_brit_t75", "412 Brit T-75", 12, "Marshall cabinet, 4x12 Celestion G12T-75", knobs(1, 12, 20, 50, 50, 50)),
  cab("cab_412_uber", "412 Uber", 13, "Bogner Uberschall cabinet, 4x12 G12T-75 + Vintage 30", knobs(0, 10, 20, 50, 50, 40)),
  cab("cab_412_tread_v30", "412 Tread V-30", 14, "Mesa/Boogie cabinet, 4x12 Celestion Vintage 30", knobs(0, 10, 20, 50, 50, 40)),
  cab("cab_412_xxl_v30", "412 XXL V-30", 15, "Engl Pro cabinet, 4x12 Celestion Vintage 30", knobs(3, 10, 20, 50, 50, 40)),
  cab("cab_115_flip_top", "115 Flip Top", 16, "Ampeg B-15 cabinet, 1x15 CTS (bass)", knobs(10, 8, 20, 50, 55, 55)),
  cab("cab_212_jazz_rivet", "212 Jazz Rivet", 17, "Roland JC-120 cabinet, 2x12", knobs(7, 20, 20, 50, 50, 50)),
  cab("cab_108_small_tweed", "108 Small Tweed", 18, "Fender tweed Champ cabinet, 1x8", knobs(5, 20, 20, 50, 50, 55)),
  cab("cab_810_sv_beast", "810 SV Beast", 19, "Ampeg SVT 8x10 cabinet (bass)", knobs(3, 8, 20, 50, 50, 45)),
  cab("cab_410_rhino", "410 Rhino", 20, "Ampeg SVT-410HLF cabinet, 4x10 + horn (bass)", knobs(12, 8, 20, 50, 50, 50)),
  cab("cab_412_classic", "412 Classic", 21, "this project's original cab voicing", knobs(0, 0, 75, 50, 50, 50)),
];
