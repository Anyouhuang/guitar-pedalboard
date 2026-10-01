#pragma once

#include "../DspUtils.h"
#include "../ModelTypes.h"

namespace fx
{
/** The Mic knob: the HD500X's 8 guitar-cab microphones, then the bass-cab microphones that are not among them. */
inline const char* const cabMicNames[] = { "57 On Xs", "57 Off Xs", "409 Dyn", "421 Dyn", "4038 Rbn", "121 Rbn", "67 Cond", "87 Cond",
                                           "12 Dyn", "112 Dyn", "20 Dyn", "7 Dyn", "40 Dyn", "47 Cond" };

namespace cab_detail
{
constexpr int numCabs = 22, numMics = 14;
constexpr int maxCabStages = 8, maxMicStages = 4, maxBankStages = 2 * maxMicStages;
constexpr int chunkSize = 32;        // filter coefficients follow a moving knob in steps of this many samples
constexpr int numRoomTaps = 6;
constexpr double maxResonanceDb = 16.0;

enum StageType { unused = 0, highPass, lowPass, peak, lowShelf, highShelf };

/** One biquad of a cabinet's or a microphone's response (q is ignored by the shelves, db by the pass filters). */
struct Stage { int type; double hz, q, db; };

/** A cabinet, built from what is known about the speaker and the box (there are no measured responses in here):
    - resHz / resQ / resDb: the bass resonance of the speaker in its box, as a peak the Res Level, Thump and
      Decay knobs scale (the values here are what 50 % / 50 % / 50 % gives)
    - tap1: a second speaker reaching the microphone late (comb ripple); tap2: the back wave, coming out of an
      open back in opposite polarity or bouncing off the back panel of a closed box. Both only below tapLowPassHz.
    - stages: the box's low roll-off, the body and box dips, the cone break-up peaks with the dips around them and
      the steep roll-off above the speaker's range (two 12 dB/octave low-passes), lowest frequency first
    - trimDb: brings every cabinet to the same loudness at its default knobs */
struct CabDef
{
    double resHz, resQ, resDb;
    double tap1Ms, tap1Gain, tap2Ms, tap2Gain, tapLowPassHz;
    double trimDb;
    Stage stages[maxCabStages];
};

inline const CabDef cabDefs[numCabs] =
{
    // 212 Blackface Double: Fender Twin Reverb, open back, 2 x Jensen C12N. Loose, not very deep bass (open back),
    // a big combo's thump at 130 Hz, mids scooped at 480 Hz, a glassy break-up peak at 3.3 kHz, the most
    // extended top of the Fenders.
    { 95.0, 1.6, 2.5,   1.1, 0.16, 1.6, -0.2, 1300.0,   -1.1,
      { { highPass, 80.0, 0.7, 0.0 }, { peak, 130.0, 1.0, 1.5 }, { peak, 480.0, 0.9, -2.5 }, { peak, 1300.0, 1.2, 1.5 },
        { peak, 3300.0, 1.3, 4.5 }, { lowPass, 5300.0, 0.7, 0.0 }, { lowPass, 6500.0, 0.9, 0.0 } } },

    // 412 Hiway: Hiwatt 4x12, closed back, Fane 12287. Tight and even: no box dip, full mids around 900 Hz,
    // a modest break-up peak and a clear top that reaches further than a Celestion's.
    { 105.0, 1.3, 2.5,   0.9, 0.14, 1.8, 0.1, 1200.0,   -1.8,
      { { highPass, 78.0, 0.8, 0.0 }, { peak, 900.0, 0.8, 2.5 }, { peak, 2700.0, 1.2, 2.0 },
        { peak, 4500.0, 1.8, 2.0 }, { lowPass, 5600.0, 0.65, 0.0 }, { lowPass, 6700.0, 0.9, 0.0 } } },

    // 6x9 Super O: Supro S6616, one small oval speaker in a little open box. No bass to speak of (resonance at
    // 140 Hz), a nasal honk around 1 kHz and an early roll-off.
    { 140.0, 2.0, 3.5,   1.0, 0.0, 1.1, -0.25, 2000.0,   -0.3,
      { { highPass, 125.0, 0.8, 0.0 }, { peak, 330.0, 1.2, -2.0 }, { peak, 950.0, 1.1, 4.0 }, { peak, 1700.0, 2.5, -1.5 },
        { peak, 2400.0, 1.6, 3.0 }, { lowPass, 3300.0, 0.7, 0.0 }, { lowPass, 4000.0, 1.0, 0.0 } } },

    // 112 Field Coil: Gibson EH-185, a 1939 field-coil 12" in an open-back combo. Mid-heavy and dark: a broad
    // 750 Hz hump, a low break-up peak at 2 kHz and the earliest, softest roll-off of the 12" speakers.
    { 92.0, 1.8, 3.0,   1.0, 0.0, 1.7, -0.22, 1500.0,   -1.8,
      { { highPass, 85.0, 0.7, 0.0 }, { peak, 240.0, 0.9, 1.5 }, { peak, 750.0, 0.8, 3.0 }, { peak, 2000.0, 1.5, 2.5 },
        { lowPass, 3100.0, 0.6, 0.0 }, { lowPass, 4200.0, 0.9, 0.0 } } },

    // 410 Tweed: '59 Bassman, open back, 4 x 10" Jensen alnico. The tens resonate higher (112 Hz) and punch at
    // 180 Hz, forward mids, a bright break-up peak at 2.9 kHz.
    { 112.0, 1.5, 3.0,   0.8, 0.18, 2.6, -0.18, 1600.0,   -2.8,
      { { highPass, 95.0, 0.75, 0.0 }, { peak, 180.0, 0.9, 2.0 }, { peak, 1100.0, 0.8, 2.0 },
        { peak, 2900.0, 1.5, 4.0 }, { peak, 4200.0, 2.2, 1.5 }, { lowPass, 5000.0, 0.7, 0.0 }, { lowPass, 6100.0, 0.95, 0.0 } } },

    // 112 BF 'Lux: Deluxe Reverb, small open-back combo, Oxford 12K5-6. Soft, scooped mids, a gentle break-up
    // peak at 3 kHz and a smooth top that gives up before 5 kHz.
    { 100.0, 1.7, 2.5,   1.0, 0.0, 1.3, -0.24, 1500.0,   0.4,
      { { highPass, 92.0, 0.7, 0.0 }, { peak, 250.0, 0.9, 1.5 }, { peak, 600.0, 0.8, -3.5 }, { peak, 1500.0, 1.5, -1.0 },
        { peak, 3000.0, 1.3, 3.5 }, { lowPass, 4300.0, 0.65, 0.0 }, { lowPass, 5400.0, 0.9, 0.0 } } },

    // 112 Celest 12-H: Divided by 13 combo, open back, Celestion G12H Heritage. The heavy-magnet speaker: a low
    // resonance and thick low mids, no mid scoop, a bark at 1.4 kHz and a strong break-up peak at 2.8 kHz.
    { 85.0, 1.5, 3.0,   1.0, 0.0, 1.6, -0.2, 1500.0,   -3.1,
      { { highPass, 75.0, 0.7, 0.0 }, { peak, 200.0, 0.8, 2.5 }, { peak, 700.0, 1.0, 1.0 }, { peak, 1400.0, 0.9, 2.5 },
        { peak, 2800.0, 1.5, 4.5 }, { peak, 4100.0, 2.5, 1.5 }, { lowPass, 4900.0, 0.7, 0.0 }, { lowPass, 6000.0, 0.95, 0.0 } } },

    // 212 PhD Ported: Dr. Z "Z Best", a ported 2x12 with one G12H Heritage and one Vintage 30. The port keeps the
    // bass flat to 70 Hz and then drops at 24 dB/octave; full low mids, and the two different speakers give
    // two break-up peaks (2.2 and 3.3 kHz).
    { 88.0, 1.2, 2.0,   0.7, 0.15, 1.6, 0.1, 1300.0,   -3.3,
      { { highPass, 70.0, 1.1, 0.0 }, { highPass, 55.0, 0.6, 0.0 }, { peak, 280.0, 0.7, 2.5 }, { peak, 1300.0, 0.9, 2.0 },
        { peak, 2200.0, 1.4, 3.0 }, { peak, 3300.0, 1.6, 4.0 }, { lowPass, 5000.0, 0.7, 0.0 }, { lowPass, 6000.0, 0.95, 0.0 } } },

    // 112 Blue Bell: '61 Vox AC-15, open back, Celestion Alnico Blue. Soft lows, relaxed mids and the chime:
    // break-up peaks at 2.6 and 4.4 kHz with a top that stays open to 6 kHz.
    { 90.0, 1.6, 2.0,   1.0, 0.0, 1.5, -0.24, 1500.0,   -1.6,
      { { highPass, 88.0, 0.7, 0.0 }, { peak, 300.0, 0.8, 1.5 }, { peak, 800.0, 1.0, -2.0 }, { peak, 2600.0, 1.3, 4.0 },
        { peak, 4400.0, 1.8, 3.0 }, { lowPass, 5400.0, 0.7, 0.0 }, { lowPass, 6500.0, 1.0, 0.0 } } },

    // 212 Silver Bell: Vox AC-30, open back, 2 x Celestion Alnico Silver. The same chime from a bigger box:
    // a lower resonance, fuller low mids, the Vox's forward upper mids and the ripple of two speakers.
    { 84.0, 1.5, 2.5,   0.85, 0.16, 1.3, -0.2, 1400.0,   -2.5,
      { { highPass, 78.0, 0.7, 0.0 }, { peak, 200.0, 0.8, 2.0 }, { peak, 1400.0, 0.9, 2.0 }, { peak, 2500.0, 1.2, 3.0 },
        { peak, 4200.0, 1.6, 2.5 }, { lowPass, 5500.0, 0.7, 0.0 }, { lowPass, 6600.0, 1.0, 0.0 } } },

    // 412 Greenback 25: Marshall 4x12, closed back, Celestion G12M. The closed box pushes the resonance up to a
    // 118 Hz thump; warm low mids with only a shallow box dip, woody mids at 850 Hz, break-up peaks at 2.3 and
    // 3.5 kHz and the earliest roll-off of the Celestions.
    { 118.0, 1.4, 3.0,   1.0, 0.18, 2.05, 0.14, 1200.0,   -2.2,
      { { highPass, 85.0, 0.8, 0.0 }, { peak, 220.0, 0.9, 1.5 }, { peak, 400.0, 1.1, -1.0 }, { peak, 850.0, 0.8, 2.0 },
        { peak, 2300.0, 1.4, 4.0 }, { peak, 3500.0, 2.0, 2.5 }, { lowPass, 4300.0, 0.65, 0.0 }, { lowPass, 5400.0, 0.9, 0.0 } } },

    // 412 Blackback 30: Marshall 4x12, Celestion G12H30. Tighter and deeper than the Greenback, a stronger and
    // higher break-up peak (3 kHz), more top.
    { 105.0, 1.3, 3.5,   1.0, 0.18, 2.05, 0.14, 1200.0,   -1.8,
      { { highPass, 72.0, 0.85, 0.0 }, { peak, 450.0, 1.0, -2.5 }, { peak, 1200.0, 1.0, 1.0 }, { peak, 3000.0, 1.5, 5.5 },
        { peak, 4400.0, 2.2, 2.5 }, { lowPass, 5100.0, 0.7, 0.0 }, { lowPass, 6000.0, 0.95, 0.0 } } },

    // 412 Brit T-75: Marshall 4x12, Celestion G12T-75. Big lows, scooped mids and the fizzy, extended top: the
    // break-up peaks sit at 3.9 and 5.3 kHz and the roll-off starts near 6 kHz.
    { 112.0, 1.2, 3.5,   1.0, 0.18, 2.05, 0.14, 1200.0,   0.2,
      { { highPass, 84.0, 0.8, 0.0 }, { peak, 650.0, 0.7, -4.5 }, { peak, 1500.0, 1.5, -1.0 }, { peak, 3900.0, 1.4, 2.5 },
        { peak, 5300.0, 2.2, 2.0 }, { lowPass, 5800.0, 0.7, 0.0 }, { lowPass, 6600.0, 1.0, 0.0 } } },

    // 412 Uber: Bogner Uberkab, an oversized closed 4x12 with two G12T-75 and two Vintage 30. The deepest guitar
    // bass, a wide mid scoop, the V30's 2.2 kHz peak and the T-75's 4.3 kHz one with a dip between them.
    { 95.0, 1.2, 4.5,   1.1, 0.2, 2.3, 0.15, 1200.0,   -1.5,
      { { highPass, 68.0, 0.9, 0.0 }, { peak, 500.0, 0.9, -3.0 }, { peak, 2200.0, 1.5, 3.5 }, { peak, 3100.0, 2.5, -1.5 },
        { peak, 4300.0, 1.7, 3.0 }, { lowPass, 5400.0, 0.7, 0.0 }, { lowPass, 6300.0, 1.0, 0.0 } } },

    // 412 Tread V-30: Mesa/Boogie Rectifier 4x12, oversized, Celestion Vintage 30. Big low end, a deep box dip,
    // the Vintage 30's pushed mids and its spike at 2.3 kHz, a notch above it, a second peak at 4.1 kHz, gone above 5 kHz.
    { 104.0, 1.3, 4.0,   1.05, 0.18, 2.2, 0.15, 1200.0,   -2.0,
      { { highPass, 76.0, 0.9, 0.0 }, { peak, 420.0, 1.0, -3.5 }, { peak, 1300.0, 0.9, 2.5 }, { peak, 2300.0, 1.6, 4.5 },
        { peak, 3300.0, 2.5, -2.5 }, { peak, 4100.0, 2.0, 2.5 }, { lowPass, 4800.0, 0.7, 0.0 }, { lowPass, 5700.0, 0.95, 0.0 } } },

    // 412 XXL V-30: Engl Pro 4x12, Vintage 30. Tighter than the Mesa: less sub-bass, lean low mids (a dip at
    // 300 Hz), focused mids at 1.6 kHz, the Vintage 30 peaks at 2.6 and 4.2 kHz, a smooth roll-off.
    { 115.0, 1.1, 2.5,   0.95, 0.16, 1.9, 0.12, 1200.0,   -2.0,
      { { highPass, 95.0, 0.8, 0.0 }, { peak, 300.0, 1.0, -3.0 }, { peak, 1600.0, 0.8, 3.5 }, { peak, 2600.0, 1.7, 4.0 },
        { peak, 4200.0, 2.0, 3.0 }, { lowPass, 5200.0, 0.65, 0.0 }, { lowPass, 6300.0, 0.9, 0.0 } } },

    // 115 Flip Top: Ampeg B-15, one 15" CTS in a closed double-baffle box. Bass down to 45 Hz with a round bump
    // at 70 Hz, a 15" cone's break-up at 1.5 kHz and nothing above 3 kHz.
    { 70.0, 1.2, 4.0,   1.0, 0.0, 2.3, 0.12, 900.0,   -1.3,
      { { highPass, 42.0, 0.8, 0.0 }, { peak, 160.0, 0.8, 2.0 }, { peak, 450.0, 0.9, -2.0 }, { peak, 1500.0, 1.2, 3.0 },
        { lowPass, 2400.0, 0.7, 0.0 }, { lowPass, 3300.0, 1.0, 0.0 } } },

    // 212 Jazz Rivet: Roland JC-120's open-back 2x12. Stiff, clean, hi-fi speakers: even mids, little resonance
    // (a solid-state amp damps it) and the brightest, most extended top of the guitar cabinets.
    { 92.0, 1.2, 1.5,   1.2, 0.16, 1.9, -0.18, 1400.0,   -0.6,
      { { highPass, 85.0, 0.7, 0.0 }, { peak, 400.0, 1.0, 1.0 }, { peak, 1500.0, 1.0, -1.5 }, { peak, 3600.0, 1.2, 2.5 },
        { peak, 5600.0, 1.8, 2.5 }, { lowPass, 6300.0, 0.7, 0.0 }, { lowPass, 7400.0, 0.95, 0.0 } } },

    // 108 Small Tweed: tweed Fender Champ, one 8" speaker in a tiny open box. Resonance at 150 Hz, a boxy 600 Hz
    // hump, a break-up peak at 2.8 kHz.
    { 150.0, 1.8, 3.0,   1.0, 0.0, 0.95, -0.26, 2200.0,   -0.5,
      { { highPass, 135.0, 0.75, 0.0 }, { peak, 600.0, 0.9, 3.0 }, { peak, 1400.0, 1.5, -1.5 }, { peak, 2800.0, 1.4, 4.0 },
        { lowPass, 4000.0, 0.7, 0.0 }, { lowPass, 5000.0, 0.95, 0.0 } } },

    // 810 SV Beast: Ampeg SVT 8x10, sealed. Deep bass with the low-mid punch of eight tens, their grind at
    // 2.4 kHz, rolled off above 4 kHz.
    { 78.0, 1.1, 3.5,   0.8, 0.22, 2.4, 0.14, 1100.0,   -2.5,
      { { highPass, 52.0, 0.75, 0.0 }, { peak, 200.0, 0.8, 2.5 }, { peak, 600.0, 1.0, -2.0 }, { peak, 2400.0, 1.3, 4.0 },
        { lowPass, 3700.0, 0.7, 0.0 }, { lowPass, 4600.0, 0.95, 0.0 } } },

    // 410 Rhino: Ampeg SVT-410HLF, ported and tuned low, with a horn. The deepest bass (24 dB/octave below 38 Hz),
    // scooped low mids and the horn's top, which makes it the most extended cabinet here.
    { 58.0, 1.0, 3.5,   0.8, 0.2, 2.6, 0.12, 1100.0,   -1.2,
      { { highPass, 38.0, 1.0, 0.0 }, { highPass, 32.0, 0.6, 0.0 }, { peak, 350.0, 0.8, -3.0 }, { peak, 2000.0, 1.2, 2.5 },
        { peak, 5000.0, 1.2, 3.0 }, { lowPass, 6300.0, 0.7, 0.0 }, { lowPass, 7400.0, 0.9, 0.0 } } },

    // 412 Classic: this project's original cab voicing (Source/DSP/CabSim.h). With Low Cut at 75 Hz and Res Level,
    // Thump and Decay at 50 % the chain is exactly CabSim's: HP 75 Hz, +3 dB at 120 Hz, -3.5 dB at 450 Hz,
    // +4 dB at 2.3 kHz, low-passes at 5 and 6.5 kHz. No comb, no trim; its microphone is handled in setBank().
    { 120.0, 1.4, 3.0,   1.0, 0.0, 1.0, 0.0, 1000.0,   0.0,
      { { peak, 450.0, 1.0, -3.5 }, { peak, 2300.0, 1.3, 4.0 }, { lowPass, 5000.0, 0.6, 0.0 }, { lowPass, 6500.0, 0.9, 0.0 } } },
};

/** A microphone in front of the speaker: shelves and peaks only, so a response can be undone exactly
    (the 412 Classic needs that). trimDb keeps the loudness the same when the microphone is changed. */
struct MicDef
{
    double trimDb;
    Stage stages[maxMicStages];
};

inline const MicDef micDefs[numMics] =
{
    // 57 On Xs: Shure SM57 on axis. Thin lows, a rise from 3 kHz into the presence peak near 6 kHz.
    { 0.0, { { lowShelf, 160.0, 0.0, -2.5 }, { peak, 3200.0, 0.7, 1.5 }, { peak, 5800.0, 1.3, 4.5 } } },
    // 57 Off Xs: the SM57 turned away from the cone: most of the presence peak is gone, the top is dull and
    // there is a cancellation dip at 2.6 kHz.
    { 0.6, { { lowShelf, 160.0, 0.0, -1.0 }, { peak, 2600.0, 1.5, -2.0 }, { peak, 5000.0, 1.0, 1.5 }, { highShelf, 3800.0, 0.0, -5.0 } } },
    // 409 Dyn: Sennheiser MD 409. Warm low mids without deep bass, a smooth presence lift at 3.6 kHz, a soft top.
    { -1.1, { { lowShelf, 100.0, 0.0, -1.0 }, { peak, 300.0, 0.7, 2.0 }, { peak, 3600.0, 1.0, 3.5 }, { highShelf, 7500.0, 0.0, -3.0 } } },
    // 421 Dyn: Sennheiser MD 421. Scooped clarity: solid lows, a dip at 420 Hz, a strong peak at 4.2 kHz, open top.
    { 0.0, { { lowShelf, 110.0, 0.0, 2.0 }, { peak, 420.0, 0.8, -3.5 }, { peak, 4200.0, 1.2, 5.0 }, { highShelf, 8500.0, 0.0, 1.0 } } },
    // 4038 Rbn: Coles 4038 ribbon. A figure-8's big proximity bass, no presence peak, the darkest top.
    { -1.8, { { lowShelf, 190.0, 0.0, 5.0 }, { peak, 3000.0, 0.8, -1.5 }, { highShelf, 4600.0, 0.0, -5.0 } } },
    // 121 Rbn: Royer R-121 ribbon. Proximity bass, a slight push at 2.9 kHz, a smooth top that falls less than the Coles'.
    { -1.6, { { lowShelf, 160.0, 0.0, 4.0 }, { peak, 2900.0, 0.8, 1.5 }, { highShelf, 6500.0, 0.0, -3.0 } } },
    // 67 Cond: Neumann U67. Nearly flat: slightly warm lows, a very broad +1 dB around 5 kHz, a soft top.
    { -0.4, { { lowShelf, 120.0, 0.0, 1.5 }, { peak, 5200.0, 0.6, 1.0 }, { highShelf, 10000.0, 0.0, -2.0 } } },
    // 87 Cond: Neumann U87. Flat lows and mids, a little forward at 3.6 kHz, a lifted, extended top.
    { -0.2, { { lowShelf, 90.0, 0.0, -1.0 }, { peak, 3600.0, 0.7, 1.5 }, { highShelf, 7500.0, 0.0, 3.5 } } },
    // 12 Dyn: AKG D12, the vintage kick-drum microphone. A bass-chamber bump at 85 Hz, scooped at 400 Hz,
    // presence at 3 kHz, a rolled-off top.
    { -1.4, { { peak, 85.0, 0.9, 5.0 }, { peak, 400.0, 0.8, -3.0 }, { peak, 3000.0, 1.0, 3.0 }, { highShelf, 7500.0, 0.0, -4.0 } } },
    // 112 Dyn: AKG D112. A bump at 100 Hz, the deepest mid scoop, a narrow, strong click peak at 4 kHz.
    { -0.7, { { peak, 100.0, 1.0, 4.0 }, { peak, 500.0, 0.7, -4.5 }, { peak, 4000.0, 1.6, 6.0 }, { highShelf, 9000.0, 0.0, -3.0 } } },
    // 20 Dyn: Electro-Voice RE20. Variable-D: no proximity boost (the leanest lows here), and the flattest
    // dynamic; a small lift at 7 kHz before the top falls away.
    { 0.7, { { lowShelf, 100.0, 0.0, -2.0 }, { peak, 380.0, 0.8, -1.5 }, { peak, 7000.0, 1.0, 2.0 }, { highShelf, 11000.0, 0.0, -3.0 } } },
    // 7 Dyn: Shure SM7B. Warm lows, a dip at 3.2 kHz, a presence lift at 6.5 kHz, a soft top.
    { -0.8, { { lowShelf, 150.0, 0.0, 3.0 }, { peak, 3200.0, 1.2, -2.0 }, { peak, 6500.0, 1.2, 2.5 }, { highShelf, 10000.0, 0.0, -4.0 } } },
    // 40 Dyn: Heil PR40. Deep, tight lows, flat mids, a broad rise around 5.5 kHz that stays open to the top.
    { -0.6, { { lowShelf, 80.0, 0.0, 2.5 }, { peak, 5500.0, 0.6, 3.5 }, { highShelf, 9000.0, 0.0, 2.0 } } },
    // 47 Cond: Neumann U47. Rich lows, slightly recessed mids, a broad upper-mid presence at 3.8 kHz, a smooth top.
    { -1.5, { { lowShelf, 150.0, 0.0, 4.0 }, { peak, 700.0, 0.7, -1.5 }, { peak, 3800.0, 0.8, 4.5 }, { highShelf, 12000.0, 0.0, -1.0 } } },
};

/** E.R.: six early reflections of a small room, alternating in polarity (so they add no bass) and darker than the direct sound. */
inline constexpr double roomTapMs[numRoomTaps]  = { 5.3, 9.1, 13.7, 19.9, 26.3, 34.1 };
inline constexpr float  roomTapGain[numRoomTaps] = { 0.42f, -0.36f, 0.30f, -0.24f, 0.19f, -0.15f };

/** Biquads below this frequency run first: they are the ones where float rounding matters (see CabFx). */
constexpr double lowStageHz = 500.0;

inline void setStage (Biquad& filter, double fs, const Stage& s, double gainSign)
{
    switch (s.type)
    {
        case highPass:  filter.setHighPass (fs, s.hz, s.q); break;
        case lowPass:   filter.setLowPass (fs, s.hz, s.q); break;
        case peak:      filter.setPeak (fs, s.hz, s.q, gainSign * s.db); break;
        case lowShelf:  filter.setLowShelf (fs, s.hz, gainSign * s.db); break;
        case highShelf: filter.setHighShelf (fs, s.hz, gainSign * s.db); break;
        default: break;
    }
}

/** Sets up the stages of `list` that lie below (low = true) or at and above lowStageHz, in their listed order. */
template <size_t size>
int addStages (Biquad* filters, int count, double fs, const Stage (&list)[size], bool low, double gainSign)
{
    for (const auto& s : list)
        if (s.type != unused && (s.hz < lowStageHz) == low)
            setStage (filters[count++], fs, s, gainSign);
    return count;
}

inline int powerOfTwoAbove (int n)
{
    int size = 64;
    while (size < n)
        size *= 2;
    return size;
}

/** A knob value gliding to its target in steps of one chunk. Doubles and an integer counter only, so that the
    JavaScript port arrives at exactly the same values: the coefficients of a biquad at 50 Hz turn on the last bit. */
struct KnobRamp
{
    explicit KnobRamp (double initial) noexcept : start (initial), target (initial), current (initial) {}

