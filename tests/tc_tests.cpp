/*
 * Three's Company - engine tests and demo render (phase 1).
 *   tc_tests            run every test, print PASS / FAIL per test
 *   tc_tests demo x.wav render a short synthetic loop through a few settings
 *   tc_tests list       print the filter catalogue and how smart limiting sees each type
 */
#include "../src/tc_engine.h"

#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <functional>
#include <set>

using namespace otiskanno::threescompany;

static const double SR = 48000.0;
static int failures = 0;

static void check(bool ok, const char *name, const std::string &detail)
{
    std::printf("%-5s %-34s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    if (!ok)
        ++failures;
}
static std::string fmt(const char *f, ...)
{
    char b[512];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof(b), f, a);
    va_end(a);
    return b;
}
static double dB(double x) { return 20.0 * std::log10(std::max(x, 1.0e-12)); }

struct Rng
{
    uint32_t s{12345};
    uint32_t u()
    {
        s = s * 1664525u + 1013904223u;
        return s;
    }
    float pm1() { return (float)((u() >> 8) / 8388608.0 - 1.0); }
    float u01() { return (float)((u() >> 8) / 16777216.0); }
    int below(int n) { return (int)(u01() * n) % std::max(1, n); }
};

static int findModel(const char *nameSub)
{
    const auto &cat = filterCatalogue();
    for (size_t i = 0; i < cat.size(); ++i)
        if (cat[i].name.find(nameSub) != std::string::npos)
            return (int)i;
    return 0;
}
static int findVariant(int model, PassKind kind, const char *labelSub = nullptr)
{
    const auto &mi = filterCatalogue()[(size_t)model];
    for (size_t i = 0; i < mi.configs.size(); ++i)
        if (mi.kinds[i] == kind && (!labelSub || mi.labels[i].find(labelSub) != std::string::npos))
            return (int)i;
    return 0;
}

// Run a block of audio through an engine in host-sized chunks.
static void run(Engine &e, const Settings &s, std::vector<float> &l, std::vector<float> &r)
{
    const int chunk = 64;
    for (size_t p = 0; p < l.size(); p += chunk)
    {
        int n = (int)std::min<size_t>(chunk, l.size() - p);
        e.process(&l[p], &r[p], &l[p], &r[p], n, s);
    }
}

// Magnitude response from an impulse, at the given frequencies.
static std::vector<double> response(const Settings &s, const std::vector<double> &freqs)
{
    Engine e;
    e.setSampleRate(SR);
    const int N = 32768;
    std::vector<float> l(N, 0.f), r(N, 0.f);
    const float amp = 0.25f;
    l[64] = r[64] = amp;
    run(e, s, l, r);
    std::vector<double> out;
    for (double f : freqs)
    {
        double re = 0, im = 0, w = 2.0 * 3.14159265358979 * f / SR;
        for (int n = 0; n < N; ++n)
        {
            re += l[n] * std::cos(w * n);
            im -= l[n] * std::sin(w * n);
        }
        out.push_back(std::sqrt(re * re + im * im) / amp);
    }
    return out;
}
static std::vector<double> logFreqs(double lo, double hi, int n)
{
    std::vector<double> f;
    for (int i = 0; i < n; ++i)
        f.push_back(lo * std::pow(hi / lo, (double)i / (n - 1)));
    return f;
}
static double maxDevDb(const std::vector<double> &mag)
{
    double m = 0;
    for (double v : mag)
        m = std::max(m, std::fabs(dB(v)));
    return m;
}
static double rms(const std::vector<float> &x, size_t from = 0)
{
    double a = 0;
    for (size_t i = from; i < x.size(); ++i)
        a += (double)x[i] * x[i];
    return std::sqrt(a / std::max<size_t>(1, x.size() - from));
}
static double peak(const std::vector<float> &x)
{
    double p = 0;
    for (float v : x)
        p = std::max(p, (double)std::fabs(v));
    return p;
}
static bool finite(const std::vector<float> &x)
{
    for (float v : x)
        if (!std::isfinite(v))
            return false;
    return true;
}
static void noise(std::vector<float> &l, std::vector<float> &r, float amp, uint32_t seed = 7)
{
    Rng g;
    g.s = seed;
    for (size_t i = 0; i < l.size(); ++i)
    {
        l[i] = amp * g.pm1();
        r[i] = amp * g.pm1();
    }
}

// ---------------------------------------------------------------------------
static void testFlat()
{
    for (int os = 0; os <= 2; ++os)
    {
        Settings s;
        s.oversample = os;
        s.routing = Parallel;
        auto mag = response(s, logFreqs(20, os == 0 ? 20000 : 16000, 120));
        double d = maxDevDb(mag);
        check(d < (os == 0 ? 0.05 : 0.3), fmt("flat sum, parallel, os %dx", 1 << os).c_str(),
              fmt("max deviation %.3f dB", d));
        s.routing = Series;
        d = maxDevDb(response(s, logFreqs(20, os == 0 ? 20000 : 16000, 120)));
        check(d < (os == 0 ? 0.05 : 0.3), fmt("flat, series, os %dx", 1 << os).c_str(),
              fmt("max deviation %.3f dB", d));
    }
    {
        Settings s;
        s.oversample = 0;
        s.mode[Parallel].mix = 0.5f;
        double d = maxDevDb(response(s, logFreqs(20, 20000, 120)));
        check(d < 0.05, "flat at 50% mix, parallel", fmt("max deviation %.3f dB (no combing)", d));
    }
    {
        Settings s;
        s.oversample = 0;
        s.mode[Parallel].xoverLoHz = 120;
        s.mode[Parallel].xoverHiHz = 9000;
        double d = maxDevDb(response(s, logFreqs(20, 20000, 120)));
        check(d < 0.05, "flat with moved split points", fmt("max deviation %.3f dB", d));
    }
}

static void testCrossoverPoints()
{
    Settings s;
    s.oversample = 0;
    auto &m = s.mode[Parallel];
    std::vector<double> f = {60, 250, 900, 3000, 12000};
    const char *nm[3] = {"low", "mid", "high"};
    double want[3][5] = {{0, -6.02, -99, -99, -99}, {-99, -6.02, 0, -6.02, -99}, {-99, -99, -99, -6.02, 0}};
    for (int b = 0; b < 3; ++b)
    {
        for (int i = 0; i < 3; ++i)
            m.band[i].solo = i == b;
        auto mag = response(s, f);
        bool ok = true;
        std::string d;
        for (int i = 0; i < 5; ++i)
        {
            double v = dB(mag[i]);
            d += fmt("%.0fHz %.1f  ", f[i], v);
            if (want[b][i] > -50)
                ok = ok && std::fabs(v - want[b][i]) < 0.6;
            else
                ok = ok && v < -20;
        }
        check(ok, fmt("band solo: %s", nm[b]).c_str(), d);
    }
}

static void testCeilingAllModels()
{
    const auto &cat = filterCatalogue();
    double worst = 0;
    bool allFinite = true;
    int count = 0;
    std::string worstName;
    for (int routing = 0; routing < 2; ++routing)
        for (size_t mi = 1; mi < cat.size(); ++mi)
            for (int dt = 0; dt < numDriveTypes; ++dt)
            {
                Settings s;
                s.routing = routing;
                s.oversample = 1;
                auto &m = s.mode[routing];
                m.inputDb = 12;
                m.outputDb = 12;
                for (int b = 0; b < 3; ++b)
                {
                    auto &B = m.band[b];
                    B.model = (int)mi;
                    B.variant = (int)((dt + b) % std::max<size_t>(1, cat[mi].configs.size()));
                    B.resonance = 1.f;
                    B.morph = 0.5f;
                    B.cutoff = -20.f + 30.f * b;
                    B.driveType = dt;
                    B.drive = 1.f;
                    B.driveFirst = (b + dt) & 1;
                    B.levelDb = 12;
                }
                Engine e;
                e.setSampleRate(SR);
                std::vector<float> l(12000), r(12000);
                noise(l, r, 1.0f, 99 + (uint32_t)mi);
                run(e, s, l, r);
                allFinite = allFinite && finite(l) && finite(r);
                double p = std::max(peak(l), peak(r));
                if (p > worst)
                {
                    worst = p;
                    worstName = cat[mi].name + " / " + driveName(dt);
                }
                ++count;
            }
    check(allFinite && worst <= Engine::limCeiling + 1.0e-6, "ceiling, every model x drive",
          fmt("%d combinations at full resonance, full drive, +12 dB everywhere: peak %.4f (%.2f dBFS), %s",
              count, worst, dB(worst), allFinite ? "all finite" : "NON-FINITE OUTPUT"));
}

static void randomise(Settings &s, Rng &g)
{
    const auto &cat = filterCatalogue();
    s.routing = g.below(2);
    s.oversample = g.below(3);
    s.modDepth = 2 * g.u01();
    s.smartEdges = g.below(4) != 0;
    s.autoGain = g.below(2);
    for (int r = 0; r < 2; ++r)
    {
        auto &m = s.mode[r];
        m.xoverLoHz = 40.f * std::pow(50.f, g.u01());
        m.xoverHiHz = 400.f * std::pow(40.f, g.u01());
        m.inputDb = -12 + 36 * g.u01();
        m.outputDb = -12 + 36 * g.u01();
        m.mix = g.u01();
        for (int b = 0; b < 3; ++b)
        {
            auto &B = m.band[b];
            B.model = g.below((int)cat.size());
            B.variant = g.below(64);
            B.cutoff = -80 + 170 * g.u01();
            B.resonance = g.below(3) ? g.u01() : 1.f;
            B.morph = g.u01();
            B.filterOn = g.below(5) != 0;
            B.driveType = g.below(numDriveTypes);
            B.drive = g.below(3) ? g.u01() : 1.f;
            B.driveFirst = g.below(2);
            B.levelDb = -70 + 82 * g.u01();
            B.mute = g.below(8) == 0;
            B.solo = g.below(10) == 0;
        }
    }
    for (int r = 0; r < 2; ++r)
    {
        auto &m = s.mode[r];
        m.feedbackOn = g.below(2);
        m.feedback = g.below(3) ? g.u01() : 1.f;
        m.feedbackToneHz = 200.f * std::pow(80.f, g.u01());
        m.noiseOn = g.below(3) == 0;
        m.noise = g.u01();
        m.noiseColor = g.u01();
        for (int b = 0; b < 3; ++b)
        {
            auto &B = m.band[b];
            B.pan = -1.f + 2.f * g.u01();
            B.bankMix = g.below(2) ? g.u01() : 0.f;
            B.bankWidth = g.u01();
            for (auto &x : B.bankGain)
                x = g.u01();
            B.rippleDepth = g.u01(), B.rippleRateHz = 0.02f + 20 * g.u01(), B.rippleWaves = 0.25f + 3.75f * g.u01(), B.bankStereo = g.u01();
            B.stripMix = g.u01(), B.modDepth = 2 * g.u01();
        }
    }
    s.sway.amount = g.u01();
    s.sway.rateHz = 0.01f * std::pow(1000.f, g.u01());
    s.sway.sync = g.below(2);
    s.sway.syncIndex = g.below(7);
    s.sway.stepped = g.below(2);
    s.sway.slew = g.u01();
    s.sway.follow = g.u01();
    s.sway.depthSemis = 48.f * g.u01();
    s.conductor.influence = g.u01();
    s.conductor.lean = g.u01();
    s.mutation.trigger = g.below(numMutTriggers);
    s.mutation.sensitivity = g.u01();
    s.mutation.holdSteps = g.below(5);
    for (auto &R : s.mutation.resp)
        R = {g.below(3), g.below(numMutTargets), g.u01(), g.u01(), g.below(5)};
    s.mutation.mutate = g.u01();
    s.mutation.everyIndex = g.below(5);
    s.mutation.freeze = g.below(4) == 0;
    s.mutation.seed = g.below(1000);
    for (auto &L : s.lane)
    {
        for (int i = 0; i < numSteps; ++i)
        {
            L.step[i] = -1 + 2 * g.u01();
            for (int k = 0; k < numBandParams; ++k)
                L.plock[k][i] = g.below(4) ? std::numeric_limits<float>::quiet_NaN() : (k == PCut ? -60 + 142 * g.u01() : k == PLevel ? -60 + 72 * g.u01() : k == PPan ? -1 + 2 * g.u01() : g.u01());
        }
        L.length = 1 + g.below(16), L.rateIndex = g.below(9), L.euclid = g.below(3), L.hits = g.below(17), L.rotate = g.below(16);
        L.smooth = g.u01(), L.depth = g.u01(), L.target = g.below(numModTargetIds), L.freeRun = g.below(2), L.speedHz = 0.1f + 30 * g.u01();
        L.swing = 0.5f + 0.25f * g.u01(), L.offset = -0.5f + g.u01(), L.lockGlideMs = 200 * g.u01();
        L.lockLength = g.below(17), L.lockRateIndex = g.below(10), L.sway = g.u01();
    }
    {
        auto &C = s.conductor;
        C.grid = g.below(17), C.divLow = 1 + g.below(16), C.divHigh = 1 + g.below(16), C.chaos = g.u01(), C.range = 48 * g.u01(), C.chance = g.u01();
        C.condition = g.below(12), C.bursts = g.u01(), C.mischief = g.u01(), C.musical = g.below(4), C.seed = g.below(1000);
        C.jack = g.u01(), C.jackPair = g.below(6), C.jackParam = g.below(6);
        auto &W = s.sway;
        W.toCut = g.u01(), W.toRes = g.u01(), W.toMorph = g.u01(), W.toDrive = g.u01(), W.toLevel = g.u01(), W.toPan = g.u01();
        for (auto &m : s.mode)
            for (auto &B : m.band)
                B.rippleSync = g.below(2), B.rippleSyncIndex = g.below(14), B.rippleShape = g.below(6), B.rippleRateHz = 200 * g.u01();
    }
    s.retrigger = g.below(5);
    for (auto &F : s.lfo)
    {
        F = {g.below(numShapes + 1), g.below(3), g.below(8), 0.01f + 30 * g.u01(), g.u01(), g.u01(), (bool)g.below(2)};
        F.target = g.below(numModTargetIds), F.amount = -1 + 2 * g.u01(), F.stepCount = 1 + g.below(16), F.glide = g.u01(), F.quantize = g.below(6);
        for (auto &x : F.step)
            x = -1 + 2 * g.u01();
    }
    s.env = {g.below(5), -24 + 60 * g.u01(), 0.1f + 100 * g.u01(), 1 + 2000 * g.u01(), -24 + 48 * g.u01(), 10 + 2000 * g.u01(), 40 + 19000 * g.u01()};
    for (auto &sl : s.slot)
        sl = {g.below(numModSources), g.below(numModTargetIds), -1 + 2 * g.u01()};
    for (int b = 0; b < 3; ++b)
    {
        s.mod[b].pan = -1 + 2 * g.u01();
        s.mod[b].cutoff = -60 + 120 * g.u01();
        s.mod[b].resonance = -1 + 2 * g.u01();
        s.mod[b].drive = -1 + 2 * g.u01();
        s.mod[b].levelDb = -24 + 48 * g.u01();
    }
}

