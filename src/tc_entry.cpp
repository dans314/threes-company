// The CLAP entry point for the CMake build. clap-wrapper builds the CLAP, VST3 and AU from the same
// plugin code (tc_clap.cpp, compiled with TC_SEPARATE_ENTRY) and needs the entry in its own file.
#include <clap/clap.h>

bool tcEntryInit(const char *path);
void tcEntryDeinit();
const void *tcEntryFactory(const char *id);

static bool entryInit(const char *path) { return tcEntryInit(path); }
static void entryDeinit() { tcEntryDeinit(); }
static const void *entryFactory(const char *id) { return tcEntryFactory(id); }

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};
