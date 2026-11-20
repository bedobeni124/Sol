# Sol

Local speech-to-text dictation for Linux. Background shortcut daemon, floating pill HUD while speaking, CLI for configuration.

Hold your shortcut in any app, speak, release — Sol transcribes locally with `faster-whisper`, formats the text, and pastes it directly at your cursor while preserving your clipboard.

## Quick Start

Prebuilt archives for **Linux x86_64** and **Linux ARM64 (`aarch64`)** are available on the [Releases](https://github.com/bedobeni124/Sol/releases) page:

```sh
tar -xf sol-linux-x86_64.tar.xz   # or sol-linux-aarch64.tar.xz
cd sol-linux-x86_64 && ./install.sh
sol setup
sol
```

Or build from source:

```sh
./install.sh          # Build and install to ~/.local (no sudo needed)
sol setup             # Install Whisper model, choose languages, HUD, and shortcut
sol                   # Start background daemon and show status
```

Arch / Omarchy package:

```sh
bin/install           # Build and install via pkgbuild/PKGBUILD
sol setup
```

Uninstall:

```sh
sol uninstall         # Stop daemon, remove Sol and managed shortcuts (keeps models/data)
sol uninstall --purge # Also delete downloaded models, dictionary, snippets, and history
```

## Capabilities

- **One shortcut, fast paste**: Hold to speak, release to paste. Double-tap to lock recording without holding the key, then tap again to paste. Works across Hyprland (Lua and `.conf`), Sway, Niri, KDE Plasma, GNOME, XFCE, i3, Wayland, XWayland (including Spotify/Electron), and X11.
- **Two minimal HUD designs**: Choose during `sol setup` (or anytime in `sol settings hud glass|mono|off`): **Frosted Glass** (translucent pill with live compositor backdrop blur matching what's behind it) or **Black & White** (pure solid black pill, white waveform & text, no border). Appears only while speaking/transcribing (zero QML/OpenGL overhead).
- **Clean speech formatting & inline casing**: Strips fillers (`um`, `uh`), collapses stutters, handles mid-sentence corrections (`Monday, actually Tuesday`, `scratch that`, `delete last sentence`), inline spoken casing (`arm in uppercase` -> `ARM`, `all caps urgent` -> `URGENT`, `API in lowercase` -> `api`), skips trailing periods in browser URL/search bars and standalone URLs, and formats spoken punctuation, emails, URLs, times, and lists.
- **Click-to-teach pill**: Click the floating pill while speaking or right after pasting to morph it into an inline input bar and type rules like `arm -> ARM`, `slash = /` (matches `slash`, `Slash`, `SLASH` case-insensitively), `Hyprland`, or `-arm` to remove. Press `Enter` to save immediately for all future dictations, or `Esc` to close.
- **Multilingual control**: Pick a single language (`en`), constrain auto-detection to your languages (`en, hu`), or auto-detect all languages during `sol setup` or anytime in `sol settings`.
- **Per-app writing styles**: Adapts tone automatically across personal chat, work chat, email, code/terminals, and custom per-app rules (`formal`, `casual`, `lowercase`, `excited`, `verbatim`).
- **Developer / Code Mode**: Auto-activates in editors and terminals for `camelCase`, `PascalCase`, `snake_case`, `kebab-case`, `CONSTANT_CASE`, filenames, visible symbols, and `@file` tagging.
- **Personal Dictionary & Snippets**: Custom vocabulary (auto-learns single-word edits) and voice snippets with `{{date}}`, `{{time}}`, `{{iso_date}}`, and `{{clipboard}}`.
- **Whisper Mode, audio chimes & media pause**: Soft-limited gain boost for quiet speech, synthesized start/stop chimes, automatic MPRIS2 media pause/resume, clipboard save/restore, and password-field protection.

## CLI Reference

| Command | Description |
|---|---|
| `sol` | Start background daemon and show status |
| `sol setup [cuda\|cpu] [model]` | Install offline Whisper model, choose languages and shortcut |
| `sol settings [KEY [VALUE]]` | Interactive settings menu (or set one option directly) |
| `sol shortcuts [show\|setup\|apply [--save]\|bind\|set]` | View, change, or apply dictation shortcut |
| `sol start` / `sol stop` / `sol toggle` / `sol cancel` | Dictation controls |
| `sol paste-last` / `sol retry` | Re-paste last dictation / re-transcribe last audio |
| `sol dictate` | Interactive voice test in terminal (shows latency in ms) |
| `sol style [show\|auto\|<tone>\|cat\|app]` | Configure per-app writing tones |
| `sol dict [list\|add\|remove]` | Manage personal dictionary |
| `sol snippet [list\|add\|remove]` | Manage voice snippets |
| `sol mic [list\|set <#>\|test]` | Select microphone or run live level test |
| `sol history [list\|copy\|raw\|edit\|reformat\|clear]` | Browse, copy, revert, edit, or clear history |
| `sol stats [--json]` | Streak, words dictated, WPM, and time saved |
| `sol transcribe [--json] [--language CODE] AUDIO` | Transcribe an audio file offline |
| `sol doctor [--json]` | Check GPU, audio, clipboard, HUD, and shortcut |
| `sol logs [-f]` | View daemon log |
| `sol quit` | Stop background daemon |
| `sol uninstall [--purge] [--yes]` | Remove Sol |

## Build & Release

```sh
bin/build             # Compile build/sol
bin/test              # Run C++ and Python test suites
bin/release           # Build stripped release binary, Arch package, source tarball, and SHA256SUMS
```
