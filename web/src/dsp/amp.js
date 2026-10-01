// The HD500X's amp models (Source/DSP/fx/Amp.h): the 30 stock amps and the 20 model-pack amps, each with its
// "Pre" (preamp only) version on the Power Amp knob. Mono; the output is the amp's output signal (the cabinet
// and microphone are a separate engine). Ported line for line from the C++; see Amp.h for the design notes.
import { PI, clamp, f32, Smoothed, Oversampler4x } from "./core.js";
import { CATEGORY, ENGINE, model, percent, choice } from "./modeltypes.js";

const MAX_STAGES = 4, NUM_KNOBS = 12, CHUNK = 32;

// One preamp tube stage: [gain, bias, limPos, limNeg, pol, hpHz, lpHz, shelfK, shelfHz] (floats, as in C++)
const stage = (...v) => Float32Array.from(v);
const tri = (gain, hp, lp, shelfK = 1, shelfHz = 100) => stage(gain, 0.22, 1.0, 1.7, -1, hp, lp, shelfK, shelfHz);
const cold = (gain, hp, lp, shelfK = 1, shelfHz = 100) => stage(gain, -0.22, 2.2, 0.55, -1, hp, lp, shelfK, shelfHz);
const cf = (hp, lp) => stage(1, 0, 0.75, 4, 1, hp, lp, 1, 100);
const pent = (gain, hp, lp, shelfK = 1, shelfHz = 100) => stage(gain, 0.08, 0.9, 1.05, -1, hp, lp, shelfK, shelfHz);
const octal = (gain, hp, lp, shelfK = 1, shelfHz = 100) => stage(gain, 0.15, 1.6, 2.4, -1, hp, lp, shelfK, shelfHz);
const ss = (gain, hp, lp, shelfK = 1, shelfHz = 100) => stage(gain, 0, 1, 1, 1, hp, lp, shelfK, shelfHz);
const rect = (gain, hp, lp) => stage(gain, 0, 0.12, 3, -1, hp, lp, 1, 100);
const noStage = stage(1, 0, 1, 1, 1, 10, 20000, 1, 100);

const stackBlackface = 0, stackBassman = 1, stackJtm45 = 2, stackPlexi = 3, stackVox = 4, stackRecto = 5, stackSoldano = 6,
      stackHiwatt = 7, stackEngl = 8, stackOrange = 9, stackPete = 10;
const eqGeneric = 0, eqSupro = 1, eqGibson = 2, eqVox = 3, eqDivide = 4, eqFlipTop = 5, eqRoute = 6, eqSvt = 7, eqGk = 8,
      eqAcoustic = 9, eqChamp = 10;
const toneStack = 0, toneEq = 1;
const midNormal = 0, midCut = 1, midTone = 2, midDivide = 3;
const flagParallel = 1, flagInteract = 2;

// <the tables of Amp.h, copied value for value: stacks, eqs, specs, trims (keep the two files identical)>
const STACKS = [
  [ 250.0,  250.0, 10.0, 100.0, 0.25, 100.0, 47.0 ], // Fender AB763 (Twin / Deluxe Reverb)
  [ 250.0, 1000.0, 25.0,  56.0, 0.25,  20.0, 20.0 ], // Fender 5F6-A Bassman
  [ 250.0, 1000.0, 25.0,  56.0, 0.27,  22.0, 22.0 ], // Marshall JTM45 (the Bassman's, British parts)
  [ 220.0, 1000.0, 22.0,  33.0, 0.47,  22.0, 22.0 ], // Marshall 1959 / 2204 / Park 75
  [ 1000.0, 1000.0, 10.0, 100.0, 0.05, 22.0, 22.0 ], // Vox AC30 Top Boost (no middle control)
  [ 250.0,  250.0, 25.0,  47.0, 0.50,  20.0, 20.0 ], // Mesa Dual Rectifier
  [ 250.0, 1000.0, 25.0,  47.0, 0.47,  22.0, 22.0 ], // Soldano SLO / Peavey 5150 / Bogner
  [ 250.0, 1000.0, 22.0,  68.0, 0.47,  10.0, 47.0 ], // Hiwatt DR103: only half the blackface scoop, lower down
  [ 250.0, 1000.0, 25.0,  47.0, 0.47,  10.0, 33.0 ], // Engl: a shallow dip lower down, strong mid control
  [ 250.0,  250.0, 22.0,  68.0, 0.68,  47.0, 22.0 ], // Orange OR80
  [ 250.0,  250.0, 25.0, 100.0, 0.25, 100.0, 22.0 ], // Black Panel Pete: blackface with more mids
].map((r) => Float32Array.from(r));

const EQS = [
  [ 120.0, 10.0, 650.0, 0.7,  8.0, 3000.0, 10.0 ], // generic (the amps with a passive stack only name it)
  [ 120.0,  8.0, 650.0, 0.7,  0.0, 3000.0,  8.0 ], // Supro: Mid is the Tone knob
  [ 100.0, 10.0, 500.0, 0.6,  6.0, 2200.0, 10.0 ], // Gibson EH-185
  [ 110.0,  9.0, 800.0, 0.7,  0.0, 2800.0, 10.0 ], // Vox without Top Boost: Mid is the Cut
  [ 120.0,  0.0, 650.0, 0.7,  0.0, 3000.0,  0.0 ], // Divide 9/15: Tone and Cut only
  [  70.0, 14.0, 400.0, 0.7,  8.0, 2200.0, 12.0 ], // Ampeg B-15 Baxandall
  [ 120.0, 10.0, 700.0, 0.7,  6.0, 2500.0, 10.0 ], // Dr. Z Route 66 bass / treble
  [  50.0, 12.0, 800.0, 1.2, 12.0, 4000.0, 14.0 ], // Ampeg SVT: Baxandall + inductor mid at 800 Hz
  [  60.0, 12.0, 500.0, 0.8, 12.0, 7000.0, 12.0 ], // Gallien-Krueger 800RB active EQ
  [ 100.0, 10.0, 800.0, 0.8, 10.0, 6000.0, 12.0 ], // Line 6 Acoustic
  [ 120.0,  8.0, 700.0, 0.7,  6.0, 3000.0,  8.0 ], // Fender Champ (no tone controls on the original)
].map((r) => Float32Array.from(r));