    void prepare (int lengthInChunks) noexcept { length = std::max (1, lengthInChunks); snap(); }
    void snap() noexcept                       { start = current = target; left = 0; }
    bool isMoving() const noexcept             { return left > 0; }

    void setTarget (double v) noexcept
    {
        if (v != target)
        {
            start = current;
            target = v;
            left = length;
        }
    }

    /** One chunk further. */
    double next() noexcept
    {
        if (left > 0)
        {
            --left;
            current = left == 0 ? target : target + (start - target) * ((double) left / (double) length);
        }
        return current;
    }

    double start, target, current;
    int length = 1, left = 0;
};
} // namespace cab_detail

//==============================================================================
/** The speaker cabinet and its microphone: the HD500X's 17 cabinets, the 4 model-pack cabinets and this
    project's original voicing ("412 Classic"), each through one of 14 microphones. Mono. Runs after the amp.

    What it models:  Low Cut (12 dB/octave high-pass, off at 20 Hz)  ->  the speaker's bass resonance (Res Level /
                     Thump / Decay)  ->  the cabinet's biquads  ->  the comb of a second speaker and of the back wave
                     ->  the microphone's biquads  ->  E.R. (six early reflections)  ->  level

    The order the filters run in is a different one, which gives the same response (they are all linear):
    every biquad below 500 Hz first (Low Cut, resonance, the cabinet's low stages, the microphone's low stages),
    then the rest. Float rounding only matters in those low biquads (a 40 Hz high-pass at 48 kHz has its poles
    a hair from z = 1), so the JavaScript port imitates float arithmetic exactly in that first part, where its
    input is still bit-identical to this one's, and runs the rest in doubles, five times faster.

    Knobs: Mic | E.R. | Low Cut | Res Level | Thump | Decay
      - Res Level scales the speaker's bass resonance peak and raises the overall level (0.03 dB per %);
        Thump scales the peak's height, Decay its Q (how long the cone rings on). Peak height in dB =
        the cabinet's own value x Res Level / 50 x Thump / 50, so Thump and Decay do nothing at Res Level 0.
      - the Mic switch crossfades over 30 ms, Low Cut fades in and out (at 20 Hz) over 30 ms; the other knobs
        glide over 50 ms. */
class CabFx
{
public:
    enum Variant { blackfaceDouble212 = 0, hiway412, superO6x9, fieldCoil112, tweed410, bfLux112, celest12H112, phdPorted212,
                   blueBell112, silverBell212, greenback412, blackback412, britT75412, uber412, treadV30412, xxlV30412,
                   flipTop115, jazzRivet212, smallTweed108, svBeast810, rhino410, classic412, numVariants };

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        using namespace cab_detail;
        fs = sampleRate;