static void testTorture()
{
    Engine e;
    e.setSampleRate(SR);
    Rng g;
    g.s = 4242;
    Settings s;
    double worst = 0;
    bool ok = true;
    const int seconds = 60, chunk = 32;
    std::vector<float> l(chunk), r(chunk);
    long total = (long)(seconds * SR / chunk);
    int changes = 0;
    for (long c = 0; c < total; ++c)
    {
        if (c % 40 == 0) // new random settings every ~27 ms
        {
            randomise(s, g);
            ++changes;
        }
        float amp = (c / 2000) % 3 == 2 ? 0.f : 1.5f; // silence sometimes: ringing with no input
        for (int i = 0; i < chunk; ++i)
        {
            l[i] = amp * g.pm1();
            r[i] = amp * g.pm1();
        }
        if (c % 9000 == 17)
            l[3] = std::nanf(""); // a host handing us garbage
        if (c % 500 == 0)
        {
            HostTime ht;
            ht.playing = g.below(3) != 0;
            ht.beatsValid = true;
            ht.bpm = 40 + 200 * g.u01();
            ht.beats = g.below(4) == 0 ? -4.0 : 2000.0 * g.u01();
            ht.num = 3 + g.below(5);
            ht.den = g.below(2) ? 4 : 8;
            e.setHostTime(ht);
        }
        e.process(l.data(), r.data(), l.data(), r.data(), chunk, s);
        ok = ok && finite(l) && finite(r);
        worst = std::max({worst, peak(l), peak(r)});
    }
    check(ok && worst <= Engine::limCeiling + 1.0e-6, "torture",
          fmt("%d s, %d random setting changes incl. routing and oversampling: peak %.4f, %s", seconds,
              changes, worst, ok ? "all finite" : "NON-FINITE OUTPUT"));
}

static void testSmartParallel()
{
    int svf = findModel("Fast SVF");
    int lp = findVariant(svf, PassKind::LowPass), hp = findVariant(svf, PassKind::HighPass);
    struct Case
    {
        const char *name;
        int band;
        int variant;
        float cutoff;
    } cases[] = {{"smart edge: LP closed, high band", High, lp, -60.f},
                 {"smart edge: LP closed, mid band", Mid, lp, -60.f},
                 {"smart edge: HP wide open, low band", Low, hp, 70.f},
                 {"smart edge: HP wide open, mid band", Mid, hp, 70.f}};
    for (auto &c : cases)
    {
        double lv[3];
        float pr = 0;
        for (int pass = 0; pass < 3; ++pass) // 0: no filter, 1: smart off, 2: smart on
        {
            Settings s;
            s.oversample = 0;
            s.smartEdges = pass == 2;
            auto &m = s.mode[Parallel];
            for (int i = 0; i < 3; ++i)
                m.band[i].solo = i == c.band;
            if (pass > 0)
            {
                m.band[c.band].model = svf;
                m.band[c.band].variant = c.variant;
                m.band[c.band].cutoff = c.cutoff;
            }
            Engine e;
            e.setSampleRate(SR);
            std::vector<float> l(48000), r(48000);
            noise(l, r, 0.3f);
            run(e, s, l, r);
            lv[pass] = dB(rms(l, 12000));
            if (pass == 2)
                pr = e.edgePress(c.band);
        }
        // white noise is the harshest case: most of its energy sits at the top of each band
        bool ok = lv[1] < lv[0] - 40 && lv[2] > lv[0] - 20 && pr > 0.5f;
        check(ok, c.name, fmt("band level: open %.1f dB, smart off %.1f dB (muted), smart on %.1f dB; light %.2f",
                              lv[0], lv[1], lv[2], pr));
    }
    {
        // and it must leave a normally placed filter completely alone
        Settings a, b;
        a.oversample = b.oversample = 0;
        a.smartEdges = false;
        b.smartEdges = true;
        for (auto *s : {&a, &b})
        {
            auto &B = s->mode[Parallel].band[Mid];
            B.model = svf;
            B.variant = lp;
            B.cutoff = hzToSemi(2500);
        }
        auto f = logFreqs(30, 18000, 40);
        auto ma = response(a, f), mb = response(b, f);
        double d = 0;
        for (size_t i = 0; i < f.size(); ++i)
            d = std::max(d, std::fabs(dB(ma[i]) - dB(mb[i])));
        check(d < 0.01, "smart edge: hands off in range", fmt("difference %.4f dB", d));
    }
}

static void testSmartSeries()
{
    int svf = findModel("Fast SVF");
    int lp = findVariant(svf, PassKind::LowPass), hp = findVariant(svf, PassKind::HighPass);
    double lv[2];
    float pr[2] = {0, 0};
    for (int pass = 0; pass < 2; ++pass)
    {
        Settings s;
        s.routing = Series;
        s.oversample = 0;
        s.smartEdges = pass == 1;
        auto &m = s.mode[Series];
        m.band[Low].model = svf;
        m.band[Low].variant = lp;
        m.band[Low].cutoff = hzToSemi(200);
        m.band[High].model = svf;
        m.band[High].variant = hp;
        m.band[High].cutoff = hzToSemi(5000);
        Engine e;
        e.setSampleRate(SR);
        std::vector<float> l(48000), r(48000);
        noise(l, r, 0.3f);
        run(e, s, l, r);
        lv[pass] = dB(rms(l, 12000)) - dB(0.3 / std::sqrt(3.0));
        if (pass == 1)
            pr[0] = e.edgePress(Low), pr[1] = e.edgePress(High);
    }
    check(lv[0] < -40 && lv[1] > -24 && pr[0] > 0.3f && pr[1] > 0.3f, "smart: crossed LP/HP in series",
          fmt("vs input: smart off %.1f dB (silence), smart on %.1f dB; lights %.2f / %.2f", lv[0], lv[1],
              pr[0], pr[1]));
}

static void testRoutingSwitch()
{
    int svf = findModel("Fast SVF");
    int lp = findVariant(svf, PassKind::LowPass);
    Settings s;
    s.oversample = 1;
    // different settings in each routing: Parallel is open, Series has a low-pass at 300 Hz
    s.mode[Series].band[Mid].model = svf;
    s.mode[Series].band[Mid].variant = lp;
    s.mode[Series].band[Mid].cutoff = hzToSemi(300);
    Engine e;
    e.setSampleRate(SR);
    const int N = 48000 * 2;
    std::vector<float> l(N), r(N);
    for (int i = 0; i < N; ++i)
        l[i] = r[i] = 0.4f * std::sin(2 * 3.14159265 * 100.0 * i / SR) +
                      0.2f * std::sin(2 * 3.14159265 * 4000.0 * i / SR);
    double maxStep = 0, natural = 0;
    std::vector<float> in = l;
    for (int p = 0; p < N; p += 32)
    {
        s.routing = (p / 12000) % 2; // flip every quarter second
        e.process(&l[p], &r[p], &l[p], &r[p], 32, s);
    }
    for (int i = 1; i < N; ++i)
    {
        maxStep = std::max(maxStep, (double)std::fabs(l[i] - l[i - 1]));
        natural = std::max(natural, (double)std::fabs(in[i] - in[i - 1]));
    }
    // 4 kHz level late in a Parallel stretch vs late in a Series stretch
    auto tone = [&](int from, double f)
    {
        double re = 0, im = 0;
        for (int i = from; i < from + 4800; ++i)
        {
            re += l[i] * std::cos(2 * 3.14159265 * f * i / SR);
            im += l[i] * std::sin(2 * 3.14159265 * f * i / SR);
        }
        return dB(std::sqrt(re * re + im * im) / 2400.0);
    };
    double hPar = tone(24000 + 6000, 4000), hSer = tone(36000 + 6000, 4000);
    check(maxStep < natural * 1.5 && hPar > -15 && hSer < hPar - 30, "routing switch",
          fmt("largest sample step %.4f (the input's own is %.4f): no click. 4 kHz: Parallel %.1f dB, Series %.1f dB: each kept its own settings",
              maxStep, natural, hPar, hSer));
}

static void testGuard()
{
    int lad = findModel("Vintage Ladder");
    double lv[2];
    for (int pass = 0; pass < 2; ++pass)
    {
        Settings s;
        s.oversample = 1;
        s.limiter = pass == 1;
        auto &B = s.mode[Parallel].band[Mid];
        B.model = lad;
        B.resonance = 1.f;
        B.cutoff = hzToSemi(1000);
        Engine e;
        e.setSampleRate(SR);
        std::vector<float> l(96000), r(96000);
        noise(l, r, 0.05f);
        run(e, s, l, r);
        lv[pass] = dB(peak(l));
    }
    check(lv[1] < -1.0 + 0.01 && lv[1] <= lv[0], "resonance guard, quiet input",
          fmt("self-oscillating ladder in the mid band: %.1f dBFS raw, %.1f dBFS guarded", lv[0], lv[1]));
}

static void testDrive()
{
    std::string d;
    bool ok = true;
    for (int t = 0; t < numDriveTypes; ++t)
    {
        Settings s;
        s.routing = Series;
        s.oversample = 2;
        auto &B = s.mode[Series].band[Low];
        B.driveType = t;
        B.drive = 0.7f;
        Engine e;
        e.setSampleRate(SR);
        const int N = 48000;
        std::vector<float> l(N), r(N);
        for (int i = 0; i < N; ++i)
            l[i] = r[i] = 0.25f * std::sin(2 * 3.14159265 * 220.0 * i / SR);
        double inR = 0.25 / std::sqrt(2.0);
        run(e, s, l, r);
        // fundamental vs everything else, over the last half second
        double re = 0, im = 0, tot = 0;
        for (int i = 24000; i < N; ++i)
        {
            re += l[i] * std::cos(2 * 3.14159265 * 220.0 * i / SR);
            im += l[i] * std::sin(2 * 3.14159265 * 220.0 * i / SR);
            tot += (double)l[i] * l[i];
        }
        double fund = (re * re + im * im) / (24000.0 * 24000.0) * 2.0; // mean square of the fundamental
        double ms = tot / 24000.0;
        double thd = 100.0 * std::sqrt(std::max(0.0, ms - fund) / std::max(ms, 1e-12));
        double lvl = dB(std::sqrt(ms) / inR);
        d += fmt("%s %.0f%% %+.1fdB  ", driveName(t), thd, lvl);
        ok = ok && finite(l) && thd > 3.0 && lvl > -12 && lvl < 14;
    }
    check(ok, "drives: harmonics and level", d);
    {
        Settings s;
        s.routing = Series;
        s.oversample = 0;
        for (int b = 0; b < 3; ++b)
            s.mode[Series].band[b].drive = 0.f;
        double dv = maxDevDb(response(s, logFreqs(20, 20000, 60)));
        check(dv < 0.01, "drive at zero is a wire", fmt("max deviation %.4f dB", dv));
    }
}