const SPEC_ROWS = [
  // '65 Twin Reverb, Normal: 12AX7 -> blackface stack -> volume -> 12AX7 -> 4x6L6 with lots of feedback, silicon rectifier.
  // Very high headroom; the scoop at 400-500 Hz and the glassy top come from the stack.
  [ "blackface_double_normal", "Blackface Double Normal", "'65 Fender Twin Reverb, Normal channel",
    2, [ tri(0.9, 10.0, 16000.0), tri(30.0, 8.0, 14000.0), noStage, noStage ],
    1, 46.0, 0.0, 1, toneStack, stackBlackface, eqGeneric, midNormal, 0,
    1.6, 2.5, 1.0, 3000.0, 6.0, 0.93, 0.10, 40.0, 11000.0, 95.0, 1.0,
    [ 45, 45, 55, 60, 35, 50, 100, 30, 50, 50, 40 ] ],
  // Twin Reverb, Vibrato: bright cap on the volume and a third 12AX7 after the reverb mixer, which clips first.
  [ "blackface_double_vibrato", "Blackface Double Vibrato", "'65 Fender Twin Reverb, Vibrato channel",
    3, [ tri(0.9, 10.0, 16000.0), tri(30.0, 8.0, 14000.0), tri(1.7, 8.0, 12000.0), noStage ],
    1, 46.0, 2650.0, 1, toneStack, stackBlackface, eqGeneric, midNormal, 0,
    1.3, 2.5, 1.0, 3000.0, 6.0, 0.93, 0.10, 40.0, 11000.0, 95.0, 1.0,
    [ 45, 45, 55, 55, 35, 50, 100, 30, 50, 50, 40 ] ],
  // Hiwatt DR103: stiff, linear, loud. Flat-ish stack after the second stage, 4xEL34 with heavy feedback.
  [ "hiway_100", "Hiway 100", "Hiwatt Custom 100 (DR103)",
    3, [ tri(0.9, 15.0, 18000.0), tri(22.0, 20.0, 16000.0), tri(2.2, 20.0, 16000.0), noStage ],
    1, 44.0, 0.0, 2, toneStack, stackHiwatt, eqGeneric, midNormal, 0,
    1.8, 2.5, 1.2, 2500.0, 6.0, 0.95, 0.08, 35.0, 14000.0, 95.0, 0.6,
    [ 50, 50, 55, 55, 45, 50, 75, 25, 50, 50, 35 ] ],
  // Supro S6616: two triodes into one single-ended 6V6, no feedback, small transformer. One Tone knob (on Mid).
  [ "super_o", "Super O", "'60s Supro S6616",
    2, [ tri(1.1, 30.0, 9000.0), tri(13.0, 40.0, 7000.0), noStage, noStage ],
    1, 40.0, 0.0, 2, toneEq, 0, eqSupro, midTone, 0,
    3.0, 2.0, 0.0, 3500.0, 5.0, 0.0, 0.30, 90.0, 6500.0, 110.0, 2.5,
    [ 55, 50, 60, 50, 40, 50, 100, 55, 50, 85, 50 ] ],
  // Gibson EH-185 (1939): octal preamp, 2x6L6 without feedback, dark and soft.
  [ "gibtone_185", "Gibtone 185", "Gibson EH-185",
    2, [ octal(0.7, 25.0, 8000.0), octal(7.0, 30.0, 6500.0), noStage, noStage ],
    1, 40.0, 0.0, 1, toneEq, 0, eqGibson, midNormal, 0,
    2.5, 2.0, 0.0, 3000.0, 5.0, 0.88, 0.35, 70.0, 5500.0, 85.0, 2.5,
    [ 60, 55, 50, 55, 40, 50, 100, 55, 50, 60, 45 ] ],
  // '59 Bassman 5F6-A, Normal: 12AY7 -> volume -> 12AX7 -> cathode follower -> stack -> long-tail inverter -> 2x5881,
  // GZ34 rectifier, a real Presence control in the feedback loop.
  [ "tweed_b_man_normal", "Tweed B-Man Normal", "'59 Fender Tweed Bassman, Normal channel",
    3, [ octal(0.7, 12.0, 15000.0), tri(22.0, 15.0, 12000.0), cf(10.0, 14000.0), noStage ],
    1, 42.0, 0.0, 3, toneStack, stackBassman, eqGeneric, midNormal, 0,
    3.5, 2.5, 0.5, 1500.0, 8.0, 0.92, 0.30, 45.0, 9000.0, 100.0, 1.5,
    [ 55, 45, 55, 55, 45, 50, 100, 50, 50, 50, 45 ] ],
  // Bassman, Bright: the bright cap across the volume pot and the leaner half of the first tube.
  [ "tweed_b_man_bright", "Tweed B-Man Bright", "'59 Fender Tweed Bassman, Bright channel",
    3, [ octal(0.7, 12.0, 16000.0, 0.75, 300.0), tri(22.0, 40.0, 13000.0), cf(10.0, 14000.0), noStage ],
    1, 42.0, 2500.0, 3, toneStack, stackBassman, eqGeneric, midNormal, 0,
    3.5, 2.5, 0.5, 1500.0, 8.0, 0.92, 0.30, 45.0, 9000.0, 100.0, 1.5,
    [ 55, 50, 55, 50, 45, 50, 100, 50, 50, 50, 45 ] ],
  // Deluxe Reverb, Normal: the Twin's preamp into 2x6V6 (22 W) with a GZ34: breaks up early and sags.
  [ "blackface_lux_normal", "Blackface 'Lux Normal", "Fender Blackface Deluxe Reverb, Normal channel",
    2, [ tri(0.9, 10.0, 15000.0), tri(30.0, 10.0, 13000.0), noStage, noStage ],
    1, 46.0, 0.0, 1, toneStack, stackBlackface, eqGeneric, midNormal, 0,
    4.2, 2.2, 0.7, 3000.0, 6.0, 0.92, 0.35, 60.0, 8000.0, 100.0, 1.5,
    [ 42, 50, 55, 55, 35, 50, 100, 50, 50, 50, 45 ] ],
  // Deluxe Reverb, Vibrato: the fixed 47 pF bright cap and the third stage.
  [ "blackface_lux_vibrato", "Blackface 'Lux Vibrato", "Fender Blackface Deluxe Reverb, Vibrato channel",
    3, [ tri(0.9, 10.0, 15000.0), tri(30.0, 10.0, 13000.0), tri(1.7, 10.0, 11000.0), noStage ],
    1, 46.0, 3386.0, 1, toneStack, stackBlackface, eqGeneric, midNormal, 0,
    3.4, 2.2, 0.7, 3000.0, 6.0, 0.92, 0.35, 60.0, 8000.0, 100.0, 1.5,
    [ 42, 50, 55, 55, 35, 50, 100, 50, 50, 50, 45 ] ],
  // Divided by 13 JRT 9/15: two 5879 pentode channels mixed (clean level on Drive, dirty drive on Bass),
  // one Tone (Mid) and a Cut (Treble), cathode-biased output pair without feedback.
  [ "divide_9_15", "Divide 9/15", "Divided by 13 JRT 9/15",
    2, [ pent(5.0, 20.0, 13000.0), pent(24.0, 45.0, 10000.0, 0.7, 250.0), noStage, noStage ],
    0, 36.0, 0.0, 2, toneEq, 0, eqDivide, midDivide, flagParallel,
    3.0, 2.0, 0.0, 3200.0, 5.0, 0.90, 0.30, 65.0, 9000.0, 100.0, 2.0,
    [ 55, 45, 60, 70, 40, 50, 100, 50, 50, 70, 50 ] ],
  // Dr. Z Route 66: one EF86 pentode, bass / treble, 2xKT66 with an ultra-linear transformer and GZ34.
  [ "phd_motorway", "PhD Motorway", "Dr. Z Route 66",
    2, [ pent(2.2, 20.0, 12000.0), tri(4.5, 15.0, 12000.0), noStage, noStage ],
    1, 40.0, 0.0, 1, toneEq, 0, eqRoute, midNormal, 0,
    2.2, 2.5, 0.35, 3000.0, 5.0, 0.93, 0.28, 40.0, 13000.0, 95.0, 1.5,
    [ 55, 50, 50, 55, 45, 50, 100, 45, 50, 55, 45 ] ],
  // '61 Vox AC15, EF86 channel: pentode into the inverter, 2xEL84 cathode-biased, no feedback, EZ81. Cut on Mid.
  [ "class_a_15", "Class A-15", "'61 \"Fawn\" Vox AC-15",
    2, [ pent(2.0, 25.0, 11000.0), tri(4.0, 25.0, 12000.0), noStage, noStage ],
    1, 40.0, 0.0, 1, toneEq, 0, eqVox, midCut, 0,
    2.8, 2.0, 0.0, 3500.0, 5.0, 0.90, 0.40, 70.0, 9000.0, 100.0, 2.5,
    [ 55, 50, 70, 55, 45, 50, 100, 60, 50, 80, 50 ] ],
  // Vox AC30 Top Boost: 12AX7 -> volume -> 12AX7 -> cathode follower -> Top Boost treble / bass -> 4xEL84,
  // cathode-biased, no feedback, GZ34. Cut on Mid.
  [ "class_a_30_tb", "Class A-30 TB", "Vox AC-30 \"Top Boost\"",
    3, [ tri(1.0, 25.0, 14000.0), tri(22.0, 30.0, 13000.0), cf(12.0, 14000.0), noStage ],
    1, 42.0, 0.0, 3, toneStack, stackVox, eqGeneric, midCut, 0,
    3.0, 2.0, 0.0, 3500.0, 5.0, 0.90, 0.35, 60.0, 10000.0, 95.0, 2.5,
    [ 55, 45, 70, 60, 45, 50, 100, 55, 50, 75, 50 ] ],
  // '65 JTM45, Normal: the Bassman circuit with ECC83s and KT66s: fat, saggy (GZ34), heavy feedback.
  [ "brit_j_45_normal", "Brit J-45 Normal", "'65 Marshall JTM-45 MkII, Normal channel",
    3, [ tri(0.9, 10.0, 12000.0), tri(24.0, 15.0, 10000.0), cf(10.0, 12000.0), noStage ],
    1, 42.0, 0.0, 3, toneStack, stackJtm45, eqGeneric, midNormal, 0,
    3.5, 2.5, 0.8, 1800.0, 8.0, 0.92, 0.40, 50.0, 8500.0, 100.0, 1.2,
    [ 60, 50, 55, 55, 45, 50, 100, 55, 50, 50, 50 ] ],
  // JTM45, Bright: bright cap on the volume and the treble-peaking mixer network.
  [ "brit_j_45_bright", "Brit J-45 Bright", "'65 Marshall JTM-45 MkII, Bright channel",
    3, [ tri(0.9, 10.0, 14000.0), tri(24.0, 40.0, 12000.0, 0.55, 700.0), cf(10.0, 12000.0), noStage ],
    1, 42.0, 2500.0, 3, toneStack, stackJtm45, eqGeneric, midNormal, 0,
    3.5, 2.5, 0.8, 1800.0, 8.0, 0.92, 0.40, 50.0, 8500.0, 100.0, 1.2,
    [ 60, 50, 55, 50, 45, 50, 100, 55, 50, 50, 50 ] ],
  // Marshall 1959 Super Lead, Normal: fully bypassed first stage, 4xEL34, moderate feedback, silicon rectifier.
  // Most of the crunch is the inverter and the output tubes.
  [ "plexi_lead_100_normal", "Plexi Lead 100 Normal", "'59 Marshall \"Plexi\" Super Lead 100, Normal channel",
    3, [ tri(0.9, 10.0, 13000.0), tri(28.0, 20.0, 11000.0), cf(10.0, 13000.0), noStage ],
    1, 42.0, 0.0, 3, toneStack, stackPlexi, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.5, 2000.0, 8.0, 0.93, 0.20, 50.0, 9000.0, 100.0, 1.5,
    [ 65, 50, 55, 55, 50, 50, 100, 40, 50, 50, 50 ] ],
  // Super Lead, Bright: 2.7k / 0.68 uF cathode, 2.2 nF coupling, the 5 nF bright cap and 470 pF across the mixer.
  [ "plexi_lead_100_bright", "Plexi Lead 100 Bright", "'59 Marshall \"Plexi\" Super Lead 100, Bright channel",
    3, [ tri(0.9, 60.0, 15000.0, 0.6, 90.0), tri(28.0, 30.0, 12000.0, 0.5, 720.0), cf(10.0, 13000.0), noStage ],
    1, 42.0, 100.0, 3, toneStack, stackPlexi, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.5, 2000.0, 8.0, 0.93, 0.20, 50.0, 9000.0, 100.0, 1.5,
    [ 60, 55, 55, 50, 45, 50, 100, 40, 50, 50, 50 ] ],
  // Park 75, Normal: a hotter plexi front end into KT88s: more gain, more headroom and tighter lows in the power amp.
  [ "brit_p_75_normal", "Brit P-75 Normal", "Park 75, Normal channel",
    3, [ tri(1.0, 10.0, 13000.0), tri(38.0, 20.0, 11000.0), cf(10.0, 13000.0), noStage ],
    1, 42.0, 0.0, 3, toneStack, stackPlexi, eqGeneric, midNormal, 0,
    3.5, 2.8, 0.6, 2200.0, 8.0, 0.94, 0.18, 40.0, 10000.0, 95.0, 1.2,
    [ 60, 50, 55, 55, 50, 50, 100, 35, 50, 50, 45 ] ],
  // Park 75, Bright.
  [ "brit_p_75_bright", "Brit P-75 Bright", "Park 75, Bright channel",
    3, [ tri(1.0, 60.0, 15000.0, 0.6, 90.0), tri(38.0, 30.0, 12000.0, 0.55, 720.0), cf(10.0, 13000.0), noStage ],
    1, 42.0, 150.0, 3, toneStack, stackPlexi, eqGeneric, midNormal, 0,
    3.5, 2.8, 0.6, 2200.0, 8.0, 0.94, 0.18, 40.0, 10000.0, 95.0, 1.2,
    [ 55, 55, 55, 50, 45, 50, 100, 35, 50, 50, 45 ] ],
  // JCM800 2204: gain pot (470 pF bright) -> cold clipper -> 470k / 470k with 470 pF -> third stage -> follower ->
  // stack -> master -> 2xEL34. The distortion is the preamp's: tight, upper-mid bark.
  [ "brit_j_800", "Brit J-800", "Marshall JCM-800 (2204)",
    4, [ tri(1.0, 15.0, 14000.0, 0.6, 90.0), cold(30.0, 30.0, 12000.0), tri(6.0, 40.0, 9000.0, 0.5, 720.0), cf(10.0, 12000.0) ],
    1, 48.0, 339.0, 4, toneStack, stackPlexi, eqGeneric, midNormal, 0,
    5.0, 2.5, 0.6, 2000.0, 8.0, 0.93, 0.20, 50.0, 9000.0, 100.0, 1.5,
    [ 60, 50, 60, 55, 45, 50, 45, 40, 50, 50, 50 ] ],
  // Bogner Uberschall: four stages of gain with the bass cut before the clipping and put back by the power amp's
  // deep resonance: huge but tight low end, dark smooth top.
  [ "bomber_uber", "Bomber Uber", "2002 Bogner Uberschall",
    4, [ tri(1.2, 20.0, 14000.0, 0.45, 170.0), tri(34.0, 130.0, 8000.0), cold(12.0, 80.0, 7000.0), tri(7.0, 40.0, 5500.0) ],
    1, 52.0, 0.0, 4, toneStack, stackSoldano, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.9, 2500.0, 8.0, 0.93, 0.20, 35.0, 7000.0, 80.0, 4.5,
    [ 55, 55, 45, 55, 40, 50, 45, 35, 50, 50, 50 ] ],
  // Mesa Dual Rectifier (modern): cascaded gain into the stack, 6L6s with almost no feedback (loose, growling
  // lows and a raw top), tube-rectifier sag.
  [ "treadplate", "Treadplate", "Mesa/Boogie Dual Rectifier",
    4, [ tri(1.2, 20.0, 15000.0, 0.7, 100.0), tri(28.0, 60.0, 12000.0), cold(10.0, 40.0, 11000.0), tri(6.0, 30.0, 9000.0) ],
    1, 52.0, 0.0, 4, toneStack, stackRecto, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.15, 4000.0, 7.0, 0.93, 0.30, 38.0, 12000.0, 90.0, 3.5,
    [ 55, 55, 40, 65, 55, 50, 45, 50, 50, 50, 50 ] ],
  // Engl Fireball 100: very tight (high coupling corners, lean first stage), mid-forward, stiff 6L6 power amp.
  [ "angel_f_ball", "Angel F-Ball", "Engl Fireball 100",
    4, [ tri(1.3, 25.0, 14000.0, 0.45, 200.0), tri(36.0, 160.0, 8500.0), cold(14.0, 110.0, 7500.0), tri(8.0, 60.0, 6000.0) ],
    1, 52.0, 0.0, 4, toneStack, stackEngl, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.8, 2800.0, 8.0, 0.94, 0.15, 45.0, 9500.0, 95.0, 2.5,
    [ 55, 50, 55, 55, 50, 50, 45, 30, 50, 50, 45 ] ],
  // Line 6 Elektrik: Line 6's own high-gain amp; Presence and Mid interact (Mid moves the presence corner).
  [ "line6_elektrik", "Line 6 Elektrik", "Line 6 original (high gain)",
    4, [ tri(1.2, 20.0, 14000.0, 0.5, 130.0), tri(38.0, 120.0, 9000.0), cold(14.0, 80.0, 8500.0), tri(9.0, 50.0, 7000.0) ],
    1, 52.0, 0.0, 4, toneStack, stackRecto, eqGeneric, midNormal, flagInteract,
    4.5, 2.5, 0.5, 2200.0, 9.0, 0.93, 0.25, 40.0, 10000.0, 90.0, 3.0,
    [ 60, 50, 50, 55, 50, 50, 45, 40, 50, 50, 50 ] ],
  // Soldano SLO-100, Normal channel, Clean: two stages and the follower, stiff 6L6 power amp.
  [ "solo_100_clean", "Solo 100 Clean", "'93 Soldano SLO-100, Normal channel (Clean)",
    3, [ tri(1.0, 15.0, 15000.0), tri(14.0, 20.0, 13000.0), cf(10.0, 14000.0), noStage ],
    1, 40.0, 0.0, 3, toneStack, stackSoldano, eqGeneric, midNormal, 0,
    2.5, 2.5, 0.8, 2500.0, 8.0, 0.93, 0.15, 40.0, 11000.0, 95.0, 2.0,
    [ 45, 50, 50, 55, 45, 50, 60, 35, 50, 50, 45 ] ],
  // SLO-100, Normal channel, Crunch: the same channel with its extra gain switched in.
  [ "solo_100_crunch", "Solo 100 Crunch", "'93 Soldano SLO-100, Normal channel (Crunch)",
    3, [ tri(1.0, 15.0, 15000.0, 0.7, 100.0), tri(30.0, 40.0, 11000.0), tri(3.5, 30.0, 10000.0), noStage ],
    1, 40.0, 0.0, 3, toneStack, stackSoldano, eqGeneric, midNormal, 0,
    3.5, 2.5, 0.8, 2500.0, 8.0, 0.93, 0.15, 40.0, 11000.0, 95.0, 2.0,
    [ 55, 50, 55, 55, 45, 50, 50, 35, 50, 50, 45 ] ],
  // SLO-100, Overdrive: the cascade everybody copied: smooth, singing, saturated, with the Depth-like low resonance.
  [ "solo_100_od", "Solo 100 OD", "'93 Soldano SLO-100, Overdrive channel",
    4, [ tri(1.2, 20.0, 14000.0, 0.55, 130.0), tri(45.0, 100.0, 10000.0), cold(13.0, 70.0, 9000.0, 0.6, 1000.0), tri(7.0, 40.0, 7500.0) ],
    1, 52.0, 0.0, 4, toneStack, stackSoldano, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.8, 2500.0, 8.0, 0.93, 0.15, 40.0, 10000.0, 95.0, 3.0,
    [ 55, 50, 55, 55, 50, 50, 45, 35, 50, 50, 50 ] ],
  // Line 6 Doom: a modded JCM800 preamp (more gain, full bass) into a Hiwatt power amp, with a lot of sag.
  [ "line6_doom", "Line 6 Doom", "Line 6 original (JCM800 preamp into a Hiwatt power amp)",
    4, [ tri(1.3, 10.0, 12000.0), cold(34.0, 25.0, 9000.0), tri(10.0, 25.0, 7000.0), cf(10.0, 10000.0) ],
    1, 48.0, 0.0, 4, toneStack, stackPlexi, eqGeneric, midNormal, 0,
    4.0, 2.5, 1.0, 2500.0, 6.0, 0.95, 0.50, 30.0, 8000.0, 80.0, 4.0,
    [ 70, 65, 45, 45, 35, 50, 60, 75, 50, 50, 60 ] ],
  // Line 6 Epic: the most gain of the stock amps, compressed so that it sustains at any playing level; smooth top.
  [ "line6_epic", "Line 6 Epic", "Line 6 original (high gain, endless sustain)",
    4, [ tri(1.4, 25.0, 14000.0, 0.5, 140.0), tri(45.0, 120.0, 8000.0), cold(18.0, 90.0, 7000.0), tri(12.0, 50.0, 5500.0) ],
    1, 52.0, 0.0, 4, toneStack, stackSoldano, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.6, 2500.0, 8.0, 0.93, 0.25, 40.0, 7000.0, 90.0, 3.0,
    [ 65, 50, 55, 50, 45, 50, 45, 45, 50, 50, 55 ] ],
  // Ampeg B-15NF Portaflex (bass): octal preamp with a Baxandall bass / treble, 2x6L6 at 25-30 W, 5AR4: round, full lows.
  [ "flip_top", "Flip Top", "Ampeg B-15NF Portaflex",
    2, [ octal(0.8, 8.0, 9000.0), octal(6.0, 8.0, 8000.0), noStage, noStage ],
    1, 40.0, 0.0, 1, toneEq, 0, eqFlipTop, midNormal, 0,
    2.2, 2.2, 0.6, 2500.0, 5.0, 0.92, 0.30, 25.0, 6000.0, 55.0, 3.0,
    [ 45, 55, 50, 45, 35, 50, 100, 45, 50, 55, 45 ] ],
  // ---- HD model packs: Metal ----
  // Peavey 5150 (block logo), lead channel: five cascaded stages' worth of gain, tight before the clipping, cold-biased
  // 6L6s (the default Bias is low: a little crossover grit) and a strong resonance.
  [ "pv_panama", "PV Panama", "Peavey 5150",
    4, [ tri(1.2, 25.0, 14000.0, 0.5, 130.0), tri(40.0, 130.0, 9000.0), cold(15.0, 90.0, 8000.0), tri(9.0, 50.0, 6500.0) ],
    1, 52.0, 0.0, 4, toneStack, stackSoldano, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.7, 2200.0, 9.0, 0.93, 0.12, 40.0, 9500.0, 100.0, 4.0,
    [ 55, 55, 45, 55, 55, 50, 45, 30, 50, 38, 50 ] ],
  // Bogner Shiva, lead channel: medium-high gain, round and smooth, less compressed than the Uberschall.
  [ "mahadeva", "Mahadeva", "Bogner Shiva",
    4, [ tri(1.0, 15.0, 14000.0, 0.7, 100.0), tri(28.0, 60.0, 10000.0), tri(8.0, 40.0, 8000.0), cf(10.0, 12000.0) ],
    1, 46.0, 0.0, 4, toneStack, stackSoldano, eqGeneric, midNormal, 0,
    4.0, 2.5, 0.6, 2500.0, 7.0, 0.93, 0.20, 45.0, 10000.0, 95.0, 2.0,
    [ 55, 50, 55, 55, 45, 50, 50, 40, 50, 50, 45 ] ],
  // The "remastered" JCM800 2204: the Brit J-800 circuit, a little hotter and tighter.
  [ "brit_2204", "Brit 2204", "Marshall JCM800 (2204), remastered",
    4, [ tri(1.0, 15.0, 15000.0, 0.6, 90.0), cold(34.0, 40.0, 12000.0), tri(7.0, 50.0, 9500.0, 0.5, 720.0), cf(10.0, 13000.0) ],
    1, 48.0, 339.0, 4, toneStack, stackPlexi, eqGeneric, midNormal, 0,
    5.5, 2.5, 0.55, 2000.0, 8.0, 0.93, 0.22, 50.0, 9500.0, 100.0, 1.8,
    [ 65, 50, 60, 55, 50, 50, 50, 40, 50, 50, 50 ] ],
  // Line 6 Insane: as much gain as the cascade will take, mid-heavy.
  [ "line6_insane", "Line 6 Insane", "Line 6 original (maximum gain)",
    4, [ tri(1.5, 25.0, 14000.0, 0.5, 150.0), tri(60.0, 140.0, 8500.0), cold(22.0, 100.0, 7500.0), tri(14.0, 60.0, 6000.0) ],
    1, 52.0, 0.0, 4, toneStack, stackRecto, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.5, 2500.0, 8.0, 0.93, 0.20, 40.0, 9000.0, 90.0, 3.0,
    [ 70, 50, 60, 50, 45, 50, 45, 40, 50, 50, 55 ] ],
  // Line 6 Big Bottom: a high-gain amp voiced for low end: loose coupling, little feedback, a big resonance at 75 Hz.
  [ "line6_big_bottom", "Line 6 Big Bottom", "Line 6 original (bass-heavy high gain)",
    4, [ tri(1.2, 12.0, 14000.0, 0.85, 80.0), tri(34.0, 45.0, 9000.0), cold(12.0, 35.0, 8000.0), tri(7.0, 25.0, 6500.0) ],
    1, 52.0, 0.0, 4, toneStack, stackRecto, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.3, 3000.0, 7.0, 0.93, 0.30, 28.0, 9500.0, 75.0, 6.0,
    [ 55, 65, 35, 55, 45, 50, 45, 45, 50, 50, 50 ] ],
  // Line 6 Variac'ed Plexi: a Super Lead run on a lowered mains voltage: the power amp gives up early, sags and browns out.
  [ "line6_variaced_plexi", "Line 6 Variac'ed Plexi", "Line 6 original (variac-sagged Plexi)",
    3, [ tri(1.0, 30.0, 13000.0, 0.7, 90.0), tri(34.0, 25.0, 10500.0, 0.6, 720.0), cf(10.0, 12000.0), noStage ],
    1, 42.0, 200.0, 3, toneStack, stackPlexi, eqGeneric, midNormal, 0,
    8.0, 2.0, 0.4, 2000.0, 8.0, 0.93, 0.45, 50.0, 8000.0, 100.0, 2.0,
    [ 70, 50, 60, 55, 45, 50, 100, 65, 50, 60, 60 ] ],
  // Line 6 Purge: tight and mid-forward (high coupling corners, a wide mid control, stiff power amp).
  [ "line6_purge", "Line 6 Purge", "Line 6 original (tight, mid-forward high gain)",
    4, [ tri(1.3, 25.0, 14000.0, 0.45, 220.0), tri(38.0, 180.0, 9500.0), cold(14.0, 120.0, 8500.0), tri(9.0, 70.0, 7500.0) ],
    1, 52.0, 0.0, 4, toneStack, stackEngl, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.7, 3000.0, 9.0, 0.94, 0.12, 45.0, 10000.0, 100.0, 2.0,
    [ 60, 45, 65, 55, 55, 50, 45, 30, 50, 50, 45 ] ],
  // Line 6 Aggro: scooped and bright, hardly any feedback: a raw edge on top of a deep low end.
  [ "line6_aggro", "Line 6 Aggro", "Line 6 original (scooped, aggressive high gain)",
    4, [ tri(1.2, 20.0, 15000.0, 0.5, 140.0), tri(36.0, 110.0, 11000.0), cold(14.0, 80.0, 10000.0), tri(8.0, 45.0, 8500.0) ],
    1, 52.0, 0.0, 4, toneStack, stackRecto, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.2, 3800.0, 8.0, 0.93, 0.20, 38.0, 11000.0, 85.0, 4.0,
    [ 60, 60, 35, 65, 55, 50, 45, 40, 50, 50, 50 ] ],
  // Line 6 Smash: thick and fuzzy: pentode-like stages that clip hard and evenly, a dark Orange-style stack, loose power amp.
  [ "line6_smash", "Line 6 Smash", "Line 6 original (thick, fuzzy high gain)",
    4, [ tri(1.4, 15.0, 12000.0), pent(30.0, 50.0, 8000.0), pent(10.0, 40.0, 6500.0), cf(10.0, 10000.0) ],
    1, 48.0, 0.0, 4, toneStack, stackOrange, eqGeneric, midNormal, 0,
    3.5, 2.2, 0.3, 2500.0, 7.0, 0.92, 0.35, 40.0, 8000.0, 90.0, 3.0,
    [ 65, 55, 55, 50, 40, 50, 55, 55, 50, 50, 55 ] ],
  // Line 6 Octone: a high-gain amp whose second stage works as a half-wave rectifier, which puts an octave overtone on the notes.
  [ "line6_octone", "Line 6 Octone", "Line 6 original (high gain with an octave overtone)",
    4, [ tri(1.3, 30.0, 12000.0, 0.6, 200.0), rect(30.0, 120.0, 9000.0), cold(12.0, 80.0, 8000.0), tri(7.0, 50.0, 7000.0) ],
    1, 52.0, 0.0, 4, toneStack, stackSoldano, eqGeneric, midNormal, 0,
    4.5, 2.5, 0.6, 2500.0, 8.0, 0.93, 0.20, 40.0, 9500.0, 95.0, 2.5,
    [ 60, 45, 60, 55, 50, 50, 45, 35, 50, 50, 50 ] ],
  // ---- Vintage ----
  // Roland JC-120 (the amp, without its chorus): solid state, a Fender-like passive stack, a stiff power amp that stays
  // clean until it clips hard. No sag, hardly any hum.
  [ "jazz_rivet", "Jazz Rivet", "Roland JC-120",
    2, [ ss(0.8, 15.0, 20000.0), ss(9.0, 10.0, 18000.0), noStage, noStage ],
    1, 46.0, 0.0, 1, toneStack, stackBlackface, eqGeneric, midNormal, 0,
    2.0, 3.0, 1.5, 4000.0, 5.0, 1.0, 0.02, 20.0, 18000.0, 95.0, 0.3,
    [ 45, 50, 50, 55, 40, 50, 100, 10, 30, 50, 20 ] ],
  // Tweed Champ 5F1: two triode stages into one 6V6, single-ended, 5Y3 rectifier, no tone controls at all.
  [ "small_tweed", "Small Tweed", "Fender Champ (tweed)",
    2, [ tri(1.0, 25.0, 12000.0), tri(9.0, 30.0, 9000.0), noStage, noStage ],
    1, 40.0, 0.0, 1, toneEq, 0, eqChamp, midNormal, 0,
    3.5, 2.0, 0.3, 3000.0, 5.0, 0.0, 0.40, 95.0, 7000.0, 120.0, 2.5,
    [ 60, 50, 50, 55, 40, 50, 100, 60, 50, 85, 50 ] ],
  // Orange OR80: a thick, mid-heavy crunch that turns fuzzy, EL34s with little feedback.
  [ "mandarin_80", "Mandarin 80", "Orange OR80",
    3, [ tri(1.0, 20.0, 12000.0), tri(30.0, 35.0, 9000.0, 0.7, 400.0), cf(10.0, 11000.0), noStage ],
    1, 44.0, 0.0, 3, toneStack, stackOrange, eqGeneric, midNormal, 0,
    4.0, 2.5, 0.35, 2000.0, 6.0, 0.93, 0.25, 45.0, 8500.0, 95.0, 2.0,
    [ 65, 50, 55, 55, 45, 50, 100, 45, 50, 50, 50 ] ],
  // Vox AC30 "Fawn" (before Top Boost), Normal: one triode into the inverter, 4xEL84, no feedback. Warm; Cut on Mid.
  [ "a30_fawn_nrm", "A30 Fawn Nrm", "Vox AC30 \"Fawn\", Normal channel",
    2, [ tri(1.0, 25.0, 11000.0), tri(5.0, 25.0, 10000.0), noStage, noStage ],
    1, 40.0, 0.0, 1, toneEq, 0, eqVox, midCut, 0,
    3.0, 2.0, 0.0, 3500.0, 5.0, 0.90, 0.35, 60.0, 9500.0, 95.0, 2.5,
    [ 55, 50, 70, 50, 40, 50, 100, 55, 50, 75, 50 ] ],
  // AC30 "Fawn", Bright (Brilliant) channel: small coupling capacitors take the lows out before the inverter.
  [ "a30_fawn_brt", "A30 Fawn Brt", "Vox AC30 \"Fawn\", Bright channel",
    2, [ tri(1.0, 25.0, 14000.0, 0.5, 500.0), tri(6.5, 60.0, 13000.0), noStage, noStage ],
    1, 40.0, 0.0, 1, toneEq, 0, eqVox, midCut, 0,
    3.0, 2.0, 0.0, 3500.0, 5.0, 0.90, 0.35, 60.0, 9500.0, 95.0, 2.5,
    [ 55, 55, 70, 50, 40, 50, 100, 55, 50, 75, 50 ] ],
  // Pete Anderson's custom blackface-style amp: the Deluxe circuit with more gain and a fuller middle.
  [ "black_panel_pete", "Black Panel Pete", "Pete Anderson custom amp",
    3, [ tri(1.0, 12.0, 15000.0), tri(30.0, 12.0, 12000.0), tri(1.8, 12.0, 10000.0), noStage ],
    1, 46.0, 3000.0, 1, toneStack, stackPete, eqGeneric, midNormal, 0,
    2.2, 2.3, 0.6, 3000.0, 6.0, 0.92, 0.28, 50.0, 10000.0, 100.0, 1.5,
    [ 45, 50, 55, 55, 40, 50, 100, 45, 50, 50, 45 ] ],
  // Line 6 Acoustic: a clean, wide-band amp voiced to make an electric guitar sound like a piezo acoustic:
  // lean low mids, lifted highs, no distortion to speak of.
  [ "line6_acoustic", "Line 6 Acoustic", "Line 6 original (acoustic simulation)",
    2, [ ss(0.7, 60.0, 20000.0), ss(8.0, 30.0, 18000.0, 0.6, 900.0), noStage, noStage ],
    1, 40.0, 0.0, 1, toneEq, 0, eqAcoustic, midNormal, 0,
    2.0, 3.0, 1.5, 5000.0, 6.0, 1.0, 0.02, 25.0, 18000.0, 95.0, 0.0,
    [ 50, 50, 35, 60, 50, 50, 100, 10, 20, 50, 20 ] ],
  // ---- Bass ----
  // Ampeg SVT, Normal: two triode stages around a Baxandall bass / treble with an inductor midrange, six 6550s
  // with heavy feedback: enormous clean headroom and a little tube growl when pushed.
  [ "svt_nrm", "SVT Nrm", "Ampeg SVT, Normal channel",
    3, [ tri(0.8, 8.0, 12000.0), tri(7.0, 8.0, 11000.0), tri(2.0, 8.0, 10000.0), noStage ],
    1, 40.0, 0.0, 2, toneEq, 0, eqSvt, midNormal, 0,
    1.8, 2.5, 1.0, 3000.0, 5.0, 0.95, 0.12, 22.0, 9000.0, 50.0, 2.0,
    [ 50, 55, 50, 50, 35, 50, 100, 30, 50, 50, 40 ] ],
  // SVT, Bright: the bright cap on the volume and a leaner first stage.
  [ "svt_brt", "SVT Brt", "Ampeg SVT, Bright channel",
    3, [ tri(0.8, 40.0, 14000.0, 0.8, 200.0), tri(7.0, 8.0, 13000.0), tri(2.0, 8.0, 11000.0), noStage ],
    1, 40.0, 1500.0, 2, toneEq, 0, eqSvt, midNormal, 0,
    1.8, 2.5, 1.0, 3000.0, 5.0, 0.95, 0.12, 22.0, 9000.0, 50.0, 2.0,
    [ 50, 55, 50, 50, 35, 50, 100, 30, 50, 50, 40 ] ],
  // Gallien-Krueger 800RB: solid state with an active four-band EQ: fast, clean, a slightly forward top.
  [ "g_cougar_800", "G Cougar 800", "Gallien-Krueger 800RB",
    2, [ ss(0.8, 20.0, 18000.0), ss(6.0, 15.0, 16000.0, 0.75, 1500.0), noStage, noStage ],
    1, 40.0, 0.0, 1, toneEq, 0, eqGk, midNormal, 0,
    2.2, 3.0, 1.5, 4000.0, 5.0, 1.0, 0.03, 20.0, 16000.0, 50.0, 0.5,
    [ 45, 55, 45, 55, 40, 50, 100, 10, 30, 50, 20 ] ],
];