        const int rampChunks = (int) std::lround (0.05 * fs / chunkSize);
        for (auto* ramp : { &lowCutHz, &resLevel, &thump, &decay })
            ramp->prepare (rampChunks);

        roomGain.reset (fs, 0.05);
        outGain.reset (fs, 0.05);
        fadeLength = std::max (1, (int) (0.03 * fs));

        combBuffer.assign ((size_t) powerOfTwoAbove ((int) (0.004 * fs) + 8), 0.0f);
        combMask = (int) combBuffer.size() - 1;
        roomBuffer.assign ((size_t) powerOfTwoAbove ((int) (0.04 * fs) + 8), 0.0f);
        roomMask = (int) roomBuffer.size() - 1;

        for (int k = 0; k < numRoomTaps; ++k)
            roomDelay[k] = std::max (1, (int) std::lround (roomTapMs[k] * 0.001 * fs));
        roomLowPass.setCutoff (fs, 4200.0);

        reset();
    }

    void reset()
    {
        for (auto* ramp : { &lowCutHz, &resLevel, &thump, &decay })
            ramp->snap();
        for (auto* s : { &roomGain, &outGain })
            s->setCurrentAndTarget (s->getTarget());
        lowCutFade = lowCutOn ? fadeLength : 0;

        micActive = micFading = micTarget;
        fadeLeft = 0;
        configure();

        lowCut.reset();
        resonance.reset();
        for (auto& s : stages)       s.reset();
        for (auto& s : bankA.stages) s.reset();
        for (auto& s : bankB.stages) s.reset();
        combLowPass.reset();
        roomLowPass.reset();
        std::fill (combBuffer.begin(), combBuffer.end(), 0.0f);
        std::fill (roomBuffer.begin(), roomBuffer.end(), 0.0f);
        combPos = roomPos = 0;
        lowCutRunning = lowCutOn;
    }

