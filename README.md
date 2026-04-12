# buckle-gui

A Windows GUI launcher for [Bucklespring](https://github.com/zevv/bucklespring) — the nostalgic IBM Model M keyboard sound simulator.

![Windows 7](https://img.shields.io/badge/Windows-7%2B-blue) ![C](https://img.shields.io/badge/language-C-lightgrey) ![License](https://img.shields.io/badge/license-MIT-green)

---

## What it does

Bucklespring is a command-line tool. This GUI wraps it so you can:

- Control gain and stereo width with sliders
- Browse for the audio folder
- Toggle options (muted start, no click sounds, fallback sounds)
- See buckle's output in a real-time log
- Enable or disable Windows autostart with one click
- Minimize to the system tray

---

## Screenshot

![Buckle-GUI Windows 7](img/preview.PNG)

---

## Requirements

Place all of these in the same folder:

```
buckle-gui.exe
buckle.exe
ALURE32.dll
libopenal-1.dll
wav/
```

---

## Building from source

You need [MSYS2](https://www.msys2.org/) with the MinGW-w64 toolchain.

```bash
# Install dependencies (first time only)
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-binutils

# Clone and build
git clone https://github.com/zinzk714-art/bucklespring-gui
cd bucklespring-gui

windres -O coff src/buckle-gui.rc -o src/buckle-gui.res
gcc -O2 -mwindows -o bin/buckle-gui.exe src/buckle-gui.c src/buckle-gui.res \
    -luser32 -lgdi32 -lshell32 -lcomctl32 -lole32
```

No additional libraries required. The GUI depends only on Windows system DLLs.

---

## Usage

1. Run `buckle-gui.exe`
2. Set the **Audio Path** to the folder containing the `.wav` files
3. Click **Start**

Settings are saved automatically to `buckle-gui.ini` in the same directory.

### Options

| Option | Description |
|--------|-------------|
| Gain | Playback volume (0–100) |
| Stereo Width | How wide the stereo field is (0 = mono, 100 = full) |
| Audio Path | Path to the folder with `.wav` sound files |
| Device | OpenAL audio device (leave blank for system default) |
| Start muted | Launch buckle in muted state |
| No mouse click sounds | Disable sounds for mouse button clicks |
| Fallback sound | Play a generic sound for unrecognized keys |
| Start buckle when GUI opens | Auto-launch buckle every time the GUI starts |
| Mute key | Hex scancode to toggle mute (default `0x46` = Scroll Lock) |

### Mute toggle

Press the mute key twice within 2 seconds to toggle mute. The default key is **Scroll Lock**. You can change it in the Options section.

### System tray

Closing the window hides it to the system tray. Right-click the tray icon for options. Double-click to restore.

---

## Project structure

```
bucklespring/
├── src/
│   ├── buckle-gui.c        — full source, single file
│   ├── buckle-gui.rc       — icon, manifest and version info
│   └── buckle-gui.manifest — DPI aware + Common Controls v6
├── bin/
│   └── buckle-gui.exe      — precompiled binary
├── wav/                    — audio files (from original bucklespring)
└── README.md
```

---

## Credits

- [Bucklespring](https://github.com/zevv/bucklespring) by zevv — the original tool this GUI wraps
- GUI by [zinzk714-art](https://github.com/zinzk714-art)
