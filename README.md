# Three's Company

A three-strip filter effect by Otis Kanno, for any DAW: **CLAP, VST3 and AU on Mac, CLAP and VST3 on Windows**.

Three strips, Low, Mid and High, each with its own filter and drive, and lots of ways to make them move together. Mid leads; Low and High follow, answer back or drift on their own.

**Version 0.18** · [Quick reference (PDF, one page)](docs/Threes-Company-Quick-Reference.pdf) · [Install](INSTALL.md)

## What it does

- **Three strips.** Each has a filter (classic analog-style models with their variants, or a Z-Plane morphing filter), a drive (Warm, Push, Bite, Fuzz, Fold, Rectify or Crush) before or after the filter, Cutoff, Reso, Morph, Pan, Level, Mix, its own Mod Depth, mute and solo.
- **Three routings.** **Series** runs Low → Mid → High on the whole sound. **Parallel** splits it into three bands with a 24 dB/oct crossover, one strip each. **Spectral** splits with an FFT, either by frequency (steep, no phase shift) or by character: **Tonal** sends the steady tones, the hits and the air to different strips. Series and Parallel each keep their own complete settings.
- **Movers**, each on its own page:

| Page | What it does |
| --- | --- |
| Sway | Slow drift from LFOs whose rates never line up, so it never repeats |
| Chaperone | Mid leads and the Chaperone passes its moves on to Low and High, always in time. Moods set it in one click; 3rd Wheel pushes back |
| Spectral | The FFT split: Balance, Sharpness and FFT Quality (Spectral routing only) |
| Bank | Eight narrow bands per strip, with Ripple running waves of boosts and cuts through them |
| Tune | Narrow resonators on the notes of a key, so the pitched part of the sound rings out. Root, scales and intervals, Layer or Blend, ring time, Tilt, Slope (a tilt EQ on the ringing) and Motion, free or synced |
| Mutation | When something happens in Mid, Low and High answer, the same way or the opposite way. Mutate slowly rewrites those answers over the song |
| LFOs | Three LFOs: shapes, drawn curves, steps and arpeggios |
| Sequencer, P-Locks, Timing | Three 32-step lanes with Euclid rhythms and parameter locks |
| Mod | A 16-slot matrix: any source to any target |
| Follower | An envelope follower on the input, a sidechain or one band |
| Mix | Feedback, noise, gain, split points and Tools (No dead bands, Copy between routings) |

## Using it

- **Double-click** a knob to reset it; **Shift-drag** for fine control.
- **Right-click** a knob to see everything moving it right now (click a line to jump to its page), add or remove modulation, reset, or copy and paste its value.
- **↶ ↷** (or ⌘Z / ⇧⌘Z, Ctrl+Z / Ctrl+Y) undo and redo changes made in the window.
- The **dice** randomize just their own card or strip, and **⧉** copies and pastes a panel.
- **Hover** anything for a tip. Tips can be turned off in **⚙ Settings**.

## Presets

The menu starts with **Start**: Init and 16 presets that each show off a feature. **Yours** holds the presets you save with **Save**. Then come categories named for what the presets do: Dirt & Crush, Pads & Textures, Phasers & Combs, Rhythm & Gates, Spectral & Tonal, Subtle Tweaks and Sweeps. Your presets are small `.tcpreset` files, and **⚙ Settings → Your presets folder** lets you keep them anywhere, such as Dropbox or iCloud.

## Install

Download the zip for your computer from **[Releases](../../releases/latest)**: `Threes-Company-Windows.zip` or `Threes-Company-macOS.zip`. Each has a READ ME FIRST install guide, the [user manual](docs/Threes-Company-User-Manual.pdf) and the quick reference inside. In short:

- **Mac:** `Threes Company.clap` → `~/Library/Audio/Plug-Ins/CLAP/`, `Threes Company.vst3` → `~/Library/Audio/Plug-Ins/VST3/`, `Threes Company.component` → `~/Library/Audio/Plug-Ins/Components/`.
- **Windows 10 / 11:** `Threes Company.clap` → `C:\Program Files\Common Files\CLAP\`, the `Threes Company.vst3` folder → `C:\Program Files\Common Files\VST3\`.

Rescan plug-ins in your DAW. Bitwig: CLAP or VST3. Ableton Live: VST3 or AU. FL Studio: CLAP or VST3. [INSTALL.md](INSTALL.md) has the full steps, including the one-time macOS unblock command.

## Something wrong?

Open **⚙ Settings → Copy details** in the plugin and send that text with a description of what happened. The quick reference has a short troubleshooting list.

## Building from source

On a Mac with Xcode's command line tools and CMake (`brew install cmake`):

```
sh build-mac.sh        # the CLAP, plus the tests
sh build-all-mac.sh    # CLAP, VST3 and AU, then Apple's AU check
```

`build-mac.sh` should end with `RESULT: PASS`, and `build-all-mac.sh` with `AU VALIDATION SUCCEEDED`. If you change the window (`gui/index.html`), run `python3 tools/embed_gui.py` before building. Every push to GitHub also builds Windows and Mac with GitHub Actions (`.github/workflows/build.yml`).

| Path | What it is |
| --- | --- |
| `src/tc_engine.h` | The sound engine |
| `src/tc_clap.cpp` | The plugin: parameters, presets, state and the window bridge |
| `gui/index.html` | The plugin window |
| `src/tc_gui_mac.mm`, `src/tc_gui_win.cpp` | The window on Mac (WebKit) and Windows (WebView2) |
| `tests/` | The engine and plugin tests |

## Licence

GPL v3. The filters are the Surge Synth Team's [sst-filters](https://github.com/surge-synthesizer/sst-filters), and the filter core started from [Two Filters](https://github.com/baconpaul/two-filters) by Paul Walker (BaconPaul), both GPL v3. Anyone who gets the plugin must be able to get its source.