    void setModel (int newVariant) noexcept
    {
        newVariant = std::clamp (newVariant, 0, (int) numVariants - 1);
        modelChanged = modelChanged || newVariant != variant;
        variant = newVariant;
    }

    void setParameters (const float* k)
    {
        micTarget = std::clamp ((int) std::lround (k[0]), 0, cab_detail::numMics - 1);
        roomGain.setTarget (0.007f * std::clamp (k[1], 0.0f, 100.0f));

        const float cut = std::clamp (k[2], 20.0f, 500.0f);
        lowCutHz.setTarget (cut);
        lowCutOn = cut > 20.5f; // 20 Hz = off

        const float res = std::clamp (k[3], 0.0f, 100.0f);
        resLevel.setTarget (res);
        thump.setTarget (std::clamp (k[4], 0.0f, 100.0f));
        decay.setTarget (std::clamp (k[5], 0.0f, 100.0f));
        outGain.setTarget (dbToGain ((float) cab_detail::cabDefs[variant].trimDb + 0.03f * (res - 50.0f)));
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        using namespace cab_detail;

        for (int pos = 0; pos < numSamples; pos += chunkSize)
        {
            const int n = std::min (chunkSize, numSamples - pos);
            updateChunk();

            float* x = work;
            for (int i = 0; i < n; ++i)
                x[i] = 0.5f * (left[pos + i] + right[pos + i]);

            // ---- the low biquads: Low Cut ...
            if (lowCutRunning)
            {
                if (lowCutOn && lowCutFade >= fadeLength)
                {
                    run (lowCut, x, n);
                }
                else // fading in or out
                {
                    for (int i = 0; i < n; ++i)
                    {
                        lowCutFade = std::clamp (lowCutFade + (lowCutOn ? 1 : -1), 0, fadeLength);
                        const float cut = lowCut.process (x[i]);
                        x[i] += smoothStep ((float) lowCutFade / (float) fadeLength) * (cut - x[i]);
                    }
                    lowCutRunning = lowCutOn || lowCutFade > 0;
                }
            }

            // ... the speaker's bass resonance and the cabinet's low stages ...
            run (resonance, x, n);
            for (int s = 0; s < numLowStages; ++s)
                run (stages[s], x, n);

            // ... and the microphone (two of them while the Mic switch crossfades)
            if (fadeLeft > 0)
            {
                float* y = work2;
                for (int i = 0; i < n; ++i)
                    y[i] = x[i];

                runBank (bankA, x, n);
                runBank (bankB, y, n);

                for (int i = 0; i < n; ++i)
                {
                    const float t = std::min (1.0f, (float) (fadeLength - fadeLeft + i + 1) / (float) fadeLength);
                    x[i] += smoothStep (t) * (y[i] - x[i]);
                }

                fadeLeft -= n;
                if (fadeLeft <= 0)
                {
                    fadeLeft = 0;
                    std::swap (bankA, bankB);
                    micActive = micFading;
                }
            }
            else
            {
                runBank (bankA, x, n);
            }

            // ---- the rest of the cabinet
            for (int s = numLowStages; s < numStages; ++s)
                run (stages[s], x, n);

            // the second speaker and the back wave, arriving late
            if (combOn)
            {
                for (int i = 0; i < n; ++i)
                {
                    const float in = x[i];
                    const float late = combGain1 * combBuffer[(size_t) ((combPos - combDelay1) & combMask)]
                                     + combGain2 * combBuffer[(size_t) ((combPos - combDelay2) & combMask)];
                    combBuffer[(size_t) combPos] = in;
                    combPos = (combPos + 1) & combMask;
                    x[i] = in + combLowPass.lowPass (late);
                }
            }

            // E.R.
            if (roomGain.isSmoothing() || roomGain.getTarget() > 0.0f)
            {
                for (int i = 0; i < n; ++i)
                {
                    const float in = x[i];
                    roomBuffer[(size_t) roomPos] = in;
                    float sum = 0.0f;
                    for (int k = 0; k < numRoomTaps; ++k)
                        sum += roomTapGain[k] * roomBuffer[(size_t) ((roomPos - roomDelay[k]) & roomMask)];
                    roomPos = (roomPos + 1) & roomMask;
                    x[i] = in + roomGain.next() * roomLowPass.lowPass (sum);
                }
            }
            else
            {
                for (int i = 0; i < n; ++i)
                {
                    roomBuffer[(size_t) roomPos] = x[i];
                    roomPos = (roomPos + 1) & roomMask;
                }
            }

            for (int i = 0; i < n; ++i)
            {
                const float out = x[i] * outGain.next();
                left[pos + i] = out;
                right[pos + i] = out;
            }
        }
    }

private:
    /** One microphone: its low stages first, then the others, then its loudness trim. */
    struct Bank
    {
        Biquad stages[cab_detail::maxBankStages];
        int count = 0;
        float gain = 1.0f;
    };

