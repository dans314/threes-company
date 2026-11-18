// Three's Company - records what the plugin sends its window, for testing the page in a browser.
//   gui_dump <plugin binary> <out.js> [messages.txt]
// Loads the plugin, optionally replays messages from the page (one per line), plays 3 s of audio
// with modulation running, and writes the page's init call followed by 90 updates as a JS array.
#include <clap/clap.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <string>
#include <vector>

struct TB
{
    void (*send)(const clap_plugin_t *, const char *);
    const char *(*poll)(const clap_plugin_t *);
};
static uint32_t inSize(const clap_input_events_t *) { return 0; }
static const clap_event_header_t *inGet(const clap_input_events_t *, uint32_t) { return nullptr; }
static bool outPush(const clap_output_events_t *, const clap_event_header_t *) { return true; }
static const void *hostExt(const clap_host_t *, const char *) { return nullptr; }
static void hostNop(const clap_host_t *) {}

static std::string jsStr(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\')
            o += '\\', o += (char)c;
        else if (c == '\n')
            o += "\\n";
        else if (c < 0x20)
            o += ' ';
        else
            o += (char)c;
    }
    return o + "\"";
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 1;
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return std::printf("cannot load %s\n", dlerror()), 1;
    auto *entry = (const clap_plugin_entry_t *)dlsym(lib, "clap_entry");
    entry->init(argv[1]);
    auto *f = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    clap_host_t host{CLAP_VERSION_INIT, nullptr, "gui_dump", "", "", "1", hostExt, hostNop, hostNop, hostNop};
    auto *p = f->create_plugin(f, &host, f->get_plugin_descriptor(f, 0)->id);
    p->init(p);
    p->activate(p, 48000, 1, 256);
    p->start_processing(p);
    auto *tb = (const TB *)p->get_extension(p, "com.otiskanno.threes-company/test-bridge");
    if (argc > 3)
    {
        std::ifstream in(argv[3]);
        std::string line;
        while (std::getline(in, line))
            if (!line.empty())
                tb->send(p, line.c_str());
    }
    tb->send(p, "ready");
    std::vector<std::string> out;
    out.push_back(tb->poll(p));
    std::vector<float> l(256), r(256), sl(256), sr(256);
    float *ins[2] = {l.data(), r.data()}, *outs[2] = {l.data(), r.data()}, *scs[2] = {sl.data(), sr.data()};
    clap_audio_buffer_t ib[2] = {{ins, nullptr, 2, 0, 0}, {scs, nullptr, 2, 0, 0}}, ob{outs, nullptr, 2, 0, 0};
    clap_input_events_t ie{nullptr, inSize, inGet};
    clap_output_events_t oe{nullptr, outPush};
    clap_event_transport_t tr{};
    tr.header = {sizeof(tr), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
    tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE | CLAP_TRANSPORT_IS_PLAYING;
    tr.tempo = 120, tr.tsig_num = tr.tsig_denom = 4;
    uint32_t seed = 1;
    long blk = 0;
    for (int tick = 0; tick < 90; ++tick) // 30 updates a second, 3 s
    {
        for (int k = 0; k < 6; ++k, ++blk) // 6 x 256 samples ~ 1/30 s
        {
            for (int i = 0; i < 256; ++i)
            {
                seed = seed * 1664525u + 1013904223u;
                l[i] = r[i] = 0.25f * ((seed >> 9) / 4194304.f - 1.f);
                long t = blk * 256 + i;
                sl[i] = sr[i] = (t % 24000) < 1500 ? 0.8f * std::sin(t * 0.008f) : 0.f;
            }
            tr.song_pos_beats = (clap_beattime)std::llround(blk * 256 / 48000.0 * 2.0 * CLAP_BEATTIME_FACTOR);
            clap_process_t pr{};
            pr.frames_count = 256;
            pr.transport = &tr;
            pr.audio_inputs = ib;
            pr.audio_inputs_count = 2;
            pr.audio_outputs = &ob;
            pr.audio_outputs_count = 1;
            pr.in_events = &ie;
            pr.out_events = &oe;
            p->process(p, &pr);
        }
        out.push_back(tb->poll(p));
    }
    std::ofstream o(argv[2]);
    o << "window.__dump = [";
    for (size_t i = 0; i < out.size(); ++i)
        o << (i ? ",\n" : "") << jsStr(out[i]);
    o << "];\n";
    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    entry->deinit();
    std::printf("wrote %zu messages\n", out.size());
    return 0;
}
