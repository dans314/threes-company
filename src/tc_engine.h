/*
 * Three's Company - audio engine (phase 1)
 * Otis Kanno, 2026.
 *
 * Three filters, three drives, two ways to wire them:
 *   Parallel: a 3-band Linkwitz-Riley crossover; each band has its own filter and drive.
 *   Series:   Low strip -> Mid strip -> High strip on the full signal (the Mid strip sits
 *             in the middle of the chain).
 * Each routing keeps its own complete set of settings (Settings::mode[0] and mode[1]).
 *
 * The filter core is a port of
 * "Two Filters" by Paul Walker (BaconPaul), https://github.com/baconpaul/two-filters,
 * Copyright 2024-2026 Paul Walker and various authors. Taken from there, lightly reshaped:
 * the filter catalogue, the way sst-filters is set up and fed coefficients, the output
 * limiter, the resonance guard, the warm saturator and the non-finite recovery.
 *
 * The filters are the Surge Synth Team's sst-filters (GPL3), so this engine and anything
 * built from it is released under the GNU GPL v3. See LICENSE.
 *
 * Phase 1: crossover, three filters, per-band drive, guards and limiter, both routings,
 * smart band-edge limiting.
 * Phase 2: pan per strip, feedback and noise per routing, and three things shared
 * by both routings: Sway (slow LFOs that never line up), the Conductor (Mid passes its movement
 * on to Low and High) and Mutation (when something happens in Mid, Low and High answer, and the
 * answers drift over the bars). Host modulation still arrives through Settings::mod.
 * Phase 3: three sequencer lanes (bipolar steps, Euclid, swing, offset, Clock or Free) each with a
 * lock lane (P-locks), three LFOs (made shapes or a drawn curve), an envelope follower that can
 * listen to a sidechain, and a 16-slot mod matrix over all of them.
 */

#ifndef THREES_COMPANY_ENGINE_H
#define THREES_COMPANY_ENGINE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sst/filters++.h"

namespace otiskanno::threescompany
{
namespace sfpp = sst::filtersplusplus;

static constexpr int blockSize{8}; // control rate: once every 8 host samples
static constexpr int numBands{3};
static constexpr int maxOsStages{2}; // 0 = off, 1 = 2x, 2 = 4x
enum BandIndex { Low = 0, Mid = 1, High = 2 };
enum Routing { Parallel = 0, Series = 1 };
enum DriveType { Warm = 0, Push, Bite, Fuzz, Fold, Rectify, Crush, numDriveTypes };

inline const char *driveName(int t)
{
    static const char *n[numDriveTypes] = {"Warm", "Push", "Bite", "Fuzz", "Fold", "Rectify", "Crush"};
    return n[std::clamp(t, 0, (int)numDriveTypes - 1)];
}

// ---------------------------------------------------------------------------
// Filter catalogue. Model 0 is "None".
// ---------------------------------------------------------------------------
enum struct PassKind { Open, LowPass, HighPass, BandPass };

struct ModelInfo
{
    sfpp::FilterModel model{sfpp::FilterModel::None};
    std::string name;
    std::vector<sfpp::ModelConfig> configs;
    std::vector<std::string> labels;
    std::vector<PassKind> kinds; // what the smart edge limiting treats each variant as
    bool zplane{false};          // our own morphing filter (not from sst-filters)
};

// ---------------------------------------------------------------------------
// Z-Plane, built the way the Morpheus manual describes its filter: one resonant low-pass section
// and six parametric EQ sections in series (14 poles), with whole filters stored as frames.
//   Morph       the realtime axis: moves from frame A to frame B. As in a Z-plane filter, it is
//               the poles and zeros that move (radius and angle in the z-plane), so peaks glide
//               from one frequency to the next instead of fading out and in.
//   Cutoff      Frequency Tracking: slides the whole shape up and down (designed at 1 kHz).
//   Resonance   Transform 2: the size and sharpness of the peaks and notches (the manual's cube
//               example), from flattened to sharper than designed.
// Unlike the Morpheus, where Frequency Tracking and Transform 2 are set at note-on, all three
// move in real time here.
// ---------------------------------------------------------------------------
static constexpr int zSections = 6, zStages = zSections + 1; // stage 0 is the low-pass
struct ZFrame
{
    float f[zSections], g[zSections], q[zSections]; // Hz at 1 kHz, dB, Q
    float lpF{0}, lpQ{0.707f};                      // the low-pass section: Hz at 1 kHz (0 = open), Q
};
struct ZDesign
{
    const char *name;
    ZFrame a, b;
    ZFrame m{};       // optional middle frame: Morph goes A -> M -> B (E-mu's three-vowel filters, e.g. Ah-Ay-Ee)
    bool mid{false};
};
inline const ZDesign *zDesigns(int &count)
{
    static const ZDesign d[] = {
        // Ah -> Ay -> Ee, measured from E-mu's ESI "Vocal Ah-Ay-Ee" (2 demo recordings, Nov 2026): a rounder Ah,
        // the first formant falling early (by the middle) and a wide, bright Ee
        {"Vowel Ah > Ee", {{551, 984, 2186, 3300, 150, 9000}, {14, 11, 8, 4, -6, -10}, {7, 8, 9, 9, 0.7f, 0.7f}},
                          {{278, 2755, 4159, 5300, 150, 9000}, {13, 12, 9, 4, -6, -10}, {7, 9, 10, 9, 0.7f, 0.7f}},
                          {{334, 1857, 3281, 4200, 150, 9000}, {13, 12, 9, 4, -6, -10}, {7, 9, 10, 9, 0.7f, 0.7f}}, true},
        {"Vowel Oh > Ah", {{570, 840, 2410, 3300, 150, 9000}, {14, 11, 7, 3, -6, -12}, {7, 8, 9, 9, 0.7f, 0.7f}},
                          {{730, 1090, 2440, 3400, 150, 9000}, {14, 11, 8, 4, -6, -10}, {7, 8, 9, 9, 0.7f, 0.7f}}},
        {"Vowel Oo > Eh", {{300, 870, 2240, 3200, 150, 9000}, {15, 10, 6, 3, -6, -14}, {7, 8, 9, 9, 0.7f, 0.7f}},
                          {{530, 1840, 2480, 3500, 150, 9000}, {14, 12, 8, 4, -6, -10}, {7, 9, 9, 9, 0.7f, 0.7f}}},
        {"Phaser Notches", {{180, 520, 1500, 4300, 9000, 13000}, {-24, -24, -24, -24, -12, -6}, {2.5f, 2.5f, 2.5f, 2.5f, 2, 2}},
                           {{360, 1040, 3000, 8600, 12000, 15000}, {-24, -24, -24, -24, -12, -6}, {2.5f, 2.5f, 2.5f, 2.5f, 2, 2}}},
        {"Comb Shift", {{250, 500, 750, 1000, 1250, 1500}, {9, 9, 8, 8, 7, 7}, {14, 16, 18, 20, 22, 24}},
                       {{370, 740, 1110, 1480, 1850, 2220}, {9, 9, 8, 8, 7, 7}, {14, 16, 18, 20, 22, 24}}},
        {"Talking Bass", {{180, 420, 1200, 2600, 5200, 9000}, {15, -6, -12, -18, -24, -24}, {3, 1, 1, 1, 1, 1}},
                         {{900, 1600, 2800, 4200, 7000, 11000}, {14, 4, -6, -14, -20, -24}, {3, 1.5f, 1, 1, 1, 1}}},
        {"Bell Ring", {{300, 828, 1620, 2679, 4002, 5592}, {12, 10, 8, 7, 6, 5}, {25, 25, 25, 25, 25, 25}},
                      {{520, 1435, 2808, 4644, 6937, 9693}, {12, 10, 8, 7, 6, 5}, {25, 25, 25, 25, 25, 25}}},
        {"Hollow > Bright", {{400, 1000, 2500, 6000, 120, 12000}, {-18, -12, 6, 8, -6, 0}, {1.5f, 1.5f, 2, 2, 0.7f, 1}},
                            {{250, 700, 3500, 9000, 120, 12000}, {8, -14, -10, 10, 0, 6}, {1.5f, 1.5f, 2, 2, 0.7f, 1}}},
        // the manual's example: a low-pass filter morphing into a band-pass
        {"Lowpass > Bandpass", {{60, 150, 400, 1000, 2500, 6000}, {0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1}, 1200, 0.9f},
                               {{60, 150, 400, 1000, 2500, 6000}, {-24, -20, -12, 6, 0, 0}, {0.8f, 0.8f, 1, 2, 1, 1}, 1400, 4.f}},
        {"Resonant Sweep", {{500, 1000, 2000, 4000, 8000, 12000}, {0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1}, 250, 6.f},
                           {{500, 1000, 2000, 4000, 8000, 12000}, {0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1}, 6000, 6.f}},
        // after shapes described in the Morpheus manual's filter reference (frequencies from the descriptions)
        {"BroadRes", {{80, 160, 320, 2000, 6000, 10000}, {0, 0, 0, 0, 0, 0}, {3, 3, 3, 4, 4, 4}},
                     {{80, 160, 320, 2000, 6000, 10000}, {-24, -24, -24, 12, 12, 12}, {3, 3, 3, 4, 4, 4}}},
        {"Rubber Hose", {{47, 95, 190, 380, 750, 1500}, {-3, 2, 2, 2, 2, 2}, {4, 5, 5, 5, 5, 5}},
                        {{47, 95, 190, 380, 750, 1500}, {-24, 12, 12, 12, 11, 10}, {4, 5, 5, 5, 5, 5}}},
        {"Swingshift", {{349, 494, 698, 988, 1397, 1976}, {0, 0, 0, 0, 0, 0}, {8, 8, 8, 8, 8, 8}, 250, 0.7f},
                       {{349, 494, 698, 988, 1397, 1976}, {12, 12, 11, 10, 8, 6}, {8, 8, 8, 8, 8, 8}}},
        {"500 Up", {{500, 1000, 1500, 2000, 2500, 3000}, {3, -3, 3, -3, 3, -3}, {6, 6, 6, 6, 6, 6}},
                   {{500, 1000, 1500, 2000, 2500, 3000}, {10, -24, 10, -24, 10, -24}, {6, 6, 6, 6, 6, 6}}},
        {"Odd > Even", {{110, 330, 550, 770, 990, 1210}, {12, 12, 11, 10, 9, 8}, {12, 14, 16, 18, 20, 22}},
                       {{220, 440, 660, 880, 1100, 1320}, {12, 12, 11, 10, 9, 8}, {12, 14, 16, 18, 20, 22}}},
        {"Guitar Body", {{100, 210, 390, 800, 2200, 4800}, {9, 7, -6, 4, 3, -8}, {6, 8, 3, 4, 3, 1.5f}, 7000, 0.7f},
                        {{120, 260, 470, 1100, 2700, 5600}, {6, 10, -9, 7, 6, -4}, {5, 9, 4, 5, 4, 1.5f}, 9000, 0.7f}},
        // ----- 0.19: measured from E-mu ESI demo recordings (always add new shapes at the end) -----
        // Oo -> Oh -> Ah, after "Vocal Oo-Ah"
        {"Vowel Oo > Ah", {{269, 707, 2200, 3200, 150, 9000}, {15, 10, 5, 2, -6, -14}, {7, 8, 9, 9, 0.7f, 0.7f}},
                          {{640, 1426, 2543, 3800, 150, 9000}, {14, 11, 8, 4, -6, -10}, {7, 8, 9, 9, 0.7f, 0.7f}},
                          {{445, 911, 2300, 3300, 150, 9000}, {14, 11, 6, 3, -6, -12}, {7, 8, 9, 9, 0.7f, 0.7f}}, true},
        // after "Phaser 1": two deep notches two octaves apart, travelling two octaves
        {"Phaser 2-Notch", {{460, 1840, 5000, 9000, 12000, 15000}, {-30, -30, 0, 0, 0, 0}, {2, 2, 1, 1, 1, 1}},
                           {{1900, 7600, 5000, 9000, 12000, 15000}, {-30, -30, 0, 0, 0, 0}, {2, 2, 1, 1, 1, 1}}},
        // after "Phaser 2": two notches whose gap closes from about 3 1/4 octaves to 2 1/2 as they rise
        {"Phaser Wide", {{230, 2380, 5000, 9000, 12000, 15000}, {-30, -30, 0, 0, 0, 0}, {2, 2, 1, 1, 1, 1}},
                        {{2050, 11000, 5000, 9000, 12000, 15000}, {-30, -30, 0, 0, 0, 0}, {2, 2, 1, 1, 1, 1}}},
        // after "Bat-Phaser": a strong moving peak with a notch about 3 1/2 semitones under it
        {"Bat Phaser", {{750, 610, 5000, 9000, 12000, 15000}, {15, -24, 0, 0, 0, 0}, {4, 4, 1, 1, 1, 1}},
                       {{6000, 4850, 5000, 9000, 12000, 15000}, {15, -24, 0, 0, 0, 0}, {4, 4, 1, 1, 1, 1}}},
    };
    count = (int)(sizeof(d) / sizeof(d[0]));
    return d;
}

// Which part of the spectrum a filter type lets through. "Open" types (notch, allpass,
// peak, comb, phaser, shelves...) cannot mute a band, so they are never limited.
inline PassKind passKindOf(sfpp::FilterModel m, const sfpp::ModelConfig &c)
{
    using P = sfpp::Passband;
    switch (c.pt)
    {
    case P::LP:
    case P::NotchAndLP:
    case P::PhaserAndLP:
    case P::LowLowLow:
        return PassKind::LowPass;
    case P::HP:
    case P::HighHighHigh:
        return PassKind::HighPass;
    case P::BP:
    case P::LowHighLow:
    case P::HighLowHigh:
        return PassKind::BandPass;
    case P::UNSUPPORTED:
        // ladder models carry no passband tag: they are low-pass
        if (m == sfpp::FilterModel::VintageLadder || m == sfpp::FilterModel::DiodeLadder ||
            m == sfpp::FilterModel::VemberLadder || m == sfpp::FilterModel::OBXD_4Pole)
            return PassKind::LowPass;
        return PassKind::Open;
    default:
        return PassKind::Open;
    }
}

inline const std::vector<ModelInfo> &filterCatalogue()
{
    // Deliberately never destroyed (no static destructor order problems at unload).
    static const std::vector<ModelInfo> &cat = *new std::vector<ModelInfo>([]()
    {
        std::vector<ModelInfo> r;
        for (auto m : sfpp::Filter::availableModels())
        {
            ModelInfo mi;
            mi.model = m;
            mi.name = sfpp::toString(m);
            if (m != sfpp::FilterModel::None)
                mi.configs = sfpp::Filter::availableModelConfigurations(m, true);
            for (auto &c : mi.configs)
            {
                auto lab = c.toString();
                mi.labels.push_back(lab.empty() ? std::string("Default") : lab);
                mi.kinds.push_back(passKindOf(m, c));
            }
            r.push_back(std::move(mi));
        }
        if (r.empty() || r[0].model != sfpp::FilterModel::None)
        {
            ModelInfo none;
            none.name = "None";
            r.insert(r.begin(), none);
        }
        {
            ModelInfo z;
            z.name = "Z-Plane";
            z.zplane = true;
            int n = 0;
            const ZDesign *d = zDesigns(n);
            for (int i = 0; i < n; ++i)
                z.configs.push_back(sfpp::ModelConfig{}), z.labels.push_back(d[i].name), z.kinds.push_back(PassKind::Open);
            r.push_back(std::move(z));
        }
        return r;
    }());
    return cat;
}

inline int variantIndexFor(int modelIndex, float variant01)
{
    const auto &cat = filterCatalogue();
    if (modelIndex < 0 || modelIndex >= (int)cat.size())
        return 0;
    int n = (int)cat[(size_t)modelIndex].configs.size();
    if (n <= 1)
        return 0;
    int i = (int)std::lround(std::clamp(variant01, 0.f, 1.f) * (float)(n - 1));
    return std::clamp(i, 0, n - 1);
}

inline bool morphUsed(int modelIndex, int variantIndex)
{
    const auto &cat = filterCatalogue();
    if (modelIndex <= 0 || modelIndex >= (int)cat.size() || cat[(size_t)modelIndex].configs.empty())
        return false;
    const auto &mi = cat[(size_t)modelIndex];
    if (mi.zplane)
        return true;
    int v = std::clamp(variantIndex, 0, (int)mi.configs.size() - 1);
    return sfpp::Filter::coefficientsExtraCount(mi.model, mi.configs[(size_t)v]) > 0;
}

// ---------------------------------------------------------------------------
// Settings. Cutoffs are in semitones from A440 (-60 .. 70).
// ---------------------------------------------------------------------------
struct BandSettings
{
    // filter
    int model{0}, variant{0};
    float cutoff{0};    // semitones from A440
    float resonance{0}; // 0..1
    float morph{0};     // 0..1
    bool filterOn{true};
    // drive
    int driveType{Warm};
    float drive{0};         // 0 = clean .. 1
    bool driveFirst{false}; // false: filter -> drive, true: drive -> filter
    // level
    float levelDb{0}; // -60 (off) .. +12
    float pan{0};     // -1 left .. +1 right
    bool mute{false}; // Parallel: silences the band. Series: bypasses the strip.
    bool solo{false}; // Parallel only
    // Bank (in the spirit of Slink Filter): eight narrow band-pass filters spread across the
    // strip's part of the spectrum, each with its own gain, with ripples running through them
    float bankMix{0};         // 0 = off .. 1 = only the bank
    float bankWidth{0.5f};    // 0 narrow .. 1 wide
    float bankGain[8]{};      // -1..1 each: 0 leaves that band alone, up boosts (to +12 dB), down cuts (to silence)
    float rippleDepth{0};     // 0..1
    float rippleRateHz{0.5f}; // speed of the ripple
    float rippleWaves{1};     // how many ripples across the eight bands (0.25 .. 4)
    float bankStereo{0};      // 0 .. 1: alternate bands go to the left and to the right
    bool rippleSync{false};   // ripple locked to the song (rippleSyncIndex) or free (rippleRateHz)
    int rippleSyncIndex{4};
    int rippleShape{0};       // 0 Wave, 1 Bounce, 2 Random, 3 Spin, 4 Rise, 5 Spotlight
    int rippleBoth{0};        // bit k set: the ripple swings band k both ways; clear: one way (the way its slider points)
    float stripMix{1};        // 0 = this strip leaves its sound untouched .. 1 = fully processed
    float modDepth{1};        // 0 .. 2: how much all the movement (Sway, Mutation, lanes, LFOs, matrix) moves this strip
};
static constexpr int numBankBands = 8;

// Offsets added on top of the knobs: host modulation (Bitwig), and inside the engine Sway,
// the Conductor and Mutation (later the sequencer lanes too).
struct BandMod
{
    float cutoff{0}, resonance{0}, morph{0}, drive{0}, levelDb{0}, pan{0};
};

struct ModeSettings
{
    BandSettings band[numBands];
    float xoverLoHz{250.f}, xoverHiHz{3000.f}; // Parallel only
    float inputDb{0}, outputDb{0};
    float mix{1}; // 0 dry .. 1 wet
    // feedback (low-passed, DC-blocked, soft-saturated, never runs away)
    bool feedbackOn{false};
    float feedback{0};       // 0..1
    float feedbackToneHz{6000};
    // noise, fed in before the filters so it rings with them
    bool noiseOn{false};
    float noise{0};      // 0..1 (cube law)
    float noiseColor{0}; // 0 white .. 1 dark
};

// Sway: four slow LFOs at rates that never line up (1 : golden ratio : root 2 : pi/4).
static constexpr float swayBarsTable[7] = {0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f, 16.f};
struct SwaySettings
{
    float amount{0};         // 0..1
    float rateHz{0.12f};     // Free: speed of the slowest LFO
    bool sync{false};        // Sync: one slow cycle per syncBars bars, locked to Bitwig's song position
    int syncIndex{4};        // into swayBarsTable (4 = 4 bars)
    bool stepped{false};     // Stepped: holds a value, then jumps (8 steps per slow cycle)
    float slew{0.4f};        // Stepped: how much of a step the jump takes (0 = hard)
    float follow{0};         // 0..1: louder input = more sway
    float depthSemis{12.f};  // cutoff movement at full Sway
    // what Sway moves (each 0..1): cutoff by depthSemis, the others over their own ranges
    float toCut{1.f}, toRes{0}, toMorph{0}, toDrive{0}, toLevel{0}, toPan{0};
    // each band's own feel (the main Sway knob still sets how much for all of them)
    struct Band
    {
        int motion{0};      // 0 = as the main setting, 1 Smooth, 2 Stepped
        float rateMul{1.f}; // its speed against the main rate (0.25 .. 4)
        float depth{1.f};   // 0..1: how much of the main Sway this band gets
        float slew{0.4f};   // Stepped: how much of a step the jump takes
    } band[numBands];
};

// The Conductor: Mid passes its own cutoff movement on to Low and High.
struct ConductorSettings
{
    float influence{0}; // 0..1: how much of Mid's movement is passed on
    float lean{0.5f};   // 0 all to Low .. 1 all to High
    // sporadic, but on the beat
    int grid{0};        // 0 = continuous, else 1 + index into condGridBeats: Mid's movement is passed on in steps
    int divLow{1}, divHigh{1}; // clock dividers: Low / High take a new value every Nth step
    float chaos{0};     // 0 = Mid's own movement .. 1 = random jumps (one per step)
    float range{12};    // semitones of the random jumps, bursts and third wheel
    float chance{1};    // chance that a step passes anything on
    int condition{0};   // Elektron-style bar conditions (see condConditions)
    float bursts{0};    // chance per bar of a burst: a beat of fast 1/32 jumps
    float mischief{0};  // Jack, the third wheel: now and then knocks a strip slightly out of whack
    int musical{0};     // jumps snap to: 0 off, 1 semitones, 2 fifths, 3 octaves
    int seed{1};
    // Jack: when one strip moves, another moves the opposite way
    float jack{0};      // 0..1
    int jackPair{0};    // see jackPairs
    int jackParam{0};   // which knob answers: cutoff, reso, morph, drive, level, pan
};
// 1/64, 1/32T, 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2, 1 bar, 2 bars, 4 bars (quarter notes)
static constexpr int numCondGrid = 16;
static constexpr float condGridBeats[numCondGrid] = {0.0625f, 1.f / 12.f, 0.125f, 1.f / 6.f, 0.25f, 0.375f, 1.f / 3.f, 0.5f, 0.75f, 2.f / 3.f, 1.f, 1.5f, 2.f, 4.f, 8.f, 16.f};
// conditions: {A, B} = act on bar A of every B bars ({0,0} always)
static constexpr int numCondConditions = 12;
static constexpr int condConditions[numCondConditions][2] = {{0, 0}, {1, 2}, {2, 2}, {1, 3}, {2, 3}, {3, 3}, {1, 4}, {2, 4}, {3, 4}, {4, 4}, {1, 8}, {8, 8}};

// Mutation: when something happens in Mid, Low and High answer.
enum MutTrigger { TrigLevel = 0, TrigCutoff, TrigGuard, TrigStep, TrigFollower, TrigChaperone, numMutTriggers };
enum MutMode { MutOff = 0, MutTandem, MutContrast };
enum MutTarget { TgtCutoff = 0, TgtReso, TgtDrive, TgtLevel, TgtPan, numMutTargets };
static constexpr int mutEveryTable[5] = {1, 2, 4, 8, 16};
struct MutResponse
{
    int mode{MutOff};
    int target{TgtCutoff};
    float amount{0.5f}; // 0..1 (cutoff 24 st, reso 0.5, drive 0.5, level 12 dB, pan 1)
    float chance{1.f};  // 0..1
    int delaySteps{0};  // 0..4 sixteenths
};
struct MutationSettings
{
    int trigger{TrigLevel};
    float sensitivity{0.5f};
    int holdSteps{1};     // at least this many sixteenths between events
    MutResponse resp[2];  // [0] Low, [1] High
    float mutate{0};      // 0..1: how far the answers drift each period
    int everyIndex{2};    // into mutEveryTable (bars)
    bool freeze{false};
    int seed{0};
};

// ---------------- Phase 3: modulation sources and the matrix ----------------
static constexpr int numLanes = 3, numSteps = 32, numLfos = 3, numSlots = 16;
enum ModSourceId { SrcNone = 0, SrcSeqLow, SrcSeqMid, SrcSeqHigh, SrcLfo1, SrcLfo2, SrcLfo3, SrcEnv, SrcRipLow, SrcRipMid, SrcRipHigh, numModSources };
// Targets: 0 none, then 6 per strip (Low 1-6, Mid 7-12, High 13-18), then the shared ones.
enum BandParamId { PCut = 0, PRes, PMorph, PDrive, PLevel, PPan, numBandParams };
enum ModTargetId
{
    TgtNone = 0,
    TgtSplitLo = 1 + numBands * numBandParams,
    TgtSplitHi,
    TgtFeedback,
    TgtNoise,
    TgtMix,
    TgtSwayAmt,
    TgtModDepth, // the master Mod Depth itself
    TgtSpecBalance, TgtSpecSharp,                       // the spectral split (when Spectral is on)
    TgtStripDepthLow, TgtStripDepthMid, TgtStripDepthHigh, // each strip's own Mod Depth
    TgtTuneAmount, TgtTuneRing, TgtTuneTilt,                // Tune
    TgtTuneSlope,                                           // Tune's tone (0.18)
    numModTargetIds
};
constexpr int bandTarget(int band, int param) { return 1 + band * numBandParams + param; }
// how far 100% moves each kind of target
inline float targetScale(int t)
{
    if (t >= 1 && t < TgtSplitLo)
    {
        switch ((t - 1) % numBandParams)
        {
        case PCut: return 24.f;   // semitones
        case PLevel: return 24.f; // dB
        default: return 1.f;      // reso, morph, drive, pan: the whole range
        }
    }
    if (t == TgtSplitLo || t == TgtSplitHi)
        return 24.f;
    return 1.f;
}

// sequencer step lengths in quarter notes: 1/32, 1/16T, 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2, 1 bar
static constexpr int numRippleSync = 14;
static constexpr float rippleSyncBeats[numRippleSync] = {0.0625f, 0.125f, 0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f, 16.f, 1.f / 6.f, 1.f / 3.f, 2.f / 3.f, 0.75f, 1.5f};
static constexpr int numLaneRates = 17;
static constexpr float laneRateBeats[numLaneRates] = {0.125f, 1.f / 6.f, 0.25f, 1.f / 3.f, 0.5f, 2.f / 3.f, 1.f, 2.f, 4.f,
                                                      // added later (kept at the end so saved projects keep their rates)
                                                      0.0625f, 1.f / 12.f, 0.375f, 0.75f, 1.5f, 3.f, 8.f / 3.f, 0.1875f};
struct LaneSettings
{
    float step[numSteps]{}; // -1..1: down or up from the knob
    int length{16};         // 1..32
    int direction{0};       // 0 forward, 1 reverse, 2 ping-pong, 3 random (the same every time through)
    int rateIndex{2};       // into laneRateBeats
    int euclid{0};          // 0 Off, 1 Gate (rests are 0), 2 Trig (hits are +1)
    int hits{4}, rotate{0};
    float smooth{0};        // 0..1 of a step to glide
    float depth{1};         // scales the whole lane (its own target and the matrix)
    int target{TgtNone};    // the lane's own target
    bool freeRun{false};    // Clock (tempo, song position) or Free (speedHz)
    float speedHz{4};       // Free: steps per second
    float swing{0.5f};      // 0.5 straight .. 0.75
    float offset{0};        // -0.5..0.5 of a step, positive = later
    int lockTarget{TgtNone};
    float lockGlideMs{2};
    LaneSettings()
    {
        for (auto &r : plock)
            for (auto &v : r)
                v = std::numeric_limits<float>::quiet_NaN();
    }
    // Elektron-style parameter locks: per step, any of the band's six knobs (absolute values;
    // NaN = not locked). Locks ride this lane's steps (or its own length / step size below).
    float plock[numBandParams][numSteps];
    int lockLength{0};      // 0 = the sequencer's length, else 1..32
    int lockRateIndex{0};   // 0 = the sequencer's rate, else 1 + index into laneRateBeats
    float sway{0};          // 0..1: Sway pushes the timing ahead of and behind the grid (up to 4 steps)
};
enum Retrigger { RetrigSongPos = 0, RetrigBar, Retrig2Bars, Retrig4Bars, RetrigOnStart };

// LFO cycle lengths: 1/16, 1/8, 1/4, 1/2 (quarter notes), then 1, 2, 4, 8 bars
static constexpr float lfoSyncLen[8] = {0.25f, 0.5f, 1.f, 2.f, 1.f, 2.f, 4.f, 8.f};
static constexpr bool lfoSyncBars[8] = {false, false, false, false, true, true, true, true};
enum LfoShape { ShapeCustom = 0, ShapeSine, ShapeTriangle, ShapeRamp, ShapeSquare, ShapePump, ShapeSwell, ShapeBounce, ShapeStairs, ShapeSH, numShapes };
enum LfoMode { LfoSync = 0, LfoFree, LfoRetrig };
struct LfoSettings
{
    int shape{ShapeSine};
    int mode{LfoSync};
    int syncIndex{4};   // into lfoSyncLen (4 = 1 bar)
    float rateHz{1.f};  // Free
    float phase{0};     // 0..1
    float smooth{0};    // 0..1
    bool unipolar{false};
    float sway{0};      // 0..1: Sway (Mid's) pushes the phase ahead and behind (up to a quarter cycle)
    int target{0};      // its own destination (like a lane's), besides the matrix
    float amount{0};    // -1..1
    // the Steps shape (Hydrasynth-style step LFO, set like an arpeggio)
    float step[16]{};   // -1..1
    int stepCount{8};   // 1..16
    float glide{0};     // 0..1 of a step
    int quantize{0};    // 0 off, 1 semitones, 2 major, 3 minor, 4 pentatonic, 5 octaves (on a 24-semitone scale)
};
static constexpr int ShapeSteps = numShapes; // after the table shapes
struct CurvePoint
{
    float x, y, t; // x 0..1, y -1..1, t bends the segment that ends here (1 straight)
};
static constexpr int maxCurvePoints = 32;

enum EnvSource { EnvInput = 0, EnvSidechain, EnvLow, EnvMid, EnvHigh };
struct EnvSettings
{
    int source{EnvInput};
    float sensDb{6.f}, attackMs{5.f}, releaseMs{150.f};
    float scGainDb{0.f}, scHpHz{20.f}, scLpHz{20000.f};
};
struct ModSlot
{
    int source{SrcNone};
    int target{TgtNone};
    float amount{0}; // -1..1
    int swing{0};    // 0 as the source moves, 1 both ways (around the knob), 2 one way (up from the knob)
};

// Bitwig's transport, handed over at the start of every host block.
struct HostTime
{
    bool playing{false};
    bool beatsValid{false};
    double bpm{120};
    double beats{0}; // song position in quarter notes
    int num{4}, den{4};
};

// Tune: narrow resonators on the notes of a key, so the pitched part of whatever comes in rings out.
// Its own idea, after the Torso S-4's RING (a tuned filter bank); not a copy of it.
struct TuneSettings
{
    bool on{false};
    int root{9};             // 0 C .. 11 B
    int scale{10};           // see tuneScaleNotes (10 = Minor)
    bool beforeStrips{false};// true: rings first and the strips filter it; false: rings after the strips
    float amount{0.4f};      // 0..1
    bool layer{true};        // true Layer: the dry sound stays whole, the ringing goes on top; false Blend: 0 dry .. 1 only the ringing
    float ringMs{400};       // how long a note rings (time constant), 20..4000 ms
    int lowest{36}, highest{96}; // MIDI notes
    float tilt{0};           // -1 favour low notes .. +1 favour high
    float detuneCents{0};    // 0..50: each note pulled a little off pitch
    float width{0.5f};       // 0..1: alternate notes to the left and the right
    int motion{0};           // 0 Still, 1 Wave, 2 Drift
    float motionHz{0.25f};
    bool motionSync{false};  // true: Motion follows the song (motionDiv), false: free (motionHz)
    int motionDiv{9};        // into tuneDivBeats (9 = 1 bar)
    float slope{0};          // -1 darker .. +1 brighter: a tilt EQ on the ringing (the dry sound is left alone)
    float pivotHz{1000};     // where Slope pivots
};
// Motion's synced lengths: quarter notes, or (negative) bars
static constexpr int numTuneDivs = 14;
static constexpr double tuneDivBeats[numTuneDivs] = {0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75, 1.0, 4.0 / 3.0, 1.5, 2.0, -1, -2, -4, -8, -16};

struct Settings
{
    int routing{Parallel};
    int splitMode{0};      // Parallel only: 0 crossover filters, 1 spectral (FFT)
    int specBy{0};         // spectral: 0 by frequency, 1 tonal (tones / hits / air)
    float specBalance{0};  // -1..1
    float specSharp{0.6f}; // 0..1
    int specQuality{1};    // FFT frame at the host rate: 0 1024, 1 2048, 2 4096, 3 8192
    ModeSettings mode[2]; // [Parallel], [Series]: each routing keeps its own settings
    BandMod mod[numBands];
    int oversample{1};     // 0 off, 1 = 2x, 2 = 4x
    float modDepth{1};     // 0 .. 2: master depth for everything that moves (P-locks excepted)
    bool smartEdges{true}; // keep a filter from muting its own band / the chain
    bool autoGain{true};   // level-match the drives
    bool limiter{true};    // guards + output limiter
    SwaySettings sway;
    ConductorSettings conductor;
    MutationSettings mutation;
    LaneSettings lane[numLanes];
    int retrigger{RetrigSongPos};
    LfoSettings lfo[numLfos];
    EnvSettings env;
    ModSlot slot[numSlots];
    TuneSettings tune;
};

// Builds a 512-point table from curve points (shared by the made shapes and drawn curves).
static constexpr int curveN = 512;
inline void renderCurve(const CurvePoint *p, int n, float *table)
{
    if (n < 2)
    {
        for (int i = 0; i <= curveN; ++i)
            table[i] = n == 1 ? p[0].y : 0.f;
        return;
    }
    int k = 0;
    for (int i = 0; i <= curveN; ++i)
    {
        float x = (float)i / curveN;
        while (k < n - 2 && x > p[k + 1].x)
            ++k;
        const auto &a = p[k], &b = p[k + 1];
        float w = b.x - a.x;
        float u = w > 1.0e-6f ? std::clamp((x - a.x) / w, 0.f, 1.f) : 1.f;
        float t = std::clamp(b.t, 0.05f, 20.f);
        table[i] = std::clamp(a.y + (b.y - a.y) * std::pow(u, t), -1.f, 1.f);
    }
}
inline void shapeTable(int shape, float *table)
{
    auto pts = [&](std::initializer_list<CurvePoint> l) { renderCurve(l.begin(), (int)l.size(), table); };
    switch (shape)
    {
    case ShapeSine:
        for (int i = 0; i <= curveN; ++i)
            table[i] = (float)std::sin(2.0 * 3.14159265358979 * i / curveN);
        break;
    case ShapeTriangle: pts({{0, 0, 1}, {0.25f, 1, 1}, {0.75f, -1, 1}, {1, 0, 1}}); break;
    case ShapeRamp: pts({{0, -1, 1}, {1, 1, 1}}); break;
    case ShapeSquare: pts({{0, 1, 1}, {0.5f, 1, 1}, {0.5f, -1, 1}, {1, -1, 1}}); break;
    case ShapePump: pts({{0, -1, 1}, {0.85f, 1, 0.35f}, {1, 1, 1}}); break;
    case ShapeSwell: pts({{0, -1, 1}, {0.9f, 1, 3}, {0.9f, -1, 1}, {1, -1, 1}}); break;
    case ShapeBounce:
        pts({{0, 1, 1}, {0.28f, -1, 2}, {0.46f, 0.35f, 0.5f}, {0.62f, -1, 2}, {0.74f, -0.35f, 0.5f}, {0.84f, -1, 2},
             {0.92f, -0.75f, 0.5f}, {1, -1, 2}});
        break;
    case ShapeStairs: pts({{0, -1, 1}, {0.25f, -1, 1}, {0.25f, -0.33f, 1}, {0.5f, -0.33f, 1}, {0.5f, 0.33f, 1}, {0.75f, 0.33f, 1}, {0.75f, 1, 1}, {1, 1, 1}}); break;
    case ShapeSH:
    {
        static constexpr float v[8] = {0.42f, -0.81f, 0.13f, 0.95f, -0.37f, 0.66f, -0.58f, -0.12f};
        for (int i = 0; i <= curveN; ++i)
            table[i] = v[std::min(7, i * 8 / curveN)];
        break;
    }
    default: // the starting drawn curve
        pts({{0, 0, 1}, {0.12f, 0.9f, 0.5f}, {0.3f, 0.2f, 2}, {0.42f, 0.6f, 1}, {0.62f, -0.8f, 0.45f}, {0.8f, -0.2f, 2.2f}, {1, 0, 1}});
        break;
    }
}

static inline float hzToSemi(double hz) { return (float)(12.0 * std::log2(hz / 440.0)); }
static inline double semiToHz(double st) { return 440.0 * std::pow(2.0, st / 12.0); }
static inline float dbToLin(float db) { return db <= -60.f ? 0.f : std::pow(10.f, db / 20.f); }

// Smooth "eases against a floor": above floor+K the value is untouched, below floor-K it
// rests on the floor, and in between it bends over with no corner.
static inline float softFloor(float x, float fl, float K)
{
    if (x >= fl + K)
        return x;
    if (x <= fl - K)
        return fl;
    float d = fl + K - x;
    return x + d * d / (4.f * K);
}
static inline float softCeil(float x, float c, float K) { return -softFloor(-x, -c, K); }

// ---------------------------------------------------------------------------
// Spectral split (Parallel's other way of splitting): a short-time FFT divides the sound into
// three parts, either by frequency (steep, no phase shift) or by character (Tonal: steady tones,
// hits, and the noisy air in between). The three masks always add up to one, so with the strips
// left alone the sound comes back whole, one FFT frame later.
//   frame N = 2048 at the host rate (more when oversampled), hop N/4, Hann in and out.
//   Left and right ride in one complex FFT (l + i r); a real, symmetric mask keeps them apart.
// ---------------------------------------------------------------------------
struct SpectralSplit
{
    static constexpr int maxN = 32768, histT = 8; // up to 8192 at the host rate, x4 when oversampled
    int N{2048}, hop{512}, bins{1025};
    float sr{48000};
    // settings, from the control thread
    int by{0};                 // 0 frequency, 1 tonal
    float f1{250}, f2{3000};   // split points (Hz) for frequency mode
    float balance{0};          // -1..1: tonal mode favours hits .. tones; frequency mode moves both splits
    float sharp{0.6f};         // 0 soft .. 1 steep (frequency) / clean separation (tonal)
    // state
    float inL[maxN]{}, inR[maxN]{};          // the last N inputs (ring)
    float dryL[maxN]{}, dryR[maxN]{};        // the dry signal, N late (ring)
    float acc[3][2][2 * maxN]{};             // overlap-add per band, per channel
    float ready[3][2][maxN / 4]{};           // the hop being played out
    float re[maxN]{}, im[maxN]{}, yr[maxN]{}, yi[maxN]{};
    float win[maxN]{}, twr[maxN / 2]{}, twi[maxN / 2]{};
    int rev[maxN]{};
    float mag[maxN / 2 + 1]{}, hist[histT][maxN / 2 + 1]{}, mask[3][maxN / 2 + 1]{};
    int w{0}, filled{0}, rd{0}, histPos{0}, frames{0};

