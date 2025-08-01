<div align="center">

![EvernightCapture logo](resources/icon.ico)

# EvernightCapture

A command-line window screenshot tool: select windows by conditions, then save that window's pixels to an image file.

[English](README.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [日本語](README.ja-JP.md)

</div>

Built on a full set of Windows.Graphics.Capture channels. The entry point is `ECAPTURE.EXE` — one executable,
statically linked CRT, no VC++ runtime on the target machine. The output is program-friendly: everything except
`--help` and `--version` is JSON, exit codes are stable, and every diagnostic carries a stable `code`, so the tool
works just as well typed by hand as called from a script or an AI agent.

Current version **0.4.0**: every `--capture` value is implemented (`wgc` / `dwm` / `printwindow` / `bitblt` /
`duplication` / `auto`), and `--monitor` gives whole-screen capture plus "filter windows by monitor". The
previously implemented `magnification` channel was removed (reasons in AGENTS.md).

## Features

- **Select windows by condition**: handle / process id / image name / full path / title (exact, contains, regex) /
  window class — different options AND together, repeating one option ORs it
- **Six capture channels**: capture a window that is covered by something else (`wgc` / `dwm` / `printwindow`), or
  deliberately copy only the pixels visible on screen (`bitblt` / `duplication`)
- **Many windows at once**: `--all` saves one image per matched window, named with placeholders like `%i`
- **Cropping inside the window**: `--roi x,y,w,h` keeps one rectangle of the delivered whole-window image, and
  `--client-area` keeps only the client area. Those coordinates belong to **that image's own pixels** (top-left
  corner = `(0,0)`, physical pixels, never scaled by DPI) — they are never re-read as desktop-absolute
  coordinates — and a rectangle that does not fit is rejected instead of being slid inside, cropped to the
  edge, or swapped for the uncropped window. The crop runs *after* the frame is captured, so it changes nothing
  about authorization: desktop-pixel paths still always ask, and `--yes` does not start applying just because
  only a small piece is kept
- **Screenshot authorization**: any capture that really grabs a frame asks in a modal dialog first, reliable
  window paths included; `--yes` skips that ask **only** for paths whose frame is bound to the selected window
  itself — anything sampling desktop pixels always needs a person and no switch can skip it
- **Target identity that is re-checked**: the handle, owning process, that process's creation time, window class and the
  original selection conditions are recorded when a target is picked, then verified again before every capture attempt and
  once more right after the consent answer. A destroyed window, a handle reused by another process, or one that no longer
  satisfies the condition it was selected by gives `capture.target_gone` / `capture.target_changed` /
  `capture.target_unverifiable` instead of a picture nobody approved
- **Deadlines that hold**: `--timeout-ms` is one budget for the whole automatic stage (matching, backend retries,
  frame waits, encoding, writing) and `--consent-timeout-ms` times the human dialog separately; calls that wait on
  another process (`PrintWindow`, the DWM read-back, regex evaluation) run in a helper process the tool can stop,
  so a wedged target window can no longer wedge this tool
- **Four message languages**: `zh-CN` / `zh-TW` / `en` / `ja`, defaulting to the system display language, all
  embedded as resources inside the exe
- **Machine-readable JSON**: capture results and errors only — no tool name, version, schema or argument echo
- **Windows you can list and inspect without capturing**: `--list` returns the matched windows as structured JSON
  (handle, PID, class, title, image name, physical rectangle, visibility / minimized, z-order, and the identity
  fields a later capture re-checks) with paging instead of an ambiguity error; `--inspect` describes one window and
  reports several matches as an ambiguity instead of picking one. Neither takes a pixel, shows a dialog, writes a
  file or touches a window, `--yes` changes nothing there, and the answer is an explicitly-labelled snapshot
- **Capabilities you can ask about read-only**: `--capabilities` / `--diagnostics` report which routes this machine
  can take, how far `--yes` really reaches, and a checkable build id — without taking a pixel, showing a dialog,
  writing a file or talking to the network. "compiled into this build", "usable here right now" and "actually tested
  by this project" stay three separate fields, an unanswered question is reported as `unknown`, and capability is
  never probed by capturing or encoding something

## Quick start

You need Visual Studio ("Desktop development with C++" workload) plus the Windows SDK; the build script locates
both automatically.

```powershell
.\build.ps1                                  # Release, output: build\ecapture.exe
ECAPTURE.EXE --process notepad.exe D:\shots\epad.png   # a dialog asks before the frame is taken
```

The output directory must **already exist** — the tool never creates one. To see what would be matched first (no
capture, no file written, and no consent dialog):

```powershell
ECAPTURE.EXE --process notepad.exe --dry-run --out D:\shots\_probe.png
```

Recipes people actually use:

```powershell
# Pin one window by title plus window class
ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png

# Name a window explicitly, using a handle taken from the dry-run candidate list
ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite D:\shots\one.png

# One image per matched window, numbered file names
ECAPTURE.EXE --pid 12345 --title-contains Report --all "D:\shots\rpt_%i.png"

# Image bytes on stdout (JSON then moves to stderr)
ECAPTURE.EXE --process notepad.exe --out - 1> D:\shots\snap.png 2> D:\shots\result.json

# Window capture without being asked: --yes only covers a path that reads the window itself
ECAPTURE.EXE --process notepad.exe --yes D:\shots\epad.png

# Whole screen: that path reads desktop pixels, so a person has to answer -- --yes cannot skip it
ECAPTURE.EXE --monitor primary --out D:\shots\screen.png
ECAPTURE.EXE --monitor all --out "D:\shots\screen_%i.png"
```

## Options

`--opt=value`, `-opt` and `/opt` are all accepted; when a value itself starts with `-` write `--title=-x` or end
option parsing with `--`. Short options **cannot** be combined (`-qi` fails with `cli.unknown_option`). The block
below is the verbatim output of `ECAPTURE.EXE --help`; run `.\scripts\mkreadme.ps1` after changing options —
**do not hand-edit that block**.

<!-- BEGIN ECAPTURE-HELP -->
```text
EvernightCapture (ECAPTURE.EXE) - capture a window selected by conditions, built on Windows.Graphics.Capture

Usage: ECAPTURE.EXE [conditions...] <output-path>     With no conditions at all => this help
       ECAPTURE.EXE [conditions...] --out <path>      "-" means image bytes go to stdout
       ECAPTURE.EXE [conditions...]                    No output path => png bytes to stdout
       ECAPTURE.EXE --monitor [n] <path>         --monitor with no window conditions => that whole screen

Capture target (without --monitor only the window conditions below are used)
  --monitor, -m [<n|primary|all|device:|id:>] Monitor number, 1-based and decimal (the order of this run's enumeration, not guaranteed to match the id in Display settings; use device in the result to identify a monitor); primary = main one, all = one image per monitor. Without window conditions = capture that whole screen; with them = only windows overlapping it. The value may be omitted (= primary), and then the next argument is not swallowed, so --monitor out.png still works. A whole screen is desktop pixels, so a person must confirm it and --yes cannot skip that; filtering windows by monitor still yields window images; to name a specific monitor use an identifier from --screens: device:<name> (this desktop attach) or id:<monitor device path> (the cross-session one) - a number can point at a different screen after a replug or a mode change

Window match conditions (repeat one option for OR, combine different options with AND)
  --hwnd <handle>                             Window handle. Plain digits are decimal; a 0x prefix or a-f digits are hexadecimal - prefer 0x. No sign and no whitespace; an underscore may only sit between two hexadecimal digits
  --pid <pid>                                 Process id, decimal only and greater than 0
  --process, -p <image-name>                  Image file name (no path), case-insensitive; without an extension .exe is assumed
  --exe <full-path>                           Full image path, case-insensitive
  --title, -t <exact-title>                   Window title, exact match
  --title-contains, -T <text>                 Window title contains this substring
  --title-regex, -R <regex>                   Window title regular-expression match (ECMAScript), validated while parsing
  --class, -c <class-name>                    Window class name, case-insensitive, e.g. Notepad / CabinetWClass

When several windows match (mutually exclusive)
  --index, -i <n>                             Take the n-th window, 1-based decimal, ordered by visibility and z-order
  --topmost-match                             Take the topmost matched window in the current z-order
  --bottommost-match                          Take the bottommost matched window in the current z-order
  --newest                                    Deprecated alias of --topmost-match: it picks by z-order, not by creation time
  --oldest                                    Deprecated alias of --bottommost-match: it picks by z-order, not by creation time
  --all, -a                                   Save one image per matched window

Capture channel (default wgc; may fail because of the OS version or the window itself)
  --capture, -C <method>                      wgc (default, works through occlusion) / dwm (DWM thumbnail, works through occlusion) / printwindow (window paints itself) / bitblt (copies visible screen pixels) / duplication (desktop duplication cropped to the rect; corrected for the monitor's rotation, and it only takes the one output overlapping the target most - partial captures come back with capturedRect/clipped) / auto (falls back wgc-dwm-printwindow-bitblt; a whole screen only uses wgc-duplication-bitblt)
  --cursor <default|include|exclude>          Whether the mouse pointer belongs in the image: default (nothing is changed and the three cursor keys stay out of the result) / include / exclude. Only wgc has a cursor switch that can really be set and read back (needs build 19041+); the other paths never contain a pointer, so include with printwindow / dwm / bitblt / duplication is refused as capture.cursor_unsupported and the tool never falls back to a screen-pixel channel; with auto those are dropped from the chain, each leaving a note.cursor_channel_skipped. This does not change consent; the requested / effective / basis rules are in the README
  --hdr <auto|tonemap|refuse>                 How to treat an HDR source: auto (default, this tool changes nothing and the color keys do not appear) / tonemap (map an HDR frame to 8-bit SDR with a fixed tone curve before delivering) / refuse (if the source is confirmed HDR, error out and never deliver a BGRA8-washed image). Only wgc and duplication can carry a wide-gamut frame, so tonemap/refuse with printwindow / dwm / bitblt report capture.hdr_unsupported at parse time and never reroute to a channel that reads desktop pixels. This does not change authorization; the source color space, bit depth and the processing actually applied are written into the result (see README and the color section of --capabilities)

Cropping inside the window (another crop of the delivered whole-window image, in that image's own pixel coordinates - not desktop coordinates; the two below are mutually exclusive)
  --roi <x,y,w,h>                             Cut a w x h block starting at x,y out of the delivered whole-window image. The origin (0,0) is this image's own top-left pixel (the image is the visible window frame you actually see; the transparent DWM resize border is not in it), in physical pixels and not scaled by DPI (the process is per-monitor v2, so multiply the scale yourself for logical pixels) - which is why these four numbers are never read as desktop-absolute coordinates. Four decimal integers separated by commas; x and y may be 0, w and h are at least 1, none above 16384. If it does not fit, nothing is written: match.roi_out_of_range when that is already clear before the frame is taken (no dialog, no file) and capture.roi_invalid when it only turns out afterwards - the rectangle is never slid inside, never cropped to the edge, and the uncropped window is never handed over instead. The crop runs after the capture, so it changes nothing about authorization: channels that sample the screen still always ask, and --yes does not start applying because only a small piece is kept. In the result cropRect is in image pixels, cropScreenRect is the same rectangle in screen coordinates (written only when the image's screen origin can be established), fullWidth/fullHeight are the size before cropping and width/height after. Mutually exclusive with --client-area, and meaningless for a whole-screen target (capture.unsupported)
  --client-area                               Keep only the window's client area: also drop the title bar and the three borders from the delivered whole-window image. That rectangle is measured from the target's geometry right now (GetClientRect plus ClientToScreen), so the coordinate system and units are exactly the --roi ones. A client area that cannot be measured reports capture.roi_unmeasurable, one that hangs outside the delivered image (off screen, or the window resized in between) reports capture.roi_invalid; neither falls back to the whole window. Mutually exclusive with --roi

Capture authorization (a real capture asks first; --yes skips window-content paths)
  --yes, -y                                   Skip the confirmation for window-content paths (wgc / printwindow / the dwm thumbnail route). Anything reading the screen (bitblt, duplication, a whole screen, dwm screen fallback) always asks; --yes cannot skip it. --yes=false asks on purpose

Deadlines (a total budget for the automatic stage; waiting for consent is timed separately)
  --timeout-ms <ms>                           Total budget in milliseconds for the automatic stage: from target selection on, matching, backend retries, frame capture, encoding and writing share this one remaining budget and no step gets a fresh copy. Omitted or 0 = no overall budget, and every isolated call is then still bounded by the built-in 5000 ms limit. Waiting for your consent is not counted here - see --consent-timeout-ms. When the budget runs out the image is not written; you get match.timeout / capture.timeout / io.timeout per stage
  --consent-timeout-ms <ms>                   How long the consent dialog may wait for an answer, in milliseconds. Omitted or 0 = wait forever. On expiry the capture is refused - never treated as consent - and reported as capture.consent_timeout. This wait is timed separately and does not consume the --timeout-ms budget; the ~1s dialog close animation after "Yes" is counted here and is never skipped to meet a deadline

Output
  --out, -o <path|->                          Output path; the special value - writes the image bytes to stdout. A positional argument works too, and giving no path at all is the same as --out -. Every name for the batch is planned before any frame is taken: two targets resolving to the same name is an error, never a silent overwrite. stdout carries only one image per run, so a batch that resolves to more than one target is a parameter error and nothing is captured
  --format, -f <name>                         Force the encoding format; otherwise it comes from the output file extension, and png when that fails too
  --quality <1-100>                           JPEG quality, decimal 1-100, default 100
  --no-overwrite                              Fail instead of overwriting an existing target (no value means the prohibition is on). --no-overwrite=false (0 / no / n / off) cancels it; =true / 1 / yes / y / on means the same as giving no value. When repeated, the last one wins

Capability queries (read-only: no capture, no dialog, no file)
  --capabilities                              Print this machine's capability report as JSON: version, OS and session conditions, each backend as available / unavailable / unverified, formats, and what --yes actually covers. Read-only - no capture, no dialog, no file, and it never probes by taking a screenshot. "available" only means this build has the route and the environment checks did not reject it; it is not a guarantee for a given window. Accepts only --lang, -v and -q; with any capture option or an output path it is cli.query_conflict (exit 1)
  --diagnostics                               Print the build report as JSON: version, a checkable build id (PE link timestamp, architecture, image size), platform and backend status - the same fields as --capabilities, not a second copy of them. It uploads nothing, captures no pixels, enumerates no user files and prints no usernames, environment variables or paths. --verbose adds the raw answer of every question. Same conflict rule
  --screens                                   Read-only list of every screen: tool number, device name, primary flag, physical rect, DPI and rotation where they can be read, and which adapter drives it - plus how far each of those identities stays true. It reads no pixel, shows no dialog, writes no file and changes no display setting. The device:<name> and id:<path> it returns go straight into --monitor. Mutually exclusive with capture options (cli.query_conflict + exit code 1); a real full-screen capture still always asks and --yes cannot skip it
  --list [<all>]                              List every top-level window satisfying all conditions as structured JSON (handle / PID / class / title / image name / rect / Z-order / identity constraint fields). No capture, no dialog, no file, no output path needed; several matches are paged with --offset / --limit rather than reported as a capture ambiguity, while the pick options are a conflict. Value all also lists minimized windows. The list expires - a later capture re-checks the identity (see the README)
  --inspect [<path>]                          Read-only check of the one window the same pick policy selects. Several matches are reported as match.ambiguous_window + exit 5 instead of picking one for you. Value path also writes the full image path of the owning process (default: image name only). Unreadable fields say so (denied / failed plus the system error code); no elevation is suggested and no window is restored or activated
  --offset <n>                                Skip the first n windows (decimal, from 0)
  --limit <n>                                 At most n windows per batch (default 50)

Miscellaneous
  --dry-run, -d                               Parse and list candidate windows only - no capture, no file written
  --json, -j                                  Deprecated compatibility switch, no effect: success and errors are already JSON
  --verbose, -v                               Add the input section to the JSON (all input, normalized) and keep notes
  --quiet, -q                                 Drop notes; errors are always returned whatever this says. --verbose wins when both are given
  --lang, -l <language>                       Message language. auto (default, follows the system display language) / zh-CN / zh-TW / en / ja; unsupported system languages fall back to en
  --help, -h                                  Print this text help
  --version                                   Print version and stage

Syntax: --opt=value / -opt / /opt all work; when a value itself starts with - write --title=-x, or end option parsing with --. Numbers are decimal only (--hwnd also takes 0x hexadecimal)
Output: success and failure are both JSON, holding only captured / images (plus errors / notes, and input only with --verbose)
       --help / --version and the no-conditions case are plain text
Exit codes: 0 success / 1 bad arguments / 2 no condition given / 3 --help / 4 no matching window / 5 several matches /
        6 target protected or refused / 7 capture failed / 8 write failed / 9 internal error
Current build: every --capture value is implemented (wgc / dwm / printwindow / bitblt / duplication, auto falls back wgc-dwm-printwindow-bitblt; a whole screen uses wgc-duplication-bitblt); the output directory must already exist
Runtime: 64-bit Windows, declared floor build 18362 (Windows 10 version 1903), tested only on build 19045; a route this machine's version cannot offer is reported before any frame or dialog as env.os_too_old / env.channel_unsupported (the first does not improve with another channel). --verbose echoes input.osBuild and input.captureChain

Examples:
  ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
  ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png
  ECAPTURE.EXE --pid 12345 --title-contains Report --all D:\shots\rpt_%i.png
  ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite out.png
  ECAPTURE.EXE --process notepad.exe --out - > snap.png
  ECAPTURE.EXE --monitor all D:\shots\screen_%i.png
  ECAPTURE.EXE --process notepad.exe --yes D:\shots\epad.png
  ECAPTURE.EXE --process notepad.exe --yes --timeout-ms 5000 --consent-timeout-ms 60000 D:\shots\epad.png
  ECAPTURE.EXE --capabilities  ask what this machine can do first, then choose --capture and the target
```
<!-- END ECAPTURE-HELP -->

