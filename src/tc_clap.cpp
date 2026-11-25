/*
 * Three's Company - CLAP plugin (the part a host such as Bitwig Studio talks to).
 * Otis Kanno, 2026. GPL v3, see LICENSE.
 *
 * The window (macOS) is a web view, see tc_gui.h. Elsewhere the host shows its own controls.
 * Every parameter can be automated; the continuous ones can be modulated.
 * Parallel and Series each have their own full set of parameters, so each routing
 * keeps its settings.
 */
#include "tc_engine.h"
#include "tc_presets.h"
#include <map>
#include <limits>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif
#include <string>
#include <unordered_map>
#include <vector>

#include <clap/clap.h>

#include "tc_gui.h"
#include "tc_gui_html.h"

#include <mutex>

#if defined(__aarch64__) || defined(__arm64__)
#elif defined(__SSE2__)
#include <xmmintrin.h>
#endif

using namespace otiskanno::threescompany;

namespace
{
// Very small numbers slow processors down badly; treat them as zero while processing.
struct ScopedNoDenormals
{
#if defined(__aarch64__) || defined(__arm64__)
    uint64_t old;
    ScopedNoDenormals()
    {
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(old));
        uint64_t v = old | (1ull << 24); // FZ
        __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
    }
    ~ScopedNoDenormals() { __asm__ __volatile__("msr fpcr, %0" : : "r"(old)); }
#elif defined(__SSE2__)
    unsigned old;
    ScopedNoDenormals()
    {
        old = _mm_getcsr();
        _mm_setcsr(old | 0x8040); // FTZ + DAZ
    }
    ~ScopedNoDenormals() { _mm_setcsr(old); }
#endif
};

// ---------------------------------------------------------------------------
// Parameters. Ids never change once a version is out; new ones get new ids.
// ---------------------------------------------------------------------------
inline const char *pluginVersion() { return "0.20.0"; }

// ----- settings kept on this computer (not in projects): defaults for new copies, and hover tips
struct Prefs
{
    int sizePct{100}; // 60..200
    bool dark{false};
    bool tips{true};
    std::string presetDir; // empty: the standard place (beside this file, in Presets)
};
inline std::string prefsPath()
{
#if defined(_WIN32)
    const char *base = std::getenv("APPDATA");
    return base ? std::string(base) + "\\Threes Company\\prefs.txt" : std::string();
#elif defined(__APPLE__)
    const char *home = std::getenv("HOME");
    return home ? std::string(home) + "/Library/Application Support/Threes Company/prefs.txt" : std::string();
#else
    const char *home = std::getenv("HOME");
    return home ? std::string(home) + "/.config/threes-company/prefs.txt" : std::string();
#endif
}
inline Prefs loadPrefs()
{
    Prefs p;
    std::string path = prefsPath();
    if (path.empty())
        return p;
    if (FILE *f = std::fopen(path.c_str(), "r"))
    {
        char line[2048];
        while (std::fgets(line, sizeof(line), f))
        {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
                l.pop_back();
            auto num = [&](size_t at) { return std::atoi(l.c_str() + at); };
            if (l.rfind("size ", 0) == 0)
                p.sizePct = std::clamp(num(5), 60, 200);
            else if (l.rfind("dark ", 0) == 0)
                p.dark = num(5) != 0;
            else if (l.rfind("tips ", 0) == 0)
                p.tips = num(5) != 0;
            else if (l.rfind("presets ", 0) == 0)
                p.presetDir = l.substr(8);
        }
        std::fclose(f);
    }
    return p;
}
inline void savePrefs(const Prefs &p)
{
    std::string path = prefsPath();
    if (path.empty())
        return;
    std::string dir = path.substr(0, path.find_last_of("/\\"));
#if defined(_WIN32)
    std::string parent = dir.substr(0, dir.find_last_of('\\'));
    (void)_mkdir(parent.c_str());
    (void)_mkdir(dir.c_str());
#else
    std::string parent = dir.substr(0, dir.find_last_of('/'));
    (void)::mkdir(parent.c_str(), 0755);
    (void)::mkdir(dir.c_str(), 0755);
#endif
    if (FILE *f = std::fopen(path.c_str(), "w"))
    {
        std::fprintf(f, "size %d\ndark %d\ntips %d\n", p.sizePct, p.dark ? 1 : 0, p.tips ? 1 : 0);
        if (!p.presetDir.empty())
            std::fprintf(f, "presets %s\n", p.presetDir.c_str());
        std::fclose(f);
    }
}

enum Kind { kPercent, kDb, kPitch, kToggle, kRouting, kOversample, kModel, kVariant, kDriveType, kOrder,
            kEnum, kInt, kPan, kLean, kLogHz, kSemis, kHzTone, kBipolar, kLock, kOffset, kMs, kRatio, kPLock, kLogMs, kNote };

enum GlobalId
{
    idRouting = 1, idOversample = 2, idSmart = 3, idAutoGain = 4, idLimiter = 5, idModDepth = 6,
    // Sway
    idSwayAmount = 20, idSwayRate, idSwaySync, idSwayBars, idSwayMotion, idSwaySlew, idSwayFollow, idSwayDepth,
    // Conductor
    idInfluence = 30, idLean,
    // Mutation
    idMutTrigger = 40, idMutSens, idMutHold,
    idMutLow = 50,  // + rMode, rTarget, rAmount, rChance, rDelay
    idMutHigh = 60,
    idMutate = 70, idMutEvery, idMutFreeze, idMutSeed
};
enum RespOffset { rMode = 0, rTarget, rAmount, rChance, rDelay };
// Phase 3
constexpr clap_id laneBase(int l) { return 3000u + 100u * (uint32_t)l; }
// a P-lock: lane l, knob k (cutoff, reso, morph, drive, level, pan), step i
// steps 1-16 keep their 0.8 ids; steps 17-32 live at 7000
constexpr clap_id plockId(int l, int k, int i) { return (i < 16 ? 6000u : 7000u) + 100u * (uint32_t)l + 16u * (uint32_t)k + (uint32_t)(i % 16); }
// a sequencer step: 1-16 at the lane's base, 17-32 at base + 60
constexpr clap_id seqStepId(int l, int i) { return 3000u + 100u * (uint32_t)l + (i < 16 ? (uint32_t)i : 60u + (uint32_t)(i - 16)); }
static const char *plockName[6] = {"Cutoff", "Reso", "Morph", "Drive", "Level", "Pan"};
static const float plockMin[6] = {-60, 0, 0, 0, -60, -1}, plockMax[6] = {82, 1, 1, 1, 12, 1}; // the strips' knob ranges
enum LaneOffset { lStep = 0, lLock = 20, lLength = 40, lRate, lEuclid, lHits, lRotate, lSmooth, lDepth, lTarget,
                  lClock, lSpeed, lSwing, lOffset, lLockTarget, lGlide, lSway, lLockLen, lLockRate, lDir };
constexpr clap_id idRetrigger = 80;
constexpr clap_id idSwayTo = 102; // 6: cutoff, reso, morph, drive, level, pan
constexpr clap_id idCondGrid = 110, idCondDivLow = 111, idCondDivHigh = 112, idCondChaos = 113, idCondRange = 114, idCondChance = 115,
                  idCondCondition = 116, idCondBursts = 117, idCondThird = 118, idCondMusical = 119, idCondSeed = 120, idJack = 121,
                  idJackPair = 122, idJackParam = 123;
// Tune (0.18)
enum TuneId : clap_id { idTuneOn = 8000, idTuneRoot, idTuneScale, idTunePlace, idTuneAmount, idTuneRing, idTuneLow, idTuneHigh,
                        idTuneTilt, idTuneDetune, idTuneWidth, idTuneMotion, idTuneMotionRate, idTuneMode,
                        idTuneSlope, idTunePivot, idTuneMotionSync, idTuneMotionDiv };
constexpr clap_id idSplitMode = 124, idSpecBy = 125, idSpecBalance = 126, idSpecSharp = 127, idSpecQuality = 128;
// Sway per band: 90 + 4 x band + {motion, rate, depth, slew}
constexpr clap_id swayBandBase(int b) { return 90u + 4u * (uint32_t)b; }
constexpr clap_id lfoBase(int i) { return 4000u + 20u * (uint32_t)i; }
enum LfoOffset { fShape = 0, fMode, fSync, fRate, fPhase, fSmooth, fPolarity, fSway, fTarget, fAmount, fStepCount, fGlide, fQuant };
constexpr clap_id lfoStepBase(int i) { return 4200u + 20u * (uint32_t)i; }
enum EnvId : clap_id { idEnvSource = 4100, idEnvSens, idEnvAttack, idEnvRelease, idScGain, idScHp, idScLp };
constexpr clap_id slotBase(int k) { return 5000u + 4u * (uint32_t)k; }
enum SlotOffset { sSource = 0, sTarget, sAmount, sSwing };

inline std::vector<std::string> targetLabels()
{
    std::vector<std::string> t{"None"};
    const char *band[3] = {"Low", "Mid", "High"}, *par[6] = {"Cutoff", "Reso", "Morph", "Drive", "Level", "Pan"};
    for (auto *b : band)
        for (auto *q : par)
            t.push_back(std::string(b) + " " + q);
    for (auto *g : {"Split Low/Mid", "Split Mid/High", "Feedback", "Noise", "Mix", "Sway", "Mod Depth", "Spectral Balance", "Spectral Sharpness",
                    "Low Mod Depth", "Mid Mod Depth", "High Mod Depth", "Tune Amount", "Tune Ring Time", "Tune Tilt", "Tune Slope"})
        t.push_back(g);
    return t;
}
enum ModeOffset { oXoverLo = 1, oXoverHi = 2, oInput = 3, oOutput = 4, oMix = 5,
                  oFeedbackOn = 6, oFeedback = 7, oFbTone = 8, oNoiseOn = 9, oNoise = 10, oNoiseColor = 11 };
enum BandOffset
{
    oModel = 1,
    oVariant,
    oCutoff,
    oReso,
    oMorph,
    oFilterOn,
    oDriveType,
    oDrive,
    oOrder,
    oLevel,
    oMute,
    oSolo,
    oPan,
    // the bank (Slink-style)
    oBankMix = 20, oBankWidth, oRipDepth, oRipRate, oRipWaves, oBankStereo, oStripMix, oModDepthB,
    oRipSync = 40, oRipSyncRate, oRipShape, oRipBoth,
    oBankGain = 30 // 8 of them
};
constexpr clap_id modeBase(int m) { return 1000u * (uint32_t)(m + 1); }
constexpr clap_id bandBase(int m, int b) { return modeBase(m) + 100u * (uint32_t)(b + 1); }

struct ParamDef
{
    clap_id id;
    std::string name, module;
    double min, max, def;
    Kind kind;
    int band{-1}, mode{-1}; // for kVariant: where its Model parameter lives
    std::vector<std::string> labels{}; // for kEnum
};

int modelIndexByName(const char *sub)
{
    const auto &cat = filterCatalogue();
    for (size_t i = 0; i < cat.size(); ++i)
        if (cat[i].name.find(sub) != std::string::npos)
            return (int)i;
    return 0;
}
// knob position (0..1) that selects the first variant of the wanted kind
double variantPosFor(int model, PassKind kind)
{
    const auto &mi = filterCatalogue()[(size_t)model];
    int n = (int)mi.configs.size();
    for (int i = 0; i < n; ++i)
        if (mi.kinds[(size_t)i] == kind)
            return n > 1 ? (double)i / (n - 1) : 0.0;
    return 0.0;
}

