# Open Editor

A non-linear video editor written from scratch in C++ with Qt 6 Widgets.

## Build and run

Open `CMakeLists.txt` in Qt Creator with the *Desktop Qt 6.11.1 MinGW 64-bit* kit and run
`appOpen_Editor`, or from a POSIX shell run `./build.sh`.

FFmpeg 8 or newer with the `whisper` filter must be available: `ffmpeg` and `ffprobe` on `PATH`, or
`ffmpeg.exe` and `ffprobe.exe` beside `OpenEditor.exe`.

## Packaging

`tools/package.sh [release folder] [iss file]` rebuilds, refreshes the release folder (Qt runtime,
FFmpeg, plugins, licenses) and compiles the Inno Setup installer. The defaults are
`Downloads/v1.0.0` and `Downloads/OpenEditor.iss`. The installer adds the install folder to PATH,
registers `.oep` and the App Paths key, and the uninstaller removes them again.

On first launch the setup wizard checks FFmpeg, FFprobe, the whisper filter, the speech model and the
MCP bridge. FFmpeg and FFprobe are required: the wizard reopens at startup if they go missing, and can
download a pinned, hash-checked FFmpeg into the app data folder or use a folder you pick. Run it any
time from Help > Setup Wizard. `OpenEditor --setup-test <outdir>` tests it, and must run with FFmpeg
neither beside the executable nor on PATH.

## Titles and fonts

- **Import fonts** (.ttf, .otf, .ttc, .otc, .woff, .woff2) from File > Import Font, the Graphics
  panel, or by importing the file like media. Imported fonts are copied to the app data `Fonts`
  folder and load at every start. Opening a project whose titles use fonts this computer lacks
  names them, and the Graphics panel marks a substituted font.
- **Graphics panel**: weight (Thin 100 to Black 900), italic, underline, strikethrough, ALL CAPS,
  Small Caps, lowercase, Title Case, tracking, kerning, skew, left/center/right/justify, top/middle/
  bottom, leading, wrap width, solid or linear/radial gradient fill, stroke inside/center/outside,
  shadow with angle, glow, background with padding, rounded corners and per-line boxes.
- **Animation**: in and out, each Fade, Typewriter, Slide Up, Slide Down, Pop or Wipe, with a length.
- **Style presets**: ten built-in looks, and your own saved from the current title.
- **AI / MCP tools**: import_font, list_fonts, set_text_style, list_text_presets, apply_text_preset;
  add_title takes preset and style.
- Titles from older projects render exactly as before: `oe_text_test` compares them with reference
  renders in `tests/data/text_ref`, and measures each option.

## New in 2.0.0

- **Audio effects**: Enhance Speech (noise reduction, EQ, de-essing, compression and levelling in
  one control), DeNoise, DeEsser, DeHum, Noise Gate, Chorus/Flanger. They run in playback and export.
  Enhance Speech is signal processing, not a neural model: it works offline and on any voice.
  DeNoise and Enhance Speech delay their clip by 512 samples (about 11 ms).
- **AI menu**: Enhance Speech on Selected Clips, Auto Ducking (volume keyframes on music under
  dialogue), Match Loudness (EBU R128 / BS.1770 LUFS), Remove Filler Words (um, uh, hmm, 음, 어),
  Warp Stabilizer on Selected Clip (position only; rotation and rolling shutter are not corrected).
- **Export**: a loudness target (-14, -16, -23 or -24 LUFS); peaks stay under -1 dBFS.
- **Video effects**: Warp Stabilizer, Leave Color, Color Key, Find Edges, Emboss, Twirl, Spherize,
  Replicate, Linear Wipe, Gradient Ramp, Strobe Light.
- **MCP / assistant tools**: measure_loudness, match_loudness, enhance_speech, auto_duck,
  remove_filler_words, stabilize_clip, and loudness_lufs on export_sequence.
- The setup wizard reopens once after an upgrade and also checks the H.264 and AAC encoders.
- The window, executable and installer share the app icon (`src/app.ico`, made with `--write-icon`).

Tests: `oe_v2_test` measures each new effect against what it should do, and
`OpenEditor --v2-test <outdir>` drives every new tool in the real window and checks the export.

## Transcription and text-based editing

The **Text** workspace transcribes the sequence on this computer with Whisper. The default model is
Large v3 Turbo; models download once, after you confirm, into the app data folder.

The mix goes to Whisper in one pass with word timestamps. Do not slice it with a short queue or VAD:
that makes Whisper re-detect the language for each slice and costs accuracy and speed. Whisper word
times are only a guide (about 300 ms off), so `placeWords` assigns words to measured speech regions by
dynamic programming and settles boundaries on energy dips. Measure changes with `oe_align_eval`, which
scores word starts against ground truth; do not judge timing by eye on one clip.

In the Transcript tab, click a word to jump there, or select words and press Delete to cut that
footage from every track. Captions, markers and links follow, and Undo restores it all.

## AI

**MCP server** (AI > MCP Server, off by default): Streamable HTTP on `127.0.0.1`, bearer token
required, cross-origin requests refused. It exposes the editor as 45 tools. `oe-mcp.exe` is a stdio
bridge for clients that launch a local command.

**AI Assistant** panel: chat that drives the same tools through the Claude Messages API, using your
own API key, which is encrypted for your Windows user. Project details, transcripts and any frames it
inspects are sent to the API. All AI edits go through normal undo.

Also in the AI menu: remove silence, scene edit detection, audio normalization.

## Plugins

Native effect plugins go in `oe-plugins` beside the executable or in the app data `plugins` folder.
See `src/plugin_api/oe_plugin.h` and `examples/plugin_duotone`. Plugins run inside the editor process,
so install only ones you trust.

## Tests

- `oe_engine_test`: frame accuracy, edit operations, compositing, mixing, every effect, transition and export format.
- `oe_features_test`: GPU against CPU output, HDR, proxies, time remap, multicam, captions, automation, plugins, analysis.
- `OpenEditor --ui-smoke <outdir> <media...>`: real mouse events through every tool, undo, save/load, docking, playback.
- `OpenEditor --text-test <outdir> <model> <media> <expected phrases...>`: transcribe, then delete words with the real key.
- `OpenEditor --ai-test <outdir>`: MCP over real HTTP, token and origin checks, stdio bridge, assistant loop against a local mock.

## Layout

| Path | Contents |
| --- | --- |
| `src/core` | Value-type project model, undo, edit operations, media probing and caches |
| `src/engine` | ffmpeg pipes, frame reader, compositor, CPU and GPU effects, mixer, playback, export, analysis, plugin host |
| `src/ui` | Docking panels, timeline, monitors, grading, audio, transcript, AI tools, MCP server, assistant |
| `src/ui/icons` | Tabler Icons (MIT, see `LICENSE-tabler.txt`) plus small UI glyphs |
| `tools` | `oe-mcp` stdio bridge |

Project files are JSON with the `.oep` extension.
