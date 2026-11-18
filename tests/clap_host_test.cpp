/*
 * Three's Company - a tiny CLAP host that loads the built plugin the way a DAW does and
 * checks it end to end.   usage: clap_host_test <path to the plugin binary>
 */
#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <set>
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <cstdlib>

static int failures = 0;
static void check(bool ok, const char *name, const std::string &detail = "")
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

static const void *hostExt(const clap_host_t *, const char *) { return nullptr; }
static void hostNop(const clap_host_t *) {}
static const clap_host_t host = {CLAP_VERSION_INIT, nullptr, "tc host test", "", "", "1", hostExt, hostNop, hostNop, hostNop};

struct EventList
{
    std::vector<clap_event_param_value_t> ev;
    clap_input_events_t in;
    EventList()
    {
        in.ctx = this;
        in.size = [](const clap_input_events_t *l) { return (uint32_t)((EventList *)l->ctx)->ev.size(); };
        in.get = [](const clap_input_events_t *l, uint32_t i) { return (const clap_event_header_t *)&((EventList *)l->ctx)->ev[i]; };
    }
    void set(uint32_t time, clap_id id, double v)
    {
        clap_event_param_value_t e{};
        e.header.size = sizeof(e);
        e.header.time = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = id;
        e.note_id = -1;
        e.port_index = e.channel = e.key = -1;
        e.value = v;
        ev.push_back(e);
    }
};
static int outValues = 0, outGestures = 0; // what the plugin tells the host
static bool pushNop(const clap_output_events_t *, const clap_event_header_t *h)
{
    if (h->type == CLAP_EVENT_PARAM_VALUE)
        ++outValues;
    if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END)
        ++outGestures;
    return true;
}
static const clap_output_events_t outEvents = {nullptr, pushNop};

struct Mem
{
    std::string data;
    size_t pos{0};
};
static int64_t memWrite(const clap_ostream_t *s, const void *b, uint64_t n)
{
    auto *m = (Mem *)s->ctx;
    n = std::min<uint64_t>(n, 100); // short writes, as a host is allowed to do
    m->data.append((const char *)b, (size_t)n);
    return (int64_t)n;
}
static int64_t memRead(const clap_istream_t *s, void *b, uint64_t n)
{
    auto *m = (Mem *)s->ctx;
    n = std::min<uint64_t>({n, (uint64_t)(m->data.size() - m->pos), (uint64_t)77});
    std::memcpy(b, m->data.data() + m->pos, (size_t)n);
    m->pos += (size_t)n;
    return (int64_t)n;
}

struct Rng
{
    uint32_t s{99};
    float pm1()
    {
        s = s * 1664525u + 1013904223u;
        return (float)((s >> 8) / 8388608.0 - 1.0);
    }
};