    /** 0..1 -> 0..1 with no kink at either end: the shape of the Mic and Low Cut fades. */
    static float smoothStep (float t) noexcept { return t * t * (3.0f - 2.0f * t); }

    static void run (Biquad& filter, float* x, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            x[i] = filter.process (x[i]);
    }

    static void runBank (Bank& bank, float* x, int n) noexcept
    {
        for (int s = 0; s < bank.count; ++s)
            run (bank.stages[s], x, n);

        for (int i = 0; i < n; ++i)
            x[i] *= bank.gain;
    }

    /** The microphone's filters. On the 412 Classic, whose voicing already is a finished, mic'd sound, the
        microphones act relative to the 57 On Xs: that one is exactly flat (no filters at all), the others are
        their own response with the 57's taken out (the same shelves and peaks with the opposite gain). */
    void setBank (Bank& bank, int mic) const
    {
        using namespace cab_detail;
        const auto& m = micDefs[mic];
        const bool relative = variant == classic412;
        bank.count = 0;
        bank.gain = dbToGain ((float) m.trimDb);

        if (relative && mic == 0)
            return;

        for (const bool low : { true, false })
        {
            bank.count = addStages (bank.stages, bank.count, fs, m.stages, low, 1.0);
            if (relative)
                bank.count = addStages (bank.stages, bank.count, fs, micDefs[0].stages, low, -1.0);
        }
    }

