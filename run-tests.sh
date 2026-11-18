#!/bin/sh
# Builds and runs the Three's Company tests: the engine, then the plugin loaded the way a
# DAW loads it, then renders the demo.
# Run from inside this folder:   sh run-tests.sh
# Needs a C++ compiler (on a Mac: the command line developer tools).
set -e
cd "$(dirname "$0")"
OUT="${TC_BUILD_DIR:-build}"
mkdir -p "$OUT"
CXX="${CXX:-c++}"
INC="-Isrc -Ilibs/sst-filters-include -Ilibs/sst-basic-blocks-include -Ilibs/simde"
"$CXX" -std=c++20 -O2 -o "$OUT/tc_tests" tests/tc_tests.cpp $INC
"$OUT/tc_tests"
if [ -f libs/clap-main/include/clap/clap.h ]; then
  if [ "$(uname)" = "Darwin" ]; then SHARED="-bundle"; LIBS=""; else SHARED="-shared -fPIC"; LIBS="-ldl"; fi
  "$CXX" -std=c++20 -O2 -fvisibility=hidden $SHARED -o "$OUT/threes-company-test.clap" src/tc_clap.cpp $INC -Ilibs/clap-main/include
  "$CXX" -std=c++20 -O2 -o "$OUT/clap_host_test" tests/clap_host_test.cpp -Ilibs/clap-main/include $LIBS
  "$OUT/clap_host_test" "$OUT/threes-company-test.clap"
else
  echo "(plugin test skipped: libs/clap-main is missing)"
fi
"$OUT/tc_tests" demo "$OUT/threes-company-phase1-demo.wav"