    int hostN{2048};
    void setup(double rate, int osf, int frameAtHost)
    {
        sr = (float)rate;
        hostN = frameAtHost;
        N = std::min(maxN, frameAtHost * std::max(1, osf));
        hop = N / 4, bins = N / 2 + 1;
        for (int i = 0; i < N; ++i)
            win[i] = 0.5f - 0.5f * std::cos(6.283185307f * (float)i / (float)N); // periodic Hann
        int lg = 0;
        while ((1 << lg) < N)
            ++lg;
        for (int i = 0; i < N; ++i)
        {
            int r = 0;
            for (int b = 0; b < lg; ++b)
                r |= ((i >> b) & 1) << (lg - 1 - b);
            rev[i] = r;
        }
        for (int i = 0; i < N / 2; ++i)
            twr[i] = std::cos(6.283185307f * (float)i / (float)N), twi[i] = -std::sin(6.283185307f * (float)i / (float)N);
        reset();
    }
    void reset()
    {
        std::memset(inL, 0, sizeof(inL)), std::memset(inR, 0, sizeof(inR));
        std::memset(dryL, 0, sizeof(dryL)), std::memset(dryR, 0, sizeof(dryR));
        std::memset(acc, 0, sizeof(acc)), std::memset(ready, 0, sizeof(ready));
        std::memset(hist, 0, sizeof(hist)), std::memset(mask, 0, sizeof(mask));
        w = filled = rd = histPos = frames = 0;
    }
    int latency() const { return N; }
    // in-place radix-2 FFT (inverse: conjugate twiddles, no scaling)
    void fft(float *xr, float *xi, bool inverse)
    {
        for (int i = 0; i < N; ++i)
            if (rev[i] > i)
                std::swap(xr[i], xr[rev[i]]), std::swap(xi[i], xi[rev[i]]);
        for (int len = 2; len <= N; len <<= 1)
        {
            int half = len >> 1, step = N / len;
            for (int i = 0; i < N; i += len)
                for (int j = 0; j < half; ++j)
                {
                    float wr = twr[j * step], wi = inverse ? -twi[j * step] : twi[j * step];
                    int a = i + j, b = a + half;
                    float tr = xr[b] * wr - xi[b] * wi, ti = xr[b] * wi + xi[b] * wr;
                    xr[b] = xr[a] - tr, xi[b] = xi[a] - ti;
                    xr[a] += tr, xi[a] += ti;
                }
        }
    }
    static float smooth01(float x) { x = std::clamp(x, 0.f, 1.f); return x * x * (3.f - 2.f * x); }
    void makeMasks()
    {
        if (by == 0)
        {
            // frequency: soft-edged bands, from half an octave wide (soft) to a twentieth (steep)
            float width = 0.5f - 0.45f * std::clamp(sharp, 0.f, 1.f);
            float shift = std::exp2(std::clamp(balance, -1.f, 1.f)); // Balance slides both splits up to an octave
            float a = f1 * shift, b = f2 * shift;
            for (int k = 0; k < bins; ++k)
            {
                float f = std::max(1.f, (float)k * sr / (float)N);
                float hiA = smooth01(std::log2(f / a) / width + 0.5f), hiB = smooth01(std::log2(f / b) / width + 0.5f);
                mask[0][k] = 1.f - hiA, mask[2][k] = hiB, mask[1][k] = std::max(0.f, hiA - hiB);
            }
            return;
        }
        // tonal: steady tones are the median over time; hits the median across frequency
        float med[histT];
        float bal = std::exp2(2.f * std::clamp(balance, -1.f, 1.f)), sep = 1.f + 3.f * std::clamp(sharp, 0.f, 1.f);
        int nh = std::min(frames, histT);
        for (int k = 0; k < bins; ++k)
        {
            for (int t = 0; t < nh; ++t)
                med[t] = hist[t][k];
            float H = 0.f;
            if (nh > 0)
            {
                std::nth_element(med, med + nh / 2, med + nh);
                H = med[nh / 2];
            }
            float win17[17];
            int c = 0;
            for (int j = std::max(0, k - 8); j <= std::min(bins - 1, k + 8); ++j)
                win17[c++] = mag[j];
            std::nth_element(win17, win17 + c / 2, win17 + c);
            float P = win17[c / 2];
            float h2 = bal * H * H, p2 = P * P / bal;
            float wh = h2 / (h2 + p2 + 1e-18f), wp = 1.f - wh;
            float wn = std::pow(std::clamp(1.f - std::fabs(wh - wp), 0.f, 1.f), sep);
            float m0 = wh * (1.f - wn), m1 = wp * (1.f - wn), m2 = wn;
            // a little smoothing from frame to frame keeps it from warbling
            mask[0][k] += 0.55f * (m0 - mask[0][k]);
            mask[1][k] += 0.55f * (m1 - mask[1][k]);
            mask[2][k] = 1.f - mask[0][k] - mask[1][k];
        }
    }
    void frame()
    {
        for (int n = 0; n < N; ++n)
        {
            int i = (w + n) % N; // oldest first
            re[n] = inL[i] * win[n], im[n] = inR[i] * win[n];
        }
        fft(re, im, false);
        // each channel's magnitude from the packed spectrum
        for (int k = 0; k < bins; ++k)
        {
            int nk = (N - k) % N;
            float lr = 0.5f * (re[k] + re[nk]), li = 0.5f * (im[k] - im[nk]);
            float rr = 0.5f * (im[k] + im[nk]), ri = -0.5f * (re[k] - re[nk]);
            mag[k] = std::sqrt(lr * lr + li * li) + std::sqrt(rr * rr + ri * ri);
        }
        if (by == 1)
        {
            std::memcpy(hist[histPos], mag, sizeof(float) * (size_t)bins);
            histPos = (histPos + 1) % histT;
            frames = std::min(frames + 1, histT);
        }
        makeMasks();
        const float norm = 1.f / (1.5f * (float)N); // Hann in and out at 75% overlap sums to 1.5
        for (int b = 0; b < 3; ++b)
        {
            for (int k = 0; k < N; ++k)
            {
                int kk = k <= N / 2 ? k : N - k;
                yr[k] = re[k] * mask[b][kk], yi[k] = im[k] * mask[b][kk];
            }
            fft(yr, yi, true);
            for (int n = 0; n < N; ++n)
            {
                acc[b][0][n] += yr[n] * win[n] * norm;
                acc[b][1][n] += yi[n] * win[n] * norm;
            }
            for (int c = 0; c < 2; ++c)
            {
                std::memcpy(ready[b][c], acc[b][c], sizeof(float) * (size_t)hop);
                std::memmove(acc[b][c], acc[b][c] + hop, sizeof(float) * (size_t)(N - hop));
                std::memset(acc[b][c] + N - hop, 0, sizeof(float) * (size_t)hop);
            }
        }
        rd = 0;
    }
    // one sample in; the three bands and the dry signal out, N samples late
    inline void tick(float l, float r, float dryInL, float dryInR, float bl[3], float br[3], float &dl, float &dr)
    {
        for (int b = 0; b < 3; ++b)
            bl[b] = ready[b][0][rd], br[b] = ready[b][1][rd];
        dl = dryL[w], dr = dryR[w];
        dryL[w] = dryInL, dryR[w] = dryInR;
        inL[w] = l, inR[w] = r;
        w = (w + 1) % N;
        ++rd;
        if (++filled >= hop)
        {
            filled = 0;
            frame();
        }
    }
};

// ---------------------------------------------------------------------------
// 3-band Linkwitz-Riley (24 dB/oct) crossover that sums back flat.
// TPT state-variable sections (Butterworth Q), safe to sweep.
//   low  = AP(f2) . LP4(f1)
//   mid  = LP4(f2) . HP4(f1)
//   high = HP4(f2) . HP4(f1)
//   low + mid + high = AP(f2) . AP(f1): flat magnitude. dry() gives that same phase to the
//   dry signal so Mix never combs.
// ---------------------------------------------------------------------------
struct Crossover
{
    struct SVF
    {
        static constexpr double k{1.4142135623730951};
        double g{0}, a1{0}, a2{0}, a3{0};
        double ic1[2]{}, ic2[2]{};
        void set(double fc, double sr)
        {
            fc = std::clamp(fc, 10.0, sr * 0.45);
            g = std::tan(3.14159265358979323846 * fc / sr);
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
        void coefsFrom(const SVF &o) { g = o.g, a1 = o.a1, a2 = o.a2, a3 = o.a3; }
        inline void tick(int ch, double x, double &lp, double &bp, double &hp)
        {
            double v3 = x + 1.0e-20 - ic2[ch]; // the tiny offset keeps the states out of denormals
            double v1 = a1 * ic1[ch] + a2 * v3;
            double v2 = ic2[ch] + a2 * ic1[ch] + a3 * v3;
            ic1[ch] = 2.0 * v1 - ic1[ch];
            ic2[ch] = 2.0 * v2 - ic2[ch];
            lp = v2;
            bp = v1;
            hp = x - k * v1 - v2;
        }
        inline double allpass(int ch, double x)
        {
            double lp, bp, hp;
            tick(ch, x, lp, bp, hp);
            return x - 2.0 * k * bp;
        }
        void reset() { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0; }
    };
    SVF s1, s1lp, s1hp, s2, s2lp, s2hp, apLow, apDry1, apDry2;