static void fft(std::vector<double> &re, std::vector<double> &im)
{
    size_t n = re.size();
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
        double ang = -2 * 3.14159265358979323846 / (double)len;
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k)
            {
                double wr = std::cos(ang * k), wi = std::sin(ang * k);
                double xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                double xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k + len / 2] = re[i + k] - xr, im[i + k + len / 2] = im[i + k] - xi;
                re[i + k] += xr, im[i + k] += xi;
            }
    }
}

// A 5003 Hz sine through a drive. Its real harmonics sit at multiples of 5003 Hz; anything
// else in the spectrum is aliasing. Returns the loudest such component against the fundamental.
static double aliasDb(int type, float amount, int os)
{
    const double f0 = 5003.0;
    Settings s;
    s.routing = Series;
    s.oversample = os;
    s.limiter = false;
    s.mode[Series].band[Low].driveType = type;
    s.mode[Series].band[Low].drive = amount;
    Engine e;
    e.setSampleRate(SR);
    const int N = 32768, pre = 16000;
    std::vector<float> l(N + pre), r(N + pre);
    for (int i = 0; i < N + pre; ++i)
        l[i] = r[i] = 0.5f * std::sin(2 * 3.14159265358979 * f0 * i / SR);
    run(e, s, l, r);
    std::vector<double> re(N), im(N, 0.0);
    for (int i = 0; i < N; ++i)
    {
        double t = 2 * 3.14159265358979 * i / (N - 1); // Blackman-Harris window
        double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        re[i] = w * l[(size_t)(i + pre)];
    }
    fft(re, im);
    double binHz = SR / N, fund = 0, worst = 0;
    for (int b = (int)(40 / binHz); b < (int)(20000 / binHz); ++b)
    {
        double f = b * binHz, mag = std::sqrt(re[b] * re[b] + im[b] * im[b]);
        double nearest = std::round(f / f0) * f0;
        bool harmonic = nearest > 0 && std::fabs(f - nearest) < 12 * binHz;
        if (std::fabs(f - f0) < 12 * binHz)
            fund = std::max(fund, mag);
        else if (!harmonic)
            worst = std::max(worst, mag);
    }
    return dB(worst / fund);
}

static void testAliasing()
{
    // Fuzz near full is the worst case there is: close to a square wave, whose harmonics
    // fall off slowly, so each doubling of the rate buys about 10 dB.
    double fz[3], wm[3];
    for (int os = 0; os <= 2; ++os)
    {
        fz[os] = aliasDb(Fuzz, 0.8f, os);
        wm[os] = aliasDb(Warm, 0.5f, os);
    }
    check(fz[1] < fz[0] - 8 && fz[2] < fz[1] - 8 && wm[1] < -80 && wm[2] <= wm[1],
          "oversampling cuts aliasing",
          fmt("loudest alias under a driven 5 kHz sine, off / 2x / 4x: Fuzz at 80%%: %.0f / %.0f / %.0f dB. Warm at 50%%: %.0f / %.0f / %.0f dB",
              fz[0], fz[1], fz[2], wm[0], wm[1], wm[2]));
}

// ===========================================================================
// Phase 2
// ===========================================================================
// Runs an engine block by block with the transport playing from `startBeats`, calling
// `each` after every host block of 64 samples.
static void runPlaying(Engine &e, const Settings &s, double seconds, double startBeats, double bpm,
                       const std::function<void(int, const std::vector<float> &, const std::vector<float> &)> &each,
                       const std::function<float(long)> &input = nullptr)
{
    const int B = 64;
    std::vector<float> l(B), r(B);
    long n = (long)(seconds * SR / B);
    Rng g;
    for (long k = 0; k < n; ++k)
    {
        HostTime ht;
        ht.playing = true;
        ht.beatsValid = true;
        ht.bpm = bpm;
        ht.beats = startBeats + k * B / SR * bpm / 60.0;
        e.setHostTime(ht);
        for (int i = 0; i < B; ++i)
        {
            float v = input ? input(k * B + i) : 0.2f * g.pm1();
            l[i] = r[i] = v;
        }
        e.process(l.data(), r.data(), l.data(), r.data(), B, s);
        each((int)k, l, r);
    }
}

static void testPan()
{
    Settings s;
    s.oversample = 0;
    s.mode[Parallel].band[Low].pan = s.mode[Parallel].band[Mid].pan = s.mode[Parallel].band[High].pan = -1.f;
    Engine e;
    e.setSampleRate(SR);
    std::vector<float> l(48000), r(48000);
    noise(l, r, 0.2f);
    std::vector<float> inL = l, inR = r;
    run(e, s, l, r);
    double gl = dB(rms(l, 12000) / rms(inL, 12000)), gr = dB(rms(r, 12000) / rms(inR, 12000));
    check(gl > 2.5 && gl < 3.5 && gr < -60, "pan: all strips hard left",
          fmt("left %+.1f dB, right %.0f dB (equal power: +3 dB on the side it goes to)", gl, gr));
}

static void testFeedbackNoise()
{
    // Feedback switched on at 0 is exactly the same as off
    {
        Settings a, b;
        a.oversample = b.oversample = 1;
        b.mode[Parallel].feedbackOn = true;
        b.mode[Parallel].feedback = 0.f;
        Engine ea, eb;
        ea.setSampleRate(SR);
        eb.setSampleRate(SR);
        std::vector<float> la(24000), ra(24000);
        noise(la, ra, 0.3f);
        std::vector<float> lb = la, rb = ra;
        run(ea, a, la, ra);
        run(eb, b, lb, rb);
        double d = 0;
        for (size_t i = 0; i < la.size(); ++i)
            d = std::max(d, (double)std::fabs(la[i] - lb[i]));
        check(d < 1e-6, "feedback on at 0 = off", fmt("largest difference %.1e", d));
    }
    // Feedback lengthens a resonant filter's ring and stays under control
    {
        int svf = findModel("Fast SVF"), bp = findVariant(svf, PassKind::BandPass);
        double tail[2];
        double pk = 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            Settings s;
            s.routing = Series;
            s.oversample = 1;
            s.limiter = false;
            auto &M = s.mode[Series];
            M.band[Mid].model = svf, M.band[Mid].variant = bp, M.band[Mid].cutoff = hzToSemi(800), M.band[Mid].resonance = 0.8f;
            M.feedbackOn = pass == 1;
            M.feedback = 1.f;
            Engine e;
            e.setSampleRate(SR);
            std::vector<float> l(48000, 0.f), r(48000, 0.f);
            for (int i = 100; i < 140; ++i)
                l[i] = r[i] = 0.8f;
            run(e, s, l, r);
            double t = 0;
            for (int i = 1100; i < 2900; ++i) // 20 to 60 ms after the hit
                t += (double)l[i] * l[i];
            tail[pass] = dB(std::sqrt(t / 1800));
            if (pass == 1)
                pk = std::max(peak(l), peak(r));
            if (pass == 1)
            {
                double end = rms(l, 43000);
                check(end < 1e-4 && finite(l), "feedback dies away", fmt("level after 0.9 s: %.0f dB", dB(end)));
            }
        }
        check(tail[1] > tail[0] + 3, "feedback lengthens the ring",
              fmt("level 20-60 ms after a hit: off %.0f dB, full feedback %.0f dB (peak %.2f)", tail[0], tail[1], pk));
    }
    // Every filter type at high resonance: once the input stops, feedback sends nothing back,
    // so it can lengthen a ring but never keep a sound going (a filter at full resonance may
    // still ring on its own: that is the filter, not the feedback)
    {
        const auto &cat = filterCatalogue();
        double worst = 0;
        std::string wn;
        int n = 0;
        for (size_t mi = 1; mi < cat.size(); ++mi)
            for (size_t v = 0; v < cat[mi].configs.size(); v += std::max<size_t>(1, cat[mi].configs.size() / 4))
                for (float re : {0.6f, 0.9f, 1.0f})
                {
                    Settings s;
                    s.routing = Series;
                    s.oversample = 1;
                    s.limiter = false;
                    auto &M = s.mode[Series];
                    M.band[Mid].model = (int)mi, M.band[Mid].variant = (int)v, M.band[Mid].cutoff = hzToSemi(800), M.band[Mid].resonance = re;
                    M.feedbackOn = true;
                    M.feedback = 1.f;
                    Engine e;
                    e.setSampleRate(SR);
                    std::vector<float> l(64, 0.f), r(64, 0.f);
                    double ret = 0;
                    for (int b = 0; b < 1000; ++b)
                    {
                        for (int i = 0; i < 64; ++i)
                            l[i] = r[i] = (b == 1 && i < 40) ? 0.8f : 0.f;
                        e.process(l.data(), r.data(), l.data(), r.data(), 64, s);
                        if (b >= 750) // the last 0.33 s
                            ret = std::max(ret, (double)e.feedbackReturn());
                    }
                    ++n;
                    if (ret > worst)
                        worst = ret, wn = cat[mi].name + " / " + cat[mi].labels[v];
                }
        check(worst < 3.2e-5, "feedback never holds a note", // -90 dB: inaudible
              fmt("%d filter settings at resonance 60-100%%, 1 to 1.3 s after one hit: feedback sends back at most %.0f dB%s", n, dB(worst), worst > 0 ? (" (" + wn + ")").c_str() : ""));
    }
    // Noise: level and colour, with silence coming in
    {
        double lv[2], hf[2];
        for (int c = 0; c < 2; ++c)
        {
            Settings s;
            s.oversample = 0;
            s.mode[Parallel].noiseOn = true;
            s.mode[Parallel].noise = 0.5f;
            s.mode[Parallel].noiseColor = c ? 1.f : 0.f;
            Engine e;
            e.setSampleRate(SR);
            std::vector<float> l(48000, 0.f), r(48000, 0.f);
            run(e, s, l, r);
            lv[c] = dB(rms(l, 12000));
            double d = 0, t = 0;
            for (size_t i = 12001; i < l.size(); ++i)
                d += (double)(l[i] - l[i - 1]) * (l[i] - l[i - 1]), t += (double)l[i] * l[i];
            hf[c] = dB(std::sqrt(d / t));
        }
        check(lv[0] > -28 && lv[0] < -19 && hf[1] < hf[0] - 10, "noise: level and colour",
              fmt("at 50%%: %.1f dB white, %.1f dB dark; dark has %.0f dB less top end", lv[0], lv[1], hf[0] - hf[1]));
    }
}

static Settings swayBase()
{
    Settings s;
    s.oversample = 0;
    s.smartEdges = false;
    int svf = findModel("Fast SVF");
    for (int b = 0; b < 3; ++b)
    {
        s.mode[Parallel].band[b].model = svf;
        s.mode[Parallel].band[b].cutoff = 0.f;
    }
    return s;
}

static void testSway()
{
    // range and stillness
    {
        Settings s = swayBase();
        s.sway.amount = 1.f;
        s.sway.depthSemis = 12.f;
        s.sway.rateHz = 1.f;
        Engine e;
        e.setSampleRate(SR);
        float mn[3] = {1e9, 1e9, 1e9}, mx[3] = {-1e9, -1e9, -1e9};
        runPlaying(e, s, 8.0, 0, 120, [&](int, auto &, auto &)
        {
            for (int b = 0; b < 3; ++b)
                mn[b] = std::min(mn[b], e.displayCutoff(b)), mx[b] = std::max(mx[b], e.displayCutoff(b));
        });
        Settings z = swayBase();
        Engine e2;
        e2.setSampleRate(SR);
        float zr = 0;
        runPlaying(e2, z, 2.0, 0, 120, [&](int, auto &, auto &) { zr = std::max(zr, std::fabs(e2.displayCutoff(Mid))); });
        bool ok = zr < 1e-4f;
        std::string d;
        for (int b = 0; b < 3; ++b)
        {
            ok = ok && mx[b] - mn[b] > 14 && mx[b] < 12.01f && mn[b] > -12.01f;
            d += fmt("%s %+.1f..%+.1f  ", b == 0 ? "Low" : b == 1 ? "Mid" : "High", mn[b], mx[b]);
        }
        check(ok, "sway: cutoff range (12 st depth)", d + fmt("Sway 0: still (%.4f)", zr));
    }
    // synced to Bitwig: the same song position gives the same sway, whatever the start
    {
        Settings s = swayBase();
        s.sway.amount = 1.f;
        s.sway.sync = true;
        s.sway.syncIndex = 2; // 1 bar
        Engine a, b;
        a.setSampleRate(SR);
        b.setSampleRate(SR);
        std::vector<float> ca, cb;
        runPlaying(a, s, 12.0, 0.0, 120, [&](int k, auto &, auto &) { if (k * 64 >= 8 * 48000) ca.push_back(a.displayCutoff(Mid)); });
        runPlaying(b, s, 8.0, 8.0, 120, [&](int k, auto &, auto &) { if (k * 64 >= 4 * 48000) cb.push_back(b.displayCutoff(Mid)); });
        double d = 0;
        for (size_t i = 0; i < std::min(ca.size(), cb.size()); ++i)
            d = std::max(d, (double)std::fabs(ca[i] - cb[i]));
        check(d < 0.01 && ca.size() > 100, "sway: locked to the song position",
              fmt("started at bar 1 or at bar 3: largest difference %.4f st over 4 s", d));
    }
    // stepped: holds, then jumps; 8 steps per cycle
    {
        Settings s = swayBase();
        s.sway.amount = 1.f;
        s.sway.stepped = true;
        s.sway.slew = 0.f;
        s.sway.rateHz = 0.5f;
        Engine e;
        e.setSampleRate(SR);
        float prev = 1e9;
        int jumps = 0;
        bool moving = false;
        runPlaying(e, s, 8.0, 0, 120, [&](int, auto &, auto &)
        {
            float v = e.displayCutoff(Mid);
            bool now = prev < 1e8 && std::fabs(v - prev) > 0.02f;
            if (now && !moving)
                ++jumps; // a jump starts after a hold
            moving = now;
            prev = v;
        });
        check(jumps >= 29 && jumps <= 33, "sway: stepped", fmt("%d jumps in 4 slow cycles (8 per cycle)", jumps));
    }
    // follow: louder input, more sway
    {
        double rng[2];
        for (int loud = 0; loud < 2; ++loud)
        {
            Settings s = swayBase();
            s.sway.amount = 1.f;
            s.sway.follow = 1.f;
            s.sway.rateHz = 1.f;
            Engine e;
            e.setSampleRate(SR);
            float mn = 1e9, mx = -1e9;
            Rng g;
            runPlaying(e, s, 4.0, 0, 120, [&](int k, auto &, auto &)
            {
                if (k > 300)
                    mn = std::min(mn, e.displayCutoff(Mid)), mx = std::max(mx, e.displayCutoff(Mid));
            }, [&](long) { return (loud ? 0.3f : 0.01f) * g.pm1(); });
            rng[loud] = mx - mn;
        }
        check(rng[1] > rng[0] * 4, "sway: follow", fmt("cutoff swing with quiet input %.1f st, loud %.1f st", rng[0], rng[1]));
    }
}