    void setResonance (double res, double thumpAmount, double decayAmount)
    {
        const auto& c = cab_detail::cabDefs[variant];
        const double gainDb = std::min (cab_detail::maxResonanceDb, c.resDb * (res / 50.0) * (thumpAmount / 50.0));
        const double q = c.resQ * std::pow (2.0, (decayAmount - 50.0) * 0.03); // 0.35 to 2.8 times the cabinet's Q
        resonance.setPeak (fs, c.resHz, q, gainDb);
    }

    /** Every coefficient for the model and the current knob values (audio state is left alone). */
    void configure()
    {
        using namespace cab_detail;
        const auto& c = cabDefs[variant];

        numLowStages = addStages (stages, 0, fs, c.stages, true, 1.0);
        numStages = addStages (stages, numLowStages, fs, c.stages, false, 1.0);

        combOn = c.tap1Gain != 0.0 || c.tap2Gain != 0.0;
        combDelay1 = std::max (1, (int) std::lround (c.tap1Ms * 0.001 * fs));
        combDelay2 = std::max (1, (int) std::lround (c.tap2Ms * 0.001 * fs));
        combGain1 = (float) c.tap1Gain;
        combGain2 = (float) c.tap2Gain;
        combLowPass.setCutoff (fs, c.tapLowPassHz);

        lowCut.setHighPass (fs, lowCutHz.current, 0.707);
        setResonance (resLevel.current, thump.current, decay.current);
        setBank (bankA, micActive);
        if (fadeLeft > 0)
            setBank (bankB, micFading);
        modelChanged = false;
    }

