#!/usr/bin/env python3
"""Builds src/tc_gui_html.h from gui/index.html (the plugin window's page), as raw string pieces."""
import os, sys
here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "..", "gui", "index.html")
dst = os.path.join(here, "..", "src", "tc_gui_html.h")
html = open(src, encoding="utf-8").read()
if ")TCHTML\"" in html:
    sys.exit("the page contains the raw-string end marker")
pieces = [html[i:i + 12000] for i in range(0, len(html), 12000)]
out = ["// Generated from gui/index.html by tools/embed_gui.py. Edit the page, not this file.",
       "#pragma once", "static const char *const tcGuiHtml ="]
out += ['R"TCHTML(' + p + ')TCHTML"' for p in pieces]
out[-1] += ";"
open(dst, "w", encoding="utf-8").write("\n".join(out) + "\n")
print("wrote", os.path.relpath(dst), len(html), "bytes of page")