static void testConductor()
{
    struct C
    {
        float infl, lean;
        double lowWant, highWant;
    } cases[] = {{1.f, 0.f, 1.0, 0.0}, {1.f, 1.f, 0.0, 1.0}, {1.f, 0.5f, 0.5, 0.5}, {0.4f, 0.25f, 0.3, 0.1}, {0.f, 0.5f, 0.0, 0.0}};
    bool ok = true;
    std::string d;
    for (auto &c : cases)
    {
        Settings s = swayBase();
        s.sway.amount = 1.f;
        s.sway.rateHz = 0.7f;
        Settings alone = s;
        s.conductor.influence = c.infl;
        s.conductor.lean = c.lean;
        Engine e, e0;
        e.setSampleRate(SR);
        e0.setSampleRate(SR);
        double worst = 0, midSum = 0;
        std::vector<float> lowA, highA, midA, low0, high0;
        runPlaying(e, s, 3.0, 0, 120, [&](int, auto &, auto &) { lowA.push_back(e.displayCutoff(Low)); highA.push_back(e.displayCutoff(High)); midA.push_back(e.displayCutoff(Mid)); });
        runPlaying(e0, alone, 3.0, 0, 120, [&](int, auto &, auto &) { low0.push_back(e0.displayCutoff(Low)); high0.push_back(e0.displayCutoff(High)); });
        for (size_t i = 0; i < lowA.size(); ++i)
        {
            double lowGot = lowA[i] - low0[i], highGot = highA[i] - high0[i];
            worst = std::max({worst, std::fabs(lowGot - c.lowWant * midA[i]), std::fabs(highGot - c.highWant * midA[i])});
            midSum += std::fabs(midA[i]);
        }
        ok = ok && worst < 0.02 && midSum > 100;
        d += fmt("[%.0f%%, lean %.0f%%: off by %.3f st] ", c.infl * 100, c.lean * 100, worst);
    }
    check(ok, "conductor passes Mid's movement on", d);
}

static Settings mutBase(int lowMode, int highMode)
{
    Settings s = swayBase();
    s.mutation.trigger = TrigStep;
    s.mutation.holdSteps = 1;
    s.mutation.resp[0] = {lowMode, TgtCutoff, 0.5f, 1.f, 0};
    s.mutation.resp[1] = {highMode, TgtCutoff, 0.5f, 1.f, 0};
    return s;
}

static void testMutation()
{
    // every sixteenth at 120 bpm: 8 events a second; Low answers with Mid, High against it
    {
        Settings s = mutBase(MutTandem, MutContrast);
        Engine e;
        e.setSampleRate(SR);
        int bad = 0, seen = 0;
        unsigned lastCount = 0;
        float maxLow = 0;
        runPlaying(e, s, 4.0, 0, 120, [&](int, auto &, auto &)
        {
            if (e.mutationEvents() != lastCount)
            {
                lastCount = e.mutationEvents();
                auto ev = e.mutationHistory(0);
                ++seen;
                if (ev.low != ev.dir || ev.high != -ev.dir)
                    ++bad;
            }
            maxLow = std::max(maxLow, std::fabs(e.displayCutoff(Low)));
        });
        check(seen >= 30 && seen <= 33 && bad == 0 && maxLow > 6, "mutation: tandem and contrast",
              fmt("%d events in 4 s at 1/16 (expect 32); Low with Mid, High against it every time; Low cutoff moved up to %.1f st", seen, maxLow));
    }
    // chance 0: nobody answers
    {
        Settings s = mutBase(MutTandem, MutTandem);
        s.mutation.resp[0].chance = s.mutation.resp[1].chance = 0.f;
        Engine e;
        e.setSampleRate(SR);
        float mv = 0;
        runPlaying(e, s, 2.0, 0, 120, [&](int, auto &, auto &) { mv = std::max({mv, std::fabs(e.displayCutoff(Low)), std::fabs(e.displayCutoff(High))}); });
        check(mv < 1e-4f && e.mutationEvents() > 10, "mutation: chance 0", fmt("%u events, nobody moved (%.5f st)", e.mutationEvents(), mv));
    }
    // delay: High answers one step later than Low
    {
        Settings s = mutBase(MutTandem, MutTandem);
        s.mutation.holdSteps = 8;
        s.mutation.resp[1].delaySteps = 1;
        Engine e;
        e.setSampleRate(SR);
        long tLow = -1, tHigh = -1, k0 = 0;
        runPlaying(e, s, 2.0, 0.5, 120, [&](int k, auto &, auto &)
        {
            if (tLow < 0 && std::fabs(e.displayCutoff(Low)) > 2)
                tLow = k;
            if (tHigh < 0 && std::fabs(e.displayCutoff(High)) > 2)
                tHigh = k;
            (void)k0;
        });
        double gapMs = (tHigh - tLow) * 64 / SR * 1000.0;
        check(tLow >= 0 && tHigh > tLow && std::fabs(gapMs - 125) < 6, "mutation: delay", fmt("High answered %.0f ms after Low (one sixteenth at 120 bpm = 125 ms)", gapMs));
    }
    // level hits as the trigger
    {
        Settings s = mutBase(MutTandem, MutOff);
        s.mutation.trigger = TrigLevel;
        s.mutation.holdSteps = 0;
        Engine e;
        e.setSampleRate(SR);
        Rng g;
        runPlaying(e, s, 4.0, 0, 120, [&](int, auto &, auto &) {}, [&](long i)
        {
            long ph = i % 24000; // a hit every half second
            return ph < 2400 ? 0.8f * g.pm1() * (1.f - ph / 2400.f) : 0.005f * g.pm1();
        });
        check(e.mutationEvents() >= 7 && e.mutationEvents() <= 8, "mutation: level hits", fmt("%u events from 8 hits", e.mutationEvents()));
    }
    // Mutate: the answers change bar by bar, and the same bar always gives the same answers
    {
        Settings s = mutBase(MutTandem, MutContrast);
        s.mutation.mutate = 1.f;
        s.mutation.everyIndex = 0; // every bar
        s.mutation.seed = 42;
        Engine a, b;
        a.setSampleRate(SR);
        b.setSampleRate(SR);
        std::vector<MutResponse> ra;
        int changes = 0;
        MutResponse prev{};
        bool first = true;
        runPlaying(a, s, 32.0, 0.0, 120, [&](int k, auto &, auto &)
        {
            const auto &r = a.mutationAnswer(0);
            if (!first && (r.amount != prev.amount || r.mode != prev.mode || r.target != prev.target))
                ++changes;
            prev = r, first = false;
            (void)k;
        });
        MutResponse atEndA = a.mutationAnswer(0);
        runPlaying(b, s, 16.0, 32.0, 120, [&](int, auto &, auto &) {});
        MutResponse atEndB = b.mutationAnswer(0);
        bool same = atEndA.amount == atEndB.amount && atEndA.mode == atEndB.mode && atEndA.target == atEndB.target &&
                    atEndA.chance == atEndB.chance;
        // freeze holds the answers
        Settings f = s;
        f.mutation.freeze = true;
        Engine c;
        c.setSampleRate(SR);
        int fchanges = 0;
        first = true;
        runPlaying(c, f, 16.0, 0.0, 120, [&](int, auto &, auto &)
        {
            const auto &r = c.mutationAnswer(0);
            if (!first && (r.amount != prev.amount || r.mode != prev.mode || r.target != prev.target))
                ++fchanges;
            prev = r, first = false;
        });
        check(changes >= 12 && same && fchanges == 0, "mutation: Mutate over the bars",
              fmt("answers changed %d times in 16 bars; bar 16 reached from bar 1 or bar 9: %s; frozen: %d changes",
                  changes, same ? "identical" : "DIFFERENT", fchanges));
    }
    // the whole thing is repeatable: two runs, same output
    {
        Settings s = mutBase(MutTandem, MutContrast);
        s.sway.amount = 0.6f;
        s.conductor.influence = 0.7f;
        s.mutation.mutate = 0.5f;
        s.mutation.resp[0].chance = 0.6f;
        s.mode[Parallel].feedbackOn = true;
        s.mode[Parallel].feedback = 0.5f;
        std::vector<float> o[2];
        for (int k = 0; k < 2; ++k)
        {
            Engine e;
            e.setSampleRate(SR);
            runPlaying(e, s, 4.0, 16.0, 97, [&](int, auto &l, auto &) { o[k].insert(o[k].end(), l.begin(), l.end()); });
        }
        double d = 0;
        for (size_t i = 0; i < o[0].size(); ++i)
            d = std::max(d, (double)std::fabs(o[0][i] - o[1][i]));
        check(d == 0.0, "repeatable (bounces match)", fmt("two runs from the same song position: largest difference %.1e", d));
    }
}

// ===========================================================================
// Phase 3
// ===========================================================================
static void testLanes()
{
    // bipolar steps on Low's cutoff, 1/16 at 120 bpm: +1, -1, +0.5, 0 ... -> +24, -24, +12, 0 st
    {
        Settings s = swayBase();
        auto &L = s.lane[Low];
        float pat[4] = {1.f, -1.f, 0.5f, 0.f};
        for (int i = 0; i < 16; ++i)
            L.step[i] = pat[i % 4];
        L.target = bandTarget(Low, PCut);
        Engine e;
        e.setSampleRate(SR);
        std::vector<float> mid;
        runPlaying(e, s, 2.0, 0.0, 120, [&](int k, auto &, auto &)
        {
            if ((k * 64) % 6000 >= 2900 && (k * 64) % 6000 < 2964) // middle of each step
                mid.push_back(e.displayCutoff(Low));
        });
        bool ok = mid.size() >= 12;
        std::string d;
        for (size_t i = 0; i < mid.size() && i < 8; ++i)
        {
            float want = 24.f * pat[i % 4];
            ok = ok && std::fabs(mid[i] - want) < 0.05f;
            d += fmt("%+.0f ", mid[i]);
        }
        check(ok, "sequencer: steps go up and down", "cutoff mid-step: " + d + "(want +24 -24 +12 0 ...)");
    }
    // length 5 with Euclid gate, 3 hits: x.x.x? pattern repeats every 5 steps
    {
        Settings s = swayBase();
        auto &L = s.lane[Mid];
        for (int i = 0; i < 16; ++i)
            L.step[i] = 1.f;
        L.length = 5, L.euclid = 1, L.hits = 3, L.rotate = 0;
        L.target = bandTarget(Mid, PCut);
        Engine e;
        e.setSampleRate(SR);
        std::string got;
        runPlaying(e, s, 1.6, 0.0, 120, [&](int k, auto &, auto &)
        {
            if ((k * 64) % 6000 >= 2900 && (k * 64) % 6000 < 2964)
                got += e.displayCutoff(Mid) > 12 ? 'x' : '.';
        });
        std::string want;
        for (size_t j = 0; j < got.size(); ++j)
            want += ((((j % 5) * 3) % 5) < 3) ? 'x' : '.';
        check(got == want && got.size() >= 10, "sequencer: length 5, Euclid 3 hits", got + " (want " + want + ")");
    }
    // swing 75%: steps of 187.5 and 62.5 ms at 120 bpm
    {
        Settings s = swayBase();
        auto &L = s.lane[Low];
        for (int i = 0; i < 16; ++i)
            L.step[i] = (i % 2) ? -1.f : 1.f;
        L.target = bandTarget(Low, PCut);
        L.swing = 0.75f;
        Engine e;
        e.setSampleRate(SR);
        std::vector<long> edges;
        float prev = 0;
        long blk = 0;
        HostTime ht;
        ht.playing = ht.beatsValid = true;
        ht.bpm = 120;
        std::vector<float> l(8), r(8);
        for (blk = 0; blk < 12000; ++blk)
        {
            ht.beats = blk * 8 / SR * 2.0;
            e.setHostTime(ht);
            e.process(l.data(), r.data(), l.data(), r.data(), 8, s);
            float v = e.displayCutoff(Low);
            if (blk > 0 && (v > 0) != (prev > 0))
                edges.push_back(blk * 8);
            prev = v;
        }
        double d1 = (edges.size() > 3) ? (edges[2] - edges[1]) / SR * 1000 : 0, d2 = (edges.size() > 3) ? (edges[3] - edges[2]) / SR * 1000 : 0;
        double a = std::max(d1, d2), b = std::min(d1, d2);
        check(std::fabs(a - 187.5) < 1 && std::fabs(b - 62.5) < 1, "sequencer: swing 75%", fmt("steps last %.1f and %.1f ms (want 187.5 and 62.5)", a, b));
    }
    // Free running at 8 steps a second, transport stopped
    {
        Settings s = swayBase();
        auto &L = s.lane[High];
        for (int i = 0; i < 16; ++i)
            L.step[i] = (i % 2) ? -1.f : 1.f;
        L.target = bandTarget(High, PCut);
        L.freeRun = true, L.speedHz = 8;
        Engine e;
        e.setSampleRate(SR);
        std::vector<float> l(64), r(64);
        int flips = 0;
        float prev = 0;
        for (int b = 0; b < 750; ++b) // 1 s
        {
            e.process(l.data(), r.data(), l.data(), r.data(), 64, s);
            float v = e.displayCutoff(High);
            if (b > 0 && (v > 0) != (prev > 0))
                ++flips;
            prev = v;
        }
        check(flips >= 7 && flips <= 8, "sequencer: Free at 8 steps/s", fmt("%d step changes in 1 s, transport stopped", flips));
    }
}