// output level calibration per amp, in dB: [full amp, Pre model]
const TRIMS = [
  [ -7.2, 3.5 ], [ -10.7, 1.1 ], [ -9.2, 1.8 ], [ -10.9, 7.4 ], [ -4.1, 6.9 ], [ -12.5, 8.1 ], [ -13.1, 5.3 ], [ -10.8, 9.2 ], [ -12.4, 6.3 ], [ -14.3, 5.8 ],
  [ -7.8, 6.1 ], [ -6.9, 7.5 ], [ -12.5, 5.4 ], [ -13.7, 5.5 ], [ -13.4, 2.7 ], [ -17.5, 7.5 ], [ -18.8, 1.5 ], [ -17.6, 6.6 ], [ -19.3, 1.1 ], [ -25.6, 4.8 ],
  [ -27.0, 4.1 ], [ -25.9, 3.0 ], [ -27.7, 4.1 ], [ -27.3, 2.9 ], [ -0.9, 2.6 ], [ -20.1, 3.5 ], [ -28.1, 3.8 ], [ -19.7, 1.9 ], [ -27.0, 3.2 ], [ 2.9, 6.5 ],
  [ -28.1, 3.6 ], [ -20.7, 4.5 ], [ -26.8, 5.2 ], [ -27.6, 3.8 ], [ -26.0, 3.9 ], [ -13.4, -5.3 ], [ -28.2, 3.5 ], [ -27.2, 3.4 ], [ -19.5, 3.3 ], [ -27.5, 3.5 ],
  [ 2.5, 5.9 ], [ -10.5, 7.7 ], [ -16.1, 7.9 ], [ -4.0, 8.8 ], [ -5.3, 8.5 ], [ -12.9, 3.7 ], [ 0.4, 5.8 ], [ -5.1, 4.9 ], [ -9.4, 4.1 ], [ 2.9, 6.8 ],
].map((r) => Float32Array.from(r));
// </tables>

