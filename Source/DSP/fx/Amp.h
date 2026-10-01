#pragma once

#include "../DspUtils.h"
#include "../ModelTypes.h"

namespace fx
{
/** The HD500X's amp models: the 30 stock amps and the 20 model-pack amps, each with its "Pre" (preamp only)
    version on the Power Amp knob. Mono. The output is the amp's output signal (what goes to the speaker):
    the cabinet and microphone are a separate engine.

    Every amp is one row of amp_detail::specs describing its circuit: up to four cascaded preamp tube stages
    (drive, bias / asymmetry, coupling and cathode-bypass corners, bright cap), where the volume control and
    the tone stack sit between them, the tone stack itself (the analytic passive Fender / Marshall / Vox
    stack with the amp's component values, or shelving / peaking EQ where the original has something else),
    and the power amp (phase inverter, push-pull or single-ended output tubes with their bias point, negative
    feedback with Presence, rectifier sag, output transformer band limits).

    How it runs: everything up to the output tubes at 4x the sample rate (Oversampler4x), and on top of that
    every tube curve is evaluated anti-aliased (as the difference quotient of its antiderivative), which keeps
    the folded-back components of even the highest-gain amps below -60 dB. The tone stack is a state-space
    model of the circuit and the EQs are state-variable filters, so that knobs can move without clicks; knob
    positions are slewed (50 ms full travel), coefficients recomputed every 32 samples and gains ramped per
    sample in between. The oversampled path keeps its state in double (see the members for why).

    Knobs (the same twelve for every amp): Drive, Bass, Mid, Treble, Presence, Ch Vol, Master, Sag, Hum,
    Bias, Bias X, Power Amp (On / Off = the "Pre" model). Remaps as on the HD500X: Super O has its single
    Tone on Mid; Divide 9/15 has clean-channel level on Drive, dirty-channel drive on Bass, Tone on Mid and
    Cut on Treble; the Vox-style amps have Cut on Mid. */
namespace amp_detail
{
    constexpr int maxStages = 4;
    constexpr int numKnobs = 12;
    constexpr int chunkSize = 32; // base-rate samples between coefficient updates

    /** One preamp tube stage, in units where 1 is the stage's own clipping level:
        y = pol * (s (gain * x + bias) - s (bias)) / s' (bias), s (x) = x / sqrt (1 + (x / lim)^2) with
        lim = limPos for x >= 0 (grid conduction) and limNeg below (cut-off). Before it a shelf that leaves
        shelfK of the lows below shelfHz (partly bypassed cathode, treble-peaking networks); after it the
        coupling capacitor (hpHz) and the stage's treble roll-off (lpHz). */
    struct Stage { float gain, bias, limPos, limNeg, pol, hpHz, lpHz, shelfK, shelfHz; };

    // 12AX7 stage at the usual bias: grid conduction comes first, cut-off later and softer
    constexpr Stage tri (float gain, float hp, float lp, float shelfK = 1.0f, float shelfHz = 100.0f) { return { gain, 0.22f, 1.0f, 1.7f, -1.0f, hp, lp, shelfK, shelfHz }; }
    // cold-biased 12AX7 ("cold clipper", 10k / 39k cathode): cuts off early and hard on one side
    constexpr Stage cold (float gain, float hp, float lp, float shelfK = 1.0f, float shelfHz = 100.0f) { return { gain, -0.22f, 2.2f, 0.55f, -1.0f, hp, lp, shelfK, shelfHz }; }
    // DC-coupled cathode follower: unity gain, squashes the positive peaks (the Marshall / Bassman "bloom")
    constexpr Stage cf (float hp, float lp) { return { 1.0f, 0.0f, 0.75f, 4.0f, 1.0f, hp, lp, 1.0f, 100.0f }; }
    // EF86 / 5879 pentode: lots of gain, clips harder and more evenly
    constexpr Stage pent (float gain, float hp, float lp, float shelfK = 1.0f, float shelfHz = 100.0f) { return { gain, 0.08f, 0.9f, 1.05f, -1.0f, hp, lp, shelfK, shelfHz }; }
    // octal / low-mu stage (6SL7, 6SC7, 12AY7): less gain, more headroom, soft
    constexpr Stage octal (float gain, float hp, float lp, float shelfK = 1.0f, float shelfHz = 100.0f) { return { gain, 0.15f, 1.6f, 2.4f, -1.0f, hp, lp, shelfK, shelfHz }; }
    // solid-state (op-amp / FET) stage: symmetrical
    constexpr Stage ss (float gain, float hp, float lp, float shelfK = 1.0f, float shelfHz = 100.0f) { return { gain, 0.0f, 1.0f, 1.0f, 1.0f, hp, lp, shelfK, shelfHz }; }
    // a stage biased so that one half of the wave is squashed almost flat: a half-wave rectifier, i.e. a strong octave overtone
    constexpr Stage rect (float gain, float hp, float lp) { return { gain, 0.0f, 0.12f, 3.0f, -1.0f, hp, lp, 1.0f, 100.0f }; }
    constexpr Stage noStage { 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 10.0f, 20000.0f, 1.0f, 100.0f };

    /** Passive treble / middle / bass stack: pots r1 (treble), r2 (bass), r3 (middle), slope resistor r4 [kOhm],
        capacitors c1 (treble), c2 (bass), c3 (middle) [nF]. */
    struct Stack { float r1, r2, r3, r4, c1, c2, c3; };

    enum StackId { stackBlackface = 0, stackBassman, stackJtm45, stackPlexi, stackVox, stackRecto, stackSoldano, stackHiwatt,
                   stackEngl, stackOrange, stackPete, numStacks };

    inline constexpr Stack stacks[numStacks] = {
        { 250.0f,  250.0f, 10.0f, 100.0f, 0.25f, 100.0f, 47.0f }, // Fender AB763 (Twin / Deluxe Reverb)
        { 250.0f, 1000.0f, 25.0f,  56.0f, 0.25f,  20.0f, 20.0f }, // Fender 5F6-A Bassman
        { 250.0f, 1000.0f, 25.0f,  56.0f, 0.27f,  22.0f, 22.0f }, // Marshall JTM45 (the Bassman's, British parts)
        { 220.0f, 1000.0f, 22.0f,  33.0f, 0.47f,  22.0f, 22.0f }, // Marshall 1959 / 2204 / Park 75
        { 1000.0f, 1000.0f, 10.0f, 100.0f, 0.05f, 22.0f, 22.0f }, // Vox AC30 Top Boost (no middle control)
        { 250.0f,  250.0f, 25.0f,  47.0f, 0.50f,  20.0f, 20.0f }, // Mesa Dual Rectifier
        { 250.0f, 1000.0f, 25.0f,  47.0f, 0.47f,  22.0f, 22.0f }, // Soldano SLO / Peavey 5150 / Bogner
        { 250.0f, 1000.0f, 22.0f,  68.0f, 0.47f,  10.0f, 47.0f }, // Hiwatt DR103: only half the blackface scoop, lower down
        { 250.0f, 1000.0f, 25.0f,  47.0f, 0.47f,  10.0f, 33.0f }, // Engl: a shallow dip lower down, strong mid control
        { 250.0f,  250.0f, 22.0f,  68.0f, 0.68f,  47.0f, 22.0f }, // Orange OR80
        { 250.0f,  250.0f, 25.0f, 100.0f, 0.25f, 100.0f, 22.0f }, // Black Panel Pete: blackface with more mids
    };

    /** Bass shelf / mid peak / treble shelf for the amps without a passive stack (and the controls Line 6
        invented for amps that never had them): corner frequencies and the +- range of each knob in dB. */
    struct Eq { float bassHz, bassDb, midHz, midQ, midDb, trebleHz, trebleDb; };

    enum EqId { eqGeneric = 0, eqSupro, eqGibson, eqVox, eqDivide, eqFlipTop, eqRoute, eqSvt, eqGk, eqAcoustic, eqChamp, numEqs };

    inline constexpr Eq eqs[numEqs] = {
        { 120.0f, 10.0f, 650.0f, 0.7f,  8.0f, 3000.0f, 10.0f }, // generic (the amps with a passive stack only name it)
        { 120.0f,  8.0f, 650.0f, 0.7f,  0.0f, 3000.0f,  8.0f }, // Supro: Mid is the Tone knob
        { 100.0f, 10.0f, 500.0f, 0.6f,  6.0f, 2200.0f, 10.0f }, // Gibson EH-185
        { 110.0f,  9.0f, 800.0f, 0.7f,  0.0f, 2800.0f, 10.0f }, // Vox without Top Boost: Mid is the Cut
        { 120.0f,  0.0f, 650.0f, 0.7f,  0.0f, 3000.0f,  0.0f }, // Divide 9/15: Tone and Cut only
        {  70.0f, 14.0f, 400.0f, 0.7f,  8.0f, 2200.0f, 12.0f }, // Ampeg B-15 Baxandall
        { 120.0f, 10.0f, 700.0f, 0.7f,  6.0f, 2500.0f, 10.0f }, // Dr. Z Route 66 bass / treble
        {  50.0f, 12.0f, 800.0f, 1.2f, 12.0f, 4000.0f, 14.0f }, // Ampeg SVT: Baxandall + inductor mid at 800 Hz
        {  60.0f, 12.0f, 500.0f, 0.8f, 12.0f, 7000.0f, 12.0f }, // Gallien-Krueger 800RB active EQ
        { 100.0f, 10.0f, 800.0f, 0.8f, 10.0f, 6000.0f, 12.0f }, // Line 6 Acoustic
        { 120.0f,  8.0f, 700.0f, 0.7f,  6.0f, 3000.0f,  8.0f }, // Fender Champ (no tone controls on the original)
    };