static void testLocks()
{
    Settings s = swayBase();
    auto &L = s.lane[Mid];
    // Elektron-style: step 3 locks Reso to 0.9 and Cutoff to +12; step 6 locks Reso to 0.1
    L.plock[PRes][2] = 0.9f;
    L.plock[PCut][2] = 12.f;
    L.plock[PRes][5] = 0.1f;
    L.lockGlideMs = 1.f;
    s.mode[Parallel].band[Mid].resonance = 0.4f;
    Engine e;
    e.setSampleRate(SR);
    std::vector<float> got;
    runPlaying(e, s, 1.0, 0.0, 120, [&](int k, auto &, auto &)
    {
        if ((k * 64) % 6000 >= 2900 && (k * 64) % 6000 < 2964)
            got.push_back(e.displayReso(Mid));
    });
    float want[7] = {0.4f, 0.4f, 0.9f, 0.4f, 0.4f, 0.1f, 0.4f}; // (the cutoff lock on step 3 is checked below)
    bool ok = got.size() >= 7;
    std::string d;
    for (int i = 0; i < 7 && i < (int)got.size(); ++i)
        ok = ok && std::fabs(got[i] - want[i]) < 0.01f, d += fmt("%.2f ", got[i]);
    check(ok, "P-locks", "Mid reso per step: " + d + "(knob 0.40; step 3 locked +0.5, step 6 -0.3)");

    // the lock lane's own length (3) and rate (1/8 against the sequencer's 1/16)
    for (int variant = 0; variant < 2; ++variant)
    {
        Settings s2 = swayBase();
        auto &M = s2.lane[Mid];
        M.plock[PRes][0] = 0.9f;
        M.lockGlideMs = 1.f;
        if (variant == 0)
            M.lockLength = 3;
        else
            M.lockRateIndex = 1 + 4, M.lockLength = 2;
        s2.mode[Parallel].band[Mid].resonance = 0.4f;
        Engine e2;
        e2.setSampleRate(SR);
        std::string pat;
        runPlaying(e2, s2, 1.0, 0.0, 120, [&](int k, auto &, auto &)
        {
            if ((k * 64) % 6000 >= 2900 && (k * 64) % 6000 < 2964)
                pat += e2.displayReso(Mid) > 0.6f ? 'x' : '.';
        });
        std::string want = variant == 0 ? "x..x..x" : "xx..xx..";
        bool ok2 = pat.size() >= want.size() && pat.compare(0, want.size(), want) == 0;
        check(ok2, variant == 0 ? "P-locks: own length" : "P-locks: own rate",
              "locked steps " + pat + (variant == 0 ? " (lock length 3)" : " (1/8 lock steps, 2 long, under 1/16 steps)"));
    }
}

// Sway pushes clocked lanes and LFOs ahead of and behind the grid, and comes back
static void testSwayTiming()
{
    auto run = [](float sway, std::vector<int> &steps, std::vector<float> &lfo)
    {
        Settings s = swayBase();
        s.sway.amount = 0.f; // the push works even with Sway off the filters
        s.sway.rateHz = 2.f;
        auto &L = s.lane[Low];
        L.sway = sway;
        L.target = bandTarget(Low, PCut);
        s.lfo[0] = {ShapeRamp, LfoSync, 4, 1.f, 0.f, 0.f, false};
        s.lfo[0].sway = sway;
        Engine e;
        e.setSampleRate(SR);
        runPlaying(e, s, 3.0, 0.0, 120, [&](int, auto &, auto &)
        {
            steps.push_back(e.laneStep(Low));
            lfo.push_back(e.lfoPhase(0));
        });
    };
    std::vector<int> a, b;
    std::vector<float> la, lb;
    run(0.f, a, la);
    run(1.f, b, lb);
    int differ = 0, maxOff = 0;
    float maxPh = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
    {
        int d = std::abs(a[i] - b[i]);
        d = std::min(d, 16 - d);
        differ += d != 0, maxOff = std::max(maxOff, d);
        float dp = std::fabs(la[i] - lb[i]);
        maxPh = std::max(maxPh, std::min(dp, 1.f - dp));
    }
    bool ok = differ > (int)a.size() / 10 && maxOff <= 5 && maxPh > 0.05f && maxPh < 0.26f;
    check(ok, "Sway pushes clocked timing", fmt("lane off the grid %d%% of the time, at most %d steps; LFO phase moved up to %.2f cycle",
                                                (int)(100 * differ / std::max<size_t>(1, a.size())), maxOff, maxPh));
}

// Free can run up to 500 Hz
static void testFastFree()
{
    Settings s = swayBase();
    s.lfo[0] = {ShapeSine, LfoFree, 4, 300.f, 0.f, 0.f, false};
    s.slot[0] = {4, bandTarget(Low, PCut), 0.5f};
    Engine e;
    e.setSampleRate(SR);
    int cross = 0;
    float last = 0;
    bool finite = true;
    runPlaying(e, s, 0.5, 0.0, 120, [&](int, auto &l, auto &)
    {
        for (float v : l)
            finite = finite && std::isfinite(v);
    });
    // count LFO zero crossings by sampling the phase as the engine runs, control tick by tick
    Engine e2;
    e2.setSampleRate(SR);
    Settings s2 = s;
    std::vector<float> in(8, 0.f), oL(8), oR(8);
    for (int k = 0; k < (int)(SR / 8); ++k) // one second in 8-sample blocks
    {
        e2.process(in.data(), in.data(), oL.data(), oR.data(), 8, s2);
        float v = e2.lfoValue(0);
        if ((last < 0) != (v < 0))
            ++cross;
        last = v;
    }
    check(finite && cross > 500 && cross < 700, "Free LFO at 300 Hz", fmt("%d zero crossings in a second (want about 600)", cross));
}

static void testLfos()
{
    // sine, synced to 1 bar at 120 bpm: a 2 s cycle, locked to the song position
    {
        Settings s = swayBase();
        s.lfo[0] = {ShapeSine, LfoSync, 4, 1.f, 0.f, 0.f, false};
        s.slot[0] = {SrcLfo1, bandTarget(Mid, PCut), 0.5f};
        Engine e;
        e.setSampleRate(SR);
        float mx = -1e9, mn = 1e9;
        double atQuarter = 0;
        runPlaying(e, s, 4.0, 0.0, 120, [&](int k, auto &, auto &)
        {
            float v = e.displayCutoff(Mid);
            mx = std::max(mx, v), mn = std::min(mn, v);
            if (k == (int)(0.5 * SR / 64))
                atQuarter = v;
        });
        check(std::fabs(mx - 12) < 0.1 && std::fabs(mn + 12) < 0.1 && std::fabs(atQuarter - 12) < 0.3, "LFO: sine, 1 bar, via the matrix",
              fmt("Mid cutoff %+.1f..%+.1f st at 50%% (want +-12); a quarter bar in: %+.1f (the peak)", mn, mx, atQuarter));
    }
    // every made shape stays in range and moves; a drawn curve is followed
    {
        bool ok = true;
        std::string d;
        for (int sh = 1; sh < numShapes; ++sh)
        {
            Settings s = swayBase();
            s.lfo[1] = {sh, LfoFree, 4, 2.f, 0.f, 0.f, false};
            Engine e;
            e.setSampleRate(SR);
            float mx = -9, mn = 9;
            std::vector<float> l(64), r(64);
            for (int b = 0; b < 750; ++b)
            {
                e.process(l.data(), r.data(), l.data(), r.data(), 64, s);
                mx = std::max(mx, e.lfoValue(1)), mn = std::min(mn, e.lfoValue(1));
            }
            ok = ok && mx <= 1.0001f && mn >= -1.0001f && mx - mn > 0.8f;
        }
        Settings s = swayBase();
        s.lfo[2] = {ShapeCustom, LfoFree, 4, 1.f, 0.f, 0.f, true};
        Engine e;
        e.setSampleRate(SR);
        CurvePoint pts[3] = {{0, -1, 1}, {0.5f, 1, 1}, {1, -1, 1}}; // a triangle
        e.setCustomCurve(2, pts, 3);
        std::vector<float> l(64), r(64);
        float at25 = 0, at50 = 0;
        for (int b = 0; b < 750; ++b)
        {
            e.process(l.data(), r.data(), l.data(), r.data(), 64, s);
            float ph = e.lfoPhase(2);
            if (std::fabs(ph - 0.25f) < 0.002f)
                at25 = e.lfoValue(2);
            if (std::fabs(ph - 0.5f) < 0.002f)
                at50 = e.lfoValue(2);
        }
        ok = ok && std::fabs(at25 - 0.5f) < 0.02f && std::fabs(at50 - 1.f) < 0.02f;
        check(ok, "LFO: every shape, and a drawn curve", fmt("9 made shapes in range and moving; drawn triangle, unipolar: %.2f at 1/4, %.2f at 1/2 (want 0.50, 1.00)", at25, at50));
    }
}

static void testFollower()
{
    // sidechain hits drive Mid's level through the matrix; the plugin's own input is silent
    Settings s = swayBase();
    s.env.source = EnvSidechain;
    s.env.sensDb = 0;
    s.slot[3] = {SrcEnv, bandTarget(High, PCut), 1.f};
    Engine e;
    e.setSampleRate(SR);
    std::vector<float> l(64), r(64), sc(64);
    float peakEnv = 0, quietEnv = 1;
    Rng g;
    for (int b = 0; b < 1500; ++b)
    {
        long t0 = (long)b * 64;
        for (int i = 0; i < 64; ++i)
        {
            long ph = (t0 + i) % 24000;
            sc[i] = ph < 1200 ? 0.9f * std::sin(2 * 3.14159f * 60.f * (t0 + i) / 48000.f) : 0.f; // a kick every 0.5 s
            l[i] = r[i] = 0.f;
        }
        e.process(l.data(), r.data(), l.data(), r.data(), 64, s, sc.data(), sc.data());
        long ph = t0 % 24000;
        if (ph >= 600 && ph < 1200)
            peakEnv = std::max(peakEnv, e.envValue());
        if (ph >= 20000)
            quietEnv = std::min(quietEnv, e.envValue());
    }
    float cut = e.displayCutoff(High);
    Settings s2 = s;
    s2.env.source = EnvInput;
    Engine e2;
    e2.setSampleRate(SR);
    float inEnv = 0;
    for (int b = 0; b < 400; ++b)
    {
        for (int i = 0; i < 64; ++i)
            sc[i] = 0.9f, l[i] = r[i] = 0.f;
        e2.process(l.data(), r.data(), l.data(), r.data(), 64, s2, sc.data(), sc.data());
        inEnv = std::max(inEnv, e2.envValue());
    }
    check(peakEnv > 0.7f && quietEnv < 0.05f && inEnv < 1e-6f, "follower: sidechain",
          fmt("kick on the sidechain: follower %.2f on a hit, %.2f between; listening to the input instead: %.2f (cutoff now %+.1f)", peakEnv, quietEnv, inEnv, cut));
}