const std::vector<ParamDef> &paramDefs()
{
    static const std::vector<ParamDef> &defs = *new std::vector<ParamDef>([]()
    {
        std::vector<ParamDef> d;
        const int nModels = (int)filterCatalogue().size();
        const int svf = modelIndexByName("Fast SVF");
        d.push_back({idRouting, "Routing", "", 0, 1, 0, kRouting});
        d.push_back({idOversample, "Oversample", "", 0, 2, 1, kOversample});
        d.push_back({idSmart, "No Dead Bands", "", 0, 1, 1, kToggle});
        d.push_back({idAutoGain, "Auto Gain", "", 0, 1, 1, kToggle});
        d.push_back({idLimiter, "Limiter", "", 0, 1, 1, kToggle});
        d.push_back({idModDepth, "Mod Depth", "", 0, 2, 1, kPercent});
        const char *modeName[2] = {"Parallel", "Series"}, *modeShort[2] = {"Par", "Ser"};
        const char *bandName[3] = {"Low", "Mid", "High"};
        for (int m = 0; m < 2; ++m)
        {
            std::string mod = modeName[m], pre = std::string(modeShort[m]) + " ";
            if (m == Parallel)
            {
                d.push_back({modeBase(m) + oXoverLo, pre + "Split Low/Mid", mod, hzToSemi(40), hzToSemi(2000), hzToSemi(250), kPitch});
                d.push_back({modeBase(m) + oXoverHi, pre + "Split Mid/High", mod, hzToSemi(400), hzToSemi(16000), hzToSemi(3000), kPitch});
            }
            d.push_back({modeBase(m) + oInput, pre + "Input", mod, -24, 24, 0, kDb});
            d.push_back({modeBase(m) + oOutput, pre + "Output", mod, -24, 24, 0, kDb});
            d.push_back({modeBase(m) + oMix, pre + "Mix", mod, 0, 1, 1, kPercent});
            d.push_back({modeBase(m) + oFeedbackOn, pre + "Feedback On", mod, 0, 1, 0, kToggle});
            d.push_back({modeBase(m) + oFeedback, pre + "Feedback", mod, 0, 1, 0.35, kPercent});
            d.push_back({modeBase(m) + oFbTone, pre + "Feedback Tone", mod, hzToSemi(200), hzToSemi(16000), hzToSemi(6000), kHzTone});
            d.push_back({modeBase(m) + oNoiseOn, pre + "Noise On", mod, 0, 1, 0, kToggle});
            d.push_back({modeBase(m) + oNoise, pre + "Noise", mod, 0, 1, 0.3, kPercent});
            d.push_back({modeBase(m) + oNoiseColor, pre + "Noise Color", mod, 0, 1, 0, kPercent});
            for (int b = 0; b < 3; ++b)
            {
                std::string bm = mod + "/" + bandName[b], bp = pre + bandName[b] + " ";
                clap_id base = bandBase(m, b);
                // Starting point: every filter is in place and wide open, every drive is clean,
                // so the plugin loads sounding like a wire and each knob does something at once.
                // Fast SVF at 30% resonance is maximally flat; the top of the Cutoff range sits
                // above hearing at every oversampling setting (the engine caps it below Nyquist).
                int model = svf;
                PassKind kind = PassKind::LowPass;
                double cutoff = 82;
                if (m == Series && b == Low)
                    kind = PassKind::HighPass, cutoff = -60;
                d.push_back({base + oModel, bp + "Filter Type", bm, 0, (double)(nModels - 1), (double)model, kModel});
                d.push_back({base + oVariant, bp + "Filter Variant", bm, 0, 1, variantPosFor(model, kind), kVariant, b, m});
                d.push_back({base + oCutoff, bp + "Cutoff", bm, -60, 82, cutoff, kPitch});
                d.push_back({base + oReso, bp + "Resonance", bm, 0, 1, 0.3, kPercent});
                d.push_back({base + oMorph, bp + "Morph", bm, 0, 1, 0, kPercent});
                d.push_back({base + oFilterOn, bp + "Filter On", bm, 0, 1, 1, kToggle});
                d.push_back({base + oDriveType, bp + "Drive Type", bm, 0, numDriveTypes - 1, Warm, kDriveType});
                d.push_back({base + oDrive, bp + "Drive", bm, 0, 1, 0, kPercent});
                d.push_back({base + oOrder, bp + "Drive First", bm, 0, 1, 0, kOrder});
                d.push_back({base + oLevel, bp + "Level", bm, -60, 12, 0, kDb});
                d.push_back({base + oMute, bp + (m == Parallel ? "Mute" : "Bypass"), bm, 0, 1, 0, kToggle});
                if (m == Parallel)
                    d.push_back({base + oSolo, bp + "Solo", bm, 0, 1, 0, kToggle});
                d.push_back({base + oPan, bp + "Pan", bm, -1, 1, 0, kPan});
                std::string bk = bp + "Bank ";
                d.push_back({base + oBankMix, bk + "Mix", bm, 0, 1, 1, kPercent});
                d.push_back({base + oBankWidth, bk + "Width", bm, 0, 1, 0.5, kPercent});
                d.push_back({base + oRipDepth, bk + "Ripple", bm, 0, 1, 0, kPercent});
                d.push_back({base + oRipRate, bk + "Ripple Rate", bm, std::log2(0.02), std::log2(200.0), std::log2(0.5), kLogHz});
                auto enB = [&](clap_id id, const std::string &name, const std::vector<std::string> &labels, int def) {
                    ParamDef p{id, name, bm, 0, (double)labels.size() - 1, (double)def, kEnum};
                    p.labels = labels;
                    d.push_back(p);
                };
                enB(base + oRipSync, bk + "Ripple Timing", {"Free", "Sync"}, 0);
                enB(base + oRipSyncRate, bk + "Ripple Sync",
                    {"1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "1/16T", "1/8T", "1/4T", "1/8D", "1/4D"}, 4);
                enB(base + oRipShape, bk + "Ripple Shape", {"Wave", "Bounce", "Random", "Spin", "Rise", "Spotlight"}, 0);
                // which of the eight bands the ripple swings both ways (a bit each); the rest go one way
                d.push_back({base + oRipBoth, bk + "Ripple Both Ways", bm, 0, 255, 0, kInt});
                d.push_back({base + oRipWaves, bk + "Ripple Waves", bm, 0.25, 4, 1, kRatio});
                d.push_back({base + oBankStereo, bk + "Stereo", bm, 0, 1, 0, kPercent});
                d.push_back({base + oStripMix, bp + "Mix", bm, 0, 1, 1, kPercent});
                d.push_back({base + oModDepthB, bp + "Mod Depth", bm, 0, 2, 1, kPercent});
                for (int k = 0; k < numBankBands; ++k)
                    d.push_back({base + oBankGain + (clap_id)k, bk + "Band " + std::to_string(k + 1), bm, -1, 1, 0, kBipolar});
            }
        }
        // Shared by every routing: Sway, the Conductor, Mutation
        auto en = [&d](clap_id id, const char *name, const char *mod, const std::vector<std::string> &labels, int def)
        {
            ParamDef p{id, name, mod, 0, (double)labels.size() - 1, (double)def, kEnum};
            p.labels = labels;
            d.push_back(std::move(p));
        };
        d.push_back({idSwayAmount, "Sway", "Sway", 0, 1, 0, kPercent});
        d.push_back({idSwayRate, "Sway Rate", "Sway", std::log2(0.01), std::log2(10.0), std::log2(0.12), kLogHz});
        d.push_back({idSwaySync, "Sway Sync", "Sway", 0, 1, 0, kToggle});
        en(idSwayBars, "Sway Sync Length", "Sway", {"1/4 bar", "1/2 bar", "1 bar", "2 bars", "4 bars", "8 bars", "16 bars"}, 4);
        en(idSwayMotion, "Sway Motion", "Sway", {"Smooth", "Stepped"}, 0);
        d.push_back({idSwaySlew, "Sway Slew", "Sway", 0, 1, 0.4, kPercent});
        d.push_back({idSwayFollow, "Sway Follow", "Sway", 0, 1, 0, kPercent});
        d.push_back({idSwayDepth, "Sway Depth", "Sway", 0, 48, 12, kSemis});
        for (int b = 0; b < 3; ++b)
        {
            std::string pre = std::string("Sway ") + bandName[b] + " ";
            en(swayBandBase(b), (pre + "Motion").c_str(), "Sway", {"Main", "Smooth", "Stepped"}, 0);
            d.push_back({swayBandBase(b) + 1, pre + "Rate", "Sway", 0.25, 4, 1, kRatio});
            d.push_back({swayBandBase(b) + 2, pre + "Depth", "Sway", 0, 1, 1, kPercent});
            d.push_back({swayBandBase(b) + 3, pre + "Slew", "Sway", 0, 1, 0.4, kPercent});
        }
        {
            static const char *dst[6] = {"Cutoff", "Reso", "Morph", "Drive", "Level", "Pan"};
            for (int k = 0; k < 6; ++k)
                d.push_back({idSwayTo + (clap_id)k, std::string("Sway to ") + dst[k], "Sway", 0, 1, k == 0 ? 1.0 : 0.0, kPercent});
        }
        en(idCondGrid, "Chaperone Grid", "Chaperone", {"Smooth", "1/64", "1/32T", "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1 bar", "2 bars", "4 bars"}, 0);
        d.push_back({idCondDivLow, "Chaperone Low Every", "Chaperone", 1, 16, 1, kInt});
        d.push_back({idCondDivHigh, "Chaperone High Every", "Chaperone", 1, 16, 1, kInt});
        d.push_back({idCondChaos, "Chaperone Chaos", "Chaperone", 0, 1, 0, kPercent});
        d.push_back({idCondRange, "Chaperone Range", "Chaperone", 0, 48, 12, kSemis});
        d.push_back({idCondChance, "Chaperone Chance", "Chaperone", 0, 1, 1, kPercent});
        en(idCondCondition, "Chaperone Bars", "Chaperone", {"Every bar", "1:2", "2:2", "1:3", "2:3", "3:3", "1:4", "2:4", "3:4", "4:4", "1:8", "8:8"}, 0);
        d.push_back({idCondBursts, "Chaperone Bursts", "Chaperone", 0, 1, 0, kPercent});
        d.push_back({idCondThird, "3rd Wheel Mischief", "Chaperone", 0, 1, 0, kPercent});
        en(idCondMusical, "Chaperone Snap", "Chaperone", {"Free", "Semitones", "Fifths", "Octaves"}, 0);
        d.push_back({idCondSeed, "Chaperone Seed", "Chaperone", 0, 999, 1, kInt});
        d.push_back({idJack, "3rd Wheel", "Chaperone", 0, 1, 0, kPercent});
        en(idJackPair, "3rd Wheel Pair", "Chaperone", {"Low <> High", "Mid vs both", "Low > High", "High > Low", "Mid > Low", "Mid > High"}, 0);
        en(idJackParam, "3rd Wheel Moves", "Chaperone", {"Cutoff", "Reso", "Morph", "Drive", "Level", "Pan"}, 0);
        // Parallel's split: crossover filters or the spectral (FFT) split
        en(idSplitMode, "Parallel Split", "Spectral", {"Crossover", "Spectral"}, 0);
        en(idSpecBy, "Spectral Split By", "Spectral", {"Frequency", "Tonal"}, 0);
        d.push_back({idSpecBalance, "Spectral Balance", "Spectral", -1, 1, 0, kBipolar});
        d.push_back({idSpecSharp, "Spectral Sharpness", "Spectral", 0, 1, 0.6, kPercent});
        en(idSpecQuality, "Spectral Quality", "Spectral", {"Fast", "Normal", "High", "Ultra"}, 1);
        d.push_back({idInfluence, "Chaperone Influence", "Chaperone", 0, 1, 0, kPercent});
        d.push_back({idLean, "Chaperone Lean", "Chaperone", 0, 1, 0.5, kLean});
        en(idMutTrigger, "Mutation Trigger", "Mutation", {"Mid gets louder", "Mid filter jumps", "Mid too hot", "Mid's sequencer", "Follower hit", "Chaperone moves"}, 0);
        d.push_back({idMutSens, "Mutation Sensitivity", "Mutation", 0, 1, 0.5, kPercent});
        en(idMutHold, "Mutation Hold-off", "Mutation", {"none", "1 step", "2 steps", "3 steps", "4 steps", "6 steps", "8 steps"}, 1);
        for (int r = 0; r < 2; ++r)
        {
            clap_id b = r == 0 ? idMutLow : idMutHigh;
            std::string pre = r == 0 ? "Mutation Low " : "Mutation High ", mod = r == 0 ? "Mutation/Low" : "Mutation/High";
            en(b + rMode, (pre + "Answer").c_str(), mod.c_str(), {"Off", "Same way", "Opposite way"}, r == 0 ? 1 : 2);
            en(b + rTarget, (pre + "Moves").c_str(), mod.c_str(), {"Cutoff", "Reso", "Drive", "Level", "Pan"}, r == 0 ? 0 : 0);
            d.push_back({b + rAmount, pre + "Amount", mod, 0, 1, 0.4, kPercent});
            d.push_back({b + rChance, pre + "Chance", mod, 0, 1, 1, kPercent});
            en(b + rDelay, (pre + "Delay").c_str(), mod.c_str(), {"none", "1 step", "2 steps", "3 steps", "4 steps"}, r == 0 ? 0 : 1);
        }
        d.push_back({idMutate, "Mutate", "Mutation", 0, 1, 0, kPercent});
        en(idMutEvery, "Mutate Every", "Mutation", {"1 bar", "2 bars", "4 bars", "8 bars", "16 bars"}, 2);
        d.push_back({idMutFreeze, "Mutate Freeze", "Mutation", 0, 1, 0, kToggle});
        d.push_back({idMutSeed, "Mutate Seed", "Mutation", 0, 999, 0, kInt});
        // ----- Phase 3: sequencer lanes with their lock lanes
        const auto tl = targetLabels();
        const char *laneName[3] = {"Low", "Mid", "High"};
        const int laneDefTarget[3] = {bandTarget(Low, PDrive), bandTarget(Mid, PCut), bandTarget(High, PLevel)};
        for (int l = 0; l < 3; ++l)
        {
            clap_id b = laneBase(l);
            std::string pre = std::string("Seq ") + laneName[l] + " ", mod = std::string("Sequencer/") + laneName[l];
            for (int i = 0; i < numSteps; ++i)
                d.push_back({seqStepId(l, i), pre + "Step " + std::to_string(i + 1), mod, -1, 1, 0, kBipolar});
            // Elektron-style locks: each step can lock each of the band's six knobs.
            // -1 = not locked, 0..1 = where the knob is locked (across its range)
            for (int k = 0; k < (int)numBandParams; ++k)
                for (int i = 0; i < numSteps; ++i)
                    d.push_back({plockId(l, k, i), std::string("Lock ") + laneName[l] + " " + plockName[k] + " " + std::to_string(i + 1),
                                 std::string("P-Locks/") + laneName[l], -1, 1, -1, kPLock});
            d.push_back({b + lLength, pre + "Length", mod, 1, 32, 16, kInt});
            en(b + lRate, (pre + "Rate").c_str(), mod.c_str(), {"1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1 bar", "1/64", "1/32T", "1/16D", "1/8D", "1/4D", "1/2D", "1/1T", "1/32D"}, 2);
            en(b + lEuclid, (pre + "Euclid").c_str(), mod.c_str(), {"Off", "Gate", "Trig"}, 0);
            d.push_back({b + lHits, pre + "Hits", mod, 0, 32, 4, kInt});
            d.push_back({b + lRotate, pre + "Rotate", mod, 0, 31, 0, kInt});
            d.push_back({b + lSmooth, pre + "Smooth", mod, 0, 1, 0, kPercent});
            d.push_back({b + lDepth, pre + "Depth", mod, 0, 1, 1, kPercent});
            en(b + lTarget, (pre + "Target").c_str(), mod.c_str(), tl, laneDefTarget[l]);
            en(b + lClock, (pre + "Timing").c_str(), mod.c_str(), {"Clock", "Free"}, 0);
            d.push_back({b + lSpeed, pre + "Speed", mod, std::log2(0.1), std::log2(500.0), std::log2(4.0), kLogHz});
            d.push_back({b + lSwing, pre + "Swing", mod, 0.5, 0.75, 0.5, kPercent});
            d.push_back({b + lOffset, pre + "Offset", mod, -0.5, 0.5, 0, kOffset});
            d.push_back({b + lGlide, std::string("Lock ") + laneName[l] + " Glide", std::string("P-Locks/") + laneName[l], 0.2, 500, 2, kMs});
            d.push_back({b + lSway, pre + "Sway", mod, 0, 1, 0, kPercent});
            en(b + lDir, (pre + "Direction").c_str(), mod.c_str(), {"Forward", "Reverse", "Ping-pong", "Random"}, 0);
            {
                std::vector<std::string> ll{"Seq"};
                for (int i = 1; i <= numSteps; ++i)
                    ll.push_back(std::to_string(i));
                en(b + lLockLen, (std::string("Lock ") + laneName[l] + " Length").c_str(), (std::string("P-Locks/") + laneName[l]).c_str(), ll, 0);
                en(b + lLockRate, (std::string("Lock ") + laneName[l] + " Rate").c_str(), (std::string("P-Locks/") + laneName[l]).c_str(),
                   {"Seq", "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1 bar", "1/64", "1/32T", "1/16D", "1/8D", "1/4D", "1/2D", "1/1T", "1/32D"}, 0);
            }
        }
        en(idRetrigger, "Retrigger", "Sequencer", {"Song position", "Every bar", "Every 2 bars", "Every 4 bars", "On start"}, 0);
        // ----- LFOs
        for (int i = 0; i < numLfos; ++i)
        {
            clap_id b = lfoBase(i);
            std::string pre = "LFO " + std::to_string(i + 1) + " ", mod = "LFO " + std::to_string(i + 1);
            en(b + fShape, (pre + "Shape").c_str(), mod.c_str(), {"Drawn", "Sine", "Triangle", "Ramp", "Square", "Pump", "Swell", "Bounce", "Stairs", "S&H", "Steps"}, i == 0 ? 0 : (i == 1 ? 5 : 1));
            en(b + fMode, (pre + "Timing").c_str(), mod.c_str(), {"Sync", "Free", "Retrig"}, 0);
            en(b + fSync, (pre + "Sync Rate").c_str(), mod.c_str(), {"1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "8 bars"}, 4);
            d.push_back({b + fRate, pre + "Rate", mod, std::log2(0.01), std::log2(500.0), 0.0, kLogHz});
            d.push_back({b + fPhase, pre + "Phase", mod, 0, 1, 0, kPercent});
            d.push_back({b + fSmooth, pre + "Smooth", mod, 0, 1, 0, kPercent});
            en(b + fPolarity, (pre + "Output").c_str(), mod.c_str(), {"Bipolar", "Unipolar"}, 0);
            d.push_back({b + fSway, pre + "Sway", mod, 0, 1, 0, kPercent});
            en(b + fTarget, (pre + "Target").c_str(), mod.c_str(), tl, 0);
            d.push_back({b + fAmount, pre + "Amount", mod, -1, 1, 0.5, kBipolar});
            d.push_back({b + fStepCount, pre + "Steps", mod, 1, 16, 8, kInt});
            d.push_back({b + fGlide, pre + "Glide", mod, 0, 1, 0, kPercent});
            en(b + fQuant, (pre + "Scale").c_str(), mod.c_str(), {"Free", "Semitones", "Major", "Minor", "Pentatonic", "Octaves"}, 0);
            for (int k = 0; k < 16; ++k)
                d.push_back({lfoStepBase(i) + (clap_id)k, pre + "Step " + std::to_string(k + 1), mod, -1, 1, 0, kBipolar});
        }
        // ----- envelope follower
        en(idEnvSource, "Follower Listens To", "Follower", {"Input", "Sidechain", "Low band", "Mid band", "High band"}, 0);
        d.push_back({idEnvSens, "Follower Sens", "Follower", -24, 36, 6, kDb});
        d.push_back({idEnvAttack, "Follower Attack", "Follower", 0.1, 100, 5, kMs});
        d.push_back({idEnvRelease, "Follower Release", "Follower", 1, 2000, 150, kMs});
        d.push_back({idScGain, "Sidechain Gain", "Follower", -24, 24, 0, kDb});
        d.push_back({idScHp, "Sidechain High-pass", "Follower", hzToSemi(10), hzToSemi(5000), hzToSemi(20), kHzTone});
        d.push_back({idScLp, "Sidechain Low-pass", "Follower", hzToSemi(40), hzToSemi(20000), hzToSemi(20000), kHzTone});
        // ----- mod matrix
        for (int k = 0; k < numSlots; ++k)
        {
            clap_id b = slotBase(k);
            std::string pre = "Mod " + std::to_string(k + 1) + " ";
            en(b + sSource, (pre + "Source").c_str(), "Mod matrix", {"None", "Seq Low", "Seq Mid", "Seq High", "LFO 1", "LFO 2", "LFO 3", "Follower", "Ripple Low", "Ripple Mid", "Ripple High"}, 0);
            en(b + sTarget, (pre + "Target").c_str(), "Mod matrix", tl, 0);
            d.push_back({b + sAmount, pre + "Amount", "Mod matrix", -1, 1, 0, kBipolar});
            // new slots go one way (0.18); older projects and presets keep "As source" (see loadStateText)
            en(b + sSwing, (pre + "Swing").c_str(), "Mod matrix", {"As source", "Both ways", "One way"}, 2);
        }
        // ----- Tune
        d.push_back({idTuneOn, "Tune On", "Tune", 0, 1, 0, kToggle});
        en(idTuneRoot, "Tune Root", "Tune", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 9);
        en(idTuneScale, "Tune Scale", "Tune", {"Octaves", "Fifths up", "Fifths down", "Major 3rds up", "Major 3rds down", "Minor 3rds up", "Minor 3rds down",
                                               "Major chord", "Minor chord", "Major", "Minor", "Dorian", "Mixolydian", "Pentatonic", "Blues", "Whole tone", "Chromatic"}, 10);
        en(idTuneMode, "Tune Mode", "Tune", {"Layer", "Blend"}, 0);
        en(idTunePlace, "Tune Place", "Tune", {"Before strips", "After strips"}, 1);
        d.push_back({idTuneAmount, "Tune Amount", "Tune", 0, 1, 0.4, kPercent});
        d.push_back({idTuneRing, "Tune Ring Time", "Tune", std::log2(20.0), std::log2(4000.0), std::log2(400.0), kLogMs});
        d.push_back({idTuneLow, "Tune Lowest", "Tune", 24, 108, 36, kNote});
        d.push_back({idTuneHigh, "Tune Highest", "Tune", 24, 108, 96, kNote});
        d.push_back({idTuneTilt, "Tune Tilt", "Tune", -1, 1, 0, kBipolar});
        d.push_back({idTuneDetune, "Tune Detune", "Tune", 0, 1, 0, kPercent});
        d.push_back({idTuneWidth, "Tune Width", "Tune", 0, 1, 0.5, kPercent});
        en(idTuneMotion, "Tune Motion", "Tune", {"Still", "Wave", "Drift"}, 0);
        d.push_back({idTuneMotionRate, "Tune Motion Rate", "Tune", std::log2(0.02), std::log2(4.0), std::log2(0.25), kLogHz});
        en(idTuneMotionSync, "Tune Motion Timing", "Tune", {"Free", "Sync"}, 0);
        en(idTuneMotionDiv, "Tune Motion Sync", "Tune", {"1/16", "1/8T", "1/8", "1/4T", "1/8D", "1/4", "1/2T", "1/4D", "1/2", "1 bar", "2 bars", "4 bars", "8 bars", "16 bars"}, 9);
        d.push_back({idTuneSlope, "Tune Slope", "Tune", -1, 1, 0, kBipolar});
        d.push_back({idTunePivot, "Tune Slope Pivot", "Tune", std::log2(100.0), std::log2(8000.0), std::log2(1000.0), kLogHz});
        return d;
    }());
    return defs;
}

const std::unordered_map<clap_id, int> &paramIndex()
{
    static const std::unordered_map<clap_id, int> &ix = *new std::unordered_map<clap_id, int>([]()
    {
        std::unordered_map<clap_id, int> r;
        const auto &d = paramDefs();
        for (size_t i = 0; i < d.size(); ++i)
            r[d[i].id] = (int)i;
        return r;
    }());
    return ix;
}

bool isStepped(Kind k)
{
    return k != kPercent && k != kDb && k != kPitch && k != kVariant && k != kPan && k != kLean && k != kLogHz &&
           k != kSemis && k != kHzTone && k != kBipolar && k != kLock && k != kOffset && k != kMs && k != kRatio && k != kPLock && k != kLogMs;
}

// ---------------------------------------------------------------------------
struct Plugin
{
    clap_plugin_t clap{};
    const clap_host_t *host{nullptr};
    std::atomic<int> latencyReported{0};
    std::atomic<bool> restartAsked{false};
    Engine engine;
    Settings settings;
    std::vector<std::atomic<float>> value; // what the knobs say
    std::vector<float> mod;                // host modulation on top (audio thread only)
    std::atomic<bool> dirty{true};
    std::vector<float> bufL, bufR, scBufL, scBufR;
    double sampleRate{48000};

    // what the window shows besides the knobs, copied from the engine after every block
    enum Meter
    {
        mBand = 0,                          // 3 x 6: modulated cutoff, reso, morph, drive, level, pan
        mGlobal = mBand + 18,               // 6: split lo, split hi, feedback, noise, mix, sway
        mLaneStep = mGlobal + 6,            // 3
        mLaneVal = mLaneStep + 3,           // 3
        mLfoVal = mLaneVal + 3,             // 3
        mLfoPhase = mLfoVal + 3,            // 3
        mEnv = mLfoPhase + 3, mSc, mLimiter, mOutGuard,
        mGuard,                             // 3
        mEdge = mGuard + 3,                 // 3
        mSway = mEdge + 3,                  // 3
        mMutEvents = mSway + 3,
        mHist,                              // 16 x 3
        mLockStep = mHist + 48,             // 3
        mBank = mLockStep + 3,              // 3 x 8 gains now
        mBankHz = mBank + 24,               // 3 x 8 centres
        mChap = mBankHz + 24,               // Low, High offsets from the Chaperone (st), its step count
        mTunePos = mChap + 3,               // Tune Motion's position (cycles)
        mMutNow,                            // 2 x 5: Low and High answers after Mutate: mode, moves, amount, chance, delay
        mMutK = mMutNow + 10,               // how many times Mutate has changed them, and how far into the period
        mMutFrac,
        numMeters
    };
    std::atomic<float> meter[numMeters];

    // the drawn curves (main thread), saved with the project
    std::vector<CurvePoint> curve[numLfos];

    // changes made in the window, on their way to the host (window -> audio thread)
    struct GuiEvent
    {
        uint16_t type; // 0 value, 1 gesture begin, 2 gesture end
        clap_id id;
        double value;
    };
    static constexpr size_t qSize = 1024;
    GuiEvent q[qSize];
    std::atomic<size_t> qHead{0}, qTail{0};
    void push(uint16_t type, clap_id id, double v)
    {
        size_t h = qHead.load(std::memory_order_relaxed), n = (h + 1) % qSize;
        if (n == qTail.load(std::memory_order_acquire))
            return; // full: the value itself is already stored, only the host's note of it is lost
        q[h] = {type, id, v};
        qHead.store(n, std::memory_order_release);
    }
    void sendGuiEvents(const clap_output_events_t *out)
    {
        size_t t = qTail.load(std::memory_order_relaxed);
        while (t != qHead.load(std::memory_order_acquire))
        {
            const auto &e = q[t];
            if (out)
            {
                if (e.type == 0)
                {
                    clap_event_param_value_t ev{};
                    ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
                    ev.param_id = e.id;
                    ev.note_id = -1, ev.port_index = -1, ev.channel = -1, ev.key = -1;
                    ev.value = e.value;
                    out->try_push(out, &ev.header);
                }
                else
                {
                    clap_event_param_gesture_t ev{};
                    ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID,
                                 (uint16_t)(e.type == 1 ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END), 0};
                    ev.param_id = e.id;
                    out->try_push(out, &ev.header);
                }
            }
            t = (t + 1) % qSize;
        }
        qTail.store(t, std::memory_order_release);
    }

    struct GuiBridge;
    GuiBridge *bridge{nullptr};
    const clap_host_params_t *hostParams{nullptr};
    std::atomic<bool> darkTheme{false}; // the window's look, saved with the project
    const clap_host_state_t *hostState{nullptr};
    void guiNeedsInit();
    void requestFlush();
    void markDirty();
    GuiBridge *ensureBridge();
    void deleteBridge();
    void *gui{nullptr};
    uint32_t guiW{1280}, guiH{940};
    uint32_t maxBlock{0};
    double guiScale{1.0}; // Windows: the screen scale the host reported (sizes to the host are in pixels)

    Plugin() : value(paramDefs().size()), mod(paramDefs().size(), 0.f)
    {
        const auto &d = paramDefs();
        for (size_t i = 0; i < d.size(); ++i)
            value[i].store((float)d[i].def, std::memory_order_relaxed);
        for (auto &m : meter)
            m.store(0.f, std::memory_order_relaxed);
        for (int i = 0; i < numLfos; ++i)
            curve[i] = defaultCurve();
        // a new copy opens with this computer's preferred theme and size (a project's own state replaces them)
        Prefs pr = loadPrefs();
        darkTheme.store(pr.dark);
        guiW = (uint32_t)std::lround(1280 * pr.sizePct / 100.0), guiH = (uint32_t)std::lround(940 * pr.sizePct / 100.0);
    }
    static std::vector<CurvePoint> defaultCurve()
    {
        return {{0, 0, 1}, {0.12f, 0.9f, 0.5f}, {0.3f, 0.2f, 2}, {0.42f, 0.6f, 1}, {0.62f, -0.8f, 0.45f}, {0.8f, -0.2f, 2.2f}, {1, 0, 1}};
    }
    void applyCurve(int i)
    {
        engine.setCustomCurve(i, curve[i].data(), (int)curve[i].size());
    }

    void readMeters()
    {
        auto st = [this](int k, float v) { meter[k].store(v, std::memory_order_relaxed); };
        for (int b = 0; b < 3; ++b)
            for (int q2 = 0; q2 < 6; ++q2)
                st(mBand + b * 6 + q2, engine.displayBand(b, q2));
        for (int g = 0; g < 6; ++g)
            st(mGlobal + g, engine.displayGlobal(TgtSplitLo + g));
        for (int l = 0; l < 3; ++l)
            st(mLaneStep + l, (float)engine.laneStep(l)), st(mLaneVal + l, engine.laneValue(l)), st(mLockStep + l, (float)engine.lockStep(l));
        for (int i = 0; i < 3; ++i)
            st(mLfoVal + i, engine.lfoValue(i)), st(mLfoPhase + i, engine.lfoPhase(i));
        st(mEnv, engine.envValue());
        st(mSc, engine.sidechainLevel());
        st(mLimiter, engine.limiterGain());
        st(mOutGuard, engine.outputGuardGain());
        for (int b = 0; b < 3; ++b)
            st(mGuard + b, engine.bandGuardGain(b)), st(mEdge + b, engine.edgePress(b)), st(mSway + b, engine.swayValue(b));
        st(mMutEvents, (float)engine.mutationEvents());
        st(mTunePos, (float)std::fmod(engine.tuneMotionPos(), 4096.0));
        for (int r = 0; r < 2; ++r)
        {
            const auto &A = engine.mutationAnswer(r);
            st(mMutNow + r * 5, (float)A.mode), st(mMutNow + r * 5 + 1, (float)A.target), st(mMutNow + r * 5 + 2, A.amount);
            st(mMutNow + r * 5 + 3, A.chance), st(mMutNow + r * 5 + 4, (float)A.delaySteps);
        }
        st(mMutK, (float)std::max(0L, engine.mutationPeriod())), st(mMutFrac, engine.mutationPeriodFrac());
        st(mChap, engine.conductorPassed(0)), st(mChap + 1, engine.conductorPassed(2)), st(mChap + 2, (float)engine.chaperoneSteps());
        for (int b = 0; b < 3; ++b)
            for (int k = 0; k < numBankBands; ++k)
                st(mBank + b * 8 + k, engine.bankGainNow(b, k)), st(mBankHz + b * 8 + k, engine.bankHz(b, k));
        for (int k = 0; k < 16; ++k)
        {
            auto ev = engine.mutationHistory(k);
            st(mHist + k * 3, (float)ev.dir), st(mHist + k * 3 + 1, (float)ev.low), st(mHist + k * 3 + 2, (float)ev.high);
        }
    }

    float val(clap_id id, bool withMod = true) const
    {
        auto it = paramIndex().find(id);
        if (it == paramIndex().end())
            return 0.f;
        const auto &d = paramDefs()[(size_t)it->second];
        float v = value[(size_t)it->second].load(std::memory_order_relaxed);
        if (withMod)
            v += mod[(size_t)it->second];
        return std::clamp(v, (float)d.min, (float)d.max);
    }
    bool on(clap_id id) const { return val(id) >= 0.5f; }
    int step(clap_id id) const { return (int)std::lround(val(id)); }

    void buildSettings()
    {
        Settings &s = settings;
        s.routing = step(idRouting);
        s.splitMode = step(idSplitMode);
        s.specBy = step(idSpecBy);
        s.specBalance = val(idSpecBalance);
        s.specSharp = val(idSpecSharp);
        s.specQuality = step(idSpecQuality);
        s.oversample = step(idOversample);
        s.modDepth = val(idModDepth);
        s.smartEdges = on(idSmart);
        s.autoGain = on(idAutoGain);
        s.limiter = on(idLimiter);
        for (int m = 0; m < 2; ++m)
        {
            auto &M = s.mode[m];
            if (m == Parallel)
            {
                M.xoverLoHz = (float)semiToHz(val(modeBase(m) + oXoverLo));
                M.xoverHiHz = (float)semiToHz(val(modeBase(m) + oXoverHi));
            }
            M.inputDb = val(modeBase(m) + oInput);
            M.outputDb = val(modeBase(m) + oOutput);
            M.mix = val(modeBase(m) + oMix);
            for (int b = 0; b < 3; ++b)
            {
                auto &B = M.band[b];
                clap_id base = bandBase(m, b);
                B.model = step(base + oModel);
                B.variant = variantIndexFor(B.model, val(base + oVariant));
                B.cutoff = val(base + oCutoff);
                B.resonance = val(base + oReso);
                B.morph = val(base + oMorph);
                B.filterOn = on(base + oFilterOn);
                B.driveType = step(base + oDriveType);
                B.drive = val(base + oDrive);
                B.driveFirst = on(base + oOrder);
                float lv = val(base + oLevel);
                B.levelDb = lv <= -59.9f ? -60.f : lv;
                B.mute = on(base + oMute);
                B.solo = m == Parallel && on(base + oSolo);
                B.pan = val(base + oPan);
                B.bankMix = val(base + oBankMix);
                B.bankWidth = val(base + oBankWidth);
                B.rippleDepth = val(base + oRipDepth);
                B.rippleRateHz = std::exp2(val(base + oRipRate));
                B.rippleWaves = val(base + oRipWaves);
                B.bankStereo = val(base + oBankStereo);
                B.rippleSync = step(base + oRipSync) == 1;
                B.rippleSyncIndex = step(base + oRipSyncRate);
                B.rippleShape = step(base + oRipShape);
                B.rippleBoth = step(base + oRipBoth);
                B.stripMix = val(base + oStripMix);
                B.modDepth = val(base + oModDepthB);
                for (int k = 0; k < numBankBands; ++k)
                    B.bankGain[k] = val(base + oBankGain + (clap_id)k);
            }
            M.feedbackOn = on(modeBase(m) + oFeedbackOn);
            M.feedback = val(modeBase(m) + oFeedback);
            M.feedbackToneHz = (float)semiToHz(val(modeBase(m) + oFbTone));
            M.noiseOn = on(modeBase(m) + oNoiseOn);
            M.noise = val(modeBase(m) + oNoise);
            M.noiseColor = val(modeBase(m) + oNoiseColor);
        }
        auto &W = s.sway;
        W.amount = val(idSwayAmount);
        W.rateHz = std::exp2(val(idSwayRate));
        W.sync = on(idSwaySync);
        W.syncIndex = step(idSwayBars);
        W.stepped = step(idSwayMotion) == 1;
        W.slew = val(idSwaySlew);
        W.follow = val(idSwayFollow);
        W.depthSemis = val(idSwayDepth);
        W.toCut = val(idSwayTo), W.toRes = val(idSwayTo + 1), W.toMorph = val(idSwayTo + 2), W.toDrive = val(idSwayTo + 3), W.toLevel = val(idSwayTo + 4),
        W.toPan = val(idSwayTo + 5);
        for (int b = 0; b < 3; ++b)
        {
            W.band[b].motion = step(swayBandBase(b));
            W.band[b].rateMul = val(swayBandBase(b) + 1);
            W.band[b].depth = val(swayBandBase(b) + 2);
            W.band[b].slew = val(swayBandBase(b) + 3);
        }
        s.conductor.influence = val(idInfluence);
        {
            auto &C = s.conductor;
            C.grid = step(idCondGrid);
            C.divLow = step(idCondDivLow), C.divHigh = step(idCondDivHigh);
            C.chaos = val(idCondChaos), C.range = val(idCondRange), C.chance = val(idCondChance);
            C.condition = step(idCondCondition);
            C.bursts = val(idCondBursts), C.mischief = val(idCondThird);
            C.musical = step(idCondMusical), C.seed = step(idCondSeed);
            C.jack = val(idJack), C.jackPair = step(idJackPair), C.jackParam = step(idJackParam);
        }
        s.conductor.lean = val(idLean);
        auto &Mu = s.mutation;
        Mu.trigger = step(idMutTrigger);
        Mu.sensitivity = val(idMutSens);
        static constexpr int holdSteps[7] = {0, 1, 2, 3, 4, 6, 8};
        Mu.holdSteps = holdSteps[std::clamp(step(idMutHold), 0, 6)];
        for (int r = 0; r < 2; ++r)
        {
            clap_id b = r == 0 ? idMutLow : idMutHigh;
            Mu.resp[r] = {step(b + rMode), step(b + rTarget), val(b + rAmount), val(b + rChance), step(b + rDelay)};
        }
        Mu.mutate = val(idMutate);
        Mu.everyIndex = step(idMutEvery);
        Mu.freeze = on(idMutFreeze);
        Mu.seed = step(idMutSeed);
        for (int l = 0; l < numLanes; ++l)
        {
            auto &L = s.lane[l];
            clap_id b = laneBase(l);
            for (int i = 0; i < numSteps; ++i)
                L.step[i] = val(seqStepId(l, i));
            for (int k = 0; k < (int)numBandParams; ++k)
                for (int i = 0; i < numSteps; ++i)
                {
                    float v = val(plockId(l, k, i));
                    L.plock[k][i] = v < 0.f ? std::numeric_limits<float>::quiet_NaN() : plockMin[k] + std::clamp(v, 0.f, 1.f) * (plockMax[k] - plockMin[k]);
                }
            L.length = step(b + lLength);
            L.rateIndex = step(b + lRate);
            L.euclid = step(b + lEuclid);
            L.hits = step(b + lHits);
            L.rotate = step(b + lRotate);
            L.smooth = val(b + lSmooth);
            L.depth = val(b + lDepth);
            L.target = step(b + lTarget);
            L.freeRun = step(b + lClock) == 1;
            L.speedHz = std::exp2(val(b + lSpeed));
            L.swing = val(b + lSwing);
            L.offset = val(b + lOffset);
            L.lockGlideMs = val(b + lGlide);
            L.sway = val(b + lSway);
            L.lockLength = step(b + lLockLen);
            L.direction = step(b + lDir);
            L.lockRateIndex = step(b + lLockRate);
        }
        s.retrigger = step(idRetrigger);
        for (int i = 0; i < numLfos; ++i)
        {
            auto &F = s.lfo[i];
            clap_id b = lfoBase(i);
            F.shape = step(b + fShape);
            F.mode = step(b + fMode);
            F.syncIndex = step(b + fSync);
            F.rateHz = std::exp2(val(b + fRate));
            F.phase = val(b + fPhase);
            F.smooth = val(b + fSmooth);
            F.unipolar = step(b + fPolarity) == 1;
            F.sway = val(b + fSway);
            F.target = step(b + fTarget);
            F.amount = val(b + fAmount);
            F.stepCount = step(b + fStepCount);
            F.glide = val(b + fGlide);
            F.quantize = step(b + fQuant);
            for (int k = 0; k < 16; ++k)
                F.step[k] = val(lfoStepBase(i) + (clap_id)k);
        }
        auto &E = s.env;
        E.source = step(idEnvSource);
        E.sensDb = val(idEnvSens);
        E.attackMs = val(idEnvAttack);
        E.releaseMs = val(idEnvRelease);
        E.scGainDb = val(idScGain);
        E.scHpHz = (float)semiToHz(val(idScHp));
        E.scLpHz = (float)semiToHz(val(idScLp));
        for (int k = 0; k < numSlots; ++k)
            s.slot[k] = {step(slotBase(k) + sSource), step(slotBase(k) + sTarget), val(slotBase(k) + sAmount), step(slotBase(k) + sSwing)};
        auto &T = s.tune;
        T.on = step(idTuneOn) != 0, T.root = step(idTuneRoot), T.scale = step(idTuneScale), T.beforeStrips = step(idTunePlace) == 0;
        T.amount = val(idTuneAmount), T.ringMs = std::exp2(val(idTuneRing)), T.lowest = step(idTuneLow), T.highest = step(idTuneHigh);
        T.tilt = val(idTuneTilt), T.detuneCents = 50.f * val(idTuneDetune), T.width = val(idTuneWidth), T.motion = step(idTuneMotion);
        T.motionHz = std::exp2(val(idTuneMotionRate));
        T.layer = step(idTuneMode) == 0;
        T.motionSync = step(idTuneMotionSync) == 1, T.motionDiv = step(idTuneMotionDiv);
        T.slope = val(idTuneSlope), T.pivotHz = (float)std::exp2(val(idTunePivot));
    }

    void handleEvent(const clap_event_header_t *h)
    {
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;
        if (h->type == CLAP_EVENT_PARAM_VALUE)
        {
            auto *e = (const clap_event_param_value_t *)h;
            auto it = paramIndex().find(e->param_id);
            if (it == paramIndex().end())
                return;
            const auto &d = paramDefs()[(size_t)it->second];
            value[(size_t)it->second].store((float)std::clamp(e->value, d.min, d.max), std::memory_order_relaxed);
            dirty.store(true, std::memory_order_relaxed);
        }
        else if (h->type == CLAP_EVENT_PARAM_MOD)
        {
            auto *e = (const clap_event_param_mod_t *)h;
            auto it = paramIndex().find(e->param_id);
            if (it == paramIndex().end() || e->note_id >= 0)
                return;
            mod[(size_t)it->second] = (float)e->amount;
            dirty.store(true, std::memory_order_relaxed);
        }
    }
};

Plugin *self(const clap_plugin_t *p) { return (Plugin *)p->plugin_data; }

// ----- text for values -------------------------------------------------------
std::string pitchText(double st)
{
    double hz = semiToHz(st);
    char b[64];
    if (hz >= 10000)
        snprintf(b, sizeof(b), "%.1f kHz", hz / 1000);
    else if (hz >= 1000)
        snprintf(b, sizeof(b), "%.2f kHz", hz / 1000);
    else if (hz >= 100)
        snprintf(b, sizeof(b), "%.0f Hz", hz);
    else
        snprintf(b, sizeof(b), "%.1f Hz", hz);
    return b;
}

std::string valueText(const Plugin *pl, const ParamDef &d, double v)
{
    char b[96];
    switch (d.kind)
    {
    case kPercent:
        snprintf(b, sizeof(b), d.max - d.min < 0.5 ? "%.1f %%" : "%.0f %%", v * 100); // Swing (50-75%) gets a decimal
        return b;
    case kDb:
        if (d.min <= -60 && v <= -59.9)
            return "Off";
        snprintf(b, sizeof(b), "%+.1f dB", v);
        return b;
    case kPitch:
        return pitchText(v);
    case kToggle:
        return v >= 0.5 ? "On" : "Off";
    case kRouting:
        return v >= 0.5 ? "Series" : "Parallel";
    case kOversample:
    {
        int i = (int)std::lround(v);
        return i <= 0 ? "Off" : i == 1 ? "2x" : "4x";
    }
    case kOrder:
        return v >= 0.5 ? "Drive > Filter" : "Filter > Drive";
    case kDriveType:
        return driveName((int)std::lround(v));
    case kModel:
    {
        const auto &cat = filterCatalogue();
        return cat[(size_t)std::clamp((int)std::lround(v), 0, (int)cat.size() - 1)].name;
    }
    case kEnum:
        return d.labels[(size_t)std::clamp((int)std::lround(v), 0, (int)d.labels.size() - 1)];
    case kInt:
        snprintf(b, sizeof(b), "%d", (int)std::lround(v));
        return b;
    case kPan:
    {
        int pc = (int)std::lround(v * 100);
        if (pc == 0)
            return "C";
        snprintf(b, sizeof(b), "%s%d", pc < 0 ? "L" : "R", std::abs(pc));
        return b;
    }
    case kLean:
    {
        int pc = (int)std::lround(v * 100);
        if (pc == 50)
            return "Even";
        snprintf(b, sizeof(b), "%d%% to %s", pc < 50 ? 100 - pc : pc, pc < 50 ? "Low" : "High");
        return b;
    }
    case kLogHz:
    {
        double hz = std::exp2(v);
        snprintf(b, sizeof(b), hz < 1 ? "%.3f Hz" : hz < 100 ? "%.2f Hz" : "%.0f Hz", hz);
        return b;
    }
    case kSemis:
        snprintf(b, sizeof(b), "%.1f st", v);
        return b;
    case kBipolar:
        snprintf(b, sizeof(b), "%+.0f%%", v * 100);
        return b;
    case kLock:
        if (std::fabs(v) < 0.01)
            return "off";
        snprintf(b, sizeof(b), "%+.0f%%", v * 100);
        return b;
    case kOffset:
        snprintf(b, sizeof(b), "%+.0f%% of a step", v * 100);
        return b;
    case kMs:
        snprintf(b, sizeof(b), v < 10 ? "%.1f ms" : "%.0f ms", v);
        return b;
    case kLogMs:
    {
        double ms = std::exp2(v);
        if (ms >= 1000)
            snprintf(b, sizeof(b), "%.2f s", ms / 1000);
        else
            snprintf(b, sizeof(b), "%.0f ms", ms);
        return b;
    }
    case kNote:
    {
        static const char *nn[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        int m = (int)std::lround(v);
        snprintf(b, sizeof(b), "%s%d", nn[((m % 12) + 12) % 12], m / 12 - 1);
        return b;
    }
    case kRatio:
        snprintf(b, sizeof(b), "%.2fx", v);
        return b;
    case kPLock:
        if (v < 0)
            return "--";
        snprintf(b, sizeof(b), "%.0f%%", v * 100.0);
        return b;
    case kHzTone:
        return pitchText(v);
    case kVariant:
    {
        const auto &cat = filterCatalogue();
        int model = pl ? pl->step(bandBase(d.mode, d.band) + oModel) : 0;
        model = std::clamp(model, 0, (int)cat.size() - 1);
        const auto &mi = cat[(size_t)model];
        if (mi.labels.empty())
            return "-";
        return mi.labels[(size_t)variantIndexFor(model, (float)v)];
    }
    }
    return "";
}

bool sameText(const std::string &a, const char *b)
{
    size_t n = std::strlen(b);
    if (a.size() != n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

bool textValue(const Plugin *pl, const ParamDef &d, const char *text, double &out)
{
    // a named value first: try every step (or, for variants, every position)
    if (d.kind == kVariant)
    {
        const auto &cat = filterCatalogue();
        int model = std::clamp(pl ? pl->step(bandBase(d.mode, d.band) + oModel) : 0, 0, (int)cat.size() - 1);
        const auto &mi = cat[(size_t)model];
        for (size_t i = 0; i < mi.labels.size(); ++i)
            if (sameText(mi.labels[i], text))
            {
                out = mi.labels.size() > 1 ? (double)i / (double)(mi.labels.size() - 1) : 0.0;
                return true;
            }
    }
    else if (isStepped(d.kind))
    {
        for (int i = (int)d.min; i <= (int)d.max; ++i)
            if (sameText(valueText(pl, d, i), text))
            {
                out = i;
                return true;
            }
    }
    else if (d.kind == kDb && sameText("Off", text) && d.min <= -60)
    {
        out = d.min;
        return true;
    }
    if (d.kind == kLock && sameText("off", text))
    {
        out = 0.0;
        return true;
    }
    if (d.kind == kPan || d.kind == kLean)
    {
        // "C", "L30", "R30", "Even", "70% to High"
        std::string t = text;
        for (auto &c : t)
            c = (char)std::tolower((unsigned char)c);
        if (t == "c" || t == "even")
        {
            out = d.kind == kPan ? 0.0 : 0.5;
            return true;
        }
        if (d.kind == kPan && (t[0] == 'l' || t[0] == 'r'))
        {
            double x = std::atof(t.c_str() + 1) / 100.0;
            out = std::clamp(t[0] == 'l' ? -x : x, -1.0, 1.0);
            return true;
        }
        if (d.kind == kLean)
        {
            double x = std::atof(t.c_str()) / 100.0;
            out = std::clamp(t.find("low") != std::string::npos ? 1.0 - x : x, 0.0, 1.0);
            return true;
        }
    }
    if (d.kind == kPLock && std::string(text).find("--") != std::string::npos)
    {
        out = -1.0; // not locked
        return true;
    }
    char *end = nullptr;
    double v = std::strtod(text, &end);
    if (end == text)
        return false;
    while (*end == ' ')
        ++end;
    switch (d.kind)
    {
    case kPercent:
        v /= 100.0;
        break;
    case kPitch:
    case kHzTone:
        if (*end == 'k' || *end == 'K')
            v *= 1000.0;
        if (v <= 0)
            return false;
        v = hzToSemi(v);
        break;
    case kLogHz:
        if (v <= 0)
            return false;
        v = std::log2(v);
        break;
    case kLogMs:
        if (*end == 's' || *end == 'S')
            v *= 1000.0;
        if (v <= 0)
            return false;
        v = std::log2(v);
        break;
    case kPan:
    case kBipolar:
    case kLock:
    case kOffset:
    case kPLock:
        v /= 100.0;
        break;
    default:
        break;
    }
    out = std::clamp(v, d.min, d.max);
    return true;
}

// ----- params extension --------------------------------------------------------
uint32_t paramsCount(const clap_plugin_t *) { return (uint32_t)paramDefs().size(); }

bool paramsInfo(const clap_plugin_t *, uint32_t index, clap_param_info_t *info)
{
    const auto &defs = paramDefs();
    if (index >= defs.size())
        return false;
    const auto &d = defs[index];
    std::memset(info, 0, sizeof(*info));
    info->id = d.id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (isStepped(d.kind))
    {
        info->flags |= CLAP_PARAM_IS_STEPPED;
        if (d.kind != kToggle && d.kind != kInt)
            info->flags |= CLAP_PARAM_IS_ENUM;
    }
    else if (d.kind != kVariant)
        info->flags |= CLAP_PARAM_IS_MODULATABLE;
    info->cookie = nullptr;
    snprintf(info->name, sizeof(info->name), "%s", d.name.c_str());
    snprintf(info->module, sizeof(info->module), "%s", d.module.c_str());
    info->min_value = d.min;
    info->max_value = d.max;
    info->default_value = d.def;
    return true;
}

bool paramsValue(const clap_plugin_t *p, clap_id id, double *out)
{
    auto it = paramIndex().find(id);
    if (it == paramIndex().end())
        return false;
    *out = self(p)->value[(size_t)it->second].load(std::memory_order_relaxed);
    return true;
}

bool paramsValueToText(const clap_plugin_t *p, clap_id id, double v, char *out, uint32_t cap)
{
    auto it = paramIndex().find(id);
    if (it == paramIndex().end() || cap == 0)
        return false;
    snprintf(out, cap, "%s", valueText(self(p), paramDefs()[(size_t)it->second], v).c_str());
    return true;
}

bool paramsTextToValue(const clap_plugin_t *p, clap_id id, const char *text, double *out)
{
    auto it = paramIndex().find(id);
    if (it == paramIndex().end() || !text)
        return false;
    return textValue(self(p), paramDefs()[(size_t)it->second], text, *out);
}

void paramsFlush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out)
{
    auto *pl = self(p);
    pl->sendGuiEvents(out);
    uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; ++i)
        pl->handleEvent(in->get(in, i));
}

const clap_plugin_params_t extParams = {paramsCount, paramsInfo, paramsValue, paramsValueToText, paramsTextToValue, paramsFlush};

// ----- state extension: plain text, "id value" per line, so old projects keep loading --------
// "<lfo> <n> x y bend x y bend ..." -> that LFO's drawn curve. Points are kept in order, start at
// x = 0, end at x = 1, and number 2 to 32.
bool parseCurveLine(Plugin *pl, const char *t)
{
    char *e = nullptr;
    long lfo = std::strtol(t, &e, 10);
    if (e == t || lfo < 0 || lfo >= numLfos)
        return false;
    t = e;
    long n = std::strtol(t, &e, 10);
    if (e == t || n < 2 || n > maxCurvePoints)
        return false;
    t = e;
    std::vector<CurvePoint> pts;
    for (long i = 0; i < n; ++i)
    {
        float v[3];
        for (auto &x : v)
        {
            x = std::strtof(t, &e);
            if (e == t || !std::isfinite(x))
                return false;
            t = e;
        }
        pts.push_back({std::clamp(v[0], 0.f, 1.f), std::clamp(v[1], -1.f, 1.f), std::clamp(v[2], 0.05f, 20.f)});
    }
    std::stable_sort(pts.begin(), pts.end(), [](const CurvePoint &a, const CurvePoint &b) { return a.x < b.x; });
    pts.front().x = 0.f;
    pts.back().x = 1.f;
    pl->curve[lfo] = pts;
    return true;
}

static std::string stateText(Plugin *pl, bool withView);
bool stateSave(const clap_plugin_t *p, const clap_ostream_t *stream)
{
    std::string s = stateText(self(p), true);
    size_t done = 0;
    while (done < s.size())
    {
        int64_t w = stream->write(stream, s.data() + done, s.size() - done);
        if (w <= 0)
            return false;
        done += (size_t)w;
    }
    return true;
}
// every setting as text; withView adds the window's theme and size (projects yes, presets no)
static std::string stateText(Plugin *pl, bool withView)
{
    std::string s = "THREESCOMPANY 3\n";
    const auto &d = paramDefs();
    char b[64];
    for (size_t i = 0; i < d.size(); ++i)
    {
        snprintf(b, sizeof(b), "%u %.9g\n", (unsigned)d[i].id, (double)pl->value[i].load(std::memory_order_relaxed));
        s += b;
    }
    if (withView)
    {
        s += pl->darkTheme.load() ? "T dark\n" : "T light\n";
        s += "W " + std::to_string(pl->guiW) + " " + std::to_string(pl->guiH) + "\n";
    }
    // the drawn LFO curves: "C <lfo> <points> x y bend ..." (older versions skip these lines)
    for (int i = 0; i < numLfos; ++i)
    {
        snprintf(b, sizeof(b), "C %d %d", i, (int)pl->curve[i].size());
        s += b;
        for (auto &c : pl->curve[i])
        {
            snprintf(b, sizeof(b), " %.6g %.6g %.6g", c.x, c.y, c.t);
            s += b;
        }
        s += "\n";
    }
    return s;
}

// ----- your own presets: one text file each, in the settings folder's Presets folder (not in projects)
namespace fs = std::filesystem;
// a path as UTF-8 text (C++20's u8string is a different string type)
static std::string pathText(const fs::path &p)
{
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
static fs::path defaultPresetDir()
{
    std::string p = prefsPath();
    if (p.empty())
        return fs::path();
    return fs::u8path(p).parent_path() / "Presets";
}
// where your presets live: the folder you chose in Settings, or the standard place
static fs::path userPresetDir()
{
    Prefs pr = loadPrefs();
    if (!pr.presetDir.empty())
        return fs::u8path(pr.presetDir);
    return defaultPresetDir();
}
// a name that is safe as a file name on Mac and Windows
static std::string cleanPresetName(std::string n)
{
    std::string o;
    for (char c : n)
        if (!std::strchr("/\\:*?\"<>|", c) && (unsigned char)c >= 32)
            o += c;
    while (!o.empty() && (o.back() == ' ' || o.back() == '.'))
        o.pop_back();
    while (!o.empty() && o.front() == ' ')
        o.erase(o.begin());
    if (o.size() > 60)
        o.resize(60);
    return o;
}
static std::vector<std::string> userPresetNames()
{
    std::vector<std::string> r;
    std::error_code ec;
    fs::path d = userPresetDir();
    if (d.empty() || !fs::is_directory(d, ec))
        return r;
    for (auto &e : fs::directory_iterator(d, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".tcpreset")
            r.push_back(pathText(e.path().stem()));
    std::sort(r.begin(), r.end());
    return r;
}
static bool saveUserPreset(Plugin *pl, const std::string &name)
{
    std::error_code ec;
    fs::path d = userPresetDir();
    if (d.empty() || name.empty())
        return false;
    fs::create_directories(d, ec);
    std::ofstream f(d / fs::u8path(name + ".tcpreset"), std::ios::binary);
    f << stateText(pl, false);
    return (bool)f;
}
static bool loadStateText(Plugin *pl, const std::string &s);
static bool loadUserPreset(Plugin *pl, const std::string &name)
{
    std::ifstream f(userPresetDir() / fs::u8path(name + ".tcpreset"), std::ios::binary);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    if (!loadStateText(pl, ss.str()))
        return false;
    if (pl->hostParams && pl->hostParams->rescan)
        pl->hostParams->rescan(pl->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
    pl->markDirty();
    return true;
}
static void deleteUserPreset(const std::string &name)
{
    std::error_code ec;
    fs::remove(userPresetDir() / fs::u8path(name + ".tcpreset"), ec);
}

static bool loadStateText(Plugin *pl, const std::string &s);
bool stateLoad(const clap_plugin_t *p, const clap_istream_t *stream)
{
    auto *pl = self(p);
    std::string s;
    char buf[4096];
    for (;;)
    {
        int64_t r = stream->read(stream, buf, sizeof(buf));
        if (r < 0)
            return false;
        if (r == 0)
            break;
        s.append(buf, (size_t)r);
        if (s.size() > (1u << 20))
            return false;
    }
    return loadStateText(pl, s);
}
static bool loadStateText(Plugin *pl, const std::string &s)
{
    if (s.rfind("THREESCOMPANY ", 0) != 0)
        return false;
    const auto &d = paramDefs();
    // anything the saved state does not mention goes back to its starting value
    for (size_t i = 0; i < d.size(); ++i)
        pl->value[i].store((float)d[i].def, std::memory_order_relaxed);
    // Saved before 0.18 (version 1, and the factory presets made then): mod slots went "as the source moves" and the
    // bank ripples swung every band both ways. Those defaults stay for them, so they sound as they did.
    if (s.rfind("THREESCOMPANY 1", 0) == 0)
    {
        auto setDef = [&](clap_id id, float v) { auto it = paramIndex().find(id); if (it != paramIndex().end()) pl->value[(size_t)it->second].store(v, std::memory_order_relaxed); };
        for (int k = 0; k < numSlots; ++k)
            setDef(slotBase(k) + sSwing, 0.f);
        for (int m = 0; m < 2; ++m)
            for (int b = 0; b < numBands; ++b)
                setDef(bandBase(m, b) + oRipBoth, 255.f);
    }
    for (int i = 0; i < numLfos; ++i)
        pl->curve[i] = Plugin::defaultCurve();
    size_t pos = s.find('\n');
    while (pos != std::string::npos && pos + 1 < s.size())
    {
        size_t next = s.find('\n', pos + 1);
        std::string line = s.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
        unsigned id = 0;
        double v = 0;
        if (!line.empty() && line[0] == 'C')
            parseCurveLine(pl, line.c_str() + 1);
        else if (line.rfind("T ", 0) == 0)
            pl->darkTheme.store(line.compare(2, 4, "dark") == 0);
        else if (line.rfind("W ", 0) == 0)
        {
            unsigned w = 0, h = 0;
            if (std::sscanf(line.c_str() + 2, "%u %u", &w, &h) == 2 && w >= 640 && w <= 3840 && h >= 470 && h <= 2820)
                pl->guiW = w, pl->guiH = h;
        }
        else if (sscanf(line.c_str(), "%u %lf", &id, &v) == 2 && std::isfinite(v))
        {
            auto it = paramIndex().find((clap_id)id);
            if (it != paramIndex().end())
            {
                const auto &pd = d[(size_t)it->second];
                pl->value[(size_t)it->second].store((float)std::clamp(v, pd.min, pd.max), std::memory_order_relaxed);
            }
        }
        pos = next;
    }
    // Saved before 0.19 (versions 1 and 2): the Z-Plane list had 16 shapes, and a strip's shape is stored as a
    // 0..1 position in that list. New shapes are added at the end, so move old positions onto the same shape.
    {
        int ver = std::atoi(s.c_str() + 14);
        bool zCurrent = s.substr(0, s.find('\n')).find("zcur") != std::string::npos;
        const int zModel = modelIndexByName("Z-Plane"), zOld = 16;
        int zNow = 0;
        zDesigns(zNow);
        if (ver < 3 && !zCurrent && zModel > 0 && zNow != zOld)
            for (int m = 0; m < 2; ++m)
                for (int b = 0; b < numBands; ++b)
                {
                    auto mi = paramIndex().find(bandBase(m, b) + oModel), vi = paramIndex().find(bandBase(m, b) + oVariant);
                    if (mi == paramIndex().end() || vi == paramIndex().end())
                        continue;
                    if ((int)std::lround(pl->value[(size_t)mi->second].load()) != zModel)
                        continue;
                    float v = pl->value[(size_t)vi->second].load();
                    int idx = std::clamp((int)std::lround(v * (float)(zOld - 1)), 0, zOld - 1);
                    pl->value[(size_t)vi->second].store((float)idx / (float)(zNow - 1), std::memory_order_relaxed);
                }
    }
    for (int i = 0; i < numLfos; ++i)
        pl->applyCurve(i);
    pl->dirty.store(true, std::memory_order_relaxed);
    if (pl->bridge)
        pl->guiNeedsInit();
    return true;
}

const clap_plugin_state_t extState = {stateSave, stateLoad};

// Three's Company's own presets, after the classic set. Built here so they can name filters by name.
size_t &presetLegacyCount();
static const std::vector<TcPreset> &presetList()
{
    static const std::vector<TcPreset> &all = *[]
    {
        auto *v = new std::vector<TcPreset>(tcPresets());
        const float sh = (float)modelIndexByName("Sample & Hold"), crush = (float)Crush, warm = (float)Warm;
        using V = std::vector<std::pair<const char *, float>>;
        auto add = [&](const char *name, V vals) { v->push_back({name, "Bitcrush", std::move(vals)}); };
        // 1. subtle: a little tape-ish loss of bits up top, everything else clean
        add("Crush: Dusty Sampler", {{"Routing", 0}, {"Par Low Drive Type", warm}, {"Par Low Drive", 0.15f},
                                     {"Par Mid Drive Type", crush}, {"Par Mid Drive", 0.42f},
                                     {"Par High Filter Type", sh}, {"Par High Filter Variant#idx", 0}, {"Par High Cutoff", 64.f}, {"Par High Drive Type", crush}, {"Par High Drive", 0.38f},
                                     {"Par High Level", -2.f}, {"Par Noise On", 1}, {"Par Noise", 0.05f}, {"Par Mix", 0.7f}});
        // 2. a 12-bit sampler: crushed and held across Mid and High, Low kept round
        add("Crush: 12-Bit Machine", {{"Routing", 0}, {"Par Low Drive Type", warm}, {"Par Low Drive", 0.2f},
                                      {"Par Mid Filter Type", sh}, {"Par Mid Filter Variant#idx", 0}, {"Par Mid Cutoff", 56.f}, {"Par Mid Drive Type", crush}, {"Par Mid Drive", 0.55f},
                                      {"Par High Filter Type", sh}, {"Par High Filter Variant#idx", 0}, {"Par High Cutoff", 56.f}, {"Par High Drive Type", crush}, {"Par High Drive", 0.55f},
                                      {"Par Mix", 1.f}});
        // 3. funky: Mid's bit depth steps in a 16th pattern, High's sample rate sways with an LFO
        add("Crush: Wobble Bits", {{"Routing", 0}, {"Par Mid Drive Type", crush}, {"Par Mid Drive", 0.45f},
                                   {"Par High Filter Type", sh}, {"Par High Filter Variant#idx", 0}, {"Par High Cutoff", 50.f}, {"Par High Drive Type", crush}, {"Par High Drive", 0.4f},
                                   {"Seq Mid Target", 10}, {"Seq Mid Rate", 2}, {"Seq Mid Length", 16}, {"Seq Mid Smooth", 0.1f},
                                   {"Seq Mid Step 1", 0.8f}, {"Seq Mid Step 3", -0.5f}, {"Seq Mid Step 4", 0.4f}, {"Seq Mid Step 7", 0.9f},
                                   {"Seq Mid Step 9", 0.6f}, {"Seq Mid Step 11", -0.4f}, {"Seq Mid Step 12", 0.7f}, {"Seq Mid Step 15", 1.f},
                                   {"Seq Low Target", 0}, {"Seq High Target", 0},
                                   {"LFO 2 Shape", 1}, {"LFO 2 Sync Rate", 4}, {"LFO 2 Target", 13}, {"LFO 2 Amount", 0.45f},
                                   {"Sway", 0.3f}, {"Par Mix", 1.f}});
        // 4. glitch: the Conductor throws the held sample rate around on a 1/16 grid, Jack pushes back
        add("Crush: Glitch Funk", {{"Routing", 0}, {"Par Low Drive Type", warm}, {"Par Low Drive", 0.25f},
                                   {"Par Mid Drive Type", crush}, {"Par Mid Drive", 0.6f},
                                   {"Par High Filter Type", sh}, {"Par High Filter Variant#idx", 0}, {"Par High Cutoff", 46.f}, {"Par High Drive Type", crush}, {"Par High Drive", 0.65f},
                                   {"Seq High Target", 16}, {"Seq High Euclid", 2}, {"Seq High Hits", 5}, {"Seq High Length", 16},
                                   {"Seq Low Target", 0}, {"Seq Mid Target", 0},
                                   {"Chaperone Influence", 0.8f}, {"Chaperone Lean", 0.8f}, {"Chaperone Grid", 5}, {"Chaperone Chaos", 0.65f},
                                   {"Chaperone Range", 14.f}, {"Chaperone Chance", 0.7f}, {"Chaperone Snap", 1}, {"3rd Wheel", 0.5f}, {"3rd Wheel Mischief", 0.3f},
                                   {"Par Mix", 1.f}});
        // 5. tame madness: all three crushed and held, everything moving, but mixed back and kept in key
        add("Crush: Tame Madness", {{"Routing", 0}, {"Par Low Drive Type", crush}, {"Par Low Drive", 0.4f},
                                    {"Par Mid Filter Type", sh}, {"Par Mid Filter Variant#idx", 0}, {"Par Mid Cutoff", 52.f}, {"Par Mid Drive Type", crush}, {"Par Mid Drive", 0.6f},
                                    {"Par High Filter Type", sh}, {"Par High Filter Variant#idx", 0}, {"Par High Cutoff", 48.f}, {"Par High Drive Type", crush}, {"Par High Drive", 0.7f},
                                    {"Seq Low Target", 0}, {"Seq High Target", 0},
                                    {"Chaperone Influence", 1.f}, {"Chaperone Grid", 4}, {"Chaperone Chaos", 0.85f}, {"Chaperone Range", 12.f},
                                    {"Chaperone Bursts", 0.4f}, {"Chaperone Snap", 2}, {"3rd Wheel", 0.6f}, {"3rd Wheel Mischief", 0.6f}, {"3rd Wheel Moves", 3},
                                    {"Sway", 0.35f}, {"Mutate", 0.3f}, {"Par Mix", 0.65f}});
        // ----- Z-Plane: ten shapes from the Morpheus-style filter, each with its own use of the bank (Slink) and Ripple
        const float zp = (float)modelIndexByName("Z-Plane");
        auto zi = [](const char *n) { int c = 0; const ZDesign *d = zDesigns(c); for (int i = 0; i < c; ++i) if (!std::strcmp(d[i].name, n)) return (float)i; return 0.f; };
        auto addZ = [&](const char *name, V vals) { v->push_back({name, "Z-Plane", std::move(vals)}); };
        // targets: Low Morph 3, Mid Morph 9, High Morph 15, Low Cutoff 1, Mid Cutoff 7, High Cutoff 13; LFO shapes: Sine 1, Triangle 2, Ramp 3, S&H 9
        // All ten run in Spectral mode with at least two Z-Plane strips. Tonal: Low = tones, Mid = hits, High = air.
        // Freq: Low / Mid / High by frequency. Morph targets: Low 3, Mid 9, High 15. Spectral Balance target 26.
        auto spec = [&](V vals, int by, int q) { V r{{"Routing", 0}, {"Parallel Split", 1}, {"Spectral Split By", (float)by}, {"Spectral Quality", (float)q}, {"Seq Low Target", 0}, {"Seq Mid Target", 0}, {"Seq High Target", 0}};
                                                  r.insert(r.end(), vals.begin(), vals.end()); return r; };
        auto Z = [&](const char *band, const char *design, float cutoff, float reso, float morph) {
            static std::deque<std::string> keep; // the names must outlive the list; a deque never moves its strings
            auto k = [&](const char *what) { keep.push_back(std::string("Par ") + band + " " + what); return keep.back().c_str(); };
            return V{{k("Filter Type"), zp}, {k("Filter Variant#idx"), zi(design)}, {k("Cutoff"), cutoff}, {k("Resonance"), reso}, {k("Morph"), morph}};
        };
        auto cat = [](std::initializer_list<V> parts) { V r; for (auto &p : parts) r.insert(r.end(), p.begin(), p.end()); return r; };
        // 1. DRUMS: the hits talk in sixteenths (vowel morph on Mid's lane), the tones get rubbery octave peaks, the air sparkles
        addZ("Z: Drum Mouth", spec(cat({Z("Mid", "Vowel Ah > Ee", 14.2f, 0.6f, 0.3f), Z("Low", "Rubber Hose", 14.2f, 0.5f, 0.4f),
            {{"Seq Mid Target", 9}, {"Seq Mid Rate", 2}, {"Seq Mid Length", 16}, {"Seq Mid Smooth", 0.25f}, {"Seq Mid Step 1", 0.8f}, {"Seq Mid Step 3", -0.4f}, {"Seq Mid Step 5", 0.6f}, {"Seq Mid Step 8", 0.9f}, {"Seq Mid Step 11", -0.3f}, {"Seq Mid Step 13", 0.7f},
             {"LFO 1 Shape", 1}, {"LFO 1 Sync Rate", 4}, {"LFO 1 Target", 3}, {"LFO 1 Amount", 0.4f},
             {"Par High Bank Ripple", 0.5f}, {"Par High Bank Ripple Shape", 2}, {"Par High Bank Ripple Timing", 1}, {"Par High Bank Ripple Sync", 2}, {"Par High Bank Stereo", 1.f}, {"Spectral Sharpness", 0.7f}}}), 1, 1));
        // 2. DRUMS: hats and noise go through sample-and-hold phaser notches, the hits ring like a waterdrop on a Euclid rhythm
        addZ("Z: Hat Sprinkler", spec(cat({Z("High", "Phaser Notches", 14.2f, 0.55f, 0.5f), Z("Mid", "BroadRes", 14.2f, 0.6f, 0.2f),
            {{"LFO 2 Shape", 9}, {"LFO 2 Sync Rate", 0}, {"LFO 2 Target", 15}, {"LFO 2 Amount", 0.5f}, {"LFO 2 Smooth", 0.15f},
             {"Seq Mid Target", 9}, {"Seq Mid Rate", 2}, {"Seq Mid Euclid", 2}, {"Seq Mid Hits", 5}, {"Seq Mid Smooth", 0.3f}, {"Seq Mid Depth", 0.8f},
             {"Par High Bank Ripple", 0.6f}, {"Par High Bank Ripple Shape", 5}, {"Par High Bank Ripple Timing", 1}, {"Par High Bank Ripple Sync", 3}, {"Par High Bank Ripple Waves", 2.f}}}), 1, 0));
        // 3. TECHNO: the kick region speaks (Talking Bass on Low, quarter-note morph), the mids pulse through 500 Up, a fast spin in the top
        addZ("Z: Kick Talker", spec(cat({Z("Low", "Talking Bass", -14.f, 0.6f, 0.2f), Z("Mid", "500 Up", 14.2f, 0.5f, 0.5f),
            {{"LFO 1 Shape", 5}, {"LFO 1 Sync Rate", 2}, {"LFO 1 Target", 3}, {"LFO 1 Amount", 0.6f},
             {"Seq Mid Target", 9}, {"Seq Mid Rate", 4}, {"Seq Mid Length", 8}, {"Seq Mid Step 1", 0.8f}, {"Seq Mid Step 3", -0.8f}, {"Seq Mid Step 5", 0.5f}, {"Seq Mid Step 7", -0.5f},
             {"Par High Bank Ripple", 0.45f}, {"Par High Bank Ripple Shape", 3}, {"Par High Bank Ripple Timing", 1}, {"Par High Bank Ripple Sync", 2}, {"Par High Bank Stereo", 0.7f}}}), 0, 0));
        // 4. TECHNO: acid vowels, the mids' vowel stepping in 1/16s with high Transform 2, a ramped resonant sweep in the highs each bar
        addZ("Z: Acid Vowels", spec(cat({Z("Mid", "Vowel Oo > Eh", 14.2f, 0.85f, 0.3f), Z("High", "Resonant Sweep", 30.f, 0.7f, 0.2f),
            {{"Seq Mid Target", 9}, {"Seq Mid Rate", 2}, {"Seq Mid Length", 16}, {"Seq Mid Smooth", 0.15f},
             {"Seq Mid Step 1", 0.9f}, {"Seq Mid Step 2", -0.2f}, {"Seq Mid Step 4", 0.5f}, {"Seq Mid Step 6", 0.9f}, {"Seq Mid Step 7", -0.6f}, {"Seq Mid Step 10", 0.7f}, {"Seq Mid Step 12", -0.3f}, {"Seq Mid Step 14", 1.f},
             {"LFO 3 Shape", 3}, {"LFO 3 Sync Rate", 4}, {"LFO 3 Target", 15}, {"LFO 3 Amount", 0.6f},
             {"Par Mid Drive Type", 1}, {"Par Mid Drive", 0.35f}, {"Par Mid Bank Ripple", 0.35f}, {"Par Mid Bank Ripple Shape", 1}, {"Par Mid Bank Ripple Timing", 1}, {"Par Mid Bank Ripple Sync", 3}}}), 0, 1));
        // 5. PADS: the sustained tones sing a slow vowel (Sway on Morph), the air flips odd/even harmonics over four bars, a slow wave through the tones' bank
        addZ("Z: Pad Choir", spec(cat({Z("Low", "Vowel Oh > Ah", 14.2f, 0.6f, 0.4f), Z("High", "Odd > Even", 14.2f, 0.5f, 0.5f),
            {{"Sway", 0.6f}, {"Sway to Cutoff", 0}, {"Sway to Morph", 0.9f}, {"Sway Rate", -3.3f},
             {"LFO 2 Shape", 1}, {"LFO 2 Sync Rate", 6}, {"LFO 2 Target", 15}, {"LFO 2 Amount", 0.5f},
             {"Par Low Bank Ripple", 0.55f}, {"Par Low Bank Ripple Shape", 0}, {"Par Low Bank Ripple Rate", -2.5f}, {"Par Low Bank Width", 0.6f}, {"Spectral Balance", 0.3f}}}), 1, 2));
        // 6. AMBIENT: bell partials ring out of the tones, the air runs through a slowly shifting comb, a spotlight walks the bands in half notes
        addZ("Z: Glass Cathedral", spec(cat({Z("Low", "Bell Ring", 14.2f, 0.85f, 0.3f), Z("High", "Comb Shift", 14.2f, 0.65f, 0.5f),
            {{"LFO 1 Shape", 1}, {"LFO 1 Sync Rate", 7}, {"LFO 1 Target", 3}, {"LFO 1 Amount", 0.45f},
             {"Sway", 0.5f}, {"Sway to Cutoff", 0}, {"Sway to Morph", 0.7f}, {"Sway Rate", -3.6f},
             {"Par Low Bank Ripple", 0.7f}, {"Par Low Bank Ripple Shape", 5}, {"Par Low Bank Ripple Timing", 1}, {"Par Low Bank Ripple Sync", 5}, {"Par Low Bank Stereo", 0.9f}, {"Par Low Bank Width", 0.25f}}}), 1, 3));
        // 7. AMBIENT: all three strips Z-Plane by frequency, each morph drifting with Sway at its own speed, a slow deep wave through the mids
        addZ("Z: Tidal Drift", spec(cat({Z("Low", "Hollow > Bright", -10.f, 0.5f, 0.4f), Z("Mid", "Lowpass > Bandpass", 14.2f, 0.55f, 0.4f), Z("High", "Swingshift", 34.f, 0.5f, 0.4f),
            {{"Sway", 0.7f}, {"Sway to Cutoff", 0}, {"Sway to Morph", 1.f}, {"Sway Rate", -3.f},
             {"Par Mid Bank Ripple", 0.7f}, {"Par Mid Bank Ripple Shape", 0}, {"Par Mid Bank Ripple Rate", -3.3f}, {"Par Mid Bank Ripple Waves", 0.5f}, {"Spectral Sharpness", 0.3f}}}), 0, 2));
        // 8. TRIPPY: three Z-Plane strips, three LFOs at 1 bar, 3/4 bar and 2 bars so they never line up, a 4 Hz spin through every bank
        addZ("Z: Warp Tunnel", spec(cat({Z("Low", "Phaser Notches", -10.f, 0.6f, 0.5f), Z("Mid", "Comb Shift", 14.2f, 0.6f, 0.5f), Z("High", "Odd > Even", 34.f, 0.6f, 0.5f),
            {{"LFO 1 Shape", 2}, {"LFO 1 Sync Rate", 4}, {"LFO 1 Target", 3}, {"LFO 1 Amount", 0.5f},
             {"LFO 2 Shape", 1}, {"LFO 2 Timing", 1}, {"LFO 2 Rate", -0.58f}, {"LFO 2 Target", 9}, {"LFO 2 Amount", 0.5f},
             {"LFO 3 Shape", 3}, {"LFO 3 Sync Rate", 5}, {"LFO 3 Target", 15}, {"LFO 3 Amount", -0.5f},
             {"Par Low Bank Ripple", 0.4f}, {"Par Mid Bank Ripple", 0.4f}, {"Par High Bank Ripple", 0.4f}, {"Par Low Bank Ripple Shape", 3}, {"Par Mid Bank Ripple Shape", 3}, {"Par High Bank Ripple Shape", 3},
             {"Par Low Bank Ripple Rate", 2.f}, {"Par Mid Bank Ripple Rate", 2.f}, {"Par High Bank Ripple Rate", 2.f}, {"Par High Bank Stereo", 1.f}}}), 0, 1));
        // 9. TRIPPY DRUMS: the hits ring like a guitar body, the tones swing open as the drums get louder, the 3rd Wheel knocks the morphs out of line
        addZ("Z: Ghost Machine", spec(cat({Z("Mid", "Guitar Body", 14.2f, 0.6f, 0.5f), Z("Low", "Swingshift", 14.2f, 0.55f, 0.2f),
            {{"Mod 1 Source", 7}, {"Mod 1 Target", 3}, {"Mod 1 Amount", 0.7f}, {"Follower Sens", 12.f},
             {"3rd Wheel Mischief", 0.6f}, {"3rd Wheel Moves", 2},
             {"Par High Bank Ripple", 0.55f}, {"Par High Bank Ripple Shape", 2}, {"Par High Bank Ripple Timing", 1}, {"Par High Bank Ripple Sync", 1}, {"Par High Bank Stereo", 1.f}}}), 1, 0));
        // 10. TECHNO / TRIPPY: an LFO slides the spectral split points themselves, so the rubbery lows and the waterdrop highs trade frequencies
        addZ("Z: Melting Grid", spec(cat({Z("Low", "Rubber Hose", -12.f, 0.6f, 0.5f), Z("High", "BroadRes", 34.f, 0.6f, 0.5f),
            {{"Mod 1 Source", 4}, {"Mod 1 Target", 26}, {"Mod 1 Amount", 0.6f}, {"LFO 1 Shape", 1}, {"LFO 1 Sync Rate", 5},
             {"LFO 2 Shape", 9}, {"LFO 2 Sync Rate", 1}, {"LFO 2 Target", 15}, {"LFO 2 Amount", 0.5f},
             {"Chaperone Influence", 0.6f}, {"Chaperone Grid", 8}, {"Chaperone Snap", 1},
             {"Par Mid Bank Ripple", 0.5f}, {"Par Mid Bank Ripple Shape", 4}, {"Par Mid Bank Ripple Timing", 1}, {"Par Mid Bank Ripple Sync", 6}, {"Spectral Sharpness", 0.8f}}}), 0, 1));
        // ===== 0.18 =====================================================================================
        // Everything above was made before 0.18 and loads with the old mod and ripple behaviour (see presetText).
        presetLegacyCount() = v->size();
        // Old names lose their "Crush:" / "Z:" prefixes; every preset goes into a category by what it does.
        {
            static const std::map<std::string, const char *> cat = {
                {"Mellow Filters", "Subtle Tweaks"}, {"A Simple Plan", "Subtle Tweaks"}, {"K35 LPF", "Subtle Tweaks"},
                {"Offset Sweep", "Sweeps"}, {"Big OB Sweep", "Sweeps"}, {"Dual BP", "Sweeps"}, {"K35 HP-LP", "Sweeps"}, {"Alternate", "Sweeps"},
                {"Sync 1/4 + 1 Bar", "Sweeps"}, {"Sync 1/8 + 1/2", "Sweeps"}, {"Sync 1/8 + 1 Bar", "Sweeps"}, {"Sync 1/2 + 2 Bars", "Sweeps"},
                {"Sync 1 Bar + 4 Bars", "Sweeps"}, {"Sync 1/4 + 1/4T", "Sweeps"}, {"Sync 1/4D + 1 Bar", "Sweeps"}, {"Sync 1/2T + 1/2", "Sweeps"},
                {"Wobble 1/8 + Bar", "Sweeps"}, {"Triplet Drift", "Sweeps"}, {"Half-Time Breathe", "Sweeps"},
                {"Pulsing Tremolo", "Rhythm & Gates"}, {"Autopan", "Rhythm & Gates"}, {"Analog Autopan", "Rhythm & Gates"}, {"Slices", "Rhythm & Gates"},
                {"Wide Pan Sync", "Rhythm & Gates"}, {"Euclid Gate 1/16", "Rhythm & Gates"}, {"Swing Pump 1/8", "Rhythm & Gates"},
                {"Try it on Drums", "Rhythm & Gates"}, {"Hats or Claves", "Rhythm & Gates"}, {"Beeps and Boops", "Rhythm & Gates"}, {"Drum Follow Sync", "Rhythm & Gates"},
                {"FlangoPhaz", "Phasers & Combs"}, {"Sea Flange", "Phasers & Combs"}, {"Spatial Phaser", "Phasers & Combs"}, {"Xpander Phase", "Phasers & Combs"},
                {"Phase Pad", "Phasers & Combs"}, {"Phase Pad II", "Phasers & Combs"}, {"Comb Shimmer", "Phasers & Combs"},
                {"Dirtifier", "Dirt & Crush"}, {"Feedback Sync Growl", "Dirt & Crush"}, {"Serial Squeeze", "Dirt & Crush"},
                {"Pad Life", "Pads & Textures"}};
            static std::deque<std::string> names;
            for (auto &p : *v)
            {
                std::string n = p.name, g = p.group;
                if (n.rfind("Crush: ", 0) == 0)
                    n = n.substr(7), g = "Dirt & Crush";
                else if (n.rfind("Z: ", 0) == 0)
                    n = n.substr(3), g = "Spectral & Tonal";
                else if (auto it = cat.find(n); it != cat.end())
                    g = it->second;
                else if (n == "Init")
                    g = "Start";
                // the classic presets keep a private mark, so the loader still leaves their Mid strip open
                if (std::string(p.group).rfind("Classic", 0) == 0)
                    g = "~" + g;
                names.push_back(n), p.name = names.back().c_str();
                names.push_back(g), p.group = names.back().c_str();
            }
        }
        auto add18 = [&](const char *name, const char *group, V vals) { v->push_back({name, group, std::move(vals)}); };
        auto var = [](const char *model, const char *label) {
            const auto &c = filterCatalogue();
            int m = modelIndexByName(model);
            const auto &L = c[(size_t)m].labels;
            for (size_t i = 0; i < L.size(); ++i)
                if (L[i].find(label) != std::string::npos)
                    return (float)i;
            return 0.f;
        };
        // one strip: "Par"/"Ser", band, model, variant (by label), cutoff (semitones from A440), reso, morph
        auto F = [&](const char *mode, const char *band, const char *model, const char *label, float cut, float reso, float morph) {
            static std::deque<std::string> keep;
            auto k = [&](const char *what) { keep.push_back(std::string(mode) + " " + band + " " + what); return keep.back().c_str(); };
            return V{{k("Filter Type"), (float)modelIndexByName(model)}, {k("Filter Variant#idx"), var(model, label)}, {k("Cutoff"), cut}, {k("Resonance"), reso}, {k("Morph"), morph}, {k("Filter On"), 1}};
        };
        auto P2 = [](std::initializer_list<V> parts) { V r; for (auto &p : parts) r.insert(r.end(), p.begin(), p.end()); return r; };
        // lanes off unless a preset uses them (their starting targets are not None)
        const V lanesOff{{"Seq Low Target", 0}, {"Seq Mid Target", 0}, {"Seq High Target", 0}};
        // a P-lock row: values are knob positions 0..1 on the given steps (-1 = no lock elsewhere)
        auto locks = [&](const char *band, const char *knob, std::initializer_list<std::pair<int, float>> steps) {
            static std::deque<std::string> keep;
            V r;
            for (auto &st : steps)
            {
                keep.push_back(std::string("Lock ") + band + " " + knob + " " + std::to_string(st.first));
                r.push_back({keep.back().c_str(), st.second});
            }
            return r;
        };
        // knob position of a cutoff in semitones (the cutoff knob runs -60..82)
        auto cutPos = [](float semis) { return (semis + 60.f) / 142.f; };

        // ----- Start: five that each lead with one feature and pair it with another
        add18("Chaperone Passes Notes", "Start", P2({lanesOff, {{"Routing", 0}},
            F("Par", "Low", "OB-Xd 2-pole", "Lowpass Standard", 2.f, 0.45f, 0), F("Par", "Mid", "Vember Classic", "Bandpass 12 dB Standard", 14.2f, 0.55f, 0), F("Par", "High", "OB-Xd 2-pole", "Highpass Standard", 30.f, 0.4f, 0),
            locks("Mid", "Cutoff", {{1, cutPos(14.2f)}, {4, cutPos(21.2f)}, {7, cutPos(26.2f)}, {9, cutPos(17.2f)}, {12, cutPos(29.2f)}, {15, cutPos(19.2f)}}),
            {{"Lock Mid Glide", 25}, {"Chaperone Influence", 0.6f}, {"Chaperone Grid", 8}, {"Chaperone Snap", 1}, {"Chaperone Range", 12}, {"Chaperone Lean", 0.65f}, {"Par Mix", 1}}}));
        add18("Locked Groove", "Start", P2({lanesOff, {{"Routing", 0}},
            F("Par", "Low", "Vember Ladder", "Lowpass 24 dB", -6.f, 0.5f, 0), F("Par", "Mid", "OB-Xd 4-pole", "Lowpass 12 dB", 18.f, 0.3f, 0.2f), F("Par", "High", "Fast SVF", "Bandpass", 36.f, 0.45f, 0),
            locks("Low", "Cutoff", {{1, cutPos(-2.f)}, {5, cutPos(-14.f)}, {9, cutPos(4.f)}, {13, cutPos(-10.f)}}),
            locks("Mid", "Reso", {{3, 0.75f}, {7, 0.2f}, {11, 0.8f}, {15, 0.35f}}), locks("Mid", "Drive", {{3, 0.6f}, {11, 0.7f}}),
            locks("High", "Pan", {{2, 0.15f}, {6, 0.85f}, {10, 0.2f}, {14, 0.8f}}), locks("High", "Level", {{4, 0.95f}, {8, 0.55f}, {12, 0.95f}, {16, 0.6f}}),
            {{"Lock Low Glide", 30}, {"Lock Mid Glide", 30}, {"Lock High Glide", 30},
             {"Sway", 0.45f}, {"Sway to Cutoff", 0}, {"Sway to Morph", 1}, {"Sway Rate", -4.f}}}));
        add18("Weather System", "Start", P2({lanesOff, {{"Routing", 0}},
            F("Par", "Low", "OB-Xd Xpander", "Lowpass 12 dB", 4.f, 0.35f, 0.3f), F("Par", "Mid", "Diode Ladder", "Lowpass 12 dB", 22.f, 0.4f, 0), F("Par", "High", "Cascade SVF", "Bandpass 24 dB Standard", 38.f, 0.35f, 0),
            {{"Sway", 0.6f}, {"Sway to Cutoff", 1}, {"Sway to Morph", 0.6f}, {"Sway to Pan", 0.5f}, {"Sway Rate", -3.5f}, {"Sway Low Rate", 0.75f}, {"Sway High Rate", 1.5f},
             {"Mutate", 0.6f}, {"Mutation Trigger", 4}, {"Mutation Sensitivity", 0.55f}, {"Mutation Hold-off", 4},
             {"Mutation Low Answer", 1}, {"Mutation Low Moves", 0}, {"Mutation Low Amount", 0.5f}, {"Mutation High Answer", 2}, {"Mutation High Moves", 0}, {"Mutation High Amount", 0.5f}}}));
        add18("Call and Response", "Start", P2({V{{"Routing", 0}, {"Seq Low Target", 0}, {"Seq High Target", 0}},
            F("Par", "Low", "K35", "Lowpass Mild", 0.f, 0.45f, 0), F("Par", "Mid", "OB-Xd 2-pole", "Bandpass Standard", 14.2f, 0.5f, 0), F("Par", "High", "K35", "Highpass Mild", 30.f, 0.45f, 0),
            {{"Seq Mid Target", 7}, {"Seq Mid Rate", 4}, {"Seq Mid Length", 8}, {"Seq Mid Step 1", 0.6f}, {"Seq Mid Step 3", -0.4f}, {"Seq Mid Step 4", 0.8f}, {"Seq Mid Step 7", -0.6f},
             {"Chaperone Influence", 0.5f}, {"Chaperone Grid", 8}, {"Chaperone Snap", 1},
             {"Mutate", 0.7f}, {"Mutation Trigger", 5}, {"Mutation Hold-off", 1},
             {"Mutation Low Answer", 1}, {"Mutation Low Amount", 0.45f}, {"Mutation Low Delay", 1}, {"Mutation High Answer", 2}, {"Mutation High Amount", 0.45f}, {"Mutation High Delay", 1}}}));
        add18("Full House", "Start", P2({lanesOff, {{"Routing", 0}},
            F("Par", "Low", "Vember Ladder", "Lowpass 24 dB", -2.f, 0.45f, 0), F("Par", "Mid", "Z-Plane", "Vowel Ah > Ee", 14.2f, 0.6f, 0.3f), F("Par", "High", "OB-Xd 2-pole", "Highpass Standard", 32.f, 0.4f, 0),
            locks("Mid", "Cutoff", {{1, cutPos(14.2f)}, {3, cutPos(19.2f)}, {6, cutPos(9.2f)}, {9, cutPos(21.2f)}, {11, cutPos(12.2f)}, {14, cutPos(24.2f)}}),
            locks("Mid", "Morph", {{3, 0.8f}, {9, 0.1f}, {14, 0.9f}}),
            {{"Lock Mid Glide", 20}, {"Chaperone Influence", 0.7f}, {"Chaperone Grid", 5}, {"Chaperone Chaos", 0.15f}, {"Chaperone Bursts", 0.1f}, {"Chaperone Snap", 1}, {"Chaperone Range", 12}, {"Chaperone Seed", 18},
             {"3rd Wheel", 0.4f}, {"3rd Wheel Mischief", 0.2f},
             {"Mutate", 0.5f}, {"Mutation Trigger", 5}, {"Mutation Hold-off", 2}, {"Mutation Low Answer", 1}, {"Mutation High Answer", 2}, {"Mutate Seed", 18},
             {"Sway", 0.3f}, {"Sway to Cutoff", 0.5f}, {"Sway to Pan", 0.4f}}}));
        // ----- Start: ten variations across the filter models, four of them on the Z-Plane
        add18("Sweeping Escort", "Start", P2({lanesOff, {{"Routing", 1}},
            F("Ser", "Low", "OB-Xd 4-pole", "Lowpass 24 dB", 20.f, 0.5f, 0), F("Ser", "Mid", "Fast SVF", "Lowpass", 82.f, 0, 0), F("Ser", "High", "OB-Xd 2-pole", "Highpass Standard", -24.f, 0.3f, 0),
            {{"LFO 1 Shape", 2}, {"LFO 1 Sync Rate", 5}, {"LFO 1 Target", 7}, {"LFO 1 Amount", 0.5f},
             {"Chaperone Influence", 0.8f}, {"Chaperone Grid", 0}, {"Chaperone Lean", 0.3f}, {"Ser Low Drive", 0.2f}}}));
        add18("Ladder Locks", "Start", P2({lanesOff, {{"Routing", 1}},
            F("Ser", "Low", "Vintage Ladder", "Lowpass Huovilainen Compensated", 8.f, 0.65f, 0), F("Ser", "Mid", "Fast SVF", "Lowpass", 82.f, 0, 0), F("Ser", "High", "Fast SVF", "Lowpass", 82.f, 0, 0),
            locks("Low", "Cutoff", {{1, cutPos(0.f)}, {3, cutPos(24.f)}, {5, cutPos(-6.f)}, {8, cutPos(30.f)}, {11, cutPos(6.f)}, {13, cutPos(18.f)}, {16, cutPos(36.f)}}),
            locks("Low", "Reso", {{3, 0.85f}, {8, 0.9f}, {16, 0.8f}}), {{"Lock Low Glide", 60}, {"Ser Low Drive Type", 1}, {"Ser Low Drive", 0.35f}}}));
        add18("Stepped Drift", "Start", P2({lanesOff, {{"Routing", 0}},
            F("Par", "Low", "Tri-pole", "Low/Low/Low Second", 6.f, 0.4f, 0), F("Par", "Mid", "Tri-pole", "Low/High/Low First", 20.f, 0.45f, 0.4f), F("Par", "High", "Tri-pole", "High/High/High First", 36.f, 0.35f, 0),
            {{"Sway", 0.55f}, {"Sway Motion", 1}, {"Sway Slew", 0.25f}, {"Sway Depth", 7}, {"Sway Sync", 1}, {"Sway Sync Length", 2}, {"Sway to Cutoff", 1}, {"Sway to Morph", 0.5f}}}));
        add18("Comb Chatter", "Start", P2({lanesOff, {{"Routing", 0}},
            F("Par", "Mid", "Comb", "Positive Feedback, Mix 100%", 26.f, 0.6f, 0), F("Par", "High", "Comb", "Negative Feedback, Mix 50%", 33.f, 0.5f, 0),
            {{"Mutate", 0.7f}, {"Mutation Trigger", 0}, {"Mutation Sensitivity", 0.6f}, {"Mutation Low Answer", 0}, {"Mutation High Answer", 2}, {"Mutation High Moves", 0}, {"Mutation High Amount", 0.6f},
             {"Par Mid Mix", 0.7f}, {"Par High Mix", 0.8f}}}));
        add18("Crushed Courier", "Start", P2({lanesOff, {{"Routing", 0}},
            F("Par", "Low", "K35", "Lowpass Moderate", 4.f, 0.35f, 0), F("Par", "Mid", "Sample & Hold", "Default", 50.f, 0.3f, 0), F("Par", "High", "K35", "Highpass Moderate", 34.f, 0.4f, 0),
            locks("Mid", "Drive", {{1, 0.3f}, {5, 0.8f}, {9, 0.5f}, {13, 0.95f}}), locks("Mid", "Cutoff", {{5, cutPos(36.f)}, {13, cutPos(28.f)}}),
            {{"Par Mid Drive Type", 6}, {"Par Mid Drive", 0.4f}, {"Chaperone Influence", 0.5f}, {"Chaperone Grid", 8}, {"Chaperone Snap", 3}}}));
        add18("Dancing Shelves", "Start", P2({lanesOff, {{"Routing", 0}},
            F("Par", "Low", "Fast SVF", "Low Shelf", -6.f, 0.3f, 0), F("Par", "Mid", "Fast SVF", "Bell", 14.2f, 0.4f, 0), F("Par", "High", "Fast SVF", "High Shelf", 40.f, 0.3f, 0),
            {{"Sway", 0.4f}, {"Sway to Cutoff", 0}, {"Sway to Level", 0.8f}, {"Sway Rate", -3.f},
             {"3rd Wheel", 0.6f}, {"3rd Wheel Pair", 0}, {"3rd Wheel Moves", 4}}}));
        add18("Vowel Relay", "Start", P2({V{{"Routing", 0}, {"Seq Low Target", 0}, {"Seq High Target", 0}},
            F("Par", "Low", "Z-Plane", "Vowel Oh > Ah", 2.f, 0.6f, 0.2f), F("Par", "Mid", "Z-Plane", "Vowel Ah > Ee", 14.2f, 0.65f, 0.5f), F("Par", "High", "Z-Plane", "Vowel Oo > Eh", 26.f, 0.6f, 0.7f),
            {{"Seq Mid Target", 9}, {"Seq Mid Rate", 4}, {"Seq Mid Length", 8}, {"Seq Mid Smooth", 0.35f}, {"Seq Mid Step 1", 0.8f}, {"Seq Mid Step 3", -0.6f}, {"Seq Mid Step 5", 0.4f}, {"Seq Mid Step 7", -0.9f},
             {"Chaperone Influence", 0.6f}, {"Chaperone Grid", 7}, {"3rd Wheel", 0.5f}, {"3rd Wheel Moves", 2}}}));
        add18("Morph Locks", "Start", P2({lanesOff, {{"Routing", 1}},
            F("Ser", "Low", "Z-Plane", "Lowpass > Bandpass", 10.f, 0.6f, 0), F("Ser", "Mid", "Fast SVF", "Lowpass", 82.f, 0, 0), F("Ser", "High", "Z-Plane", "Odd > Even", 20.f, 0.55f, 0.5f),
            locks("Low", "Morph", {{1, 0.f}, {4, 0.7f}, {7, 0.3f}, {10, 1.f}, {13, 0.5f}}), locks("High", "Morph", {{2, 0.9f}, {6, 0.1f}, {10, 0.6f}, {14, 0.2f}}),
            {{"Lock Low Glide", 80}, {"Lock High Glide", 40}}}));
        add18("Rubber Weather", "Start", P2({lanesOff, {{"Routing", 0}},
            F("Par", "Low", "Z-Plane", "Rubber Hose", -10.f, 0.6f, 0.3f), F("Par", "Mid", "Z-Plane", "BroadRes", 14.2f, 0.55f, 0.5f), F("Par", "High", "OB-Xd 2-pole", "Highpass Standard", 32.f, 0.3f, 0),
            {{"Sway", 0.65f}, {"Sway to Cutoff", 0.3f}, {"Sway to Morph", 1}, {"Sway Rate", -3.3f},
             {"Mutate", 0.5f}, {"Mutation Trigger", 4}, {"Mutation Low Answer", 2}, {"Mutation Low Moves", 0}, {"Mutation High Answer", 0}}}));
        add18("Formant Mutiny", "Start", P2({lanesOff, {{"Routing", 0}},
            F("Par", "Low", "Z-Plane", "Talking Bass", -14.f, 0.6f, 0.4f), F("Par", "Mid", "Z-Plane", "Swingshift", 14.2f, 0.6f, 0.5f), F("Par", "High", "Z-Plane", "500 Up", 30.f, 0.55f, 0.4f),
            locks("Mid", "Morph", {{1, 0.1f}, {5, 0.9f}, {9, 0.4f}, {13, 1.f}}),
            {{"Lock Mid Glide", 15}, {"Chaperone Influence", 0.5f}, {"Chaperone Grid", 5}, {"Chaperone Chaos", 0.3f},
             {"Mutate", 0.6f}, {"Mutation Trigger", 5}, {"Mutation Low Answer", 1}, {"Mutation High Answer", 2}, {"3rd Wheel Mischief", 0.3f}, {"3rd Wheel Moves", 2}}}));
        add18("Drawn Diode", "Start", P2({lanesOff, {{"Routing", 1}},
            F("Ser", "Low", "Diode Ladder", "Lowpass 24 dB", 6.f, 0.6f, 0), F("Ser", "Mid", "Fast SVF", "Lowpass", 82.f, 0, 0), F("Ser", "High", "Fast SVF", "Lowpass", 82.f, 0, 0),
            {{"LFO 1 Shape", 10}, {"LFO 1 Sync Rate", 1}, {"LFO 1 Target", 1}, {"LFO 1 Amount", 0.5f}, {"LFO 1 Glide", 0.3f}, {"LFO 1 Scale", 3},
             {"Sway", 0.3f}, {"Sway to Reso", 0.6f}, {"Ser Low Drive Type", 0}, {"Ser Low Drive", 0.3f}}}));

        // ----- ten for pads and synths (four after the Morpheus manual's filter ideas)
        add18("Soft Focus", "Subtle Tweaks", P2({lanesOff, {{"Routing", 1}},
            F("Ser", "Low", "OB-Xd 2-pole", "Lowpass Standard", 45.f, 0.15f, 0), F("Ser", "Mid", "Fast SVF", "Lowpass", 82.f, 0, 0), F("Ser", "High", "Fast SVF", "Lowpass", 82.f, 0, 0),
            {{"Ser Low Drive Type", 0}, {"Ser Low Drive", 0.1f}, {"Sway", 0.2f}, {"Sway Depth", 4}, {"Sway Rate", -5.f}}}));
        add18("Air Lift", "Subtle Tweaks", P2({lanesOff, {{"Routing", 0}},
            F("Par", "High", "Fast SVF", "High Shelf", 45.f, 0.2f, 0),
            {{"Par High Bank Mix", 0.4f}, {"Par High Bank Band 6", 0.4f}, {"Par High Bank Band 7", 0.55f}, {"Par High Bank Band 8", 0.6f},
             {"Sway", 0.25f}, {"Sway to Cutoff", 0}, {"Sway to Pan", 0.6f}, {"Sway Low Depth", 0}, {"Sway Mid Depth", 0}}}));
        add18("Analog Breath", "Subtle Tweaks", P2({lanesOff, {{"Routing", 1}},
            F("Ser", "Low", "Fast SVF", "Lowpass", 82.f, 0, 0), F("Ser", "Mid", "Vember Ladder", "Lowpass 24 dB", 36.f, 0.25f, 0), F("Ser", "High", "Fast SVF", "Lowpass", 82.f, 0, 0),
            {{"Sway", 0.35f}, {"Sway Depth", 3}, {"Sway Rate", -4.5f}, {"Sway Low Depth", 0}, {"Sway High Depth", 0}}}));
        add18("Wide Chorus Bands", "Pads & Textures", P2({lanesOff, {{"Routing", 0}},
            {{"Par Low Bank Mix", 0.6f}, {"Par Mid Bank Mix", 0.6f}, {"Par High Bank Mix", 0.6f},
             {"Par Low Bank Stereo", 0.8f}, {"Par Mid Bank Stereo", 0.8f}, {"Par High Bank Stereo", 0.8f},
             {"Par Low Bank Ripple", 0.5f}, {"Par Mid Bank Ripple", 0.5f}, {"Par High Bank Ripple", 0.5f},
             {"Par Low Bank Ripple Rate", -2.3f}, {"Par Mid Bank Ripple Rate", -2.0f}, {"Par High Bank Ripple Rate", -1.7f},
             {"Par Low Bank Ripple Waves", 1.5f}, {"Par Mid Bank Ripple Waves", 1.5f}, {"Par High Bank Ripple Waves", 1.5f},
             {"Par Low Bank Ripple Both Ways", 255}, {"Par Mid Bank Ripple Both Ways", 255}, {"Par High Bank Ripple Both Ways", 255}}}));
        add18("Tidal Phaser", "Pads & Textures", P2({lanesOff, {{"Routing", 1}},
            F("Ser", "Low", "OB-Xd Xpander", "Phaser 18 dB", 8.f, 0.55f, 0), F("Ser", "Mid", "Fast SVF", "Lowpass", 82.f, 0, 0), F("Ser", "High", "OB-Xd Xpander", "Phaser 18 dB", 26.f, 0.5f, 0),
            {{"LFO 1 Shape", 1}, {"LFO 1 Sync Rate", 7}, {"LFO 1 Target", 1}, {"LFO 1 Amount", 0.5f},
             {"LFO 2 Shape", 2}, {"LFO 2 Timing", 1}, {"LFO 2 Rate", -4.3f}, {"LFO 2 Target", 13}, {"LFO 2 Amount", 0.45f},
             {"3rd Wheel", 0.4f}, {"3rd Wheel Pair", 0}}}));
        add18("Talking Saw", "Rhythm & Gates", P2({V{{"Routing", 0}, {"Seq Low Target", 0}, {"Seq High Target", 0}},
            F("Par", "Mid", "Z-Plane", "Vowel Ah > Ee", 14.2f, 0.65f, 0.3f),
            {{"Seq Mid Target", 9}, {"Seq Mid Rate", 4}, {"Seq Mid Length", 8}, {"Seq Mid Smooth", 0.2f},
             {"Seq Mid Step 1", 0.9f}, {"Seq Mid Step 2", -0.5f}, {"Seq Mid Step 4", 0.6f}, {"Seq Mid Step 5", -0.9f}, {"Seq Mid Step 7", 0.4f}}}));
        add18("Gated Shimmer", "Rhythm & Gates", P2({V{{"Routing", 0}, {"Seq Low Target", 0}, {"Seq Mid Target", 0}},
            F("Par", "High", "Fast SVF", "Bandpass", 40.f, 0.4f, 0),
            {{"Seq High Target", 17}, {"Seq High Rate", 2}, {"Seq High Euclid", 1}, {"Seq High Hits", 5}, {"Seq High Length", 8}, {"Seq High Depth", 1},
             {"Par High Bank Mix", 0.5f}, {"Par High Bank Ripple", 0.4f}, {"Par High Bank Ripple Shape", 2}, {"Par High Bank Ripple Timing", 1}, {"Par High Bank Ripple Sync", 2}, {"Par High Bank Stereo", 0.7f}}}));
        // Morpheus-inspired: BroadRes is the manual's wide resonant peak; here it frames a comb that freezes into a drone
        add18("Frozen Comb", "Pads & Textures", P2({lanesOff, {{"Routing", 1}},
            F("Ser", "Low", "Z-Plane", "BroadRes", 14.2f, 0.5f, 0.3f), F("Ser", "Mid", "Comb", "Positive Feedback, Mix 50%", 21.f, 0.85f, 0), F("Ser", "High", "Fast SVF", "Lowpass", 82.f, 0, 0),
            {{"Ser Feedback On", 1}, {"Ser Feedback", 0.4f}, {"Ser Feedback Tone", 26.2f}, {"Sway", 0.4f}, {"Sway Depth", 2}, {"Sway Rate", -5.5f}}}));
        // Morpheus-inspired: Odd > Even morphs a harmonic comb between odd and even partials, slowly, with crushed air
        add18("Crushed Memory", "Pads & Textures", P2({lanesOff, {{"Routing", 0}},
            F("Par", "Low", "Z-Plane", "Hollow > Bright", 2.f, 0.4f, 0.2f), F("Par", "High", "Z-Plane", "Odd > Even", 30.f, 0.5f, 0.5f),
            {{"Par High Drive Type", 6}, {"Par High Drive", 0.35f}, {"Par Low Drive Type", 0}, {"Par Low Drive", 0.25f}, {"Par Noise On", 1}, {"Par Noise", 0.08f}, {"Par Noise Color", 0.7f},
             {"Sway", 0.4f}, {"Sway to Cutoff", 0.3f}, {"Sway to Morph", 1}, {"Sway Rate", -4.f}}}));
        // Morpheus-inspired: the manual's Rubber Hose and Swingshift, set slow so a pad stretches and twists
        add18("Rubber Drone", "Pads & Textures", P2({lanesOff, {{"Routing", 1}},
            F("Ser", "Low", "Z-Plane", "Rubber Hose", 0.f, 0.55f, 0.2f), F("Ser", "Mid", "Fast SVF", "Lowpass", 82.f, 0, 0), F("Ser", "High", "Z-Plane", "Swingshift", 20.f, 0.5f, 0.6f),
            {{"LFO 1 Shape", 6}, {"LFO 1 Sync Rate", 7}, {"LFO 1 Target", 3}, {"LFO 1 Amount", 0.6f},
             {"LFO 2 Shape", 1}, {"LFO 2 Sync Rate", 6}, {"LFO 2 Target", 15}, {"LFO 2 Amount", -0.5f}}}));
        // Morpheus-inspired: Bell Ring partials pulled out of a pad by the Tonal split, the hits pushed back
        add18("Spectral Smear", "Spectral & Tonal", P2({lanesOff, {{"Routing", 0}, {"Parallel Split", 1}, {"Spectral Split By", 1}, {"Spectral Quality", 2}},
            F("Par", "Low", "Z-Plane", "Bell Ring", 14.2f, 0.8f, 0.3f),
            {{"Par Mid Level", -12}, {"Par High Bank Mix", 0.6f}, {"Par High Bank Ripple", 0.5f}, {"Par High Bank Ripple Shape", 3}, {"Par High Bank Stereo", 1},
             {"Sway", 0.4f}, {"Sway to Cutoff", 0}, {"Sway to Morph", 0.8f}}}));

        // ----- Airwindows: 25 built on Chris Johnson's Airwindows filters (Air Z-Plane, Air X-Series, Air Y-Series)
        // Targets: Low 1-6, Mid 7-12, High 13-18 in the order Cutoff, Reso, Morph, Drive, Level, Pan.
        // LFO shapes: Sine 1, Triangle 2, Ramp 3, Square 4, S&H 9. LFO sync: 1/16 0, 1/8 1, 1/4 2, 1/2 3, 1 bar 4, 2 bars 5, 4 bars 6, 8 bars 7.
        // Lane rates: 1/16 2, 1/8 4, 1/4 6, 1/2 7. Cutoffs in semitones from A440: 100 Hz -25.6, 250 Hz -9.8, 500 Hz 2.2, 1 kHz 14.2, 2 kHz 26.2, 4 kHz 38.2, 8 kHz 50.2.
        const char *AWZ = "Air Z-Plane", *AWX = "Air X-Series", *AWY = "Air Y-Series";
        auto addAW = [&](const char *name, V vals) { add18(name, "Airwindows", std::move(vals)); };
        auto open = [&](const char *band) { return F("Ser", band, "Fast SVF", "Lowpass", 82.f, 0, 0); }; // a Series strip left wide open
        const V serial{{"Routing", 1}}, parallel{{"Routing", 0}};
        // the lanes not named are switched off (their starting targets are Drive, Cutoff and Level)
        auto lanesBut = [](const char *keep) { V r; for (const char *b : {"Low", "Mid", "High"}) if (std::strcmp(b, keep)) r.push_back({b[0] == 'L' ? "Seq Low Target" : b[0] == 'M' ? "Seq Mid Target" : "Seq High Target", 0}); return r; };

        // Air Z-Plane (e6400 Ultra style): Resonance drives the clipping, Morph stacks poles
        addAW("e6400 Ultra Sweep", P2({lanesOff, serial,
            F("Ser", "Low", AWZ, "ZLowpass", 20.f, 0.3f, 0.66f), open("Mid"), open("High"),
            {{"LFO 1 Shape", 1}, {"LFO 1 Sync Rate", 5}, {"LFO 1 Target", 1}, {"LFO 1 Amount", 0.5f}}}));
        addAW("Ultra Three-Band", P2({lanesOff, parallel,
            F("Par", "Low", AWZ, "ZLowpass", -10.f, 0.3f, 0.66f), F("Par", "Mid", AWZ, "ZBandpass", 14.2f, 0.3f, 0.33f), F("Par", "High", AWZ, "ZHighpass", 38.f, 0.3f, 0.33f),
            {{"Sway", 0.35f}, {"Sway to Cutoff", 0.6f}, {"Sway Rate", -3.5f}}}));
        addAW("Shark Teeth", P2({lanesBut("Low"), serial,
            F("Ser", "Low", AWZ, "ZAcidLowpass", 26.f, 0.5f, 0.4f), open("Mid"), open("High"),
            {{"Seq Low Target", 3}, {"Seq Low Rate", 2}, {"Seq Low Length", 16}, {"Seq Low Smooth", 0.1f},
             {"Seq Low Step 1", 0.9f}, {"Seq Low Step 4", -0.4f}, {"Seq Low Step 7", 0.7f}, {"Seq Low Step 9", -0.6f}, {"Seq Low Step 11", 0.8f}, {"Seq Low Step 15", 0.5f}}}));
        addAW("Acid Meltdown", P2({lanesOff, serial,
            F("Ser", "Low", AWZ, "ZAcidLowpass", 14.f, 0.7f, 0.6f), open("Mid"), open("High"),
            {{"LFO 1 Shape", 9}, {"LFO 1 Sync Rate", 1}, {"LFO 1 Target", 3}, {"LFO 1 Amount", 0.5f},
             {"LFO 2 Shape", 2}, {"LFO 2 Sync Rate", 5}, {"LFO 2 Target", 1}, {"LFO 2 Amount", 0.4f}}}));
        addAW("Phaser Notch Drift", P2({lanesOff, parallel,
            F("Par", "Mid", AWZ, "ZNotch", 14.2f, 0.2f, 0.5f), F("Par", "High", AWZ, "ZNotch", 38.f, 0.2f, 0.3f),
            {{"Sway", 0.6f}, {"Sway to Cutoff", 1}, {"Sway Rate", -3.f}, {"Sway Low Depth", 0}}}));
        addAW("Ultra Bandpass Talk", P2({lanesBut("Mid"), parallel,
            F("Par", "Mid", AWZ, "ZBandpass", 14.2f, 0.4f, 0.33f),
            {{"Seq Mid Target", 7}, {"Seq Mid Rate", 4}, {"Seq Mid Length", 8}, {"Seq Mid Smooth", 0.3f},
             {"Seq Mid Step 1", 0.6f}, {"Seq Mid Step 3", -0.5f}, {"Seq Mid Step 4", 0.9f}, {"Seq Mid Step 6", -0.3f}, {"Seq Mid Step 7", 0.4f}}}));
        addAW("Region Smear", P2({lanesOff, parallel,
            F("Par", "Mid", AWZ, "ZRegion", 26.f, 0.6f, 0.5f), F("Par", "High", AWZ, "ZHighpass", 40.f, 0.2f, 0.33f),
            {{"Sway", 0.5f}, {"Sway to Cutoff", 0.4f}, {"Sway to Morph", 1}, {"Sway to Reso", 0.5f}, {"Sway Rate", -4.f}}}));
        addAW("Sampler Pump", P2({lanesOff, serial,
            F("Ser", "Low", AWZ, "ZLowpass", 8.f, 0.5f, 0.33f), open("Mid"), open("High"),
            {{"LFO 1 Shape", 3}, {"LFO 1 Sync Rate", 2}, {"LFO 1 Target", 1}, {"LFO 1 Amount", 0.5f}, {"Ser Low Drive Type", 0}, {"Ser Low Drive", 0.2f}, {"Ser Low Level", 3.f}}}));

        // Air X-Series (distorted retro-sampler EQ): Resonance is Gain into the distortion, Morph is Nuke
        addAW("Retro Sampler EQ", P2({lanesOff, parallel,
            F("Par", "Low", AWX, "XLowpass", -6.f, 0.3f, 0), F("Par", "Mid", AWX, "XBandpass", 14.2f, 0.2f, 0.1f), F("Par", "High", AWX, "XHighpass", 40.f, 0.2f, 0),
            {{"Sway", 0.25f}, {"Sway to Cutoff", 0.5f}}}));
        addAW("Nuked Bass", P2({lanesOff, serial,
            F("Ser", "Low", AWX, "XLowpass", 6.f, 0.15f, 0.55f), open("Mid"), open("High"),
            {{"LFO 1 Shape", 1}, {"LFO 1 Sync Rate", 4}, {"LFO 1 Target", 3}, {"LFO 1 Amount", 0.25f}, {"Ser Low Level", 12.f}}}));
        addAW("Nuke Locks", P2({lanesOff, serial,
            F("Ser", "Low", AWX, "XLowpass", 18.f, 0.4f, 0), open("Mid"), open("High"),
            locks("Low", "Morph", {{1, 0.f}, {3, 0.6f}, {5, 0.2f}, {7, 0.9f}, {9, 0.f}, {11, 0.5f}, {13, 0.3f}, {15, 1.f}}),
            locks("Low", "Cutoff", {{1, cutPos(18.f)}, {7, cutPos(30.f)}, {13, cutPos(12.f)}}),
            {{"Lock Low Glide", 10}, {"Ser Low Level", 4.f}}}));
        addAW("Glitch Region", P2({lanesOff, parallel,
            F("Par", "Mid", AWX, "XRegion", 26.f, 0.6f, 0.5f),
            {{"Chaperone Influence", 0.7f}, {"Chaperone Grid", 5}, {"Chaperone Chaos", 0.5f}, {"Chaperone Range", 12.f}, {"Chaperone Snap", 1}, {"3rd Wheel", 0.4f}}}));
        addAW("Mangled Region Drift", P2({lanesOff, parallel,
            F("Par", "Low", AWX, "XRegion", 0.f, 0.8f, 0.6f), F("Par", "High", AWX, "XRegion", 38.f, 0.5f, 0.3f),
            {{"Sway", 0.55f}, {"Sway to Cutoff", 0.3f}, {"Sway to Morph", 1}, {"Sway Rate", -3.8f}, {"Par Low Mix", 0.8f}}}));
        addAW("Crunchy Hats", P2({lanesBut("High"), parallel,
            F("Par", "High", AWX, "XHighpass", 46.f, 0.6f, 0.4f),
            {{"Seq High Target", 13}, {"Seq High Rate", 2}, {"Seq High Length", 16}, {"Seq High Smooth", 0.05f},
             {"Seq High Step 1", 0.5f}, {"Seq High Step 3", -0.3f}, {"Seq High Step 5", 0.7f}, {"Seq High Step 8", -0.5f}, {"Seq High Step 11", 0.6f}, {"Seq High Step 14", -0.2f}}}));
        addAW("Distorted Notch Sweep", P2({lanesOff, serial,
            F("Ser", "Low", AWX, "XNotch", 14.f, 0.3f, 0.5f), open("Mid"), open("High"),
            {{"LFO 1 Shape", 2}, {"LFO 1 Sync Rate", 5}, {"LFO 1 Target", 1}, {"LFO 1 Amount", 0.6f}}}));
        addAW("Broken Telephone", P2({lanesOff, serial,
            F("Ser", "Low", AWX, "XBandpass", 20.f, 0.5f, 0.5f), open("Mid"), open("High"),
            {{"Sway", 0.3f}, {"Sway to Cutoff", 0.4f}, {"Sway to Morph", 0.6f}, {"Ser Low Drive Type", 2}, {"Ser Low Drive", 0.25f}}}));

        // Air Y-Series (soft to nasty): Resonance is Reson8, Morph is ResEdge (about 10% is neutral)
        addAW("Smooth Resonance", P2({lanesOff, serial,
            F("Ser", "Low", AWY, "YLowpass", 20.f, 0.5f, 0.1f), open("Mid"), open("High"),
            {{"Sway", 0.45f}, {"Sway to Cutoff", 1}, {"Sway Rate", -4.f}}}));
        addAW("Nasty Edge", P2({lanesOff, serial,
            F("Ser", "Low", AWY, "YLowpass", 14.f, 0.75f, 0.9f), open("Mid"), open("High"),
            {{"LFO 1 Shape", 1}, {"LFO 1 Sync Rate", 4}, {"LFO 1 Target", 1}, {"LFO 1 Amount", 0.45f},
             {"LFO 2 Shape", 2}, {"LFO 2 Sync Rate", 6}, {"LFO 2 Target", 3}, {"LFO 2 Amount", 0.4f}}}));
        addAW("Soft Round Air", P2({lanesOff, parallel,
            F("Par", "High", AWY, "YHighpass", 34.f, 0.4f, 0.f),
            {{"Sway", 0.3f}, {"Sway to Cutoff", 0.5f}, {"Sway to Pan", 0.5f}, {"Sway Low Depth", 0}, {"Sway Mid Depth", 0}}}));
        addAW("Edgy Bandpass Talk", P2({lanesBut("Mid"), parallel,
            F("Par", "Mid", AWY, "YBandpass", 14.2f, 0.5f, 0.6f),
            {{"Seq Mid Target", 7}, {"Seq Mid Rate", 4}, {"Seq Mid Length", 8}, {"Seq Mid Smooth", 0.25f},
             {"Seq Mid Step 1", 0.8f}, {"Seq Mid Step 2", -0.4f}, {"Seq Mid Step 4", 0.5f}, {"Seq Mid Step 5", -0.8f}, {"Seq Mid Step 7", 0.3f}}}));
        addAW("Notch Breathing", P2({lanesOff, serial,
            F("Ser", "Low", AWY, "YNotch", 14.2f, 0.4f, 0.2f), open("Mid"), open("High"),
            {{"Sway", 0.5f}, {"Sway to Cutoff", 1}, {"Sway to Reso", 0.4f}, {"Sway Rate", -3.5f}}}));
        addAW("Resonant Locks", P2({lanesOff, serial,
            F("Ser", "Low", AWY, "YLowpass", 8.f, 0.7f, 0.3f), open("Mid"), open("High"),
            locks("Low", "Cutoff", {{1, cutPos(2.f)}, {3, cutPos(26.f)}, {6, cutPos(-4.f)}, {9, cutPos(32.f)}, {12, cutPos(10.f)}, {15, cutPos(20.f)}}),
            locks("Low", "Morph", {{3, 0.8f}, {9, 0.95f}, {12, 0.1f}}),
            {{"Lock Low Glide", 40}}}));
        addAW("Texture Morph", P2({lanesOff, parallel,
            F("Par", "Low", AWY, "YLowpass", -8.f, 0.6f, 0.1f), F("Par", "Mid", AWY, "YBandpass", 14.2f, 0.6f, 0.5f), F("Par", "High", AWY, "YHighpass", 36.f, 0.6f, 0.9f),
            {{"LFO 1 Shape", 1}, {"LFO 1 Sync Rate", 6}, {"LFO 1 Target", 9}, {"LFO 1 Amount", 0.5f},
             {"LFO 2 Shape", 2}, {"LFO 2 Sync Rate", 7}, {"LFO 2 Target", 15}, {"LFO 2 Amount", -0.4f}}}));
        addAW("Resonant Escort", P2({lanesOff, parallel,
            F("Par", "Low", AWY, "YLowpass", 2.f, 0.5f, 0.1f), F("Par", "Mid", AWY, "YLowpass", 20.f, 0.7f, 0.2f), F("Par", "High", AWY, "YHighpass", 32.f, 0.5f, 0.1f),
            {{"LFO 1 Shape", 1}, {"LFO 1 Sync Rate", 4}, {"LFO 1 Target", 7}, {"LFO 1 Amount", 0.5f},
             {"Chaperone Influence", 0.7f}, {"Chaperone Grid", 8}, {"Chaperone Snap", 1}, {"Chaperone Range", 12.f}}}));

        // all three families at once
        addAW("Airwindows Trio", P2({lanesOff, parallel,
            F("Par", "Low", AWZ, "ZLowpass", -6.f, 0.3f, 0.33f), F("Par", "Mid", AWX, "XBandpass", 14.2f, 0.3f, 0.25f), F("Par", "High", AWY, "YHighpass", 34.f, 0.5f, 0.5f),
            {{"Sway", 0.35f}, {"Sway to Cutoff", 0.5f}, {"Sway to Morph", 0.6f},
             {"Mutate", 0.5f}, {"Mutation Trigger", 4}, {"Mutation Low Answer", 1}, {"Mutation High Answer", 2}}}));
        return v;
    }();
    return all;
}
// how many of the factory presets were made before 0.18 (they load with the old mod and ripple behaviour)
size_t &presetLegacyCount() { static size_t n = 0; return n; }

// A factory preset as state text: every control back to its start, then the preset's values.
// "<name>#idx" gives a filter variant by number (turned into the knob's 0..1 for its model).
static std::string presetText(size_t index)
{
    const auto &ps = presetList();
    std::string t = index < presetLegacyCount() ? "THREESCOMPANY 1 zcur\n" : "THREESCOMPANY 3\n"; // factory presets are built with the current Z-Plane list
    if (index >= ps.size())
        return t;
    const bool classic = ps[index].group[0] == '~';
    const auto &d = paramDefs();
    auto idOf = [&](const std::string &n) -> int {
        for (size_t i = 0; i < d.size(); ++i)
            if (d[i].name == n)
                return (int)i;
        return -1;
    };
    std::map<std::string, float> got;
    for (auto &kv : ps[index].values)
        got[kv.first] = kv.second;
    // The classic set has two filters. The Mid strip keeps its starting filter (Fast SVF, wide open): it is
    // in place and switched on but changes nothing, so the middle of the sound is never missing.
    // In Parallel the Mid band is at least an octave wide around the point between the two filters.
    if (got.count("Par Split Low/Mid") && got.count("Par Split Mid/High"))
    {
        float lo = got["Par Split Low/Mid"], hi = got["Par Split Mid/High"];
        if (hi - lo < 12.f)
        {
            float c = 0.5f * (lo + hi);
            got["Par Split Low/Mid"] = std::clamp(c - 6.f, -41.5f, 26.2f);
            got["Par Split Mid/High"] = std::clamp(c + 6.f, -1.6f, 62.2f);
        }
    }
    char b[64];
    for (auto &kv : ps[index].values)
    {
        std::string n = kv.first;
        float v = got.count(n) ? got[n] : kv.second;
        if (n.rfind("Conductor ", 0) == 0)
            n = "Chaperone " + n.substr(10); // the Conductor's old names, in presets made before it was renamed
        if (classic && (n == "Par Mid Filter Type" || n == "Ser Mid Filter Type"))
            continue; // classic presets: the Mid strip keeps its open, neutral filter
        if (n.size() > 4 && n.compare(n.size() - 4, 4, "#idx") == 0)
        {
            n.resize(n.size() - 4);
            std::string typeName = n.substr(0, n.size() - std::string("Variant").size()) + "Type";
            int model = got.count(typeName) ? (int)got[typeName] : 0;
            const auto &cat = filterCatalogue();
            int nv = model > 0 && model < (int)cat.size() ? (int)cat[(size_t)model].labels.size() : 1;
            v = nv > 1 ? std::clamp(v, 0.f, (float)(nv - 1)) / (float)(nv - 1) : 0.f;
        }
        int i = idOf(n);
        if (i < 0)
        {
            if (std::getenv("TC_PRESET_CHECK"))
                std::fprintf(stderr, "preset %s: no parameter named %s\n", ps[index].name, n.c_str());
            continue;
        }
        snprintf(b, sizeof(b), "%u %.9g\n", (unsigned)d[(size_t)i].id, (double)v);
        t += b;
    }
    return t;
}
// Loads a factory preset (from the window): values, curves, then tells Bitwig to re-read everything
static bool loadPreset(Plugin *pl, size_t index)
{
    if (!loadStateText(pl, presetText(index)))
        return false;
    if (pl->hostParams && pl->hostParams->rescan)
        pl->hostParams->rescan(pl->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
    pl->markDirty();
    return true;
}

// ----- audio ports: stereo in, stereo out ---------------------------------------
// stereo in, a stereo sidechain in (for the follower), stereo out
uint32_t portsCount(const clap_plugin_t *, bool isInput) { return isInput ? 2 : 1; }
bool portsGet(const clap_plugin_t *, uint32_t index, bool isInput, clap_audio_port_info_t *info)
{
    if (index >= (isInput ? 2u : 1u))
        return false;
    std::memset(info, 0, sizeof(*info));
    if (isInput && index == 1)
    {
        info->id = 2;
        snprintf(info->name, sizeof(info->name), "%s", "Sidechain");
        info->flags = 0;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }
    info->id = isInput ? 0 : 1;
    snprintf(info->name, sizeof(info->name), "%s", isInput ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t extPorts = {portsCount, portsGet};

// ----- the plugin itself ------------------------------------------------------------
bool plInit(const clap_plugin_t *p)
{
    auto *pl = self(p);
    if (pl->host && pl->host->get_extension)
    {
        pl->hostParams = (const clap_host_params_t *)pl->host->get_extension(pl->host, CLAP_EXT_PARAMS);
        pl->hostState = (const clap_host_state_t *)pl->host->get_extension(pl->host, CLAP_EXT_STATE);
    }
    for (int i = 0; i < numLfos; ++i)
        pl->applyCurve(i);
    return true;
}
void plDestroy(const clap_plugin_t *p)
{
    auto *pl = self(p);
    if (pl->gui)
        tcgui::destroy(pl->gui);
    pl->deleteBridge();
    delete pl;
}
// the spectral split delays the sound by one FFT frame (2048 samples); the host is told so it can line it up
static int wantedLatency(Plugin *pl)
{
    auto v = [&](clap_id id) { auto it = paramIndex().find(id); return it == paramIndex().end() ? 0.0 : pl->value[(size_t)it->second].load(std::memory_order_relaxed); };
    return (std::lround(v(idRouting)) == 0 && std::lround(v(idSplitMode)) == 1) ? (1024 << std::clamp((int)std::lround(v(idSpecQuality)), 0, 3)) : 0;
}
uint32_t latencyGet(const clap_plugin_t *p) { return (uint32_t)self(p)->latencyReported.load(); }
const clap_plugin_latency_t extLatency = {latencyGet};
bool plActivate(const clap_plugin_t *p, double sr, uint32_t, uint32_t maxFrames)
{
    auto *pl = self(p);
    pl->latencyReported.store(wantedLatency(pl));
    pl->restartAsked.store(false);
    pl->sampleRate = sr;
    pl->maxBlock = maxFrames;
    pl->engine.setSampleRate(sr);
    size_t n = std::max<uint32_t>(maxFrames, 64);
    pl->bufL.assign(n, 0.f);
    pl->bufR.assign(n, 0.f);
    pl->scBufL.assign(n, 0.f);
    pl->scBufR.assign(n, 0.f);
    pl->dirty.store(true, std::memory_order_relaxed);
    return true;
}
void plDeactivate(const clap_plugin_t *) {}
bool plStart(const clap_plugin_t *) { return true; }
void plStop(const clap_plugin_t *) {}
void plReset(const clap_plugin_t *p)
{
    auto *pl = self(p);
    pl->engine.reset();
    std::fill(pl->mod.begin(), pl->mod.end(), 0.f);
    pl->dirty.store(true, std::memory_order_relaxed);
}

clap_process_status plProcess(const clap_plugin_t *p, const clap_process_t *proc)
{
    auto *pl = self(p);
    ScopedNoDenormals noDenormals;
    // the spectral split was switched on or off (or its quality changed): ask the host to restart us so it hears the new delay
    if (wantedLatency(pl) != pl->latencyReported.load() && !pl->restartAsked.exchange(true) && pl->host && pl->host->request_restart)
        pl->host->request_restart(pl->host);
    const uint32_t frames = proc->frames_count;
    const float *inL = nullptr, *inR = nullptr;
    if (proc->audio_inputs_count > 0 && proc->audio_inputs[0].data32 && proc->audio_inputs[0].channel_count > 0)
    {
        inL = proc->audio_inputs[0].data32[0];
        inR = proc->audio_inputs[0].channel_count > 1 ? proc->audio_inputs[0].data32[1] : inL;
    }
    float *outL = nullptr, *outR = nullptr;
    if (proc->audio_outputs_count > 0 && proc->audio_outputs[0].data32 && proc->audio_outputs[0].channel_count > 0)
    {
        outL = proc->audio_outputs[0].data32[0];
        outR = proc->audio_outputs[0].channel_count > 1 ? proc->audio_outputs[0].data32[1] : nullptr;
    }

    // Bitwig's tempo and song position: Sway sync, the sixteenth-step trigger and Mutate follow them
    if (const auto *t = proc->transport)
    {
        HostTime ht;
        ht.playing = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
        if (t->flags & CLAP_TRANSPORT_HAS_TEMPO)
            ht.bpm = t->tempo;
        if (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
        {
            ht.beatsValid = true;
            ht.beats = (double)t->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
        }
        if (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE)
        {
            ht.num = t->tsig_num;
            ht.den = t->tsig_denom;
        }
        pl->engine.setHostTime(ht);
    }
    pl->sendGuiEvents(proc->out_events);
    const float *scL = nullptr, *scR = nullptr;
    if (proc->audio_inputs_count > 1 && proc->audio_inputs[1].data32 && proc->audio_inputs[1].channel_count > 0)
    {
        scL = proc->audio_inputs[1].data32[0];
        scR = proc->audio_inputs[1].channel_count > 1 ? proc->audio_inputs[1].data32[1] : scL;
    }
    const uint32_t nEvents = proc->in_events ? proc->in_events->size(proc->in_events) : 0;
    uint32_t ev = 0, pos = 0;
    const uint32_t cap = (uint32_t)pl->bufL.size();
    while (pos < frames)
    {
        // take in every change due at or before this point, then run up to the next one
        while (ev < nEvents)
        {
            const clap_event_header_t *h = proc->in_events->get(proc->in_events, ev);
            if (h->time > pos)
                break;
            pl->handleEvent(h);
            ++ev;
        }
        uint32_t until = frames;
        if (ev < nEvents)
            until = std::min(until, proc->in_events->get(proc->in_events, ev)->time);
        uint32_t n = std::min(std::max(until, pos + 1) - pos, cap);
        n = std::min(n, frames - pos);
        if (pl->dirty.exchange(false, std::memory_order_relaxed))
            pl->buildSettings();
        for (uint32_t i = 0; i < n; ++i)
        {
            pl->bufL[i] = inL ? inL[pos + i] : 0.f;
            pl->bufR[i] = inR ? inR[pos + i] : 0.f;
        }
        if (scL)
            for (uint32_t i = 0; i < n; ++i)
                pl->scBufL[i] = scL[pos + i], pl->scBufR[i] = scR[pos + i];
        pl->engine.process(pl->bufL.data(), pl->bufR.data(), pl->bufL.data(), pl->bufR.data(), (int)n, pl->settings,
                           scL ? pl->scBufL.data() : nullptr, scL ? pl->scBufR.data() : nullptr);
        if (outL && outR)
        {
            std::memcpy(outL + pos, pl->bufL.data(), n * sizeof(float));
            std::memcpy(outR + pos, pl->bufR.data(), n * sizeof(float));
        }
        else if (outL)
            for (uint32_t i = 0; i < n; ++i)
                outL[pos + i] = 0.5f * (pl->bufL[i] + pl->bufR[i]);
        pos += n;
    }
    while (ev < nEvents)
        pl->handleEvent(proc->in_events->get(proc->in_events, ev++));
    pl->readMeters();
    return CLAP_PROCESS_CONTINUE;
}

// ----- the window's bridge ---------------------------------------------------------------
std::string jsonEsc(const std::string &in);
static std::string presetDirJson()
{
    return std::string("{\"path\":\"") + jsonEsc(pathText(userPresetDir())) + "\",\"custom\":" + (loadPrefs().presetDir.empty() ? "false" : "true") + "}";
}
static std::string userPresetJson()
{
    std::string o = "[";
    auto names = userPresetNames();
    for (size_t i = 0; i < names.size(); ++i)
        o += (i ? ",\"" : "\"") + jsonEsc(names[i]) + "\"";
    return o + "]";
}
std::string jsonEsc(const std::string &in)
{
    std::string o;
    o.reserve(in.size() + 2);
    for (unsigned char c : in)
    {
        if (c == '"' || c == '\\')
            o += '\\', o += (char)c;
        else if (c < 0x20)
        {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        }
        else if (c == '<')
            o += "\\u003c"; // never let a name close a script tag
        else
            o += (char)c;
    }
    return o;
}
std::string num(double v)
{
    if (!std::isfinite(v))
        return "0";
    char b[32];
    snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

struct Plugin::GuiBridge : tcgui::Bridge
{
    Plugin *pl;
    bool needInit{true};
    std::vector<double> lastV;
    std::vector<std::string> lastT;
    std::string out; // kept for the test hook
    int view{1};     // what the top display shows: 0 curves only, 1 spectrum, 2 scope
    float specHold[4][96]{};

    explicit GuiBridge(Plugin *p) : pl(p)
    {
        for (auto &r : specHold)
            for (auto &v : r)
                v = -120.f;
    }

    // radix-2 FFT in place (n a power of two)
    static void fft(std::vector<float> &re, std::vector<float> &im)
    {
        const size_t n = re.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(re[i], re[j]), std::swap(im[i], im[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            double a = -2.0 * 3.14159265358979 / (double)len;
            float wr = (float)std::cos(a), wi = (float)std::sin(a);
            for (size_t i = 0; i < n; i += len)
            {
                float cr = 1.f, ci = 0.f;
                for (size_t k = 0; k < len / 2; ++k)
                {
                    size_t u = i + k, v = u + len / 2;
                    float tr = re[v] * cr - im[v] * ci, ti = re[v] * ci + im[v] * cr;
                    re[v] = re[u] - tr, im[v] = im[u] - ti;
                    re[u] += tr, im[u] += ti;
                    float ncr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr, cr = ncr;
                }
            }
        }
    }
    // spectrum of the three strips and the output: 96 points from 20 Hz to 20 kHz, in dB
    std::string spectrum()
    {
        const int N = 2048;
        std::vector<float> buf(N), re(N), im(N);
        double sr = pl->sampleRate > 0 ? pl->sampleRate : 48000.0;
        std::string o = "[";
        for (int w = 0; w < 4; ++w)
        {
            pl->engine.readAnalyser(w, buf.data(), N);
            for (int i = 0; i < N; ++i)
            {
                float hann = 0.5f - 0.5f * (float)std::cos(2.0 * 3.14159265358979 * i / (N - 1));
                re[i] = buf[i] * hann, im[i] = 0.f;
            }
            fft(re, im);
            o += w ? ",[" : "[";
            for (int k = 0; k < 96; ++k)
            {
                double f0 = 20.0 * std::pow(1000.0, k / 96.0), f1 = 20.0 * std::pow(1000.0, (k + 1) / 96.0);
                int b0 = std::max(1, (int)std::floor(f0 * N / sr)), b1 = std::max(b0, (int)std::ceil(f1 * N / sr));
                float pk = 0.f;
                for (int bi = b0; bi <= b1 && bi < N / 2; ++bi)
                    pk = std::max(pk, re[bi] * re[bi] + im[bi] * im[bi]);
                // a full-scale sine reads about 0 dB (Hann window: sum = N / 2)
                float db = 10.f * std::log10(pk + 1e-20f) - 20.f * std::log10((float)N / 4.f);
                float &h = specHold[w][k];
                h = db > h ? db : std::max(db, h - 2.5f); // falls back slowly
                o += (k ? "," : "") + std::to_string((int)std::lround(std::clamp(h, -120.f, 12.f)));
            }
            o += "]";
        }
        return o + "]";
    }
    // the three strips and the output as waveforms, steadied on a rising zero crossing of the output
    std::string scope()
    {
        const int N = 4096, W = 1920, P = 240;
        std::vector<float> b[4];
        for (int w = 0; w < 4; ++w)
            b[w].resize(N), pl->engine.readAnalyser(w, b[w].data(), N);
        int start = N - W;
        for (int i = N - W - 1; i > N - W - 1800 && i > 1; --i)
            if (b[3][i - 1] < 0.f && b[3][i] >= 0.f)
            {
                start = i;
                break;
            }
        std::string o = "[";
        for (int w = 0; w < 4; ++w)
        {
            o += w ? ",[" : "[";
            for (int k = 0; k < P; ++k)
            {
                // the largest swing in each slice, with its sign, so peaks are not lost
                int i0 = start + k * W / P, i1 = start + (k + 1) * W / P;
                float best = 0.f;
                for (int i = i0; i < i1 && i < N; ++i)
                    if (std::fabs(b[w][i]) > std::fabs(best))
                        best = b[w][i];
                char t[16];
                snprintf(t, sizeof(t), "%.3f", std::clamp(best, -2.f, 2.f));
                o += (k ? "," : "") + std::string(t);
            }
            o += "]";
        }
        return o + "]";
    }
    const char *html() override { return tcGuiHtml; }

    void setValue(int i, double v)
    {
        const auto &d = paramDefs()[(size_t)i];
        if (!std::isfinite(v))
            return;
        v = std::clamp(v, d.min, d.max);
        if (isStepped(d.kind))
            v = std::round(v);
        pl->value[(size_t)i].store((float)v, std::memory_order_relaxed);
        pl->dirty.store(true, std::memory_order_relaxed);
        pl->push(0, d.id, v);
        pl->requestFlush();
    }

    void onMessage(const char *msg) override
    {
        if (!msg)
            return;
        std::string m = msg;
        char cmd[16] = {0};
        unsigned id = 0;
        double v = 0;
        if (m == "ready")
        {
            needInit = true;
            return;
        }
        if (m.rfind("zoom ", 0) == 0)
        {
            // a size step from the window's - / + buttons: ask the host for that size (same shape as always)
            double z = std::clamp(std::atof(m.c_str() + 5), 0.5, 2.0);
            uint32_t w = (uint32_t)std::lround(1280 * z * pl->guiScale), h = (uint32_t)std::lround(940 * z * pl->guiScale);
            auto *hg = pl->host && pl->host->get_extension ? (const clap_host_gui_t *)pl->host->get_extension(pl->host, CLAP_EXT_GUI) : nullptr;
            if (hg && hg->request_resize && hg->request_resize(pl->host, w, h))
            {
                pl->guiW = (uint32_t)std::lround(1280 * z), pl->guiH = (uint32_t)std::lround(940 * z);
                if (pl->gui)
                    tcgui::setSize(pl->gui, (int)w, (int)h);
                pl->markDirty();
            }
            return;
        }
        if (m.rfind("prefs ", 0) == 0) // "prefs <size %> <dark 0/1> <tips 0/1>": this computer's defaults
        {
            Prefs p;
            int sz = 100, dk = 0, tp = 1;
            if (std::sscanf(m.c_str() + 6, "%d %d %d", &sz, &dk, &tp) == 3)
            {
                p = loadPrefs(); // keep the preset folder
                p.sizePct = std::clamp(sz, 60, 200), p.dark = dk != 0, p.tips = tp != 0;
                savePrefs(p);
            }
            return;
        }
        if (m.rfind("theme ", 0) == 0)
        {
            pl->darkTheme.store(m.compare(6, 4, "dark") == 0);
            pl->markDirty();
            return;
        }
        if (m.rfind("ctx ", 0) == 0) // the window asks for Bitwig's menu items for a knob
        {
            clap_id id = (clap_id)std::strtoul(m.c_str() + 4, nullptr, 10);
            MenuCollect c;
            if (auto *hm = hostMenu(); hm && hm->populate)
            {
                clap_context_menu_target_t t{CLAP_CONTEXT_MENU_TARGET_KIND_PARAM, id};
                clap_context_menu_builder_t b{&c, menuAdd, menuSupports};
                hm->populate(pl->host, &t, &b);
            }
            pendingJs += "tc.ctx(" + std::to_string(id) + ",[" + c.json + "]);";
            return;
        }
        if (m.rfind("ctxdo ", 0) == 0) // ... and runs the one picked
        {
            unsigned long id = 0, action = 0;
            if (std::sscanf(m.c_str() + 6, "%lu %lu", &id, &action) == 2)
                if (auto *hm = hostMenu(); hm && hm->perform)
                {
                    clap_context_menu_target_t t{CLAP_CONTEXT_MENU_TARGET_KIND_PARAM, (clap_id)id};
                    hm->perform(pl->host, &t, (clap_id)action);
                }
            return;
        }
        if (m == "ufolder pick")
        {
            pickFolder = true; // done from poll(): a system dialog should not open inside the web view's message callback
            return;
        }
        if (m == "ufolder default" || m == "ufolder show")
        {
            if (m == "ufolder default")
            {
                Prefs p = loadPrefs();
                p.presetDir.clear();
                savePrefs(p);
                pendingJs += "tc.user(" + userPresetJson() + "," + presetDirJson() + ");";
            }
            else
            {
                std::error_code ec;
                fs::create_directories(userPresetDir(), ec);
                tcgui::reveal(pathText(userPresetDir()));
            }
            return;
        }
        if (m.rfind("usave ", 0) == 0 || m.rfind("uload ", 0) == 0 || m.rfind("udel ", 0) == 0) // your own presets
        {
            const bool save = m[1] == 's', load = m[1] == 'l';
            std::string name = cleanPresetName(m.substr(m.find(' ') + 1));
            if (name.empty())
                return;
            if (save)
                saveUserPreset(pl, name);
            else if (load)
                loadUserPreset(pl, name);
            else
                deleteUserPreset(name);
            if (!load)
                pendingJs += "tc.user(" + userPresetJson() + "," + presetDirJson() + ");";
            return;
        }
        if (m.rfind("preset ", 0) == 0)
        {
            int k = std::atoi(m.c_str() + 7);
            if (k >= 0)
                loadPreset(pl, (size_t)k);
            return;
        }
        if (m.rfind("view ", 0) == 0)
        {
            view = std::clamp(std::atoi(m.c_str() + 5), 0, 2);
            return;
        }
        if (m.rfind("curve ", 0) == 0)
        {
            int lfo = std::atoi(m.c_str() + 6);
            if (parseCurveLine(pl, m.c_str() + 6) && lfo >= 0 && lfo < numLfos)
            {
                pl->applyCurve(lfo);
                pl->markDirty();
            }
            return;
        }
        int got = sscanf(m.c_str(), "%15s %u %lf", cmd, &id, &v);
        if (got < 2)
            return;
        auto it = paramIndex().find((clap_id)id);
        if (it == paramIndex().end())
            return;
        std::string c = cmd;
        if (c == "set" && got == 3)
            setValue(it->second, v);
        else if (c == "reset")
            setValue(it->second, paramDefs()[(size_t)it->second].def);
        else if (c == "begin")
            pl->push(1, (clap_id)id, 0), pl->requestFlush();
        else if (c == "end")
            pl->push(2, (clap_id)id, 0), pl->requestFlush();
    }

    // Bitwig's own menu for a knob (automation, modulators ...), fetched through CLAP's context-menu
    // extension and shown inside our own right-click menu
    std::string pendingJs;
    bool pickFolder{false}, inPicker{false};
    const clap_host_context_menu_t *hostMenu()
    {
        if (!pl->host || !pl->host->get_extension)
            return nullptr;
        auto *m = (const clap_host_context_menu_t *)pl->host->get_extension(pl->host, CLAP_EXT_CONTEXT_MENU);
        if (!m)
            m = (const clap_host_context_menu_t *)pl->host->get_extension(pl->host, CLAP_EXT_CONTEXT_MENU_COMPAT);
        return m;
    }
    struct MenuCollect
    {
        std::string json;
        int n{0};
        std::string prefix;
    };
    static bool menuAdd(const clap_context_menu_builder_t *b, clap_context_menu_item_kind_t kind, const void *data)
    {
        auto *c = (MenuCollect *)b->ctx;
        auto item = [&](const char *label, bool enabled, long action, int type) {
            c->json += (c->n++ ? "," : "") + std::string("[\"") + jsonEsc(c->prefix + (label ? label : "")) + "\"," + std::to_string(action) + "," +
                       (enabled ? "1" : "0") + "," + std::to_string(type) + "]";
        };
        switch (kind)
        {
        case CLAP_CONTEXT_MENU_ITEM_ENTRY:
        {
            auto *e = (const clap_context_menu_entry_t *)data;
            item(e->label, e->is_enabled, (long)e->action_id, 0);
            break;
        }
        case CLAP_CONTEXT_MENU_ITEM_CHECK_ENTRY:
        {
            auto *e = (const clap_context_menu_check_entry_t *)data;
            item(e->label, e->is_enabled, (long)e->action_id, e->is_checked ? 2 : 1);
            break;
        }
        case CLAP_CONTEXT_MENU_ITEM_SEPARATOR: item("", false, -1, 3); break;
        case CLAP_CONTEXT_MENU_ITEM_TITLE:
        {
            auto *t = (const clap_context_menu_item_title_t *)data;
            item(t->title, false, -1, 4);
            break;
        }
        case CLAP_CONTEXT_MENU_ITEM_BEGIN_SUBMENU:
        {
            auto *m = (const clap_context_menu_submenu_t *)data;
            c->prefix = std::string(m->label ? m->label : "") + " › ";
            break;
        }
        case CLAP_CONTEXT_MENU_ITEM_END_SUBMENU: c->prefix.clear(); break;
        default: break;
        }
        return true;
    }
    static bool menuSupports(const clap_context_menu_builder_t *, clap_context_menu_item_kind_t) { return true; }

    std::string poll() override
    {
        // the folder chooser for your presets (asked for by the page; opened here, from the window's timer)
        if (pickFolder && !inPicker)
        {
            pickFolder = false, inPicker = true;
            std::string chosen = tcgui::chooseFolder(pl->gui, pathText(userPresetDir()));
            inPicker = false;
            if (!chosen.empty())
            {
                Prefs p = loadPrefs();
                p.presetDir = chosen;
                savePrefs(p);
                pendingJs += "tc.user(" + userPresetJson() + "," + presetDirJson() + ");";
            }
        }
        if (inPicker)
            return std::string();
        const auto &d = paramDefs();
        if (lastV.size() != d.size())
            lastV.assign(d.size(), 1.0e300), lastT.assign(d.size(), std::string());
        std::string o;
        if (!pendingJs.empty())
            o += pendingJs, pendingJs.clear();
        if (needInit)
        {
            needInit = false;
            o = "tc.init({\"params\":[";
            for (size_t i = 0; i < d.size(); ++i)
            {
                double v = pl->value[i].load(std::memory_order_relaxed);
                std::string t = valueText(pl, d[i], v);
                lastV[i] = v, lastT[i] = t;
                if (i)
                    o += ",";
                o += "{\"id\":" + std::to_string(d[i].id) + ",\"n\":\"" + jsonEsc(d[i].name) + "\",\"min\":" + num(d[i].min) +
                     ",\"max\":" + num(d[i].max) + ",\"def\":" + num(d[i].def) + ",\"v\":" + num(v) + ",\"t\":\"" + jsonEsc(t) +
                     "\",\"st\":" + (isStepped(d[i].kind) ? "true" : "false");
                std::vector<std::string> labels = d[i].labels;
                if (labels.empty() && isStepped(d[i].kind) && d[i].kind != kInt && d[i].max - d[i].min <= 64)
                    for (int k = (int)d[i].min; k <= (int)d[i].max; ++k)
                        labels.push_back(valueText(pl, d[i], k)); // on/off, routing, models, drive types...
                if (!labels.empty())
                {
                    o += ",\"lab\":[";
                    for (size_t k = 0; k < labels.size(); ++k)
                        o += (k ? ",\"" : "\"") + jsonEsc(labels[k]) + "\"";
                    o += "]";
                }
                o += "}";
            }
            o += "],\"models\":[";
            const auto &cat = filterCatalogue();
            for (size_t m = 0; m < cat.size(); ++m)
            {
                o += (m ? ",{\"name\":\"" : "{\"name\":\"") + jsonEsc(cat[m].name) + "\",\"variants\":[";
                for (size_t k = 0; k < cat[m].labels.size(); ++k)
                    o += (k ? ",\"" : "\"") + jsonEsc(cat[m].labels[k]) + "\"";
                o += "]}";
            }
            o += "],\"zplane\":[";
            {
                int nz = 0;
                const ZDesign *zd = zDesigns(nz);
                auto fr = [&](const ZFrame &F) {
                    std::string r = "{\"f\":[";
                    for (int k = 0; k < zSections; ++k)
                        r += (k ? "," : "") + num(F.f[k]);
                    r += "],\"g\":[";
                    for (int k = 0; k < zSections; ++k)
                        r += (k ? "," : "") + num(F.g[k]);
                    r += "],\"q\":[";
                    for (int k = 0; k < zSections; ++k)
                        r += (k ? "," : "") + num(F.q[k]);
                    return r + "],\"lpF\":" + num(F.lpF) + ",\"lpQ\":" + num(F.lpQ) + "}";
                };
                for (int i = 0; i < nz; ++i)
                    o += (i ? "," : "") + std::string("{\"a\":") + fr(zd[i].a) + ",\"b\":" + fr(zd[i].b) + "}";
            }
            {
                Prefs pr = loadPrefs();
                const char *os =
#if defined(_WIN32)
                    "Windows";
#elif defined(__APPLE__)
                    "macOS";
#else
                    "Linux";
#endif
                o += std::string("],\"prefs\":{\"size\":") + std::to_string(pr.sizePct) + ",\"dark\":" + (pr.dark ? "1" : "0") + ",\"tips\":" + (pr.tips ? "1" : "0") + "}";
                o += std::string(",\"info\":{\"version\":\"") + jsonEsc(pluginVersion()) + "\",\"os\":\"" + os + "\",\"host\":\"" +
                     jsonEsc(pl->host && pl->host->name ? pl->host->name : "?") + "\",\"hostVersion\":\"" + jsonEsc(pl->host && pl->host->version ? pl->host->version : "") +
                     "\",\"block\":" + std::to_string(pl->maxBlock) + "}";
            }
            o += ",\"user\":" + userPresetJson() + ",\"presetDir\":" + presetDirJson();
            o += std::string(",\"sr\":") + num((float)pl->sampleRate) + std::string(",\"theme\":\"") + (pl->darkTheme.load() ? "dark" : "light") + "\",\"presets\":[";
            for (size_t i = 0; i < presetList().size(); ++i)
                o += (i ? ",[\"" : "[\"") + jsonEsc(presetList()[i].name) + "\",\"" + jsonEsc(presetList()[i].group + (presetList()[i].group[0] == '~' ? 1 : 0)) + "\"]";
            o += "],\"curves\":[";
            for (int i = 0; i < numLfos; ++i)
            {
                o += i ? ",[" : "[";
                for (size_t k = 0; k < pl->curve[i].size(); ++k)
                {
                    const auto &c = pl->curve[i][k];
                    o += (k ? "," : "") + num(c.x) + "," + num(c.y) + "," + num(c.t);
                }
                o += "]";
            }
            o += "]});";
            return o;
        }
        // values that changed (from automation, Bitwig's own knobs, presets), with their text
        std::string vals;
        for (size_t i = 0; i < d.size(); ++i)
        {
            double v = pl->value[i].load(std::memory_order_relaxed);
            std::string t = valueText(pl, d[i], v);
            if (v != lastV[i] || t != lastT[i])
            {
                lastV[i] = v, lastT[i] = t;
                vals += (vals.empty() ? "\"" : ",\"") + std::to_string(d[i].id) + "\":[" + num(v) + ",\"" + jsonEsc(t) + "\"]";
            }
        }
        // the modulated values of the knobs that can move, for the rings
        int m = std::clamp(pl->step(idRouting), 0, 1);
        std::string mods;
        auto addMod = [&](clap_id id, float v) { mods += (mods.empty() ? "\"" : ",\"") + std::to_string(id) + "\":" + num(v); };
        const int bandOff[6] = {oCutoff, oReso, oMorph, oDrive, oLevel, oPan};
        for (int b = 0; b < 3; ++b)
            for (int q2 = 0; q2 < 6; ++q2)
                addMod(bandBase(m, b) + bandOff[q2], pl->meter[mBand + b * 6 + q2].load(std::memory_order_relaxed));
        if (m == Parallel)
        {
            addMod(modeBase(m) + oXoverLo, pl->meter[mGlobal + 0].load());
            addMod(modeBase(m) + oXoverHi, pl->meter[mGlobal + 1].load());
        }
        if (pl->on(modeBase(m) + oFeedbackOn)) // switched off, they have no movement to show
            addMod(modeBase(m) + oFeedback, pl->meter[mGlobal + 2].load());
        if (pl->on(modeBase(m) + oNoiseOn))
            addMod(modeBase(m) + oNoise, pl->meter[mGlobal + 3].load());
        addMod(modeBase(m) + oMix, pl->meter[mGlobal + 4].load());
        addMod(idSwayAmount, pl->meter[mGlobal + 5].load());
        auto arr = [&](int from, int n)
        {
            std::string a = "[";
            for (int k = 0; k < n; ++k)
                a += (k ? "," : "") + num(pl->meter[from + k].load(std::memory_order_relaxed));
            return a + "]";
        };
        o = "tc.upd({\"v\":{" + vals + "},\"m\":{" + mods + "},\"lane\":" + arr(mLaneStep, 3) + ",\"lockLane\":" + arr(mLockStep, 3) + ",\"laneV\":" + arr(mLaneVal, 3) +
            ",\"lfo\":" + arr(mLfoVal, 3) + ",\"lfoPh\":" + arr(mLfoPhase, 3) + ",\"env\":" + num(pl->meter[mEnv].load()) +
            ",\"sc\":" + num(pl->meter[mSc].load()) + ",\"lim\":" + num(pl->meter[mLimiter].load()) + ",\"guard\":" + arr(mGuard, 3) +
            ",\"edge\":" + arr(mEdge, 3) + ",\"sway\":" + arr(mSway, 3) + ",\"ev\":" + num(pl->meter[mMutEvents].load()) +
            ",\"chap\":" + arr(mChap, 3) + ",\"hist\":" + arr(mHist, 48) + ",\"bank\":" + arr(mBank, 24) + ",\"bankHz\":" + arr(mBankHz, 24) +
            ",\"tunePos\":" + num(pl->meter[mTunePos].load()) + ",\"mutNow\":" + arr(mMutNow, 10) + ",\"mutK\":" + num(pl->meter[mMutK].load()) +
            ",\"mutFrac\":" + num(pl->meter[mMutFrac].load()) +
            (view == 1 ? ",\"spec\":" + spectrum() : view == 2 ? ",\"scope\":" + scope() : std::string()) + "});";
        out = o;
        return o;
    }
};

void Plugin::guiNeedsInit()
{
    if (bridge)
        bridge->needInit = true;
}
void Plugin::requestFlush()
{
    if (hostParams && hostParams->request_flush)
        hostParams->request_flush(host);
}
void Plugin::markDirty()
{
    if (hostState && hostState->mark_dirty)
        hostState->mark_dirty(host);
}
void Plugin::deleteBridge()
{
    delete bridge;
    bridge = nullptr;
}
Plugin::GuiBridge *Plugin::ensureBridge()
{
    if (!bridge)
        bridge = new GuiBridge(this);
    return bridge;
}

// ----- window (CLAP gui extension) ---------------------------------------------------------
static constexpr uint32_t baseW = 1280, baseH = 940;
bool guiIsApiSupported(const clap_plugin_t *, const char *api, bool floating)
{
#if defined(__APPLE__)
    return !floating && api && !std::strcmp(api, CLAP_WINDOW_API_COCOA);
#elif defined(_WIN32)
    return !floating && api && !std::strcmp(api, CLAP_WINDOW_API_WIN32);
#else
    (void)api, (void)floating;
    return false;
#endif
}
bool guiPreferredApi(const clap_plugin_t *, const char **api, bool *floating)
{
#if defined(__APPLE__)
    *api = CLAP_WINDOW_API_COCOA;
    *floating = false;
    return true;
#elif defined(_WIN32)
    *api = CLAP_WINDOW_API_WIN32;
    *floating = false;
    return true;
#else
    (void)api, (void)floating;
    return false;
#endif
}
bool guiCreate(const clap_plugin_t *p, const char *api, bool floating)
{
    if (!guiIsApiSupported(p, api, floating))
        return false;
    auto *pl = self(p);
    if (pl->gui)
        return true;
    auto *b = pl->ensureBridge();
    b->needInit = true;
    pl->gui = tcgui::create(b, (int)pl->guiW, (int)pl->guiH);
    return pl->gui != nullptr;
}
void guiDestroy(const clap_plugin_t *p)
{
    auto *pl = self(p);
    if (pl->gui)
        tcgui::destroy(pl->gui);
    pl->gui = nullptr;
}
#if defined(_WIN32)
// Windows hosts size plugin windows in pixels and tell us the screen's scale; the page itself fits whatever size it gets
bool guiSetScale(const clap_plugin_t *p, double scale)
{
    self(p)->guiScale = std::clamp(scale, 0.5, 4.0);
    return true;
}
#else
bool guiSetScale(const clap_plugin_t *, double) { return false; } // macOS works in points
#endif
bool guiGetSize(const clap_plugin_t *p, uint32_t *w, uint32_t *h)
{
    auto *pl = self(p);
    *w = (uint32_t)std::lround(pl->guiW * pl->guiScale), *h = (uint32_t)std::lround(pl->guiH * pl->guiScale);
    return true;
}
bool guiCanResize(const clap_plugin_t *) { return true; }
bool guiResizeHints(const clap_plugin_t *, clap_gui_resize_hints_t *h)
{
    h->can_resize_horizontally = h->can_resize_vertically = true;
    h->preserve_aspect_ratio = true; // the layout is drawn for one shape; keeping it means nothing is ever cut off
    h->aspect_ratio_width = baseW, h->aspect_ratio_height = baseH;
    return true;
}
bool guiAdjustSize(const clap_plugin_t *p, uint32_t *w, uint32_t *h)
{
    // free dragging, but always the designed shape (1280 x 940), from 50% to 200%: the window
    // scales as a whole, so nothing runs off the edge or gets squeezed at odd sizes
    double s = self(p)->guiScale, z = std::min((double)*w / (baseW * s), (double)*h / (baseH * s));
    z = std::clamp(z, 0.5, 2.0);
    *w = (uint32_t)std::lround(baseW * z * s), *h = (uint32_t)std::lround(baseH * z * s);
    return true;
}
bool guiSetSize(const clap_plugin_t *p, uint32_t w, uint32_t h)
{
    auto *pl = self(p);
    pl->guiW = (uint32_t)std::lround(w / pl->guiScale), pl->guiH = (uint32_t)std::lround(h / pl->guiScale);
    if (pl->gui)
        tcgui::setSize(pl->gui, (int)w, (int)h);
    return true;
}
bool guiSetParent(const clap_plugin_t *p, const clap_window_t *win)
{
    auto *pl = self(p);
#if defined(_WIN32)
    return pl->gui && win && tcgui::setParent(pl->gui, win->win32);
#else
    return pl->gui && win && tcgui::setParent(pl->gui, win->cocoa);
#endif
}
bool guiSetTransient(const clap_plugin_t *, const clap_window_t *) { return false; }
void guiSuggestTitle(const clap_plugin_t *, const char *) {}
bool guiShow(const clap_plugin_t *p)
{
    if (self(p)->gui)
        tcgui::show(self(p)->gui, true);
    return self(p)->gui != nullptr;
}
bool guiHide(const clap_plugin_t *p)
{
    if (self(p)->gui)
        tcgui::show(self(p)->gui, false);
    return self(p)->gui != nullptr;
}
const clap_plugin_gui_t extGui = {guiIsApiSupported, guiPreferredApi, guiCreate, guiDestroy, guiSetScale, guiGetSize,
                                  guiCanResize, guiResizeHints, guiAdjustSize, guiSetSize, guiSetParent, guiSetTransient,
                                  guiSuggestTitle, guiShow, guiHide};

// ----- test hook: drives the window's bridge without a window (used by tests/clap_host_test) -----
struct TestBridge
{
    void (*send)(const clap_plugin_t *, const char *);
    const char *(*poll)(const clap_plugin_t *);
};
void tbSend(const clap_plugin_t *p, const char *m) { self(p)->ensureBridge()->onMessage(m); }
const char *tbPoll(const clap_plugin_t *p)
{
    auto *b = self(p)->ensureBridge();
    b->out = b->poll();
    return b->out.c_str();
}
const TestBridge extTestBridge = {tbSend, tbPoll};

const void *plExtension(const clap_plugin_t *, const char *id)
{
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &extParams;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &extState;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &extPorts;
    if (!std::strcmp(id, CLAP_EXT_LATENCY))
        return &extLatency;
    if (!std::strcmp(id, CLAP_EXT_GUI) && tcgui::available())
        return &extGui;
    if (!std::strcmp(id, "com.otiskanno.threes-company/test-bridge"))
        return &extTestBridge;
    return nullptr;
}
void plMainThread(const clap_plugin_t *) {}

const char *const features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_FILTER,
                                CLAP_PLUGIN_FEATURE_DISTORTION, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT,
    "com.otiskanno.threes-company",
    "Three's Company",
    "Otis Kanno",
    "",
    "",
    "",
    "0.20.0",
    "Three filters and three drives, in three bands or in series, swaying together; the middle one conducts the other two. Each routing keeps its own settings.",
    features,
};

// ----- factory and entry --------------------------------------------------------------
uint32_t factoryCount(const clap_plugin_factory_t *) { return 1; }
const clap_plugin_descriptor_t *factoryDescriptor(const clap_plugin_factory_t *, uint32_t i)
{
    return i == 0 ? &descriptor : nullptr;
}
const clap_plugin_t *factoryCreate(const clap_plugin_factory_t *, const clap_host_t *host, const char *id)
{
    if (!host || !clap_version_is_compatible(host->clap_version) || !id || std::strcmp(id, descriptor.id))
        return nullptr;
    auto *pl = new Plugin();
    pl->host = host;
    pl->clap.desc = &descriptor;
    pl->clap.plugin_data = pl;
    pl->clap.init = plInit;
    pl->clap.destroy = plDestroy;
    pl->clap.activate = plActivate;
    pl->clap.deactivate = plDeactivate;
    pl->clap.start_processing = plStart;
    pl->clap.stop_processing = plStop;
    pl->clap.reset = plReset;
    pl->clap.process = plProcess;
    pl->clap.get_extension = plExtension;
    pl->clap.on_main_thread = plMainThread;
    return &pl->clap;
}
const clap_plugin_factory_t factory = {factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char *)
{
    (void)filterCatalogue();
    (void)paramDefs();
    (void)paramIndex();
    return true;
}
void entryDeinit() {}
const void *entryFactory(const char *id) { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &factory; }
} // namespace

// The CLAP entry. The CMake build (clap-wrapper: CLAP, VST3, AU) defines it in tc_entry.cpp instead, from these.
bool tcEntryInit(const char *path) { return entryInit(path); }
void tcEntryDeinit() { entryDeinit(); }
const void *tcEntryFactory(const char *id) { return entryFactory(id); }
#ifndef TC_SEPARATE_ENTRY
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};
#endif