    /** Once per chunk: lets the filters follow the knobs. */
    void updateChunk()
    {
        if (modelChanged)
            configure(); // a model picked without reset(): the slot always resets, so this is only a safety net

        if (lowCutHz.isMoving())
            lowCut.setHighPass (fs, lowCutHz.next(), 0.707);

        if (lowCutOn && ! lowCutRunning)
        {
            lowCut.reset();
            lowCutRunning = true;
        }

        if (resLevel.isMoving() || thump.isMoving() || decay.isMoving())
        {
            const double r = resLevel.next(), t = thump.next(), d = decay.next();
            setResonance (r, t, d);
        }

        if (fadeLeft <= 0 && micTarget != micActive)
        {
            micFading = micTarget;
            setBank (bankB, micFading);
            for (auto& s : bankB.stages)
                s.reset();
            fadeLeft = fadeLength;
        }
    }

    double fs = 48000.0;
    int variant = classic412, micTarget = 0, micActive = 0, micFading = 0;
    bool modelChanged = true;

    // knob values, moving in steps of one chunk (the filter coefficients are computed from them)
    cab_detail::KnobRamp lowCutHz { 20.0 }, resLevel { 50.0 }, thump { 50.0 }, decay { 50.0 };
    // per sample
    Smoothed roomGain { 0.0f }, outGain { 1.0f };

    Biquad lowCut, resonance, stages[cab_detail::maxCabStages];
    int numLowStages = 0, numStages = 0;
    bool lowCutOn = false, lowCutRunning = false;
    int lowCutFade = 0; // 0 = out ... fadeLength = fully in

    bool combOn = false;
    std::vector<float> combBuffer;
    int combMask = 0, combPos = 0, combDelay1 = 1, combDelay2 = 1;
    float combGain1 = 0.0f, combGain2 = 0.0f;
    OnePole combLowPass;

    Bank bankA, bankB;
    int fadeLeft = 0, fadeLength = 1; // fadeLength: 30 ms, for the microphones and for Low Cut

    std::vector<float> roomBuffer;
    int roomMask = 0, roomPos = 0, roomDelay[cab_detail::numRoomTaps] {};
    OnePole roomLowPass;

    float work[cab_detail::chunkSize] {}, work2[cab_detail::chunkSize] {};
};