// Runs noise through the plugin in blocks; returns peak and whether everything was finite.
static double runAudio(const clap_plugin_t *p, int blocks, int frames, EventList *events, double *rmsOut = nullptr,
                       float amp = 0.5f, double startBeats = -1, std::vector<float> *record = nullptr,
                       double *rmsRight = nullptr)
{
    clap_event_transport_t tr{};
    tr.header.size = sizeof(tr);
    tr.header.type = CLAP_EVENT_TRANSPORT;
    tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE | CLAP_TRANSPORT_IS_PLAYING;
    tr.tempo = 120;
    tr.tsig_num = tr.tsig_denom = 4;
    double sumR = 0;
    std::vector<float> il(frames), ir(frames), ol(frames), orr(frames);
    float *in[2] = {il.data(), ir.data()}, *out[2] = {ol.data(), orr.data()};
    clap_audio_buffer_t ib{in, nullptr, 2, 0, 0}, ob{out, nullptr, 2, 0, 0};
    EventList none;
    Rng g;
    double peak = 0, sum = 0;
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < frames; ++i)
            il[i] = amp * g.pm1(), ir[i] = amp * g.pm1();
        clap_process_t pr{};
        pr.frames_count = (uint32_t)frames;
        pr.audio_inputs = &ib;
        pr.audio_outputs = &ob;
        pr.audio_inputs_count = pr.audio_outputs_count = 1;
        pr.in_events = (b == 0 && events) ? &events->in : &none.in;
        if (startBeats >= 0)
        {
            tr.song_pos_beats = (clap_beattime)std::llround((startBeats + (double)b * frames / 48000.0 * 2.0) * CLAP_BEATTIME_FACTOR);
            pr.transport = &tr;
        }
        pr.out_events = &outEvents;
        if (p->process(p, &pr) == CLAP_PROCESS_ERROR)
            return -1;
        for (int i = 0; i < frames; ++i)
        {
            if (!std::isfinite(ol[i]) || !std::isfinite(orr[i]))
                return -1;
            peak = std::max({peak, (double)std::fabs(ol[i]), (double)std::fabs(orr[i])});
            if (b >= blocks / 2)
                sum += (double)ol[i] * ol[i], sumR += (double)orr[i] * orr[i];
        }
        if (record)
            record->insert(record->end(), ol.begin(), ol.end());
    }
    if (rmsOut)
        *rmsOut = std::sqrt(sum / (double)((blocks - blocks / 2) * frames));
    if (rmsRight)
        *rmsRight = std::sqrt(sumR / (double)((blocks - blocks / 2) * frames));
    return peak;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::printf("usage: clap_host_test <plugin binary>\n");
        return 2;
    }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib)
    {
        std::printf("FAIL  cannot load %s: %s\n", argv[1], dlerror());
        return 1;
    }
    auto *entry = (const clap_plugin_entry_t *)dlsym(lib, "clap_entry");
    check(entry && clap_version_is_compatible(entry->clap_version), "entry point");
    if (!entry)
        return 1;
    check(entry->init(argv[1]), "entry init");
    auto *factory = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    check(factory && factory->get_plugin_count(factory) == 1, "one plugin in the factory");
    auto *desc = factory->get_plugin_descriptor(factory, 0);
    check(desc && desc->id && desc->name, "descriptor", desc ? fmt("%s / %s / %s / %s", desc->name, desc->vendor, desc->version, desc->id) : "");
    const clap_plugin_t *p = factory->create_plugin(factory, &host, desc->id);
    check(p && p->init(p), "create and init");
    if (!p)
        return 1;

    auto *params = (const clap_plugin_params_t *)p->get_extension(p, CLAP_EXT_PARAMS);
    auto *state = (const clap_plugin_state_t *)p->get_extension(p, CLAP_EXT_STATE);
    auto *ports = (const clap_plugin_audio_ports_t *)p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
    check(params && state && ports, "extensions: params, state, ports");

    clap_audio_port_info_t pi{}, sci{};
    check(ports->count(p, true) == 2 && ports->count(p, false) == 1 && ports->get(p, 0, true, &pi) && pi.channel_count == 2 &&
              ports->get(p, 1, true, &sci) && sci.channel_count == 2 && !(sci.flags & CLAP_AUDIO_PORT_IS_MAIN) &&
              !std::strcmp(sci.name, "Sidechain"),
          "stereo in, sidechain, stereo out");

    // every parameter: sane info, unique id, text both ways
    uint32_t n = params->count(p);
    std::set<clap_id> ids;
    std::set<std::string> names;
    bool infoOk = true, textOk = true;
    std::string firstBad;
    std::vector<clap_param_info_t> infos(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        auto &inf = infos[i];
        infoOk = infoOk && params->get_info(p, i, &inf) && inf.max_value > inf.min_value &&
                 inf.default_value >= inf.min_value && inf.default_value <= inf.max_value && inf.name[0];
        ids.insert(inf.id);
        names.insert(inf.name);
        double v = 0;
        infoOk = infoOk && params->get_value(p, inf.id, &v) && std::fabs(v - inf.default_value) < 1e-4;
        for (double t : {0.0, 0.31, 0.5, 1.0})
        {
            double val = inf.min_value + t * (inf.max_value - inf.min_value);
            if (std::string(inf.name).rfind("Lock ", 0) == 0 && val < 0)
                val = -1; // a P-lock below 0 means "not locked", shown as "--"
            if (inf.flags & CLAP_PARAM_IS_STEPPED)
                val = std::round(val);
            char txt[128];
            double back = 0;
            bool ok = params->value_to_text(p, inf.id, val, txt, sizeof(txt)) && txt[0] &&
                      params->text_to_value(p, inf.id, txt, &back);
            // text is rounded for display, so allow 1.5% of the range (variants snap to a step)
            bool variant = std::strstr(inf.name, "Variant") != nullptr;
            ok = ok && (variant || std::fabs(back - val) <= 0.015 * (inf.max_value - inf.min_value) + 1e-9);
            if (!ok && firstBad.empty())
                firstBad = fmt("%s: %g -> '%s' -> %g", inf.name, val, txt, back);
            textOk = textOk && ok;
        }
    }
    check(infoOk && ids.size() == n && names.size() == n, "parameter list", fmt("%u parameters, ids and names unique", n));
    check(textOk, "values to text and back", firstBad);

    auto idOf = [&](const char *name) -> clap_id
    {
        for (auto &inf : infos)
            if (!std::strcmp(inf.name, name))
                return inf.id;
        std::printf("FAIL  no parameter named %s\n", name);
        ++failures;
        return CLAP_INVALID_ID;
    };

    check(p->activate(p, 48000, 1, 512) && p->start_processing(p), "activate at 48 kHz");

    // 1. as loaded it is a wire (filters wide open, drives clean)
    // (quiet noise for this check: the crossover shifts phase, which reshapes the peaks of
    // loud noise enough to touch the limiter, and that is not what is being measured here)
    double rw = 0, r0 = 0, pk = runAudio(p, 200, 256, nullptr, &rw, 0.2f);
    double want = 0.2 / std::sqrt(3.0);
    check(pk > 0 && std::fabs(20 * std::log10(rw / want)) < 0.2, "loads sounding like a wire",
          fmt("level %+.2f dB against the input", 20 * std::log10(rw / want)));
    runAudio(p, 200, 256, nullptr, &r0);

    // 2. a parameter change arriving mid-block takes effect
    {
        EventList e;
        e.set(100, idOf("Par Low Cutoff"), -60);
        e.set(100, idOf("Par Mid Cutoff"), -60);
        e.set(100, idOf("Par High Cutoff"), -60);
        e.set(100, idOf("No Dead Bands"), 0);
        double r = 0;
        runAudio(p, 200, 256, &e, &r);
        check(r < r0 * 0.05, "automation: closing all three filters", fmt("%.1f dB quieter", 20 * std::log10(r / r0)));
        EventList e2;
        e2.set(0, idOf("No Dead Bands"), 1);
        runAudio(p, 200, 256, &e2, &r);
        check(r > r0 * 0.05, "automation: No Dead Bands back on", fmt("now %.1f dB under the open level", 20 * std::log10(r / r0)));
    }

    // 3. Series has its own settings: flipping Routing brings back a wire
    {
        EventList e;
        e.set(0, idOf("Routing"), 1);
        double r = 0;
        runAudio(p, 200, 256, &e, &r, 0.2f);
        check(std::fabs(20 * std::log10(r / want)) < 0.2, "Series kept its own settings",
              fmt("level %+.2f dB against the input while Parallel is closed down", 20 * std::log10(r / want)));
    }

    // 4. worst case stays under the ceiling
    {
        EventList e;
        for (auto &inf : infos)
        {
            const char *nm = inf.name;
            if (std::strstr(nm, "Resonance") || std::strstr(nm, "Drive") == nm + std::strlen(nm) - 5)
                e.set(0, inf.id, 1);
            if (std::strstr(nm, "Input") || std::strstr(nm, "Output"))
                e.set(0, inf.id, 24);
            if (std::strstr(nm, "Level"))
                e.set(0, inf.id, 12);
            if (std::strstr(nm, "Drive Type"))
                e.set(0, inf.id, 3);
            if (std::strstr(nm, "Feedback") || std::strstr(nm, "Noise On") || !std::strcmp(nm, "Sway") ||
                std::strstr(nm, "Influence") || !std::strcmp(nm, "Mutate") || std::strstr(nm, "Amount") ||
                std::strstr(nm, "Sensitivity"))
                e.set(0, inf.id, inf.max_value);
        }
        e.set(0, idOf("Sway Depth"), 48);
        e.set(0, idOf("Mutation Trigger"), 3);
        double worst = 0;
        for (int routing = 0; routing < 2; ++routing)
        {
            e.set(0, idOf("Routing"), routing);
            worst = std::max(worst, runAudio(p, 300, 256, &e, nullptr, 1.0f, 0.0));
        }
        check(worst > 0 && worst <= 0.8913, "everything maxed, both routings", fmt("peak %.4f (%.2f dBFS)", worst, 20 * std::log10(worst)));
    }

    // 4b. back to the starting values (state load), then the phase 2 controls
    auto loadDefaults = [&](const clap_plugin_t *pp)
    {
        Mem m;
        m.data = "THREESCOMPANY 1\n";
        clap_istream_t is{&m, memRead};
        auto *st = (const clap_plugin_state_t *)pp->get_extension(pp, CLAP_EXT_STATE);
        return st->load(pp, &is);
    };
    {
        loadDefaults(p);
        EventList e;
        e.set(0, idOf("Par Low Pan"), -1);
        e.set(0, idOf("Par Mid Pan"), -1);
        e.set(0, idOf("Par High Pan"), -1);
        double rl = 0, rr = 0;
        runAudio(p, 200, 256, &e, &rl, 0.2f, -1, nullptr, &rr);
        check(rr < rl * 0.001, "pan: all strips hard left", fmt("right channel %.0f dB under the left", 20 * std::log10(rr / rl + 1e-12)));
    }
    {
        // Mutation on every sixteenth, Low answering by Level, at Bitwig's song position: the
        // output follows the transport, and the same position plays back the same
        auto setup = [&](const clap_plugin_t *pp)
        {
            loadDefaults(pp);
            EventList e;
            e.set(0, idOf("Mutation Trigger"), 3);
            e.set(0, idOf("Mutation High Moves"), 3);
            e.set(0, idOf("Mutation High Amount"), 1);
            e.set(0, idOf("Sway"), 0.7);
            e.set(0, idOf("Sway Sync"), 1);
            e.set(0, idOf("Chaperone Influence"), 0.8);
            e.set(0, idOf("Mutate"), 0.6);
            e.set(0, idOf("Par Feedback On"), 1);
            return e;
        };
        auto *q = factory->create_plugin(factory, &host, desc->id);
        q->init(q);
        q->activate(q, 48000, 1, 512);
        q->start_processing(q);
        EventList e1 = setup(p), e2 = setup(q);
        p->reset(p); // as on a transport restart; the second instance is fresh
        std::vector<float> a, b;
        runAudio(p, 400, 256, &e1, nullptr, 0.2f, 32.0, &a);
        runAudio(q, 400, 256, &e2, nullptr, 0.2f, 32.0, &b);
        double d = 0;
        for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
            d = std::max(d, (double)std::fabs(a[i] - b[i]));
        // and it moves: the level changes from step to step
        double mn = 1e9, mx = 0;
        for (size_t s0 = 48000; s0 + 3000 < a.size(); s0 += 3000)
        {
            double acc = 0;
            for (size_t i = s0; i < s0 + 3000; ++i)
                acc += (double)a[i] * a[i];
            mn = std::min(mn, acc), mx = std::max(mx, acc);
        }
        check(d < 1e-6 && mx > mn * 2, "transport: Sway, Conductor, Mutation",
              fmt("two instances from bar 9: largest difference %.1e; level swings %.1f dB step to step", d, 10 * std::log10(mx / mn)));
        q->stop_processing(q);
        q->deactivate(q);
        q->destroy(q);
    }

    // 4c. the window's bridge, driven the way the page drives it
    {
        struct TB
        {
            void (*send)(const clap_plugin_t *, const char *);
            const char *(*poll)(const clap_plugin_t *);
        };
        auto *tb = (const TB *)p->get_extension(p, "com.otiskanno.threes-company/test-bridge");
        bool ok = tb != nullptr;
        std::string init, upd;
        if (ok)
        {
            tb->send(p, "ready");
            init = tb->poll(p);
            ok = init.rfind("tc.init({\"params\":[", 0) == 0 && init.find("\"models\":[") != std::string::npos &&
                 init.find("\"curves\":[") != std::string::npos;
            // the page turns a knob: the value moves and a later update reports it with its text
            clap_id cut = idOf("Par Mid Cutoff");
            tb->send(p, ("begin " + std::to_string(cut)).c_str());
            tb->send(p, ("set " + std::to_string(cut) + " 12").c_str());
            tb->send(p, ("end " + std::to_string(cut)).c_str());
            double v = 0;
            params->get_value(p, cut, &v);
            upd = tb->poll(p);
            ok = ok && std::fabs(v - 12) < 1e-4 && upd.find("\"" + std::to_string(cut) + "\":[12,\"880 Hz\"]") != std::string::npos;
            // the host hears about it: value and gesture events come out of the next process call
            outValues = outGestures = 0;
            runAudio(p, 1, 64, nullptr);
            ok = ok && outValues == 1 && outGestures == 2;
            // garbage is ignored
            tb->send(p, "set 999999 1");
            tb->send(p, "curve 7 2 0 0 1 1 1 1");
            tb->send(p, "nonsense");
            tb->send(p, "");
        }
        check(ok, "window bridge", fmt("init %zu bytes; a knob moved from the page reaches the plugin and comes back with its text", init.size()));
        // a drawn curve from the page is saved with the project and comes back
        if (tb)
        {
            tb->send(p, "curve 1 4 0 -1 1 0.3 0.8 0.5 0.7 -0.2 2 1 0.5 1");
            Mem m;
            clap_ostream_t os{&m, memWrite};
            state->save(p, &os);
            bool has = m.data.find("C 1 4 0 -1 1 0.3 0.8 0.5 0.7 -0.2 2 1 0.5 1") != std::string::npos;
            check(has, "drawn curve saved with the project", has ? "" : m.data.substr(m.data.find("C 0"), 120));
        }
        // every factory preset loads, sounds safe, and has all three strips in place: Mid on, wide open (neutral)
        if (tb)
        {
            size_t at = init.find("\"presets\":[");
            int count = 0;
            size_t end = at == std::string::npos ? at : init.find("]]", at);
            for (size_t i = at; at != std::string::npos && i < end; i = init.find("[\"", i + 1))
                if (i > at)
                    ++count;
            int bad = 0, midSet = 0, own = 0;
            // Init and the 41 classic presets come first; the presets after them set their own Mid strip
            own = count - 42;
            double worst = 0;
            std::string firstBad;
            for (int k = 0; k < count; ++k)
            {
                tb->send(p, ("preset " + std::to_string(k)).c_str());
                double r = 0;
                double pk = runAudio(p, 120, 256, nullptr, &r, 0.5f, 8.0);
                worst = std::max(worst, pk);
                if (!(pk >= 0 && pk < 0.92))
                {
                    ++bad;
                    if (firstBad.empty())
                        firstBad = std::to_string(k);
                }
                double rt = 0, mt = 0;
                params->get_value(p, idOf("Routing"), &rt);
                params->get_value(p, idOf(rt > 0.5 ? "Ser Mid Filter Type" : "Par Mid Filter Type"), &mt);
                double mon = 0, mcut = 0;
                params->get_value(p, idOf(rt > 0.5 ? "Ser Mid Filter On" : "Par Mid Filter On"), &mon);
                params->get_value(p, idOf(rt > 0.5 ? "Ser Mid Cutoff" : "Par Mid Cutoff"), &mcut);
                if (k < count - own && !(mt > 0.5 && mon > 0.5 && mcut > 81)) // the classic set (our own presets come after)
                    ++midSet;
            }
            tb->send(p, "preset 0");
            check(count > 40 && bad == 0 && midSet == 0, "factory presets",
                  fmt("%d presets load and play: loudest %.2f, %d unsafe%s, %d without an open Mid filter", count, worst, bad,
                      firstBad.empty() ? "" : (" (first " + firstBad + ")").c_str(), midSet));
        }
    }

    // 4b. (TC_AUDIT=1) every parameter reaches the sound: in a setting where it should matter, moving it
    // away from where it starts changes the output. Lists the ones that don't.
    if (std::getenv("TC_AUDIT"))
    {
        struct TB
        {
            void (*send)(const clap_plugin_t *, const char *);
            const char *(*poll)(const clap_plugin_t *);
        };
        auto *tb = (const TB *)p->get_extension(p, "com.otiskanno.threes-company/test-bridge");
        std::map<std::string, const clap_param_info_t *> byName;
        for (auto &inf : infos)
            byName[inf.name] = &inf;
        auto has = [&](const std::string &n) { return byName.count(n) > 0; };
        auto starts = [](const std::string &s, const char *q) { return s.rfind(q, 0) == 0; };
        int blocks = 160;
        auto render = [&](const std::vector<std::pair<std::string, double>> &ctx, std::vector<float> &rec)
        {
            p->stop_processing(p);
            p->deactivate(p);
            tb->send(p, "preset 0");
            tb->poll(p);
            p->activate(p, 48000, 1, 512);
            p->start_processing(p);
            EventList ev;
            for (auto &kv : ctx)
                if (has(kv.first))
                    ev.set(0, byName[kv.first]->id, kv.second);
            rec.clear();
            runAudio(p, blocks, 256, &ev, nullptr, 0.3f, 0.0, &rec);
        };
        auto rmsDiff = [](const std::vector<float> &a, const std::vector<float> &b)
        {
            double s2 = 0;
            for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
                s2 += (double)(a[i] - b[i]) * (a[i] - b[i]);
            return std::sqrt(s2 / (double)std::max<size_t>(1, a.size()));
        };
        const char *bands[3] = {"Low", "Mid", "High"};
        int tested = 0, dead = 0;
        std::string deadList;
        int part = 0, parts = 1, idx = -1;
        std::sscanf(std::getenv("TC_AUDIT"), "%d/%d", &part, &parts);
        for (auto &inf : infos)
        {
            if (++idx % std::max(1, parts) != part)
                continue;
            std::string n = inf.name;
            if (std::getenv("TC_AUDIT_ONLY") && !std::regex_search(n, std::regex(std::getenv("TC_AUDIT_ONLY"))))
                continue;
            std::vector<std::pair<std::string, double>> ctx;
            bool ser = starts(n, "Ser ");
            std::string R = ser ? "Ser " : "Par ";
            ctx.push_back({"Routing", ser ? 1 : 0});
            // each strip's filter part-way into its own band, so moving it is heard
            const double cut[3] = {-15, 20, 50};
            for (int k = 0; k < 3; ++k)
                ctx.push_back({R + bands[k] + " Cutoff", cut[k]}), ctx.push_back({R + bands[k] + " Resonance", 0.4});
            blocks = 160;
            if (n.find("Feedback") != std::string::npos || n.find("Fb") != std::string::npos)
                ctx.push_back({R + "Feedback On", 1}), ctx.push_back({R + "Feedback", 0.5});
            if (n.find("Noise") != std::string::npos)
                ctx.push_back({R + "Noise On", 1}), ctx.push_back({R + "Noise", 0.3});
            for (auto *b : bands)
                if (n.find(std::string(b) + " Drive") != std::string::npos || n.find("Drive First") != std::string::npos)
                    ctx.push_back({R + b + " Drive", 0.6});
            if (n.find("Mod Depth") != std::string::npos)
                for (int k = 0; k < 3; ++k)
                    ctx.push_back({"Mod " + std::to_string(k + 1) + " Source", 4}), ctx.push_back({"Mod " + std::to_string(k + 1) + " Target", 1 + 6 * k}),
                        ctx.push_back({"Mod " + std::to_string(k + 1) + " Amount", 0.8});
            if (starts(n, "Seq ") || starts(n, "Lock "))
                blocks = 240;
            if (starts(n, "Seq ") || starts(n, "Lock "))
                for (auto *b : bands) // all 32 steps in two beats
                    ctx.push_back({std::string("Seq ") + b + " Length", 32}), ctx.push_back({std::string("Seq ") + b + " Rate", 9});
            if (n.find(" Hits") != std::string::npos || n.find(" Rotate") != std::string::npos)
                ctx.push_back({n.substr(0, n.rfind(' ')) + " Euclid", 1});
            if (n.find("Seq ") == 0 && n.find(" Speed") != std::string::npos)
                ctx.push_back({n.substr(0, n.rfind(' ')) + " Timing", 1});
            if (starts(n, "Seq ") || starts(n, "Lock ") || starts(n, "Chaperone") || starts(n, "3rd Wheel") || starts(n, "Mutat") || n == "Retrigger")
                for (auto *b : bands)
                    for (int i = 1; i <= 32; ++i)
                        ctx.push_back({std::string("Seq ") + b + " Step " + std::to_string(i), (i % 2 ? 0.9 : -0.9) * (i % 3 ? 1 : 0.4)});
            if (starts(n, "Lock "))
                for (auto *b : bands)
                    for (int i = 1; i <= 32; i += 3)
                        ctx.push_back({std::string("Lock ") + b + " Cutoff " + std::to_string(i), 0.3}),
                            ctx.push_back({std::string("Lock ") + b + " Reso " + std::to_string(i + 1), 0.9});
            if (starts(n, "LFO "))
            {
                std::string q = n.substr(0, 6);
                ctx.push_back({q + "Target", 1 + 6 * (n[4] - '1')}), ctx.push_back({q + "Amount", 0.8});
                if (n.find("Rate") != std::string::npos)
                    ctx.push_back({q + "Timing", 1});
                if (n.find("Step") != std::string::npos || n.find("Glide") != std::string::npos || n.find("Scale") != std::string::npos)
                {
                    ctx.push_back({q + "Sync Rate", 0});
                    ctx.push_back({q + "Shape", byName[q + "Shape"]->max_value});
                    for (int i = 1; i <= 16; ++i)
                        ctx.push_back({q + "Step " + std::to_string(i), (i % 2 ? 0.7 : -0.5) * (i % 3 ? 1 : 0.3)});
                }
            }
            if (starts(n, "Mod ") && n.find("Depth") == std::string::npos)
            {
                std::string q = n.substr(0, n.find(' ', 4) + 1);
                ctx.push_back({q + "Source", 4}), ctx.push_back({q + "Target", 1}), ctx.push_back({q + "Amount", 0.6});
            }
            if (starts(n, "Follower") || starts(n, "Sidechain") || n.find("Env") != std::string::npos)
                ctx.push_back({"Mod 1 Source", 7}), ctx.push_back({"Mod 1 Target", 1}), ctx.push_back({"Mod 1 Amount", 0.9});
            if (starts(n, "Sway"))
            {
                ctx.push_back({"Sway", 0.8}), ctx.push_back({"Sway Depth", 24}), ctx.push_back({"Sway Rate", std::log2(8.0)});
                for (auto *s2 : {"Sway to Reso", "Sway to Morph", "Sway to Drive", "Sway to Level", "Sway to Pan"})
                    if (n != s2)
                        ctx.push_back({s2, 0.5});
                if (n == "Sway Sync Length")
                    ctx.push_back({"Sway Sync", 1}), ctx.push_back({n, 0});
                if (n.find("Motion") != std::string::npos || n.find("Slew") != std::string::npos)
                    blocks = 600;
            }
            if (starts(n, "Chaperone") || starts(n, "3rd Wheel"))
                ctx.push_back({"Chaperone Influence", 1}), ctx.push_back({"3rd Wheel", 0.8}), ctx.push_back({"Chaperone Chaos", 0.5}),
                    ctx.push_back({"Chaperone Grid", 5});
            if (starts(n, "Mutat"))
                ctx.push_back({"Mutation Trigger", 3}), ctx.push_back({"Mutate", 0.8}), ctx.push_back({"Mutate Every", 0});
            if (starts(n, "Mutat") || starts(n, "Chaperone Bars") || starts(n, "Chaperone Bursts") || n == "3rd Wheel Mischief" || n == "Retrigger" ||
                n == "Chaperone Snap")
                blocks = 1600; // a few bars
            if (n == "Retrigger")
                ctx.push_back({"Seq Low Length", 13});
            if (n.find("Sidechain") != std::string::npos)
                ctx.push_back({"Follower Listens To", 1});
            if (n.find("Bank") != std::string::npos)
                for (auto &kv : byName)
                    if (kv.first.find(R) == 0 && kv.first.find("Bank Band") != std::string::npos)
                        ctx.push_back({kv.first, 0.7});
            if (n.find("Bank Ripple Sync") != std::string::npos)
                ctx.push_back({n.substr(0, n.find("Sync")) + "Timing", 1});
            if (n == "Auto Gain")
                blocks = 1600;
            if (n.find("Bank Ripple ") != std::string::npos)
                ctx.push_back({n.substr(0, n.find("Bank Ripple ") + 11), 0.8});
            // the move: to the far end from where it starts (or the next choice)
            double v0 = inf.default_value, v1;
            for (auto &kv : ctx)
                if (kv.first == n)
                    v0 = kv.second;
            if (inf.flags & CLAP_PARAM_IS_STEPPED)
                v1 = v0 + 1 <= inf.max_value ? v0 + 1 : v0 - 1;
            else
                v1 = (v0 - inf.min_value) > 0.5 * (inf.max_value - inf.min_value) ? inf.min_value + 0.15 * (inf.max_value - inf.min_value)
                                                                                  : inf.max_value - 0.15 * (inf.max_value - inf.min_value);
            if (!(inf.flags & CLAP_PARAM_IS_STEPPED) && (n.find("Speed") != std::string::npos || n.find("Rate") != std::string::npos))
                v1 = inf.max_value - 0.3 * (inf.max_value - inf.min_value);
            std::vector<float> a, a2, b;
            std::fprintf(stderr, "audit %s\n", n.c_str());
            render(ctx, a);
            render(ctx, a2);
            auto ctxB = ctx;
            ctxB.push_back({n, v1});
            render(ctxB, b);
            double noise = rmsDiff(a, a2), d = rmsDiff(a, b);
            ++tested;
            if (!(d > 3 * noise + 1e-5))
            {
                ++dead;
                deadList += "    " + n + fmt(" (%g -> %g)\n", v0, v1);
            }
        }
        std::printf("AUDIT %d parameters, %d made no difference to the sound here:\n%s", tested, dead, deadList.c_str());
    }

    // 5. save, load into a second instance, every value comes back
    {
        Mem m;
        clap_ostream_t os{&m, memWrite};
        bool saved = state->save(p, &os);
        const clap_plugin_t *q = factory->create_plugin(factory, &host, desc->id);
        q->init(q);
        auto *qs = (const clap_plugin_state_t *)q->get_extension(q, CLAP_EXT_STATE);
        auto *qp = (const clap_plugin_params_t *)q->get_extension(q, CLAP_EXT_PARAMS);
        clap_istream_t is{&m, memRead};
        bool loaded = qs->load(q, &is);
        int diff = 0;
        for (auto &inf : infos)
        {
            double a = 0, b = 0;
            params->get_value(p, inf.id, &a);
            qp->get_value(q, inf.id, &b);
            if (std::fabs(a - b) > 1e-5 * std::max(1.0, std::fabs(a)))
                ++diff;
        }
        check(saved && loaded && diff == 0, "save and reload", fmt("%zu bytes, %d values differ", m.data.size(), diff));
        Mem bad;
        bad.data = "something else entirely";
        clap_istream_t bis{&bad, memRead};
        check(!qs->load(q, &bis), "refuses a state that is not its own");
        q->destroy(q);
    }

    // 6. odd block sizes, including one frame and the largest allowed
    {
        bool ok = true;
        for (int frames : {1, 7, 33, 512})
            ok = ok && runAudio(p, 50, frames, nullptr) >= 0;
        check(ok, "block sizes 1, 7, 33, 512");
    }

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    entry->deinit();
    dlclose(lib);
    std::printf("\n%s (%d failed)\n", failures ? "RESULT: FAIL" : "RESULT: PASS", failures);
    return failures ? 1 : 0;
}