## Argument syntax

Each numeric option takes only the spelling it documents; the parser no longer guesses a base.

- `--pid`, `--index`, `--monitor <n>`, `--quality` and the two timeout options take **decimal digits
  only** (`[0-9]+`): no sign, no whitespace, no dot, no exponent (`1e3`), no digit separators, no `0x`,
  no non-ASCII digits. The range is checked in the same call (`--pid` 1..4294967295, `--index` and
  `--monitor` 1..65535, `--quality` 1..100, timeouts 0..86400000). Anything else is
  `cli.invalid_number` + exit code 1 - a value is never cast, wrapped or re-read in another base, so
  `--pid 1e3` cannot silently become 483 and `--hwnd -1` cannot become `UINT64_MAX`.
- `--hwnd` keeps its three documented spellings: plain digits are decimal, a `0x` / `0X` prefix is
  hexadecimal, and a bare spelling containing `a-f` is hexadecimal (the Spy++ form, so `--hwnd 1e3`
  is `0x1e3` by design). Sign, whitespace, overflow past 64 bits and handle `0` are rejected. An
  underscore is legal only inside the hexadecimal spellings and only between two hexadecimal digits:
  `0x001A_0B4C` yes, `0x_1A`, `1A__0B4C`, `1A0B4C_` and `12_34` no.
- The value of `--monitor` may be omitted, so "is the next argument mine" uses exactly the grammar
  above: `--monitor out.png` still means "primary monitor, write out.png", while `--monitor 1e3` is a
  malformed monitor number and is reported instead of quietly becoming an output file name.
- The token right after an option that takes a value is that value, even when it looks like another
  option: `--title --lang ja` looks for the title `--lang`. For a value that starts with `-`, write
  `--title=-x`, or use `--` to stop option parsing (everything after it is positional; the `--`
  itself is dropped).
- Repeating an option: match conditions OR (`--title A --title B`), value options take the last one
  (`--timeout-ms 9000 --timeout-ms 300` is 300), and `--lang` behaves the same way - `auto` (or an
  omitted value) resets to the system display language rather than keeping the previous choice. An
  invalid `--lang` reports `cli.unknown_language`, written in the language already settled on.
- `--verbose` and `--quiet` together are handled as `--verbose`: notes are still delivered, plus one
  `note.flag_overrides_quiet` saying why. `errors`, and the provenance fields `images[].source` /
  `path` / `scope`, are never hidden by `--quiet`.

## Matching semantics

Different options are ANDed together (all of them must hit the same window); repeating one option ORs it.
Conditions are never combined across two different windows.

```powershell
ECAPTURE.EXE --process notepad.exe --title-contains Report D:\shots\r.png
# windows whose process is notepad.exe AND whose title contains "Report"
```

- `--title` compares the whole string and `--title-contains` matches a substring; both are **case-sensitive**.
  `--class`, `--process` and `--exe` are case-insensitive.
- Enumeration skips invisible and zero-size windows by default; **minimized windows cannot be captured** and are
  only mentioned separately in the `hint`.
- When several windows match and no disambiguation option was given, the tool refuses to pick one: it reports
  `match.ambiguous_window` (exit code 5) and lists every candidate in `hint`, ordered by z-order.
- The candidate list is ordered by the **current z-order**, topmost first, and `--topmost-match` / `--bottommost-match` take its first /
  last entry. The older `--newest` / `--oldest` names stay as aliases with exactly the same behaviour — what they have always
  selected is a z-order position, never a creation time, because Windows exposes no API for a window's creation time (and a
  process start time is not one). Writing an alias only adds a `note.deprecated_option`; giving both spellings of one strategy
  (`--newest --topmost-match`) is still that one strategy, not a conflict.

## Output format

`--help`, `--version` and the "no conditions given" case are plain text. Everything else is JSON carrying only the
capture result and the errors.