//==============================================================================
/** In the order of CabFx::Variant. Knobs: Mic, E.R., Low Cut (20 Hz = off), Res Level, Thump, Decay. */
inline std::vector<ModelInfo> cabModels()
{
    auto knobs = [] (int mic, float room, float lowCut, float res, float thump, float decay)
    {
        return std::vector<KnobSpec> { choice ("Mic", cabMicNames, cab_detail::numMics, mic), percent ("E.R.", room),
                                       freq ("Low Cut", 20.0f, 500.0f, lowCut, 100.0f), percent ("Res Level", res),
                                       percent ("Thump", thump), percent ("Decay", decay) };
    };
    auto cab = [&] (const char* key, const char* name, int variant, const char* basedOn, std::vector<KnobSpec> k)
    {
        return ModelInfo { key, name, Category::cab, Engine::cabFx, variant, basedOn, std::move (k) };
    };

    enum { m57On = 0, m57Off, m409, m421, m4038, m121, m67, m87, m12, m112, m20, m7, m40, m47 };

    return {
        cab ("cab_212_blackface",  "212 Blackface Double", CabFx::blackfaceDouble212, "Fender Blackface Twin Reverb combo, 2x12 Jensen",          knobs (m57On, 20.0f, 20.0f, 50.0f, 50.0f, 50.0f)),
        cab ("cab_412_hiway",      "412 Hiway",            CabFx::hiway412,           "Hiwatt cabinet, 4x12 Fane 12287",                          knobs (m421,  12.0f, 20.0f, 50.0f, 50.0f, 45.0f)),
        cab ("cab_6x9_super_o",    "6x9 Super O",          CabFx::superO6x9,          "Supro S6616 combo, 6x9 oval speaker",                      knobs (m121,  20.0f, 20.0f, 50.0f, 50.0f, 55.0f)),
        cab ("cab_112_field_coil", "112 Field Coil",       CabFx::fieldCoil112,       "Gibson EH-185 combo, 1x12 field-coil speaker",             knobs (m67,   20.0f, 20.0f, 50.0f, 50.0f, 55.0f)),
        cab ("cab_410_tweed",      "410 Tweed",            CabFx::tweed410,           "'59 Fender Tweed Bassman combo, 4x10 Jensen alnico",       knobs (m57On, 20.0f, 20.0f, 50.0f, 50.0f, 50.0f)),
        cab ("cab_112_bf_lux",     "112 BF 'Lux",          CabFx::bfLux112,           "Fender Blackface Deluxe Reverb combo, 1x12 Oxford 12K5-6", knobs (m57On, 20.0f, 20.0f, 50.0f, 50.0f, 50.0f)),
        cab ("cab_112_celest_12h", "112 Celest 12-H",      CabFx::celest12H112,       "Divided by 13 9/15 combo, 1x12 Celestion G12H Heritage",   knobs (m57On, 20.0f, 20.0f, 50.0f, 50.0f, 50.0f)),
        cab ("cab_212_phd_ported", "212 PhD Ported",       CabFx::phdPorted212,       "Dr. Z Z Best ported cabinet, G12H Heritage + Vintage 30",  knobs (m57On, 15.0f, 20.0f, 50.0f, 55.0f, 45.0f)),
        cab ("cab_112_blue_bell",  "112 Blue Bell",        CabFx::blueBell112,        "'61 Vox AC-15 combo, 1x12 Celestion Alnico Blue",          knobs (m57On, 20.0f, 20.0f, 50.0f, 50.0f, 50.0f)),
        cab ("cab_212_silver_bell", "212 Silver Bell",     CabFx::silverBell212,      "Vox AC-30 Top Boost, 2x12 Celestion Alnico Silver",        knobs (m409,  20.0f, 20.0f, 50.0f, 50.0f, 50.0f)),
        cab ("cab_412_greenback",  "412 Greenback 25",     CabFx::greenback412,       "Marshall cabinet, 4x12 Celestion G12M Greenback",          knobs (m57On, 12.0f, 20.0f, 50.0f, 50.0f, 50.0f)),
        cab ("cab_412_blackback",  "412 Blackback 30",     CabFx::blackback412,       "Marshall cabinet, 4x12 Celestion Rola G12H30 Blackback",   knobs (m57On, 12.0f, 20.0f, 50.0f, 50.0f, 50.0f)),
        cab ("cab_412_brit_t75",   "412 Brit T-75",        CabFx::britT75412,         "Marshall cabinet, 4x12 Celestion G12T-75",                 knobs (m57Off, 12.0f, 20.0f, 50.0f, 50.0f, 50.0f)),
        cab ("cab_412_uber",       "412 Uber",             CabFx::uber412,            "Bogner Uberschall cabinet, 4x12 G12T-75 + Vintage 30",     knobs (m57On, 10.0f, 20.0f, 50.0f, 50.0f, 40.0f)),
        cab ("cab_412_tread_v30",  "412 Tread V-30",       CabFx::treadV30412,        "Mesa/Boogie cabinet, 4x12 Celestion Vintage 30",           knobs (m57On, 10.0f, 20.0f, 50.0f, 50.0f, 40.0f)),
        cab ("cab_412_xxl_v30",    "412 XXL V-30",         CabFx::xxlV30412,          "Engl Pro cabinet, 4x12 Celestion Vintage 30",              knobs (m421,  10.0f, 20.0f, 50.0f, 50.0f, 40.0f)),
        cab ("cab_115_flip_top",   "115 Flip Top",         CabFx::flipTop115,         "Ampeg B-15 cabinet, 1x15 CTS (bass)",                      knobs (m20,   8.0f,  20.0f, 50.0f, 55.0f, 55.0f)),
        cab ("cab_212_jazz_rivet", "212 Jazz Rivet",       CabFx::jazzRivet212,       "Roland JC-120 cabinet, 2x12",                              knobs (m87,   20.0f, 20.0f, 50.0f, 50.0f, 50.0f)),
        cab ("cab_108_small_tweed", "108 Small Tweed",     CabFx::smallTweed108,      "Fender tweed Champ cabinet, 1x8",                          knobs (m121,  20.0f, 20.0f, 50.0f, 50.0f, 55.0f)),
        cab ("cab_810_sv_beast",   "810 SV Beast",         CabFx::svBeast810,         "Ampeg SVT 8x10 cabinet (bass)",                            knobs (m421,  8.0f,  20.0f, 50.0f, 50.0f, 45.0f)),
        cab ("cab_410_rhino",      "410 Rhino",            CabFx::rhino410,           "Ampeg SVT-410HLF cabinet, 4x10 + horn (bass)",             knobs (m40,   8.0f,  20.0f, 50.0f, 50.0f, 50.0f)),
        cab ("cab_412_classic",    "412 Classic",          CabFx::classic412,         "this project's original cab voicing",                      knobs (m57On, 0.0f,  75.0f, 50.0f, 50.0f, 50.0f)),
    };
}

} // namespace fx
