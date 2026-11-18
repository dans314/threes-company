/*
 * Three's Company - the plugin window.
 *
 * The window is a web view showing gui/index.html (built into the plugin as tc_gui_html.h).
 * The page and the plugin talk through a Bridge:
 *   page -> plugin: short text messages ("ready", "set <id> <value>", "begin <id>", "end <id>",
 *                   "reset <id>", "curve <lfo> <n> <x> <y> <t> ...")
 *   plugin -> page: JavaScript to run, about 30 times a second ("tc.init({...})", "tc.upd({...})")
 * Only the web view itself is platform code: tc_gui_mac.mm (WKWebView) on macOS, tc_gui_win.cpp
 * (WebView2) on Windows. Elsewhere there is no window, and the host shows its own controls.
 */
#ifndef THREES_COMPANY_GUI_H
#define THREES_COMPANY_GUI_H

#include <string>

namespace tcgui
{
struct Bridge
{
    virtual ~Bridge() = default;
    virtual void onMessage(const char *msg) = 0; // from the page (main thread)
    virtual std::string poll() = 0;              // JavaScript for the page, or "" (main thread)
    virtual const char *html() = 0;              // the page
};

#if defined(__APPLE__) || defined(_WIN32)
void *create(Bridge *b, int w, int h);
bool setParent(void *gui, void *parentNSView);
void setSize(void *gui, int w, int h);
void show(void *gui, bool on);
void destroy(void *gui);
std::string chooseFolder(void *gui, const std::string &start); // the system's folder picker; "" if cancelled
void reveal(const std::string &path);                          // opens a folder in Finder / Explorer
inline bool available() { return true; }
#else
inline void *create(Bridge *, int, int) { return nullptr; }
inline bool setParent(void *, void *) { return false; }
inline void setSize(void *, int, int) {}
inline void show(void *, bool) {}
inline void destroy(void *) {}
inline std::string chooseFolder(void *, const std::string &) { return std::string(); }
inline void reveal(const std::string &) {}
inline bool available() { return false; }
#endif
} // namespace tcgui

#endif