// The bank: a band at 0 leaves the sound alone, up boosts it, down takes it out; stereo splits bands
static void testBank()
{
    auto toneLevel = [](double hz, float gainOdd, float stereo, float ripple, bool right, float mix = 1.f, float gainEven = 0.f) {
        Settings s = swayBase();
        auto &B = s.mode[Parallel].band[Mid];
        B.bankMix = mix, B.bankWidth = 0.3f, B.bankStereo = stereo, B.rippleDepth = ripple, B.rippleRateHz = 3.f;
        for (int k = 0; k < 8; ++k)
            B.bankGain[k] = (k & 1) ? gainOdd : gainEven;
        s.mode[Parallel].band[Low].levelDb = -60.f, s.mode[Parallel].band[High].levelDb = -60.f;
        Engine e;
        e.setSampleRate(SR);
        double sum = 0;
        long n = 0;
        double ph = 0;
        runPlaying(e, s, 1.0, 0.0, 120, [&](int k, auto &l, auto &r)
        {
            if (k > 300)
                for (size_t i = 0; i < l.size(); ++i)
                    sum += (right ? r[i] : l[i]) * (right ? r[i] : l[i]), ++n;
        }, [&](long) { ph += 2 * 3.14159265358979 * hz / SR; return 0.3f * (float)std::sin(ph); });
        return 10 * std::log10(sum / std::max(1L, n) + 1e-20);
    };
    double lo = hzToSemi(250.0) - 3, hi = hzToSemi(3000.0) + 3;
    double fOdd = semiToHz(lo + 3.5 * (hi - lo) / 8);
    double off = toneLevel(fOdd, 0.f, 0.f, 0.f, false, 0.f), flat = toneLevel(fOdd, 0.f, 0.f, 0.f, false);
    double up = toneLevel(fOdd, 1.f, 0.f, 0.f, false), down = toneLevel(fOdd, -1.f, 0.f, 0.f, false);
    check(std::fabs(flat - off) < 0.2 && up - off > 6 && off - down > 12, "bank: centre does nothing, up boosts, down cuts",
          fmt("tone on a band: bank off %.1f dB, band at 0 %.1f dB, at +1 %.1f dB, at -1 %.1f dB", off, flat, up, down));
    double sl = toneLevel(fOdd, 1.f, 1.f, 0.f, false), sr = toneLevel(fOdd, 1.f, 1.f, 0.f, true);
    check(sr - sl > 6, "bank: stereo sends alternate bands left and right", fmt("an odd band boosted with Stereo 100%%: left %.1f dB, right %.1f dB", sl, sr));
    double rp = toneLevel(fOdd, 0.f, 0.f, 1.f, false);
    check(std::fabs(rp - off) > 1, "bank: ripple moves through the bands", fmt("full ripple on a flat bank: %.1f dB against %.1f dB", rp, off));
}

// Euclid: Gate keeps the step values only on the hits, Trig plays +1 on the hits; Rotate moves them
static void testEuclid()
{
    auto pattern = [](int mode, int hits, int rotate) {
        Settings s = swayBase();
        auto &L = s.lane[Low];
        L.target = bandTarget(Low, PCut), L.length = 16, L.rateIndex = 2, L.euclid = mode, L.hits = hits, L.rotate = rotate;
        for (int k = 0; k < 16; ++k)
            L.step[k] = 0.5f;
        Engine e;
        e.setSampleRate(SR);
        std::string pat(16, '?');
        runPlaying(e, s, 2.0, 0.0, 120, [&](int, auto &, auto &) {
            int st = e.laneStep(Low);
            if (st >= 0)
                pat[(size_t)st] = std::fabs(e.laneValue(Low)) > 0.9f ? 'X' : std::fabs(e.laneValue(Low)) > 0.4f ? 'x' : '.';
        });
        return pat;
    };
    std::string off = pattern(0, 5, 0), gate = pattern(1, 5, 0), trig = pattern(2, 5, 0), rot = pattern(1, 5, 2);
    auto count = [](const std::string &p, char c) { return (int)std::count(p.begin(), p.end(), c); };
    check(count(off, 'x') == 16 && count(gate, 'x') == 5 && count(trig, 'X') == 5 && rot != gate && count(rot, 'x') == 5, "Euclid",
          "off " + off + ", Gate 5 of 16 " + gate + ", Trig " + trig + ", rotated 2 " + rot);
}

// Sway per band: its own speed, Smooth or Stepped, and its share of the main Sway
static void testSwayBands()
{
    Settings s = swayBase();
    s.sway.amount = 1.f, s.sway.rateHz = 1.f, s.sway.depthSemis = 12.f;
    s.sway.band[Low].motion = 2, s.sway.band[Low].slew = 0.f;
    s.sway.band[Mid].depth = 0.f;
    s.sway.band[High].rateMul = 4.f;
    Engine e;
    e.setSampleRate(SR);
    int held = 0, blocks = 0;
    float prevLow = 1e9f;
    float midMax = 0, hiCross = 0, last = 0;
    runPlaying(e, s, 4.0, 0.0, 120, [&](int k, auto &, auto &) {
        float lv = e.displayCutoff(Low);
        held += std::fabs(lv - prevLow) < 1e-3f, ++blocks, prevLow = lv;
        midMax = std::max(midMax, std::fabs(e.displayCutoff(Mid)));
        float h = e.displayCutoff(High);
        if (k > 0 && (h > 0) != (last > 0))
            ++hiCross;
        last = h;
    });
    bool ok = held > blocks * 3 / 4 && midMax < 0.01f && hiCross > 12;
    check(ok, "Sway per band", fmt("Low stepped: holds still %d%% of the time; Mid at 0%% moves %.2f st; High at 4x crosses the middle %.0f times",
                                   100 * held / std::max(1, blocks), midMax, hiCross));
}

// The Steps LFO: values per step, snapped to a scale, sent to its own target
static void testStepLfo()
{
    Settings s = swayBase();
    auto &F = s.lfo[1];
    F.shape = ShapeSteps, F.mode = LfoSync, F.syncIndex = 4; // 1 bar
    F.stepCount = 4;
    float vals[4] = {0.f, 7.3f / 24.f, -5.2f / 24.f, 12.f / 24.f};
    for (int k = 0; k < 4; ++k)
        F.step[k] = vals[k];
    F.quantize = 2; // major
    F.target = bandTarget(Low, PCut), F.amount = 1.f;
    Engine e;
    e.setSampleRate(SR);
    std::vector<float> got;
    runPlaying(e, s, 2.0, 0.0, 120, [&](int k, auto &, auto &)
    {
        long smp = (long)k * 64;
        if (smp % 24000 >= 11968 && smp % 24000 < 12032) // middle of each quarter-bar step (0.5 s)
            got.push_back(e.displayCutoff(Low));
    });
    // major scale from the root: 7.3 -> 7, -5.2 -> -5, 12 -> 12
    float want[4] = {0.f, 7.f, -5.f, 12.f};
    bool ok = got.size() >= 4;
    std::string d;
    for (int i = 0; i < 4 && i < (int)got.size(); ++i)
        ok = ok && std::fabs(got[i] - want[i]) < 0.05f, d += fmt("%+.1f ", got[i]);
    check(ok, "Steps LFO to its own target", "Low cutoff per step: " + d + "st (want 0 +7 -5 +12, major scale)");
}

// The analyser sees the strips and the output
static void testAnalyser()
{
    Settings s = swayBase();
    Engine e;
    e.setSampleRate(SR);
    double ph = 0;
    runPlaying(e, s, 0.3, 0.0, 120, [](int, auto &, auto &) {}, [&](long) { ph += 2 * 3.14159265358979 * 100 / SR; return 0.5f * (float)std::sin(ph); });
    std::vector<float> b(2048);
    float pk[4]{};
    for (int w = 0; w < 4; ++w)
    {
        e.readAnalyser(w, b.data(), 2048);
        for (float v : b)
            pk[w] = std::max(pk[w], std::fabs(v));
    }
    check(pk[0] > 0.3f && pk[2] < 0.05f && pk[3] > 0.3f, "analyser feed", fmt("100 Hz tone: Low %.2f, Mid %.2f, High %.2f, output %.2f", pk[0], pk[1], pk[2], pk[3]));
}

// Z-Plane: Morph moves the shape from one frame to the other; Cutoff slides it
static void testZPlane()
{
    int zp = findModel("Z-Plane");
    auto level = [&](double hz, float morph, float cutoff) {
        Settings s;
        s.oversample = 0, s.smartEdges = false, s.limiter = false;
        s.routing = Series;
        auto &M = s.mode[Series];
        for (int b = 0; b < 3; ++b)
            M.band[b].model = 0;
        M.band[Low].model = zp, M.band[Low].variant = 0; // Vowel Ah > Ee
        M.band[Low].cutoff = cutoff, M.band[Low].resonance = 0.5f, M.band[Low].morph = morph;
        Engine e;
        e.setSampleRate(SR);
        double sum = 0, ph = 0;
        long n = 0;
        runPlaying(e, s, 0.6, 0.0, 120, [&](int k, auto &l, auto &) {
            if (k > 150)
                for (float v : l)
                    sum += v * v, ++n;
        }, [&](long) { ph += 2 * 3.14159265358979 * hz / SR; return 0.1f * (float)std::sin(ph); });
        return 10 * std::log10(sum / std::max(1L, n) + 1e-20);
    };
    float c0 = (float)hzToSemi(1000.0);
    // first formants measured from E-mu's "Vocal Ah-Ay-Ee": Ah 551 Hz, Ay (middle frame) 334 Hz, Ee 278 Hz
    double a551 = level(551, 0, c0), a278 = level(278, 0, c0), b551 = level(551, 1, c0), b278 = level(278, 1, c0);
    double m334 = level(334, 0.5f, c0), m551 = level(551, 0.5f, c0);
    double up = level(551 * 2, 0, c0 + 12);
    bool finite = std::isfinite(a551) && std::isfinite(b278) && std::isfinite(m334);
    check(finite && a551 - a278 > 6 && b278 - b551 > 6 && m334 - m551 > 6 && std::fabs(up - a551) < 4, "Z-Plane morph and cutoff",
          fmt("Ah frame: 551 Hz %.1f dB, 278 Hz %.1f dB; Ay (middle): 334 Hz %.1f dB, 551 Hz %.1f dB; Ee frame: 551 Hz %.1f dB, 278 Hz %.1f dB; an octave up moves the peak (%.1f dB at 1102 Hz)",
              a551, a278, m334, m551, b551, b278, up));
}

// Mod Depth (master and per strip) and a strip's own Mix
static void testDepthAndMix()
{
    auto swing = [](float master, float strip, bool lock) {
        Settings s = swayBase();
        s.modDepth = master;
        s.mode[Parallel].band[Low].modDepth = strip;
        auto &L = s.lane[Low];
        L.target = bandTarget(Low, PCut), L.depth = 1.f, L.length = 2;
        L.step[0] = 0.5f, L.step[1] = -0.5f;
        if (lock)
            L.plock[PRes][0] = 0.5f, L.lockGlideMs = 1;
        Engine e;
        e.setSampleRate(SR);
        float lo = 1e9f, hi = -1e9f, rmax = 0;
        runPlaying(e, s, 1.0, 0.0, 120, [&](int k, auto &, auto &) {
            if (k > 100)
                lo = std::min(lo, e.displayCutoff(Low)), hi = std::max(hi, e.displayCutoff(Low)), rmax = std::max(rmax, e.displayReso(Low));
        });
        return std::make_pair(hi - lo, rmax);
    };
    float one = swing(1, 1, false).first, none = swing(0, 1, false).first, two = swing(2, 1, false).first, half = swing(1, 0.5f, false).first;
    auto lk = swing(0, 0, true);
    check(none < 0.01f && std::fabs(two - 2 * one) < 0.5f && std::fabs(half - 0.5f * one) < 0.5f && lk.first < 0.01f && lk.second > 0.45f,
          "Mod Depth: master and per strip", fmt("lane swing %.1f st at 100%%, %.2f at 0%%, %.1f at 200%%, %.1f with the strip at 50%%; P-locks still move with depth at 0 (reso %.2f)",
                                                  one, none, two, half, lk.second));
    // a strip's Mix: at 0 the strip leaves its sound alone even with its filter shut
    auto lvl = [](float mix) {
        Settings s = swayBase();
        s.limiter = false;
        s.mode[Parallel].band[Low].cutoff = -60.f, s.mode[Parallel].band[Low].stripMix = mix;
        Engine e;
        e.setSampleRate(SR);
        double sum = 0, ph = 0;
        long n = 0;
        runPlaying(e, s, 0.5, 0.0, 120, [&](int k, auto &l, auto &) {
            if (k > 100)
                for (float v : l)
                    sum += v * v, ++n;
        }, [&](long) { ph += 2 * 3.14159265358979 * 80 / SR; return 0.3f * (float)std::sin(ph); });
        return 10 * std::log10(sum / std::max(1L, n) + 1e-20);
    };
    double m0 = lvl(0), m1 = lvl(1), mh = lvl(0.5f);
    check(m0 - m1 > 15 && mh < m0 && mh > m1, "strip Mix", fmt("80 Hz through a shut Low filter: Mix 0%% %.1f dB, 50%% %.1f dB, 100%% %.1f dB", m0, mh, m1));
}