const SPECS = SPEC_ROWS.map((r) => ({
  key: r[0], name: r[1], basedOn: r[2], numStages: r[3], stage: r[4], drivePos: r[5], driveRangeDb: f32(r[6]), brightHz: f32(r[7]),
  tonePos: r[8], toneType: r[9], stack: r[10], eq: r[11], midMode: r[12], flags: r[13], paDrive: f32(r[14]), piLimit: f32(r[15]),
  feedback: f32(r[16]), presenceHz: f32(r[17]), presenceDb: f32(r[18]), match: f32(r[19]), sagDepth: f32(r[20]), lowHz: f32(r[21]),
  highHz: f32(r[22]), resHz: f32(r[23]), resDb: f32(r[24]), def: r[25],
}));
const NUM_AMPS = SPECS.length;
const POWER_AMP_NAMES = ["On", "Off (Pre)"];

// The passive stack's H(s) = (b1 s + b2 s^2 + b3 s^3) / (1 + a1 s + a2 s^2 + a3 s^3) for the pot positions t, m, l
function fmvAnalog(k, t, m, l, b, a) {
  const R1 = k[0] * 1.0e3, R2 = k[1] * 1.0e3, R3 = k[2] * 1.0e3, R4 = k[3] * 1.0e3;
  const C1 = k[4] * 1.0e-9, C2 = k[5] * 1.0e-9, C3 = k[6] * 1.0e-9;
  b[0] = 0;
  b[1] = t*C1*R1 + m*C3*R3 + l*(C1*R2 + C2*R2) + (C1*R3 + C2*R3);
  b[2] = t*(C1*C2*R1*R4 + C1*C3*R1*R4) - m*m*(C1*C3*R3*R3 + C2*C3*R3*R3) + m*(C1*C3*R1*R3 + C1*C3*R3*R3 + C2*C3*R3*R3)
       + l*(C1*C2*R1*R2 + C1*C2*R2*R4 + C1*C3*R2*R4) + l*m*(C1*C3*R2*R3 + C2*C3*R2*R3) + (C1*C2*R1*R3 + C1*C2*R3*R4 + C1*C3*R3*R4);
  b[3] = l*m*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4) - m*m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
       + m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4) + t*C1*C2*C3*R1*R3*R4 - t*m*C1*C2*C3*R1*R3*R4 + t*l*C1*C2*C3*R1*R2*R4;
  a[0] = 1;
  a[1] = (C1*R1 + C1*R3 + C2*R3 + C2*R4 + C3*R4) + m*C3*R3 + l*(C1*R2 + C2*R2);
  a[2] = m*(C1*C3*R1*R3 - C2*C3*R3*R4 + C1*C3*R3*R3 + C2*C3*R3*R3) + l*m*(C1*C3*R2*R3 + C2*C3*R2*R3) - m*m*(C1*C3*R3*R3 + C2*C3*R3*R3)
       + l*(C1*C2*R2*R4 + C1*C2*R1*R2 + C1*C3*R2*R4 + C2*C3*R2*R4)
       + (C1*C2*R1*R4 + C1*C3*R1*R4 + C1*C2*R3*R4 + C1*C2*R1*R3 + C1*C3*R3*R4 + C2*C3*R3*R4);
  a[3] = l*m*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4) - m*m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
       + m*(C1*C2*C3*R3*R3*R4 + C1*C2*C3*R1*R3*R3 - C1*C2*C3*R1*R3*R4) + l*C1*C2*C3*R1*R2*R4 + C1*C2*C3*R1*R3*R4;
}