    void set(double f1, double f2, double sr)
    {
        s1.set(f1, sr);
        s1lp.coefsFrom(s1);
        s1hp.coefsFrom(s1);
        apDry1.coefsFrom(s1);
        s2.set(f2, sr);
        s2lp.coefsFrom(s2);
        s2hp.coefsFrom(s2);
        apLow.coefsFrom(s2);
        apDry2.coefsFrom(s2);
    }
    inline void split(int ch, double x, double &low, double &mid, double &high)
    {
        double lp, bp, hp, t, l4, h4;
        s1.tick(ch, x, lp, bp, hp);
        s1lp.tick(ch, lp, l4, bp, t);
        s1hp.tick(ch, hp, t, bp, h4);
        double lp2, hp2;
        s2.tick(ch, h4, lp2, bp, hp2);
        s2lp.tick(ch, lp2, mid, bp, t);
        s2hp.tick(ch, hp2, t, bp, high);
        low = apLow.allpass(ch, l4);
    }
    inline double dry(int ch, double x) { return apDry2.allpass(ch, apDry1.allpass(ch, x)); }
    void reset()
    {
        for (auto *s : {&s1, &s1lp, &s1hp, &s2, &s2lp, &s2hp, &apLow, &apDry1, &apDry2})
            s->reset();
    }
};

// ---------------------------------------------------------------------------
// Resonance guard. Compares a level with a reference
// level (held ~1 s). When the output rises more than `boost` above the reference, or keeps
// ringing with nothing coming in, it is turned down fast (~2 ms) and let back up slowly
// (~150 ms). Anything under the floor (-18 dBFS RMS) is left alone.
// ---------------------------------------------------------------------------
struct Guard
{
    static constexpr float floorRms{0.126f};
    static constexpr float kneeDb{6.f};
    float ref2{0}, out2{0}, gain{1}, gmin{1};
    float refAtt{0}, refRel{0}, outC{0}, down{0}, up{0};
    void setRate(double sr)
    {
        auto c = [sr](float ms) { return 1.f - std::exp(-1.f / (ms * 0.001f * (float)sr)); };
        refAtt = c(10.f);
        refRel = c(1000.f);
        outC = c(30.f);
        down = c(2.f);
        up = c(150.f);
    }
    void reset() { ref2 = out2 = 0.f, gain = 1.f; }
    inline void process(float refMS, float boost, float &l, float &r)
    {
        ref2 += (refMS > ref2 ? refAtt : refRel) * (refMS - ref2);
        float o = 0.5f * (l * l + r * r);
        out2 += outC * (o - out2);
        if (!std::isfinite(ref2) || !std::isfinite(out2))
            ref2 = out2 = 0.f;
        if (ref2 < 1.0e-12f)
            ref2 = 0.f;
        if (out2 < 1.0e-12f)
            out2 = 0.f;
        float allowed = std::max(std::sqrt(ref2) * boost, floorRms);
        float outLv = std::sqrt(out2);
        float target = 1.f;
        if (outLv > allowed * 0.7079f) // only do the dB maths inside or above the knee
        {
            float over = 20.f * std::log10(outLv / allowed);
            float red = 0.f;
            if (over > -0.5f * kneeDb && over < 0.5f * kneeDb)
                red = (over + 0.5f * kneeDb) * (over + 0.5f * kneeDb) / (2.f * kneeDb);
            else if (over >= 0.5f * kneeDb)
                red = over;
            if (red > 0.f)
                target = std::pow(10.f, -red / 20.f);
        }
        gain += (target < gain ? down : up) * (target - gain);
        gmin = std::min(gmin, gain);
        l *= gain;
        r *= gain;
    }
};

// ---------------------------------------------------------------------------
// 2x up / down sampler: a polyphase half-band IIR (two chains of six all-pass sections,
// about 100 dB of rejection, pass band flat to 0.49 of the lower Nyquist).
//
// Why not sst-filters' HalfRateFilter: its per-sample down-sampler
// (process_sample_D2) returns one all-pass chain only, so it never low-passes before dropping
// samples and the aliasing comes straight back. Measured with a fuzzed 5 kHz sine, the
// loudest alias sat at -17 dB with oversampling off, 2x and 4x alike. This one is tested in
// tests/tc_tests.cpp ("oversampling cuts aliasing").
// ---------------------------------------------------------------------------
struct HalfBand
{
    static constexpr int M{6};
    static constexpr float ca[M]{0.036681502163648017f, 0.2746317593794541f, 0.56109896978791948f,
                                 0.769741833862266f,    0.8922608180038789f, 0.962094548378084f};
    static constexpr float cb[M]{0.13654762463195771f, 0.42313861743656667f, 0.6775400499741616f,
                                 0.839889624849638f,   0.9315419599631839f,  0.9878163707328971f};
    struct AP
    {
        float x1{0}, y1{0};
        inline float tick(float x, float a)
        {
            float y = x1 + a * (x - y1);
            x1 = x;
            y1 = y;
            return y;
        }
    };
    AP a[2][M], b[2][M];
    float old[2]{};
    // one sample in, two out (at twice the rate)
    inline void up(int ch, float x, float out[2])
    {
        float u = x + 1.0e-18f, v = u; // the tiny offset keeps the states out of denormals
        for (int j = 0; j < M; ++j)
        {
            u = a[ch][j].tick(u, ca[j]);
            v = b[ch][j].tick(v, cb[j]);
        }
        out[0] = u;
        out[1] = v;
    }
    // two samples in (at twice the rate), one out
    inline float down(int ch, float x0, float x1)
    {
        float u = x0 + 1.0e-18f, v = x1 + 1.0e-18f;
        for (int j = 0; j < M; ++j)
        {
            u = a[ch][j].tick(u, ca[j]);
            v = b[ch][j].tick(v, cb[j]);
        }
        float y = 0.5f * (u + old[ch]);
        old[ch] = v;
        return y;
    }
    void reset() { *this = HalfBand{}; }
};

// Feedback loop conditioning: the fed-back signal is low-passed,
// DC-blocked and soft-saturated, and the loop gain tops out at 0.2, so it warms up and never
// runs away.
struct FbConditioner
{
    float lp{0}, x1{0}, y1{0};
    float process(float x, float lpCoef)
    {
        lp += lpCoef * (x - lp);
        float y = lp - x1 + 0.995f * y1;
        x1 = lp;
        y1 = std::fabs(y) < 1.0e-20f ? 0.f : y;
        return y;
    }
    void reset() { lp = x1 = y1 = 0; }
};

// Deterministic random numbers: the same seed and the same count always give the same value,
// so a mutated arrangement plays back (and bounces) the same every time.
static inline uint64_t mix64(uint64_t z)
{
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static inline float rand01(uint64_t a, uint64_t b, uint64_t c)
{
    return (float)((mix64(a * 0x100000001B3ull ^ mix64(b * 31 + c)) >> 40) / 16777216.0);
}

// A value that glides in a straight line to its target over one control block.
struct Ramp
{
    float v{0}, d{0};
    void to(float target, int steps) { d = (target - v) / (float)steps; }
    void snap(float x) { v = x, d = 0; }
    inline float tick()
    {
        float r = v;
        v += d;
        return r;
    }
};

// the notes of each scale (semitones above the root); Tune's scale list, in the order the window shows it
// First the root with one interval (it rings in every octave of the range), then two chords, then the scales.
// "Down" means the interval below the root: a fifth down from A is D (the same notes as a fourth up).
static constexpr int numTuneScales = 17;
inline const std::vector<int> &tuneScaleNotes(int scale)
{
    static const std::vector<int> sc[numTuneScales] = {
        {0},          // Octaves
        {0, 7},       // Fifths up
        {0, 5},       // Fifths down
        {0, 4},       // Major 3rds up
        {0, 8},       // Major 3rds down
        {0, 3},       // Minor 3rds up
        {0, 9},       // Minor 3rds down
        {0, 4, 7},    // Major chord
        {0, 3, 7},    // Minor chord
        {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10}, {0, 2, 4, 5, 7, 9, 10},
        {0, 3, 5, 7, 10}, {0, 3, 5, 6, 7, 10}, {0, 2, 4, 6, 8, 10}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
    return sc[std::clamp(scale, 0, numTuneScales - 1)];
}
// ----- Tune: up to 85 resonators (Chromatic, C1..C8), each a band-pass state-variable filter (stable when moved)
class TuneBank
{
  public:
    static constexpr int maxNotes = 85;
    void setRate(double sr)
    {
        fs = sr;
        envA = (float)(1.0 - std::exp(-1.0 / (0.3 * sr))); // level followers: about 0.3 s
        reset();
    }
    void reset()
    {
        for (auto &r : res)
            r = {};
        envIn = envOut = 0.f;
        lpL = lpR = 0.f;
        configured = false;
    }
    // control rate: recompute the notes and their coefficients when the settings change
    void configure(const TuneSettings &t, float ringMs, float tilt)
    {
        const auto &notes = tuneScaleNotes(t.scale);
        int lo = std::clamp(std::min(t.lowest, t.highest), 24, 108), hi = std::clamp(std::max(t.lowest, t.highest), 24, 108);
        bool same = configured && lo == cLo && hi == cHi && t.root == cRoot && t.scale == cScale && std::fabs(ringMs - cRing) < 0.5f &&
                    std::fabs(t.detuneCents - cDet) < 0.01f && std::fabs(tilt - cTilt) < 0.001f && std::fabs(t.width - cWidth) < 0.001f;
        if (same)
            return;
        int n = 0;
        for (int m = lo; m <= hi && n < maxNotes; ++m)
        {
            int pc = ((m - t.root) % 12 + 12) % 12;
            bool in = false;
            for (int nk : notes)
                in = in || nk == pc;
            if (!in)
                continue;
            float det = t.detuneCents * (rand01(31, (uint64_t)m, 7) * 2.f - 1.f);
            double f = 440.0 * std::pow(2.0, (m - 69 + det / 100.0) / 12.0);
            f = std::min(f, 0.45 * fs);
            // ring time -> bandwidth: a resonator of bandwidth B (Hz) decays with time constant 1/(pi B)
            double bw = 1.0 / (3.14159265358979 * std::max(0.02, ringMs / 1000.0));
            double q = std::clamp(f / (bw * 1.554), 2.0, 3000.0); // 1.554: two stages in a row ring about as long as one of bandwidth bw
            double g = std::tan(3.14159265358979 * f / fs), k = 1.0 / q;
            auto &R = res[n];
            if (!R.used || std::fabs(R.f - (float)f) > 0.01f)
                for (int c = 0; c < 4; ++c)
                    R.ic1[c] = R.ic2[c] = 0.f; // a new note starts quiet
            R.used = true, R.f = (float)f, R.midi = m;
            R.k = (float)k;
            R.a1 = (float)(1.0 / (1.0 + g * (g + k))), R.a2 = (float)(g * R.a1), R.a3 = (float)(g * R.a2);
            float pos = hi > lo ? (float)(m - lo) / (float)(hi - lo) : 0.5f;
            R.tiltGain = std::pow(10.f, tilt * (pos - 0.5f) * 24.f / 20.f); // +-12 dB across the range
            float side = (n & 1) ? 1.f : -1.f, pan = 0.5f + 0.5f * side * std::clamp(t.width, 0.f, 1.f);
            R.gl = std::sqrt(1.f - pan), R.gr = std::sqrt(pan);
            ++n;
        }
        for (int i = n; i < maxNotes; ++i)
            res[i].used = false;
        count = n;
        // longer rings pass less of a full mix, so they may be lifted more: +18 dB at 400 ms, up to +30 dB at 4 s
        capGain = std::min(32.f, 8.f * std::sqrt(std::max(1.f, ringMs / 400.f)));
        configured = true, cLo = lo, cHi = hi, cRoot = t.root, cScale = t.scale, cRing = ringMs, cDet = t.detuneCents, cTilt = tilt, cWidth = t.width;
    }
    // control rate: the motion through the notes (a swell that travels, or a slow random drift).
    // Free: pos runs at hz. Synced: pos is the song position in divisions, so a bar always looks the same.
    // Wave: one sweep through the notes per cycle. Drift: a new level every quarter cycle (free)
    // or every division (synced).
    void moveControl(int motion, float hz, int samples, bool sync, double syncPos)
    {
        if (sync)
            pos = std::max(0.0, syncPos);
        else
        {
            pos += hz * samples / fs;
            if (pos >= 1048576.0)
                pos -= 1048576.0;
        }
        const double ph = pos - std::floor(pos), q = sync ? pos : pos * 4.0;
        for (int i = 0; i < count; ++i)
        {
            float m = 1.f;
            if (motion == 1)
                m = 0.35f + 0.65f * (0.5f + 0.5f * (float)std::cos(6.283185307 * (ph - (double)i / std::max(1, count) * 1.5)));
            else if (motion == 2)
            {
                long t = (long)std::floor(q);
                float u = (float)(q - (double)t), a = rand01(53, (uint64_t)t, (uint64_t)i), b = rand01(53, (uint64_t)(t + 1), (uint64_t)i);
                u = u * u * (3.f - 2.f * u);
                m = 0.25f + 0.75f * (a + (b - a) * u);
            }
            res[i].motion = m;
        }
    }
    double motionPos() const { return pos; }
    // control rate: Slope, a tilt EQ on the ringing. A one-pole split at the pivot; the lows go down
    // as the highs go up (up to 9 dB each way), so the ringing gets darker or brighter, not louder.
    void setTone(float slope, float pivotHz)
    {
        float sl = std::clamp(slope, -1.f, 1.f), f = std::clamp(pivotHz, 40.f, (float)(0.4 * fs));
        toneA = 1.f - std::exp(-6.2831853f * f / (float)fs);
        tLo = std::pow(10.f, -9.f * sl / 20.f), tHi = std::pow(10.f, 9.f * sl / 20.f);
        toneOn = std::fabs(sl) > 1.0e-4f || std::fabs(gLo - 1.f) > 1.0e-4f || std::fabs(gHi - 1.f) > 1.0e-4f;
    }
    // audio rate: the ringing of one stereo sample, level-matched to what came in
    inline void tick(float &l, float &r, float amount, bool layer)
    {
        float sl = 0.f, sr = 0.f;
        const float in[2] = {l, r};
        for (int i = 0; i < count; ++i)
        {
            auto &R = res[i];
            float o[2];
            for (int c = 0; c < 2; ++c)
            {
                // two band-passes in a row (0 dB at the note): steep sides, so what is out of key stays out
                float x = in[c];
                for (int st = 0; st < 2; ++st)
                {
                    float &c1 = R.ic1[st * 2 + c], &c2 = R.ic2[st * 2 + c];
                    float v3 = x - c2, v1 = R.a1 * c1 + R.a2 * v3, v2 = c2 + R.a2 * c1 + R.a3 * v3;
                    c1 = 2.f * v1 - c1, c2 = 2.f * v2 - c2;
                    x = R.k * v1;
                }
                o[c] = x;
            }
            float w = R.tiltGain * R.motion;
            sl += (o[0] * R.gl + o[1] * R.gl) * w, sr += (o[0] * R.gr + o[1] * R.gr) * w;
        }
        if (toneOn)
        {
            gLo += 0.002f * (tLo - gLo), gHi += 0.002f * (tHi - gHi); // smoothed, so moving Slope doesn't click
            lpL += toneA * (sl - lpL), lpR += toneA * (sr - lpR);
            sl = lpL * gLo + (sl - lpL) * gHi, sr = lpR * gLo + (sr - lpR) * gHi;
        }
        // keep the ringing at about the level of the input (slow followers, so notes can still swell and fade)
        envIn += envA * (0.5f * (l * l + r * r) - envIn);
        envOut += envA * (0.5f * (sl * sl + sr * sr) - envOut);
        float gmatch = std::clamp(std::sqrt((envIn + 1.0e-12f) / (envOut + 1.0e-12f)), 0.f, capGain); // capped, so what is out of key stays quieter
        float a = std::clamp(amount, 0.f, 1.f);
        const float dry = layer ? 1.f : 1.f - a;
        l = l * dry + sl * gmatch * a;
        r = r * dry + sr * gmatch * a;
        if (!std::isfinite(l) || !std::isfinite(r))
        {
            reset();
            l = r = 0.f;
        }
    }
    int notes() const { return count; }
    int noteMidi(int i) const { return res[i].midi; }
    float noteLevel(int i) const { return res[i].tiltGain * res[i].motion; }

  private:
    struct Res
    {
        bool used{false};
        int midi{0};
        float f{0}, k{1}, a1{0}, a2{0}, a3{0};
        float ic1[4]{}, ic2[4]{}; // [stage * 2 + channel]
        float tiltGain{1}, motion{1}, gl{0.7f}, gr{0.7f};
    };
    Res res[maxNotes];
    int count{0};
    double fs{48000}, pos{0};
    float toneA{0.1f}, tLo{1}, tHi{1}, gLo{1}, gHi{1}, lpL{0}, lpR{0};
    bool toneOn{false};
    float envIn{0}, envOut{0}, envA{0.0004f}, capGain{8.f};
    bool configured{false};
    int cLo{0}, cHi{0}, cRoot{-1}, cScale{-1};
    float cRing{0}, cDet{0}, cTilt{0}, cWidth{0};
};

class Engine
{
  public:
    ~Engine() { delete spec; }
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    Engine()
    {
        (void)filterCatalogue(); // build the catalogue off the audio thread
        std::memset(combDelays, 0, sizeof(combDelays));
        for (int sh = 0; sh < numShapes; ++sh)
            shapeTable(sh, shapeTables[sh]);
        for (int i = 0; i < numLfos; ++i)
        {
            shapeTable(ShapeCustom, customTable[i][0]);
            shapeTable(ShapeCustom, customTable[i][1]);
            curveActive[i].store(0);
        }
    }

    // Not real-time: call from prepare().
    void setSampleRate(double sr)
    {
        sampleRate = sr;
        knobAlpha = (float)(1.0 - std::exp(-(double)blockSize / (0.005 * sampleRate)));
        limRelease = (float)(1.0 - std::exp(-1.0 / (0.08 * sampleRate)));
        fadeBlocks = std::max(4, (int)std::ceil(0.006 * sampleRate / blockSize));
        finalGuard.setRate(sampleRate);
        tune.setRate(sampleRate);
        configureRates();
        reset();
    }

    // Clear all audio state.
    void reset()
    {
        tune.reset();
        needsSnapBank = true;
        for (auto &b : bs)
            b.ripPh = 0, b.bkLo = -1e9f, b.bkOn = false;
        for (auto &h : hrUp)
            h.reset();
        for (auto &h : hrDn)
            h.reset();
        for (int i = 0; i < numBands; ++i)
            setupFilter(i);
        xover.reset();
        spec->reset();
        finalGuard.reset();
        for (auto &b : bs)
        {
            b.guard.reset();
            b.dcX[0] = b.dcX[1] = b.dcY[0] = b.dcY[1] = 0;
            for (int c = 0; c < 2; ++c)
                for (int kb = 0; kb < numBankBands; ++kb)
                    b.bkS1[c][kb] = b.bkS2[c][kb] = 0.f;
        }
        limGain = 1.f;
        ctr = 0;
        needsSnap = true;
        clearAudioState();
        resetModulation();
        fadeState = FadeIdle;
        fade.snap(1.f);
        curRouting = -1;
    }

    // A drawn curve for an LFO (main thread). Takes effect at the next control block.
    void setCustomCurve(int lfo, const CurvePoint *p, int n)
    {
        if (lfo < 0 || lfo >= numLfos)
            return;
        int back = 1 - curveActive[lfo].load(std::memory_order_acquire);
        renderCurve(p, std::clamp(n, 0, maxCurvePoints), customTable[lfo][back]);
        curveActive[lfo].store(back, std::memory_order_release);
    }

    // In-place safe. scL / scR: the sidechain, or null.
    void process(const float *inL, const float *inR, float *outL, float *outR, int n,
                 const Settings &s, const float *scL = nullptr, const float *scR = nullptr)
    {
        for (int i = 0; i < n; ++i)
        {
            if (ctr == 0)
                processControl(s);
            float l = inL[i], r = inR[i], ol, orr;
            if (!std::isfinite(l) || !std::isfinite(r))
                l = r = 0.f;
            inPeak = std::max(inPeak, std::max(std::fabs(l), std::fabs(r)));
            if (scL)
            {
                // sidechain: high-pass then low-pass, one pole each, then its peak
                float x = 0.5f * (scL[i] + (scR ? scR[i] : scL[i]));
                if (!std::isfinite(x))
                    x = 0.f;
                x *= scGain;
                scHp = scHpA * (scHp + x - scHpX);
                scHpX = x;
                scLp += scLpA * (scHp - scLp);
                if (std::fabs(scHp) < 1.0e-20f)
                    scHp = 0.f;
                if (std::fabs(scLp) < 1.0e-20f)
                    scLp = 0.f;
                scPeak = std::max(scPeak, std::fabs(scLp));
            }
            if (tuneOn && tuneBefore)
                tune.tick(l, r, tuneAmt, tuneLayer);
            tickOS(osStages, l, r, ol, orr);
            if (tuneOn && !tuneBefore)
                tune.tick(ol, orr, tuneAmt, tuneLayer);
            // Safety net: anything non-finite means silence for the rest of this control
            // block and a clean rebuild at the next one.
            if (!std::isfinite(ol) || !std::isfinite(orr) || recovering)
            {
                ol = orr = 0.f;
                recovering = true;
            }
            ol = std::clamp(ol, -4.f, 4.f);
            orr = std::clamp(orr, -4.f, 4.f);
            if (s.limiter)
            {
                finalGuard.process(0.5f * (l * l + r * r) * finalRefScale2 + finalNoise2, 4.0f, ol, orr);
                limit(ol, orr);
            }
            if (std::fabs(ol) < 1.0e-20f)
                ol = 0.f;
            if (std::fabs(orr) < 1.0e-20f)
                orr = 0.f;
            outL[i] = ol;
            outR[i] = orr;
            ctr = (ctr + 1) % blockSize;
        }
    }

    // Call at the start of every host block with that block's transport.
    void setHostTime(const HostTime &t)
    {
        bpm = (t.bpm > 1 && t.bpm < 1000) ? t.bpm : 120.0;
        beatsPerBar = 4.0 * std::max(1, t.num) / std::max(1, t.den);
        if (t.playing && t.beatsValid && std::isfinite(t.beats))
            beats = t.beats; // anchored to the song while playing; free-runs at tempo otherwise
        playing = t.playing;
    }

    struct MutEvent
    {
        int dir{0}, low{0}, high{0}; // +1 up, -1 down, 0 none
    };

    // ----- read-only info for the screen -------------------------------------------------
    float swayValue(int b) const { return swOut[b]; }            // -1..1 before depth
    float conductorPassed(int b) const { return condCut[b]; }    // semitones passed on from Mid
    int chaperoneSteps() const { return chapSteps; }             // counts each step it passes on (for the window)
    float mutationOffset(int r) const { return respVal[r]; }     // -1..1, [0] Low, [1] High
    const MutResponse &mutationAnswer(int r) const { return effResp[r]; } // after Mutate
    MutEvent mutationHistory(int ago) const { return hist[(histPos + 15 - std::clamp(ago, 0, 15)) % 16]; }
    unsigned mutationEvents() const { return eventCount; }
    long mutationPeriod() const { return mutK; }          // how many times Mutate has changed the answers (at this song position)
    float mutationPeriodFrac() const { return mutFrac; }  // 0..1 through the current period
    // phase 3
    int laneStep(int l) const { return laneIdx[l]; }      // the step sounding now
    int lockStep(int l) const { return lockIdx[l]; }      // the lock lane's step now
    float laneValue(int l) const { return laneOut[l]; }   // -1..1 after Euclid and smoothing (before depth)
    float lfoValue(int i) const { return lfoOut[i]; }     // -1..1 (or 0..1 unipolar)
    float lfoPhase(int i) const { return (float)lfoPh[i]; }
    float envValue() const { return efOut; }               // 0..1
    float sidechainLevel() const { return scShow; }        // 0..1
    float targetOffset(int t) const { return (t > 0 && t < numModTargetIds) ? tgtOff[t] : 0.f; }
    float displayBand(int b, int param) const { return dispBand[b][param]; } // modulated knob values
    float displayGlobal(int t) const { return (t >= TgtSplitLo && t < numModTargetIds) ? dispGlobal[t - TgtSplitLo] : 0.f; }
    // Tune, for the window: which notes ring and how strongly
    int tuneNotes() const { return tuneOn ? tune.notes() : 0; }
    int tuneNoteMidi(int i) const { return tune.noteMidi(i); }
    double tuneMotionPos() const { return tune.motionPos(); }
    float tuneNoteLevel(int i) const { return tune.noteLevel(i); }
    float feedbackReturn() const { return std::max(std::fabs(fbL), std::fabs(fbR)); } // what goes back round now
    float displayCutoff(int b) const { return dispCo[b]; } // after modulation and smart limiting
    float bankGainNow(int b, int k) const { return dispBankGain[b][k]; }
    float bankHz(int b, int k) const { return dispBankHz[b][k]; }
    // copies the latest n analyser samples of one signal (0..2 strips, 3 output), oldest first
    void readAnalyser(int which, float *dst, int n) const
    {
        uint32_t w = anaWrite.load(std::memory_order_acquire);
        n = std::min(n, anaSize);
        for (int i = 0; i < n; ++i)
            dst[i] = ana[which][(w - (uint32_t)n + (uint32_t)i) & (anaSize - 1)].load(std::memory_order_relaxed);
    }
    float displayReso(int b) const { return dispRe[b]; }
    float edgePress(int b) const { return press[b]; } // 0..1: how hard the knob leans on its limit
    float bandGuardGain(int b) const { return bs[b].guard.gain; }
    float outputGuardGain() const { return finalGuard.gain; }
    float limiterGain() const { return limGain; }
    float xoverLoHz() const { return (float)curF1; }
    float xoverHiHz() const { return (float)curF2; }
    int activeRouting() const { return curRouting; }
    // samples of delay at the host rate (the spectral split's frame), for the host to compensate
    int latencySamples() const { return spectralOn ? spec->hostN : 0; }
    bool spectralActive() const { return spectralOn; }
    PassKind bandKind(int b) const { return kind[b]; }

    static constexpr float limCeiling{0.8912509f}; // -1 dBFS
    static constexpr float edgeKnee{6.f};          // semitones
    static constexpr float seriesGap{12.f};        // semitones kept open between HP and LP

  private:
    // ----- configuration -----------------------------------------------------------------
    double sampleRate{48000}, innerRate{96000};
    double maxCutoff{67};
    int osStages{1}, nInner{16};
    float knobAlpha{0.04f};
    int ctr{0};
    bool needsSnap{true}, recovering{false};

    int curRouting{-1};
    enum FadeState { FadeIdle, FadeOut, FadeIn };
    int fadeState{FadeIdle}, fadeCount{0}, fadeBlocks{36};
    Ramp fade;

    std::array<sfpp::Filter, numBands> filters;
    int curModel[numBands]{-1, -1, -1}, curVariant[numBands]{-1, -1, -1};
    bool curOn[numBands]{true, true, true};
    PassKind kind[numBands]{PassKind::Open, PassKind::Open, PassKind::Open};
    // Z-Plane sections (transposed direct form II), per strip
    struct ZSec
    {
        float b0{1}, b1{0}, b2{0}, a1{0}, a2{0};
        float z1[2]{}, z2[2]{};
    };
    ZSec zsec[numBands][zStages];
    bool zOn[numBands]{};
    int zDesign[numBands]{};
    Ramp zComp[numBands];
    bool zSnap[numBands]{true, true, true};
    struct ZBiquad
    {
        double b0, b1, b2, a1, a2; // normalised (a0 = 1)
    };
    // RBJ peaking EQ, or the low-pass section (q > 0, gain ignored)
    ZBiquad zPeak(double f, double gDb, double q) const
    {
        double A = std::pow(10.0, gDb / 40.0), w0 = 2.0 * 3.14159265358979 * f / innerRate, al = std::sin(w0) / (2.0 * q), cs = std::cos(w0);
        double a0 = 1.0 + al / A;
        return {(1.0 + al * A) / a0, -2.0 * cs / a0, (1.0 - al * A) / a0, -2.0 * cs / a0, (1.0 - al / A) / a0};
    }
    ZBiquad zLow(double f, double q) const
    {
        double w0 = 2.0 * 3.14159265358979 * f / innerRate, al = std::sin(w0) / (2.0 * q), cs = std::cos(w0), a0 = 1.0 + al;
        return {(1.0 - cs) / 2.0 / a0, (1.0 - cs) / a0, (1.0 - cs) / 2.0 / a0, -2.0 * cs / a0, (1.0 - al) / a0};
    }
    // a second-order polynomial 1 + c1 z^-1 + c2 z^-2 moved between two frames: when both have a
    // complex pair of roots, their radius and angle are interpolated (the root glides across the
    // z-plane); otherwise the coefficients are (stable for poles: the stability region is convex)
    static void zMove(double c1a, double c2a, double c1b, double c2b, double t, double &c1, double &c2)
    {
        bool ca = c1a * c1a < 4.0 * c2a && c2a > 0, cb = c1b * c1b < 4.0 * c2b && c2b > 0;
        if (ca && cb)
        {
            double ra = std::sqrt(c2a), rb = std::sqrt(c2b);
            double tha = std::acos(std::clamp(-c1a / (2.0 * ra), -1.0, 1.0)), thb = std::acos(std::clamp(-c1b / (2.0 * rb), -1.0, 1.0));
            double r = ra + t * (rb - ra), th = tha + t * (thb - tha);
            c1 = -2.0 * r * std::cos(th), c2 = r * r;
        }
        else
            c1 = c1a + t * (c1b - c1a), c2 = c2a + t * (c2b - c2a);
    }
    void zCoefficients(int b, float cutoffSemi, float reso, float morph)
    {
        int n = 0;
        const ZDesign *d = zDesigns(n);
        const ZDesign &D = d[std::clamp(zDesign[b], 0, n - 1)];
        float t = std::clamp(morph, 0.f, 1.f);
        // a three-frame shape: the first half of Morph goes A -> M, the second half M -> B
        const ZFrame *fa = &D.a, *fb = &D.b;
        if (D.mid)
        {
            if (t <= 0.5f)
                fb = &D.m, t = 2.f * t;
            else
                fa = &D.m, t = 2.f * (t - 0.5f);
        }
        float shift = std::exp2(3.f * std::tanh((cutoffSemi - hzToSemi(1000.0)) / 36.f)); // Frequency Tracking
        float qs = 0.5f + 2.5f * std::pow(std::clamp(reso, 0.f, 1.f), 1.5f), gs = 0.6f + 0.8f * std::clamp(reso, 0.f, 1.f); // Transform 2
        const double fMax = 0.45 * innerRate;
        auto frameStage = [&](const ZFrame &F, int k, float &peakDb) -> ZBiquad
        {
            if (k == 0)
            {
                if (F.lpF <= 0.f)
                    return {1, 0, 0, 0, 0};
                return zLow(std::clamp((double)F.lpF * shift, 20.0, fMax), std::max(0.5, (double)F.lpQ * (0.6 + 0.4 * qs)));
            }
            int j = k - 1;
            float g = F.g[j] * (F.g[j] > 0 ? gs : 1.f);
            peakDb = std::max(peakDb, g);
            return zPeak(std::clamp((double)F.f[j] * shift, 20.0, fMax), g, (double)F.q[j] * qs);
        };
        float peakA = 0.f, peakB = 0.f;
        for (int k = 0; k < zStages; ++k)
        {
            ZBiquad A = frameStage(*fa, k, peakA), B = frameStage(*fb, k, peakB);
            double a1, a2, n1, n2;
            zMove(A.a1, A.a2, B.a1, B.a2, t, a1, a2);                                             // the poles
            double za = std::fabs(A.b0) > 1e-12 ? A.b0 : 1e-12, zb = std::fabs(B.b0) > 1e-12 ? B.b0 : 1e-12;
            zMove(A.b1 / za, A.b2 / za, B.b1 / zb, B.b2 / zb, t, n1, n2);                         // the zeros
            double g0 = std::exp(std::log(std::fabs(za)) + t * (std::log(std::fabs(zb)) - std::log(std::fabs(za)))); // and the gain
            auto &Z = zsec[b][k];
            Z.b0 = (float)g0, Z.b1 = (float)(g0 * n1), Z.b2 = (float)(g0 * n2), Z.a1 = (float)a1, Z.a2 = (float)a2;
        }
        float maxG = peakA + t * (peakB - peakA);
        float comp = std::pow(10.f, -0.4f * maxG / 20.f); // most of the loudest peak taken back (the Filter Level)
        if (zSnap[b])
            zComp[b].snap(comp), zSnap[b] = false;
        else
            zComp[b].to(comp, nInner);
    }
    inline void zTick(int b, float &l, float &r)
    {
        float x[2] = {l, r}, c = zComp[b].tick();
        for (int k = 0; k < zStages; ++k)
        {
            auto &Z = zsec[b][k];
            for (int ch = 0; ch < 2; ++ch)
            {
                float y = Z.b0 * x[ch] + Z.z1[ch];
                Z.z1[ch] = Z.b1 * x[ch] - Z.a1 * y + Z.z2[ch];
                Z.z2[ch] = Z.b2 * x[ch] - Z.a2 * y;
                if (std::fabs(Z.z1[ch]) < 1e-20f)
                    Z.z1[ch] = 0.f;
                if (std::fabs(Z.z2[ch]) < 1e-20f)
                    Z.z2[ch] = 0.f;
                x[ch] = y;
            }
        }
        l = std::clamp(x[0] * c, -8.f, 8.f);
        r = std::clamp(x[1] * c, -8.f, 8.f);
    }
    float combDelays[numBands][4][sst::filters::utilities::MAX_FB_COMB +
                                  sst::filters::utilities::SincTable::FIRipol_N];

    HalfBand hrUp[maxOsStages], hrDn[maxOsStages];
    SpectralSplit *spec{new SpectralSplit()}; // large: kept off the stack
    int specQ{1};
    bool spectralOn{false};

    Crossover xover;
    double curF1{250}, curF2{3000};

    struct BandState
    {
        bool active{false};     // filter in use
        bool driveFirst{false}; // order
        bool bypass{false};     // Series: strip bypassed
        int dtype{Warm};
        Ramp dGain, dComp, dWet, dAmt, level;
        float refScale2{1.f};
        float dcX[2]{}, dcY[2]{};
        Guard guard;
        // the bank: TPT state-variable band-passes (two channels x eight)
        float bkA1[numBankBands]{}, bkA2[numBankBands]{}, bkA3[numBankBands]{}, bkK[numBankBands]{};
        float bkS1[2][numBankBands]{}, bkS2[2][numBankBands]{};
        Ramp bkGL[numBankBands], bkGR[numBankBands], bkWet, smix;
        bool bkOn{false};
        float bkNorm{1.f}, bkLo{-1e9f}, bkHi{-1e9f}, bkW{-1.f};
        double ripPh{0};
    } bs[numBands];
    float dispBankGain[numBands][numBankBands]{}, dispBankHz[numBands][numBankBands]{};
    bool needsSnapBank{true};
    // the analyser: the three strips and the output, at the host rate, for the window
    static constexpr int anaSize = 4096;
    std::atomic<float> ana[4][anaSize]{};
    std::atomic<uint32_t> anaWrite{0};
    int anaCount{0};
    inline void anaPush(float a, float b, float c, float o)
    {
        if (++anaCount < nInner / blockSize)
            return;
        anaCount = 0;
        uint32_t w = anaWrite.load(std::memory_order_relaxed);
        uint32_t i = w & (anaSize - 1);
        ana[0][i].store(a, std::memory_order_relaxed);
        ana[1][i].store(b, std::memory_order_relaxed);
        ana[2][i].store(c, std::memory_order_relaxed);
        ana[3][i].store(o, std::memory_order_relaxed);
        anaWrite.store(w + 1, std::memory_order_release);
    }
    float dcR{0.999f};
    float bandBoost{4.f};
    bool guardsOn{true};

    Ramp inGain, outGain, mix;
    Guard finalGuard;
    float finalRefScale2{1.f}, finalNoise2{0.f};

    // pan per strip
    Ramp panL[numBands], panR[numBands];

    // feedback and noise (per routing)
    FbConditioner fbc[2];
    float fbL{0}, fbR{0}, fbLpCoef{0.5f};
    Ramp fbWet, fbLevel, nsGain, nsColor;
    float curFw{0}, curFl{0};
    double fbInSum{0}, fbRetSum{0};
    float fbInMs{0}, fbRetMs{0}, fbCtl{1.f}, fbCtlTarget{1.f};
    uint32_t nsState{0x12345678u};
    float nsLp[2]{}, nsLpCoef{0.05f};

    // transport
    double bpm{120}, beatsPerBar{4}, beats{0};
    bool playing{false};
    double timeSec{0};

    // Sway
    double swBandPh[numBands][2]{{0.0, 0.31}, {0.57, 0.83}, {0.0, 0.83}}, swBandBase[numBands]{};
    long swBandStep[numBands]{-1, -1, -1};
    float swHold[numBands]{}, swOut[numBands]{};
    float inPeak{0}, env{0}, envSlow{0};
    bool envArmed{true};

    // Conductor
    float condCut[numBands]{};
    bool chapStepped{false};
    int chapSteps{0}, chapDir{1};

    // Mutation
    float midPeak{0}, midFast{0}, midSlow{0}, moveSlow{0};
    // phase 3
    float bandPeak[numBands]{};
    float scPeak{0}, scGain{1}, scHp{0}, scHpX{0}, scLp{0}, scHpA{1}, scLpA{1}, scShow{0};
    float efState{0}, efOut{0};
    double lanePos[numLanes]{};
    int laneIdx[numLanes]{-1, -1, -1}, lockIdx[numLanes]{-1, -1, -1};
    float laneOut[numLanes]{}, laneTarget[numLanes]{}, lockOut[numLanes]{}, lockK[numLanes][numBandParams]{};
    bool midLaneStepped{false};
    double lfoPh[numLfos]{};
    double lfoFree[numLfos]{};
    float lfoOut[numLfos]{}, lfoRaw[numLfos]{};
    float shapeTables[numShapes][curveN + 1];
    float customTable[numLfos][2][curveN + 1];
    std::atomic<int> curveActive[numLfos]{};
    bool wasPlaying{false};
    double playStart{0};
    float tgtOff[numModTargetIds]{}, tgtLock[numModTargetIds]{};
    float ripOut[numBands]{};
    float modMaster{1.f};
    float dispBand[numBands][numBandParams]{};
    float dispGlobal[numModTargetIds - TgtSplitLo]{};
    float stripDepthOff[numBands]{}, specBalOff{0}, specSharpOff{0};
    TuneBank tune;
    bool tuneOn{false}, tuneBefore{false}, tuneLayer{false};
    float tuneAmt{0};
    bool lvlArmed{true}, cutArmed{true}, guardArmed{true};
    long stepPrev{LONG_MIN};
    double lastEvent{-1.0e9};
    unsigned eventCount{0};
    struct Pending
    {
        double at{0};
        float val{0};
        bool live{false};
    } pend[2][8];
    float respKick[2]{}, respVal[2]{};
    int respTgt[2]{-1, -1};
    MutResponse effResp[2], lastParams[2];
    long mutK{-1};
    int lastSeed{-1};
    float lastMutate{-1.f}, mutFrac{0.f};
    bool wasFrozen{false};
    long frozenK{0};
    MutEvent hist[16]{};
    int histPos{0};

    // smoothed knob values
    struct Smoothed
    {
        float co[numBands], re[numBands], mo[numBands], dr[numBands], lv[numBands], pan[numBands];
        float f1, f2, inDb, outDb, mix, fb, ns, nsCol;
    } k{};

    float dispCo[numBands]{}, dispRe[numBands]{}, press[numBands]{};

    // ----- output limiter ----------------------------------------------
    float limGain{1.f}, limRelease{0.0003f};
    void limit(float &l, float &r)
    {
        float a = std::max(std::fabs(l), std::fabs(r));
        float target = a > limCeiling ? limCeiling / a : 1.f;
        if (target < limGain)
            limGain = target;
        else
            limGain += limRelease * (target - limGain);
        l *= limGain;
        r *= limGain;
        l = std::clamp(l, -limCeiling, limCeiling);
        r = std::clamp(r, -limCeiling, limCeiling);
    }

    // ----- saturators ----------------------------------------------------------------------
    static inline float warm(float x) // tanh-like, |y| <= 1
    {
        x = std::clamp(x, -3.f, 3.f);
        return x * (27.f + x * x) / (27.f + 9.f * x * x);
    }
    static inline float shape(int type, float x, float amt)
    {
        switch (type)
        {
        default:
        case Warm:
            return warm(x);
        case Push: // tube-like: the two halves of the wave saturate differently
            return (warm(x + 0.4f) - 0.382f) * 1.15f;
        case Bite: // clean until it clips, then a short knee into a flat top
        {
            float a = std::fabs(x);
            if (a <= 0.8f)
                return x;
            float y = a >= 1.2f ? 1.f : a - (a - 0.8f) * (a - 0.8f) * 1.25f;
            return x < 0 ? -y : y;
        }
        case Fuzz: // high gain, lopsided ceilings
        {
            x = std::clamp(x, -30.f, 30.f);
            return x >= 0 ? 1.f - std::exp(-x) : -0.8f * (1.f - std::exp(x * 1.25f));
        }
        case Fold: // wavefolder: past full scale the wave turns back on itself
            return std::sin(std::clamp(x, -64.f, 64.f));
        case Rectify: // blends in the flipped half of the wave: octave-up grit
            return warm((1.f - amt) * x + amt * 1.5f * std::fabs(x));
        case Crush: // bit depth: 16 bits at the bottom of the knob down to 2 at the top (smoothly in between)
        {
            float levels = std::exp2(15.f - 14.f * std::clamp(amt, 0.f, 1.f)); // steps per half wave
            return std::clamp(std::round(std::clamp(x, -1.f, 1.f) * levels) / levels, -1.f, 1.f);
        }
        }
    }
    static inline float driveGainFor(int type, float a)
    {
        float a2 = a * a;
        switch (type)
        {
        default:
        case Warm:
            return 1.f + 15.f * a2; // up to +24 dB into the saturator
        case Crush:
            return 1.f; // the knob sets the bits, not the level
        case Push:
        case Bite:
            return 1.f + 31.f * a2; // +30 dB
        case Fuzz:
            return 1.f + 99.f * a2; // +40 dB
        case Fold:
            return 1.f + 7.f * a2; // 8x: several folds
        case Rectify:
            return 1.f + 3.f * a2;
        }
    }

    // ----- setup ---------------------------------------------------------------------------
    void configureRates()
    {
        int osf = 1 << osStages;
        innerRate = sampleRate * osf;
        nInner = blockSize * osf;
        maxCutoff = 12.0 * std::log2(innerRate * 0.495 / 440.0);
        maxCutoff = std::min(maxCutoff, 12.0 * std::log2(sampleRate * 0.495 / 440.0) + 12.0);
        for (auto &b : bs)
            b.guard.setRate(innerRate);
        dcR = (float)(1.0 - 2.0 * 3.14159265358979 * 8.0 / innerRate); // 8 Hz DC blocker
        spec->setup(innerRate, osf, 1024 << std::clamp(specQ, 0, 3));
    }

    void setupFilter(int f)
    {
        std::memset(combDelays[f], 0, sizeof(combDelays[f]));
        const auto &cat = filterCatalogue();
        auto model = sfpp::FilterModel::None;
        sfpp::ModelConfig cfg{};
        int mi = curModel[f];
        kind[f] = PassKind::Open;
        bs[f].active = false;
        zOn[f] = false;
        zSnap[f] = true;
        for (auto &Z : zsec[f])
            Z.z1[0] = Z.z1[1] = Z.z2[0] = Z.z2[1] = 0.f;
        if (curOn[f] && mi > 0 && mi < (int)cat.size() && cat[(size_t)mi].zplane)
        {
            zOn[f] = true;
            zDesign[f] = std::clamp(curVariant[f], 0, (int)cat[(size_t)mi].configs.size() - 1);
            bs[f].active = true;
            mi = 0; // the sst filter itself stays empty
        }
        if (curOn[f] && mi > 0 && mi < (int)cat.size() && !cat[(size_t)mi].configs.empty())
        {
            model = cat[(size_t)mi].model;
            int vi = std::clamp(curVariant[f], 0, (int)cat[(size_t)mi].configs.size() - 1);
            cfg = cat[(size_t)mi].configs[(size_t)vi];
            kind[f] = cat[(size_t)mi].kinds[(size_t)vi];
            bs[f].active = true;
        }
        int osf = 1 << osStages;
        filters[f].setFilterModel(model);
        filters[f].setModelConfiguration(cfg);
        filters[f].setStereo();
        filters[f].setSampleRateAndBlockSize(osf * sampleRate, osf * blockSize);
        for (int i = 0; i < 4; ++i)
            filters[f].provideDelayLine(i, combDelays[f][i]);
        (void)filters[f].prepareInstance();
        filters[f].reset();
    }

    void clearAudioState()
    {
        for (auto &h : hrUp)
            h.reset();
        for (auto &h : hrDn)
            h.reset();
        xover.reset();
        for (auto &b : bs)
        {
            b.guard.reset();
            b.dcX[0] = b.dcX[1] = b.dcY[0] = b.dcY[1] = 0;
            for (int c = 0; c < 2; ++c)
                for (int kb = 0; kb < numBankBands; ++kb)
                    b.bkS1[c][kb] = b.bkS2[c][kb] = 0.f;
        }
        finalGuard.reset();
        for (auto &c : fbc)
            c.reset();
        fbL = fbR = 0.f;
        fbInSum = fbRetSum = 0.0;
        fbInMs = fbRetMs = 0.f;
        fbCtl = fbCtlTarget = 1.f;
        nsLp[0] = nsLp[1] = 0.f;
    }

    void resetModulation()
    {
        nsState = 0x12345678u; // the noise restarts the same way, so bounces match
        timeSec = 0;
        for (int b = 0; b < numBands; ++b)
            bandPeak[b] = 0.f;
        scPeak = scHp = scHpX = scLp = scShow = efState = efOut = 0.f;
        for (int l = 0; l < numLanes; ++l)
        {
            lanePos[l] = 0, laneIdx[l] = lockIdx[l] = -1, laneOut[l] = laneTarget[l] = lockOut[l] = 0.f;
            for (auto &x : lockK[l])
                x = 0.f;
        }
        midLaneStepped = false;
        for (int i = 0; i < numLfos; ++i)
            lfoPh[i] = lfoFree[i] = 0, lfoOut[i] = lfoRaw[i] = 0.f;
        wasPlaying = false;
        playStart = 0;
        for (auto &t : tgtOff)
            t = 0.f;
        for (auto &t : tgtLock)
            t = 0.f;
        {
            static const double start[numBands][2] = {{0.0, 0.31}, {0.57, 0.83}, {0.0, 0.83}};
            for (int b = 0; b < numBands; ++b)
                swBandPh[b][0] = start[b][0], swBandPh[b][1] = start[b][1], swBandBase[b] = 0, swBandStep[b] = -1;
        }
        for (int b = 0; b < numBands; ++b)
            swHold[b] = swOut[b] = condCut[b] = condHeld[b] = condBurst[b] = condSm[b] = 0.f;
        condTick = condBar = condFine = -1, condTw = 0.f, jmTick = LONG_MIN;
        for (int b = 0; b < numBands; ++b)
            jmT[b] = jmS[b] = 0.f;
        inPeak = env = envSlow = 0.f;
        midPeak = midFast = midSlow = moveSlow = 0.f;
        lvlArmed = cutArmed = guardArmed = envArmed = true;
        stepPrev = LONG_MIN;
        lastEvent = -1.0e9;
        eventCount = 0;
        for (auto &row : pend)
            for (auto &p : row)
                p.live = false;
        respKick[0] = respKick[1] = respVal[0] = respVal[1] = 0.f;
        respTgt[0] = respTgt[1] = -1;
        mutK = -1;
        for (auto &h : hist)
            h = MutEvent{};
        histPos = 0;
    }

    // ----- Sway, Conductor and Mutation (control rate) ------------------------------------------
    // One step of Mutate: each answer drifts a little, and may flip or change what it moves.
    // Amount and Chance wander but are pulled back toward what you set, and never fall to nothing,
    // so an answer that drifts never goes silent for good.
    static void mutateStep(MutResponse r[2], const MutResponse base[2], int seed, long i, float m)
    {
        for (int k = 0; k < 2; ++k)
        {
            auto u = [&](int n) { return rand01((uint64_t)(uint32_t)seed + 77, (uint64_t)i, (uint64_t)(k * 16 + n)); };
            r[k].amount += 0.2f * (base[k].amount - r[k].amount);
            r[k].chance += 0.2f * (base[k].chance - r[k].chance);
            r[k].amount = std::clamp(r[k].amount + (u(0) - 0.5f) * 0.6f * m, std::min(0.15f, base[k].amount), 1.f);
            r[k].chance = std::clamp(r[k].chance + (u(1) - 0.5f) * 0.4f * m, std::min(0.25f, base[k].chance), 1.f);
            if (r[k].mode != MutOff && u(2) < 0.25f * m)
                r[k].mode = r[k].mode == MutTandem ? MutContrast : MutTandem;
            if (u(3) < 0.15f * m)
                r[k].target = std::min((int)(u(4) * (float)numMutTargets), numMutTargets - 1);
            if (u(5) < 0.10f * m)
                r[k].delaySteps = std::min((int)(u(6) * 5), 4);
        }
    }

    static bool sameResp(const MutResponse &a, const MutResponse &b)
    {
        return a.mode == b.mode && a.target == b.target && a.amount == b.amount && a.chance == b.chance &&
               a.delaySteps == b.delaySteps;
    }

    static bool midLaneActive(const Settings &s)
    {
        const auto &L = s.lane[Mid];
        if (L.euclid != 0)
            return true;
        for (int i = 0; i < std::clamp(L.length, 1, numSteps); ++i)
        {
            if (std::fabs(L.step[i]) > 1.0e-4f)
                return true;
            for (int k = 0; k < numBandParams; ++k)
                if (std::isfinite(L.plock[k][i]))
                    return true;
        }
        return false;
    }

    // ----- Phase 3: lanes, P-locks, LFOs, follower, matrix (control rate) -----------------------
    void runSources(const Settings &s, double dt)
    {
        // band and input peaks for the follower (and Mutation's level trigger)
        float pkBand[numBands];
        for (int b = 0; b < numBands; ++b)
            pkBand[b] = bandPeak[b], bandPeak[b] = 0.f;
        midPeak = pkBand[Mid];
        float pkSc = scPeak;
        scPeak = 0.f;
        scShow = std::max(pkSc, scShow * std::exp(-(float)dt / 0.15f));

        bool startNow = playing && !wasPlaying;
        if (startNow)
            playStart = beats;
        wasPlaying = playing;
        for (auto &t : tgtOff)
            t = 0.f;
        for (auto &t : tgtLock)
            t = 0.f;

        // sidechain filters (one pole each) from the settings
        const auto &E = s.env;
        scGain = std::pow(10.f, std::clamp(E.scGainDb, -24.f, 24.f) / 20.f);
        scHpA = (float)std::exp(-2.0 * 3.14159265358979 * std::clamp((double)E.scHpHz, 10.0, 5000.0) / sampleRate);
        scLpA = (float)(1.0 - std::exp(-2.0 * 3.14159265358979 * std::clamp((double)E.scLpHz, 40.0, 20000.0) / sampleRate));

        // ----- sequencer lanes
        for (int l = 0; l < numLanes; ++l)
        {
            const auto &L = s.lane[l];
            int n = std::clamp(L.length, 1, numSteps);
            double stepBeats = laneRateBeats[std::clamp(L.rateIndex, 0, numLaneRates - 1)];
            double stepSec;
            double pos;
            // Sway pushes this lane ahead of and behind the grid (its own band's Sway), so the
            // steps speed up and slow down but come back to the beat
            double swayPush = std::clamp((double)L.sway, 0.0, 1.0) * 4.0 * swOut[l];
            if (L.freeRun)
            {
                double hz = std::clamp((double)L.speedHz, 0.05, 500.0);
                if (startNow)
                    lanePos[l] = 0;
                lanePos[l] += hz * dt;
                if (lanePos[l] > 1.0e7)
                    lanePos[l] = std::fmod(lanePos[l], 720720.0);
                pos = lanePos[l];
                stepSec = 1.0 / hz;
            }
            else
            {
                double b = beats;
                switch (s.retrigger)
                {
                case RetrigBar: b = std::fmod(std::max(0.0, beats), beatsPerBar); break;
                case Retrig2Bars: b = std::fmod(std::max(0.0, beats), 2 * beatsPerBar); break;
                case Retrig4Bars: b = std::fmod(std::max(0.0, beats), 4 * beatsPerBar); break;
                case RetrigOnStart: b = beats - playStart; break;
                default: break;
                }
                pos = b / stepBeats;
                stepSec = stepBeats * 60.0 / bpm;
            }
            pos += swayPush - std::clamp((double)L.offset, -0.5, 0.5);
            // swing: in each pair of steps the first lasts 2 x swing of the pair
            double sw = std::clamp((double)L.swing, 0.5, 0.75);
            auto stepAt = [sw](double q)
            {
                double pair = std::floor(q / 2.0), ph = q - pair * 2.0;
                double inPair = ph < 2.0 * sw ? ph / (2.0 * sw) : 1.0 + (ph - 2.0 * sw) / (2.0 - 2.0 * sw);
                return (long)(pair * 2.0 + std::floor(inPair));
            };
            long st = stepAt(pos);
            // which step plays: forward, reverse, ping-pong (ends not repeated) or random (by step count, so it repeats)
            const int dir = L.direction;
            auto stepIndex = [dir, l](long q, int len)
            {
                int f = (int)(((q % len) + len) % len);
                switch (dir)
                {
                case 1: return len - 1 - f;
                case 2:
                {
                    if (len < 2)
                        return 0;
                    long per = 2L * len - 2, r = ((q % per) + per) % per;
                    return (int)(r < len ? r : per - r);
                }
                case 3: return std::min((int)(rand01(4242 + (uint64_t)l, (uint64_t)q, 777) * (float)len), len - 1);
                default: return f;
                }
            };
            int idx = stepIndex(st, n);
            int k = std::clamp(L.hits, 0, n), rot = ((L.rotate % n) + n) % n;
            bool hit = ((((idx + rot) % n) * k) % n) < k;
            float v = std::clamp(L.step[idx], -1.f, 1.f);
            if (L.euclid == 1)
                v = hit ? v : 0.f;
            else if (L.euclid == 2)
                v = hit ? 1.f : 0.f;
            if (idx != laneIdx[l])
            {
                if (l == Mid && laneIdx[l] >= 0 && (L.euclid == 0 || hit))
                    midLaneStepped = true;
                laneIdx[l] = idx;
            }
            laneTarget[l] = v;
            double tau = std::max(0.0005, std::clamp((double)L.smooth, 0.0, 1.0) * stepSec);
            laneOut[l] += (float)(1.0 - std::exp(-dt / tau)) * (laneTarget[l] - laneOut[l]);
            float depth = std::clamp(L.depth, 0.f, 1.f);
            if (L.target > 0 && L.target < numModTargetIds)
                tgtOff[L.target] += laneOut[l] * depth * targetScale(L.target);
            // P-locks: a locked step pushes its target; unlocked steps glide back
            // the lock lane has its own length and rate (or follows the sequencer's)
            int ln = L.lockLength > 0 ? std::clamp(L.lockLength, 1, numSteps) : n;
            long lst = st;
            if (L.lockRateIndex > 0)
                lst = stepAt(pos * stepBeats / laneRateBeats[std::clamp(L.lockRateIndex - 1, 0, numLaneRates - 1)]);
            int lidx = stepIndex(lst, ln);
            lockIdx[l] = lidx;
            // each knob locked on this step goes to its locked value; the others glide back to the knob
            const auto &KB = s.mode[std::clamp(s.routing, 0, 1)].band[l];
            const float base[numBandParams] = {KB.cutoff, KB.resonance, KB.morph, KB.drive, KB.levelDb, KB.pan};
            float ga = 1.f - std::exp(-(float)dt / (std::max(0.2f, L.lockGlideMs) * 0.001f));
            for (int k = 0; k < numBandParams; ++k)
            {
                float lv = L.plock[k][lidx];
                float want = std::isfinite(lv) ? lv - base[k] : 0.f;
                lockK[l][k] += ga * (want - lockK[l][k]);
                if (std::fabs(lockK[l][k]) < 1.0e-6f)
                    lockK[l][k] = 0.f;
                tgtLock[bandTarget(l, k)] += lockK[l][k];
            }
        }

        // ----- LFOs
        for (int i = 0; i < numLfos; ++i)
        {
            const auto &F = s.lfo[i];
            double cycleSec;
            if (F.mode == LfoFree)
            {
                double hz = std::clamp((double)F.rateHz, 0.005, 500.0);
                lfoFree[i] += hz * dt;
                if (lfoFree[i] > 1.0e6)
                    lfoFree[i] = std::fmod(lfoFree[i], 1.0);
                lfoPh[i] = lfoFree[i];
                cycleSec = 1.0 / hz;
            }
            else
            {
                int si = std::clamp(F.syncIndex, 0, 7);
                double cycBeats = lfoSyncLen[si] * (lfoSyncBars[si] ? beatsPerBar : 1.0);
                double b = F.mode == LfoRetrig ? beats - playStart : beats;
                lfoPh[i] = b / cycBeats;
                cycleSec = cycBeats * 60.0 / bpm;
            }
            double ph = lfoPh[i] + std::clamp((double)F.phase, 0.0, 1.0) + std::clamp((double)F.sway, 0.0, 1.0) * 0.25 * swOut[Mid];
            ph -= std::floor(ph);
            lfoPh[i] = ph;
            float v;
            if (F.shape == ShapeSteps)
                v = stepLfoValue(F, ph);
            else
            {
                const float *tab = F.shape == ShapeCustom ? customTable[i][curveActive[i].load(std::memory_order_acquire)]
                                                          : shapeTables[std::clamp(F.shape, 0, numShapes - 1)];
                double x = ph * curveN;
                int xi = std::min((int)x, curveN - 1);
                float fr = (float)(x - xi);
                v = tab[xi] + fr * (tab[xi + 1] - tab[xi]);
            }
            double tau = std::max(0.0005, std::clamp((double)F.smooth, 0.0, 1.0) * cycleSec * 0.15);
            lfoRaw[i] += (float)(1.0 - std::exp(-dt / tau)) * (v - lfoRaw[i]);
            lfoOut[i] = F.unipolar ? 0.5f * (lfoRaw[i] + 1.f) : lfoRaw[i];
            if (F.target > 0 && F.target < numModTargetIds)
                tgtOff[F.target] += lfoOut[i] * std::clamp(F.amount, -1.f, 1.f) * targetScale(F.target);
        }

        // ----- envelope follower (peak, attack / release, sensitivity)
        {
            float pk = 0.f;
            switch (E.source)
            {
            case EnvSidechain: pk = pkSc; break;
            case EnvLow: pk = pkBand[Low]; break;
            case EnvMid: pk = pkBand[Mid]; break;
            case EnvHigh: pk = pkBand[High]; break;
            default: pk = inPeakForEnv; break;
            }
            pk *= std::pow(10.f, std::clamp(E.sensDb, -24.f, 36.f) / 20.f);
            float att = 1.f - std::exp(-(float)dt / (std::max(0.1f, E.attackMs) * 0.001f));
            float rel = 1.f - std::exp(-(float)dt / (std::max(1.f, E.releaseMs) * 0.001f));
            efState += (pk > efState ? att : rel) * (pk - efState);
            if (efState < 1.0e-9f)
                efState = 0.f;
            efOut = std::clamp(efState, 0.f, 1.f);
        }

        // ----- the matrix
        float src[numModSources] = {0.f,
                                    laneOut[0] * std::clamp(s.lane[0].depth, 0.f, 1.f),
                                    laneOut[1] * std::clamp(s.lane[1].depth, 0.f, 1.f),
                                    laneOut[2] * std::clamp(s.lane[2].depth, 0.f, 1.f),
                                    lfoOut[0], lfoOut[1], lfoOut[2], efOut, ripOut[0], ripOut[1], ripOut[2]};
        // which sources only go one way (0..1): the follower, and LFOs set to Unipolar
        const bool uni[numModSources] = {false, false, false, false, s.lfo[0].unipolar, s.lfo[1].unipolar, s.lfo[2].unipolar, true, false, false, false};
        for (const auto &sl : s.slot)
            if (sl.source > 0 && sl.source < numModSources && sl.target > 0 && sl.target < numModTargetIds)
            {
                float v = src[sl.source];
                if (sl.swing == 1 && uni[sl.source])
                    v = 2.f * v - 1.f; // both ways: swings below the knob as far as above
                else if (sl.swing == 2 && !uni[sl.source])
                    v = 0.5f * (v + 1.f); // one way: only ever up (or down, with a negative amount)
                tgtOff[sl.target] += v * std::clamp(sl.amount, -1.f, 1.f) * targetScale(sl.target);
            }
        // the master Mod Depth scales everything that moves; P-locks are set by hand, so they stay as drawn
        modMaster = std::clamp(s.modDepth + tgtOff[TgtModDepth], 0.f, 2.f);
        // these move settings, not sounds: taken before the master depth scales everything
        for (int b = 0; b < numBands; ++b)
            stripDepthOff[b] = tgtOff[TgtStripDepthLow + b];
        specBalOff = tgtOff[TgtSpecBalance], specSharpOff = tgtOff[TgtSpecSharp];
        dispGlobal[TgtModDepth - TgtSplitLo] = modMaster * 0.5f;
        for (int t = 0; t < numModTargetIds; ++t)
            tgtOff[t] = tgtOff[t] * modMaster + tgtLock[t];
    }

    // The Conductor passes Mid's movement to Low and High. With a grid it does so in steps, on the
    // beat: each step may pass (Chance, bar Conditions, clock dividers), Chaos swaps Mid's own
    // movement for random jumps, Bursts and the Third wheel add sudden moves. All of it is a
    // function of the seed and the song position, so the same bar plays the same way.
    double condBeat{0};
    long condTick{-1}, condBar{-1}, condFine{-1};
    float condHeld[numBands]{}, condBurst[numBands]{}, condTw{0}, condSm[numBands]{};
    long jmTick{LONG_MIN};
    float jmT[numBands]{}, jmS[numBands]{}; // Jack's mischief: where each strip is knocked to, and its glide
    bool condBarOn{true}, condBurstBar{false}, condTwBar{false};
    int condBurstBeat{0}, condTwSign{1};
    static float snapMusical(float st, int m)
    {
        if (m == 1)
            return std::round(st);
        if (m == 2)
            return std::round(st / 7.f) * 7.f;
        if (m == 3)
            return std::round(st / 12.f) * 12.f;
        return st;
    }
    void runConductor(const Settings &s, float midMove, double dt)
    {
        const auto &C = s.conductor;
        float infl = std::clamp(C.influence, 0.f, 1.f), lean = std::clamp(C.lean, 0.f, 1.f);
        float share[numBands] = {1.f - lean, 0.f, lean};
        float chaos = std::clamp(C.chaos, 0.f, 1.f), range = std::clamp(C.range, 0.f, 48.f);
        uint64_t seed = (uint64_t)(uint32_t)C.seed;
        condBeat = beats;
        double bpb = std::max(1.0, beatsPerBar);
        long bar = (long)std::floor(condBeat / bpb);
        if (bar != condBar)
        {
            condBar = bar;
            int ci = std::clamp(C.condition, 0, numCondConditions - 1);
            int A = condConditions[ci][0], B = condConditions[ci][1];
            condBarOn = B == 0 || ((bar % B + B) % B) + 1 == A;
            condBurstBar = rand01(seed, (uint64_t)bar, 501) < std::clamp(C.bursts, 0.f, 1.f);
            condBurstBeat = (int)(rand01(seed, (uint64_t)bar, 502) * bpb);
        }
        float target[numBands]{};
        if (C.grid <= 0)
        {
            // continuous: Mid's movement (or its random walk) passed straight on
            float r = 0.f;
            if (chaos > 0.f)
            {
                long t = (long)std::floor(condBeat / 0.25);
                if (t != condTick)
                    condTick = t, condHeld[Mid] = snapMusical((rand01(seed, (uint64_t)t, 510) * 2.f - 1.f) * range, C.musical);
                r = condHeld[Mid];
            }
            float m = (1.f - chaos) * midMove + chaos * r;
            for (int b = 0; b < numBands; ++b)
                target[b] = condBarOn ? infl * share[b] * m : 0.f;
        }
        else
        {
            double gb = condGridBeats[std::clamp(C.grid - 1, 0, numCondGrid - 1)];
            long t = (long)std::floor(condBeat / gb);
            if (t != condTick)
            {
                condTick = t;
                float r = snapMusical((rand01(seed, (uint64_t)t, 511) * 2.f - 1.f) * range, C.musical);
                float m = (1.f - chaos) * snapMusical(midMove, C.musical) + chaos * r;
                bool pass = condBarOn && rand01(seed, (uint64_t)t, 512) < std::clamp(C.chance, 0.f, 1.f);
                if (pass && infl > 0.f)
                    chapStepped = true, ++chapSteps, chapDir = m >= 0.f ? 1 : -1; // the Chaperone passed something on (it can lead Mutation)
                int div[numBands] = {std::max(1, C.divLow), 1, std::max(1, C.divHigh)};
                for (int b = 0; b < numBands; ++b)
                    if (b != Mid && ((t % div[b]) + div[b]) % div[b] == 0)
                        condHeld[b] = pass ? infl * share[b] * m : (condBarOn ? condHeld[b] : 0.f);
            }
            for (int b = 0; b < numBands; ++b)
                target[b] = condHeld[b];
        }
        // bursts: one beat of 1/32 jumps; the third wheel: Mid grabs both for the first beat of a bar
        double inBar = condBeat - (double)bar * bpb;
        bool burstNow = condBurstBar && (int)std::floor(inBar) == condBurstBeat;
        long fine = (long)std::floor(condBeat / 0.125);
        if (fine != condFine)
        {
            condFine = fine;
            for (int b = 0; b < numBands; ++b)
                condBurst[b] = burstNow && b != Mid ? snapMusical((rand01(seed, (uint64_t)fine, 520 + (uint64_t)b) * 2.f - 1.f) * range, C.musical) : 0.f;
        }
        if (!burstNow)
            condBurst[Low] = condBurst[High] = 0.f;
        condTw = 0.f;
        // a few milliseconds of glide so jumps don't click
        float a = 1.f - std::exp(-(float)dt / 0.006f);
        for (int b = 0; b < numBands; ++b)
        {
            // stepped values glide; the smooth (continuous) pass-on goes straight through, as before
            bool stepped = C.grid > 0 || chaos > 0.f;
            float jumps = b == Mid ? 0.f : (stepped ? target[b] : 0.f) + infl * (condBurst[b] + condTw);
            condSm[b] += a * (jumps - condSm[b]);
            condCut[b] = b == Mid ? 0.f : condSm[b] + (stepped ? 0.f : target[b]);
        }
    }

    // A step value snapped to a scale. Values are -1..1 over +-24 semitones (the cutoff scale).
    static float quantizeStep(float v, int q)
    {
        if (q <= 0)
            return v;
        float st = std::clamp(v, -1.f, 1.f) * 24.f;
        if (q == 5)
            return std::round(st / 12.f) * 12.f / 24.f;
        if (q == 1)
            return std::round(st) / 24.f;
        static const int major[7] = {0, 2, 4, 5, 7, 9, 11}, minor[7] = {0, 2, 3, 5, 7, 8, 10}, penta[5] = {0, 3, 5, 7, 10};
        const int *sc = q == 2 ? major : q == 3 ? minor : penta;
        int n = q == 4 ? 5 : 7;
        float best = 0, bd = 1e9f;
        for (int oct = -3; oct <= 2; ++oct)
            for (int kk = 0; kk < n; ++kk)
            {
                float c = (float)(oct * 12 + sc[kk]);
                if (std::fabs(c - st) < bd)
                    bd = std::fabs(c - st), best = c;
            }
        return std::clamp(best, -24.f, 24.f) / 24.f;
    }
    // The Steps shape at phase ph: the step's value, gliding in from the last one
    static float stepLfoValue(const LfoSettings &F, double ph)
    {
        int n = std::clamp(F.stepCount, 1, 16);
        double pos = ph * n;
        int kk = std::min(n - 1, (int)pos);
        float fr = (float)(pos - kk);
        float v = quantizeStep(F.step[kk], F.quantize);
        float g = std::clamp(F.glide, 0.f, 1.f);
        if (g > 0.f && fr < g)
        {
            float prev = quantizeStep(F.step[(kk + n - 1) % n], F.quantize);
            float t = fr / g;
            t = t * t * (3.f - 2.f * t);
            v = prev + (v - prev) * t;
        }
        return v;
    }

    // Works out this control block's Sway, Conductor and Mutation offsets into im[].
    float inPeakForEnv{0};
    void runModulation(const Settings &s, BandMod im[numBands])
    {
        inPeakForEnv = inPeak;
        inPeak = 0.f;
        const double dt = (double)blockSize / sampleRate;
        timeSec += dt;
        beats += dt * bpm / 60.0; // replaced by the host's song position while playing
        const double stepSec = 15.0 / bpm; // one sixteenth

        // input follower (for Sway's Follow and the Follower trigger)
        {
            float a = inPeakForEnv;
            float att = 1.f - std::exp(-(float)dt / 0.010f), rel = 1.f - std::exp(-(float)dt / 0.300f);
            env += (a > env ? att : rel) * (a - env);
            envSlow += (1.f - std::exp(-(float)dt / 0.600f)) * (a - envSlow);
            if (env < 1.0e-7f)
                env = 0.f;
            if (envSlow < 1.0e-7f)
                envSlow = 0.f;
        }

        // ----- Phase 3 sources: lanes, LFOs, follower; then the matrix
        runSources(s, dt);

        // ----- Tune (after the matrix, so its mod targets are in)
        {
            const auto &T = s.tune;
            tuneOn = T.on, tuneBefore = T.beforeStrips, tuneLayer = T.layer;
            tuneAmt = std::clamp(T.amount + tgtOff[TgtTuneAmount], 0.f, 1.f);
            if (tuneOn)
            {
                // ring time moves by up to 3 octaves of time at 100 %; both rounded so the notes are only re-tuned on a real change
                float ring = std::clamp(T.ringMs * std::exp2(3.f * tgtOff[TgtTuneRing]), 20.f, 4000.f);
                ring = std::exp2(std::round(std::log2(ring) * 24.f) / 24.f);
                float tilt = std::round(std::clamp(T.tilt + tgtOff[TgtTuneTilt], -1.f, 1.f) * 50.f) / 50.f;
                tune.configure(T, ring, tilt);
                tune.setTone(std::clamp(T.slope + tgtOff[TgtTuneSlope], -1.f, 1.f), T.pivotHz);
            }
            // Motion runs even with Tune off, so the picture on the Tune page keeps moving with the song
            double div = tuneDivBeats[std::clamp(T.motionDiv, 0, numTuneDivs - 1)];
            double divBeats = div > 0 ? div : -div * beatsPerBar;
            tune.moveControl(T.motion, std::clamp(T.motionHz, 0.01f, 8.f), blockSize, T.motionSync, beats / std::max(1.0e-3, divBeats));
        }

        // ----- Sway: each band has two slow LFOs at rates that never line up, its own speed
        // (against the main rate), Smooth or Stepped, and its own share of the main Sway
        const auto &W = s.sway;
        static constexpr double ratio[4] = {1.0, 1.6180339887, 1.4142135624, 0.7853981634};
        static constexpr double off[4] = {0.0, 0.31, 0.57, 0.83};
        static constexpr int pairA[numBands] = {0, 2, 0}, pairB[numBands] = {1, 3, 3};
        static constexpr double wA[numBands] = {0.6, 0.7, -0.6}, wB[numBands] = {0.4, 0.3, 0.4}; // High leans away from Low
        const double cycleBeats = swayBarsTable[std::clamp(W.syncIndex, 0, 6)] * beatsPerBar;
        const double hz = std::clamp((double)W.rateHz, 0.005, 20.0);
        for (int b = 0; b < numBands; ++b)
        {
            const auto &WB = W.band[b];
            double mul = std::clamp((double)WB.rateMul, 0.25, 4.0), baseTotal, pa, pb;
            if (W.sync)
            {
                baseTotal = beats / cycleBeats * mul;
                pa = baseTotal * ratio[pairA[b]] + off[pairA[b]];
                pb = baseTotal * ratio[pairB[b]] + off[pairB[b]];
                swBandPh[b][0] = pa, swBandPh[b][1] = pb, swBandBase[b] = baseTotal;
            }
            else
            {
                swBandBase[b] += hz * mul * dt;
                swBandPh[b][0] += hz * mul * ratio[pairA[b]] * dt;
                swBandPh[b][1] += hz * mul * ratio[pairB[b]] * dt;
                for (double *x : {&swBandBase[b], &swBandPh[b][0], &swBandPh[b][1]})
                    if (*x > 1.0e6)
                        *x = std::fmod(*x, 1.0);
                baseTotal = swBandBase[b], pa = swBandPh[b][0], pb = swBandPh[b][1];
            }
            float raw = (float)(wA[b] * std::sin(6.283185307 * (pa - std::floor(pa))) + wB[b] * std::sin(6.283185307 * (pb - std::floor(pb))));
            bool stepped = WB.motion == 0 ? W.stepped : WB.motion == 2;
            if (stepped)
            {
                long st = (long)std::floor(baseTotal * 8.0);
                if (st != swBandStep[b])
                    swBandStep[b] = st, swHold[b] = raw;
                double cycleSec = (W.sync ? cycleBeats * 60.0 / bpm : 1.0 / hz) / mul;
                float slew = WB.motion == 0 ? W.slew : WB.slew;
                double tau = std::max(0.001, std::clamp((double)slew, 0.0, 1.0) * cycleSec / 8.0 * 0.5);
                swOut[b] += (float)(1.0 - std::exp(-dt / tau)) * (swHold[b] - swOut[b]);
            }
            else
                swOut[b] = raw;
        }
        float follow = std::clamp(W.follow, 0.f, 1.f);
        float depthScale = (1.f - follow) + follow * std::clamp(env * 4.f, 0.f, 1.5f);
        float sw[numBands], swU[numBands];
        for (int b = 0; b < numBands; ++b)
        {
            swU[b] = std::clamp(W.amount + tgtOff[TgtSwayAmt], 0.f, 1.f) * std::clamp(W.band[b].depth, 0.f, 1.f) * swOut[b] * depthScale;
            sw[b] = swU[b] * std::clamp(W.depthSemis, 0.f, 48.f) * std::clamp(W.toCut, 0.f, 1.f);
        }

        // ----- Conductor: Mid's cutoff movement (its Sway plus host modulation) is passed on
        // Mid's movement: everything moving its cutoff (Sway, lanes, LFOs, matrix, Bitwig)
        float midMove = sw[Mid] + s.mod[Mid].cutoff + tgtOff[bandTarget(Mid, PCut)];
        runConductor(s, midMove, dt);

        // ----- Mutation
        const auto &M = s.mutation;
        // the answers after Mutate: a function of the seed and how many periods have passed,
        // so the same bar always plays the same answers
        {
            long k = 0;
            if (M.mutate > 0.f)
            {
                double every = mutEveryTable[std::clamp(M.everyIndex, 0, 4)];
                double per = std::max(0.0, beats) / beatsPerBar / every;
                k = (long)std::floor(per);
                mutFrac = (float)(per - (double)k);
                k = std::min(k, 4096L);
            }
            else
                mutFrac = 0.f;
            if (M.freeze)
            {
                if (!wasFrozen)
                    frozenK = k;
                k = frozenK;
            }
            wasFrozen = M.freeze;
            if (k != mutK || M.seed != lastSeed || M.mutate != lastMutate || !sameResp(M.resp[0], lastParams[0]) ||
                !sameResp(M.resp[1], lastParams[1]))
            {
                mutK = k;
                lastSeed = M.seed;
                lastMutate = M.mutate;
                lastParams[0] = M.resp[0];
                lastParams[1] = M.resp[1];
                effResp[0] = M.resp[0];
                effResp[1] = M.resp[1];
                float m = std::clamp(M.mutate, 0.f, 1.f);
                if (m > 0.f)
                    for (long i = 0; i < k; ++i)
                        mutateStep(effResp, M.resp, M.seed, i, m);
                for (int r = 0; r < 2; ++r)
                    effResp[r].target = std::clamp(effResp[r].target, 0, numMutTargets - 1);
            }
        }
        // detect an event in Mid
        float sens = std::clamp(M.sensitivity, 0.f, 1.f);
        {
            float pk = midPeak; // copied by runSources this block
            midFast = std::max(pk, midFast * std::exp(-(float)dt / 0.030f));
            midSlow += (1.f - std::exp(-(float)dt / 0.400f)) * (pk - midSlow);
            if (midFast < 1.0e-7f)
                midFast = 0.f;
            if (midSlow < 1.0e-7f)
                midSlow = 0.f;
        }
        float prevSlowMove = moveSlow;
        moveSlow += (1.f - std::exp(-(float)dt / 0.050f)) * (midMove - moveSlow);
        bool fire = false;
        int dir = 1;
        float ratioThr = 1.4f + (1.f - sens) * 2.6f; // +3 to +12 dB over the recent level
        switch (std::clamp(M.trigger, 0, numMutTriggers - 1))
        {
        case TrigLevel:
            if (lvlArmed && midFast > 0.01f && midFast > midSlow * ratioThr)
                fire = true, lvlArmed = false;
            else if (midFast < midSlow * 1.1f)
                lvlArmed = true;
            break;
        case TrigCutoff:
        {
            float jump = midMove - prevSlowMove, thr = 0.5f + (1.f - sens) * 6.f;
            if (cutArmed && std::fabs(jump) > thr)
                fire = true, cutArmed = false, dir = jump >= 0 ? 1 : -1;
            else if (std::fabs(jump) < thr * 0.5f)
                cutArmed = true;
            break;
        }
        case TrigGuard:
            if (guardArmed && bs[Mid].guard.gain < 0.85f)
                fire = true, guardArmed = false, dir = -1;
            else if (bs[Mid].guard.gain > 0.95f)
                guardArmed = true;
            break;
        case TrigChaperone: // the Chaperone leads: each step it passes on sets off a mutation
            if (chapStepped)
                fire = true, dir = chapDir;
            chapStepped = false;
            break;
        case TrigStep:
        {
            // Mid's own sequencer lane when it is doing something, otherwise Bitwig's 1/16 grid
            long st = (long)std::floor(beats * 4.0);
            if (midLaneActive(s))
                fire = midLaneStepped;
            else if (stepPrev != LONG_MIN && st != stepPrev)
                fire = true;
            stepPrev = st;
            float d = midMove - prevSlowMove;
            dir = std::fabs(d) < 0.05f ? ((st & 1) ? -1 : 1) : (d > 0 ? 1 : -1);
            break;
        }
        case TrigFollower:
            if (envArmed && env > 0.01f && env > envSlow * ratioThr)
                fire = true, envArmed = false;
            else if (env < envSlow * 1.1f)
                envArmed = true;
            break;
        }
        if (M.trigger != TrigStep)
            stepPrev = LONG_MIN;
        midLaneStepped = false;
        double hold = std::max(0.015, std::clamp(M.holdSteps, 0, 16) * stepSec);
        if (fire && timeSec - lastEvent < hold - 1.5 * dt) // events right on the limit still count
            fire = false;
        if (fire)
        {
            lastEvent = timeSec;
            ++eventCount;
            MutEvent ev;
            ev.dir = dir;
            for (int r = 0; r < 2; ++r)
            {
                const auto &R = effResp[r];
                if (R.mode == MutOff)
                    continue;
                if (rand01((uint64_t)(uint32_t)M.seed, eventCount, (uint64_t)r + 1000) > R.chance)
                    continue;
                float v = (float)dir * (R.mode == MutTandem ? 1.f : -1.f) * std::clamp(R.amount, 0.f, 1.f);
                (r == 0 ? ev.low : ev.high) = R.mode == MutTandem ? dir : -dir;
                double at = timeSec + std::clamp(R.delaySteps, 0, 4) * stepSec;
                for (auto &p : pend[r])
                    if (!p.live)
                    {
                        p = {at, v, true};
                        break;
                    }
            }
            hist[histPos] = ev;
            histPos = (histPos + 1) % 16;
        }
        // play the answers: each one jumps in quickly and fades back over about a third of a second
        float kDecay = std::exp(-(float)dt / 0.300f), kAtt = 1.f - std::exp(-(float)dt / 0.004f);
        for (int r = 0; r < 2; ++r)
        {
            if (effResp[r].target != respTgt[r])
            {
                respTgt[r] = effResp[r].target;
                respKick[r] = respVal[r] = 0.f;
            }
            for (auto &p : pend[r])
                if (p.live && timeSec >= p.at)
                {
                    respKick[r] = std::clamp(respKick[r] + p.val, -1.f, 1.f);
                    p.live = false;
                }
            respKick[r] *= kDecay;
            respVal[r] += kAtt * (respKick[r] - respVal[r]);
            if (std::fabs(respKick[r]) < 1.0e-6f && std::fabs(respVal[r]) < 1.0e-6f)
                respKick[r] = respVal[r] = 0.f;
        }

        // ----- add it all up
        for (int b = 0; b < numBands; ++b)
            im[b] = BandMod{};
        for (int b = 0; b < numBands; ++b)
            sw[b] *= modMaster, swU[b] *= modMaster;
        for (int b = 0; b < numBands; ++b)
        {
            im[b].cutoff = sw[b] + condCut[b] + tgtOff[bandTarget(b, PCut)];
            im[b].resonance = tgtOff[bandTarget(b, PRes)] + 0.5f * W.toRes * swU[b];
            im[b].morph = tgtOff[bandTarget(b, PMorph)] + 0.5f * W.toMorph * swU[b];
            im[b].drive = tgtOff[bandTarget(b, PDrive)] + 0.5f * W.toDrive * swU[b];
            im[b].levelDb = tgtOff[bandTarget(b, PLevel)] + 12.f * W.toLevel * swU[b];
            im[b].pan = tgtOff[bandTarget(b, PPan)] + W.toPan * swU[b];
        }
        // ----- Jack: one strip moves, another moves the same amount the other way
        {
            const auto &C = s.conductor;
            float jk = std::clamp(C.jack, 0.f, 1.f), mis = std::clamp(C.mischief, 0.f, 1.f);
            // the mischief: on some half beats Jack knocks one strip (any of the three) a little out of
            // line for a beat or so, on the knob Jack moves. Random, but the same with the same seed.
            {
                long t = (long)std::floor(beats / 0.5);
                if (t != jmTick)
                {
                    jmTick = t;
                    uint64_t sd = (uint64_t)(uint32_t)C.seed + 991;
                    if (rand01(sd, (uint64_t)t, 530) < mis * 0.6f)
                    {
                        int b = std::min((int)(rand01(sd, (uint64_t)t, 531) * 3.f), 2);
                        float amt = (rand01(sd, (uint64_t)t, 532) * 2.f - 1.f) * (1.5f + 7.f * mis); // semitones
                        jmT[b] = amt;
                    }
                    else if (rand01(sd, (uint64_t)t, 533) < 0.5f)
                        for (auto &v : jmT)
                            v = 0.f; // and back in line
                }
                if (mis <= 0.f)
                    for (auto &v : jmT)
                        v = 0.f;
                float ga = 1.f - std::exp(-(float)dt / 0.03f);
                for (int b = 0; b < numBands; ++b)
                    jmS[b] += ga * (jmT[b] - jmS[b]);
            }
            if (jk > 0.f || mis > 0.f)
            {
                float mv[numBands];
                for (int b = 0; b < numBands; ++b)
                    mv[b] = im[b].cutoff - tgtLock[bandTarget(b, PCut)]; // how far it is being moved (not its P-locks)
                float push[numBands]{};
                switch (std::clamp(C.jackPair, 0, 5))
                {
                case 0: push[Low] = -mv[High], push[High] = -mv[Low]; break; // Low <-> High
                case 1: push[Low] = push[High] = -mv[Mid]; break;           // Mid against both
                case 2: push[High] = -mv[Low]; break;                       // Low -> High
                case 3: push[Low] = -mv[High]; break;                       // High -> Low
                case 4: push[Low] = -mv[Mid]; break;                        // Mid -> Low
                case 5: push[High] = -mv[Mid]; break;                       // Mid -> High
                }
                for (int b = 0; b < numBands; ++b)
                {
                    float v = jk * push[b] + jmS[b]; // semitones
                    switch (std::clamp(C.jackParam, 0, numBandParams - 1))
                    {
                    case PCut: im[b].cutoff += v; break;
                    case PRes: im[b].resonance += v / 48.f; break;
                    case PMorph: im[b].morph += v / 48.f; break;
                    case PDrive: im[b].drive += v / 48.f; break;
                    case PLevel: im[b].levelDb += v * 0.5f; break;
                    case PPan: im[b].pan += v / 24.f; break;
                    }
                }
            }
        }
        for (int r = 0; r < 2; ++r)
        {
            int b = r == 0 ? Low : High;
            float v = respVal[r];
            switch (respTgt[r])
            {
            case TgtCutoff: im[b].cutoff += 24.f * v * modMaster; break;
            case TgtReso: im[b].resonance += 0.5f * v * modMaster; break;
            case TgtDrive: im[b].drive += 0.5f * v * modMaster; break;
            case TgtLevel: im[b].levelDb += 12.f * v * modMaster; break;
            case TgtPan: im[b].pan += v * modMaster; break;
            }
        }
        // each strip's own Mod Depth (its P-locks stay as drawn)
        const auto &MS = s.mode[std::clamp(s.routing, 0, 1)];
        for (int b = 0; b < numBands; ++b)
        {
            float d = std::clamp(MS.band[b].modDepth + stripDepthOff[b], 0.f, 2.f);
            dispGlobal[TgtStripDepthLow + b - TgtSplitLo] = d * 0.5f;
            auto sc = [&](float &x, int p) { float lk = tgtLock[bandTarget(b, p)]; x = lk + d * (x - lk); };
            sc(im[b].cutoff, PCut), sc(im[b].resonance, PRes), sc(im[b].morph, PMorph), sc(im[b].drive, PDrive), sc(im[b].levelDb, PLevel), sc(im[b].pan, PPan);
        }
    }

    // ----- control rate (every 8 host samples) ---------------------------------------------
    void processControl(const Settings &s)
    {
        bool rebuildAll = false;
        if (recovering)
        {
            recovering = false;
            clearAudioState();
            limGain = 1.f;
            rebuildAll = true;
            needsSnap = true;
        }
        int os = std::clamp(s.oversample, 0, maxOsStages);
        if (os != osStages)
        {
            osStages = os;
            configureRates();
            clearAudioState();
            rebuildAll = true;
            needsSnap = true;
        }

        // Routing change: fade out (~6 ms), swap to the other routing's own settings with
        // clean state, fade back in.
        int wantRouting = std::clamp(s.routing, 0, 1);
        bool wantSpec = wantRouting == Parallel && s.splitMode == 1;
        int wantQ = std::clamp(s.specQuality, 0, 3);
        if (curRouting < 0)
        {
            curRouting = wantRouting, spectralOn = wantSpec;
            if (wantQ != specQ)
                specQ = wantQ, configureRates();
        }
        if ((wantRouting != curRouting || wantSpec != spectralOn || (wantSpec && wantQ != specQ)) && fadeState != FadeOut)
        {
            fadeState = FadeOut;
            fadeCount = fadeBlocks;
        }
        if (fadeState == FadeOut)
        {
            if (fadeCount > 0)
            {
                fade.d = (0.f - fade.v) / (float)(fadeCount * nInner);
                --fadeCount;
            }
            else
            {
                fade.snap(0.f);
                curRouting = wantRouting;
                spectralOn = wantSpec;
                if (wantQ != specQ)
                    specQ = wantQ, spec->setup(innerRate, 1 << osStages, 1024 << specQ);
                spec->reset();
                clearAudioState();
                rebuildAll = true;
                needsSnap = true;
                fadeState = FadeIn;
                fadeCount = fadeBlocks;
            }
        }
        if (fadeState == FadeIn)
        {
            if (fadeCount > 0)
            {
                fade.d = (1.f - fade.v) / (float)(fadeCount * nInner);
                --fadeCount;
            }
            else
            {
                fade.snap(1.f);
                fadeState = FadeIdle;
            }
        }

        const ModeSettings &m = s.mode[curRouting];
        const bool par = curRouting == Parallel;

        // Sway, Conductor and Mutation, added to the host's modulation
        BandMod im[numBands];
        runModulation(s, im);
        BandMod md[numBands];
        for (int i = 0; i < numBands; ++i)
        {
            md[i].cutoff = s.mod[i].cutoff + im[i].cutoff;
            md[i].resonance = s.mod[i].resonance + im[i].resonance;
            md[i].morph = s.mod[i].morph + im[i].morph;
            md[i].drive = s.mod[i].drive + im[i].drive;
            md[i].levelDb = s.mod[i].levelDb + im[i].levelDb;
            md[i].pan = s.mod[i].pan + im[i].pan;
        }

        // Filter type / on changes rebuild that filter
        for (int i = 0; i < numBands; ++i)
        {
            const auto &B = m.band[i];
            if (rebuildAll || B.model != curModel[i] || B.variant != curVariant[i] ||
                B.filterOn != curOn[i])
            {
                curModel[i] = B.model;
                curVariant[i] = B.variant;
                curOn[i] = B.filterOn;
                setupFilter(i);
            }
            else
            {
                filters[i].concludeBlock();
            }
        }

        // Knob smoothing (one-pole, ~5 ms)
        auto sm = [this](float &v, float t)
        {
            if (needsSnap)
                v = t;
            else
                v += knobAlpha * (t - v);
        };
        for (int i = 0; i < numBands; ++i)
        {
            const auto &B = m.band[i];
            sm(k.co[i], B.cutoff);
            sm(k.re[i], B.resonance);
            sm(k.mo[i], B.morph);
            sm(k.dr[i], B.drive);
            sm(k.lv[i], B.levelDb);
            sm(k.pan[i], B.pan);
        }
        sm(k.fb, m.feedbackOn ? m.feedback : 0.f);
        sm(k.ns, m.noiseOn ? m.noise : 0.f);
        sm(k.nsCol, m.noiseColor);
        sm(k.f1, hzToSemi(std::clamp(m.xoverLoHz, 40.f, 2000.f)));
        sm(k.f2, hzToSemi(std::clamp(m.xoverHiHz, 400.f, 16000.f)));
        sm(k.inDb, m.inputDb);
        sm(k.outDb, m.outputDb);
        sm(k.mix, m.mix);

        // Crossover: always at least half an octave between the two split points
        float f1m = std::clamp(k.f1 + tgtOff[TgtSplitLo], hzToSemi(30.0), hzToSemi(8000.0));
        float f2m = std::clamp(k.f2 + tgtOff[TgtSplitHi], hzToSemi(200.0), hzToSemi(18000.0));
        float f1st = f1m, f2st = std::max(f2m, f1m + 6.f);
        dispGlobal[TgtSplitLo - TgtSplitLo] = f1st;
        dispGlobal[TgtSplitHi - TgtSplitLo] = f2st;
        curF1 = semiToHz(f1st);
        curF2 = semiToHz(f2st);
        if (par)
            xover.set(curF1, curF2, innerRate);
        spec->f1 = (float)curF1, spec->f2 = (float)curF2;
        spec->by = std::clamp(s.specBy, 0, 1);
        spec->balance = std::clamp(s.specBalance + specBalOff, -1.f, 1.f), spec->sharp = std::clamp(s.specSharp + specSharpOff, 0.f, 1.f);
        dispGlobal[TgtSpecBalance - TgtSplitLo] = spec->balance, dispGlobal[TgtSpecSharp - TgtSplitLo] = spec->sharp;

        // ----- cutoff, resonance, morph per band (modulation, then smart limiting) --------
        float co[numBands], re[numBands], mo[numBands];
        for (int i = 0; i < numBands; ++i)
        {
            co[i] = k.co[i] + md[i].cutoff;
            re[i] = std::clamp(k.re[i] + md[i].resonance, 0.f, 1.f);
            mo[i] = std::clamp(k.mo[i] + md[i].morph, 0.f, 1.f);
            press[i] = 0.f;
        }
        if (s.smartEdges)
        {
            if (par)
            {
                // Each band knows its own edges. A low-pass may not close below a point just
                // inside the bottom of its band, a high-pass may not open above a point just
                // inside the top, and a band-pass keeps its centre inside the band.
                const float edge[4] = {hzToSemi(30.0), f1st, f2st, hzToSemi(16000.0)};
                const bool whole = spectralOn && spec->by == 1; // Tonal: each strip has the whole range
                for (int i = 0; i < numBands; ++i)
                {
                    if (!bs[i].active)
                        continue;
                    float lo = whole ? edge[0] : edge[i], hi = whole ? edge[3] : edge[i + 1];
                    float inset = std::min(7.f, 0.35f * (hi - lo));
                    float was = co[i];
                    if (kind[i] == PassKind::LowPass)
                        co[i] = softFloor(co[i], lo + inset, edgeKnee);
                    else if (kind[i] == PassKind::HighPass)
                        co[i] = softCeil(co[i], hi - inset, edgeKnee);
                    else if (kind[i] == PassKind::BandPass)
                        co[i] = softCeil(softFloor(co[i], lo, edgeKnee), hi, edgeKnee);
                    press[i] = std::min(1.f, std::fabs(co[i] - was) / 12.f);
                }
            }
            else
            {
                // Series: a high-pass above a low-pass leaves nothing. Find the tightest of
                // each and keep at least an octave open between them.
                float lpMin = 1.0e9f, hpMax = -1.0e9f;
                for (int i = 0; i < numBands; ++i)
                {
                    if (!bs[i].active || m.band[i].mute)
                        continue;
                    if (kind[i] == PassKind::LowPass)
                        lpMin = std::min(lpMin, co[i]);
                    else if (kind[i] == PassKind::HighPass)
                        hpMax = std::max(hpMax, co[i]);
                }
                bool haveLp = lpMin < 1.0e8f, haveHp = hpMax > -1.0e8f;
                float lpFloor = -1.0e9f, hpCeil = 1.0e9f;
                if (haveLp && haveHp && hpMax > lpMin - seriesGap)
                {
                    float mid = 0.5f * (hpMax + lpMin);
                    lpFloor = mid + 0.5f * seriesGap;
                    hpCeil = mid - 0.5f * seriesGap;
                    lpMin = std::max(lpMin, lpFloor);
                    hpMax = std::min(hpMax, hpCeil);
                }
                for (int i = 0; i < numBands; ++i)
                {
                    if (!bs[i].active || m.band[i].mute)
                        continue;
                    float was = co[i];
                    if (kind[i] == PassKind::LowPass)
                        co[i] = std::max(co[i], lpFloor);
                    else if (kind[i] == PassKind::HighPass)
                        co[i] = std::min(co[i], hpCeil);
                    else if (kind[i] == PassKind::BandPass)
                    {
                        // a band-pass has to sit in the window the other two leave open
                        if (haveHp)
                            co[i] = std::max(co[i], hpMax);
                        if (haveLp)
                            co[i] = std::min(co[i], std::max(lpMin, haveHp ? hpMax : lpMin));
                    }
                    press[i] = std::min(1.f, std::fabs(co[i] - was) / 12.f);
                }
            }
        }

        const auto &cat = filterCatalogue();
        for (int i = 0; i < numBands; ++i)
        {
            co[i] = std::clamp(co[i], -60.f, (float)maxCutoff);
            dispCo[i] = co[i];
            dispRe[i] = re[i];
            dispBand[i][PCut] = co[i];
            dispBand[i][PRes] = re[i];
            dispBand[i][PMorph] = mo[i];
            float mv = mo[i];
            if (zOn[i])
            {
                zCoefficients(i, co[i], re[i], mv);
                filters[i].makeCoefficients(0, co[i], re[i], 0.f);
                filters[i].copyCoefficientsFromVoiceToVoice(0, 1);
                filters[i].prepareBlock();
                continue;
            }
            if (bs[i].active)
            {
                const auto &mi = cat[(size_t)curModel[i]];
                const auto &cfg = mi.configs[(size_t)std::clamp(curVariant[i], 0,
                                                                (int)mi.configs.size() - 1)];
                if (sfpp::Filter::coefficientsExtraIsBipolar(mi.model, cfg, 0))
                    mv = mv * 2 - 1;
            }
            filters[i].makeCoefficients(0, co[i], re[i], mv);
            filters[i].copyCoefficientsFromVoiceToVoice(0, 1);
            filters[i].prepareBlock();
        }

        // ----- drive, level, gains --------------------------------------------------------
        bool anySolo = false;
        if (par)
            for (int i = 0; i < numBands; ++i)
                anySolo = anySolo || m.band[i].solo;

        float sumScale = 0.f, prodScale = 1.f;
        for (int i = 0; i < numBands; ++i)
        {
            const auto &B = m.band[i];
            auto &S = bs[i];
            S.driveFirst = B.driveFirst;
            S.dtype = std::clamp(B.driveType, 0, (int)numDriveTypes - 1);
            S.bypass = !par && B.mute;
            float amt = std::clamp(k.dr[i] + md[i].drive, 0.f, 1.f);
            dispBand[i][PDrive] = amt;
            dispBand[i][PLevel] = std::clamp(k.lv[i] + md[i].levelDb, -60.f, 12.f);
            float g = driveGainFor(S.dtype, amt);
            float comp = s.autoGain ? 1.f / std::sqrt(g) : 1.f;
            float wet = std::clamp(amt * 50.f, 0.f, 1.f); // drive fades in over the first 2%
            wet = wet * wet * (3.f - 2.f * wet);
            float lv = dbToLin(std::clamp(k.lv[i] + md[i].levelDb, -60.f, 12.f));
            if (par && (B.mute || (anySolo && !B.solo)))
                lv = 0.f;
            // equal-power balance: centre is unity on both sides
            float p = std::clamp(k.pan[i] + md[i].pan, -1.f, 1.f);
            dispBand[i][PPan] = p;
            float th = (p + 1.f) * 0.78539816f;
            float pl = std::cos(th) * 1.41421356f, pr = std::sin(th) * 1.41421356f;
            if (std::fabs(p) < 1.0e-6f)
                pl = pr = 1.f;
            if (needsSnap)
                panL[i].snap(pl), panR[i].snap(pr);
            else
                panL[i].to(pl, nInner), panR[i].to(pr, nInner);
            // how much louder a quiet signal leaves the drive: the guard allows for it
            float ss = 1.f + wet * (g * comp - 1.f);
            S.refScale2 = ss * ss;
            if (needsSnap)
            {
                S.dGain.snap(g);
                S.dComp.snap(comp);
                S.dWet.snap(wet);
                S.dAmt.snap(amt);
                S.level.snap(lv);
                S.smix.snap(std::clamp(B.stripMix, 0.f, 1.f));
            }
            else
            {
                S.dGain.to(g, nInner);
                S.dComp.to(comp, nInner);
                S.dWet.to(wet, nInner);
                S.dAmt.to(amt, nInner);
                S.level.to(lv, nInner);
                S.smix.to(std::clamp(B.stripMix, 0.f, 1.f), nInner);
            }
            float pg = std::max(pl, pr);
            if (par)
                sumScale = std::max(sumScale, ss * lv * pg);
            else if (!S.bypass)
                prodScale *= std::max(1.f, ss * lv * pg);
        }

        // ----- the banks -------------------------------------------------------------------
        {
            const float edgeP[4] = {hzToSemi(30.0), f1st, f2st, hzToSemi(16000.0)};
            const double dtc = (double)blockSize / sampleRate;
            for (int i = 0; i < numBands; ++i)
            {
                const auto &B = m.band[i];
                auto &S = bs[i];
                float wetT = std::clamp(B.bankMix, 0.f, 1.f);
                float lo = par ? edgeP[i] - 3.f : hzToSemi(40.0), hi = par ? edgeP[i + 1] + 3.f : hzToSemi(16000.0);
                if (hi < lo + 12.f)
                    hi = lo + 12.f;
                float wdt = std::clamp(B.bankWidth, 0.f, 1.f);
                double nyq = 0.45 * innerRate;
                if (std::fabs(lo - S.bkLo) > 0.05f || std::fabs(hi - S.bkHi) > 0.05f || std::fabs(wdt - S.bkW) > 1e-4f)
                {
                    S.bkLo = lo, S.bkHi = hi, S.bkW = wdt;
                    float spacingOct = (hi - lo) / numBankBands / 12.f;
                    float bwOct = spacingOct * (0.35f + 1.8f * wdt);
                    double q = 1.0 / (2.0 * std::sinh(0.34657359 * bwOct));
                    double fc[numBankBands];
                    for (int kb = 0; kb < numBankBands; ++kb)
                    {
                        fc[kb] = std::min(nyq, semiToHz(lo + (kb + 0.5f) * (hi - lo) / numBankBands));
                        double g = std::tan(3.14159265358979 * fc[kb] / innerRate), kq = 1.0 / q;
                        double a1 = 1.0 / (1.0 + g * (g + kq));
                        S.bkA1[kb] = (float)a1, S.bkA2[kb] = (float)(g * a1), S.bkA3[kb] = (float)(g * g * a1), S.bkK[kb] = (float)kq;
                        dispBankHz[i][kb] = (float)fc[kb];
                    }
                    // level: the eight together at full gain average about unity across the range
                    double sum = 0;
                    const int probes = 48;
                    for (int pi = 0; pi < probes; ++pi)
                    {
                        double f = semiToHz(lo + (pi + 0.5) * (hi - lo) / probes);
                        double re = 0, im2 = 0;
                        for (int kb = 0; kb < numBankBands; ++kb)
                        {
                            double w = f / fc[kb], a = 1 - w * w, b2 = w / q, d = a * a + b2 * b2;
                            re += (b2 * b2) / d, im2 += (b2 * a) / d;
                        }
                        sum += std::sqrt(re * re + im2 * im2);
                    }
                    S.bkNorm = (float)(probes / std::max(1e-6, sum));
                }
                if (B.rippleSync)
                    S.ripPh = beats / rippleSyncBeats[std::clamp(B.rippleSyncIndex, 0, numRippleSync - 1)];
                else
                {
                    S.ripPh += std::clamp((double)B.rippleRateHz, 0.01, 200.0) * dtc;
                    if (S.ripPh > 1e6)
                        S.ripPh = std::fmod(S.ripPh, 1.0);
                }
                float dep = std::clamp(B.rippleDepth, 0.f, 1.f), waves = std::clamp(B.rippleWaves, 0.25f, 4.f);
                // the ripple as a mod source: its shape where it passes the first band, -1..1, at its own rate
                // (whether or not the bank is heard, so it can drive anything as an LFO in time with the bank)
                ripOut[i] = std::clamp(rippleShapeValue(B.rippleShape, S.ripPh, waves, 0, (uint64_t)i), -1.f, 1.f);
                float st = std::clamp(B.bankStereo, 0.f, 1.f);
                for (int kb = 0; kb < numBankBands; ++kb)
                {
                    // Ripple: boosts and cuts running through the bands, in one of six shapes
                    const float g0 = std::clamp(B.bankGain[kb], -1.f, 1.f), sh = rippleShapeValue(B.rippleShape, S.ripPh, waves, kb, (uint64_t)i);
                    // one way (default): the ripple pushes the band further the way its slider points (up from the middle);
                    // both ways: it swings the band up and down around its slider
                    const float rv = (B.rippleBoth >> kb) & 1 ? sh : (g0 < 0.f ? -0.5f : 0.5f) * (sh + 1.f);
                    float v = std::clamp(g0 + dep * 0.8f * rv, -1.f, 1.f);
                    dispBankGain[i][kb] = v;
                    // added to the strip's own sound: +1 = about +12 dB at the band, -1 = the band taken out
                    float g = v > 0.f ? 3.f * v * S.bkNorm : v * std::min(1.f, S.bkNorm);
                    float gl = g * ((kb & 1) ? 1.f - st : 1.f), gr = g * ((kb & 1) ? 1.f : 1.f - st);
                    if (needsSnapBank)
                        S.bkGL[kb].snap(gl), S.bkGR[kb].snap(gr);
                    else
                        S.bkGL[kb].to(gl, nInner), S.bkGR[kb].to(gr, nInner);
                }
                if (needsSnapBank)
                    S.bkWet.snap(wetT);
                else
                    S.bkWet.to(wetT, nInner);
                // a flat bank (every band at 0, no ripple) adds nothing: skip it once its gains have settled
                bool flat = dep <= 0.f;
                for (int kb = 0; kb < numBankBands && flat; ++kb)
                    flat = B.bankGain[kb] == 0.f && std::fabs(S.bkGL[kb].v) < 1e-5f && std::fabs(S.bkGR[kb].v) < 1e-5f;
                bool on = (wetT > 0.f || S.bkWet.v > 1e-6f) && !flat;
                if (on && !S.bkOn)
                    for (int c = 0; c < 2; ++c)
                        for (int kb = 0; kb < numBankBands; ++kb)
                            S.bkS1[c][kb] = S.bkS2[c][kb] = 0.f;
                S.bkOn = on;
            }
            needsSnapBank = false;
        }

        float inG = dbToLin(std::clamp(k.inDb, -60.f, 24.f));
        float outG = dbToLin(std::clamp(k.outDb, -60.f, 24.f));
        float mx = std::clamp(k.mix + tgtOff[TgtMix], 0.f, 1.f);
        dispGlobal[TgtMix - TgtSplitLo] = mx;
        // feedback: fades in over the first 2% so Off and 0 are exactly the same
        float fbAmt = m.feedbackOn ? std::clamp(k.fb + tgtOff[TgtFeedback], 0.f, 1.f) : 0.f;
        dispGlobal[TgtFeedback - TgtSplitLo] = fbAmt;
        float fw = std::clamp(fbAmt * 50.f, 0.f, 1.f);
        float fl = fbAmt * fbAmt;
        // noise: cube law
        float nsA = m.noiseOn ? std::clamp(k.ns + tgtOff[TgtNoise], 0.f, 1.f) : 0.f;
        dispGlobal[TgtNoise - TgtSplitLo] = nsA;
        dispGlobal[TgtSwayAmt - TgtSplitLo] = std::clamp(s.sway.amount + tgtOff[TgtSwayAmt], 0.f, 1.f);
        float nsG = nsA * nsA * nsA;
        fbLpCoef = (float)(1.0 - std::exp(-2.0 * 3.14159265358979 * std::clamp((double)m.feedbackToneHz, 200.0, 16000.0) / innerRate));
        nsLpCoef = (float)(1.0 - std::exp(-2.0 * 3.14159265358979 * 600.0 / innerRate));
        if (needsSnap)
        {
            inGain.snap(inG);
            outGain.snap(outG);
            mix.snap(mx);
            fbWet.snap(fw);
            fbLevel.snap(fl);
            nsGain.snap(nsG);
            nsColor.snap(std::clamp(k.nsCol, 0.f, 1.f));
            needsSnap = false;
        }
        else
        {
            inGain.to(inG, nInner);
            outGain.to(outG, nInner);
            mix.to(mx, nInner);
            fbWet.to(fw, nInner);
            fbLevel.to(fl, nInner);
            nsGain.to(nsG, nInner);
            nsColor.to(std::clamp(k.nsCol, 0.f, 1.f), nInner);
        }
        // Loop leveller: the signal coming back round may never be louder than what is coming in
        // (input plus noise), so feedback colours and lengthens the sound but cannot hold a note
        // on its own or build up, whatever the filter does.
        {
            const float n = (float)nInner;
            float a = 1.f - std::exp(-(float)blockSize / (0.050f * (float)sampleRate));
            fbInMs += a * ((float)(fbInSum / n) - fbInMs);
            fbRetMs += a * ((float)(fbRetSum / n) - fbRetMs);
            fbInSum = fbRetSum = 0.0;
            if (fbInMs < 1.0e-14f)
                fbInMs = 0.f;
            if (fbRetMs < 1.0e-14f)
                fbRetMs = 0.f;
            float allowed = std::sqrt(fbInMs) * 0.7f;
            float ret = std::sqrt(fbRetMs);
            fbCtlTarget = ret > allowed ? allowed / (ret + 1.0e-12f) : 1.f; // silence in: the loop closes
        }
        // the noise counts as input for the output guard (about its mean square)
        float nsOut = nsG * outG * mx * (par ? std::max(sumScale, 1.0e-3f) : prodScale);
        finalNoise2 = 0.4f * nsOut * nsOut;
        float wetScale = inG * outG * (par ? std::max(sumScale, 1.0e-3f) : prodScale);
        float sc = (1.f - mx) + mx * wetScale;
        finalRefScale2 = sc * sc;
        guardsOn = s.limiter;
        bandBoost = par ? 4.0f : 2.82f; // Series stacks three stages: each gets +9 dB, not +12
    }

    // ----- audio rate ------------------------------------------------------------------------
    // Oversampling: each stage doubles the rate around everything inside it.
    void tickOS(int stage, float inL, float inR, float &oL, float &oR)
    {
        if (stage == 0)
        {
            if (curRouting == Series)
                coreSeries(inL, inR, oL, oR);
            else
                coreParallel(inL, inR, oL, oR);
            return;
        }
        float lu[2], ru[2], lo[2], ro[2];
        hrUp[stage - 1].up(0, inL, lu);
        hrUp[stage - 1].up(1, inR, ru);
        tickOS(stage - 1, lu[0], ru[0], lo[0], ro[0]);
        tickOS(stage - 1, lu[1], ru[1], lo[1], ro[1]);
        oL = hrDn[stage - 1].down(0, lo[0], lo[1]);
        oR = hrDn[stage - 1].down(1, ro[0], ro[1]);
    }

    inline void driveTick(int b, float &l, float &r)
    {
        auto &S = bs[b];
        float g = S.dGain.tick(), comp = S.dComp.tick(), wet = S.dWet.tick(), amt = S.dAmt.tick();
        if (wet <= 0.f)
            return;
        float x[2] = {l, r};
        for (int ch = 0; ch < 2; ++ch)
        {
            float y = shape(S.dtype, x[ch] * g, amt) * comp;
            // DC blocker: the lopsided shapes put out a standing offset
            float d = y - S.dcX[ch] + dcR * S.dcY[ch];
            S.dcX[ch] = y;
            S.dcY[ch] = std::fabs(d) < 1.0e-20f ? 0.f : d;
            x[ch] += wet * (d - x[ch]);
        }
        l = x[0];
        r = x[1];
    }

    inline void filterTick(int b, float &l, float &r)
    {
        if (!bs[b].active)
            return;
        if (zOn[b])
        {
            zTick(b, l, r);
            return;
        }
        float ol, orr;
        filters[b].processStereoSample(l, r, ol, orr);
        l = std::clamp(ol, -8.f, 8.f);
        r = std::clamp(orr, -8.f, 8.f);
    }

    // Ripple shapes across the eight bands (ph in cycles), each -1..1
    static float rippleShapeValue(int shape, double ph, float waves, int kb, uint64_t strip)
    {
        double x = (double)kb / (numBankBands - 1);
        double f = ph - std::floor(ph);
        switch (shape)
        {
        default:
        case 0: // Wave: travels across the bands
            return (float)std::cos(6.283185307 * (ph - waves * kb / (double)numBankBands));
        case 1: // Bounce: a peak that runs up and back
        {
            double pos = f < 0.5 ? f * 2.0 : 2.0 - f * 2.0;
            return (float)std::clamp(1.0 - std::fabs(x - pos) * 4.0 / std::max(0.25, (double)waves), -1.0, 1.0);
        }
        case 2: // Random: every band to a new level each eighth of a cycle
        {
            long t = (long)std::floor(ph * 8.0);
            return rand01(strip + 900, (uint64_t)t, (uint64_t)kb) * 2.f - 1.f;
        }
        case 3: // Spin: odd and even bands swap, so with Stereo it turns left-right
            return (float)((kb & 1 ? -1.0 : 1.0) * std::cos(6.283185307 * (ph - waves * kb / (2.0 * numBankBands))));
        case 4: // Rise: a ramp climbing through the bands
        {
            double r = ph + waves * kb / (double)numBankBands;
            return (float)((r - std::floor(r)) * 2.0 - 1.0);
        }
        case 5: // Spotlight: one band at a time
        {
            int on = (int)std::floor(f * numBankBands);
            return kb == on ? 1.f : -0.35f;
        }
        }
    }
    // The bank: eight band-passes, each with a gain that adds to (or takes from) the strip's sound
    inline void bankTick(int b, float &l, float &r)
    {
        auto &S = bs[b];
        float wet = S.bkWet.tick();
        float x[2] = {l, r}, y[2] = {0.f, 0.f};
        for (int kb = 0; kb < numBankBands; ++kb)
        {
            float gk[2] = {S.bkGL[kb].tick(), S.bkGR[kb].tick()};
            for (int c = 0; c < 2; ++c)
            {
                float v3 = x[c] - S.bkS2[c][kb];
                float v1 = S.bkA1[kb] * S.bkS1[c][kb] + S.bkA2[kb] * v3;
                float v2 = S.bkS2[c][kb] + S.bkA2[kb] * S.bkS1[c][kb] + S.bkA3[kb] * v3;
                S.bkS1[c][kb] = 2.f * v1 - S.bkS1[c][kb];
                S.bkS2[c][kb] = 2.f * v2 - S.bkS2[c][kb];
                if (std::fabs(S.bkS1[c][kb]) < 1e-20f)
                    S.bkS1[c][kb] = 0.f;
                if (std::fabs(S.bkS2[c][kb]) < 1e-20f)
                    S.bkS2[c][kb] = 0.f;
                y[c] += gk[c] * S.bkK[kb] * v1;
            }
        }
        l = std::clamp(x[0] + wet * y[0], -8.f, 8.f);
        r = std::clamp(x[1] + wet * y[1], -8.f, 8.f);
    }

    // One strip: filter and drive in the chosen order, its guard, then its level.
    inline void stripTick(int b, float &l, float &r)
    {
        auto &S = bs[b];
        float refMS = 0.5f * (l * l + r * r) * S.refScale2;
        const float dryL = l, dryR = r, sm = S.smix.tick();
        if (S.driveFirst)
        {
            driveTick(b, l, r);
            filterTick(b, l, r);
        }
        else
        {
            filterTick(b, l, r);
            driveTick(b, l, r);
        }
        if (S.bkOn)
            bankTick(b, l, r);
        if (sm < 0.9999f) // the strip's own Mix: how much of its processing is heard
            l = dryL + sm * (l - dryL), r = dryR + sm * (r - dryR);
        if (guardsOn)
            S.guard.process(refMS, bandBoost, l, r);
        float lv = S.level.tick();
        l *= lv * panL[b].tick();
        r *= lv * panR[b].tick();
        bandPeak[b] = std::max(bandPeak[b], std::max(std::fabs(l), std::fabs(r)));
    }

    static inline float warmFb(float x)
    {
        x = std::clamp(x, -3.f, 3.f);
        return x * (27.f + x * x) / (27.f + 9.f * x * x);
    }

    // noise in, then the feedback loop's return, before the strips
    inline void preTick(float &l, float &r)
    {
        float ns = nsGain.tick(), col = nsColor.tick();
        if (ns > 0.f)
        {
            float w[2];
            for (int ch = 0; ch < 2; ++ch)
            {
                nsState = nsState * 1664525u + 1013904223u;
                w[ch] = (float)((int32_t)nsState) * (1.f / 2147483648.f);
                nsLp[ch] += nsLpCoef * (w[ch] - nsLp[ch]);
                w[ch] = (1.f - col) * w[ch] + col * 3.f * nsLp[ch];
            }
            l += ns * w[0];
            r += ns * w[1];
        }
        curFw = fbWet.tick();
        curFl = fbLevel.tick();
        fbInSum += 0.5 * ((double)l * l + (double)r * r);
        if (curFw > 0.f)
        {
            // The input is driven into the saturator as feedback rises; the
            // returning signal joins after the drive, so the drive's gain is not inside the loop
            // and a resonant filter rings longer but still dies away.
            float drive = 1.f + 4.f * curFl;
            float sd = 1.f / std::sqrt(drive);
            float yl = warmFb(l * drive) * sd + fbL, yr = warmFb(r * drive) * sd + fbR;
            l += curFw * (yl - l);
            r += curFw * (yr - r);
        }
    }

    // the wet sum goes back round, and is turned down a little as feedback rises
    inline void postTick(float &l, float &r)
    {
        if (curFw > 0.f)
        {
            // the leveller closes fast (about 1 ms) and opens slowly (about 100 ms)
            fbCtl += (fbCtlTarget < fbCtl ? 0.02f : 0.0002f) * (fbCtlTarget - fbCtl);
            float rl = 0.2f * curFl * fbc[0].process(l, fbLpCoef), rr = 0.2f * curFl * fbc[1].process(r, fbLpCoef);
            fbRetSum += 0.5 * ((double)rl * rl + (double)rr * rr);
            fbL = warmFb(rl * fbCtl);
            fbR = warmFb(rr * fbCtl);
            float c = 1.f + curFw * (1.f / (1.f + 2.f * curFl) - 1.f);
            l *= c;
            r *= c;
        }
        else
            fbL = fbR = 0.f;
    }

    void coreParallel(float inL, float inR, float &oL, float &oR)
    {
        float g = inGain.tick();
        float xl = inL * g, xr = inR * g;
        preTick(xl, xr);
        float bl[numBands], br[numBands], dl, dr;
        if (spectralOn)
            spec->tick(xl, xr, inL, inR, bl, br, dl, dr);
        else
        {
            double lo[2], mi[2], hi[2];
            xover.split(0, (double)xl, lo[0], mi[0], hi[0]);
            xover.split(1, (double)xr, lo[1], mi[1], hi[1]);
            dl = (float)xover.dry(0, inL), dr = (float)xover.dry(1, inR);
            bl[0] = (float)lo[0], bl[1] = (float)mi[0], bl[2] = (float)hi[0];
            br[0] = (float)lo[1], br[1] = (float)mi[1], br[2] = (float)hi[1];
        }
        float sl = 0, sr = 0;
        for (int b = 0; b < numBands; ++b)
        {
            stripTick(b, bl[b], br[b]);
            sl += bl[b];
            sr += br[b];
        }
        postTick(sl, sr);
        float og = outGain.tick(), mx = mix.tick(), f = fade.tick();
        oL = (mx * sl * og + (1.f - mx) * dl) * f;
        oR = (mx * sr * og + (1.f - mx) * dr) * f;
        anaPush(0.5f * (bl[0] + br[0]), 0.5f * (bl[1] + br[1]), 0.5f * (bl[2] + br[2]), 0.5f * (oL + oR));
    }

    void coreSeries(float inL, float inR, float &oL, float &oR)
    {
        float g = inGain.tick();
        float l = inL * g, r = inR * g;
        preTick(l, r);
        float tap[numBands]{};
        for (int b = 0; b < numBands; ++b) // Low strip -> Mid strip -> High strip
        {
            if (bs[b].bypass)
            {
                (void)bs[b].dGain.tick(), (void)bs[b].dComp.tick(), (void)bs[b].dWet.tick(),
                    (void)bs[b].dAmt.tick(), (void)bs[b].level.tick(), (void)bs[b].smix.tick(), (void)panL[b].tick(), (void)panR[b].tick();
                tap[b] = 0.5f * (l + r);
                continue;
            }
            stripTick(b, l, r);
            tap[b] = 0.5f * (l + r);
        }
        postTick(l, r);
        float og = outGain.tick(), mx = mix.tick(), f = fade.tick();
        oL = (mx * l * og + (1.f - mx) * inL) * f;
        oR = (mx * r * og + (1.f - mx) * inR) * f;
        anaPush(tap[0], tap[1], tap[2], 0.5f * (oL + oR));
    }
};
} // namespace otiskanno::threescompany

#endif