// Jack, the sporadic Conductor and the ripple shapes
static void testConductorPlus()
{
    // Jack: Low's cutoff moved +12 by its lane; High answers -12 at 100% (Low <> High)
    {
        Settings s = swayBase();
        s.conductor.jack = 1.f, s.conductor.jackPair = 0, s.conductor.jackParam = PCut;
        auto &L = s.lane[Low];
        L.target = bandTarget(Low, PCut), L.length = 1, L.step[0] = 0.5f;
        Engine e;
        e.setSampleRate(SR);
        float lo = 0, hi = 0;
        runPlaying(e, s, 0.5, 0.0, 120, [&](int, auto &, auto &) { lo = e.displayCutoff(Low), hi = e.displayCutoff(High); });
        float hiBase = s.mode[Parallel].band[High].cutoff;
        check(std::fabs((lo - s.mode[Parallel].band[Low].cutoff) - 12.f) < 0.3f && std::fabs((hi - hiBase) + 12.f) < 0.3f, "Jack",
              fmt("Low pushed %+.1f st by its lane, High answers %+.1f st", lo - s.mode[Parallel].band[Low].cutoff, hi - hiBase));
    }
    // a 1/4 grid with Chaos: Low's offset holds still within each beat and changes between beats; same seed, same moves
    auto run = [](int seed, std::vector<float> &v) {
        Settings s = swayBase();
        s.conductor.influence = 1.f, s.conductor.lean = 0.f, s.conductor.grid = 11, s.conductor.chaos = 1.f, s.conductor.range = 12.f, s.conductor.seed = seed;
        s.conductor.musical = 1;
        Engine e;
        e.setSampleRate(SR);
        runPlaying(e, s, 4.0, 0.0, 120, [&](int k, auto &, auto &) { if (k % 25 == 0) v.push_back(e.conductorPassed(Low)); });
    };
    std::vector<float> a, b, c;
    run(5, a), run(5, b), run(6, c);
    std::set<int> vals;
    for (float x : a)
        vals.insert((int)std::lround(x));
    int whole = 0;
    for (float x : a)
        whole += std::fabs(x - std::round(x)) < 0.05f;
    bool semis = whole * 10 >= (int)a.size() * 8; // a few samples catch a jump mid-glide
    check(a == b && a != c && vals.size() >= 3 && semis, "Conductor: grid, chaos, seed",
          fmt("%d different jumps on whole semitones over 8 beats; same seed repeats, another seed differs", (int)vals.size()));
    // ripple shapes all move and stay in range
    int moving = 0;
    for (int sh = 0; sh < 6; ++sh)
    {
        Settings s = swayBase();
        auto &B = s.mode[Parallel].band[Mid];
        B.bankMix = 1.f, B.rippleDepth = 1.f, B.rippleShape = sh, B.rippleSync = true, B.rippleSyncIndex = 4;
        Engine e;
        e.setSampleRate(SR);
        float mn = 1, mx = -1;
        bool fin = true;
        runPlaying(e, s, 2.0, 0.0, 120, [&](int, auto &l, auto &) {
            float g3 = e.bankGainNow(Mid, 3);
            mn = std::min(mn, g3), mx = std::max(mx, g3);
            for (float x : l)
                fin = fin && std::isfinite(x);
        });
        moving += fin && mx - mn > 0.3f && mn >= -1.f && mx <= 1.f;
    }
    check(moving == 6, "ripple shapes", fmt("%d of 6 shapes move a band within -1..1, synced to the beat", moving));
}

static void testMatrix()
{
    // two slots on one target add up; a slot on Feedback does nothing while Feedback is off
    Settings s = swayBase();
    s.lfo[0] = {ShapeSquare, LfoFree, 4, 0.5f, 0.f, 0.f, false};
    s.slot[0] = {SrcLfo1, bandTarget(Low, PRes), 0.3f};
    s.slot[5] = {SrcLfo1, bandTarget(Low, PRes), 0.2f};
    s.slot[6] = {SrcLfo1, TgtFeedback, 1.f};
    s.mode[Parallel].band[Low].resonance = 0.5f;
    Engine e;
    e.setSampleRate(SR);
    std::vector<float> l(64), r(64);
    float mx = 0, mn = 1, fb = 0;
    for (int b = 0; b < 1500; ++b)
    {
        e.process(l.data(), r.data(), l.data(), r.data(), 64, s);
        mx = std::max(mx, e.displayReso(Low)), mn = std::min(mn, e.displayReso(Low));
        fb = std::max(fb, e.displayGlobal(TgtFeedback));
    }
    check(std::fabs(mx - 1.0f) < 0.01f && std::fabs(mn - 0.0f) < 0.01f && fb == 0.f, "mod matrix",
          fmt("two slots (30%% + 20%%) on Low reso from a square LFO: %.2f..%.2f (want 0..1); Feedback slot with Feedback off: %.2f", mn, mx, fb));
}

static void testStepTrigger()
{
    // Mutation's step trigger follows Mid's lane once it has steps: 1/8 at 120 bpm = 4 events a second
    Settings s = mutBase(MutTandem, MutOff);
    auto &L = s.lane[Mid];
    L.length = 4, L.rateIndex = 4; // 1/8
    L.step[0] = 0.5f;
    Engine e;
    e.setSampleRate(SR);
    runPlaying(e, s, 4.0, 0.0, 120, [&](int, auto &, auto &) {});
    unsigned n = e.mutationEvents();
    check(n >= 15 && n <= 16, "mutation: steps of Mid's lane", fmt("%u events in 4 s from a 1/8 lane at 120 bpm (want 15 or 16)", n));
}

static void testSpeed()
{
    int svf = findModel("Fast SVF"), lad = findModel("Vintage Ladder"), obx = findModel("OB-Xd");
    std::string d;
    for (int os = 0; os <= 2; ++os)
    {
        Settings s;
        s.oversample = os;
        auto &m = s.mode[Parallel];
        m.band[Low].model = lad;
        m.band[Mid].model = svf;
        m.band[High].model = obx ? obx : svf;
        for (int b = 0; b < 3; ++b)
        {
            m.band[b].resonance = 0.5f;
            m.band[b].drive = 0.6f;
            m.band[b].driveType = b + 1;
            m.band[b].cutoff = -20.f + 30.f * b;
        }
        Engine e;
        e.setSampleRate(SR);
        const int N = 48000 * 10;
        std::vector<float> l(N), r(N);
        noise(l, r, 0.3f);
        auto t0 = std::chrono::steady_clock::now();
        run(e, s, l, r);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        d += fmt("%dx: %.1f%% of one core  ", 1 << os, 100.0 * sec / 10.0);
    }
    std::printf("INFO  %-34s %s\n", "speed, all three strips busy", d.c_str());
}

// ---------------------------------------------------------------------------
// Demo: a small synthetic loop (kick, snare, hats, saw bass, saw chord) through four
// settings, four seconds each: dry, Parallel, Series, Parallel again with other drives.
// ---------------------------------------------------------------------------
static void writeWav(const char *path, const std::vector<float> &l, const std::vector<float> &r)
{
    FILE *f = std::fopen(path, "wb");
    if (!f)
    {
        std::printf("cannot write %s\n", path);
        return;
    }
    uint32_t n = (uint32_t)l.size(), dataBytes = n * 4, sr = (uint32_t)SR;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    w32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16);
    w16(1);
    w16(2);
    w32(sr);
    w32(sr * 4);
    w16(4);
    w16(16);
    std::fwrite("data", 1, 4, f);
    w32(dataBytes);
    for (uint32_t i = 0; i < n; ++i)
    {
        w16((uint16_t)(int16_t)std::lround(std::clamp(l[i], -1.f, 1.f) * 32767.f));
        w16((uint16_t)(int16_t)std::lround(std::clamp(r[i], -1.f, 1.f) * 32767.f));
    }
    std::fclose(f);
}

static void synthLoop(std::vector<float> &l, std::vector<float> &r, double seconds)
{
    const double bpm = 96.0, beat = 60.0 / bpm, s16 = beat / 4.0;
    int N = (int)(seconds * SR);
    l.assign((size_t)N, 0.f);
    r.assign((size_t)N, 0.f);
    Rng g;
    const int kick[16] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    const int snare[16] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1};
    const int bassNote[16] = {33, 0, 0, 33, 0, 0, 36, 0, 0, 0, 31, 0, 0, 31, 0, 0};
    const double chord[2][4] = {{57, 60, 64, 67}, {55, 58, 62, 65}};
    auto mtof = [](double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); };
    double hpState = 0, bassPhase = 0, bassF = 55, bassEnv = 0, chPh[4][2] = {};
    for (int i = 0; i < N; ++i)
    {
        double t = i / SR;
        int step = (int)(t / s16);
        double ts = t - step * s16;
        int st = step % 16, bar = step / 16;
        double v = 0, vs = 0;
        // kick
        for (int back = 0; back < 4; ++back)
        {
            int sp = step - back;
            if (sp < 0 || !kick[sp % 16])
                continue;
            double tk = t - sp * s16;
            double f = 45 + 110 * std::exp(-tk * 28);
            v += 0.9 * std::sin(2 * 3.14159265 * (45 * tk + 110 / 28.0 * (1 - std::exp(-tk * 28)))) *
                 std::exp(-tk * 7);
            (void)f;
        }
        // snare
        for (int back = 0; back < 3; ++back)
        {
            int sp = step - back;
            if (sp < 0 || !snare[sp % 16])
                continue;
            double tk = t - sp * s16;
            v += (0.35 * g.pm1() + 0.25 * std::sin(2 * 3.14159265 * 190 * tk)) * std::exp(-tk * 22);
        }
        // hats: high-passed noise on every 16th, louder off the beat
        {
            double n = g.pm1();
            hpState += 0.35 * (n - hpState);
            double h = (n - hpState) * std::exp(-ts * (st % 4 == 2 ? 30 : 75)) * (st % 2 ? 0.16 : 0.1);
            vs += h;
        }
        // saw bass
        if (bassNote[st] && ts < 1.0 / SR * 1.5)
        {
            bassF = mtof(bassNote[st]);
            bassEnv = 1;
        }
        bassEnv *= std::exp(-1.0 / (0.22 * SR));
        bassPhase += bassF / SR;
        bassPhase -= std::floor(bassPhase);
        v += 0.33 * (2 * bassPhase - 1) * bassEnv;
        // chord: two detuned saws per note, slow swell per bar
        double cl = 0, cr = 0;
        double tb = t - bar * 16 * s16, env = std::min(1.0, tb * 3) * std::exp(-tb * 0.5);
        for (int n = 0; n < 4; ++n)
        {
            double f = mtof(chord[bar % 2][n]);
            chPh[n][0] += f * 1.004 / SR;
            chPh[n][1] += f * 0.996 / SR;
            chPh[n][0] -= std::floor(chPh[n][0]);
            chPh[n][1] -= std::floor(chPh[n][1]);
            cl += 2 * chPh[n][0] - 1;
            cr += 2 * chPh[n][1] - 1;
        }
        l[(size_t)i] = (float)(0.5 * (v + vs + 0.07 * env * cl));
        r[(size_t)i] = (float)(0.5 * (v - vs * 0.3 + 0.07 * env * cr));
    }
}

