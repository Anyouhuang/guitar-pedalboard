#pragma once

#include "ModelTypes.h"
#include "fx/Volume.h"
#include "fx/Dynamics.h"
#include "fx/Mod.h"
#include "fx/Filter.h"
#include "fx/Pitch.h"
#include "fx/Eq.h"
#include "fx/DelayFx.h"
#include "fx/Verb.h"
#include "fx/Wah.h"

#include "fx/Amp.h"
#include "fx/Cab.h"

namespace fx
{
/** Every model, in a fixed order: the order is saved in presets, so only ever append. */
inline const std::vector<ModelInfo>& models()
{
    static const std::vector<ModelInfo> list = []
    {
        std::vector<ModelInfo> m;
        m.push_back ({ "empty", "Empty", Category::none, Engine::none, 0, "", {} });

        // ---- dynamics
        m.push_back ({ "noise_gate", "Noise Gate", Category::dynamics, Engine::gate, 0, "Noise suppressor with hysteresis",
                       { decibels ("Threshold", -90.0f, -20.0f, -65.0f, 0.5f), millis ("Decay", 5.0f, 500.0f, 60.0f, 80.0f) } });

        // ---- distortion: the POD HD500X's 15 models. Like on the HD, every one has Drive, Bass, Mid, Treble and
        // Output (Bass / Mid / Treble: 50 % = flat), except where the original's own control takes the Mid knob's place.
        auto drive = [&] (const char* key, const char* name, int variant, const char* basedOn, const char* third, float driveDefault)
        {
            m.push_back ({ key, name, Category::distortion, Engine::distortion, variant, basedOn,
                           { percent ("Drive", driveDefault), percent ("Bass", 50.0f), percent (third, 50.0f),
                             percent ("Treble", 50.0f), decibels ("Output", -30.0f, 12.0f, 0.0f) } });
        };
        drive ("tube_drive",   "Tube Drive",   0, "Chandler Tube Driver", "Mid", 50.0f);
        drive ("screamer",     "Screamer",     1, "Ibanez TS808 Tube Screamer", "Tone", 50.0f);
        drive ("overdrive",    "Overdrive",    2, "DOD Overdrive/Preamp 250", "Mid", 50.0f);
        drive ("classic_dist", "Classic Dist", 3, "Pro Co RAT", "Filter", 50.0f);
        drive ("heavy_dist",   "Heavy Dist",   4, "BOSS MT-2 Metal Zone", "Mid", 60.0f);
        drive ("color_drive",  "Color Drive",  5, "Colorsound Overdriver", "Mid", 50.0f);
        drive ("buzz_saw",     "Buzz Saw",     6, "Maestro Fuzz-Tone FZ-1", "Mid", 60.0f);
        drive ("facial_fuzz",  "Facial Fuzz",  7, "Arbiter Fuzz Face", "Mid", 60.0f);
        drive ("jumbo_fuzz",   "Jumbo Fuzz",   8, "Vox Tone Bender", "Mid", 60.0f);
        drive ("fuzz_pi",      "Fuzz Pi",      9, "Electro-Harmonix Big Muff Pi", "Mid", 60.0f);
        m.push_back ({ "jet_fuzz", "Jet Fuzz", Category::distortion, Engine::distortion, 10, "Roland AP-7 Jet Phaser",
                       { percent ("Drive", 60.0f), percent ("Fdbk", 50.0f), percent ("Tone", 50.0f),
                         hertz ("Speed", 0.05f, 8.0f, 0.4f, 1.0f), decibels ("Output", -30.0f, 12.0f, 0.0f) } });
        drive ("line6_drive",  "Line 6 Drive", 11, "Line 6 original: Mid morphs '70s fuzz > modern high gain > Tone Bender grit", "Mid", 50.0f);
        drive ("line6_dist",   "Line 6 Distortion", 12, "Line 6 original: massive, over-the-top gain", "Mid", 60.0f);
        drive ("sub_oct_fuzz", "Sub Octave Fuzz", 13, "PAiA Roctave Divider", "Sub", 60.0f);
        drive ("octave_fuzz",  "Octave Fuzz",  14, "Tycobrahe Octavia", "Mid", 60.0f);

        // ---- modulation
        auto mod = [&] (const char* key, const char* name, int variant, const char* basedOn, const char* third)
        {
            m.push_back ({ key, name, Category::modulation, Engine::modulation, variant, basedOn,
                           { hertz ("Speed", 0.05f, 10.0f, 0.8f, 1.0f), percent ("Depth", 50.0f), percent (third, 50.0f) },
                           -1, -1, nullptr, true });
        };
        mod ("chorus",  "Chorus",  0, "Stereo chorus", "Mix");
        mod ("flanger", "Flanger", 1, "Stereo flanger", "Mix");
        mod ("phaser",  "Classic Phaser", 2, "6-stage phaser", "Mix"); // "Phaser" is the HD500X one (phaser_hd)
        mod ("tremolo", "Tremolo", 3, "Tremolo, sine to square", "Shape");

        // ---- delay
        m.push_back ({ "analog_delay", "Analog Delay", Category::delay, Engine::delay, 0, "Stereo delay, darker repeats",
                       { millis ("Time", 20.0f, 2000.0f, 500.0f, 400.0f), choice ("Note", delayNoteNames, 6, 1),
                         percent ("Feedback", 35.0f, 95.0f), percent ("Mix", 35.0f), knob10 ("Tone", 6.0f) },
                       0, 1, delayNoteBeats, true, true });

        // ---- reverb
        m.push_back ({ "room_reverb", "Room Reverb", Category::reverb, Engine::reverb, 0, "Freeverb room / hall",
                       { percent ("Size", 55.0f), percent ("Damp", 45.0f), percent ("Mix", 25.0f),
                         millis ("Pre-Delay", 0.0f, 500.0f, 0.0f, 120.0f), choice ("Note", reverbNoteNames, 5, 0) },
                       3, 4, reverbNoteBeats, true, true });

        // ---- the fx/ engines (docs/engine-contract.md), each with its own model list
        auto append = [&] (std::vector<ModelInfo> more) { m.insert (m.end(), more.begin(), more.end()); };
        append (dynamicsModels());
        append (modModels());
        append (filterModels());
        append (pitchModels());
        append (eqModels());
        append (delayFxModels());
        append (verbModels());
        append (wahModels());
        append (volumeModels());
        return m;
    }();
    return list;
}

inline int numModels() { return (int) models().size(); }

//==============================================================================
/** The amp block's own lists: amp models and speaker cabinets (with their microphones). */
inline const std::vector<ModelInfo>& amps() { static const auto list = ampModels(); return list; }
inline const std::vector<ModelInfo>& cabs() { static const auto list = cabModels(); return list; }

constexpr int maxAmpKnobs = 12;
constexpr int maxCabKnobs = 8;

/** Index in `list` of the model with this key, or -1 ("none"). */
inline int findIn (const std::vector<ModelInfo>& list, std::string_view key)
{
    for (size_t i = 0; i < list.size(); ++i)
        if (key == list[i].key)
            return (int) i;
    return -1;
}

/** The amp block, in plain units. amp / cab: index in amps() / cabs(), or -1 for none. */
struct AmpParams
{
    bool on = true;
    int amp = -1;
    std::array<float, maxAmpKnobs> ampKnobs {};
    int cab = -1;
    std::array<float, maxCabKnobs> cabKnobs {};
};

/** An amp block with this amp and cab ("" = none), every knob at its default. */
inline AmpParams makeAmp (std::string_view ampKey, std::string_view cabKey, bool on = true)
{
    AmpParams p;
    p.on = on;
    p.amp = findIn (amps(), ampKey);
    p.cab = findIn (cabs(), cabKey);

    if (p.amp >= 0)
        for (size_t k = 0; k < amps()[(size_t) p.amp].knobs.size(); ++k)
            p.ampKnobs[k] = amps()[(size_t) p.amp].knobs[k].def;

    if (p.cab >= 0)
        for (size_t k = 0; k < cabs()[(size_t) p.cab].knobs.size(); ++k)
            p.cabKnobs[k] = cabs()[(size_t) p.cab].knobs[k].def;

    return p;
}

/** The cab this project has always used: the default. */
inline constexpr const char* defaultCabKey = "cab_412_classic";

/** The cabinet an amp is usually played through (the HD500X selects it together with the amp), or "" to keep the current one. */
inline std::string_view defaultCabFor (std::string_view ampKey)
{
    struct Pair { const char* amp; const char* cab; };
    static constexpr Pair pairs[] =
    {
        { "blackface_double_normal", "cab_212_blackface" },
        { "blackface_double_vibrato", "cab_212_blackface" },
        { "hiway_100", "cab_412_hiway" },
        { "super_o", "cab_6x9_super_o" },
        { "gibtone_185", "cab_112_field_coil" },
        { "tweed_b_man_normal", "cab_410_tweed" },
        { "tweed_b_man_bright", "cab_410_tweed" },
        { "blackface_lux_normal", "cab_112_bf_lux" },
        { "blackface_lux_vibrato", "cab_112_bf_lux" },
        { "divide_9_15", "cab_112_celest_12h" },
        { "phd_motorway", "cab_212_phd_ported" },
        { "class_a_15", "cab_112_blue_bell" },
        { "class_a_30_tb", "cab_212_silver_bell" },
        { "brit_j_45_normal", "cab_412_greenback" },
        { "brit_j_45_bright", "cab_412_greenback" },
        { "plexi_lead_100_normal", "cab_412_blackback" },
        { "plexi_lead_100_bright", "cab_412_blackback" },
        { "brit_p_75_normal", "cab_412_greenback" },
        { "brit_p_75_bright", "cab_412_greenback" },
        { "brit_j_800", "cab_412_brit_t75" },
        { "bomber_uber", "cab_412_uber" },
        { "treadplate", "cab_412_tread_v30" },
        { "angel_f_ball", "cab_412_xxl_v30" },
        { "line6_elektrik", "cab_412_xxl_v30" },
        { "solo_100_clean", "cab_412_tread_v30" },
        { "solo_100_crunch", "cab_412_tread_v30" },
        { "solo_100_od", "cab_412_tread_v30" },
        { "line6_doom", "cab_412_uber" },
        { "line6_epic", "cab_412_xxl_v30" },
        { "flip_top", "cab_115_flip_top" },
        { "pv_panama", "cab_412_tread_v30" },
        { "mahadeva", "cab_412_tread_v30" },
        { "brit_2204", "cab_412_brit_t75" },
        { "line6_insane", "cab_412_uber" },
        { "line6_big_bottom", "cab_412_uber" },
        { "line6_variaced_plexi", "cab_412_greenback" },
        { "line6_purge", "cab_412_xxl_v30" },
        { "line6_aggro", "cab_412_tread_v30" },
        { "line6_smash", "cab_412_brit_t75" },
        { "line6_octone", "cab_412_greenback" },
        { "jazz_rivet", "cab_212_jazz_rivet" },
        { "small_tweed", "cab_108_small_tweed" },
        { "mandarin_80", "cab_412_greenback" },
        { "a30_fawn_nrm", "cab_212_silver_bell" },
        { "a30_fawn_brt", "cab_212_silver_bell" },
        { "black_panel_pete", "cab_212_blackface" },
        { "line6_acoustic", "cab_212_jazz_rivet" },
        { "svt_nrm", "cab_810_sv_beast" },
        { "svt_brt", "cab_810_sv_beast" },
        { "g_cougar_800", "cab_410_rhino" },
    };
    for (const auto& p : pairs)
        if (ampKey == p.amp)
            return p.cab;
    return "";
}

inline int findModel (std::string_view key)
{
    const auto& list = models();
    for (size_t i = 0; i < list.size(); ++i)
        if (key == list[i].key)
            return (int) i;
    return 0;
}

inline const ModelInfo& modelInfo (int index)
{
    return models()[(size_t) std::clamp (index, 0, numModels() - 1)];
}

//==============================================================================
/** A slot holding `key` with every knob at its default. */
inline SlotParams makeSlot (std::string_view key, bool on)
{
    SlotParams s;
    s.on = on;
    s.model = findModel (key);
    const auto& knobs = modelInfo (s.model).knobs;
    for (size_t k = 0; k < knobs.size(); ++k)
        s.knobs[k] = knobs[k].def;
    return s;
}

/** Tempo sync: when the note knob is not "ms", the time knob follows the tempo. */
inline void resolveTempo (SlotParams& s, double bpm)
{
    const auto& m = modelInfo (s.model);
    if (m.timeKnob < 0 || m.noteKnob < 0 || bpm <= 0.0)
        return;

    auto sync = [&] (int timeKnob, int noteKnob)
    {
        if (timeKnob < 0 || noteKnob < 0)
            return;

        const int note = (int) std::lround (s.knobs[(size_t) noteKnob]);
        if (note > 0)
        {
            const auto& spec = m.knobs[(size_t) timeKnob];
            s.knobs[(size_t) timeKnob] = std::clamp ((float) (60000.0 / bpm * m.noteBeats[note]), spec.min, spec.max);
        }
    };
    sync (m.timeKnob, m.noteKnob);
    sync (m.timeKnob2, m.noteKnob2);
}

} // namespace fx