The read-only environment queries are **separate contracts** (`--capabilities` / `--diagnostics` / `--screens`, see
[System support](#system-support)): they carry this machine's
environment rather than one capture outcome, so only they have `contract` and `contractVersion`. That does not
run the other way - the capture JSON keeps exactly `captured` / `images` (plus `errors` / `notes` / `input` as
described below) and never grows a `program.version` because the query has one.

A window image (real shape of the output; the numbers come from one actual capture):

```json
{
  "captured": 1,
  "images": [
    {
      "file": "D:\\shots\\EvernightCapture - File Explorer.png",
      "bytes": 60198,
      "width": 1247,
      "height": 607,
      "format": "png",
      "source": "wgc",
      "path": "wgc",
      "scope": "window",
      "rect": {
        "x": 688,
        "y": 29,
        "width": 1247,
        "height": 607
      },
      "hwnd": "0x001B0C48",
      "pid": 31468,
      "title": "D:\\share\\EvernightCapture - File Explorer",
      "class": "CabinetWClass",
      "image": "explorer.exe",
      "elapsedMs": 156
    }
  ]
}
```

A screen image (`--monitor` with no window conditions) has no window to attribute, so it swaps those fields for
`monitor` / `device` / `primary`, and `hwnd` / `pid` / `title` / `class` / `image` do not appear at all — callers
tell the two kinds apart by checking whether `monitor` exists. Both kinds carry `path` / `scope` / `rect`: a whole
screen is `screen.wgc` / `screen.bitblt` / `screen.duplication` with `scope` `desktop`, while `--monitor <n>`
together with window conditions still produces window images with `scope` `window`.

When `--cursor` was given, each image also carries `cursorRequested` / `cursorEffective` / `cursorBasis` - what was
asked, what this path actually delivered, and on what evidence. Without that option none of the three appears, which
is what keeps the default (no request at all) byte-for-byte the older output shape; see
[The mouse pointer in the image](#the-mouse-pointer-in-the-image---cursor).

The channels that read the screen and cut the target out of a whole desktop frame (`duplication`, and `bitblt` /
`dwm`'s screen route) additionally report *where* in the desktop they actually got those pixels:
`requestedRect` is the area this channel set out to capture, `capturedRect` is the area it really captured — both in
virtual-screen coordinates, so they line up with `rect` and with what the confirmation dialog listed. `clipped`
appears only when the two are not the same (a window straddling two monitors, or one hanging off the edge): the
image is delivered as-is, it is just not the whole target, and a `note.capture_clipped` entry says how much was
lost on each side. `rotation` appears only when the desktop frame had to be turned (90 / 180 / 270 degrees
clockwise) to match the orientation the monitor is actually displaying in; an absent `rotation` means no turn was
applied. Window-content channels (`wgc`, `printwindow`, `dwm.thumbnail`) capture the target whole by construction,
so they emit none of these keys — an absent key means "nothing was left out", not "unknown".

An error (`--hwnd` given a garbage value):

```json
{
  "captured": 0,
  "images": [],
  "errors": [
    {
      "code": "cli.invalid_number",
      "message": "--hwnd needs a valid handle value (decimal, or hexadecimal with a 0x prefix)",
      "option": "--hwnd",
      "value": "zzz",
      "hint": "Plain digits parse as decimal; write hexadecimal as 0x..., or it is taken as hexadecimal when it contains a-f. No sign and no whitespace; an underscore may only sit between two hexadecimal digits (0x_1A and 1A__2B are both rejected)"
    }
  ]
}
```

Rules:

1. `captured` and `images` are always present (`[]` when empty); `errors` appears whenever it is non-empty
   (`--quiet` cannot suppress it); `notes` appears only when non-empty and not `--quiet`; `input` appears only with
   `--verbose`. Look at `errors` before reading `images`.
2. Empty fields of a diagnostic drop the whole key — there is no `null` placeholder. The three fields describing
   where a frame came from are the one thing never dropped by `--quiet`: every image carries `path` (the internal
   route that really ran — `wgc`, `printwindow`, `dwm.thumbnail`, `dwm.screen`, `bitblt.screen`,
   `duplication.frame`, `screen.wgc`, …), `scope` (`window` or `desktop`, derived from `path`) and `rect` (the area
   of the screen that route was authorized to sample; omitted only when it cannot be measured). The screen-reading
   channels also keep `requestedRect` / `capturedRect` / `clipped` / `rotation` (see above) — those are location
   judgements too, so `--quiet` does not hide them either.
3. `code` values are stable: `cli.*` / `note.*` / `match.*` / `capture.*` / `io.*`, append-only, never renamed.
   Among frame failures, "the frame never arrived" (`capture.frame_timeout`) and "the target window is gone"
   (`capture.window_gone`) each have their own code instead of being lumped in with the generic `capture.failed` —
   the next step differs between them (wait and retry versus enumerate the windows again). A frame whose own memory
   layout does not add up (zero size, a side beyond 16384 px, a row pitch that cannot hold one row of pixels, or a
   buffer shorter than pitch × height) is refused with `capture.frame_invalid` (exit code 7); crop, row repack and
   encoding all re-check it, so a broken frame is never read past its buffer. "That monitor is no longer part of the
   desktop / its picture changed after the confirmation" is `capture.monitor_changed` (exit code 7): the next step is
   to enumerate the monitors again and re-confirm, not to switch channel and hope — another monitor would be a
   picture nobody approved. `note.capture_clipped` is a quality note like `note.frame_uniform`: the image is
   delivered and the exit code is unchanged.
4. Streams: by default everything goes to stdout and stderr stays empty; once the image occupies stdout (explicit
   `--out -`, or no output path at all) the whole JSON moves to stderr. The two streams never mix. Even the
   last-resort diagnostic for "building the result itself threw" always goes to stderr (at that moment there is no way
   to tell whether image bytes already claimed stdout, and the tool will not re-parse the command line to guess).
   **A result that cannot reach the agreed stream is an I/O failure**: the exit code becomes `8`, and a text copy that
   did get through on the other stream never restores the original value — a caller reads the agreed stream, so not
   finding it there means not getting it.
5. `captured` equals the number of `images`; one window per image, and with `--monitor all` one monitor per image.
   **stdout delivers exactly one image per run**: when more than one target is hit (`--all`, or several monitors) and
   stdout is the destination, the whole batch is refused before the consent dialog and before the first frame
   (`cli.stdout_multiple_targets` + exit code 1) — nothing captured, no file written. The judgement uses the number of
   targets actually matched, so `--all` that hits a single window may still write to stdout. PNGs concatenated head to
   tail on one stream are not a decodable image, and the tool will not treat `-` as a file-name prefix that produces
   local files like `-_1.png`.
6. `images[].source` and the `backend` field of an error always name the **channel that really ran**: when
   `--capture auto` falls back successfully, `source` names the link of that chain which delivered the frame rather
   than `auto`; when the whole fallback chain fails, `backend` lists the channels that were actually tried. Window
   images and screen images both carry `source`. `images[].path` is finer than that: one channel can hold several
   routes, and the authorization is decided by the route, not by the label — `dwm.thumbnail` reads the window's own
   pixels while `dwm.screen` (that channel's internal fallback) reads the screen. That fallback is entered when the
   thumbnail route itself failed, never because the delivered picture came back a single colour.
7. **Saving**: the whole batch's final absolute output names are computed before the first frame is taken (and
   before any consent dialog). Two targets resolving to the same name give `io.output_collision` (exit code 8)
   with nothing captured and nothing written — the tool never renames behind your back and never lets image 2
   overwrite image 1. Each file is then written to a unique temporary file in the target directory and only renamed
   onto the target once everything is written and flushed, so a failed write leaves the previous file untouched; with
   `--no-overwrite` that final rename is itself the "already exists?" check (`io.file_exists`), never a pre-check.
8. Every step's failure diagnostic also carries its own coordinates, present only when that step really obtained the
   value: `target` (which target — a `0x…` handle for a window, a device name such as `DISPLAY1` for a monitor),
   `backend` (which channel), `stage` (`consent` / `capture` / `encode` / `write` / `stdout`; parse-time errors have
   no stage at all), `hresult` (a raw value shaped like `0x80070005`), `win32` (the raw `GetLastError()` number).
   `message` follows `--lang` while these never do. Consent failures are their own branch:
   `capture.access_denied` means somebody answered "No" or closed the dialog, `capture.consent_unavailable` means
   the dialog could not be shown at all (no interactive desktop) — both are exit code `6`, both carry
   `stage=consent`, `target`, `backend` (the channel) and `value` (the route), and neither is a technical access
   denial (`capture.failed` carrying `hresult=0x80070005`), which is what makes them tellable apart. A target that
   moved or resized after the answer gives `capture.consent_stale` at `stage=capture` instead: exit code `7`,
   retryable by selecting the target again.
   The HRESULT and Win32 code the backend handed back are passed through as they are instead of being masked by
   `E_FAIL` or `E_NOINTERFACE`. A black frame is never asserted to be DRM — the wording only lists the possibilities.
   A single-colour frame is never asserted to be a failed capture either: the image is delivered and
   `note.frame_uniform` records the fact (which colour, which channel, which target). Only `duplication` refuses a
   frame on such grounds, and only when the API itself says there was nothing to show — no present record at all
   *and* the whole frame one colour.

## Cropping inside the window (`--roi` / `--client-area`)

Both options answer a single question: *which part of the delivered window image do you actually want*. They are
mutually exclusive (`cli.crop_conflict` + exit code 1), they say nothing sensible about a whole-screen target
(`capture.unsupported` + exit code 1 — a screen has no window whose top-left corner a crop could be relative to),
and they are refused together with the read-only queries (`cli.query_conflict` / `cli.window_query_conflict`).

### The coordinate system, and why it is never desktop coordinates

The origin `(0,0)` is **the top-left pixel of the delivered whole-window image**, and the far edge is exclusive.
That image is the visible window frame the user actually sees (`DWMWA_EXTENDED_FRAME_BOUNDS`): the transparent
DWM resize border that `GetWindowRect` still counts is not part of it, and `--client-area` drops the title bar and
the three borders from inside what is left.

Units are **physical pixels, never scaled by DPI**. The process declares per-monitor DPI v2, so window rectangles,
frame sizes and pixel buffers already live in physical pixels and there is no scaling step here: one and the same
`--roi 0,0,200,120` takes 200×120 pixels whether the window sits on a 1x or a 2x display. A caller that thinks in
logical pixels (DIP) multiplies that scale factor itself — the tool does not guess which monitor the window is on,
and does not guess which DPI to apply.

Because the rectangle is anchored to the image, **it is never re-read as a desktop-absolute position**. That is not
a wording choice: taking desktop coordinates would let a caller ask for a region nobody approved.

`--roi` wants exactly four decimal integers separated by commas (`[0-9]+` only: no sign, no spaces, no decimal
point, no exponent, no underscores, no `0x`, no non-ASCII digits). `x` and `y` may be `0`; `w` and `h` are at least
`1`; none of the four may exceed `16384` — the same ceiling as a frame's single side, which `--capabilities`
reports as `limits.roiMaxValue` so help, parser and query read one number.

### What does not fit is refused, never repaired

| Situation | Code | Exit | Stage |
| --- | --- | --- | --- |
| Four fields that are not what was promised (sign, space, wrong field count, zero width or height, over the ceiling) | `cli.invalid_value` | 1 | parse |
| The rectangle is already too large for the window as selected, so before any dialog and before any file is planned | `match.roi_out_of_range` | 1 | match |
| The frame came back too small to hold it (the target resized in between, or part of it hangs off the screen) | `capture.roi_invalid` | 7 | capture |
| The question needed to locate the rectangle gave no answer (client area unreadable, or this image cannot be tied to a region of the screen) | `capture.roi_unmeasurable` | 7 | capture |

None of these clamps the rectangle to the edge, slides it inside, or falls back to "here is the whole window
instead" — the last one would hand over an image the caller did not ask for. And none of them lands a pixel: the
pre-capture check runs before the consent dialog, so a request that cannot be honoured never disturbs a person,
and the post-capture one throws the frame away instead of writing it. A batch is a batch: if one of several
matched windows cannot hold the rectangle, nothing in that batch is captured (same rule as
`match.index_out_of_range`). `--dry-run` takes no frame, so it does not judge the geometry either — the requested
crop is echoed under `-v` as `input.crop` either way.

### The crop does not widen or narrow what a person approved

The crop runs **after** the frame is captured, which is the whole reason it cannot be used to reach pixels that
were never on the table: what a person saw in the dialog is the whole target, the tier the path belongs to is still
decided by `images[].path` (see [Screenshot authorization](#screenshot-authorization-and---yes)), and a path that
samples the screen still always asks. `--yes` does not start applying because only a small piece is kept in the end
— a `--roi 0,0,8,8` on `bitblt` or `duplication` is refused exactly like a full-screen grab is. Window-content
paths (`wgc`, `printwindow`, `dwm.thumbnail`) are the only ones `--yes` can silence, and that is unchanged here.

### What the result says

A cropped image carries one more group of fields, all of them locating judgements that `--quiet` must not hide:

```json
"width": 120, "height": 80,
"cropMode": "roi",
"cropRect":       { "x": 20, "y": 40, "width": 120, "height": 80 },
"fullWidth": 486, "fullHeight": 293,
"cropScreenRect": { "x": 227, "y": 240, "width": 120, "height": 80 }
```

`cropRect` is in image pixels, `width` / `height` are the final (cropped) size, `fullWidth` / `fullHeight` the size
of the whole-window image before cropping, and `cropScreenRect` is that same rectangle in virtual-screen
coordinates — the same frame of reference as `rect`, `requestedRect` and the rectangles listed in the dialog. The
mapping closes: `cropScreenRect - cropRect` is the image's own screen origin, so a caller can verify it against
`rect` instead of trusting it.

That last line is written **only when the image's screen origin can actually be established**: either the channel
reported the region it really sampled (`capturedRect`, which is what the desktop-crop routes do), or the window's
visible rectangle measured at that moment is exactly the size of the delivered image. Otherwise the key is absent
and `note.crop_mapping_unavailable` says so — an unanswerable question is never folded into a plausible number.
`--client-area` needs that mapping to locate the client area inside the image at all, so when it cannot be
established the capture fails with `capture.roi_unmeasurable` rather than returning the uncropped window.

These fields sit alongside `requestedRect` / `capturedRect` / `clipped` / `rotation`, and the two groups do not
overlap: the first says whether the *whole window* could be sampled from the desktop, the second says which part
of the image that arrived is being handed over.

### Not verified on this machine

`.\tests\crop.ps1` judges the geometry against three independent questions it asks itself
(`GetWindowRect`, `DWMWA_EXTENDED_FRAME_BOUNDS`, `GetClientRect` + `ClientToScreen`), against pixel content
(a cropped image's corners must equal the whole-window image at the requested offset), against a window it really
resized, and against the authorization tier (desktop paths still pop the dialog with a tiny `--roi` and `--yes` —
the test only looks, it never answers). Two things this machine cannot stage are recorded as unverified instead of
inferred: the same `--roi` across two monitors with different DPI (only one monitor is attached), and the race
where the target shrinks between the pre-capture check and the returned frame (that window of time is exactly what
cannot be scheduled — the offline suite `build\ecapture-crop-tests.exe` judges it instead).

## Omitting `--out` (compatibility note)

Giving no output path is **the same request as `--out -`**: the image goes to stdout as PNG, the JSON goes to stderr,
and every diagnostic is the one that step really produced.

| | before | now |
| --- | --- | --- |
| Failure with no output path | rewritten into a single `cli.missing_output` + exit code `1`; `images` and `notes` cleared, real reason held back (exceptions: `cli.stdout_multiple_targets` and a consent refusal) | the real code and the real exit code, unchanged: `match.no_window` (4), `match.ambiguous_window` (5), `capture.access_denied` (6), `capture.failed` (7), `io.write_failed` (8), `cli.invalid_number` (1), … |
| Partial success with no output path | never visible (the whole `images` array was dropped) | images already delivered on stdout stay in `images`, `captured` counts them |
| `cli.missing_output` | exit code 1 | no longer produced. The code is kept in the list so it can never be given a different meaning |
| "You did not give an output path" | a code | a `hint`, and only where naming a file actually avoids the failure: `io.write_failed` + `stage=stdout` on that implicit route (`--out -` means you chose the pipe, so it stays unsaid there) |

How to branch: read `errors[].code` (and its `stage` / `target` / `backend` / `hresult` / `win32`), never the exit
code alone and never whether `--out` was present. `code` values are append-only, so a caller written against either
behaviour keeps working — it only stops seeing a reason that was a lie.

## Exit codes

`0` success / `1` bad arguments / `2` no condition given / `3` `--help` / `4` no matching window /
`5` several matches / `6` target protected, refused on the confirmation dialog, nobody answered it within
`--consent-timeout-ms`, or no dialog could be shown / `7` capture failed, an exhausted `--timeout-ms` budget
**and this machine's Windows version not offering what was asked** (`env.os_too_old` /
`env.channel_unsupported`, see [System support](#system-support)) included / `8` write failed, a budget
exhausted in the write/stdout stage included / `9` internal error.
New meanings only ever append numbers.
`8` also covers "the result JSON could not reach the agreed stream" (writing stdout or stderr failed); text delivered
on the other stream does not count as delivery in that case.

The exit code and the body are two independent signals; `2`/`3`/`4`/`5` are normal control flow, not crashes.
**Partial success is allowed**: with `--all` or `--monitor all`, if some targets fail the images already written
stay in `images` (`captured` can be greater than 0) while the exit code is `7`. A backend that throws instead of
returning a failure invalidates only the single target it was working on: the images before it stay, the fallback
chain and the remaining `--all` targets are not dragged down with it, and the failure rides in `errors` as
`capture.failed`. Errors where switching backend would make no difference — running out of memory, the display device
being removed (`DXGI_ERROR_DEVICE_REMOVED` / `DXGI_ERROR_DEVICE_RESET` / `DXGI_ERROR_DEVICE_HUNG`) — end the whole
batch deliberately instead of being retried one target at a time. An access denial is never a reason to keep
falling back, and neither is a refusal: once somebody answers "No" (or no dialog can be shown), the rest of that
request is not attempted — no other backend, no second ask, while every image already completed stays in `images`.

The three read-only environment queries use only `0` and `1`: `0` = the document was delivered, even when it says this machine is
too old and no route is available (**a successful query and a possible capture are two different things** - branch
on `status`, do not infer the environment from an exit code); `1` = that invocation does not fit the contract
(`cli.query_conflict`, see [System support](#system-support)).
They never produce `4`/`5`/`6`/`7`/`8`: no window was enumerated, no dialog was shown, no file was written.

The two read-only window queries (`--list` / `--inspect`) share `0` and `1`, and additionally use `4`
(`match.no_window`, only for `--inspect`, which needs one target) and `5` (`match.ambiguous_window`, several
windows survive the selection policy), plus **`7` on exactly one path**: this run's *condition evaluation did not
finish* (`match.timeout` - the `--timeout-ms` budget was spent on regex backtracking or on fetching a title from a
hung window, or that step's helper process itself failed). That `7` means "this question could not be answered",
not "the capture failed", so its `hint` is written in query terms and says plainly that switching `--capture` does
nothing - there is no channel to switch on this path. **`6` and `8` never appear**: no dialog was shown and no file
was written, and those two codes are exactly about those two things. For `--list` the exit code is `0` even when
nothing matched - an empty list is the answer.

## System support

Three different numbers must not be blended into one slogan:

| Layer | Value | Where it comes from |
| --- | --- | --- |
| Per-route API history floor | any image at all 10.0.10240 · `duplication` 10.0.9200 · `printwindow` / `dwm` 10.0.9600 · `wgc` 10.0.18362 | what Microsoft documents for the exact call that route makes. The encoder (WinRT `BitmapEncoder`) is shared by every channel and every format; the WGC route goes through `IGraphicsCaptureItemInterop::CreateForWindow` / `CreateForMonitor`, which arrived with Windows 10 version 1903 — even though the `Windows.Graphics.Capture` namespace itself appeared in 1803, and this tool has no "let the user pick a window in the system picker" path to fall back on |
| What this tool declares | 64-bit Windows 10 version 1903 (build 18362) or later | the highest of those floors, because that is what the default channel needs in order to deliver a window image — not the oldest API some route touches |
| What has actually been tested | Windows 10 version 22H2 (build 19045), x64 | every on-device judgement under `tests\` runs on that one machine |

Builds between 10240 and 18361 can load this exe, and can capture: the tool gates each route by its own
floor instead of refusing the whole program there. They are not declared support and have never been
measured, so treat them as "expected to work, unverified".

Nothing here claims anything about Windows 7 or 8 — **this binary cannot load there at all**. It statically
imports the `api-ms-win-core-winrt-error-l1-1-1` API-set contract (whose documented minimum client is
Windows 8.1) plus `api-ms-win-core-winrt-l1-1-0` and `api-ms-win-core-job-l2-1-0` (Windows 8), and the
API-set mechanism does not exist on Windows 7; none of those three contracts is in the set the UCRT
redistributable installs, so they cannot be added to an older system. `.\tests\compat.ps1` checks those
names against the shipped binary, so the load floor is a fact about this file rather than an inference.
Windows 8.1 *can* load and start it - and there the capability check below is what answers, because
`Windows.Graphics.Imaging.BitmapEncoder` does not exist before Windows 10 and no image at all can be
produced. A single `BitBlt` or `DwmRegisterThumbnail` call existing on Windows 7 therefore says nothing
about this program, and the PE header's `subsystem version 6.00` is the MSVC linker default, not a
support claim either.

### Capability check at run time

Before enumerating windows, planning output names, showing a consent dialog or reading a pixel, the tool
compares the Windows build read from `ntdll!RtlGetVersion` (never `GetVersionEx` — that one answers
according to the application manifest and to version helper) against the floors above, and reports:

| Code | When | Exit | Does another channel help? |
| --- | --- | --- | --- |
| `env.os_too_old` | build below 10240 (so also Windows 8.1, which can load the file): the one encoder implementation every format goes through is not there | 7 | **No.** Not a channel problem and not a target problem — nothing on this machine can produce an image |
| `env.channel_unsupported` | the channel asked for explicitly has a higher floor than this build | 7 | **Yes** — another `--capture` value, or `auto`. Retrying the same target cannot help, and the requested channel is never substituted |
| `note.channel_unavailable` | `auto` found a channel whose floor this build is under and dropped it from the chain | unchanged | the image can still come from another channel; `images[].source` names the one that did it |
| `note.os_unverifiable` | the build number could not be read at all | unchanged | nothing was filtered by version this time - no answer counts as either "supported" or "unsupported" |

`--verbose` echoes `input.osBuild` and `input.captureChain` (the channels this machine can actually offer
for the kind of target requested), so an agent can ask the capability question without capturing
anything. `--dry-run` never gates on the environment, because it takes no frame.

Device-level capability is deliberately not predicted: a driver that will not feed desktop duplication, a
machine that refuses Windows.Graphics.Capture, a session with no interactive desktop, an N edition missing
media components — none of those show up in a version number, and each reports its own `capture.*` code
with the real HRESULT rather than being guessed at in advance.

### Read-only capability queries (`--capabilities` / `--diagnostics`)

That check does run before any capture, but you only see it after placing an order. These two commands are
the read-only outlet for the same judgement: not a pixel taken, not a consent dialog shown, no file written,
no network, no environment variables read, and no window condition required.

```powershell
ECAPTURE.EXE --capabilities              # which routes this machine can take right now (JSON)
ECAPTURE.EXE --diagnostics               # build version + a checkable build id + backend status (JSON)
ECAPTURE.EXE --capabilities -v           # plus a probes section: each raw answer and which API gave it
```

Three rules:

* **Three separate facts.** `compiled` says whether this binary implements the route at all. `status` says
  whether this machine's own evidence (version floors plus screen topology) lets it run now.
  `verifiedOnThisMachine` says whether **this project** has actually exercised the route on a machine just
  like this one (only the development machine - see the table above). None of the three stands in for another.
* **No answer is reported as no answer.** Every fact is one of `yes` / `no` / `unknown`; `unknown` is never
  folded into either "works" or "does not work", and the key is not silently dropped. When the build number
  could not be read, every `status` becomes `unverified` while `autoChainWindow` still lists all four
  channels - not filtered means not filtered, not "all supported".
* **`available` is not a guarantee.** It carries no promise that some specific window will capture: drivers,
  protected content and HDR mode are outside this layer. The `caveats` array at the end of the document
  exists precisely to pin that down.

| Section | Contents |
| --- | --- |
| `contract` / `contractVersion` | only these two documents carry a contract version (currently 1). The capture JSON stays as lean as 《Output format》 describes and gains no top-level metadata from this |
| `program` | name, the file name `ECAPTURE.EXE` itself (no directory), version, architecture, `buildId` |
| `os` | this machine's build (that group reads `unknown` when `known` is false), `declaredMinBuild`, `encoderMinBuild`, `testedMinBuild` + `testedArch` (the environment this project actually tested) and `matchesTestedEnvironment` |
| `session` | attached to the console session, remote desktop, screen topology present and how many monitors, whether this process is elevated, `consentDialogExpected` (inferred; `consentDialogProbed: false` says no dialog was ever shown) |
| `authorization` | `yesSkips: "window-content"`, `desktopPixelsAlwaysAsk: true`, unregistered paths treated as `desktop`, plus the whole internal-path registry with `scope` and `consentWithoutYes` / `consentWithYes` per row - the machine-readable form of the table in 《Screenshot authorization and \`--yes\`》 |
| `backends` | per route: `compiled` / `status` / `reason` / `minBuild` / `verifiedOnThisMachine`, plus which internal path it takes for window and for screen targets (`dwm`'s screen fallback included, so `--yes` cannot be read as covering more than it does) |
| `formats` | per format: `compiled` / `status` / `reason` / `minBuild` / `registered`. `registered` is always `unknown` because this layer does not exercise encoders (doing so would be probing capability by producing an image, the same reason we never probe by capturing). `webp` / `ico`, once advertised and then removed for lack of an encoder, stay here as `compiled: false` + `reason: "not_compiled"` so a caller gets a definite answer |
| `cursor` | the `--cursor` story: `default` (what happens when the option is absent), the three values, that one switch as `compiled` / `status` / `reason` / `minBuild` (19041) / `verifiedOnThisMachine`, then one row per registered internal path with `capability` (`settable` / `excludes_cursor` / `unregistered`), `reason` and `include` / `exclude` each as `yes` / `no` / `unknown`, plus `pointerShapeCompositing: "never"` and `pixelRetouching: "never"`. Nothing is probed by capturing, so a path that is not in the registry reads `unknown` rather than a guessed answer |
| `color` | the `--hdr` story: `default` (what happens when the option is absent), the three values (`auto` / `tonemap` / `refuse`), `compiled` / `status` / `reason` / `verifiedOnThisMachine`. `status` is about "can this build bring back a wide-gamut frame and how does it map", it does **not** ask whether this screen is currently in HDR mode (reason `hdr_display_mode_not_probed`); `verifiedOnThisMachine` is always `no` (this project has no HDR display, so it never claims color acceptance). One row per registered internal path (`capability` = `wide_gamut_capable` / `sdr_source_only` / `unregistered`), plus `toneMapping` / `floatIntermediateFrame: "per_pixel_registers"` / `encoderOutput: "sdr_bgra8"` (HDR is always mapped to 8-bit SDR for delivery; no native-HDR output) |
| `autoChainWindow` / `autoChainScreen` | the `auto` chain this machine can take now. Computed by the **same** `GateChannels` call that fills `input.captureChain` for a real run, and `tests\capabilities.ps1` compares the two |
| `limits` | maximum frame side and bytes, `--timeout-ms` ceiling, built-in isolated-call ceiling, WGC frame-pool rebuild count, ordinal and PID ceilings, `stdoutTargetsMax: 1`, JPEG quality range |
| `privacy` | what this query declares it did not do: no screen captured, no dialog shown, nothing uploaded, no user files enumerated, no environment variables read, no usernames, no paths |
| `caveats` | stable ASCII tokens listing what this report does **not** assert: `available_is_not_a_guarantee`, `no_capture_performed`, `no_consent_dialog_shown`, `encoder_state_not_probed`, `device_capability_not_predicted`, `consent_dialog_state_inferred_not_probed`, `subsystem_version_is_linker_default`, plus per machine `os_version_unavailable` / `display_topology_absent` / `display_topology_unavailable` / `remote_session_observed` / `desktop_paths_need_answerable_dialog` / `unelevated_process_may_miss_elevated_targets` / `build_identity_unavailable` / `this_environment_not_tested` / `tested_environment_unknown`, and always `cursor_effective_is_a_setting_not_a_pixel_check` + `pointer_shape_never_composited_nor_erased` (the cursor fields stop at the setting and the source, never at "this picture visibly has or has no pointer"), and always `hdr_tone_mapping_not_verified_on_hdr_display` + `hdr_output_is_tone_mapped_to_sdr_bgra8` (the HDR mapping math is verified offline but there is no HDR display to test end-to-end, and HDR is always mapped down to 8-bit SDR) |

Both documents come out of **one** judgement function (`BuildEnvReport` in `src/EnvReport.cpp`) and differ only
in which sections they print: `--diagnostics` always includes the `build` section (PE link timestamp, machine
type, image size, subsystem) while `--capabilities` expands it only under `--verbose`. Version numbers,
statuses, the backend list and `limits` are the same object, so there is no second copy of the environment
that could contradict the first.

**The build identity is checkable**: `buildId` is `version-architecture-hex link timestamp`, and that timestamp
is the same field `dumpbin /headers` reads in the published artifact. It is read from this process's own
already-mapped PE headers - no file is opened and no directory is enumerated, so an installation path
containing a user name cannot leak into the report either. The PE `subsystem version` is listed as a bare fact
with the `subsystem_version_is_linker_default` caveat attached: it is MSVC's linker default, not a support claim.

`--capabilities` / `--diagnostics` accept only `--lang`, `-v` and `-q`. Combining them with any capture option
(window conditions, `--monitor`, `--capture`, `--out` or a positional path, `--yes`, `--dry-run`, either
deadline) is `cli.query_conflict` + exit code 1, which lists every offending name at once and captures nothing
and writes nothing. Neither command falls into "no condition means help": a query is itself an explicit intent.
`-q` on a query drops only the `caveats` section, `-v` adds `probes`, and neither touches the answers.

The whole document is ASCII (machine-readable values are never translated), so on one machine every `--lang`
produces byte-identical output.

### Structured window discovery and inspection (`--list` / `--inspect`)

Before these two commands existed, an AI caller that wanted to know *which* windows a set of conditions hits had
exactly two ways to ask, and both were wrong. `--dry-run` answers with one human-readable line per candidate inside
`note.dry_run` (`hwnd=0x… pid=… 1261x614+681+22 class=… title=…`), so the handle, the rectangle and the title have
to be parsed back out of a string that the tool never promised to keep stable - and a title containing a space or a
`|` breaks the parsing. Asking for a real image instead requires an output path, opens the capture-level consent
dialog, and turns "several windows match" into an error - which is a sensible outcome for a batch of screenshots and
a nonsense outcome for "let me look first".

These two commands are the read-only outlet for that question. They run **the same condition evaluation** as a
capture - same OR within one option, same AND across options, same `--monitor` screen filtering, and the same helper
process whenever `--title-regex` or `--timeout-ms` is in play - but they produce no image:

```powershell
ECAPTURE.EXE --list --process notepad.exe                    # every matching window, structured
ECAPTURE.EXE --list --class CabinetWClass --limit 5 --offset 5
ECAPTURE.EXE --list=all --title-contains Report              # also the minimised ones
ECAPTURE.EXE --inspect --hwnd 0x001A0B4C                     # one window, fully described
ECAPTURE.EXE --inspect --process notepad.exe --topmost-match # disambiguate exactly like a capture would
```

Five rules, and each one exists because the alternative was worse:

* **Nothing is captured, nothing is asked, nothing is written.** No backend is called, no consent dialog is shown,
  no file is created, no network access, no environment variable read - the document itself states this in
  `authorization` (`pixelsRead: 0`, `consentDialogShown: false`, `filesWritten: false`). The query also **never
  touches a target window**: no restore, no activation, no z-order change, because "let me look at what is open"
  must not change what is on the screen. `caveats` carries `no_capture_performed` and `no_window_touched` for this.
* **Several matches are not a capture ambiguity.** `--list` pages them (`--offset` / `--limit`, default 50 per
  batch) and reports the real total in `pagination.matched`, so a short list never reads as "there are only these".
  Zero matches is a normal answer: `windows: []` and exit code `0`, not `match.no_window` + `4`. `--inspect` needs
  one target, so it applies **the same** selection policy a capture uses: if that policy still leaves several, it is
  `match.ambiguous_window` + exit code `5` - it never picks one for you, and it never grabs a look-alike instead.
* **The list is a snapshot and it expires.** Handles get reused, titles change, processes exit, so the `hwnd` / `pid`
  / class in here are **not a credential** you may hold on to. Every successful query carries
  `note.window_query_stale`, and the `identity` block of each row states `verificationRequired: true`,
  `isAuthorizationToken: false` and `raceWindowReducedNotEliminated: true`. A later capture re-checks the target
  identity before reading a pixel (that is `capture.target_gone` / `capture.target_changed` /
  `capture.target_unverifiable`), and consent is still decided by where the pixels come from: **`--yes` changes
  nothing here** (`authorization.yesAffectsResult: false`) - it neither unlocks a field nor skips a dialog that this
  query never shows.
* **An unreadable field says so.** Cross-process questions have three outcomes, written per field: `readable`,
  `denied` (the system refused this caller), `failed` (asked and it did not answer), each with the raw Win32 code.
  Unreadable values are sentinels (`0` / empty) plus that state, never silent absences, and the document does not
  advise running elevated: `caveats` carries `unreadable_fields_are_not_a_prediction`.
* **The visibility policy is stated, not implied.** Invisible and zero-sized windows are excluded - the same rule the
  capture enumeration uses - and `policy` says so (`invisibleExcluded`, `zeroSizedExcluded`); minimized windows are
  excluded by default too, are counted in `policy.minimizedExcluded`, and `--list=all` merges them into the same
  z-order. There is **no** claim about "system windows": Windows exposes no attribute that means "this is a system
  window", so `policy.systemWindowAssertion` is `false`.

Fields per row (window inspect returns the same object as `window` instead of the `windows` array):

```json
{ "contract": "windowquery", "contractVersion": 1, "query": "list",
  "program": { "version": "0.4.0" },
  "authorization": { "readOnly": true, "pixelsRead": 0, "consentDialogShown": false,
                     "filesWritten": false, "yesAffectsResult": false,
                     "identityFieldsAreNotConsent": true },
  "policy": { "invisibleExcluded": true, "zeroSizedExcluded": true, "minimizedIncluded": false,
              "minimizedExcluded": 2, "systemWindowAssertion": false, "order": "zOrder" },
  "pagination": { "offset": 0, "limit": 50, "limitDefaulted": true, "defaultLimit": 50,
                  "maxLimit": 8192, "matched": 52, "returned": 50, "truncated": true,
                  "nextOffset": 50 },
  "windows": [ { "hwnd": "0x001A0B4C", "pid": 31468, "title": "…", "class": "CabinetWClass",
                 "image": "explorer.exe",
                 "rect": { "x": 681, "y": 22, "width": 1261, "height": 614 },
                 "visible": true, "minimized": false, "zOrder": 3,
                 "readability": { "process": { "state": "readable" },
                                  "imagePath": { "state": "denied", "win32": 5 },
                                  "processStart": { "state": "readable" },
                                  "rect": { "state": "readable" } },
                 "identity": { "hwnd": "0x001A0B4C", "pid": 31468, "class": "CabinetWClass",
                               "processStartTicks": 134351142668527401,
                               "selectionNeedsRecheck": false, "verificationRequired": true,
                               "isAuthorizationToken": false,
                               "raceWindowReducedNotEliminated": true } } ],
  "caveats": [ "no_capture_performed", "no_consent_dialog_shown", "no_window_touched",
               "snapshot_expires", "identity_fields_are_not_a_token",
               "invisible_and_zero_sized_excluded", "unreadable_fields_are_not_a_prediction",
               "list_may_be_partial" ] }
```

`title`, `class` and `image` are delivered verbatim - not truncated, not escaped into a prose line, not case-folded -
so a caller reads fields instead of parsing a sentence. The full process path is **not** written unless
`--inspect=path` asks for it, because installation paths commonly contain a user name; matching `--exe` reads the
path either way, which is unrelated to what the report exposes. `identity` carries exactly the facts the capture
pipeline re-checks (handle, PID, that PID's creation time, class, plus whether the original conditions have to be
re-evaluated), so `--inspect --hwnd <that handle>` describes the same constraint set the capture will insist on.
`processStartTicks` is `unknown` when it could not be read at enumeration time - which is a judgement that was not
made, not a value of zero.

`--list` / `--inspect` accept the window conditions, `--monitor`, `--offset` / `--limit`, `--timeout-ms`, `--yes`
(inert) and `--lang` / `-v` / `-q`. Capture-level options are a `cli.window_query_conflict` + exit code `1`
(`--out`, a positional path, `--format`, `--quality`, `--no-overwrite`, `--capture`, `--dry-run`,
`--consent-timeout-ms`, `--capabilities`, `--diagnostics`, and the two queries together). The pick options are
handled per entry point: they narrow a single target, so they are valid with `--inspect` and a conflict with
`--list`, which is by definition about all matches. Like the environment queries, these never fall into "no
condition means help", and when a query itself has nothing to report the failure shape stays the one the capture
uses (`captured: 0`, `images: []`, `errors[]` with the same codes) for argument-level problems - so a caller's
`errors[].code` branch does not fork. `--list`'s document is `windowquery`, `--inspect`'s is `windowinspect`:
different shapes, different contract names, one shared set of fields.

`--dry-run` is unchanged and remains the compatible entry point: it still answers in `note.dry_run`, still requires
no output path, and is still a conflict with `--list` / `--inspect` rather than being silently replaced by them.

### Read-only screen enumeration (`--screens`)

Naming "that monitor" used to have exactly one spelling: `--monitor <n>`, where `n` is the position in *this run's*
`EnumDisplayMonitors` order. That is not the id Windows writes in Settings, and after a replug or a resolution
change it can name a different panel - and capturing the wrong screen means pixels nobody approved are already on
disk. `--screens` turns that question into data:

```powershell
ECAPTURE.EXE --screens                                  # every screen with all of its identities
ECAPTURE.EXE --monitor device:DISPLAY1 --out shot.png   # by the name of this desktop attach
ECAPTURE.EXE --monitor "id:\?\DISPLAY#GSM41A2#5&…#{…}" --out shot.png   # by the cross-session monitor path
```

It is read-only like the other queries: no pixel taken, no consent dialog, no file written, no network, and no
display setting touched - finding out "is this screen rotated" by calling `SetDisplayConfig` would be editing the
exam to read the answer. Its document is a third contract (`screens`, version 1) and stays as separate from the
capture result as `capabilities` and `windowquery` are.

Four identities per screen, each stating how far it stays true (fields, not prose):

| Field | What it is | Stable across | Selector |
| --- | --- | --- | --- |
| `ordinal` | position in this run's enumeration | `this_invocation` | `--monitor <n>` |
| `deviceName` | GDI view device name, `\\.\DISPLAY1` | `this_desktop_attach` | `device:` |
| `monitorDevicePath` | monitor devnode device interface path | `cross_session_expected` | `id:` |
| `adapterLuid` | adapter's locally unique id | `this_session` | none - association only |

`screens[].selectors` holds the exact strings to write back (`device:DISPLAY1`, `id:\?\DISPLAY#…`), so nothing has
to be guessed, and `identity.*` says which kinds may be used as a selector. The adapter LUID deliberately has none:
it is unique only inside the current session, so naming a monitor with it would be a bet rather than a reference.
`adapter.devicePath` (the adapter's own devnode path), `adapter.outputTechnology`, `targetId` and `targetAvailable`
come from the same answer, which is how "which card drives this screen" gets reported.

Three rules this layer keeps:

- **Never another screen instead.** An identifier matching nothing is `match.monitor_unknown_id` (exit 4), one
  matching several is `match.monitor_ambiguous_id` (exit 5, every candidate listed - the tool will not pick one),
  and an identity question that returned no answer is `match.monitor_id_unverifiable` (exit 7, whose `hint` says
  plainly that switching `--capture` is not the next step, because this path never chose a channel). None of them
  quietly becomes "use the primary monitor then": a whole-screen image nobody approved is exactly what the consent
  dialog exists to prevent.
- **Unreadable is reported, not emptied.** `dpi` (effective and raw, through `shcore!GetDpiForMonitor`, Win8.1+),
  `rotation.degrees` (what a person sees, from the current `DEVMODE`) and `rotation.panel` (relative to the panel's
  native orientation, from the display config) are separate questions with separate `readability` entries
  (`readable` / `denied` / `failed`) plus that API's own error code. A missing value always means "no answer", and
  the reason sits next to it; nothing here suggests running elevated.
- **A snapshot, not a credential.** Every successful list carries `note.screen_query_stale`, and `caveats` includes
  `device_names_are_not_persistent`, `cross_session_stability_not_tested` and `screen_capture_always_asks`. Naming a
  monitor does not skip the identity re-check before the frame, and it does not skip consent: desktop pixels always
  need a person, `--yes` included (see 《Screenshot authorization and `--yes`》 below).

Identifier naming works wherever a screen is chosen: with window conditions (`--monitor id:… --class …` filters
windows onto that monitor), with `--list` / `--inspect` (literally the same selection function), and in the
pre-capture re-check, which uses the identity that was known at selection time - a device name that turns out to
belong to a different panel stops with `capture.monitor_changed`, and a re-check that cannot be answered stops with
`capture.monitor_unverifiable` instead of falling back to the name. `--monitor <n>` keeps its old meaning, including
`match.monitor_out_of_range` with the full local list in `hint`; the two new identifier forms are documented in
`--help` and in the value syntax section above.

`--screens` belongs to the environment-query family: it accepts only `--lang` / `-v` / `-q`, anything else - `--yes`
and `--monitor` included - is `cli.query_conflict` + exit code `1`, and it never falls into "no condition means
help". `--quiet` removes only `notes`; `identity`, `readability`, `authorization` and `caveats` are judgements, not
courtesy. The document prints device paths and neither a filesystem path nor a username
(`privacy.includesDevicePaths: true`, `includesFileSystemPaths: false`).

## Capture channels

| Value | Channel | Covered window | Hardware-accelerated content | Pointer (`--cursor`) | API history floor |
| --- | --- | --- | --- | --- | --- |
| `wgc` | Windows.Graphics.Capture | yes (DWM cache) | normal | a real switch, set and read back (19041+) | Win10 1903 (18362) — the `CreateForWindow` / `CreateForMonitor` interop, not the 1803 namespace |
| `dwm` | DwmRegisterThumbnail | yes | mostly normal, protected windows black | never in the source pixels | Win8.1 (9600) — registering is older, but the read-back is `PrintWindow(PW_RENDERFULLCONTENT)` |
| `printwindow` | PrintWindow + PW_RENDERFULLCONTENT | yes (window self-draw) | often fully black | never in the source pixels | Win8.1 (9600) for that flag |
| `bitblt` | BitBlt from a screen DC | no, visible pixels only | partly black | never in the source pixels | no floor of its own |
| `duplication` | DXGI desktop duplication frame, cropped to the rect | no, visible pixels only | normal | the pointer arrives as separate metadata | Win8 (9200); RDP / virtual GPUs often yield nothing |
| `auto` | falls back wgc → dwm → printwindow → bitblt | best effort | best effort | `include` narrows it to `wgc` | the chain minus whatever this build gates out |

Those are the **API history floors**, each checked against the Microsoft documentation for the exact call
that route makes. They are not what this program claims to run on, and they are not measured either: see
[System support](#system-support) for the declared floor (Win10 1903, x64), the encoder floor that applies
to every channel, the version actually tested (Win10 22H2 / 19045), and what the run-time capability check
reports instead of a vague failure.

- Want "the window's own content", even if something is on top of it: keep the default `wgc`. Want "what the screen
  looks like right now", occluder included: use `bitblt` or `duplication`.
- `wgc` tracks the window's live size: it reads each frame's own content size rather than only the size the capture
  item had when the frame pool was built. If the window is resized smaller between selecting it and grabbing the
  frame, only the valid rectangle is copied (never the undefined edge left in the larger texture); if it grows beyond
  the frame pool, the pool is recreated and the frame re-grabbed within the `--timeout-ms` budget. A frame is never
  delivered clipped yet reported as the whole window, and `images[].width`/`height` equal the size at that moment.
- Whether a capture has to ask a person is decided by the route it really takes, not by the value you typed — see
  the next section.
- A bad `--capture` value fails during parsing with `cli.unknown_capture_method` (exit code 1) and **never degrades
  to the default channel**; only `auto` may fall back, and a successful fallback emits `note.capture_channel`.
- DRM / protected content is always black. Driver-level black bars (some players) are defeated by some channels and
  not by others — nothing is guaranteed.
- A single-colour result is not treated as a failed capture. `dwm` used to read "the thumbnail came back flat" as
  "nothing was composed" and fell back to sampling the screen at that rectangle — a desktop route that has to be
  authorized separately. It now falls back only when that route actually failed; a flat image is delivered with the
  quality hint `note.frame_uniform` instead.
- Whole-screen capture only uses `wgc` / `duplication` / `bitblt`; `--monitor` with `dwm` or `printwindow` fails
  during parsing with `capture.unsupported` (exit code 1). In screen mode `auto` falls back wgc → duplication →
  bitblt.
- `duplication` is monitor-aware in three ways, all of them reported rather than assumed:
  - **Rotation.** The desktop frame a driver hands back is not necessarily in the orientation the monitor is
    displaying in. The route compares what the output claims (`DesktopCoordinates`) with the texture it actually got,
    then turns the crop 0 / 90 / 180 / 270 degrees clockwise so the delivered image is always in the same coordinate
    space as the rectangles the confirmation dialog listed — never a double swap. What was applied shows up as
    `images[].rotation`. A texture that matches neither shape is refused with `capture.frame_invalid` instead of being
    cropped anyway.
  - **Which graphics adapter.** All adapters and outputs are enumerated first and the target is located in that
    table; the D3D11 device is then created **on the adapter that owns the output**, which is what `DuplicateOutput`
    requires. A screen driven by a second GPU is therefore reachable, and the old "default adapter first" blind spot
    is gone. There is no WARP fallback on this route: a software device owns no physical output, so it would hand
    back an empty frame while still reporting the right size.
  - **Only one output per target.** A window that straddles two monitors (or hangs off the edge) is captured where it
    overlaps the output with the largest overlap; the rest is *not* in the image. That shows up as
    `capturedRect` != `requestedRect`, `clipped` and `note.capture_clipped` rather than silently looking like the
    whole window. Stitching one window across adapters is not implemented.
- If the target monitor leaves the desktop or changes shape after the confirmation, the capture stops with
  `capture.monitor_changed` (exit code 7) — the tool never substitutes another monitor, and the authorization stays
  bound to the one a person looked at.

## The mouse pointer in the image (`--cursor`)

`--cursor default|include|exclude` says whether the mouse pointer belongs in the image. The default value,
`default`, means this tool changes **nothing**: it touches no channel's cursor setting and the result carries none of
the three cursor fields, so the output is exactly what it was before this option existed. Writing
`--cursor default` on purpose is a different thing - still no change, but the result reports what that path really
delivered.

What each route can promise is decided by **where its pixels come from**, not by the channel name, and the registry
for that is `src/CursorControl.h`: one row per internal `images[].path`, the same shape as the authorization
registry, and a path that is not registered is treated strictly (neither request is claimed).

- `wgc` and `screen.wgc` are the only paths with a switch that can really be set and read back -
  `IGraphicsCaptureSession2::IsCursorCaptureEnabled`, introduced in Windows build 19041. That is a *higher* floor
  than the `wgc` channel's own 18362: a 1903 machine can capture with `wgc` and still be unable to say anything
  about the pointer.
- `printwindow` (a window painting itself into a DC), `dwm.thumbnail` (the DWM redirection surface),
  `dwm.screen` / `bitblt.screen` / `screen.bitblt` (a screen DC, where the system cursor is drawn outside the DC's
  content) and `duplication.frame` / `screen.duplication` (the desktop image, from which the pointer is delivered
  as *separate metadata*) contain no pointer at all. So `exclude` is true of them as a fact about the source, and
  `include` is simply not something they can do.

Two rules follow, and both exist so that "I asked for it" can never be read as "I got it":

- **A request a route cannot deliver is refused, not re-routed.** `--cursor include` with `printwindow`, `dwm`,
  `bitblt` or `duplication` is `capture.cursor_unsupported` (exit `1`) at parse time - before any dialog, before any
  output name is planned, before a pixel. Switching to a channel that reads the screen would neither add a pointer
  (those sources hold none) nor be a frame anybody approved. With `--capture auto` the routes that cannot honour the
  request are dropped from the chain and each leaves a `note.cursor_channel_skipped`; when nothing is left, or when
  this machine's Windows build is below 19041 and the request was explicit, the code is `env.cursor_unsupported`
  (exit `7`) and nothing is captured. `--verbose` echoes the surviving chain as `input.captureChain`, computed by the
  same function the run uses, so `--cursor include` + `auto` shows `["wgc"]`.
- **No pixel retouching.** The tool does not fetch the duplication pointer shape to draw it, never `DrawIcon`s a
  cursor into a frame, and never tries to erase a pointer that is already there - those are image patching and none
  of them is verifiable. `tests\cursor.ps1` fails if such a call ever appears anywhere in `src/`.

For every delivered image, and only when `--cursor` was written at all, three fields say what happened:

| Field | Values | Meaning |
| --- | --- | --- |
| `cursorRequested` | `default` / `include` / `exclude` | what was asked for |
| `cursorEffective` | `include` / `exclude` / `unverified` | what this path actually delivered |
| `cursorBasis` | `wgc_session_property_set` / `wgc_session_property_read` / `path_excludes_cursor` / `wgc_cursor_property_unavailable` | on what evidence |

`wgc_session_property_set` means the switch was set for this request and the value read back matched it;
`wgc_session_property_read` means nothing was set (`--cursor default`) and only the current value was read;
`path_excludes_cursor` means this path's source pixels contain no pointer; `wgc_cursor_property_unavailable` means
that question gave no answer, and then `cursorEffective` is `unverified` rather than either answer.

`cursorEffective` stops at exactly that evidence. It says this session was set to draw the pointer, or that this
source holds none - it does **not** claim a pointer happened to be sitting over the target in these pixels. This
SDK's session interface has no read-only `IsCursorVisible`, so the tool makes no pixel-level assertion, and
`--capabilities` states that boundary as the `cursor_effective_is_a_setting_not_a_pixel_check` caveat. If an explicit
`include` / `exclude` request cannot be confirmed on a `wgc` capture (the interface cannot be obtained, the set call
fails, or the value read back is the opposite one), the code is `capture.cursor_unverifiable` (exit `7`) *before*
`StartCapture`, so a frame that contradicts the request is never delivered; under `--cursor default` the same
situation is reported as `unverified` instead of being folded into either answer.

Asking about the pointer changes nothing about authorization. The tier is still decided by where the pixels come
from, so `--cursor exclude` on `bitblt`, `duplication` or any whole-screen target still shows the dialog and `--yes`
still does not cover it; `wgc` without `--yes` still asks. Those three cases are checked on a real window in
`tests\cursor.ps1`. Like `path` / `scope` / `rect` and the crop fields, these three are locating evidence and
`--quiet` does not suppress them.

`--capabilities` answers all of the above without capturing anything, in its `cursor` section: the default value, the
three values, that single switch as `compiled` / `status` / `minBuild` / `verifiedOnThisMachine`, one row per
registered path with `capability` / `reason` / `include` / `exclude` (each of the last two `yes` / `no` / `unknown`),
and `pointerShapeCompositing: "never"` plus `pixelRetouching: "never"`.

## HDR color handling (`--hdr`)

When a display is in HDR mode the captured frame can carry luminance beyond SDR and a different transfer function. Interpreting such a frame as plain 8-bit BGRA yields a washed-out, desaturated image whose highlights are blown — and it "looks like a normal picture", which is exactly the result this tool refuses to treat as correct. `--hdr auto|tonemap|refuse` makes that a decision you state. The default `auto` means this tool changes **nothing** about color: it does not probe the display state, does not change the capture format, does not tone-map, and the color keys do not appear in the result — the output is byte-for-byte what it was before this option existed.

Which channels can bring back a wide-gamut frame is judged by the same rule as everything else: **where the pixels come from**, not the channel name. That registry lives in `src/HdrColor.h`, one row per `images[].path`:

- `wgc` / `screen.wgc` / `duplication.frame` / `screen.duplication` can (their source follows the display mode: WGC can return FP16 scRGB linear, and the Desktop Duplication surface can be FP16 scRGB or 10-bit ST.2084 (PQ) / HLG BT.2020).
- `printwindow` (the window self-draws into an 8-bit DC), `dwm.thumbnail` / `dwm.screen`, `bitblt.screen` / `screen.bitblt` can only bring back 8-bit SDR, so HDR handling has nothing to act on there — it is the identity, not "impossible so switch channels".

The three values:

- `tonemap` — map an HDR frame to SDR before delivering. This tool builds a **per-pixel float intermediate** before encoding (it never allocates a whole float frame, which would blow the 1 GiB per-frame budget by 4×) and applies a fixed curve: decode the transfer function (scRGB linear / PQ → absolute nits → relative linear / HLG inverse OETF) → BT.2020→BT.709 primaries matrix → **extended-Reinhard** luminance tone map (deterministic, monotonic; it degenerates to the identity when `white=1`) → sRGB encode → opaque alpha passed through. For an already-SDR source it is an identity passthrough.
- `refuse` — once the source is confirmed to be HDR, error out and write not a single pixel; never deliver a BGRA8-washed image.
- `auto` (default) — do not engage the path above; just report the source color space the frame actually came back as.

Two rules follow from that table, both sharing the `--cursor` principle:

- **If a channel cannot do it, refuse; never silently reroute.** `--hdr tonemap` / `refuse` with `printwindow` / `dwm` / `bitblt` is `capture.hdr_unsupported` at parse time (exit code `1`) and does **not** switch channels (a desktop-reading channel has no more HDR to map and would only capture a screen no one approved). `--capture auto` is not judged at parse time (which channel it lands on is a runtime fact, and the two wide-capable channels are in the chain).
- **Unrecognized is unrecognized.** A wide-gamut pixel format this build cannot name is `capture.hdr_unverifiable` (exit code `7`) — it is neither forced into BGRA8 nor "mapped by guessing"; `--hdr refuse` on a confirmed-HDR source is `capture.hdr_refused` (exit code `7`). All three are given before encoding and write nothing.

Whenever `--hdr` is written, each delivered image carries this group of fields (with the option absent none appear, byte-identical to before):

| Field | Values | What it says |
| --- | --- | --- |
| `hdrRequested` | `auto` / `tonemap` / `refuse` | which policy was asked |
| `hdrEffective` | `sdr_passthrough` / `tone_mapped` / `unverified` | what this frame actually went through |
| `hdrBasis` | `delivered_bgra8_sdr` / `scrgb_float_tone_mapped` / `pq_bt2020_tone_mapped` / `hlg_bt2020_tone_mapped` / `path_sdr_source` / `format_unrecognized` | what that conclusion rests on |
| `sourceColorSpace` | `srgb_bgra8` / `scrgb_float` / `pq_bt2020` / `hlg_bt2020` / `unknown` | the source before encoding |
| `sourceBitDepth` | `8` / `10` / `16` (omitted entirely when unrecognized) | bits per channel of the source |

When processing was explicitly asked (`tonemap` / `refuse`) but the frame's source turns out to be 8-bit SDR, the image is still delivered (mapping an SDR source is the identity) and a `note.hdr_source_sdr` is recorded, so "I asked for HDR handling" and "this frame had no HDR" stay distinguishable instead of a silent pass masquerading as "HDR was mapped". `--hdr auto` does not emit that note (it only reports passively).

HDR color does not change authorization: the whole step runs after capture and before encoding, and the rule is still "where do this path's pixels come from". Channels that read desktop pixels always prompt, `--yes` still does not cover them, and no "mapped, therefore pre-approved" bypass is introduced.

**This development machine's display cannot enable HDR**, so the end-to-end "bring back a real HDR frame and map it" cannot be produced here: the tone-mapping math is judged point-by-point offline with known color blocks and a brightness gradient (`tests\hdr_state.cpp`), `--capabilities` records `color.verifiedOnThisMachine` as `no` for that reason (the `hdr_tone_mapping_not_verified_on_hdr_display` caveat), and the HDR-device checks in `tests\hdr.ps1` are recorded as unverified. Before being re-checked on an HDR display, this tool does not claim color acceptance passed.

## Screenshot authorization and `--yes`

Any capture that really grabs a frame asks first, in a modal dialog — **the reliable window paths included**. What
never asks, because nothing is captured: giving no conditions (text help + `2`), `--help`, `--version`, `--dry-run`,
no match (`4`), several matches (`5`), parse errors (`1`) and output-plan failures such as `io.output_collision`
(`8`) — every output name is settled before the first question.

`--yes` (`-y`, a positive boolean: bare or `=true/1/yes/y/on` turns it on, `=false/0/no/n/off` turns it off, the
last occurrence wins, and `-v` echoes the result as `input.yes`) skips the ask for **one tier only** — the paths
whose frame is bound to the selected window itself and never sample desktop pixels. It guarantees nothing else: not
a valid image, not permissions, not protected content, not errors, not overwrite protection. What decides the tier
is the route actually taken, never the channel label:

| Route (`images[].path`) | Where the pixels come from | Without `--yes` | With `--yes` |
| --- | --- | --- | --- |
| `wgc`, `printwindow`, `dwm.thumbnail` | the selected window itself | asked once | not asked |
| `dwm.screen`, `bitblt.screen`, `duplication.frame` | the area of the screen behind/around that window | asked | **still asked** |
| `screen.wgc`, `screen.bitblt`, `screen.duplication` | a whole monitor | asked | **still asked** |

A route that is unknown or cannot be proven is treated as a desktop route, so a new channel that forgot to register
itself ends up stricter rather than looser. `--monitor` together with window conditions filters **windows**, so those
images follow the window rows above.

- **Nothing skips a desktop route**: not `--yes`, not `--quiet`, not an environment variable, not stdin, not who the
  caller is. That is the whole point of the tiering.
- One confirmation covers the batch of targets this request listed, so several backends or windows do not each
  re-prompt. Consent is never cached across requests, never widened to a target the dialog did not show, and
  approving a window-content capture is never approval of a desktop one: `--capture auto` with `--yes` may run the
  window routes silently, but asks before it enters a desktop route.
- After a refusal, a closed dialog, or an unavailable interactive desktop, the rest of that request stops — no
  fallback to another backend, no retry, no second ask — while every image already completed stays in `images`.
- If a target's area moves or the monitor topology changes, the authorization covering it is void and the tool asks
  again; a frame already bound to the old area gives `capture.consent_stale` (exit code `7`, retry by selecting the
  target again).
- The dialog puts the default focus on "No" and lists the targets with their areas, the route about to be taken, each
  expanded absolute output path (or "standard output"), and whether other windows can end up in the image; when the
  route reads the desktop it also says out loud that the `--yes` passed does not apply here. It closes before the
  frame is taken so it never appears in the picture, and after "Yes" the tool still waits about a second, because the
  close animation is still on the DWM screen.
- With no interactive desktop (service session, scheduled task, lock screen) a window-content capture with `--yes`
  goes ahead normally, while a desktop route can only be refused — it never proceeds just because the dialog could
  not be shown. A refusal gives `capture.access_denied` + `6`, an undisplayable dialog the distinct
  `capture.consent_unavailable` + `6`; both carry `stage=consent`, `target`, `backend` and the route in `value`.
- A refusal is reported as what it is — `capture.access_denied` (`stage=consent`) with exit code `6` — whether or not
  an output path was given. Omitting `--out` is `--out -`, so nothing about the authorization result depends on it;
  see [Omitting `--out` (compatibility note)](#omitting---out-compatibility-note).
- Several monitors plus stdout (`--monitor all --out -`) is refused before any dialog appears: one confirmation
  cannot buy "one image per monitor squeezed into the same stream". With only one monitor attached, `--monitor all`
  is a single target and that path still delivers one image on stdout.
- Honest limit: this is a plain `MessageBox`. It is mis-click protection for cooperative automation — it cannot tell
  whether a human or a script pressed the button, and it does not defend against a same-privilege process that
  intends to bypass it. What it does guarantee is that a caller following these rules gets asked at least once.

`--monitor` (value omitted) and `--monitor primary` are the main monitor, `--monitor 2` the second one,
`--monitor all` one image per monitor. The number is **the position in this run's `EnumDisplayMonitors` enumeration**,
starting at 1 — it is not the id Windows writes in Settings, and unplugging a monitor or changing a resolution can
reshuffle it, so do not store a number to identify a screen across runs. To name that monitor again, run `--screens` and write back what it reports (the
`\\.\DISPLAY1` name of this desktop attach: `--monitor device:DISPLAY1`, or the monitor devnode path, which is the one that survives a session change: `--monitor "id:\\?\DISPLAY#…"`). An identifier that matches nothing gives `match.monitor_unknown_id` (exit code 4), one that matches several gives `match.monitor_ambiguous_id` (exit code 5, every candidate listed - the tool will not pick one), and an identity question that returned no answer gives `match.monitor_id_unverifiable` (exit code 7) - none of them quietly falls back to the primary monitor. An out-of-range number still gives
`match.monitor_out_of_range` (exit code 1) with every local monitor listed in `hint`. Before a screen target is
captured, the tool re-checks that monitor by identity - by devnode path when that was known at selection time, by name only when it was not: if the monitor left the desktop, or that device name now belongs to a different panel, the capture stops with
`capture.monitor_changed` (a re-check that cannot be answered at all stops with `capture.monitor_unverifiable`), and if its rectangle or position changed, the new rectangle is what a person is asked to
approve — an old confirmation is never reused for a resized or relocated monitor. `--monitor <n>` together
with window conditions means "filter windows by monitor" (a window overlapping that monitor matches, and a window
spanning monitors matches on both), still producing window images, so the window rows of the table above apply.
`--monitor all` is mutually exclusive with any window **matching** condition (`cli.monitor_conflict`, exit code 1),
but disambiguation options such as `--all` and `--index` do not count as matching conditions and may accompany it.

## Target identity and handle reuse

The window that was picked may no longer be that window by the time pixels are actually read. Between matching and capturing sit the output-name planning, the consent dialog (somebody may take a few seconds, and there is a ~1 s close animation after the answer) and up to four channels in an `auto` fallback. In that span the target can be destroyed, some other window can take over its HWND value, and its PID can be recycled by a different process — and a 64-bit integer cannot tell "still the same window" apart from "a new object that looks like it".

So the identity is recorded at the moment of selection: the handle, the owning PID, that process's **creation time** (that is what separates "this PID was recycled" from "still the same process"), the window class, and the conditions that made this window a target. It is then re-checked in two grades:

- **Before every attempt on every channel** (and once more after the authorization, right before a pixel is read): is the handle still a window, does it still belong to that process, is that PID's process creation time unchanged, is the class the same. All four answers come from user32 / kernel32's own data structures and send no message to the target's thread, so they can be asked this often and cannot be stalled by a hung window here.
- **Once per target before it starts**, plus before anything that would switch to reading desktop pixels (the `dwm` screen fallback): **re-run the original conditions** and check this handle is still among the matches. Volatile properties such as the title are judged that way — an application refreshing its own title (playback position, a modified-document mark, a tab caption) is still the same target, while a window that no longer satisfies the `--title` condition it was picked by is not. That question enumerates windows, so it is not multiplied by the fallback chain; a `--title-regex` has no interruptible point, so this re-run keeps exactly the same isolation rule as the first evaluation (into the helper process whenever a deadline applies).

Failing the check reads no pixels at all and returns one of three stable codes:

| Code | Exit | Meaning | Next step |
| --- | --- | --- | --- |
| `capture.target_gone` | 7 | the handle was destroyed during this request | enumerate the windows again |
| `capture.target_changed` | 7 | that handle value now belongs to another object (or no longer satisfies the conditions) | select the target again — **the permission you gave is not transferred to a new object** |
| `capture.target_unverifiable` | 7 | one of the questions could not be answered (process information unreadable, the condition re-evaluation did not finish) | check the execution environment (rights, policy, antivirus), or raise `--timeout-ms` |

A changed identity never becomes a licence to "relax the conditions and grab a look-alike" — same rule as `--yes`: consent is bound to the object that was listed for the human.

**How far that goes:** this re-check narrows the race window, it does not claim to close it. Checking and capturing are not one atomic operation, an `HWND` is not a waitable object, and there is no public API that pins a window into existence. A change in the instant between the check and the frame is still possible — it just no longer has a whole planning-plus-consent span of time to happen in.

## Deadlines and calls that block (`--timeout-ms` / `--consent-timeout-ms`)

`--timeout-ms <ms>` is a **total** budget for the automatic part of the run, measured on a monotonic clock from
the moment targets start being selected. Window/screen matching (including `--title-regex`), the `auto` fallback
chain, frame waits, encoding and the final commit all spend **the same** budget: no step and no further target of
the batch gets a fresh copy, so four backends cannot each wait 2 seconds and two targets cannot each wait again.
Omitted or `0` means no overall budget; even then every isolated call is bounded by a built-in 5000 ms cap, which
is what the old `timeoutMs` argument should have done. When the budget runs out the affected image is **not**
written — `match.timeout` (`stage=match`) when the budget died while evaluating conditions, `capture.timeout`
(`stage=capture`, encoding included), `io.timeout` (`stage=write` / `stdout`, exit code `8`) — the remaining
targets of the batch are not started, and images already written stay in `images`. Partial batches therefore behave
exactly like partial capture failures: exit code non-zero, whatever already landed is still delivered.

Waiting for a person is a **separate** clock: `--consent-timeout-ms <ms>` bounds the confirmation dialog only and
never eats the automatic budget (somebody stepping away is not "the machine is slow"). If nobody answers in time
the request is **refused** — `capture.consent_timeout`, exit code `6` — and never treated as consent, and the
rest of the batch stops just like after an explicit "No". Omitted or `0` waits forever, as before. The ~1 second
buffer after "Yes", which keeps the dialog's close animation out of the picture, belongs to the human stage and is
never skipped to make a deadline: what is bounded is the waiting for an answer, not the settle time after one.

**Where the blocking actually goes.** `PrintWindow` hands the target window a draw request and waits for its
thread; `--capture printwindow` and the `dwm` read-back do exactly that, and there is no interrupt point inside
the call to check a deadline against. So does `std::regex`: a pattern such as `(a+)+$` against a long title can
backtrack for minutes, and a length limit on the pattern is not an execution deadline. Those calls now run in a
helper process of the same `ECAPTURE.EXE`, fed one already-parsed task over a private pipe; when the deadline
expires the parent stops **its own** helper process and reports the timeout. The target application's window is
never killed, and no worker can outlive the parent (a kill-on-close job object plus a broken-pipe check and an
idle watchdog). What that does **not** change: the helper only ever reads a single window's own picture or lists
top-level windows, it never samples desktop pixels and never writes files, so every desktop route still goes
through the authorization above — there is no `--worker` option, and nothing about `--yes` gets weaker.

Honest limits, stated as limits: the budget bites at interruptible points and by stopping the helper. The atomic
file write, a stdout pipe that somebody stopped draining, and a WinRT encoder that ignores the cancel request have
no cancellation point, so those are gated before they start and timed after they finish — not preempted
mid-call. And on Windows 10 19045 `PrintWindow(PW_RENDERFULLCONTENT)` renders from the DWM-cached surface without
ever sending `WM_PRINT`, so a window that hangs inside `WM_PRINT` does not stall the parent there; the fallback
`PrintWindow` call without that flag is the one that waits for the target's thread. Do not assume the wedged
scenario is reachable on every Windows build — assume only that this tool returns within its deadline.

## File name placeholders

Usable anywhere in the `--out` path; multiple images rely on them:

| Placeholder | Meaning |
| --- | --- |
| `%i` | ordinal, starting at 1 (`--all` windows, `--monitor all` screens) |
| `%h` | window handle, shaped like `0x001B0C48`; screen targets give 0 |
| `%p` | process id; screen targets give 0 |
| `%n` | window title, or the device name without the `\\.\` prefix for screen targets (e.g. `DISPLAY1`) — cleaned into a file-name fragment: characters illegal in a name become `_`, trailing dots and spaces are dropped, a result that is exactly a reserved device name (`CON`, `NUL`, `COM1`, `LPT1`, …) gets a `_` prefix, and it is cut to 80 UTF-16 units without splitting a surrogate pair |
| `%d` | local date `YYYYMMDD` |
| `%t` | local time `HHMMSS` |
| `%%` | one literal `%`; any other `%x` is kept verbatim |

When `--all` is used without a placeholder the tool appends `_1`, `_2`, … and emits
`note.all_without_placeholder`. A placeholder that cannot tell the targets apart (`%d` alone, or `%p` for two
windows of one process) is not renamed silently: the batch is planned up front and collides with `io.output_collision`.
`%d` and `%t` come from a clock read once per batch, so every image of one run carries the same date and time even if
the batch crosses midnight. Planned names are compared as absolute paths, case-insensitively by code point (the way
NTFS treats them); aliases that string comparison cannot see — 8.3 short names, hard links, junctions and symlinks,
UNC versus drive letters — are settled by the atomic commit instead, so a name the pre-check could not recognise as
occupied still cannot be silently replaced. `--out -` is not a path: no expansion, no extension, no collision check,
and since stdout delivers only one image per run, there is never a batch to write into that stream (see the output
rules above).

## Message language

`--lang` (`-l`) takes `zh-CN` / `zh-TW` / `en` / `ja`; omitted or `auto` uses the Windows display language, falling
back to `en` when that is not one of the four. Values are accepted generously: case-insensitive, `_` and `-` are
equivalent, `zh_TW` / `zh-Hant` / `cht` / `tw` map to Traditional Chinese, `chs` / `cn` / `zh-Hans` to Simplified,
`jp` to Japanese. An unknown value fails during parsing with `cli.unknown_language` (exit code 1) instead of
silently defaulting.

**Only human-readable text changes with the language**: `message` / `hint` of diagnostics and the whole `--help`.
`code`, JSON keys, value enums, `0x…` handles and `HRESULT` numbers never change, so callers can branch on `code`
alone. The strings are embedded resources (`resources/strings-<language>.txt` compiled as four `RCDATA` blocks), so
switching language works offline.

## Guide for AI and scripts

The tool is designed for programmatic calls; following these conventions is the cheapest way to use it. The
repository also ships a skill that teaches an agent to drive it: `.agents/skills/ecapture-screenshot/` (contains
`SKILL.md`, `references/cli-contract.md`, and a copy of the exe).

1. **Ask the capability question first, then discover windows with `--list` / `--inspect`, then capture for real.**
   `--capabilities` is read-only - no pixel taken, no consent dialog, no file written - so it never disturbs anybody
   and belongs at the front of an automation flow. It hands back this machine's version, session and screen topology,
   every route as `available` / `unavailable` / `unverified`, every format, exactly which internal paths `--yes`
   covers, and the value ceilings. That answers "which `--capture` should I ask for", "is this failure about the
   channel or about the machine" and "would a dialog here ever be answered" *before* you take an image. For a bug
   report run `--diagnostics` (same judgements, plus a checkable build id; `-v` expands each raw answer). Neither
   document changes with `--lang` (all ASCII), so comparisons stay stable. `available` is **not** "this window will
   capture": drivers, protected content and HDR are outside that layer, and the `caveats` array says so.
   Then `--dry-run` as usual: it takes no frame, writes no file and shows no dialog; candidates are in
   `notes[0].value`, shaped like `hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=…`.
   `--dry-run` does not need an output path either (with none it just means "nothing to deliver", so the JSON goes
   to stderr with `note.output_defaulted_stdout`); **`--dry-run` alone with no window condition = text help +
   exit code 2**.
   When you need the *list* rather than one line of prose, use `--list` (structured, paged, several matches are not
   an error, zero matches is an empty list + exit `0`) and `--inspect` (one window, and several matches stay an
   ambiguity - the tool will not pick one for you). Both read no pixel and open no dialog, and neither is changed by
   `--yes`. What they hand back is a snapshot: capture still re-checks the target identity, so pass the handle from a
   fresh `--inspect` rather than one cached from an earlier run. See the section
   《Structured window discovery and inspection》 above.
2. **Branch on `errors[].code`, never on `message` text** (that follows `--lang`) and never on whether you passed
   `--out` — the two ways of asking for stdout report the same codes. The codes you actually hit:
   `match.no_window` (4, conditions too narrow or the window is minimized), `match.ambiguous_window` (5, choose
   from the candidates in `hint`), `match.index_out_of_range` / `match.monitor_out_of_range` (1, `hint` lists all
   candidates), `match.monitor_unknown_id` (4, an identifier `--screens` reported earlier is not on the desktop
   now), `match.monitor_ambiguous_id` (5, several screens share that identifier - the tool will not pick one),
   `match.monitor_id_unverifiable` (7, the screen identity could not be read at all), plus
   `cli.monitor_selector_empty` / `cli.monitor_selector_kind` (1, `--monitor`'s identifier forms), and
   `capture.monitor_unverifiable` (7, the pre-capture identity re-check got no answer - it does not fall back to
   the name either). Naming a monitor by identifier comes from `--screens`; see that section above instead of
   guessing an ordinal,
   candidates), `cli.invalid_format` (1), `cli.stdout_multiple_targets` (1, several targets
   want to share one stdout), `capture.failed` (7), `capture.frame_timeout` (7, waiting for the frame ran out),
   `capture.window_gone` (7, the target is already gone — enumerate again), `capture.frame_invalid` (7, the frame's own
   memory layout does not add up — zero size, a side beyond 16384 px, or a row pitch / buffer that contradicts it),
   `capture.access_denied` (6, somebody answered "No"), `capture.consent_unavailable` (6, this session has no
   interactive desktop, so nobody could answer),
   `capture.consent_stale` (7, the target moved after consent — select it again and expect a fresh ask),
   `io.write_failed` (8, directory missing or the commit failed), `io.file_exists` (8, with `--no-overwrite`),
   `io.output_collision` (8, two targets expand to the same output name — nothing was captured).
   Two of those are about **this machine**, not about the target, and they are the ones where retrying the same
   window is pointless: `env.os_too_old` (7, the Windows build is below the one encoder every format uses —
   changing `--capture` changes nothing) and `env.channel_unsupported` (7, the channel that was asked for
   explicitly needs a newer build — another channel or `auto` is what can help, and the tool will not switch
   on its own). `note.channel_unavailable` says the same about one link of an `auto` chain that was dropped
   while the rest still captured. Read `input.osBuild` / `input.captureChain` with `--verbose` to ask which
   channels this machine offers before capturing anything. See
   [System support](#system-support) for the floors and for what has actually been tested.
   Every error also carries `target` / `backend` / `stage` / `hresult` / `win32` (see the output rules above) as far as
   that step really had them, so there is no need to dig values out of `message`.
3. **Read the right stream**: with `--out <file>` the JSON is on stdout and stderr is empty, so parse stdout
   directly. With `--out -` (or no output path) the image bytes occupy stdout and the whole JSON moves to stderr.
   stdout hands over one image at a time, so write several targets to files. In PowerShell 5.1 `2>&1` wraps native
   stderr into error records, so redirect `1>` and `2>` separately if you want both the image and the JSON.
4. **Do not treat a non-zero exit code as total failure**: on partial success `captured` is greater than 0 while the
   exit code is 7, the images already on disk are perfectly usable, and `images[].source` / `path` / `scope` name the
   channel, the route inside it, and whether that frame is the window's own pixels or desktop pixels.
5. **Exit code 0 does not mean the picture is correct**: protected content and some player drivers hand you black
   frames while reporting success. The tool now tells you when the whole image really is one colour — it compares
   every pixel against the top-left one (all four BGRA bytes, row padding excluded) and emits
   `note.frame_uniform` (with the colour as `0xAARRGGBB`) while still delivering the image. A single colour is a
   quality hint, not a failure: a solid window or a plain wallpaper looks exactly like that, so nothing is refused
   and no extra authorization is asked for it. Verify pixels to be sure — for instance put a solid-colour window on
   top of the target and capture again, then check whether you got the target's content or the cover; at minimum
   compare `width`/`height` against the target window rectangle.
6. **Expect a dialog before the first frame**: with no `--yes`, every real capture blocks until somebody answers,
   plain window captures included. Add `--yes` when the target is one window and the route stays a window-content one
   (`wgc` / `printwindow` / `dwm.thumbnail`); for `bitblt`, `duplication`, `dwm`'s screen fallback or any whole screen
   it changes nothing and a person has to answer — tell the user first and pass `--out` explicitly. Afterwards read
   `images[].scope`: `desktop` means other windows, open documents and notifications may be in that picture. If you
   only need one window, do not escalate to the whole screen.
7. To reliably target "some application", prefer `--process`/`--exe` plus `--class`; title matching is
   case-sensitive and unreliable across locales.

## Build and test

| Command | Purpose |
| --- | --- |
| `.\build.ps1` | Release build, output `build\ecapture.exe`; `-Config Debug` and `-Clean` available |
| `.\tests\cli.ps1` | 619 output-contract assertions (the `--yes` and `--no-overwrite` boolean forms, and the query-versus-capture conflict group included) + stream separation + "no `--out`" against `--out -` equivalence + multi-language checks (all `--dry-run`, no capture) |
| `.\tests\capabilities.ps1` | Capability and diagnostics queries (`--capabilities` / `--diagnostics`): offline runs `build\ecapture-capabilities-tests.exe` (fake probes for "no screen at all", "just under a channel's floor", "the build number could not be read", "one encoder is missing", "the `--yes` scope matches the registry", "both queries come from one set of judgements"); the real-machine layer proves the query never blocks on a dialog (the timeout is itself the assertion), writes no file, agrees with WMI and with `--dry-run -v` on version / architecture / chain, is all-ASCII so it cannot change with `--lang`, and carries no user name or path. A session with no interactive desktop, older builds, a genuinely missing encoder, ARM64 / Server / Remote Desktop cannot be arranged here and are recorded as unverified |
| `.\scripts\check-lang.ps1` | Verifies the four string tables align on keys/placeholders and that the exe really carries four resources |
| `.\tests\invoker.ps1` | Offline checks for the shared test process invoker: argv quoting, both streams at once, binary output, hung child, per-run scratch dirs (no capture) |
| `.\tests\build-path.ps1` | Build-path checks: offline layer (the temporary batch body must stay ASCII, VS environment import failures reported before cmake runs) + on-device layer (Release / Debug / RelWithDebInfo and `-Clean` built from a directory holding CJK text, spaces, parentheses and `%`, plus a CJK `%TEMP%`; no capture, `-OfflineOnly` skips the on-device layer) |
| `.\tests\image.ps1` | Frame checks: offline suite (135 checks) over hand-built pixel layouts (stripes, checkerboard, alpha, row padding, over-large / short buffers, out-of-range crops) plus on-device single-colour captures |
| `.\tests\dup.ps1` | Desktop Duplication multi-monitor checks: offline layer (`build\ecapture-dup-tests.exe`, from `tests\dup_state.cpp`) injects the four rotations against the production geometry judges — pixel judgements are taken against the test's own naive "rotate the whole frame first, then crop" — plus negative coordinates, cropped/clipped rectangles, a fake two-adapter output table (target on the second adapter, no outputs, detached) and the "that monitor changed after the confirmation" cases; on-device layer checks the dialog must appear (`--yes` cannot skip a desktop route), `requestedRect` / `capturedRect` / `clipped` / `rotation` against real images, a four-corner orientation probe per monitor, and that every monitor is reachable. Displays are never re-arranged or re-oriented: rotated-panel and hot-unplug judgements are recorded as SKIP ("not verified") when the machine does not offer that situation |
| `.\tests\compat.ps1` | System support checks. Offline layer (`build\ecapture-compat-tests.exe`, from `tests\compat_state.cpp`) injects fake Windows builds into the production capability judges: every floor on both sides, an explicitly requested channel that is gated out never being substituted, which link an `auto` chain loses, and no filtering at all when the build cannot be read. On-device layer (captures nothing): the probed build equals what WMI reports independently (so the probe is not version-helped), the echoed chain agrees with that build, `--dry-run` never fails on the environment gate, all four help texts carry the floors and the `env.*` codes, and the shipped binary really imports the Windows 8-era WinRT / job API sets that are why the load floor is what it is. Anything needing a second Windows version is recorded as unverified rather than inferred |
| `.\tests\smoke.ps1` | On-device smoke: capture its own test window → validate PNG size and pixel content |
| `.\tests\save.ps1` | On-device file saving and overwrite protection: every `--no-overwrite` boolean form against a real file, batch output-name planning + collision detection (`%p` / `%n` / `%d` / `%t` / `%%` / unknown `%x` / case / cleaning / truncation), atomic commit (locked target, target is a directory, missing directory, killed mid-run), concurrent `--no-overwrite` race |
| `.\tests\channels.ps1` | On-device channel comparison: six channels + occlusion control, against its own windows. The window-content channels run with `--yes` and fail if a dialog appears; `bitblt` / `duplication` sample the desktop, so their image judgements need `-SimulateConsent` and are recorded as SKIP ("not verified") without it |
| `.\tests\consent.ps1` | Consent tiers: an offline layer runs the whole `ConsentGate` state machine against an injected fake prompt (`build\ecapture-consent-tests.exe`, from `tests\consent_state.cpp`), and the on-device layer answers every dialog "No" to check which paths must ask, what a refusal reports (`code` / `stage` / `target` / `value`), that nothing lands on disk, and that `images[].path` / `scope` / `rect` are right. Never answers "Yes" on a human's behalf |
| `.\tests\isolation.ps1` | On-device resource isolation: a same-named process it did not start stays alive and is never the target, two concurrent runs don't cross, an aborted run cleans up only itself |
| `.\tests\identity.ps1` | Target identity and z-order selection. Offline layer (`build\ecapture-identity-tests.exe`, injected fake query layer): handle reused by another process, same PID but a different process, class changed, the selection condition no longer holding, every question that cannot be answered, and which questions each grade asks in what order. On-device layer (self-made windows only): healthy targets are never blocked, `capture.target_gone` when the target is destroyed mid-batch, `capture.target_changed` when a renamed window no longer satisfies the `--title` condition, a refreshed title that still satisfies it captures normally, and `--topmost-match` / `--bottommost-match` are judged against the current z-order (the window created first but living in the topmost band wins — exactly what a "most recently created" reading gets wrong). Handle and PID recycling cannot be staged on purpose without killing somebody's process, and the consent-dialog span needs `-SimulateConsent`; both are recorded as unverified, never faked |
| `.\tests\hdr.ps1` | HDR color handling (`--hdr`): an offline layer (`build\ecapture-hdr-tests.exe`, from `tests\hdr_state.cpp`) checks that the two registries cover the same set of paths, classifies DXGI formats and display color spaces (anything unrecognized is `unknown`, never guessed), verifies the half decode / transfer functions / tone-curve properties (black maps to black, monotonic, identity at `white=1`, never exceeds 1), drives `ConvertWideFrameToSdrBgra8` point-by-point with known color blocks and a brightness gradient, guards the source and the shape, and composes the result-key group plus `HdrRequestPossible`. The on-device layer uses only self-created windows with `--yes` (this machine is not HDR): the color keys are absent when `--hdr` is not written, `--hdr auto` honestly reports `srgb_bgra8` / `sdr_passthrough` with no note, `tonemap` / `refuse` on an SDR source are an identity passthrough that each leave a `note.hdr_source_sdr`, `--quiet` does not suppress the keys, and each image's size / dominant colour / colour-count match the not-writing-`--hdr` case (HDR handling did not distort an SDR picture). The `color` section of `--capabilities`: `verifiedOnThisMachine` is always `no`, per-path wide-gamut reach, and both queries agree. Real HDR captures, refusing on a real HDR frame, the FP16 frame pool delivering, and HLG on-device cannot be produced here and are recorded as not verified |
| `.\tests\cursor.ps1` | Mouse pointer (`--cursor`): an offline layer (`build\ecapture-cursor-tests.exe`, from `tests\cursor_state.cpp`) feeds fake Windows builds and fake channel chains into the production judgements - the per-path capability registry, the two registries describing the same set of paths, how the chain narrows under a cursor request (both sides of the 19041 line, and structure-only filtering when the build cannot be read), how `requested` / `effective` / `basis` are composed, and the parser called directly so "include with a route that cannot deliver it" is refused without capturing anything. A second offline guard reads `src/` and fails if any pointer-shape fetching, cursor drawing or pointer-moving call ever appears. The on-device layer only uses windows it created itself: the `wgc` switch really gets set and read back, all three requests produce a frame that is still this window (size plus signature colour), the three fields stay absent when `--cursor` was never written, `--quiet` does not suppress them, `auto` + `include` delivers from `wgc` and nothing else, refused combinations neither land nor ask a person, and asking about the pointer did not loosen authorization (the two screen-sampling routes still show the dialog, probed but never answered). Pixel-level "the pointer is or is not visible here", machines below 19041, and any desktop capture that needs a human to answer Yes are recorded as not verified |
| `.\tests\crop.ps1` | Window-internal cropping (`--roi` / `--client-area`): the offline layer (`build\ecapture-crop-tests.exe`, from `tests\crop_state.cpp`) injects delivered-image sizes, whether the image's screen origin was answerable and whether the client area was measured, then judges edge alignment, one-pixel overflow, zero width or height, 64-bit wrap-around, the side ceiling, a client area hanging outside the image, and negative-coordinate monitors. The on-device layer uses its own bordered window (WS_OVERLAPPEDWINDOW, so the window / client / visible-frame rectangles differ), cross-checks `cropRect` / `cropScreenRect` / `fullWidth` / `fullHeight` against three independent Win32 questions, compares pixel content against an uncropped capture, proves an over-large rectangle is refused before any dialog or file, that a resized target invalidates the same rectangle, and that a desktop-pixel route with a tiny `--roi` still pops the dialog even with `--yes` (the test only looks and never answers). Mixed DPI across monitors and the shrink-between-check-and-frame race cannot be staged here and are recorded as unverified |
| `.\tests\screen.ps1` | On-device whole-screen test: three screen channels (all desktop routes, so every one of them must ask) + red-block placement + negative control. Only `-SimulateConsent` answers the consent dialog, and only for a desktop dedicated to testing; without it the judgements that need an answer are recorded as SKIP |
| `.\tests\streams.ps1` | On-device stream and structured-result reliability: one image on stdout for a single target, a batch that resolves to several targets is refused, the judgement uses the number of targets actually hit, `--monitor all` to stdout is refused with no dialog shown, the diagnostic locator fields, images already captured when a batch fails halfway are kept, and a result that cannot reach the agreed stream gives exit code 8, plus the no-`--out` / `--out -` equivalence across success, no match, ambiguity, bad arguments, a backend failure, a refusal and a broken stdout (only its own windows are captured, the desktop-route case again needs `-SimulateConsent`) |
| `.\tests\window_shot.bat` | Human walkthrough: compile the test window helper → capture it with every channel (a person clicks the dialogs) → whole-screen step → open the screenshot folder → end just that PID |
| `.\scripts\mkreadme.ps1` | Regenerates the help block of all four READMEs from each language's `--help` output |

Every desktop test targets a window of its own: `tests\helper\ec_window.cs` compiles into the run's
scratch folder and the test keeps that PID and HWND, so nothing is ever found or killed by image name and
only the folder it created gets deleted. Those tests pass `--yes` when they need a window image, which is the
one case where no dialog appears at all; a desktop route is only exercised with `-SimulateConsent`, or by a person
clicking in `tests\window_shot.bat`. `tests\harness.psm1` holds the shared process invoker (argv
quoting, both streams drained concurrently, bounded wait, kills only its own process tree) and the scratch
directory / window helpers, and `tests\invoker.ps1` is what proves that invoker.

`build.ps1` uses vswhere to find Visual Studio and prefers its bundled cmake/ninja. Repository, build and
toolchain paths reach the temporary batch file only through the child process's environment block, so a checkout
sitting in a directory with Chinese characters, spaces, parentheses or `%` builds just as well as an ASCII one —
and a batch body containing anything but ASCII is rejected before it is written. That is what
`.\tests\build-path.ps1` proves. Builds must stay warning-free under `/W4`. When testing by hand in Git Bash,
run `export MSYS2_ARG_CONV_EXCL='*'` first — otherwise `/help` gets rewritten as a path and
`--out /tmp/x.png` turns into a mangled one.

## License

EvernightCapture is licensed under the [Mulan Permissive Software License v2 (Mulan PSL v2)](http://license.coscl.org.cn/MulanPSL2).
The complete bilingual (Chinese and English) text is in [LICENSE](LICENSE).

```
Copyright (c) 2025 KagurazakaYashi (KagurazakaMiyabi)
EvernightCapture is licensed under Mulan PSL v2.
```