static int demo(const char *path)
{
    const double part = 5.0;
    std::vector<float> l, r;
    synthLoop(l, r, part * 4);
    int svf = findModel("Fast SVF"), lad = findModel("Vintage Ladder");
    int lp = findVariant(svf, PassKind::LowPass), bp = findVariant(svf, PassKind::BandPass),
        hp = findVariant(svf, PassKind::HighPass);
    Engine e;
    e.setSampleRate(SR);
    Settings s;
    s.oversample = 2;
    // Parallel settings
    auto &P = s.mode[Parallel];
    P.xoverLoHz = 180;
    P.xoverHiHz = 2800;
    P.outputDb = -3;
    // Series settings
    auto &Q = s.mode[Series];
    Q.outputDb = 5;
    Q.band[Low].model = svf, Q.band[Low].variant = hp, Q.band[Low].cutoff = hzToSemi(40), Q.band[Low].resonance = 0.2f;
    Q.band[Low].driveType = Push, Q.band[Low].drive = 0.45f;
    Q.band[Mid].model = lad, Q.band[Mid].resonance = 0.62f, Q.band[Mid].driveType = Warm, Q.band[Mid].drive = 0.5f;
    Q.band[High].model = svf, Q.band[High].variant = lp, Q.band[High].cutoff = hzToSemi(9000);

    const int chunk = 32;
    int N = (int)l.size();
    float minLim[4] = {1, 1, 1, 1}, minGuard[4] = {1, 1, 1, 1};
    for (int p = 0; p < N; p += chunk)
    {
        double t = p / SR;
        int sec = std::min(3, (int)(t / part));
        double tp = t - sec * part, ph = tp / part;
        if (sec == 0)
        {
            s.routing = Parallel;
            P.mix = 0.f; // dry
        }
        else if (sec == 1)
        {
            // Parallel: warm lows, a fuzzed band-pass sweeping the mids, folded highs
            s.routing = Parallel;
            P.mix = 1.f;
            P.band[Low].model = svf, P.band[Low].variant = lp, P.band[Low].cutoff = hzToSemi(160), P.band[Low].resonance = 0.35f;
            P.band[Low].driveType = Warm, P.band[Low].drive = 0.55f, P.band[Low].levelDb = 1;
            P.band[Mid].model = svf, P.band[Mid].variant = bp, P.band[Mid].resonance = 0.55f;
            P.band[Mid].cutoff = hzToSemi(300.0 * std::pow(8.0, 0.5 - 0.5 * std::cos(2 * 3.14159265 * ph * 2)));
            P.band[Mid].driveType = Fuzz, P.band[Mid].drive = 0.5f, P.band[Mid].driveFirst = false, P.band[Mid].levelDb = 3;
            P.band[High].model = 0;
            P.band[High].driveType = Fold, P.band[High].drive = 0.35f + 0.25f * (float)std::sin(2 * 3.14159265 * ph * 3), P.band[High].levelDb = -2;
        }
        else if (sec == 2)
        {
            // Series: high-pass -> resonant ladder sweep with drive -> low-pass
            s.routing = Series;
            Q.band[Mid].cutoff = hzToSemi(180.0 * std::pow(30.0, 0.5 - 0.5 * std::cos(2 * 3.14159265 * ph)));
        }
        else
        {
            // Parallel again: bitten lows, clean mids, rectified highs, split points moved
            s.routing = Parallel;
            P.xoverLoHz = 240;
            P.xoverHiHz = 1800;
            P.band[Low].model = 0, P.band[Low].driveType = Bite, P.band[Low].drive = 0.6f, P.band[Low].levelDb = 0;
            P.band[Mid].model = svf, P.band[Mid].variant = lp, P.band[Mid].resonance = 0.3f, P.band[Mid].drive = 0.f, P.band[Mid].levelDb = 0;
            P.band[Mid].cutoff = hzToSemi(600.0 * std::pow(4.0, ph));
            P.band[High].model = svf, P.band[High].variant = hp, P.band[High].cutoff = hzToSemi(3000), P.band[High].resonance = 0.4f;
            P.band[High].driveType = Rectify, P.band[High].drive = 0.7f, P.band[High].levelDb = -3;
        }
        int n = std::min(chunk, N - p);
        e.process(&l[(size_t)p], &r[(size_t)p], &l[(size_t)p], &r[(size_t)p], n, s);
        minLim[sec] = std::min(minLim[sec], e.limiterGain());
        minGuard[sec] = std::min({minGuard[sec], e.outputGuardGain(), e.bandGuardGain(0), e.bandGuardGain(1), e.bandGuardGain(2)});
    }
    writeWav(path, l, r);
    std::printf("wrote %s (%.0f s): 0-5 s dry, 5-10 s Parallel, 10-15 s Series, 15-20 s Parallel with other drives\n",
                path, part * 4);
    const char *nm[4] = {"dry", "Parallel", "Series", "Parallel 2"};
    for (int k = 0; k < 4; ++k)
    {
        std::vector<float> part1(l.begin() + (long)(k * part * SR), l.begin() + (long)((k + 1) * part * SR));
        std::printf("  %-10s level %.1f dB RMS, peak %.1f dBFS, most limiting %.1f dB, most guarding %.1f dB\n", nm[k],
                    dB(rms(part1)), dB(peak(part1)), dB(minLim[k]), dB(minGuard[k]));
    }
    std::printf("finite %s\n", finite(l) && finite(r) ? "yes" : "NO");
    return 0;
}

static int list()
{
    const char *kn[4] = {"open", "low-pass", "high-pass", "band-pass"};
    const auto &cat = filterCatalogue();
    for (size_t i = 0; i < cat.size(); ++i)
    {
        std::printf("%2zu %s (%zu variants)\n", i, cat[i].name.c_str(), cat[i].configs.size());
        for (size_t j = 0; j < cat[i].configs.size(); ++j)
            std::printf("      %2zu %-44s %s\n", j, cat[i].labels[j].c_str(), kn[(int)cat[i].kinds[j]]);
    }
    return 0;
}

// 0.9 / 0.10: lane direction, Mod Depth as a target, a slot's Swing
static void testDirectionSwingDepth()
{
    auto runLane = [](int dir, int len)
    {
        Settings s = swayBase();
        s.lane[Low].length = len, s.lane[Low].direction = dir, s.lane[Low].rateIndex = 2; // 1/16
        Engine e;
        e.setSampleRate(SR);
        std::vector<float> l(64), r(64);
        std::vector<int> seq;
        for (int b = 0; b < 3000 && seq.size() < 12; ++b)
        {
            HostTime ht;
            ht.playing = true, ht.beatsValid = true, ht.bpm = 120, ht.beats = (double)b * 64 / SR * 2.0;
            e.setHostTime(ht);
            e.process(l.data(), r.data(), l.data(), r.data(), 64, s);
            int st = e.laneStep(Low);
            if (st >= 0 && (seq.empty() || seq.back() != st))
                seq.push_back(st);
        }
        return seq;
    };
    auto f = runLane(0, 5), rv = runLane(1, 5), pp = runLane(2, 5), rnd = runLane(3, 5), rnd2 = runLane(3, 5);
    std::vector<int> wantF{0, 1, 2, 3, 4, 0, 1}, wantR{4, 3, 2, 1, 0, 4, 3}, wantP{0, 1, 2, 3, 4, 3, 2, 1, 0, 1};
    bool dirOk = std::equal(wantF.begin(), wantF.end(), f.begin()) && std::equal(wantR.begin(), wantR.end(), rv.begin()) &&
                 std::equal(wantP.begin(), wantP.end(), pp.begin()) && rnd == rnd2;
    // Mod Depth as a target: an LFO on it changes how far another slot moves the cutoff
    auto depthSpan = [](bool onDepth)
    {
        Settings s = swayBase();
        s.lfo[0].shape = ShapeSine, s.lfo[0].mode = LfoFree, s.lfo[0].rateHz = 3.f;
        s.lfo[1].shape = ShapeSine, s.lfo[1].mode = LfoFree, s.lfo[1].rateHz = 0.7f;
        s.slot[0] = {SrcLfo1, bandTarget(Low, PCut), 0.5f};
        if (onDepth)
            s.slot[1] = {SrcLfo2, TgtModDepth, 1.f};
        Engine e;
        e.setSampleRate(SR);
        std::vector<float> l(64), r(64);
        float lo = 1e9f, hi = -1e9f;
        for (int b = 0; b < 3000; ++b)
        {
            e.process(l.data(), r.data(), l.data(), r.data(), 64, s);
            float v = e.targetOffset(bandTarget(Low, PCut));
            lo = std::min(lo, v), hi = std::max(hi, v);
        }
        return hi - lo;
    };
    float plain = depthSpan(false), modded = depthSpan(true);
    // Swing: the follower (0..1) set to Both ways goes below the knob as well as above
    auto swingRange = [](int swing)
    {
        Settings s = swayBase();
        s.lfo[0].shape = ShapeSine, s.lfo[0].mode = LfoFree, s.lfo[0].rateHz = 2.f, s.lfo[0].unipolar = true;
        s.slot[0] = {SrcLfo1, bandTarget(Low, PCut), 0.5f, swing};
        Engine e;
        e.setSampleRate(SR);
        std::vector<float> l(64), r(64);
        float lo = 1e9f, hi = -1e9f;
        for (int b = 0; b < 2000; ++b)
        {
            e.process(l.data(), r.data(), l.data(), r.data(), 64, s);
            float v = e.targetOffset(bandTarget(Low, PCut));
            lo = std::min(lo, v), hi = std::max(hi, v);
        }
        return std::make_pair(lo, hi);
    };
    auto as = swingRange(0), both = swingRange(1);
    bool swingOk = as.first > -0.5f && as.second > 10.f && both.first < -10.f && both.second > 10.f;
    check(dirOk && modded > plain * 1.3f && swingOk, "Direction, Mod Depth target, Swing",
          fmt("fwd %d%d%d.. rev %d%d%d.. ping-pong %d%d%d%d%d%d%d; depth span %.1f -> %.1f st; one-way %.1f..%.1f, both ways %.1f..%.1f", f[0], f[1], f[2], rv[0], rv[1], rv[2],
              pp[3], pp[4], pp[5], pp[6], pp[7], pp[8], pp[9], plain, modded, as.first, as.second, both.first, both.second));
}

// Spectral split: with the strips left alone the sound comes back whole, 2048 samples late, in both
// modes; by frequency, muting Low takes a 100 Hz tone out; tonal sends a steady tone to Low (tones)
static void testSpectral()
{
    int quality = 1;
    auto run = [&quality](int by, bool muteLow, float hz, std::vector<float> &in, std::vector<float> &out)
    {
        Settings s;
        s.oversample = 0, s.splitMode = 1, s.specBy = by, s.limiter = false, s.specQuality = quality;
        s.mode[Parallel].band[Low].mute = muteLow;
        Engine e;
        e.setSampleRate(SR);
        const int B = 64, n = 48000 * 2;
        in.assign(n, 0.f), out.assign(n, 0.f);
        Rng g;
        for (int i = 0; i < n; ++i)
            in[i] = hz > 0 ? 0.3f * std::sin(6.2831853f * hz * i / SR) : 0.2f * g.pm1();
        std::vector<float> r(B), ol(B), orr(B);
        for (int k = 0; k + B <= n; k += B)
        {
            e.process(&in[k], &in[k], ol.data(), orr.data(), B, s);
            std::copy(ol.begin(), ol.end(), out.begin() + k);
        }
        return e.latencySamples();
    };
    auto match = [](const std::vector<float> &in, const std::vector<float> &out, int lat)
    {
        double sxy = 0, sxx = 0, syy = 0;
        for (size_t i = 24000; i < out.size(); ++i)
        {
            double x = in[i - (size_t)lat], y = out[i];
            sxy += x * y, sxx += x * x, syy += y * y;
        }
        return sxy / std::sqrt(sxx * syy + 1e-30);
    };
    std::vector<float> in, out, in2, out2;
    int lat = run(0, false, 0, in, out);
    double cf = match(in, out, lat);
    run(1, false, 0, in2, out2);
    double ct = match(in2, out2, lat);
    auto rms = [](const std::vector<float> &v) { double a = 0; for (size_t i = 24000; i < v.size(); ++i) a += (double)v[i] * v[i]; return std::sqrt(a / (double)(v.size() - 24000)); };
    std::vector<float> i3, o3, i4, o4;
    run(0, false, 100.f, i3, o3);
    run(0, true, 100.f, i4, o4);
    double drop = 20 * std::log10(rms(o4) / std::max(1e-9, rms(o3)));
    std::vector<float> i5, o5, i6, o6;
    run(1, false, 440.f, i5, o5);
    run(1, true, 440.f, i6, o6);
    double tone = 20 * std::log10(rms(o6) / std::max(1e-9, rms(o5)));
    // every quality comes back whole, with its own delay
    std::string qs;
    bool qOk = true;
    for (quality = 0; quality < 4; ++quality)
    {
        std::vector<float> qi, qo;
        int ql = run(quality % 2, false, 0, qi, qo);
        double qc = match(qi, qo, ql);
        qs += fmt(" %d:%.4f", ql, qc);
        qOk = qOk && ql == (1024 << quality) && qc > 0.97;
    }
    check(qOk, "Spectral quality", "latency:correlation" + qs);
    check(lat == 2048 && cf > 0.97 && ct > 0.97 && drop < -30 && tone < -12, "Spectral split",
          fmt("latency %d; comes back whole: by frequency %.4f, tonal %.4f (correlation); muting Low takes a 100 Hz tone down %.0f dB; tonal: muting Low (tones) takes a steady 440 Hz down %.0f dB",
              lat, cf, ct, drop, tone));
}

int main(int argc, char **argv)
{
    if (argc >= 3 && std::string(argv[1]) == "demo")
        return demo(argv[2]);
    if (argc >= 2 && std::string(argv[1]) == "list")
        return list();
    testFlat();
    testCrossoverPoints();
    testSmartParallel();
    testSmartSeries();
    testRoutingSwitch();
    testGuard();
    testDrive();
    testAliasing();
    testPan();
    testFeedbackNoise();
    testSway();
    testSwayBands();
    testEuclid();
    testConductor();
    testMutation();
    testLanes();
    testLocks();
    testSwayTiming();
    testBank();
    testConductorPlus();
    testZPlane();
    testDepthAndMix();
    testStepLfo();
    testAnalyser();
    testFastFree();
    testLfos();
    testFollower();
    testMatrix();
    testDirectionSwingDepth();
    testSpectral();
    testStepTrigger();
    testCeilingAllModels();
    testTorture();
    testSpeed();
    std::printf("\n%s (%d failed)\n", failures ? "RESULT: FAIL" : "RESULT: PASS", failures);
    return failures ? 1 : 0;
}