    //==============================================================================
    /** The passive stack's transfer function H (s) = (b1 s + b2 s^2 + b3 s^3) / (1 + a1 s + a2 s^2 + a3 s^3)
        for the pot positions t, m, l in 0..1 (Yeh & Smith, "Discretization of the '59 Fender Bassman tone stack"). */
    inline void fmvAnalog (const Stack& k, double t, double m, double l, double* b, double* a) noexcept
    {
        const double R1 = k.r1 * 1.0e3, R2 = k.r2 * 1.0e3, R3 = k.r3 * 1.0e3, R4 = k.r4 * 1.0e3;
        const double C1 = k.c1 * 1.0e-9, C2 = k.c2 * 1.0e-9, C3 = k.c3 * 1.0e-9;
        b[0] = 0.0;
        b[1] = t*C1*R1 + m*C3*R3 + l*(C1*R2 + C2*R2) + (C1*R3 + C2*R3);
        b[2] = t*(C1*C2*R1*R4 + C1*C3*R1*R4) - m*m*(C1*C3*R3*R3 + C2*C3*R3*R3) + m*(C1*C3*R1*R3 + C1*C3*R3*R3 + C2*C3*R3*R3)
             + l*(C1*C2*R1*R2 + C1*C2*R2*R4 + C1*C3*R2*R4) + l*m*(C1*C3*R2*R3 + C2*C3*R2*R3) + (C1*C2*R1*R3 + C1*C2*R3*R4 + C1*C3*R3*R4);
        b[3] = l*m*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4) - m*m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
             + m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4) + t*C1*C2*C3*R1*R3*R4 - t*m*C1*C2*C3*R1*R3*R4 + t*l*C1*C2*C3*R1*R2*R4;
        a[0] = 1.0;
        a[1] = (C1*R1 + C1*R3 + C2*R3 + C2*R4 + C3*R4) + m*C3*R3 + l*(C1*R2 + C2*R2);
        a[2] = m*(C1*C3*R1*R3 - C2*C3*R3*R4 + C1*C3*R3*R3 + C2*C3*R3*R3) + l*m*(C1*C3*R2*R3 + C2*C3*R2*R3) - m*m*(C1*C3*R3*R3 + C2*C3*R3*R3)
             + l*(C1*C2*R2*R4 + C1*C2*R1*R2 + C1*C3*R2*R4 + C2*C3*R2*R4)
             + (C1*C2*R1*R4 + C1*C3*R1*R4 + C1*C2*R3*R4 + C1*C2*R1*R3 + C1*C3*R3*R4 + C2*C3*R3*R4);
        a[3] = l*m*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4) - m*m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
             + m*(C1*C2*C3*R3*R3*R4 + C1*C2*C3*R1*R3*R3 - C1*C2*C3*R1*R3*R4) + l*C1*C2*C3*R1*R2*R4 + C1*C2*C3*R1*R3*R4;
    }

    /** |H (j 2 pi hz)| of the analog stack. */
    inline double fmvMagnitude (const double* b, const double* a, double hz) noexcept
    {
        const double w = 2.0 * pi * hz;
        const double nr = -b[2] * w * w, ni = b[1] * w - b[3] * w * w * w;
        const double dr = a[0] - a[2] * w * w, di = a[1] * w - a[3] * w * w * w;
        return std::sqrt ((nr * nr + ni * ni) / (dr * dr + di * di));
    }

    /** The same stack as a state-space system on its three capacitor voltages (C1: input - treble pot, C2 and C3: from
        the slope resistor to the bass and the middle pot), discretised with the trapezoidal rule:
            x[n+1] = P x[n] + Q (u[n] + u[n+1]),   y[n] = C x[n] + D u[n]
        which has exactly the bilinear-transformed response of H (s). Unlike a direct-form filter its state means the
        same thing whatever the pot positions, so turning a knob neither clicks nor zippers.
        P: 3x3 (row-major), Q: 3, C: 3; `gain` scales the output. Needs m > 0 and l > 0 (see midTaper / bassTaper). */
    inline void fmvStateSpace (const Stack& k, double t, double m, double l, double fs, double gain, double* P, double* Q, double* C, double& D) noexcept
    {
        const double C1 = k.c1 * 1.0e-9, C2 = k.c2 * 1.0e-9, C3 = k.c3 * 1.0e-9;
        const double G1 = 1.0 / (k.r1 * 1.0e3), G4 = 1.0 / (k.r4 * 1.0e3), G3 = 1.0 / (m * k.r3 * 1.0e3);
        const double Gs = 1.0 / (l * k.r2 * 1.0e3 + (1.0 - m) * k.r3 * 1.0e3); // bass pot in series with the top of the middle pot

        // the voltage at the slope resistor's far end: vB = bu u + b1 x1 + b2 x2 + b3 x3
        const double Gt = G1 + G4 + G3, bu = (G4 + G1) / Gt, b1 = -G1 / Gt, b2 = G1 / Gt, b3 = G3 / Gt;
        // x' = A x + B u
        const double A[9] = { G1 * (-1.0 - b1) / C1,  G1 * (1.0 - b2) / C1,          -G1 * b3 / C1,
                              -G1 * (-1.0 - b1) / C2, (-Gs - G1 * (1.0 - b2)) / C2,  (Gs + G1 * b3) / C2,
                              G3 * b1 / C3,           (G3 * b2 + Gs) / C3,           (G3 * (b3 - 1.0) - Gs) / C3 };
        const double B[3] = { G1 * (1.0 - bu) / C1, -G1 * (1.0 - bu) / C2, G3 * bu / C3 };

        // P = (I - h A)^-1 (I + h A), Q = (I - h A)^-1 h B with h = T / 2
        const double h = 0.5 / fs;
        double M[9], N[9];
        for (int i = 0; i < 9; ++i)
        {
            M[i] = -h * A[i];
            N[i] = h * A[i];
        }
        M[0] += 1.0; M[4] += 1.0; M[8] += 1.0;
        N[0] += 1.0; N[4] += 1.0; N[8] += 1.0;

        const double c00 = M[4] * M[8] - M[5] * M[7], c01 = M[5] * M[6] - M[3] * M[8], c02 = M[3] * M[7] - M[4] * M[6];
        const double id = 1.0 / (M[0] * c00 + M[1] * c01 + M[2] * c02);
        const double inv[9] = { c00 * id, (M[2] * M[7] - M[1] * M[8]) * id, (M[1] * M[5] - M[2] * M[4]) * id,
                                c01 * id, (M[0] * M[8] - M[2] * M[6]) * id, (M[2] * M[3] - M[0] * M[5]) * id,
                                c02 * id, (M[1] * M[6] - M[0] * M[7]) * id, (M[0] * M[4] - M[1] * M[3]) * id };
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                P[3 * r + c] = inv[3 * r] * N[c] + inv[3 * r + 1] * N[3 + c] + inv[3 * r + 2] * N[6 + c];
            Q[r] = h * (inv[3 * r] * B[0] + inv[3 * r + 1] * B[1] + inv[3 * r + 2] * B[2]);
        }

        // the treble pot's wiper, between the C1 end (u - x1) and the bass end (vB - x2)
        C[0] = gain * ((1.0 - t) * b1 - t);
        C[1] = gain * (1.0 - t) * (b2 - 1.0);
        C[2] = gain * (1.0 - t) * b3;
        D = gain * ((1.0 - t) * bu + t);
    }

    /** The bass pot's taper (a log pot: about 17 % of its resistance at noon; never quite 0 ohms). */
    inline double bassTaper (double x) noexcept { return 0.01 + 0.99 * x * x * (0.4 + 0.6 * x); }
    /** The middle pot (linear; never quite 0 ohms). */
    inline double midTaper (double x) noexcept { return 0.02 + 0.98 * x; }

    /** Trapezoidal state-variable filter (A. Simper's form): c = { a1, a2, a3, m0, m1, m2 } for
            v3 = x - ic2;  v1 = a1 ic1 + a2 v3;  v2 = ic2 + a2 ic1 + a3 v3;  ic1 = 2 v1 - ic1;  ic2 = 2 v2 - ic2;
            y = m0 x + m1 v1 + m2 v2
        Used for the three-band EQ because, like the state-space stack, it stays quiet while its settings move. */
    inline void svfSet (double* c, double g, double k, double m0, double m1, double m2) noexcept
    {
        c[0] = 1.0 / (1.0 + g * (g + k));
        c[1] = g * c[0];
        c[2] = g * c[1];
        c[3] = m0;
        c[4] = m1;
        c[5] = m2;
    }

    inline double svfG (double fs, double fc) noexcept { return std::tan (pi * std::clamp (fc, 1.0, 0.45 * fs) / fs); }

    inline void svfShelf (double* c, double fs, double fc, double gainDb, bool high) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0), k = 1.4142135623730951;
        if (high)
            svfSet (c, svfG (fs, fc) * std::sqrt (A), k, A * A, k * (1.0 - A) * A, 1.0 - A * A);
        else
            svfSet (c, svfG (fs, fc) / std::sqrt (A), k, 1.0, k * (A - 1.0), A * A - 1.0);
    }

    inline void svfPeak (double* c, double fs, double fc, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0), k = 1.0 / (q * A);
        svfSet (c, svfG (fs, fc), k, 1.0, k * (A * A - 1.0), 0.0);
    }

    inline void svfLowPass (double* c, double fs, double fc, double q) noexcept
    {
        svfSet (c, svfG (fs, fc), 1.0 / q, 0.0, 0.0, 1.0);
    }

    /** RBJ shelf / peak / low-pass / high-pass biquads with double coefficients { b0, b1, b2, a1, a2 }, for the fixed
        output-transformer and speaker-impedance filters (float state is too coarse for 20 Hz corners). */
    inline void eqSet (double* c, double b0, double b1, double b2, double a0, double a1, double a2) noexcept
    {
        c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = a1 / a0; c[4] = a2 / a0;
    }

    inline void eqShelf (double* c, double fs, double fc, double gainDb, bool high) noexcept
    {
        const double w0 = 2.0 * pi * std::clamp (fc, 1.0, 0.45 * fs) / fs, cosw = std::cos (w0), alpha = std::sin (w0) / (2.0 * 0.70710678);
        const double A = std::pow (10.0, gainDb / 40.0), k = 2.0 * std::sqrt (A) * alpha, sgn = high ? -1.0 : 1.0;
        eqSet (c, A * ((A + 1.0) - sgn * (A - 1.0) * cosw + k), sgn * 2.0 * A * ((A - 1.0) - sgn * (A + 1.0) * cosw), A * ((A + 1.0) - sgn * (A - 1.0) * cosw - k),
               (A + 1.0) + sgn * (A - 1.0) * cosw + k, -sgn * 2.0 * ((A - 1.0) + sgn * (A + 1.0) * cosw), (A + 1.0) + sgn * (A - 1.0) * cosw - k);
    }

    inline void eqPeak (double* c, double fs, double fc, double q, double gainDb) noexcept
    {
        const double w0 = 2.0 * pi * std::clamp (fc, 1.0, 0.45 * fs) / fs, cosw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double A = std::pow (10.0, gainDb / 40.0);
        eqSet (c, 1.0 + alpha * A, -2.0 * cosw, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * cosw, 1.0 - alpha / A);
    }

    inline void eqLowPass (double* c, double fs, double fc, double q) noexcept
    {
        const double w0 = 2.0 * pi * std::clamp (fc, 1.0, 0.45 * fs) / fs, cosw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        eqSet (c, (1.0 - cosw) * 0.5, 1.0 - cosw, (1.0 - cosw) * 0.5, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
    }

    inline void eqHighPass (double* c, double fs, double fc, double q) noexcept
    {
        const double w0 = 2.0 * pi * std::clamp (fc, 1.0, 0.45 * fs) / fs, cosw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        eqSet (c, (1.0 + cosw) * 0.5, -(1.0 + cosw), (1.0 + cosw) * 0.5, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
    }

    /** Decaying filter states are cleared before they turn into denormal numbers (the JavaScript port has no
        flush-to-zero mode, and both versions must do the same). */
    inline double flush (double v) noexcept { return std::abs (v) < 1.0e-30 ? 0.0 : v; }

    /** Coefficient of a topology-preserving one-pole at the rate `fs` (see OnePole). */
    inline double onePoleG (double fs, double fc) noexcept
    {
        const double g = std::tan (pi * std::clamp (fc, 1.0, 0.45 * fs) / fs);
        return g / (1.0 + g);
    }

    //==============================================================================
    enum ToneType { toneStack = 0, toneEq };
    enum MidMode  { midNormal = 0, // Bass / Mid / Treble as labelled
                    midCut,        // Mid = Vox "Cut" (a treble cut across the phase inverter), no middle control
                    midTone,       // Mid = the amp's single Tone knob (treble cut in the preamp)
                    midDivide };   // Divide 9/15: Drive = clean-channel level, Bass = dirty-channel drive, Mid = Tone, Treble = Cut
    enum Flags    { flagParallel = 1, // stages 0 and 1 are two channels fed in parallel and mixed
                    flagInteract = 2 }; // Presence and Mid interact (Line 6 Elektrik)

    /** One amp. */
    struct Spec
    {
        const char* key; const char* name; const char* basedOn;
        int numStages; Stage stage[maxStages];
        int drivePos;         // the stage the Drive control (the amp's volume / gain pot) sits in front of
        float driveRangeDb;   // the pot's range below full up
        float brightHz;       // bright cap across that pot: 1 / (2 pi R C), 0 = none
        int tonePos;          // the tone stack sits in front of this stage (numStages = after the last one)
        int toneType, stack, eq, midMode, flags;
        float paDrive;        // how hard the preamp at full level drives the power amp with Master at 100 %
        float piLimit;        // phase inverter headroom, in units of the output tubes' clipping level
        float feedback;       // negative feedback loop gain (0 = none: soft, loose; 1+ = stiff, hard knee)
        float presenceHz, presenceDb;
        float match;          // 0 = single-ended, 0.9+ = push-pull pair
        float sagDepth;       // supply drop at full power with Sag at 50 % (tube rectifiers ~0.3, silicon ~0.1)
        float lowHz, highHz;  // output transformer band limits
        float resHz, resDb;   // the speaker's resonance seen through the amp's output impedance
        float def[numKnobs - 1];
    };

    constexpr int numStock = 30, numAmps = 50;

    inline constexpr Spec specs[numAmps] = {
        // '65 Twin Reverb, Normal: 12AX7 -> blackface stack -> volume -> 12AX7 -> 4x6L6 with lots of feedback, silicon rectifier.
        // Very high headroom; the scoop at 400-500 Hz and the glassy top come from the stack.
        { "blackface_double_normal", "Blackface Double Normal", "'65 Fender Twin Reverb, Normal channel",
          2, { tri (0.9f, 10.0f, 16000.0f), tri (30.0f, 8.0f, 14000.0f), noStage, noStage },
          1, 46.0f, 0.0f, 1, toneStack, stackBlackface, eqGeneric, midNormal, 0,
          1.6f, 2.5f, 1.0f, 3000.0f, 6.0f, 0.93f, 0.10f, 40.0f, 11000.0f, 95.0f, 1.0f,
          { 45, 45, 55, 60, 35, 50, 100, 30, 50, 50, 40 } },
        // Twin Reverb, Vibrato: bright cap on the volume and a third 12AX7 after the reverb mixer, which clips first.
        { "blackface_double_vibrato", "Blackface Double Vibrato", "'65 Fender Twin Reverb, Vibrato channel",
          3, { tri (0.9f, 10.0f, 16000.0f), tri (30.0f, 8.0f, 14000.0f), tri (1.7f, 8.0f, 12000.0f), noStage },
          1, 46.0f, 2650.0f, 1, toneStack, stackBlackface, eqGeneric, midNormal, 0,
          1.3f, 2.5f, 1.0f, 3000.0f, 6.0f, 0.93f, 0.10f, 40.0f, 11000.0f, 95.0f, 1.0f,
          { 45, 45, 55, 55, 35, 50, 100, 30, 50, 50, 40 } },
        // Hiwatt DR103: stiff, linear, loud. Flat-ish stack after the second stage, 4xEL34 with heavy feedback.
        { "hiway_100", "Hiway 100", "Hiwatt Custom 100 (DR103)",
          3, { tri (0.9f, 15.0f, 18000.0f), tri (22.0f, 20.0f, 16000.0f), tri (2.2f, 20.0f, 16000.0f), noStage },
          1, 44.0f, 0.0f, 2, toneStack, stackHiwatt, eqGeneric, midNormal, 0,
          1.8f, 2.5f, 1.2f, 2500.0f, 6.0f, 0.95f, 0.08f, 35.0f, 14000.0f, 95.0f, 0.6f,
          { 50, 50, 55, 55, 45, 50, 75, 25, 50, 50, 35 } },
        // Supro S6616: two triodes into one single-ended 6V6, no feedback, small transformer. One Tone knob (on Mid).
        { "super_o", "Super O", "'60s Supro S6616",
          2, { tri (1.1f, 30.0f, 9000.0f), tri (13.0f, 40.0f, 7000.0f), noStage, noStage },
          1, 40.0f, 0.0f, 2, toneEq, 0, eqSupro, midTone, 0,
          3.0f, 2.0f, 0.0f, 3500.0f, 5.0f, 0.0f, 0.30f, 90.0f, 6500.0f, 110.0f, 2.5f,
          { 55, 50, 60, 50, 40, 50, 100, 55, 50, 85, 50 } },
        // Gibson EH-185 (1939): octal preamp, 2x6L6 without feedback, dark and soft.
        { "gibtone_185", "Gibtone 185", "Gibson EH-185",
          2, { octal (0.7f, 25.0f, 8000.0f), octal (7.0f, 30.0f, 6500.0f), noStage, noStage },
          1, 40.0f, 0.0f, 1, toneEq, 0, eqGibson, midNormal, 0,
          2.5f, 2.0f, 0.0f, 3000.0f, 5.0f, 0.88f, 0.35f, 70.0f, 5500.0f, 85.0f, 2.5f,
          { 60, 55, 50, 55, 40, 50, 100, 55, 50, 60, 45 } },
        // '59 Bassman 5F6-A, Normal: 12AY7 -> volume -> 12AX7 -> cathode follower -> stack -> long-tail inverter -> 2x5881,
        // GZ34 rectifier, a real Presence control in the feedback loop.
        { "tweed_b_man_normal", "Tweed B-Man Normal", "'59 Fender Tweed Bassman, Normal channel",
          3, { octal (0.7f, 12.0f, 15000.0f), tri (22.0f, 15.0f, 12000.0f), cf (10.0f, 14000.0f), noStage },
          1, 42.0f, 0.0f, 3, toneStack, stackBassman, eqGeneric, midNormal, 0,
          3.5f, 2.5f, 0.5f, 1500.0f, 8.0f, 0.92f, 0.30f, 45.0f, 9000.0f, 100.0f, 1.5f,
          { 55, 45, 55, 55, 45, 50, 100, 50, 50, 50, 45 } },
        // Bassman, Bright: the bright cap across the volume pot and the leaner half of the first tube.
        { "tweed_b_man_bright", "Tweed B-Man Bright", "'59 Fender Tweed Bassman, Bright channel",
          3, { octal (0.7f, 12.0f, 16000.0f, 0.75f, 300.0f), tri (22.0f, 40.0f, 13000.0f), cf (10.0f, 14000.0f), noStage },
          1, 42.0f, 2500.0f, 3, toneStack, stackBassman, eqGeneric, midNormal, 0,
          3.5f, 2.5f, 0.5f, 1500.0f, 8.0f, 0.92f, 0.30f, 45.0f, 9000.0f, 100.0f, 1.5f,
          { 55, 50, 55, 50, 45, 50, 100, 50, 50, 50, 45 } },
        // Deluxe Reverb, Normal: the Twin's preamp into 2x6V6 (22 W) with a GZ34: breaks up early and sags.
        { "blackface_lux_normal", "Blackface 'Lux Normal", "Fender Blackface Deluxe Reverb, Normal channel",
          2, { tri (0.9f, 10.0f, 15000.0f), tri (30.0f, 10.0f, 13000.0f), noStage, noStage },
          1, 46.0f, 0.0f, 1, toneStack, stackBlackface, eqGeneric, midNormal, 0,
          4.2f, 2.2f, 0.7f, 3000.0f, 6.0f, 0.92f, 0.35f, 60.0f, 8000.0f, 100.0f, 1.5f,
          { 42, 50, 55, 55, 35, 50, 100, 50, 50, 50, 45 } },
        // Deluxe Reverb, Vibrato: the fixed 47 pF bright cap and the third stage.
        { "blackface_lux_vibrato", "Blackface 'Lux Vibrato", "Fender Blackface Deluxe Reverb, Vibrato channel",
          3, { tri (0.9f, 10.0f, 15000.0f), tri (30.0f, 10.0f, 13000.0f), tri (1.7f, 10.0f, 11000.0f), noStage },
          1, 46.0f, 3386.0f, 1, toneStack, stackBlackface, eqGeneric, midNormal, 0,
          3.4f, 2.2f, 0.7f, 3000.0f, 6.0f, 0.92f, 0.35f, 60.0f, 8000.0f, 100.0f, 1.5f,
          { 42, 50, 55, 55, 35, 50, 100, 50, 50, 50, 45 } },
        // Divided by 13 JRT 9/15: two 5879 pentode channels mixed (clean level on Drive, dirty drive on Bass),
        // one Tone (Mid) and a Cut (Treble), cathode-biased output pair without feedback.
        { "divide_9_15", "Divide 9/15", "Divided by 13 JRT 9/15",
          2, { pent (5.0f, 20.0f, 13000.0f), pent (24.0f, 45.0f, 10000.0f, 0.7f, 250.0f), noStage, noStage },
          0, 36.0f, 0.0f, 2, toneEq, 0, eqDivide, midDivide, flagParallel,
          3.0f, 2.0f, 0.0f, 3200.0f, 5.0f, 0.90f, 0.30f, 65.0f, 9000.0f, 100.0f, 2.0f,
          { 55, 45, 60, 70, 40, 50, 100, 50, 50, 70, 50 } },
        // Dr. Z Route 66: one EF86 pentode, bass / treble, 2xKT66 with an ultra-linear transformer and GZ34.
        { "phd_motorway", "PhD Motorway", "Dr. Z Route 66",
          2, { pent (2.2f, 20.0f, 12000.0f), tri (4.5f, 15.0f, 12000.0f), noStage, noStage },
          1, 40.0f, 0.0f, 1, toneEq, 0, eqRoute, midNormal, 0,
          2.2f, 2.5f, 0.35f, 3000.0f, 5.0f, 0.93f, 0.28f, 40.0f, 13000.0f, 95.0f, 1.5f,
          { 55, 50, 50, 55, 45, 50, 100, 45, 50, 55, 45 } },
        // '61 Vox AC15, EF86 channel: pentode into the inverter, 2xEL84 cathode-biased, no feedback, EZ81. Cut on Mid.
        { "class_a_15", "Class A-15", "'61 \"Fawn\" Vox AC-15",
          2, { pent (2.0f, 25.0f, 11000.0f), tri (4.0f, 25.0f, 12000.0f), noStage, noStage },
          1, 40.0f, 0.0f, 1, toneEq, 0, eqVox, midCut, 0,
          2.8f, 2.0f, 0.0f, 3500.0f, 5.0f, 0.90f, 0.40f, 70.0f, 9000.0f, 100.0f, 2.5f,
          { 55, 50, 70, 55, 45, 50, 100, 60, 50, 80, 50 } },
        // Vox AC30 Top Boost: 12AX7 -> volume -> 12AX7 -> cathode follower -> Top Boost treble / bass -> 4xEL84,
        // cathode-biased, no feedback, GZ34. Cut on Mid.
        { "class_a_30_tb", "Class A-30 TB", "Vox AC-30 \"Top Boost\"",
          3, { tri (1.0f, 25.0f, 14000.0f), tri (22.0f, 30.0f, 13000.0f), cf (12.0f, 14000.0f), noStage },
          1, 42.0f, 0.0f, 3, toneStack, stackVox, eqGeneric, midCut, 0,
          3.0f, 2.0f, 0.0f, 3500.0f, 5.0f, 0.90f, 0.35f, 60.0f, 10000.0f, 95.0f, 2.5f,
          { 55, 45, 70, 60, 45, 50, 100, 55, 50, 75, 50 } },
        // '65 JTM45, Normal: the Bassman circuit with ECC83s and KT66s: fat, saggy (GZ34), heavy feedback.
        { "brit_j_45_normal", "Brit J-45 Normal", "'65 Marshall JTM-45 MkII, Normal channel",
          3, { tri (0.9f, 10.0f, 12000.0f), tri (24.0f, 15.0f, 10000.0f), cf (10.0f, 12000.0f), noStage },
          1, 42.0f, 0.0f, 3, toneStack, stackJtm45, eqGeneric, midNormal, 0,
          3.5f, 2.5f, 0.8f, 1800.0f, 8.0f, 0.92f, 0.40f, 50.0f, 8500.0f, 100.0f, 1.2f,
          { 60, 50, 55, 55, 45, 50, 100, 55, 50, 50, 50 } },
        // JTM45, Bright: bright cap on the volume and the treble-peaking mixer network.
        { "brit_j_45_bright", "Brit J-45 Bright", "'65 Marshall JTM-45 MkII, Bright channel",
          3, { tri (0.9f, 10.0f, 14000.0f), tri (24.0f, 40.0f, 12000.0f, 0.55f, 700.0f), cf (10.0f, 12000.0f), noStage },
          1, 42.0f, 2500.0f, 3, toneStack, stackJtm45, eqGeneric, midNormal, 0,
          3.5f, 2.5f, 0.8f, 1800.0f, 8.0f, 0.92f, 0.40f, 50.0f, 8500.0f, 100.0f, 1.2f,
          { 60, 50, 55, 50, 45, 50, 100, 55, 50, 50, 50 } },
        // Marshall 1959 Super Lead, Normal: fully bypassed first stage, 4xEL34, moderate feedback, silicon rectifier.
        // Most of the crunch is the inverter and the output tubes.
        { "plexi_lead_100_normal", "Plexi Lead 100 Normal", "'59 Marshall \"Plexi\" Super Lead 100, Normal channel",
          3, { tri (0.9f, 10.0f, 13000.0f), tri (28.0f, 20.0f, 11000.0f), cf (10.0f, 13000.0f), noStage },
          1, 42.0f, 0.0f, 3, toneStack, stackPlexi, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.5f, 2000.0f, 8.0f, 0.93f, 0.20f, 50.0f, 9000.0f, 100.0f, 1.5f,
          { 65, 50, 55, 55, 50, 50, 100, 40, 50, 50, 50 } },
        // Super Lead, Bright: 2.7k / 0.68 uF cathode, 2.2 nF coupling, the 5 nF bright cap and 470 pF across the mixer.
        { "plexi_lead_100_bright", "Plexi Lead 100 Bright", "'59 Marshall \"Plexi\" Super Lead 100, Bright channel",
          3, { tri (0.9f, 60.0f, 15000.0f, 0.6f, 90.0f), tri (28.0f, 30.0f, 12000.0f, 0.5f, 720.0f), cf (10.0f, 13000.0f), noStage },
          1, 42.0f, 100.0f, 3, toneStack, stackPlexi, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.5f, 2000.0f, 8.0f, 0.93f, 0.20f, 50.0f, 9000.0f, 100.0f, 1.5f,
          { 60, 55, 55, 50, 45, 50, 100, 40, 50, 50, 50 } },
        // Park 75, Normal: a hotter plexi front end into KT88s: more gain, more headroom and tighter lows in the power amp.
        { "brit_p_75_normal", "Brit P-75 Normal", "Park 75, Normal channel",
          3, { tri (1.0f, 10.0f, 13000.0f), tri (38.0f, 20.0f, 11000.0f), cf (10.0f, 13000.0f), noStage },
          1, 42.0f, 0.0f, 3, toneStack, stackPlexi, eqGeneric, midNormal, 0,
          3.5f, 2.8f, 0.6f, 2200.0f, 8.0f, 0.94f, 0.18f, 40.0f, 10000.0f, 95.0f, 1.2f,
          { 60, 50, 55, 55, 50, 50, 100, 35, 50, 50, 45 } },
        // Park 75, Bright.
        { "brit_p_75_bright", "Brit P-75 Bright", "Park 75, Bright channel",
          3, { tri (1.0f, 60.0f, 15000.0f, 0.6f, 90.0f), tri (38.0f, 30.0f, 12000.0f, 0.55f, 720.0f), cf (10.0f, 13000.0f), noStage },
          1, 42.0f, 150.0f, 3, toneStack, stackPlexi, eqGeneric, midNormal, 0,
          3.5f, 2.8f, 0.6f, 2200.0f, 8.0f, 0.94f, 0.18f, 40.0f, 10000.0f, 95.0f, 1.2f,
          { 55, 55, 55, 50, 45, 50, 100, 35, 50, 50, 45 } },
        // JCM800 2204: gain pot (470 pF bright) -> cold clipper -> 470k / 470k with 470 pF -> third stage -> follower ->
        // stack -> master -> 2xEL34. The distortion is the preamp's: tight, upper-mid bark.
        { "brit_j_800", "Brit J-800", "Marshall JCM-800 (2204)",
          4, { tri (1.0f, 15.0f, 14000.0f, 0.6f, 90.0f), cold (30.0f, 30.0f, 12000.0f), tri (6.0f, 40.0f, 9000.0f, 0.5f, 720.0f), cf (10.0f, 12000.0f) },
          1, 48.0f, 339.0f, 4, toneStack, stackPlexi, eqGeneric, midNormal, 0,
          5.0f, 2.5f, 0.6f, 2000.0f, 8.0f, 0.93f, 0.20f, 50.0f, 9000.0f, 100.0f, 1.5f,
          { 60, 50, 60, 55, 45, 50, 45, 40, 50, 50, 50 } },
        // Bogner Uberschall: four stages of gain with the bass cut before the clipping and put back by the power amp's
        // deep resonance: huge but tight low end, dark smooth top.
        { "bomber_uber", "Bomber Uber", "2002 Bogner Uberschall",
          4, { tri (1.2f, 20.0f, 14000.0f, 0.45f, 170.0f), tri (34.0f, 130.0f, 8000.0f), cold (12.0f, 80.0f, 7000.0f), tri (7.0f, 40.0f, 5500.0f) },
          1, 52.0f, 0.0f, 4, toneStack, stackSoldano, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.9f, 2500.0f, 8.0f, 0.93f, 0.20f, 35.0f, 7000.0f, 80.0f, 4.5f,
          { 55, 55, 45, 55, 40, 50, 45, 35, 50, 50, 50 } },
        // Mesa Dual Rectifier (modern): cascaded gain into the stack, 6L6s with almost no feedback (loose, growling
        // lows and a raw top), tube-rectifier sag.
        { "treadplate", "Treadplate", "Mesa/Boogie Dual Rectifier",
          4, { tri (1.2f, 20.0f, 15000.0f, 0.7f, 100.0f), tri (28.0f, 60.0f, 12000.0f), cold (10.0f, 40.0f, 11000.0f), tri (6.0f, 30.0f, 9000.0f) },
          1, 52.0f, 0.0f, 4, toneStack, stackRecto, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.15f, 4000.0f, 7.0f, 0.93f, 0.30f, 38.0f, 12000.0f, 90.0f, 3.5f,
          { 55, 55, 40, 65, 55, 50, 45, 50, 50, 50, 50 } },
        // Engl Fireball 100: very tight (high coupling corners, lean first stage), mid-forward, stiff 6L6 power amp.
        { "angel_f_ball", "Angel F-Ball", "Engl Fireball 100",
          4, { tri (1.3f, 25.0f, 14000.0f, 0.45f, 200.0f), tri (36.0f, 160.0f, 8500.0f), cold (14.0f, 110.0f, 7500.0f), tri (8.0f, 60.0f, 6000.0f) },
          1, 52.0f, 0.0f, 4, toneStack, stackEngl, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.8f, 2800.0f, 8.0f, 0.94f, 0.15f, 45.0f, 9500.0f, 95.0f, 2.5f,
          { 55, 50, 55, 55, 50, 50, 45, 30, 50, 50, 45 } },
        // Line 6 Elektrik: Line 6's own high-gain amp; Presence and Mid interact (Mid moves the presence corner).
        { "line6_elektrik", "Line 6 Elektrik", "Line 6 original (high gain)",
          4, { tri (1.2f, 20.0f, 14000.0f, 0.5f, 130.0f), tri (38.0f, 120.0f, 9000.0f), cold (14.0f, 80.0f, 8500.0f), tri (9.0f, 50.0f, 7000.0f) },
          1, 52.0f, 0.0f, 4, toneStack, stackRecto, eqGeneric, midNormal, flagInteract,
          4.5f, 2.5f, 0.5f, 2200.0f, 9.0f, 0.93f, 0.25f, 40.0f, 10000.0f, 90.0f, 3.0f,
          { 60, 50, 50, 55, 50, 50, 45, 40, 50, 50, 50 } },
        // Soldano SLO-100, Normal channel, Clean: two stages and the follower, stiff 6L6 power amp.
        { "solo_100_clean", "Solo 100 Clean", "'93 Soldano SLO-100, Normal channel (Clean)",
          3, { tri (1.0f, 15.0f, 15000.0f), tri (14.0f, 20.0f, 13000.0f), cf (10.0f, 14000.0f), noStage },
          1, 40.0f, 0.0f, 3, toneStack, stackSoldano, eqGeneric, midNormal, 0,
          2.5f, 2.5f, 0.8f, 2500.0f, 8.0f, 0.93f, 0.15f, 40.0f, 11000.0f, 95.0f, 2.0f,
          { 45, 50, 50, 55, 45, 50, 60, 35, 50, 50, 45 } },
        // SLO-100, Normal channel, Crunch: the same channel with its extra gain switched in.
        { "solo_100_crunch", "Solo 100 Crunch", "'93 Soldano SLO-100, Normal channel (Crunch)",
          3, { tri (1.0f, 15.0f, 15000.0f, 0.7f, 100.0f), tri (30.0f, 40.0f, 11000.0f), tri (3.5f, 30.0f, 10000.0f), noStage },
          1, 40.0f, 0.0f, 3, toneStack, stackSoldano, eqGeneric, midNormal, 0,
          3.5f, 2.5f, 0.8f, 2500.0f, 8.0f, 0.93f, 0.15f, 40.0f, 11000.0f, 95.0f, 2.0f,
          { 55, 50, 55, 55, 45, 50, 50, 35, 50, 50, 45 } },
        // SLO-100, Overdrive: the cascade everybody copied: smooth, singing, saturated, with the Depth-like low resonance.
        { "solo_100_od", "Solo 100 OD", "'93 Soldano SLO-100, Overdrive channel",
          4, { tri (1.2f, 20.0f, 14000.0f, 0.55f, 130.0f), tri (45.0f, 100.0f, 10000.0f), cold (13.0f, 70.0f, 9000.0f, 0.6f, 1000.0f), tri (7.0f, 40.0f, 7500.0f) },
          1, 52.0f, 0.0f, 4, toneStack, stackSoldano, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.8f, 2500.0f, 8.0f, 0.93f, 0.15f, 40.0f, 10000.0f, 95.0f, 3.0f,
          { 55, 50, 55, 55, 50, 50, 45, 35, 50, 50, 50 } },
        // Line 6 Doom: a modded JCM800 preamp (more gain, full bass) into a Hiwatt power amp, with a lot of sag.
        { "line6_doom", "Line 6 Doom", "Line 6 original (JCM800 preamp into a Hiwatt power amp)",
          4, { tri (1.3f, 10.0f, 12000.0f), cold (34.0f, 25.0f, 9000.0f), tri (10.0f, 25.0f, 7000.0f), cf (10.0f, 10000.0f) },
          1, 48.0f, 0.0f, 4, toneStack, stackPlexi, eqGeneric, midNormal, 0,
          4.0f, 2.5f, 1.0f, 2500.0f, 6.0f, 0.95f, 0.50f, 30.0f, 8000.0f, 80.0f, 4.0f,
          { 70, 65, 45, 45, 35, 50, 60, 75, 50, 50, 60 } },
        // Line 6 Epic: the most gain of the stock amps, compressed so that it sustains at any playing level; smooth top.
        { "line6_epic", "Line 6 Epic", "Line 6 original (high gain, endless sustain)",
          4, { tri (1.4f, 25.0f, 14000.0f, 0.5f, 140.0f), tri (45.0f, 120.0f, 8000.0f), cold (18.0f, 90.0f, 7000.0f), tri (12.0f, 50.0f, 5500.0f) },
          1, 52.0f, 0.0f, 4, toneStack, stackSoldano, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.6f, 2500.0f, 8.0f, 0.93f, 0.25f, 40.0f, 7000.0f, 90.0f, 3.0f,
          { 65, 50, 55, 50, 45, 50, 45, 45, 50, 50, 55 } },
        // Ampeg B-15NF Portaflex (bass): octal preamp with a Baxandall bass / treble, 2x6L6 at 25-30 W, 5AR4: round, full lows.
        { "flip_top", "Flip Top", "Ampeg B-15NF Portaflex",
          2, { octal (0.8f, 8.0f, 9000.0f), octal (6.0f, 8.0f, 8000.0f), noStage, noStage },
          1, 40.0f, 0.0f, 1, toneEq, 0, eqFlipTop, midNormal, 0,
          2.2f, 2.2f, 0.6f, 2500.0f, 5.0f, 0.92f, 0.30f, 25.0f, 6000.0f, 55.0f, 3.0f,
          { 45, 55, 50, 45, 35, 50, 100, 45, 50, 55, 45 } },
        // ---- HD model packs: Metal ----
        // Peavey 5150 (block logo), lead channel: five cascaded stages' worth of gain, tight before the clipping, cold-biased
        // 6L6s (the default Bias is low: a little crossover grit) and a strong resonance.
        { "pv_panama", "PV Panama", "Peavey 5150",
          4, { tri (1.2f, 25.0f, 14000.0f, 0.5f, 130.0f), tri (40.0f, 130.0f, 9000.0f), cold (15.0f, 90.0f, 8000.0f), tri (9.0f, 50.0f, 6500.0f) },
          1, 52.0f, 0.0f, 4, toneStack, stackSoldano, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.7f, 2200.0f, 9.0f, 0.93f, 0.12f, 40.0f, 9500.0f, 100.0f, 4.0f,
          { 55, 55, 45, 55, 55, 50, 45, 30, 50, 38, 50 } },
        // Bogner Shiva, lead channel: medium-high gain, round and smooth, less compressed than the Uberschall.
        { "mahadeva", "Mahadeva", "Bogner Shiva",
          4, { tri (1.0f, 15.0f, 14000.0f, 0.7f, 100.0f), tri (28.0f, 60.0f, 10000.0f), tri (8.0f, 40.0f, 8000.0f), cf (10.0f, 12000.0f) },
          1, 46.0f, 0.0f, 4, toneStack, stackSoldano, eqGeneric, midNormal, 0,
          4.0f, 2.5f, 0.6f, 2500.0f, 7.0f, 0.93f, 0.20f, 45.0f, 10000.0f, 95.0f, 2.0f,
          { 55, 50, 55, 55, 45, 50, 50, 40, 50, 50, 45 } },
        // The "remastered" JCM800 2204: the Brit J-800 circuit, a little hotter and tighter.
        { "brit_2204", "Brit 2204", "Marshall JCM800 (2204), remastered",
          4, { tri (1.0f, 15.0f, 15000.0f, 0.6f, 90.0f), cold (34.0f, 40.0f, 12000.0f), tri (7.0f, 50.0f, 9500.0f, 0.5f, 720.0f), cf (10.0f, 13000.0f) },
          1, 48.0f, 339.0f, 4, toneStack, stackPlexi, eqGeneric, midNormal, 0,
          5.5f, 2.5f, 0.55f, 2000.0f, 8.0f, 0.93f, 0.22f, 50.0f, 9500.0f, 100.0f, 1.8f,
          { 65, 50, 60, 55, 50, 50, 50, 40, 50, 50, 50 } },
        // Line 6 Insane: as much gain as the cascade will take, mid-heavy.
        { "line6_insane", "Line 6 Insane", "Line 6 original (maximum gain)",
          4, { tri (1.5f, 25.0f, 14000.0f, 0.5f, 150.0f), tri (60.0f, 140.0f, 8500.0f), cold (22.0f, 100.0f, 7500.0f), tri (14.0f, 60.0f, 6000.0f) },
          1, 52.0f, 0.0f, 4, toneStack, stackRecto, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.5f, 2500.0f, 8.0f, 0.93f, 0.20f, 40.0f, 9000.0f, 90.0f, 3.0f,
          { 70, 50, 60, 50, 45, 50, 45, 40, 50, 50, 55 } },
        // Line 6 Big Bottom: a high-gain amp voiced for low end: loose coupling, little feedback, a big resonance at 75 Hz.
        { "line6_big_bottom", "Line 6 Big Bottom", "Line 6 original (bass-heavy high gain)",
          4, { tri (1.2f, 12.0f, 14000.0f, 0.85f, 80.0f), tri (34.0f, 45.0f, 9000.0f), cold (12.0f, 35.0f, 8000.0f), tri (7.0f, 25.0f, 6500.0f) },
          1, 52.0f, 0.0f, 4, toneStack, stackRecto, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.3f, 3000.0f, 7.0f, 0.93f, 0.30f, 28.0f, 9500.0f, 75.0f, 6.0f,
          { 55, 65, 35, 55, 45, 50, 45, 45, 50, 50, 50 } },
        // Line 6 Variac'ed Plexi: a Super Lead run on a lowered mains voltage: the power amp gives up early, sags and browns out.
        { "line6_variaced_plexi", "Line 6 Variac'ed Plexi", "Line 6 original (variac-sagged Plexi)",
          3, { tri (1.0f, 30.0f, 13000.0f, 0.7f, 90.0f), tri (34.0f, 25.0f, 10500.0f, 0.6f, 720.0f), cf (10.0f, 12000.0f), noStage },
          1, 42.0f, 200.0f, 3, toneStack, stackPlexi, eqGeneric, midNormal, 0,
          8.0f, 2.0f, 0.4f, 2000.0f, 8.0f, 0.93f, 0.45f, 50.0f, 8000.0f, 100.0f, 2.0f,
          { 70, 50, 60, 55, 45, 50, 100, 65, 50, 60, 60 } },
        // Line 6 Purge: tight and mid-forward (high coupling corners, a wide mid control, stiff power amp).
        { "line6_purge", "Line 6 Purge", "Line 6 original (tight, mid-forward high gain)",
          4, { tri (1.3f, 25.0f, 14000.0f, 0.45f, 220.0f), tri (38.0f, 180.0f, 9500.0f), cold (14.0f, 120.0f, 8500.0f), tri (9.0f, 70.0f, 7500.0f) },
          1, 52.0f, 0.0f, 4, toneStack, stackEngl, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.7f, 3000.0f, 9.0f, 0.94f, 0.12f, 45.0f, 10000.0f, 100.0f, 2.0f,
          { 60, 45, 65, 55, 55, 50, 45, 30, 50, 50, 45 } },
        // Line 6 Aggro: scooped and bright, hardly any feedback: a raw edge on top of a deep low end.
        { "line6_aggro", "Line 6 Aggro", "Line 6 original (scooped, aggressive high gain)",
          4, { tri (1.2f, 20.0f, 15000.0f, 0.5f, 140.0f), tri (36.0f, 110.0f, 11000.0f), cold (14.0f, 80.0f, 10000.0f), tri (8.0f, 45.0f, 8500.0f) },
          1, 52.0f, 0.0f, 4, toneStack, stackRecto, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.2f, 3800.0f, 8.0f, 0.93f, 0.20f, 38.0f, 11000.0f, 85.0f, 4.0f,
          { 60, 60, 35, 65, 55, 50, 45, 40, 50, 50, 50 } },
        // Line 6 Smash: thick and fuzzy: pentode-like stages that clip hard and evenly, a dark Orange-style stack, loose power amp.
        { "line6_smash", "Line 6 Smash", "Line 6 original (thick, fuzzy high gain)",
          4, { tri (1.4f, 15.0f, 12000.0f), pent (30.0f, 50.0f, 8000.0f), pent (10.0f, 40.0f, 6500.0f), cf (10.0f, 10000.0f) },
          1, 48.0f, 0.0f, 4, toneStack, stackOrange, eqGeneric, midNormal, 0,
          3.5f, 2.2f, 0.3f, 2500.0f, 7.0f, 0.92f, 0.35f, 40.0f, 8000.0f, 90.0f, 3.0f,
          { 65, 55, 55, 50, 40, 50, 55, 55, 50, 50, 55 } },
        // Line 6 Octone: a high-gain amp whose second stage works as a half-wave rectifier, which puts an octave overtone on the notes.
        { "line6_octone", "Line 6 Octone", "Line 6 original (high gain with an octave overtone)",
          4, { tri (1.3f, 30.0f, 12000.0f, 0.6f, 200.0f), rect (30.0f, 120.0f, 9000.0f), cold (12.0f, 80.0f, 8000.0f), tri (7.0f, 50.0f, 7000.0f) },
          1, 52.0f, 0.0f, 4, toneStack, stackSoldano, eqGeneric, midNormal, 0,
          4.5f, 2.5f, 0.6f, 2500.0f, 8.0f, 0.93f, 0.20f, 40.0f, 9500.0f, 95.0f, 2.5f,
          { 60, 45, 60, 55, 50, 50, 45, 35, 50, 50, 50 } },
        // ---- Vintage ----
        // Roland JC-120 (the amp, without its chorus): solid state, a Fender-like passive stack, a stiff power amp that stays
        // clean until it clips hard. No sag, hardly any hum.
        { "jazz_rivet", "Jazz Rivet", "Roland JC-120",
          2, { ss (0.8f, 15.0f, 20000.0f), ss (9.0f, 10.0f, 18000.0f), noStage, noStage },
          1, 46.0f, 0.0f, 1, toneStack, stackBlackface, eqGeneric, midNormal, 0,
          2.0f, 3.0f, 1.5f, 4000.0f, 5.0f, 1.0f, 0.02f, 20.0f, 18000.0f, 95.0f, 0.3f,
          { 45, 50, 50, 55, 40, 50, 100, 10, 30, 50, 20 } },
        // Tweed Champ 5F1: two triode stages into one 6V6, single-ended, 5Y3 rectifier, no tone controls at all.
        { "small_tweed", "Small Tweed", "Fender Champ (tweed)",
          2, { tri (1.0f, 25.0f, 12000.0f), tri (9.0f, 30.0f, 9000.0f), noStage, noStage },
          1, 40.0f, 0.0f, 1, toneEq, 0, eqChamp, midNormal, 0,
          3.5f, 2.0f, 0.3f, 3000.0f, 5.0f, 0.0f, 0.40f, 95.0f, 7000.0f, 120.0f, 2.5f,
          { 60, 50, 50, 55, 40, 50, 100, 60, 50, 85, 50 } },
        // Orange OR80: a thick, mid-heavy crunch that turns fuzzy, EL34s with little feedback.
        { "mandarin_80", "Mandarin 80", "Orange OR80",
          3, { tri (1.0f, 20.0f, 12000.0f), tri (30.0f, 35.0f, 9000.0f, 0.7f, 400.0f), cf (10.0f, 11000.0f), noStage },
          1, 44.0f, 0.0f, 3, toneStack, stackOrange, eqGeneric, midNormal, 0,
          4.0f, 2.5f, 0.35f, 2000.0f, 6.0f, 0.93f, 0.25f, 45.0f, 8500.0f, 95.0f, 2.0f,
          { 65, 50, 55, 55, 45, 50, 100, 45, 50, 50, 50 } },
        // Vox AC30 "Fawn" (before Top Boost), Normal: one triode into the inverter, 4xEL84, no feedback. Warm; Cut on Mid.
        { "a30_fawn_nrm", "A30 Fawn Nrm", "Vox AC30 \"Fawn\", Normal channel",
          2, { tri (1.0f, 25.0f, 11000.0f), tri (5.0f, 25.0f, 10000.0f), noStage, noStage },
          1, 40.0f, 0.0f, 1, toneEq, 0, eqVox, midCut, 0,
          3.0f, 2.0f, 0.0f, 3500.0f, 5.0f, 0.90f, 0.35f, 60.0f, 9500.0f, 95.0f, 2.5f,
          { 55, 50, 70, 50, 40, 50, 100, 55, 50, 75, 50 } },
        // AC30 "Fawn", Bright (Brilliant) channel: small coupling capacitors take the lows out before the inverter.
        { "a30_fawn_brt", "A30 Fawn Brt", "Vox AC30 \"Fawn\", Bright channel",
          2, { tri (1.0f, 25.0f, 14000.0f, 0.5f, 500.0f), tri (6.5f, 60.0f, 13000.0f), noStage, noStage },
          1, 40.0f, 0.0f, 1, toneEq, 0, eqVox, midCut, 0,
          3.0f, 2.0f, 0.0f, 3500.0f, 5.0f, 0.90f, 0.35f, 60.0f, 9500.0f, 95.0f, 2.5f,
          { 55, 55, 70, 50, 40, 50, 100, 55, 50, 75, 50 } },
        // Pete Anderson's custom blackface-style amp: the Deluxe circuit with more gain and a fuller middle.
        { "black_panel_pete", "Black Panel Pete", "Pete Anderson custom amp",
          3, { tri (1.0f, 12.0f, 15000.0f), tri (30.0f, 12.0f, 12000.0f), tri (1.8f, 12.0f, 10000.0f), noStage },
          1, 46.0f, 3000.0f, 1, toneStack, stackPete, eqGeneric, midNormal, 0,
          2.2f, 2.3f, 0.6f, 3000.0f, 6.0f, 0.92f, 0.28f, 50.0f, 10000.0f, 100.0f, 1.5f,
          { 45, 50, 55, 55, 40, 50, 100, 45, 50, 50, 45 } },
        // Line 6 Acoustic: a clean, wide-band amp voiced to make an electric guitar sound like a piezo acoustic:
        // lean low mids, lifted highs, no distortion to speak of.
        { "line6_acoustic", "Line 6 Acoustic", "Line 6 original (acoustic simulation)",
          2, { ss (0.7f, 60.0f, 20000.0f), ss (8.0f, 30.0f, 18000.0f, 0.6f, 900.0f), noStage, noStage },
          1, 40.0f, 0.0f, 1, toneEq, 0, eqAcoustic, midNormal, 0,
          2.0f, 3.0f, 1.5f, 5000.0f, 6.0f, 1.0f, 0.02f, 25.0f, 18000.0f, 95.0f, 0.0f,
          { 50, 50, 35, 60, 50, 50, 100, 10, 20, 50, 20 } },
        // ---- Bass ----
        // Ampeg SVT, Normal: two triode stages around a Baxandall bass / treble with an inductor midrange, six 6550s
        // with heavy feedback: enormous clean headroom and a little tube growl when pushed.
        { "svt_nrm", "SVT Nrm", "Ampeg SVT, Normal channel",
          3, { tri (0.8f, 8.0f, 12000.0f), tri (7.0f, 8.0f, 11000.0f), tri (2.0f, 8.0f, 10000.0f), noStage },
          1, 40.0f, 0.0f, 2, toneEq, 0, eqSvt, midNormal, 0,
          1.8f, 2.5f, 1.0f, 3000.0f, 5.0f, 0.95f, 0.12f, 22.0f, 9000.0f, 50.0f, 2.0f,
          { 50, 55, 50, 50, 35, 50, 100, 30, 50, 50, 40 } },
        // SVT, Bright: the bright cap on the volume and a leaner first stage.
        { "svt_brt", "SVT Brt", "Ampeg SVT, Bright channel",
          3, { tri (0.8f, 40.0f, 14000.0f, 0.8f, 200.0f), tri (7.0f, 8.0f, 13000.0f), tri (2.0f, 8.0f, 11000.0f), noStage },
          1, 40.0f, 1500.0f, 2, toneEq, 0, eqSvt, midNormal, 0,
          1.8f, 2.5f, 1.0f, 3000.0f, 5.0f, 0.95f, 0.12f, 22.0f, 9000.0f, 50.0f, 2.0f,
          { 50, 55, 50, 50, 35, 50, 100, 30, 50, 50, 40 } },
        // Gallien-Krueger 800RB: solid state with an active four-band EQ: fast, clean, a slightly forward top.
        { "g_cougar_800", "G Cougar 800", "Gallien-Krueger 800RB",
          2, { ss (0.8f, 20.0f, 18000.0f), ss (6.0f, 15.0f, 16000.0f, 0.75f, 1500.0f), noStage, noStage },
          1, 40.0f, 0.0f, 1, toneEq, 0, eqGk, midNormal, 0,
          2.2f, 3.0f, 1.5f, 4000.0f, 5.0f, 1.0f, 0.03f, 20.0f, 16000.0f, 50.0f, 0.5f,
          { 45, 55, 45, 55, 40, 50, 100, 10, 30, 50, 20 } },
    };

    /** Output level calibration per amp, in dB: { full amp, Pre model }, so that every amp is about as loud as
        the input at its default knobs (measured with the test riff; see amp_test.cpp). */
    inline constexpr float trims[numAmps][2] = {
        { -7.2f, 3.5f }, { -10.7f, 1.1f }, { -9.2f, 1.8f }, { -10.9f, 7.4f }, { -4.1f, 6.9f }, { -12.5f, 8.1f }, { -13.1f, 5.3f }, { -10.8f, 9.2f }, { -12.4f, 6.3f }, { -14.3f, 5.8f },
        { -7.8f, 6.1f }, { -6.9f, 7.5f }, { -12.5f, 5.4f }, { -13.7f, 5.5f }, { -13.4f, 2.7f }, { -17.5f, 7.5f }, { -18.8f, 1.5f }, { -17.6f, 6.6f }, { -19.3f, 1.1f }, { -25.6f, 4.8f },
        { -27.0f, 4.1f }, { -25.9f, 3.0f }, { -27.7f, 4.1f }, { -27.3f, 2.9f }, { -0.9f, 2.6f }, { -20.1f, 3.5f }, { -28.1f, 3.8f }, { -19.7f, 1.9f }, { -27.0f, 3.2f }, { 2.9f, 6.5f },
        { -28.1f, 3.6f }, { -20.7f, 4.5f }, { -26.8f, 5.2f }, { -27.6f, 3.8f }, { -26.0f, 3.9f }, { -13.4f, -5.3f }, { -28.2f, 3.5f }, { -27.2f, 3.4f }, { -19.5f, 3.3f }, { -27.5f, 3.5f },
        { 2.5f, 5.9f }, { -10.5f, 7.7f }, { -16.1f, 7.9f }, { -4.0f, 8.8f }, { -5.3f, 8.5f }, { -12.9f, 3.7f }, { 0.4f, 5.8f }, { -5.1f, 4.9f }, { -9.4f, 4.1f }, { 2.9f, 6.8f },
    };

    inline const char* const powerAmpNames[] = { "On", "Off (Pre)" };
} // namespace amp_detail

class AmpFx
{
public:
    enum Variant { firstModel = 0, numVariants = amp_detail::numAmps };

    void prepare (double sampleRate, int maxBlockSize)
    {
        fs = sampleRate;
        fsOs = 4.0 * sampleRate;
        os.prepare (maxBlockSize);
        slewPerSample = float (1.0 / (0.05 * fs));
        xAtk = 1.0 - std::exp (-1.0 / (0.003 * fsOs));  // bias excursion: the coupling caps charge fast...
        xRel = 1.0 - std::exp (-1.0 / (0.060 * fsOs));  // ...and recover slowly
        sAtk = 1.0 - std::exp (-1.0 / (0.012 * fsOs));  // supply sag
        sRel = 1.0 - std::exp (-1.0 / (0.160 * fsOs));
        humCw = std::cos (2.0 * pi * 60.0 / fs);
        humSw = std::sin (2.0 * pi * 60.0 / fs);
        dcG = 1.0 - std::exp (-2.0 * pi * 7.0 / fs);
        outGain.reset (fs, 0.05);
        mixB.reset (fs, 0.05);
        reset();
    }

    void reset()
    {
        configure();
        for (int i = 0; i < amp_detail::numKnobs; ++i)
            cur[i] = target[i];
        updateChunk (amp_detail::chunkSize, true);
        outGain.setCurrentAndTarget (outGain.getTarget());
        mixB.setCurrentAndTarget (mixB.getTarget());

        for (int s = 0; s < amp_detail::maxStages; ++s)
        {
            stShelf[s] = stHp[s] = stLp[s] = 0.0;
            stA[s] = sBias[s];
            stF[s] = idleF[s];
        }
        for (auto& z : tz) z = 0.0;
        for (auto& z : eqZ) z = 0.0;
        for (auto& z : postZ) z = 0.0;
        brS = cutS = presS = envX = envS = pX = dcS = 0.0;
        humC = 1.0;
        humS = 0.0;
        os.reset();
        dirty = false;
    }

    void setModel (int newVariant) noexcept
    {
        newVariant = std::clamp (newVariant, 0, (int) numVariants - 1);
        dirty = dirty || newVariant != variant;
        variant = newVariant;
    }

    /** Drive, Bass, Mid, Treble, Presence, Ch Vol, Master, Sag, Hum, Bias, Bias X [%], Power Amp (0 = On, 1 = Off). */
    void setParameters (const float* k) noexcept
    {
        for (int i = 0; i < amp_detail::numKnobs - 1; ++i)
            target[i] = std::clamp (k[i] * 0.01f, 0.0f, 1.0f);
        target[11] = k[11] < 0.5f ? 1.0f : 0.0f;
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        if (dirty)
            reset(); // a model was picked without the reset() that should follow it

        for (int i = 0; i < numSamples; ++i)
            left[i] = 0.5f * (left[i] + right[i]);

        float* x = os.up (left, numSamples);
        for (int pos = 0; pos < numSamples; pos += amp_detail::chunkSize)
        {
            const int len = std::min (amp_detail::chunkSize, numSamples - pos);
            updateChunk (len, false);
            processChunk (x + 4 * pos, len);
        }
        os.down (left, numSamples);

        // base rate: output transformer and speaker-impedance response (full amp only), a DC blocker at 7 Hz
        // (lopsided waves leave a little DC in the Pre model's line stage), then Ch Vol
        const double* c = postC;
        double dcState = dcS;
        const double dcCoeff = dcG;
        double p0 = postZ[0], p1 = postZ[1], p2 = postZ[2], p3 = postZ[3], p4 = postZ[4], p5 = postZ[5], p6 = postZ[6], p7 = postZ[7];
        for (int i = 0; i < numSamples; ++i)
        {
            double z = left[i];
            double y = c[0] * z + p0;       // transformer low end
            p0 = c[1] * z - c[3] * y + p1;
            p1 = c[2] * z - c[4] * y;
            double w = c[5] * y + p2;       // transformer top end
            p2 = c[6] * y - c[8] * w + p3;
            p3 = c[7] * y - c[9] * w;
            y = c[10] * w + p4;             // speaker resonance
            p4 = c[11] * w - c[13] * y + p5;
            p5 = c[12] * w - c[14] * y;
            w = c[15] * y + p6;             // the voice coil's rising impedance
            p6 = c[16] * y - c[18] * w + p7;
            p7 = c[17] * y - c[19] * w;
            z += mixB.next() * (w - z);
            dcState += (z - dcState) * dcCoeff;
            z = (z - dcState) * outGain.next();
            if (std::abs (z) > 2.0) // protection far above any normal level: whatever the knobs, the output stays below 4
            {
                const double over = std::abs (z) - 2.0, limited = 2.0 + over / (1.0 + 0.5 * over);
                z = z > 0.0 ? limited : -limited;
            }
            left[i] = (float) z;
            right[i] = (float) z;
        }
        using amp_detail::flush;
        postZ[0] = flush (p0); postZ[1] = flush (p1); postZ[2] = flush (p2); postZ[3] = flush (p3);
        postZ[4] = flush (p4); postZ[5] = flush (p5); postZ[6] = flush (p6); postZ[7] = flush (p7);
        dcS = flush (dcState);
    }

private:
    // values that ramp linearly across a chunk: the first numFast ones step every oversampled sample, the rest every base-rate sample
    enum Ramp { rA = 0, rAc, rB, rMaster, rPre, rPa, rPres, numFast,
                rComp = numFast, rQ, rSag, rBx, rBxG, rHumU, rHumPre, rRip, numRamps };
    static constexpr int numMix = 9;

    const amp_detail::Spec& spec() const noexcept { return amp_detail::specs[variant]; }

    /** Everything that depends only on the model and the sample rate. */
    void configure()
    {
        using namespace amp_detail;
        const Spec& sp = spec();
        numStages = sp.numStages;
        drivePos = sp.drivePos;
        tonePos = sp.tonePos;
        parallel = (sp.flags & flagParallel) != 0;
        toneFmv = sp.toneType == toneStack;
        hasCut = sp.midMode == midCut || sp.midMode == midDivide;

        double polarity = 1.0;
        for (int s = 0; s < maxStages; ++s)
        {
            const Stage& st = s < numStages ? sp.stage[s] : noStage;
            sGain[s] = st.gain;
            sBias[s] = st.bias;
            sInvP[s] = 1.0 / st.limPos;
            sInvN[s] = 1.0 / st.limNeg;
            sSqP[s] = (double) st.limPos * st.limPos;
            sSqN[s] = (double) st.limNeg * st.limNeg;
            const double w = sBias[s] * (sBias[s] >= 0.0 ? sInvP[s] : sInvN[s]), root = std::sqrt (1.0 + w * w);
            sOff[s] = sBias[s] / root; // exactly what the stage gives with no signal
            sNorm[s] = st.pol * root * root * root;
            idleF[s] = (sBias[s] >= 0.0 ? sSqP[s] : sSqN[s]) * (root - 1.0);
            sShelfG[s] = onePoleG (fsOs, st.shelfHz);
            sShelfK[s] = 1.0 - st.shelfK;
            sHpG[s] = onePoleG (fsOs, st.hpHz);
            sLpG[s] = onePoleG (fsOs, st.lpHz);
            if (s < numStages && ! (parallel && s == 1))
                polarity *= st.pol;
        }

        makeup = 1.0;
        if (toneFmv)
        {
            double b[4], a[4];
            fmvAnalog (stacks[sp.stack], 0.5, sp.midMode == midCut ? 1.0 : midTaper (0.5), bassTaper (0.5), b, a);
            makeup = 0.5 / fmvMagnitude (b, a, 1000.0); // the stack's loss is made up by the stage after it
        }

        piLimit = sp.piLimit;
        piInv = 1.0 / sp.piLimit;
        nfbK = sp.feedback;
        mm = sp.match;
        invN = 1.0 / (1.0 + sp.match);
        presLin = std::pow (10.0, sp.presenceDb / 20.0) - 1.0;
        trimLin = std::pow (10.0, trims[variant][0] / 20.0);
        preLin = polarity * std::pow (10.0, trims[variant][1] / 20.0);
        paDrive = polarity * sp.paDrive;

        eqHighPass (postC, fs, sp.lowHz, 0.7071);
        eqLowPass (postC + 5, fs, sp.highHz, 0.7071);
        eqPeak (postC + 10, fs, sp.resHz, 1.2, sp.resDb);
        eqShelf (postC + 15, fs, 4500.0, 0.5 * sp.resDb, true);
    }

    /** Moves the knobs towards their targets (50 ms for the full travel) and recomputes what depends on them. */
    void updateChunk (int len, bool force)
    {
        const float step = (float) len * slewPerSample;
        unsigned changed = force ? 0xfffu : 0u;
        for (int i = 0; i < amp_detail::numKnobs; ++i)
        {
            const float d = target[i] - cur[i];
            if (d != 0.0f)
            {
                cur[i] = std::abs (d) <= step ? target[i] : cur[i] + (d > 0.0f ? step : -step);
                changed |= 1u << i;
            }
        }

        if (changed != 0u)
            derive (changed);

        const double perBase = 1.0 / len, perOver = 0.25 * perBase;
        for (int r = 0; r < numRamps; ++r)
        {
            if (force)
                ramp[r] = rampTarget[r];
            rampStep[r] = (rampTarget[r] - ramp[r]) * (r < numFast ? perOver : perBase);
        }
        for (int i = 0; i < numMix; ++i)
        {
            if (force)
                mixC[i] = mixT[i];
            mixStep[i] = (mixT[i] - mixC[i]) * perOver;
        }
    }

    void derive (unsigned changed)
    {
        using namespace amp_detail;
        const Spec& sp = spec();
        const double bass = cur[1], mid = cur[2], treble = cur[3];

        if ((changed & 0x003u) != 0u) // Drive: the volume / gain pot, with its bright cap
        {
            const double alpha = std::pow (10.0, -sp.driveRangeDb * (1.0 - cur[0]) / 20.0);
            if (sp.brightHz > 0.0f)
            {
                // H = alpha + (1 - alpha) * highpass at brightHz / (alpha (1 - alpha)): full treble, the rest turned down
                rampTarget[rA] = 1.0;
                rampTarget[rAc] = 1.0 - alpha;
                brG = onePoleG (fsOs, sp.brightHz / (alpha * (1.0 - alpha) + 1.0e-4));
            }
            else
            {
                rampTarget[rA] = alpha;
                rampTarget[rAc] = 0.0;
            }
            rampTarget[rB] = sp.midMode == midDivide ? std::pow (10.0, -sp.driveRangeDb * (1.0 - bass) / 20.0) : 1.0;
        }

        if ((changed & 0x00eu) != 0u) // Bass, Mid, Treble
        {
            if (toneFmv)
            {
                fmvStateSpace (stacks[sp.stack], treble, sp.midMode == midCut ? 1.0 : midTaper (mid), bassTaper (bass), fsOs, makeup, tP, tQ, mixT, mixT[3]);
            }
            else
            {
                const Eq& e = eqs[sp.eq];
                double c[6];
                svfShelf (c, fsOs, e.bassHz, (bass - 0.5) * 2.0 * e.bassDb, false);
                eqA[0] = c[0]; eqA[1] = c[1]; eqA[2] = c[2]; mixT[0] = c[3]; mixT[1] = c[4]; mixT[2] = c[5];
                if (sp.midMode == midTone || sp.midMode == midDivide)
                    svfLowPass (c, fsOs, 700.0 * std::pow (20.0, mid), 0.7071);
                else
                    svfPeak (c, fsOs, e.midHz, e.midQ, (mid - 0.5) * 2.0 * e.midDb);
                eqA[3] = c[0]; eqA[4] = c[1]; eqA[5] = c[2]; mixT[3] = c[3]; mixT[4] = c[4]; mixT[5] = c[5];
                svfShelf (c, fsOs, e.trebleHz, (treble - 0.5) * 2.0 * e.trebleDb, true);
                eqA[6] = c[0]; eqA[7] = c[1]; eqA[8] = c[2]; mixT[6] = c[3]; mixT[7] = c[4]; mixT[8] = c[5];
            }

            if (hasCut)
                cutG = onePoleG (fsOs, 900.0 * std::pow (28.0, sp.midMode == midCut ? mid : treble));
        }

        if ((changed & 0x014u) != 0u) // Presence (its corner follows Mid on the Elektrik)
        {
            rampTarget[rPres] = cur[4] * presLin;
            presG = onePoleG (fsOs, sp.presenceHz * ((sp.flags & flagInteract) != 0 ? 1.6 - 1.2 * mid : 1.0));
        }

        if ((changed & 0x020u) != 0u) // Ch Vol: 50 % = the calibrated level, 100 % = +9.5 dB
            outGain.setTarget (float (trimLin * std::pow (2.0 * cur[5], 1.585)));

        if ((changed & 0x940u) != 0u) // Master, Power Amp, Hum
        {
            const double cutDb = 36.0 * std::pow (1.0 - cur[6], 1.2), giveBack = std::pow (10.0, 0.55 * cutDb / 20.0), on = cur[11];
            rampTarget[rMaster] = paDrive * std::pow (10.0, -cutDb / 20.0);
            rampTarget[rPa] = on * giveBack; // part of the level lost by turning Master down is given back
            rampTarget[rPre] = 1.0 - on;
            mixB.setTarget (cur[11]);

            const double hum = 0.0012 * cur[8] * cur[8]; // at the output, before Ch Vol
            rampTarget[rHumU] = hum / (giveBack * trimLin);
            rampTarget[rHumPre] = hum / trimLin;
            rampTarget[rRip] = 0.04 * cur[8] * cur[8];
        }

        if ((changed & 0x080u) != 0u)
            rampTarget[rSag] = std::min (0.8, 2.0 * cur[7] * sp.sagDepth);

        if ((changed & 0x200u) != 0u) // Bias: the output tubes' idle point, from cold (crossover notch) to class A
        {
            double q = -1.15 + 1.15 * cur[9];
            if (q < -0.575)
                q = -0.575 + (q + 0.575) / (1.0 + 0.5 * sp.feedback); // feedback straightens part of the crossover notch
            const double qc = std::max (q, -0.6);
            rampTarget[rQ] = q;
            rampTarget[rComp] = std::pow (1.0 + qc * qc, 1.5); // unit gain for small signals, down to the ideal class AB point
        }

        if ((changed & 0x400u) != 0u) // Bias X: how far hard drive pushes the idle point colder, and how much gain that costs
        {
            rampTarget[rBx] = 0.8 * cur[10] / (1.0 + sp.feedback);
            rampTarget[rBxG] = 0.7 * cur[10];
        }
    }

    /** `len` base-rate samples = 4 * len oversampled ones, in place: preamp stages, tone stack, power amp. */
    void processChunk (float* x, int len) noexcept
    {
        using namespace amp_detail;
        const int nSt = numStages, dPos = drivePos, tPos = tonePos;
        const bool par = parallel, fmv = toneFmv, cut = hasCut;
        const bool paActive = ramp[rPa] != 0.0 || rampTarget[rPa] != 0.0;
        const bool preActive = ramp[rPre] != 0.0 || rampTarget[rPre] != 0.0;

        double gA = ramp[rA], gAc = ramp[rAc], gB = ramp[rB], gMaster = ramp[rMaster], gPre = ramp[rPre], gPa = ramp[rPa], pGain = ramp[rPres];
        const double dA = rampStep[rA], dAc = rampStep[rAc], dB = rampStep[rB], dMaster = rampStep[rMaster], dPre = rampStep[rPre],
                     dPa = rampStep[rPa], dPres = rampStep[rPres];
        double cmp = ramp[rComp], qIdle = ramp[rQ], sag = ramp[rSag], bxK = ramp[rBx], bxG = ramp[rBxG], hU = ramp[rHumU], hPre = ramp[rHumPre], hRip = ramp[rRip];
        const double dCmp = rampStep[rComp], dQ = rampStep[rQ], dSag = rampStep[rSag], dBxK = rampStep[rBx], dBxG = rampStep[rBxG],
                     dHU = rampStep[rHumU], dHPre = rampStep[rHumPre], dHRip = rampStep[rRip];

        double shelfS[maxStages], hpS[maxStages], lpS[maxStages], tubeA[maxStages], tubeF[maxStages];
        for (int s = 0; s < maxStages; ++s) { shelfS[s] = stShelf[s]; hpS[s] = stHp[s]; lpS[s] = stLp[s]; tubeA[s] = stA[s]; tubeF[s] = stF[s]; }

        // tone stack (state-space) or the three-band EQ (state-variable filters), and their output mixes
        const double P0 = tP[0], P1 = tP[1], P2 = tP[2], P3 = tP[3], P4 = tP[4], P5 = tP[5], P6 = tP[6], P7 = tP[7], P8 = tP[8];
        const double Q0 = tQ[0], Q1 = tQ[1], Q2 = tQ[2];
        double z0 = tz[0], z1 = tz[1], z2 = tz[2], zu = tz[3];
        const double A0 = eqA[0], A1 = eqA[1], A2 = eqA[2], A3 = eqA[3], A4 = eqA[4], A5 = eqA[5], A6 = eqA[6], A7 = eqA[7], A8 = eqA[8];
        double e0 = eqZ[0], e1 = eqZ[1], e2 = eqZ[2], e3 = eqZ[3], e4 = eqZ[4], e5 = eqZ[5];
        double k0 = mixC[0], k1 = mixC[1], k2 = mixC[2], k3 = mixC[3], k4 = mixC[4], k5 = mixC[5], k6 = mixC[6], k7 = mixC[7], k8 = mixC[8];
        const double dk0 = mixStep[0], dk1 = mixStep[1], dk2 = mixStep[2], dk3 = mixStep[3], dk4 = mixStep[4], dk5 = mixStep[5],
                     dk6 = mixStep[6], dk7 = mixStep[7], dk8 = mixStep[8];

        double bs = brS, cs = cutS, ps = presS, ex = envX, es = envS, px = pX;
        const double bG = brG, cG = cutG, pG = presG, pLin = preLin, pInv = piInv, pLimit = piLimit;
        const double fb = nfbK, match = mm, norm = invN;
        double hc = humC, hs = humS;
        const double hcw = humCw, hsw = humSw;

        for (int i = 0; i < len; ++i)
        {
            // heater hum (60 Hz and its 3rd harmonic) and supply ripple (120 Hz): one value per base-rate sample
            const double nc = hc * hcw - hs * hsw, ns = hs * hcw + hc * hsw;
            hc = nc;
            hs = ns;
            const double heater = ns + 0.35 * ns * (3.0 - 4.0 * ns * ns);
            const double humIn = hU * heater, humOut = hPre * heater;

            // the output tubes' idle point: driven hard, the grids charge the coupling caps and the bias goes colder,
            // which opens a crossover notch and takes gain away (Bias X)
            const double q = qIdle - bxK * ex;
            const double rq = q / std::sqrt (1.0 + q * q), dcq = (rq - match * rq) * norm; // what the tubes give with no signal
            const double normC = norm / cmp;
            const double gx = 1.0 / (1.0 + bxG * ex), supply = (1.0 - hRip * (2.0 * ns * nc)) * gx, sagG = sag * gx;
            // the tubes' antiderivative at the last sample, for this idle point
            const double pc = cmp * px, pa1 = q + pc, pa2 = q - pc;
            double pF = (std::sqrt (1.0 + pa1 * pa1) + match * std::sqrt (1.0 + pa2 * pa2)) * normC - dcq * px;

            for (int j = 0; j < 4; ++j)
            {
                double v = *x;
                const double in0 = v;
                double acc = 0.0;

                for (int s = 0; ; ++s)
                {
                    if (s == tPos)
                    {
                        if (fmv) // tone stack: x = the three capacitor voltages
                        {
                            const double us = zu + v;
                            const double n0 = P0 * z0 + P1 * z1 + P2 * z2 + Q0 * us;
                            const double n1 = P3 * z0 + P4 * z1 + P5 * z2 + Q1 * us;
                            const double n2 = P6 * z0 + P7 * z1 + P8 * z2 + Q2 * us;
                            z0 = n0; z1 = n1; z2 = n2; zu = v;
                            v = k0 * z0 + k1 * z1 + k2 * z2 + k3 * v;
                            k0 += dk0; k1 += dk1; k2 += dk2; k3 += dk3;
                        }
                        else // bass shelf, mid peak (or Tone low-pass), treble shelf
                        {
                            double v3 = v - e1, v1 = A0 * e0 + A1 * v3, v2 = e1 + A1 * e0 + A2 * v3;
                            e0 = 2.0 * v1 - e0;
                            e1 = 2.0 * v2 - e1;
                            const double y = k0 * v + k1 * v1 + k2 * v2;
                            v3 = y - e3; v1 = A3 * e2 + A4 * v3; v2 = e3 + A4 * e2 + A5 * v3;
                            e2 = 2.0 * v1 - e2;
                            e3 = 2.0 * v2 - e3;
                            const double w = k3 * y + k4 * v1 + k5 * v2;
                            v3 = w - e5; v1 = A6 * e4 + A7 * v3; v2 = e5 + A7 * e4 + A8 * v3;
                            e4 = 2.0 * v1 - e4;
                            e5 = 2.0 * v2 - e5;
                            v = k6 * w + k7 * v1 + k8 * v2;
                            k0 += dk0; k1 += dk1; k2 += dk2; k3 += dk3; k4 += dk4; k5 += dk5; k6 += dk6; k7 += dk7; k8 += dk8;
                        }
                    }

                    if (s == nSt)
                        break;

                    if (s == dPos) // the volume / gain pot and its bright cap
                    {
                        const double t = (v - bs) * bG, lp = t + bs;
                        bs = lp + t;
                        v = v * gA - lp * gAc;
                    }
                    else if (par && s == 1) // the second channel takes the input too
                    {
                        acc = v;
                        v = in0 * gB;
                    }

                    // cathode / treble-peaking shelf
                    double t, lp;
                    if (sShelfK[s] != 0.0)
                    {
                        t = (v - shelfS[s]) * sShelfG[s];
                        lp = t + shelfS[s];
                        shelfS[s] = lp + t;
                        v -= sShelfK[s] * lp;
                    }

                    // the tube, anti-aliased: the mean of its curve between the last sample and this one, which is
                    // the difference quotient of the curve's antiderivative F = lim^2 (sqrt (1 + (a / lim)^2) - 1)
                    const double a = v * sGain[s] + sBias[s];
                    const double w = a * (a >= 0.0 ? sInvP[s] : sInvN[s]);
                    const double F = (a >= 0.0 ? sSqP[s] : sSqN[s]) * (std::sqrt (1.0 + w * w) - 1.0);
                    const double da = a - tubeA[s];
                    double y;
                    if (std::abs (da) > 1.0e-6)
                    {
                        y = (F - tubeF[s]) / da;
                    }
                    else
                    {
                        const double am = 0.5 * (a + tubeA[s]), wm = am * (am >= 0.0 ? sInvP[s] : sInvN[s]);
                        y = am / std::sqrt (1.0 + wm * wm);
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

                    if (par && s == 1)
                        v += acc;
                }

                if (cut) // Vox-style Cut
                {
                    const double t = (v - cs) * cG, lp = t + cs;
                    cs = lp + t;
                    v = lp;
                }

                {   // Presence: less feedback at high frequencies = a treble shelf in front of the output tubes
                    const double t = (v - ps) * pG, lp = t + ps;
                    ps = lp + t;
                    v += pGain * (v - lp);
                }

                double o = 0.0;
                if (preActive) // the "Pre" model: the preamp's output through a line stage with plenty of headroom
                {
                    const double w = v * 0.3333333333333333;
                    o = (v / std::sqrt (1.0 + w * w) * pLin + humOut) * gPre;
                }

                if (paActive)
                {
                    // Master, then the phase inverter (soft, symmetrical, reaches its limit at twice that drive)
                    double u = v * gMaster * pInv;
                    u = u > 2.0 ? 2.0 : (u < -2.0 ? -2.0 : u);
                    u = (u - 0.25 * u * std::abs (u)) * pLimit + humIn;

                    double e = std::abs (u) - 1.0;
                    e = e < 0.0 ? 0.0 : (e > 1.5 ? 1.5 : e);
                    ex += (e - ex) * (e > ex ? xAtk : xRel);

                    // negative feedback: about what the output tubes fail to deliver (nothing up to half their swing, all
                    // of it beyond clipping) is added to their drive: a straighter curve with a sharper knee
                    double d = std::abs (u) - 0.5;
                    d = d <= 0.0 ? 0.0 : (d < 0.7 ? d * d * 0.7142857142857143 : d - 0.35);
                    const double xp = u + (u >= 0.0 ? fb * d : -fb * d);

                    // output tubes: each one a sigmoid a / sqrt (1 + a^2) around its idle point q, the pair (or the single
                    // tube) combined; anti-aliased like the preamp tubes
                    const double cu = cmp * xp, a1 = q + cu, a2 = q - cu;
                    const double F = (std::sqrt (1.0 + a1 * a1) + match * std::sqrt (1.0 + a2 * a2)) * normC - dcq * xp;
                    const double dx = xp - px;
                    double y;
                    if (std::abs (dx) > 1.0e-6)
                    {
                        y = (F - pF) / dx;
                    }
                    else
                    {
                        const double cm = cmp * 0.5 * (xp + px), b1 = q + cm, b2 = q - cm;
                        y = (b1 / std::sqrt (1.0 + b1 * b1) - match * (b2 / std::sqrt (1.0 + b2 * b2))) * norm - dcq;
                    }
                    px = xp;
                    pF = F;

                    // the supply sags with the current drawn, and carries the rectifier's ripple
                    const double ay = std::min (std::abs (y), 1.0);
                    es += (ay - es) * (ay > es ? sAtk : sRel);
                    o += y * (supply - sagG * es) * gPa;
                }

                *x++ = (float) o;
                gA += dA; gAc += dAc; gB += dB; gMaster += dMaster; gPre += dPre; gPa += dPa; pGain += dPres;
            }

            cmp += dCmp; qIdle += dQ; sag += dSag; bxK += dBxK; bxG += dBxG; hU += dHU; hPre += dHPre; hRip += dHRip;
        }

        for (int s = 0; s < maxStages; ++s)
        {
            stShelf[s] = flush (shelfS[s]); stHp[s] = flush (hpS[s]); stLp[s] = flush (lpS[s]);
            stA[s] = tubeA[s]; stF[s] = tubeF[s];
        }
        tz[0] = flush (z0); tz[1] = flush (z1); tz[2] = flush (z2); tz[3] = zu;
        eqZ[0] = flush (e0); eqZ[1] = flush (e1); eqZ[2] = flush (e2); eqZ[3] = flush (e3); eqZ[4] = flush (e4); eqZ[5] = flush (e5);
        brS = flush (bs); cutS = flush (cs); presS = flush (ps); envX = flush (ex); envS = flush (es); pX = px;
        humC = hc;
        humS = hs;
        for (int r = 0; r < numRamps; ++r)
            ramp[r] = rampTarget[r];
        for (int i = 0; i < numMix; ++i)
            mixC[i] = mixT[i];
    }

    double fs = 48000.0, fsOs = 192000.0;
    int variant = 0;
    bool dirty = true;
    Oversampler4x os;

    float target[amp_detail::numKnobs] { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f, 0.5f, 0.0f, 0.5f, 0.5f, 1.0f };
    float cur[amp_detail::numKnobs] {};
    float slewPerSample = 0.0f;
    double ramp[numRamps] {}, rampTarget[numRamps] {}, rampStep[numRamps] {};
    double mixC[numMix] {}, mixT[numMix] {}, mixStep[numMix] {}; // the tone section's output mix (ramped like the gains)

    // the model
    int numStages = 2, drivePos = 1, tonePos = 1;
    bool parallel = false, toneFmv = true, hasCut = false;
    double sGain[amp_detail::maxStages] {}, sBias[amp_detail::maxStages] {}, sInvP[amp_detail::maxStages] {}, sInvN[amp_detail::maxStages] {},
           sOff[amp_detail::maxStages] {}, sNorm[amp_detail::maxStages] {}, sShelfG[amp_detail::maxStages] {}, sShelfK[amp_detail::maxStages] {},
           sHpG[amp_detail::maxStages] {}, sLpG[amp_detail::maxStages] {}, sSqP[amp_detail::maxStages] {}, sSqN[amp_detail::maxStages] {},
           idleF[amp_detail::maxStages] {};
    double makeup = 1.0, presLin = 1.0, trimLin = 1.0;
    double preLin = 1.0, paDrive = 1.0, piInv = 0.4, piLimit = 2.5, nfbK = 0.0, mm = 0.9, invN = 0.5;
    double postC[20] {}; // output transformer and speaker-impedance filters (base rate)

    // knob-dependent
    double tP[9] {}, tQ[3] {}, eqA[9] {};
    double brG = 0.5, cutG = 0.5, presG = 0.5;
    double xAtk = 0.0, xRel = 0.0, sAtk = 0.0, sRel = 0.0;

    // state. The oversampled path is in double: one-poles at 8 Hz and the tone stack running at up to 768 kHz have
    // their poles too close to z = 1 for float state (the rounding shows up as noise around -100 dB).
    double stShelf[amp_detail::maxStages] {}, stHp[amp_detail::maxStages] {}, stLp[amp_detail::maxStages] {};
    double stA[amp_detail::maxStages] {}, stF[amp_detail::maxStages] {}, pX = 0.0; // last tube inputs and antiderivatives
    double tz[4] {}, eqZ[6] {}, postZ[8] {};
    double brS = 0.0, cutS = 0.0, presS = 0.0, envX = 0.0, envS = 0.0, dcS = 0.0, dcG = 0.0;
    double humC = 1.0, humS = 0.0, humCw = 1.0, humSw = 0.0;
    Smoothed outGain { 1.0f }, mixB { 1.0f };
};

/** In the order of AmpFx's variants (= amp_detail::specs): the 30 stock amps, then the model packs. */
inline std::vector<ModelInfo> ampModels()
{
    std::vector<ModelInfo> models;
    for (int i = 0; i < amp_detail::numAmps; ++i)
    {
        const auto& sp = amp_detail::specs[i];
        const float* d = sp.def;
        models.push_back ({ sp.key, sp.name, Category::amp, Engine::ampFx, i, sp.basedOn,
                            { percent ("Drive", d[0]), percent ("Bass", d[1]), percent ("Mid", d[2]), percent ("Treble", d[3]),
                              percent ("Presence", d[4]), percent ("Ch Vol", d[5]), percent ("Master", d[6]), percent ("Sag", d[7]),
                              percent ("Hum", d[8]), percent ("Bias", d[9]), percent ("Bias X", d[10]),
                              choice ("Power Amp", amp_detail::powerAmpNames, 2, 0) } });
    }
    return models;
}

} // namespace fx
