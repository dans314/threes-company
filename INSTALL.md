# Installing Three's Company 0.18

A three-strip filter effect by Otis Kanno. Free software (GPL v3): the source code is at the project's GitHub page.

## Windows 10 / 11 (64-bit) — FL Studio and others

1. Unzip `Threes-Company-Windows.zip` (right-click → Extract All).
2. Copy the files:
   - `Threes Company.clap` → `C:\Program Files\Common Files\CLAP\`
   - the `Threes Company.vst3` folder → `C:\Program Files\Common Files\VST3\`
   (Windows asks for permission to copy into Program Files; say yes. Create the CLAP folder if it is not there.)
3. In FL Studio: **Options → Manage plugins → Find installed plugins**. Three's Company shows up as an effect (CLAP and VST3; either works).
4. The window needs the Microsoft Edge **WebView2 Runtime**, which most Windows 10 PCs already have. If the window shows a message instead, install the "Evergreen Bootstrapper" from https://developer.microsoft.com/microsoft-edge/webview2/ and open the plugin again.

## macOS 11 or later (Apple Silicon and Intel)

1. Unzip `Threes-Company-macOS.zip`.
2. Copy the files:
   - `Threes Company.clap` → `~/Library/Audio/Plug-Ins/CLAP/` (Bitwig, Reaper, FL Studio)
   - `Threes Company.vst3` → `~/Library/Audio/Plug-Ins/VST3/` (Bitwig, Ableton Live, FL Studio, Reaper)
   - `Threes Company.component` → `~/Library/Audio/Plug-Ins/Components/` (Ableton Live, Logic, GarageBand)
   (`~/Library` is hidden: in Finder press Cmd+Shift+G and paste the path.)
3. These builds are not notarized by Apple, so macOS may block them. Open Terminal and run, once:

   ```
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/CLAP/"Threes Company.clap" ~/Library/Audio/Plug-Ins/VST3/"Threes Company.vst3" ~/Library/Audio/Plug-Ins/Components/"Threes Company.component"
   ```
4. Rescan plug-ins in your DAW. Bitwig does not load Audio Units; use the CLAP or VST3 there.

## If something goes wrong

Open the plugin, click **⚙ Settings** in the yellow footer, then **Copy details**, and send that text with a description of what happened.