function fmvMagnitude(b, a, hz) {
  const w = 2 * PI * hz;
  const nr = -b[2] * w * w, ni = b[1] * w - b[3] * w * w * w;
  const dr = a[0] - a[2] * w * w, di = a[1] * w - a[3] * w * w * w;
  return Math.sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

// The same stack as a state-space system on its three capacitor voltages, discretised with the trapezoidal rule:
//   x[n+1] = P x[n] + Q (u[n] + u[n+1]),   y[n] = C x[n] + D u[n]      (C[3] = D)
// Its state means the same thing whatever the pot positions, so turning a knob neither clicks nor zippers.
function fmvStateSpace(k, t, m, l, fs, gain, P, Q, C) {
  const C1 = k[4] * 1.0e-9, C2 = k[5] * 1.0e-9, C3 = k[6] * 1.0e-9;
  const G1 = 1 / (k[0] * 1.0e3), G4 = 1 / (k[3] * 1.0e3), G3 = 1 / (m * k[2] * 1.0e3);
  const Gs = 1 / (l * k[1] * 1.0e3 + (1 - m) * k[2] * 1.0e3); // bass pot in series with the top of the middle pot

  // the voltage at the slope resistor's far end: vB = bu u + b1 x1 + b2 x2 + b3 x3
  const Gt = G1 + G4 + G3, bu = (G4 + G1) / Gt, b1 = -G1 / Gt, b2 = G1 / Gt, b3 = G3 / Gt;
  // x' = A x + B u
  const A0 = (G1 * (-1 - b1)) / C1, A1 = (G1 * (1 - b2)) / C1, A2 = (-G1 * b3) / C1;
  const A3 = (-G1 * (-1 - b1)) / C2, A4 = (-Gs - G1 * (1 - b2)) / C2, A5 = (Gs + G1 * b3) / C2;
  const A6 = (G3 * b1) / C3, A7 = (G3 * b2 + Gs) / C3, A8 = (G3 * (b3 - 1) - Gs) / C3;
  const B0 = (G1 * (1 - bu)) / C1, B1 = (-G1 * (1 - bu)) / C2, B2 = (G3 * bu) / C3;

  // P = (I - h A)^-1 (I + h A), Q = (I - h A)^-1 h B with h = T / 2
  const h = 0.5 / fs;
  const M0 = -h * A0 + 1, M1 = -h * A1, M2 = -h * A2, M3 = -h * A3, M4 = -h * A4 + 1, M5 = -h * A5, M6 = -h * A6, M7 = -h * A7, M8 = -h * A8 + 1;
  const N0 = h * A0 + 1, N1 = h * A1, N2 = h * A2, N3 = h * A3, N4 = h * A4 + 1, N5 = h * A5, N6 = h * A6, N7 = h * A7, N8 = h * A8 + 1;

  const c00 = M4 * M8 - M5 * M7, c01 = M5 * M6 - M3 * M8, c02 = M3 * M7 - M4 * M6;
  const id = 1 / (M0 * c00 + M1 * c01 + M2 * c02);
  const i0 = c00 * id, i1 = (M2 * M7 - M1 * M8) * id, i2 = (M1 * M5 - M2 * M4) * id;
  const i3 = c01 * id, i4 = (M0 * M8 - M2 * M6) * id, i5 = (M2 * M3 - M0 * M5) * id;
  const i6 = c02 * id, i7 = (M1 * M6 - M0 * M7) * id, i8 = (M0 * M4 - M1 * M3) * id;
  P[0] = i0 * N0 + i1 * N3 + i2 * N6; P[1] = i0 * N1 + i1 * N4 + i2 * N7; P[2] = i0 * N2 + i1 * N5 + i2 * N8;
  Q[0] = h * (i0 * B0 + i1 * B1 + i2 * B2);
  P[3] = i3 * N0 + i4 * N3 + i5 * N6; P[4] = i3 * N1 + i4 * N4 + i5 * N7; P[5] = i3 * N2 + i4 * N5 + i5 * N8;
  Q[1] = h * (i3 * B0 + i4 * B1 + i5 * B2);
  P[6] = i6 * N0 + i7 * N3 + i8 * N6; P[7] = i6 * N1 + i7 * N4 + i8 * N7; P[8] = i6 * N2 + i7 * N5 + i8 * N8;
  Q[2] = h * (i6 * B0 + i7 * B1 + i8 * B2);

  // the treble pot's wiper, between the C1 end (u - x1) and the bass end (vB - x2)
  C[0] = gain * ((1 - t) * b1 - t);
  C[1] = gain * (1 - t) * (b2 - 1);
  C[2] = gain * (1 - t) * b3;
  C[3] = gain * ((1 - t) * bu + t);
}

// pot tapers: bass (log, about 17 % at noon) and middle (linear); neither ever quite reaches 0 ohms
const bassTaper = (x) => 0.01 + 0.99 * x * x * (0.4 + 0.6 * x);
const midTaper = (x) => 0.02 + 0.98 * x;

// Trapezoidal state-variable filter (A. Simper's form): c = [a1, a2, a3, m0, m1, m2] for
//   v3 = x - ic2;  v1 = a1 ic1 + a2 v3;  v2 = ic2 + a2 ic1 + a3 v3;  ic1 = 2 v1 - ic1;  ic2 = 2 v2 - ic2;  y = m0 x + m1 v1 + m2 v2
function svfSet(c, g, k, m0, m1, m2) {
  c[0] = 1 / (1 + g * (g + k));
  c[1] = g * c[0];
  c[2] = g * c[1];
  c[3] = m0;
  c[4] = m1;
  c[5] = m2;
}
const svfG = (fs, fc) => Math.tan((PI * clamp(fc, 1, 0.45 * fs)) / fs);
function svfShelf(c, fs, fc, gainDb, high) {
  const A = Math.pow(10, gainDb / 40), k = 1.4142135623730951;
  if (high) svfSet(c, svfG(fs, fc) * Math.sqrt(A), k, A * A, k * (1 - A) * A, 1 - A * A);
  else svfSet(c, svfG(fs, fc) / Math.sqrt(A), k, 1, k * (A - 1), A * A - 1);
}
function svfPeak(c, fs, fc, q, gainDb) {
  const A = Math.pow(10, gainDb / 40), k = 1 / (q * A);
  svfSet(c, svfG(fs, fc), k, 1, k * (A * A - 1), 0);
}
function svfLowPass(c, fs, fc, q) {
  svfSet(c, svfG(fs, fc), 1 / q, 0, 0, 1);
}

// RBJ shelf / peak / low-pass / high-pass biquads with double coefficients { b0, b1, b2, a1, a2 } at c[o..o+4],
// for the fixed output-transformer and speaker-impedance filters
function eqSet(c, o, b0, b1, b2, a0, a1, a2) {
  c[o] = b0 / a0; c[o + 1] = b1 / a0; c[o + 2] = b2 / a0; c[o + 3] = a1 / a0; c[o + 4] = a2 / a0;
}
function eqShelf(c, o, fs, fc, gainDb, high) {
  const w0 = (2 * PI * clamp(fc, 1, 0.45 * fs)) / fs, cosw = Math.cos(w0), alpha = Math.sin(w0) / (2 * 0.70710678);
  const A = Math.pow(10, gainDb / 40), k = 2 * Math.sqrt(A) * alpha, sgn = high ? -1 : 1;
  eqSet(c, o, A * ((A + 1) - sgn * (A - 1) * cosw + k), sgn * 2 * A * ((A - 1) - sgn * (A + 1) * cosw), A * ((A + 1) - sgn * (A - 1) * cosw - k),
        (A + 1) + sgn * (A - 1) * cosw + k, -sgn * 2 * ((A - 1) + sgn * (A + 1) * cosw), (A + 1) + sgn * (A - 1) * cosw - k);
}
function eqPeak(c, o, fs, fc, q, gainDb) {
  const w0 = (2 * PI * clamp(fc, 1, 0.45 * fs)) / fs, cosw = Math.cos(w0), alpha = Math.sin(w0) / (2 * q);
  const A = Math.pow(10, gainDb / 40);
  eqSet(c, o, 1 + alpha * A, -2 * cosw, 1 - alpha * A, 1 + alpha / A, -2 * cosw, 1 - alpha / A);
}
function eqLowPass(c, o, fs, fc, q) {
  const w0 = (2 * PI * clamp(fc, 1, 0.45 * fs)) / fs, cosw = Math.cos(w0), alpha = Math.sin(w0) / (2 * q);
  eqSet(c, o, (1 - cosw) * 0.5, 1 - cosw, (1 - cosw) * 0.5, 1 + alpha, -2 * cosw, 1 - alpha);
}

function eqHighPass(c, o, fs, fc, q) {
  const w0 = (2 * PI * clamp(fc, 1, 0.45 * fs)) / fs, cosw = Math.cos(w0), alpha = Math.sin(w0) / (2 * q);
  eqSet(c, o, (1 + cosw) * 0.5, -(1 + cosw), (1 + cosw) * 0.5, 1 + alpha, -2 * cosw, 1 - alpha);
}

// decaying filter states are cleared before they turn into denormal numbers (JavaScript has no flush-to-zero mode)
const flush = (v) => (Math.abs(v) < 1.0e-30 ? 0 : v);

function onePoleG(fs, fc) {
  const g = Math.tan((PI * clamp(fc, 1, 0.45 * fs)) / fs);
  return g / (1 + g);
}

// values that ramp linearly across a chunk: the first NUM_FAST ones step every oversampled sample, the rest every base-rate sample
const R_A = 0, R_AC = 1, R_B = 2, R_MASTER = 3, R_PRE = 4, R_PA = 5, R_PRES = 6, NUM_FAST = 7,
      R_COMP = 7, R_Q = 8, R_SAG = 9, R_BX = 10, R_BXG = 11, R_HUMU = 12, R_HUMPRE = 13, R_RIP = 14, NUM_RAMPS = 15;
const NUM_MIX = 9;

export class AmpFx {
  constructor() {
    this.fs = 48000; this.fsOs = 192000; this.variant = 0; this.dirty = true;
    this.os = null;
    this.target = Float32Array.from([0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 1, 0.5, 0, 0.5, 0.5, 1]);
    this.cur = new Float32Array(NUM_KNOBS);
    this.slewPerSample = 0;
    this.ramp = new Float64Array(NUM_RAMPS); this.rampTarget = new Float64Array(NUM_RAMPS); this.rampStep = new Float64Array(NUM_RAMPS);
    this.mixC = new Float64Array(NUM_MIX); this.mixT = new Float64Array(NUM_MIX); this.mixStep = new Float64Array(NUM_MIX); // the tone section's output mix
    // the model
    this.numStages = 2; this.drivePos = 1; this.tonePos = 1;
    this.parallel = false; this.toneFmv = true; this.hasCut = false;
    this.sGain = new Float64Array(MAX_STAGES); this.sBias = new Float64Array(MAX_STAGES); this.sInvP = new Float64Array(MAX_STAGES);
    this.sInvN = new Float64Array(MAX_STAGES); this.sOff = new Float64Array(MAX_STAGES); this.sNorm = new Float64Array(MAX_STAGES);
    this.sShelfG = new Float64Array(MAX_STAGES); this.sShelfK = new Float64Array(MAX_STAGES); this.sHpG = new Float64Array(MAX_STAGES);
    this.sLpG = new Float64Array(MAX_STAGES); this.sSqP = new Float64Array(MAX_STAGES); this.sSqN = new Float64Array(MAX_STAGES);
    this.idleF = new Float64Array(MAX_STAGES);
    this.makeup = 1; this.presLin = 1; this.trimLin = 1;
    this.preLin = 1; this.paDrive = 1; this.piInv = 0.4; this.piLimit = 2.5; this.nfbK = 0; this.mm = 0.9; this.invN = 0.5;
    this.postC = new Float64Array(20); // output transformer and speaker-impedance filters (base rate)
    // knob-dependent
    this.tP = new Float64Array(9); this.tQ = new Float64Array(3); this.eqA = new Float64Array(9);
    this.brG = 0.5; this.cutG = 0.5; this.presG = 0.5;
    this.xAtk = 0; this.xRel = 0; this.sAtk = 0; this.sRel = 0;
    // state (the oversampled path is in double in C++ too)
    this.stShelf = new Float64Array(MAX_STAGES); this.stHp = new Float64Array(MAX_STAGES); this.stLp = new Float64Array(MAX_STAGES);
    this.stA = new Float64Array(MAX_STAGES); this.stF = new Float64Array(MAX_STAGES); this.pX = 0; // last tube inputs and antiderivatives
    this.tz = new Float64Array(4); this.eqZ = new Float64Array(6); this.postZ = new Float64Array(8);
    this.brS = 0; this.cutS = 0; this.presS = 0; this.envX = 0; this.envS = 0; this.dcS = 0; this.dcG = 0;
    this.humC = 1; this.humS = 0; this.humCw = 1; this.humSw = 0;
    this.outGain = new Smoothed(1); this.mixB = new Smoothed(1);
    this.fb = new Float64Array(4); this.fa = new Float64Array(4); this.sc = new Float64Array(6); // scratch for coefficient maths
  }

  prepare(sampleRate, maxBlock) {
    this.fs = sampleRate;
    this.fsOs = 4 * sampleRate;
    this.os = new Oversampler4x(maxBlock);
    this.slewPerSample = f32(1 / (0.05 * this.fs));
    this.xAtk = 1 - Math.exp(-1 / (0.003 * this.fsOs)); // bias excursion: the coupling caps charge fast...
    this.xRel = 1 - Math.exp(-1 / (0.060 * this.fsOs)); // ...and recover slowly
    this.sAtk = 1 - Math.exp(-1 / (0.012 * this.fsOs)); // supply sag
    this.sRel = 1 - Math.exp(-1 / (0.160 * this.fsOs));
    this.humCw = Math.cos((2 * PI * 60) / this.fs);
    this.humSw = Math.sin((2 * PI * 60) / this.fs);
    this.dcG = 1 - Math.exp((-2 * PI * 7) / this.fs);
    this.outGain.reset(this.fs, 0.05);
    this.mixB.reset(this.fs, 0.05);
    this.reset();
  }

  reset() {
    this.configure();
    this.cur.set(this.target);
    this.updateChunk(CHUNK, true);
    this.outGain.setCurrentAndTarget(this.outGain.target);
    this.mixB.setCurrentAndTarget(this.mixB.target);
    this.stShelf.fill(0); this.stHp.fill(0); this.stLp.fill(0);
    this.stA.set(this.sBias);
    this.stF.set(this.idleF);
    this.tz.fill(0); this.eqZ.fill(0); this.postZ.fill(0);
    this.brS = this.cutS = this.presS = this.envX = this.envS = this.pX = this.dcS = 0;
    this.humC = 1;
    this.humS = 0;
    if (this.os) this.os.reset();
    this.dirty = false;
  }

  setModel(variant) {
    variant = clamp(variant | 0, 0, NUM_AMPS - 1);
    this.dirty = this.dirty || variant !== this.variant;
    this.variant = variant;
  }

  /** Drive, Bass, Mid, Treble, Presence, Ch Vol, Master, Sag, Hum, Bias, Bias X [%], Power Amp (0 = On, 1 = Off). */
  setParameters(k) {
    const t = this.target;
    for (let i = 0; i < NUM_KNOBS - 1; ++i) t[i] = clamp(f32(f32(k[i]) * f32(0.01)), 0, 1);
    t[11] = k[11] < 0.5 ? 1 : 0;
  }

  process(left, right, n) {
    if (this.dirty) this.reset(); // a model was picked without the reset() that should follow it

    for (let i = 0; i < n; ++i) left[i] = 0.5 * (left[i] + right[i]);

    const x = this.os.up(left, n);
    for (let pos = 0; pos < n; pos += CHUNK) {
      const len = Math.min(CHUNK, n - pos);
      this.updateChunk(len, false);
      this.processChunk(x, 4 * pos, len);
    }
    this.os.down(left, n);

    // base rate: output transformer and speaker-impedance response (full amp only), a DC blocker at 7 Hz
    // (lopsided waves leave a little DC in the Pre model's line stage), then Ch Vol
    const c = this.postC, postZ = this.postZ, mixB = this.mixB, outGain = this.outGain;
    let dcState = this.dcS;
    const dcCoeff = this.dcG;
    let p0 = postZ[0], p1 = postZ[1], p2 = postZ[2], p3 = postZ[3], p4 = postZ[4], p5 = postZ[5], p6 = postZ[6], p7 = postZ[7];
    for (let i = 0; i < n; ++i) {
      let z = left[i];
      let y = c[0] * z + p0;       // transformer low end
      p0 = c[1] * z - c[3] * y + p1;
      p1 = c[2] * z - c[4] * y;
      let w = c[5] * y + p2;       // transformer top end
      p2 = c[6] * y - c[8] * w + p3;
      p3 = c[7] * y - c[9] * w;
      y = c[10] * w + p4;          // speaker resonance
      p4 = c[11] * w - c[13] * y + p5;
      p5 = c[12] * w - c[14] * y;
      w = c[15] * y + p6;          // the voice coil's rising impedance
      p6 = c[16] * y - c[18] * w + p7;
      p7 = c[17] * y - c[19] * w;
      z += mixB.next() * (w - z);
      dcState += (z - dcState) * dcCoeff;
      z = (z - dcState) * outGain.next();
      if (Math.abs(z) > 2) { // protection far above any normal level: whatever the knobs, the output stays below 4
        const over = Math.abs(z) - 2, limited = 2 + over / (1 + 0.5 * over);
        z = z > 0 ? limited : -limited;
      }
      left[i] = z;
      right[i] = z;
    }
    postZ[0] = flush(p0); postZ[1] = flush(p1); postZ[2] = flush(p2); postZ[3] = flush(p3);
    postZ[4] = flush(p4); postZ[5] = flush(p5); postZ[6] = flush(p6); postZ[7] = flush(p7);
    this.dcS = flush(dcState);
  }

  /** Everything that depends only on the model and the sample rate. */
  configure() {
    const sp = SPECS[this.variant], fsOs = this.fsOs;
    this.numStages = sp.numStages;
    this.drivePos = sp.drivePos;
    this.tonePos = sp.tonePos;
    this.parallel = (sp.flags & flagParallel) !== 0;
    this.toneFmv = sp.toneType === toneStack;
    this.hasCut = sp.midMode === midCut || sp.midMode === midDivide;

    let polarity = 1;
    for (let s = 0; s < MAX_STAGES; ++s) {
      const st = s < this.numStages ? sp.stage[s] : noStage; // [gain, bias, limPos, limNeg, pol, hpHz, lpHz, shelfK, shelfHz]
      this.sGain[s] = st[0];
      this.sBias[s] = st[1];
      this.sInvP[s] = 1 / st[2];
      this.sInvN[s] = 1 / st[3];
      this.sSqP[s] = st[2] * st[2];
      this.sSqN[s] = st[3] * st[3];
      const w = this.sBias[s] * (this.sBias[s] >= 0 ? this.sInvP[s] : this.sInvN[s]), root = Math.sqrt(1 + w * w);
      this.sOff[s] = this.sBias[s] / root; // exactly what the stage gives with no signal
      this.sNorm[s] = st[4] * root * root * root;
      this.idleF[s] = (this.sBias[s] >= 0 ? this.sSqP[s] : this.sSqN[s]) * (root - 1);
      this.sShelfG[s] = onePoleG(fsOs, st[8]);
      this.sShelfK[s] = 1 - st[7];
      this.sHpG[s] = onePoleG(fsOs, st[5]);
      this.sLpG[s] = onePoleG(fsOs, st[6]);
      if (s < this.numStages && !(this.parallel && s === 1)) polarity *= st[4];
    }

    this.makeup = 1;
    if (this.toneFmv) {
      fmvAnalog(STACKS[sp.stack], 0.5, sp.midMode === midCut ? 1 : midTaper(0.5), bassTaper(0.5), this.fb, this.fa);
      this.makeup = 0.5 / fmvMagnitude(this.fb, this.fa, 1000); // the stack's loss is made up by the stage after it
    }

    this.piLimit = sp.piLimit;
    this.piInv = 1 / sp.piLimit;
    this.nfbK = sp.feedback;
    this.mm = sp.match;
    this.invN = 1 / (1 + sp.match);
    this.presLin = Math.pow(10, sp.presenceDb / 20) - 1;
    this.trimLin = Math.pow(10, TRIMS[this.variant][0] / 20);
    this.preLin = polarity * Math.pow(10, TRIMS[this.variant][1] / 20);
    this.paDrive = polarity * sp.paDrive;

    eqHighPass(this.postC, 0, this.fs, sp.lowHz, 0.7071);
    eqLowPass(this.postC, 5, this.fs, sp.highHz, 0.7071);
    eqPeak(this.postC, 10, this.fs, sp.resHz, 1.2, sp.resDb);
    eqShelf(this.postC, 15, this.fs, 4500, 0.5 * sp.resDb, true);
  }

  /** Moves the knobs towards their targets (50 ms for the full travel) and recomputes what depends on them. */
  updateChunk(len, force) {
    const step = f32(len * this.slewPerSample), cur = this.cur, target = this.target;
    let changed = force ? 0xfff : 0;
    for (let i = 0; i < NUM_KNOBS; ++i) {
      const d = f32(target[i] - cur[i]);
      if (d !== 0) {
        cur[i] = Math.abs(d) <= step ? target[i] : cur[i] + (d > 0 ? step : -step);
        changed |= 1 << i;
      }
    }

    if (changed !== 0) this.derive(changed);

    const perBase = 1 / len, perOver = 0.25 * perBase;
    const ramp = this.ramp, rampTarget = this.rampTarget, rampStep = this.rampStep;
    for (let r = 0; r < NUM_RAMPS; ++r) {
      if (force) ramp[r] = rampTarget[r];
      rampStep[r] = (rampTarget[r] - ramp[r]) * (r < NUM_FAST ? perOver : perBase);
    }
    const mixC = this.mixC, mixT = this.mixT, mixStep = this.mixStep;
    for (let i = 0; i < NUM_MIX; ++i) {
      if (force) mixC[i] = mixT[i];
      mixStep[i] = (mixT[i] - mixC[i]) * perOver;
    }
  }

  derive(changed) {
    const sp = SPECS[this.variant], cur = this.cur, fsOs = this.fsOs, rampTarget = this.rampTarget;
    const bass = cur[1], mid = cur[2], treble = cur[3];

    if ((changed & 0x003) !== 0) { // Drive: the volume / gain pot, with its bright cap
      const alpha = Math.pow(10, (-sp.driveRangeDb * (1 - cur[0])) / 20);
      if (sp.brightHz > 0) {
        // H = alpha + (1 - alpha) * highpass at brightHz / (alpha (1 - alpha)): full treble, the rest turned down
        rampTarget[R_A] = 1;
        rampTarget[R_AC] = 1 - alpha;
        this.brG = onePoleG(fsOs, sp.brightHz / (alpha * (1 - alpha) + 1.0e-4));
      } else {
        rampTarget[R_A] = alpha;
        rampTarget[R_AC] = 0;
      }
      rampTarget[R_B] = sp.midMode === midDivide ? Math.pow(10, (-sp.driveRangeDb * (1 - bass)) / 20) : 1;
    }

    if ((changed & 0x00e) !== 0) { // Bass, Mid, Treble
      const mixT = this.mixT;
      if (this.toneFmv) {
        fmvStateSpace(STACKS[sp.stack], treble, sp.midMode === midCut ? 1 : midTaper(mid), bassTaper(bass), fsOs, this.makeup, this.tP, this.tQ, mixT);
      } else {
        const e = EQS[sp.eq], c = this.sc, eqA = this.eqA; // e: [bassHz, bassDb, midHz, midQ, midDb, trebleHz, trebleDb]
        svfShelf(c, fsOs, e[0], (bass - 0.5) * 2 * e[1], false);
        eqA[0] = c[0]; eqA[1] = c[1]; eqA[2] = c[2]; mixT[0] = c[3]; mixT[1] = c[4]; mixT[2] = c[5];
        if (sp.midMode === midTone || sp.midMode === midDivide) svfLowPass(c, fsOs, 700 * Math.pow(20, mid), 0.7071);
        else svfPeak(c, fsOs, e[2], e[3], (mid - 0.5) * 2 * e[4]);
        eqA[3] = c[0]; eqA[4] = c[1]; eqA[5] = c[2]; mixT[3] = c[3]; mixT[4] = c[4]; mixT[5] = c[5];
        svfShelf(c, fsOs, e[5], (treble - 0.5) * 2 * e[6], true);
        eqA[6] = c[0]; eqA[7] = c[1]; eqA[8] = c[2]; mixT[6] = c[3]; mixT[7] = c[4]; mixT[8] = c[5];
      }

      if (this.hasCut) this.cutG = onePoleG(fsOs, 900 * Math.pow(28, sp.midMode === midCut ? mid : treble));
    }

    if ((changed & 0x014) !== 0) { // Presence (its corner follows Mid on the Elektrik)
      rampTarget[R_PRES] = cur[4] * this.presLin;
      this.presG = onePoleG(fsOs, sp.presenceHz * ((sp.flags & flagInteract) !== 0 ? 1.6 - 1.2 * mid : 1));
    }

    if ((changed & 0x020) !== 0) // Ch Vol: 50 % = the calibrated level, 100 % = +9.5 dB
      this.outGain.setTarget(f32(this.trimLin * Math.pow(2 * cur[5], 1.585)));

    if ((changed & 0x940) !== 0) { // Master, Power Amp, Hum
      const cutDb = 36 * Math.pow(1 - cur[6], 1.2), giveBack = Math.pow(10, (0.55 * cutDb) / 20), on = cur[11];
      rampTarget[R_MASTER] = this.paDrive * Math.pow(10, -cutDb / 20);
      rampTarget[R_PA] = on * giveBack; // part of the level lost by turning Master down is given back
      rampTarget[R_PRE] = 1 - on;
      this.mixB.setTarget(cur[11]);

      const hum = 0.0012 * cur[8] * cur[8]; // at the output, before Ch Vol
      rampTarget[R_HUMU] = hum / (giveBack * this.trimLin);
      rampTarget[R_HUMPRE] = hum / this.trimLin;
      rampTarget[R_RIP] = 0.04 * cur[8] * cur[8];
    }

    if ((changed & 0x080) !== 0) rampTarget[R_SAG] = Math.min(0.8, 2 * cur[7] * sp.sagDepth);

    if ((changed & 0x200) !== 0) { // Bias: the output tubes' idle point, from cold (crossover notch) to class A
      let q = -1.15 + 1.15 * cur[9];
      if (q < -0.575) q = -0.575 + (q + 0.575) / (1 + 0.5 * sp.feedback); // feedback straightens part of the crossover notch
      const qc = Math.max(q, -0.6);
      rampTarget[R_Q] = q;
      rampTarget[R_COMP] = Math.pow(1 + qc * qc, 1.5); // unit gain for small signals, down to the ideal class AB point
    }

    if ((changed & 0x400) !== 0) { // Bias X: how far hard drive pushes the idle point colder, and how much gain that costs
      rampTarget[R_BX] = (0.8 * cur[10]) / (1 + sp.feedback);
      rampTarget[R_BXG] = 0.7 * cur[10];
    }
  }

  /** `len` base-rate samples = 4 * len oversampled ones of x from `offset`, in place: preamp stages, tone stack, power amp. */
  processChunk(x, offset, len) {
    const nSt = this.numStages, dPos = this.drivePos, tPos = this.tonePos;
    const par = this.parallel, fmv = this.toneFmv, cut = this.hasCut;
    const ramp = this.ramp, rampTarget = this.rampTarget, rampStep = this.rampStep;
    const paActive = ramp[R_PA] !== 0 || rampTarget[R_PA] !== 0;
    const preActive = ramp[R_PRE] !== 0 || rampTarget[R_PRE] !== 0;

    let gA = ramp[R_A], gAc = ramp[R_AC], gB = ramp[R_B], gMaster = ramp[R_MASTER], gPre = ramp[R_PRE], gPa = ramp[R_PA], pGain = ramp[R_PRES];
    const dA = rampStep[R_A], dAc = rampStep[R_AC], dB = rampStep[R_B], dMaster = rampStep[R_MASTER], dPre = rampStep[R_PRE],
          dPa = rampStep[R_PA], dPres = rampStep[R_PRES];
    let cmp = ramp[R_COMP], qIdle = ramp[R_Q], sag = ramp[R_SAG], bxK = ramp[R_BX], bxG = ramp[R_BXG], hU = ramp[R_HUMU], hPre = ramp[R_HUMPRE], hRip = ramp[R_RIP];
    const dCmp = rampStep[R_COMP], dQ = rampStep[R_Q], dSag = rampStep[R_SAG], dBxK = rampStep[R_BX], dBxG = rampStep[R_BXG],
          dHU = rampStep[R_HUMU], dHPre = rampStep[R_HUMPRE], dHRip = rampStep[R_RIP];

    const shelfS = this.stShelf, hpS = this.stHp, lpS = this.stLp, tubeA = this.stA, tubeF = this.stF;
    const sGain = this.sGain, sBias = this.sBias, sInvP = this.sInvP, sInvN = this.sInvN, sOff = this.sOff, sNorm = this.sNorm,
          sShelfG = this.sShelfG, sShelfK = this.sShelfK, sHpG = this.sHpG, sLpG = this.sLpG, sSqP = this.sSqP, sSqN = this.sSqN;

    // tone stack (state-space) or the three-band EQ (state-variable filters), and their output mixes
    const tP = this.tP, tQ = this.tQ, tz = this.tz, eqA = this.eqA, eqZ = this.eqZ, mixC = this.mixC, mixT = this.mixT, mixStep = this.mixStep;
    const P0 = tP[0], P1 = tP[1], P2 = tP[2], P3 = tP[3], P4 = tP[4], P5 = tP[5], P6 = tP[6], P7 = tP[7], P8 = tP[8];
    const Q0 = tQ[0], Q1 = tQ[1], Q2 = tQ[2];
    let z0 = tz[0], z1 = tz[1], z2 = tz[2], zu = tz[3];
    const A0 = eqA[0], A1 = eqA[1], A2 = eqA[2], A3 = eqA[3], A4 = eqA[4], A5 = eqA[5], A6 = eqA[6], A7 = eqA[7], A8 = eqA[8];
    let e0 = eqZ[0], e1 = eqZ[1], e2 = eqZ[2], e3 = eqZ[3], e4 = eqZ[4], e5 = eqZ[5];
    let k0 = mixC[0], k1 = mixC[1], k2 = mixC[2], k3 = mixC[3], k4 = mixC[4], k5 = mixC[5], k6 = mixC[6], k7 = mixC[7], k8 = mixC[8];
    const dk0 = mixStep[0], dk1 = mixStep[1], dk2 = mixStep[2], dk3 = mixStep[3], dk4 = mixStep[4], dk5 = mixStep[5],
          dk6 = mixStep[6], dk7 = mixStep[7], dk8 = mixStep[8];

    let bs = this.brS, cs = this.cutS, ps = this.presS, ex = this.envX, es = this.envS, px = this.pX;
    const bG = this.brG, cG = this.cutG, pG = this.presG, pLin = this.preLin, pInv = this.piInv, pLimit = this.piLimit;
    const fb = this.nfbK, match = this.mm, norm = this.invN;
    const xAtk = this.xAtk, xRel = this.xRel, sAtk = this.sAtk, sRel = this.sRel;
    let hc = this.humC, hs = this.humS;
    const hcw = this.humCw, hsw = this.humSw;
    let p = offset;

    for (let i = 0; i < len; ++i) {
      // heater hum (60 Hz and its 3rd harmonic) and supply ripple (120 Hz): one value per base-rate sample
      const nc = hc * hcw - hs * hsw, ns = hs * hcw + hc * hsw;
      hc = nc;
      hs = ns;
      const heater = ns + 0.35 * ns * (3 - 4 * ns * ns);
      const humIn = hU * heater, humOut = hPre * heater;

      // the output tubes' idle point: driven hard, the grids charge the coupling caps and the bias goes colder,
      // which opens a crossover notch and takes gain away (Bias X)
      const q = qIdle - bxK * ex;
      const rq = q / Math.sqrt(1 + q * q), dcq = (rq - match * rq) * norm; // what the tubes give with no signal
      const normC = norm / cmp;
      const gx = 1 / (1 + bxG * ex), supply = (1 - hRip * (2 * ns * nc)) * gx, sagG = sag * gx;
      // the tubes' antiderivative at the last sample, for this idle point
      const pc = cmp * px, pa1 = q + pc, pa2 = q - pc;
      let pF = (Math.sqrt(1 + pa1 * pa1) + match * Math.sqrt(1 + pa2 * pa2)) * normC - dcq * px;

      for (let j = 0; j < 4; ++j) {
        let v = x[p];
        const in0 = v;
        let acc = 0;

        for (let s = 0; ; ++s) {
          if (s === tPos) {
            if (fmv) { // tone stack: x = the three capacitor voltages
              const us = zu + v;
              const n0 = P0 * z0 + P1 * z1 + P2 * z2 + Q0 * us;
              const n1 = P3 * z0 + P4 * z1 + P5 * z2 + Q1 * us;
              const n2 = P6 * z0 + P7 * z1 + P8 * z2 + Q2 * us;
              z0 = n0; z1 = n1; z2 = n2; zu = v;
              v = k0 * z0 + k1 * z1 + k2 * z2 + k3 * v;
              k0 += dk0; k1 += dk1; k2 += dk2; k3 += dk3;
            } else { // bass shelf, mid peak (or Tone low-pass), treble shelf
              let v3 = v - e1, v1 = A0 * e0 + A1 * v3, v2 = e1 + A1 * e0 + A2 * v3;
              e0 = 2 * v1 - e0;
              e1 = 2 * v2 - e1;
              const y = k0 * v + k1 * v1 + k2 * v2;
              v3 = y - e3; v1 = A3 * e2 + A4 * v3; v2 = e3 + A4 * e2 + A5 * v3;
              e2 = 2 * v1 - e2;
              e3 = 2 * v2 - e3;
              const w = k3 * y + k4 * v1 + k5 * v2;
              v3 = w - e5; v1 = A6 * e4 + A7 * v3; v2 = e5 + A7 * e4 + A8 * v3;
              e4 = 2 * v1 - e4;
              e5 = 2 * v2 - e5;
              v = k6 * w + k7 * v1 + k8 * v2;
              k0 += dk0; k1 += dk1; k2 += dk2; k3 += dk3; k4 += dk4; k5 += dk5; k6 += dk6; k7 += dk7; k8 += dk8;
            }
          }

          if (s === nSt) break;

          if (s === dPos) { // the volume / gain pot and its bright cap
            const t = (v - bs) * bG, lp = t + bs;
            bs = lp + t;
            v = v * gA - lp * gAc;
          } else if (par && s === 1) { // the second channel takes the input too
            acc = v;
            v = in0 * gB;
          }

          // cathode / treble-peaking shelf
          let t, lp;
          if (sShelfK[s] !== 0) {
            t = (v - shelfS[s]) * sShelfG[s];
            lp = t + shelfS[s];
            shelfS[s] = lp + t;
            v -= sShelfK[s] * lp;
          }

          // the tube, anti-aliased: the mean of its curve between the last sample and this one, which is
          // the difference quotient of the curve's antiderivative F = lim^2 (sqrt (1 + (a / lim)^2) - 1)
          const a = v * sGain[s] + sBias[s];
          const w = a * (a >= 0 ? sInvP[s] : sInvN[s]);
          const F = (a >= 0 ? sSqP[s] : sSqN[s]) * (Math.sqrt(1 + w * w) - 1);
          const da = a - tubeA[s];
          let y;
          if (Math.abs(da) > 1.0e-6) {
            y = (F - tubeF[s]) / da;
          } else {
            const am = 0.5 * (a + tubeA[s]), wm = am * (am >= 0 ? sInvP[s] : sInvN[s]);
            y = am / Math.sqrt(1 + wm * wm);
          }
          tubeA[s] = a;
          tubeF[s] = F;
          y = (y - sOff[s]) * sNorm[s];

          // coupling capacitor, then the stage's treble roll-off
          t = (y - hpS[s]) * sHpG[s];
          lp = t + hpS[s];
          hpS[s] = lp + t;
          y -= lp;
          t = (y - lpS[s]) * sLpG[s];
          lp = t + lpS[s];
          lpS[s] = lp + t;
          v = lp;

          if (par && s === 1) v += acc;
        }

        if (cut) { // Vox-style Cut
          const t = (v - cs) * cG, lp = t + cs;
          cs = lp + t;
          v = lp;
        }

        { // Presence: less feedback at high frequencies = a treble shelf in front of the output tubes
          const t = (v - ps) * pG, lp = t + ps;
          ps = lp + t;
          v += pGain * (v - lp);
        }

        let o = 0;
        if (preActive) { // the "Pre" model: the preamp's output through a line stage with plenty of headroom
          const w = v * 0.3333333333333333;
          o = ((v / Math.sqrt(1 + w * w)) * pLin + humOut) * gPre;
        }

        if (paActive) {
          // Master, then the phase inverter (soft, symmetrical, reaches its limit at twice that drive)
          let u = v * gMaster * pInv;
          u = u > 2 ? 2 : (u < -2 ? -2 : u);
          u = (u - 0.25 * u * Math.abs(u)) * pLimit + humIn;

          let e = Math.abs(u) - 1;
          e = e < 0 ? 0 : (e > 1.5 ? 1.5 : e);
          ex += (e - ex) * (e > ex ? xAtk : xRel);

          // negative feedback: about what the output tubes fail to deliver (nothing up to half their swing, all
          // of it beyond clipping) is added to their drive: a straighter curve with a sharper knee
          let d = Math.abs(u) - 0.5;
          d = d <= 0 ? 0 : (d < 0.7 ? d * d * 0.7142857142857143 : d - 0.35);
          const xp = u + (u >= 0 ? fb * d : -fb * d);

          // output tubes: each one a sigmoid a / sqrt (1 + a^2) around its idle point q, the pair (or the single
          // tube) combined; anti-aliased like the preamp tubes
          const cu = cmp * xp, a1 = q + cu, a2 = q - cu;
          const F = (Math.sqrt(1 + a1 * a1) + match * Math.sqrt(1 + a2 * a2)) * normC - dcq * xp;
          const dx = xp - px;
          let y;
          if (Math.abs(dx) > 1.0e-6) {
            y = (F - pF) / dx;
          } else {
            const cm = cmp * 0.5 * (xp + px), b1 = q + cm, b2 = q - cm;
            y = (b1 / Math.sqrt(1 + b1 * b1) - match * (b2 / Math.sqrt(1 + b2 * b2))) * norm - dcq;
          }
          px = xp;
          pF = F;

          // the supply sags with the current drawn, and carries the rectifier's ripple
          const ay = Math.min(Math.abs(y), 1);
          es += (ay - es) * (ay > es ? sAtk : sRel);
          o += y * (supply - sagG * es) * gPa;
        }

        x[p++] = o;
        gA += dA; gAc += dAc; gB += dB; gMaster += dMaster; gPre += dPre; gPa += dPa; pGain += dPres;
      }

      cmp += dCmp; qIdle += dQ; sag += dSag; bxK += dBxK; bxG += dBxG; hU += dHU; hPre += dHPre; hRip += dHRip;
    }

    for (let s = 0; s < MAX_STAGES; ++s) {
      shelfS[s] = flush(shelfS[s]); hpS[s] = flush(hpS[s]); lpS[s] = flush(lpS[s]);
    }
    tz[0] = flush(z0); tz[1] = flush(z1); tz[2] = flush(z2); tz[3] = zu;
    eqZ[0] = flush(e0); eqZ[1] = flush(e1); eqZ[2] = flush(e2); eqZ[3] = flush(e3); eqZ[4] = flush(e4); eqZ[5] = flush(e5);
    this.brS = flush(bs); this.cutS = flush(cs); this.presS = flush(ps); this.envX = flush(ex); this.envS = flush(es); this.pX = px;
    this.humC = hc;
    this.humS = hs;
    ramp.set(rampTarget);
    mixC.set(mixT);
  }
}

/** In the order of AmpFx's variants: the 30 stock amps, then the model packs. */
export const AMP_MODELS = SPECS.map((sp, i) => model(sp.key, sp.name, CATEGORY.amp, ENGINE.ampFx, i, sp.basedOn, [
  percent("Drive", sp.def[0]), percent("Bass", sp.def[1]), percent("Mid", sp.def[2]), percent("Treble", sp.def[3]),
  percent("Presence", sp.def[4]), percent("Ch Vol", sp.def[5]), percent("Master", sp.def[6]), percent("Sag", sp.def[7]),
  percent("Hum", sp.def[8]), percent("Bias", sp.def[9]), percent("Bias X", sp.def[10]), choice("Power Amp", POWER_AMP_NAMES, 0),
]));
