![EvernightCapture logo](resources/icon.ico)

# EvernightCapture

A command-line window screenshot tool: select windows by conditions, then save that window's pixels to an image file.

[English](README.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [日本語](README.ja-JP.md)

Built on Windows.Graphics.Capture plus four other acquisition channels (DWM thumbnail, `PrintWindow`, screen
`BitBlt`, DXGI desktop duplication). The entry point is `ECAPTURE.EXE` — one executable, statically linked CRT, no
VC++ runtime on the target machine. The output is program-friendly: everything except `--help`, `--version` and the
"no conditions given" case is JSON, exit codes are stable, and every diagnostic carries a stable `code`, so the tool
works just as well typed by hand as called from a script or an AI agent.

Current version **0.4.0**: every `--capture` value is implemented (`wgc` / `dwm` / `printwindow` / `bitblt` /
`duplication` / `auto`), and `--monitor` gives whole-screen capture plus "filter windows by monitor". An earlier
build also had a `magnification` channel; it was removed because Windows 11's `magnification.dll` no longer exports
`MagGetImage`, so the route had no off-screen read path, would have had to place a magnifier control window on the
desktop, and would only have produced what `bitblt` already produces. That reasoning is recorded in
`src/CliOptions.h`.

## Where to look

First time here: [Quick start](#quick-start) (install, first capture, always pass `--out <file>`) →
[Read-only discovery](#structured-window-discovery-and-inspection---list----inspect) (find the window before
capturing it) → [Screenshot authorization and `--yes`](#screenshot-authorization-and---yes) (what a person has to
confirm) → [Exit codes](#exit-codes) (what went wrong and what to do).

| Question                                                                                          | Section                                                                                                                                                                                                                                                                                                                  |
| ------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| First capture, and why `--out <file>` is the reliable default                                     | [Quick start](#quick-start), [Omitting `--out` (compatibility note)](#omitting---out-compatibility-note)                                                                                                                                                                                                                 |
| Find a window or a screen without taking a pixel                                                  | [Structured window discovery and inspection (`--list` / `--inspect`)](#structured-window-discovery-and-inspection---list----inspect), [Read-only screen enumeration (`--screens`)](#read-only-screen-enumeration---screens), [Read-only capability queries](#read-only-capability-queries---capabilities----diagnostics) |
| What a person must confirm, and what no switch can skip                                           | [Screenshot authorization and `--yes`](#screenshot-authorization-and---yes)                                                                                                                                                                                                                                              |
| Which stream carries the image bytes, which carries the JSON, and what a given shell can preserve | [Output format](#output-format), [Writing image bytes to stdout, per shell](#writing-image-bytes-to-stdout-per-shell)                                                                                                                                                                                                    |
| How the deadline budget is shared and what it cannot interrupt                                    | [Deadlines and calls that block](#deadlines-and-calls-that-block---timeout-ms----consent-timeout-ms)                                                                                                                                                                                                                     |
| Stable codes, stages, exit codes, partial success, "what do I do when it errors"                  | [Exit codes](#exit-codes), [Guide for AI and scripts](#guide-for-ai-and-scripts)                                                                                                                                                                                                                                         |
| What is compiled, what is available here, what has actually been tested                           | [System support](#system-support), [Read-only capability queries](#read-only-capability-queries---capabilities----diagnostics)                                                                                                                                                                                           |
| Why capture from the skill directory fails with `0x80070005` / error `5`, and what actually helps | [Process integrity level](#process-integrity-level)                                                                                                                                                                                                                                                                      |
| Open questions this repository has not settled on real hardware                                   | [Boundaries and unverified items](#boundaries-and-unverified-items)                                                                                                                                                                                                                                                      |
| Which test proves which rule                                                                      | [Build and test](#build-and-test)                                                                                                                                                                                                                                                                                        |

## Features

- **Select windows by condition**: handle / process id / image name / full path / title (exact, contains, regex) /
  window class — different options AND together, repeating one option ORs it
- **Six capture channels**: capture a window that is covered by something else (`wgc` / `dwm` / `printwindow`), or
  deliberately copy only the pixels visible on screen (`bitblt` / `duplication`)
- **Many windows at once**: `--all` saves one image per matched window, named with placeholders like `%i`
- **Cropping inside the window**: `--roi x,y,w,h` keeps one rectangle of the delivered whole-window image, and
  `--client-area` keeps only the client area. Those coordinates belong to **that image's own pixels** (origin
  `(0,0)`, physical pixels, never DPI-scaled) — never re-read as desktop-absolute coordinates — and a rectangle
  that does not fit is refused, never slid inside or clipped to the edge. Full rule set:
  [Cropping inside the window](#cropping-inside-the-window---roi----client-area)
- **Shrinking the delivered image**: `--scale max-width=N,max-height=N,max-pixels=N` keeps the image inside those
  ceilings proportionally through one single ratio (the tightest one), rounding each side down and **never
  upscaling**, by nearest neighbour alone. Cropping happens first, so the ceilings are applied to the cropped
  block. Full rule set: [Scaling the delivered image down](#scaling-the-delivered-image-down---scale)
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
- **A history copy of every image that succeeded**: on by default, written into `history\<local date>\` **beside the
  exe that is actually running**, from the very bytes the primary delivery already encoded (no re-capture, no
  re-encode, no reading the primary output file back, no hard link), committed exclusively so an existing history
  file is never overwritten. The primary image delivered while the copy did not land is a partial success (exit code
  `7`, `images[].history` says which kind), nothing is rotated or deleted automatically, and pruning is the user's own
  explicit job. Full rule set:
  [Screenshot history archive](#screenshot-history-archive-on-by-default)
- **Windows you can list and inspect without capturing**: `--list` returns the matched windows as structured JSON
  (handle, PID, class, title, image name, physical rectangle, visibility / minimized, z-order, and the identity
  fields a later capture re-checks) with paging instead of an ambiguity error; `--inspect` describes one window and
  reports several matches as an ambiguity instead of picking one. Neither takes a pixel, shows a dialog, writes a
  file or touches a window, `--yes` changes nothing there, and the answer is an explicitly-labelled snapshot
- **Capabilities you can ask about read-only**: `--capabilities` / `--diagnostics` report which routes this machine
  can take, how far `--yes` really reaches, and a checkable build id — without taking a pixel, showing a dialog,
  writing a file or talking to the network. "compiled into this build", "usable here right now" and "this project has
  run its on-device tests in an environment like this one" stay three separate fields, an unanswered question is
  reported as `unknown`, and capability is never probed by capturing or encoding something

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

# Image bytes on stdout, JSON on stderr. Safe from cmd and from PowerShell 7.4+; Windows PowerShell 5.1
# corrupts both, so prefer --out <file> there (see "Writing image bytes to stdout, per shell")
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
  --title-regex, -R <regex>                   Window title regular-expression match (ECMAScript), validated at match time
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
  --cursor <default|include|exclude>          Whether the mouse pointer belongs in the image: default (nothing is changed and the three cursor keys stay out of the result) / include / exclude. Only wgc has a switch that can be set and read back (needs build 19041+); printwindow / dwm / bitblt come from pixels that hold no pointer, while duplication returns a desktop frame that may already have the pointer drawn into it and has no switch, so include with those four and exclude with duplication are refused as capture.cursor_unsupported (two wordings) and --cursor default delivers the frame with cursorEffective unverified. It never falls back to a screen-pixel channel; with auto those paths are dropped, each leaving a note.cursor_channel_skipped. This does not change consent; the requested / effective / basis rules are in the README
  --hdr <auto|tonemap|refuse>                 How to treat an HDR source: auto (default; changes nothing about color. Unwritten, the color keys do not appear) / tonemap (map an HDR frame to 8-bit SDR with a fixed tone curve before delivering) / refuse (if the source is confirmed HDR, error out and never deliver a BGRA8-washed image). In this build only wgc can fulfill tonemap/refuse, so asking for one with printwindow / dwm / bitblt / duplication reports capture.hdr_unsupported at parse time and never reroutes to a channel that reads desktop pixels; with --capture auto the same judgment narrows the fallback chain to the channels that really fulfill it (note.hdr_channel_skipped per channel dropped, env.hdr_unsupported if none is left). A capture that comes back capture.hdr_refused stops the chain instead of being retried on another backend. This does not change authorization; the source color space, bit depth and the processing actually applied are written into the result (see README and the color section of --capabilities)

Cropping inside the window and proportional shrinking (another crop of the delivered whole-window image, in that image's own pixel coordinates - not desktop coordinates; the order is crop first and then scale, and the two crop options are mutually exclusive)
  --roi <x,y,w,h>                             Cut a w x h block starting at x,y out of the delivered whole-window image. The origin (0,0) is this image's own top-left pixel (the image is the visible window frame you actually see; the transparent DWM resize border is not in it), in physical pixels and not scaled by DPI (the process is per-monitor v2, so multiply the scale yourself for logical pixels) - which is why these four numbers are never read as desktop-absolute coordinates. Four decimal integers separated by commas; x and y may be 0, w and h are at least 1, none above 16384. If it does not fit, nothing is written: match.roi_out_of_range when that is already clear before the frame is taken (no dialog, no file) and capture.roi_invalid when it only turns out afterwards - the rectangle is never slid inside, never cropped to the edge, and the uncropped window is never handed over instead. The crop runs after the capture, so it changes nothing about authorization: channels that sample the screen still always ask, and --yes does not start applying because only a small piece is kept. In the result cropRect is in image pixels, cropScreenRect is the same rectangle in screen coordinates (written only when the image's screen origin can be established), fullWidth/fullHeight are the size before cropping and width/height after. Mutually exclusive with --client-area, and meaningless for a whole-screen target (capture.unsupported)
  --client-area                               Keep only the window's client area: also drop the title bar and the three borders from the delivered whole-window image. That rectangle is measured from the target's geometry right now (GetClientRect plus ClientToScreen), so the coordinate system and units are exactly the --roi ones. A client area that cannot be measured reports capture.roi_unmeasurable, one that hangs outside the delivered image (off screen, or the window resized in between) reports capture.roi_invalid; neither falls back to the whole window. Mutually exclusive with --roi
  --scale <key=N>                             Shrink the delivered image proportionally into the given ceilings: max-width=N limits the width, max-height=N the height, max-pixels=N the total pixel count (all decimal; sides 1..16384, pixel count 1..268435456). The three are independent - give one, or several in one comma-separated token (for example max-width=1920,max-pixels=2073600); repeating this option keeps each ceiling separately and the last value written for the same ceiling wins. All three are applied through one single ratio (the tightest one), and width and height are each rounded down; it never upscales, so an image already inside the ceilings is delivered untouched (scaleApplied=false in the result). There is exactly one interpolation strategy, a predictable integer mapping: nearest neighbour. The order is crop (--roi / --client-area) first, then scale, then encode, so the scaling applies to the cropped block; in the result scaleFromWidth/scaleFromHeight are the size before scaling and width/height after, scaleMethod is the strategy, and the mapping closes in that same order. This option changes nothing about authorization or the frame limits: channels that sample the screen still always ask (--yes does not start applying because a small image is delivered in the end), a frame too large for the frame shape check cannot be scaled back down, and --roi is still judged against the unscaled image

Capture authorization (a real capture asks first; --yes skips window-content paths)
  --yes, -y                                   Skip the confirmation for window-content paths (wgc / printwindow / the dwm thumbnail route). Anything reading the screen (bitblt, duplication, a whole screen, dwm screen fallback) always asks; --yes cannot skip it. --yes=false asks on purpose

Deadlines (a total budget for the automatic stage; waiting for consent is timed separately)
  --timeout-ms <ms>                           Total budget in milliseconds for the automatic stage: from target selection on, matching, backend retries, frame capture, encoding and writing share this one remaining budget and no step gets a fresh copy. Omitted or 0 = no overall budget, and every isolated call is then still bounded by the built-in 5000 ms limit. Waiting for your consent is not counted here - see --consent-timeout-ms. When the budget runs out, a step that has not started yet is refused and its image is not written (a file that already finished committing is never rolled back); you get match.timeout / capture.timeout / io.timeout per stage
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
Current build: every --capture value is implemented (wgc / dwm / printwindow / bitblt / duplication, auto falls back wgc-dwm-printwindow-bitblt; a whole screen uses wgc-duplication-bitblt); the output directory must already exist; every delivered image also keeps a copy under history next to the program (never pruned automatically)
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
- **A question that could not be answered never becomes a match.** A window whose title or process information the
  system refuses to hand over (`denied` / `failed`) simply does not satisfy the conditions that need it, and a failed
  `--title-regex` discards every hit it had collected and reports the failure instead of returning a half-evaluated
  list. The regex is compiled and run **at match time** (not while parsing), inside the isolated helper process, and
  the three ways it can fail are kept distinct:
  - **syntax error** (a pattern that will not compile) → `cli.invalid_regex`, exit code `1`, `stage=match`;
  - **complexity / resource limit** (a pattern like `(a+)+$` hitting MSVC's backtracking `error_complexity` on a long
    title) → the same `cli.invalid_regex` code and exit code `1`, but a "too complex" message whose hint says plainly
    that **raising `--timeout-ms` does not help** — this is a bounded resource stop, not a slow-but-usable answer;
  - **budget overrun** (evaluation spent the `--timeout-ms`) → `match.timeout`, exit code `7`; and if the helper that
    ran it did not come back at all, `capture.worker_failed`, exit code `7`.
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
      "elapsedMs": 156,
      "history": {
        "status": "saved",
        "file": "D:\\shots\\history\\2026-10-10\\20261010-113122-31468-1a2b3c4d5e6f-1.png"
      }
    }
  ]
}
```

This image also carries the `history` member: where that independent copy, in addition to the primary delivery,
ended up (on by default, located beside the directory of the program that actually ran; rules and code meanings in
[Screenshot history archive](#screenshot-history-archive-on-by-default)). It is the verdict about the **second
delivery** and cannot change the `file`, `bytes` or `captured` above: when the copy fails, that image still counts as
delivered while the exit code becomes the partial-success `7`.

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
`dwm`'s screen route) additionally report _where_ in the desktop they actually got those pixels:
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
   finding it there means not getting it. Whether a given shell can carry those image bytes without damaging them is
   a separate question, answered in [Writing image bytes to stdout, per shell](#writing-image-bytes-to-stdout-per-shell).
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
   before any consent dialog). Two targets resolving to the same name give `io.output_collision` (exit code 8,
   `stage=plan`) with nothing captured and nothing written — the tool never renames behind your back and never lets
   image 2 overwrite image 1. Each file is then written to a unique temporary file in the target directory and only
   renamed onto the target once everything is written and flushed, so a failed write leaves the previous file
   untouched; with `--no-overwrite` that final rename is itself the "already exists?" check (`io.file_exists`), never
   a pre-check. **Format and file name**: `--format` takes `png` / `jpg` / `jpeg` / `bmp` / `tiff` / `gif` and wins
   over the file extension; with no `--format` the extension decides (`.tif` and `.tiff` both mean TIFF) and anything
   unrecognized falls back to PNG. A name with **no** extension gets the encoding's own extension appended
   (`.png` / `.jpg` / `.bmp` / `.tif` / `.gif`) plus one `note.output_extension_appended` per batch; a name that
   already has one is never rewritten, so `--out shot.png --format jpeg` really writes JPEG bytes into `shot.png`.
   `webp` and `ico` are not accepted (`cli.invalid_format`): this SDK has no encoder for them, and `--capabilities`
   reports them as `compiled: false` rather than leaving a caller to guess.
8. Every step's failure diagnostic also carries its own coordinates, present only when that step really obtained the
   value: `target` (which target — a `0x…` handle for a window, a device name such as `DISPLAY1` for a monitor),
   `backend` (which channel), `stage` (one of `parse` / `match` / `plan` / `consent` / `capture` / `encode` / `write`
   / `stdout` / `report` — append-only like the codes; `match` for `match.*`, `plan` for the batch-name judgements
   `io.output_collision` and `cli.stdout_multiple_targets`, `encode` for anything that happened while the frame was
   being encoded; command-line parse errors carry no `stage` at all), `hresult` (a raw value shaped like
   `0x80070005`), `win32` (the raw `GetLastError()` number).
   `message` follows `--lang` while these never do. Consent failures are their own branch:
   `capture.access_denied` means the answer was not Yes (on this `MB_YESNO` box the only refusal a person can give is
   "No", and the tool treats any non-Yes result as a refusal), `capture.consent_unavailable` means
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
   _and_ the whole frame one colour.

### Writing image bytes to stdout, per shell

`--out -` asks for two streams at once: PNG/JPEG bytes on stdout, the whole JSON document on stderr. The tool itself
does not translate either one — image bytes go out through `WriteFile` on the raw handle and text never touches that
stream — so a redirect that damages the image is corrupting it downstream of the tool, in the shell. The two streams
are also **not** the same kind of data: the JSON is text that a shell may re-encode harmlessly, while the image is a
byte stream where one substituted byte ruins the file. Whether a given shell preserves native byte streams is
therefore the first question (a file that will not decode can still have other causes — a truncated write, a
half-open pipe — so confirm the redirect before blaming it):

| Shell                    | Redirecting a native command's stdout to a file                                                                                                                                                                                                                                    | What to do                                                                                                                                            |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `cmd.exe`                | byte-exact (`1>` / `2>` rewire the real file handles)                                                                                                                                                                                                                              | safe as written                                                                                                                                       |
| PowerShell 7.4 and later | byte-exact — 7.4 changed the redirection operators to keep the byte stream of a native command's stdout                                                                                                                                                                            | safe as written                                                                                                                                       |
| PowerShell 7.0 – 7.3     | stdout is decoded through the text pipeline                                                                                                                                                                                                                                        | use `cmd /c`, or `--out <file>`                                                                                                                       |
| Windows PowerShell 5.1   | **corrupts it**: the bytes are decoded as text and rewritten as UTF-16LE, so NUL and every byte ≥ 0x80 are already lost before the file is written; the redirected stderr file gets PowerShell's own error-record rendering (a `node : `-style prefix, the script position, a BOM) | use `--out <file>`, or `cmd /c`, or `Start-Process -RedirectStandardOutput … -RedirectStandardError …` (byte-exact, because the OS wires the handles) |

```cmd
:: cmd.exe: image on stdout, JSON on stderr, both byte-for-byte
ECAPTURE.EXE --process notepad.exe --out - 1> D:\shots\snap.png 2> D:\shots\result.json
```

```powershell
# PowerShell 7.4+ only: these two lines keep the byte stream intact.
ECAPTURE.EXE --process notepad.exe --out - 1> D:\shots\snap.png 2> D:\shots\result.json

# In Windows PowerShell 5.1 the same two lines do NOT: native stdout is decoded as text and rewritten as UTF-16LE,
# so NUL and every byte >= 0x80 are already gone before the file is written. You can reproduce the mechanism with a
# fixed, non-sensitive byte stream instead of a screenshot — redirect `ECAPTURE.EXE --version` and compare: under cmd
# the file is the exact ASCII bytes (`1>` rewires the handle), under 5.1 the same output comes back roughly double
# the length and UTF-16LE, which no PNG decoder would accept either. So for 5.1 ask for a file, or use Start-Process
# (the OS wires the handles, byte-exact):
$p = Start-Process -FilePath 'D:\tools\ECAPTURE.EXE' `
  -ArgumentList '--process','notepad.exe','--yes','--out','-' `
  -NoNewWindow -Wait -PassThru `
  -RedirectStandardOutput 'D:\shots\snap.png' -RedirectStandardError 'D:\shots\result.json'
$p.ExitCode   # read the exit code here; -Wait alone does not surface it
```

`2>&1` (or `*>`) is never the answer on any of them: merging the two streams makes the shell treat the result as
string data, and the image bytes are gone. And on 5.1 the error-record rendering of native stderr happens on the way
into the pipeline, which is why separating `1>` and `2>` there does not rescue the JSON either. If you want both the
picture and the JSON, keep them in two files — or ask for a file and let the JSON stay on stdout, which is the
default and the route this tool is built around.

## Cropping inside the window (`--roi` / `--client-area`)

Both options answer a single question: _which part of the delivered window image do you actually want_. They are
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

| Situation                                                                                                                                   | Code                       | Exit | Stage   |
| ------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------- | ---- | ------- |
| Four fields that are not what was promised (sign, space, wrong field count, zero width or height, over the ceiling)                         | `cli.invalid_value`        | 1    | parse   |
| The rectangle is already too large for the window as selected, so before any dialog and before any file is planned                          | `match.roi_out_of_range`   | 1    | match   |
| The frame came back too small to hold it (the target resized in between, or part of it hangs off the screen)                                | `capture.roi_invalid`      | 7    | capture |
| The question needed to locate the rectangle gave no answer (client area unreadable, or this image cannot be tied to a region of the screen) | `capture.roi_unmeasurable` | 7    | capture |

None of these clamps the rectangle to the edge, slides it inside, or falls back to "here is the whole window
instead" — the last one would hand over an image the caller did not ask for. And none of them lands a pixel: the
pre-capture check runs before the consent dialog, so a request that cannot be honoured never disturbs a person,
and the post-capture one throws the frame away instead of writing it. A batch is a batch: if one of several
matched windows cannot hold the rectangle, nothing in that batch is captured (same rule as
`match.index_out_of_range`). `--dry-run` takes no frame, so it does not judge the geometry either — the requested
crop is echoed under `-v` as `input.crop` either way.

### The crop does not widen or narrow what a person approved

The crop runs **after** the frame is captured, so it cannot reach pixels that were never on the table: the tier a
request belongs to is still decided by `images[].path` alone (see
[Screenshot authorization and `--yes`](#screenshot-authorization-and---yes)) — a desktop-sampling route with
`--roi 0,0,8,8` asks exactly like a full-screen grab does, and `--yes` still covers only the three window-content
paths.

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
overlap: the first says whether the _whole window_ could be sampled from the desktop, the second says which part
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

## Scaling the delivered image down (`--scale`)

`--scale max-width=N,max-height=N,max-pixels=N` shrinks the image the tool is about to deliver so that it fits inside
those ceilings. The three keys are independent (give one, give several in one comma-separated token, or repeat this option -
each ceiling is kept separately and the last value written for the same ceiling wins); writing `--scale` with no ceiling at
all is an argument error, because "no ceiling" and "shrink to zero" cannot be told apart. N is decimal only, sides are
`1..16384` (the same line as the frame side ceiling) and the pixel count is `1..268435456`.

The rules are short, and they are the whole contract:

- **One ratio, the tightest ceiling.** Each ceiling proposes a ratio (width, height, or the square root for a pixel budget)
  and the smallest one wins. Width and height are each **rounded down** and each keeps at least one pixel.
- **It never upscales.** An image already inside every ceiling is delivered untouched and the result says
  `scaleApplied: false`. Nothing is _reduced_ by this option unless a ceiling is actually tighter than the image.
- **One interpolation strategy, and it is predictable**: nearest neighbour (`scaleMethod` is always `nearest`). Delivered
  pixel `(x,y)` comes from `(floor(x*scaleFromWidth/width), floor(y*scaleFromHeight/height))` of the image before scaling.
  There is no floating-point sampling and no per-channel choice of algorithm.
- **Crop first, then scale, then encode.** `scaleFromWidth` / `scaleFromHeight` are the size of the **cropped** image (not of
  the whole window), `cropRect` / `cropScreenRect` are unchanged, and `width` / `height` are the final size. The uniform-colour
  quality note is judged on the image that is delivered (that is, the scaled one).
- **Nothing about authorization moves.** Scaling runs after consent and is not a way to lower the risk of a request:
  the tier still comes from `images[].path` alone, so a desktop-pixel path with `--scale max-width=8` still pops the
  dialog even with `--yes`. It is also not a way around existing limits - a frame too large for the frame-shape check
  never reaches this step, and `--roi` is still judged against the unscaled image.
- **Only when this option is written** do `scaleMethod` / `scaleApplied` / `scaleFromWidth` / `scaleFromHeight` appear; without
  `--scale` those keys are absent (not `null`, `0` or `false`), so that flow is byte-for-byte what it was before.
- The encoding step is untouched: `.\tests\scale.ps1` checks on the device that `png`, `bmp` and `jpeg` all write the
  scaled size. `tiff` and `gif` reach the encoder through the same call with a different encoder id and are not part
  of that test.

Not verified on this machine: the on-device case of a frame whose side exceeds 16384 (no window that large can be staged, and
such a frame would not pass the frame-shape check anyway - the offline layer judges that rule), HDR combined with scaling
(this development machine cannot enable HDR), and mixed DPI across monitors (only one monitor is attached).

## Omitting `--out` (compatibility note)

Giving no output path is **the same request as `--out -`**: PNG bytes on stdout, the whole JSON on stderr, and every
diagnostic the one that step really produced.

- The real code and the real exit code come back unchanged on that route: `match.no_window` (4),
  `match.ambiguous_window` (5), `capture.access_denied` (6), `capture.failed` (7), `io.write_failed` (8),
  `cli.invalid_number` (1), … An earlier build collapsed all of them into `cli.missing_output` + exit code `1` and
  cleared `images` / `notes`, which hid the reason and threw away images already delivered; that rewrite is gone.
  `cli.missing_output` is no longer produced, and the code stays reserved so nothing else can take its meaning.
- Images already delivered on stdout stay in `images` and `captured` counts them, so partial success is visible on
  this route too.
- "You did not give an output path" is now a `hint`, and only where naming a file would actually have avoided the
  failure: it accompanies `io.write_failed` + `stage=stdout` on the implicit route. `--out -` means the pipe was
  chosen on purpose, so it goes unsaid there.

How to branch: read `errors[].code` (and its `stage` / `target` / `backend` / `hresult` / `win32`), never the exit
code alone and never whether `--out` was present.

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
**A failed history copy is exactly this kind of partial success**: the image has been delivered to `--out`,
`images[].history.status` reads `failed` (or `skipped`) and `errors` gained a `history.*` entry (`stage` = `history`)
— then the exit code is `7`, not `0`, and never the "nothing was written" `8`: the primary image is not deleted, the
stdout already emitted is not rolled back, and the caller should not re-capture because of it (see
[Screenshot history archive](#screenshot-history-archive-on-by-default)).

Failure **classes** stay distinct — a verdict about identity, policy or the execution environment is never folded into
a generic "capture failed", because the correct next step differs:

- **Recoverable backend failure** (`capture.frame_timeout`, `capture.window_gone`, `capture.frame_invalid`,
  `capture.failed`): exit `7`, the `auto` chain may try the next backend — the next step is another backend or a
  re-selected target.
- **Target/monitor identity verdict** (`capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable`,
  `capture.monitor_changed` / `capture.monitor_unverifiable`): exit `7`, but the chain does **not** retry a different
  backend or substitute another object/screen — the permission was bound to what was listed. The next step is to
  enumerate again and re-confirm, never to fall back.
- **Authorization verdict**: "No" / consent timeout / no dialog → `capture.access_denied` /
  `capture.consent_timeout` / `capture.consent_unavailable`, exit `6`, whole request stops; a target that moved after
  the answer → `capture.consent_stale`, exit `7`, retry by selecting it again.
- **Regex verdicts**: syntax / too-complex → `cli.invalid_regex`, exit `1` (more time does not help a complexity
  stop); budget overrun during evaluation → `match.timeout`, exit `7`.
- **Isolation mechanism failure** (`capture.worker_failed`): exit `7`, and its hint says to check the machine
  (rights, policy, antivirus) — the helper that runs regex / `PrintWindow` / DWM read-back did not behave, which is
  an environment problem, **not** the same thing as a recoverable `capture.failed` on the target.

The three read-only environment queries use only `0` and `1`: `0` = the document was delivered, even when it says this machine is
too old and no route is available (**a successful query and a possible capture are two different things** - branch
on `status`, do not infer the environment from an exit code); `1` = that invocation does not fit the contract
(`cli.query_conflict`, see [System support](#system-support)).
They never produce `4`/`5`/`6`/`7`/`8`: no window was enumerated, no dialog was shown, no file was written.

The two read-only window queries (`--list` / `--inspect`) share `0` and `1`, and additionally use `4`
(`match.no_window`, only for `--inspect`, which needs one target) and `5` (`match.ambiguous_window`, several
windows survive the selection policy), plus **`7` on exactly one path**: this run's _condition evaluation did not
finish_ (`match.timeout` - the `--timeout-ms` budget was spent on regex backtracking or on fetching a title from a
hung window, or that step's helper process itself failed). That `7` means "this question could not be answered",
not "the capture failed", so its `hint` is written in query terms and says plainly that switching `--capture` does
nothing - there is no channel to switch on this path. **`6` and `8` never appear**: no dialog was shown and no file
was written, and those two codes are exactly about those two things. For `--list` the exit code is `0` even when
nothing matched - an empty list is the answer.

## System support

Three different numbers must not be blended into one slogan:

| Layer                         | Value                                                                                                      | Where it comes from                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ----------------------------- | ---------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Per-route API history floor   | any image at all 10.0.10240 · `duplication` 10.0.9200 · `printwindow` / `dwm` 10.0.9600 · `wgc` 10.0.18362 | what Microsoft documents for the exact call that route makes. The encoder (WinRT `BitmapEncoder`) is shared by every channel and every format; the WGC route goes through `IGraphicsCaptureItemInterop::CreateForWindow` / `CreateForMonitor`, which arrived with Windows 10 version 1903 — even though the `Windows.Graphics.Capture` namespace itself appeared in 1803, and this tool has no "let the user pick a window in the system picker" path to fall back on |
| What this tool declares       | 64-bit Windows 10 version 1903 (build 18362) or later                                                      | the highest of those floors, because that is what the default channel needs in order to deliver a window image — not the oldest API some route touches                                                                                                                                                                                                                                                                                                                |
| What has actually been tested | Windows 10 version 22H2 (build 19045), x64                                                                 | the one machine every on-device judgement under `tests\` runs on. `--capabilities` compares this machine against that recorded baseline and calls the answer `verifiedOnThisMachine`; matching it is an environment match, not proof that this particular device was ever tested                                                                                                                                                                                      |

Builds between 10240 and 18361 can load this exe, and can capture: the tool gates each route by its own
floor instead of refusing the whole program there. They are not declared support and have never been
measured, so treat them as "expected to work, unverified".

Nothing here claims anything about Windows 7 or 8 — **this binary cannot load there at all**. It statically
imports the `api-ms-win-core-winrt-error-l1-1-1` API-set contract (whose documented minimum client is
Windows 8.1) plus `api-ms-win-core-winrt-l1-1-0` and `api-ms-win-core-job-l2-1-0` (Windows 8), and the
API-set mechanism does not exist on Windows 7; none of those three contracts is in the set the UCRT
redistributable installs, so they cannot be added to an older system. `.\tests\compat.ps1` checks those
names against the shipped binary, so the load floor is a fact about this file rather than an inference.
Windows 8.1 _can_ load and start it - and there the capability check below is what answers, because
`Windows.Graphics.Imaging.BitmapEncoder` does not exist before Windows 10 and no image at all can be
produced. A single `BitBlt` or `DwmRegisterThumbnail` call existing on Windows 7 therefore says nothing
about this program, and the PE header's `subsystem version 6.00` is the MSVC linker default, not a
support claim either.

### Capability check at run time

Before enumerating windows, planning output names, showing a consent dialog or reading a pixel, the tool
compares the Windows build read from `ntdll!RtlGetVersion` (never `GetVersionEx` — that one answers
according to the application manifest and to version helper) against the floors above, and reports:

| Code                       | When                                                                                                                                    | Exit      | Does another channel help?                                                                                                           |
| -------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- | --------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| `env.os_too_old`           | build below 10240 (so also Windows 8.1, which can load the file): the one encoder implementation every format goes through is not there | 7         | **No.** Not a channel problem and not a target problem — nothing on this machine can produce an image                                |
| `env.channel_unsupported`  | the channel asked for explicitly has a higher floor than this build                                                                     | 7         | **Yes** — another `--capture` value, or `auto`. Retrying the same target cannot help, and the requested channel is never substituted |
| `note.channel_unavailable` | `auto` found a channel whose floor this build is under and dropped it from the chain                                                    | unchanged | the image can still come from another channel; `images[].source` names the one that did it                                           |
| `note.os_unverifiable`     | the build number could not be read at all                                                                                               | unchanged | nothing was filtered by version this time - no answer counts as either "supported" or "unsupported"                                  |

`--verbose` echoes `input.osBuild` and `input.captureChain` (the channels this machine can actually offer
for the kind of target requested), so an agent can ask the capability question without capturing
anything. `--dry-run` never gates on the environment, because it takes no frame.

Device-level capability is deliberately not predicted: a driver that will not feed desktop duplication, a
machine that refuses Windows.Graphics.Capture, a session with no interactive desktop, an N edition missing
media components — none of those show up in a version number, and each reports its own `capture.*` code
with the real HRESULT rather than being guessed at in advance.

### Process integrity level

**There is one piece of evidence about this process's own level, and only one**: `session.integrityLevel` from
`--capabilities` / `--diagnostics`, which asks the token of ECAPTURE's own process. "The directory carries an explicit
`Mandatory Label\Low Mandatory Level` label", "the label a file inside it inherits" and "`whoami /groups` of the shell
you are in right now" are three facts that are not equivalent — the first two are *possible* causes of a process
started from there running below Medium, while the third describes a different process altogether. A low-integrity
process creating a file inside a medium-integrity directory is refused (`io.write_failed` + Win32 `5`), and
**an access denial has more than one cause**: that same pair of codes can also come from something holding the file, a
path that does not fit, or a directory that genuinely refuses writes. So this section is about what *usually* breaks
below Medium, not a lookup table that reads "this code, therefore this cause".

When a process started from such a path does run below Medium, three unrelated things typically break, and until now
all three looked like one plain "capture failed / write failed", so callers switched backends, raised deadlines and
retried — all wasted:

- `wgc` is refused at the frame step: `GraphicsCaptureItem.CreateForWindow` returns `E_ACCESSDENIED`
  (`0x80070005`)
- `printwindow` is refused with Win32 error `5`: a low-integrity process cannot send its cross-process draw message
  to a medium-integrity window (UIPI)
- saving is refused: such a process cannot create a file inside a medium-integrity directory, so the write reports
  `io.write_failed` + `5` **with the image already in memory** — a directory carrying the same label accepts it

What this level did **not** block on the machine where it was measured: `--capture dwm` (the `dwm.thumbnail` path)
still delivers a window image as long as the output directory lets a low-integrity process create the file, and
`bitblt.screen` / `duplication.frame` still deliver — those two always need a person's own answer, which is a
different layer (see [Screenshot authorization and `--yes`](#screenshot-authorization-and---yes)). Those two
sentences are **one result measured on this development machine**, not a promise for every machine: low integrity is
not "this machine cannot take screenshots", nothing here says another route will succeed, and the `dwm` path
**escalates to the route that reads desktop pixels** when the window itself cannot yield its picture — that route
still always asks a person, and the authorization tiers do not change with the integrity level. `--capabilities`
leaves each route's `status` untouched, because that field is the version-floor and screen-topology layer. The
measurements cover one development machine (Windows 10 version 22H2, build 19045, x64, 2026-10-09) and are not a
general conclusion: the byte-identical exe fails inside
`%UserProfile%\.agents` and its subdirectories, and works at every other place it was tried
(`%LOCALAPPDATA%\Temp`, `%APPDATA%\Roaming`, `%UserProfile%`, this repository's own `build\`). To check a machine
of your own, keep the three questions apart:

```powershell
icacls "%UserProfile%\.agents"                # does the directory carry Mandatory Label\Low Mandatory Level? (one possible cause)
whoami /groups | findstr /i "Mandatory Label"   # the level of this shell itself (here: S-1-16-8192 = Medium) — not ECAPTURE's level
ECAPTURE.EXE --capabilities | findstr integrityLevel   # the evidence about this tool's own process: below Medium it reads low
```

The tool now names that layer. `--capabilities` / `--diagnostics` report `session.integrityLevel` as an ASCII token
(`unknown` / `untrusted` / `low` / `medium` / `high` / `system` / `protected_process`), `caveats` gains
`process_integrity_below_medium` when it is under `medium`, and `--diagnostics -v` adds a `tokenIntegrityLevel`
probe. In a real run below `medium`, every "the system refused" failure of that round (HRESULT `0x80070005` or
Win32 `5` at the frame step, the file write or the stdout write) gets its `hint` extended with an actionable
sentence, and the round gains one `note.low_integrity` in `notes` carrying `target` / `backend` / `stage`. `code`,
`stage`, `hresult` / `win32`, the exit code and every image already delivered stay exactly as they were, so a caller
that branches on `code` sees the same contract, and `--yes` and the authorization tiers are unaffected. A question
with no answer reads `unknown` — neither "good enough" nor "downgraded". `note.low_integrity` is a note, so
`--quiet` hides it, while the `hint` added to `errors` is not hidden by `--quiet`.

Three ways out, cheapest first:

1. Point `--out` at a path inside a directory that carries the same low label (the tool never creates directories,
   so that level has to exist already)
2. Ask for the window's own pixels with `--capture dwm` — measured on this development machine to still deliver at
   this level, which is not a promise for every machine, and when that path escalates internally to the desktop-pixel
   fallback it still always asks a person
3. Install this skill somewhere that is not labelled down, or have the owner of that directory remove the label —
   **changing a label, an ACL, the install location or an integrity level is never something this tool does by
   itself**; that is a system security setting, so whoever owns it decides and authorises it separately. The tool
   neither elevates nor changes those settings to "make the screenshot pass"

**Running as administrator is not the answer this hint is pointing at**, and the message never suggests doing it.

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

- **Three separate facts.** `compiled` says whether this binary implements the route at all. `status` says whether
  this machine's own evidence (version floors plus screen topology) lets it run now. `verifiedOnThisMachine` is an
  **environment comparison, not a per-device test record**: it answers `yes` only when this machine's Windows build
  and architecture equal the environment this project ran its on-device tests in (build 19045, x64), `unknown` when
  either half could not be read, and `no` otherwise - which is why a 26100 machine reads `no` and gains the
  `this_environment_not_tested` caveat. `os.matchesTestedEnvironment` is that same judgement, and none of the three
  facts stands in for another: `available` + `verifiedOnThisMachine: no` means "this build can take the route here,
  this project has only proven it on a different build", while `yes` still does not promise that a given window
  captures.
- **No answer is reported as no answer.** Every fact is one of `yes` / `no` / `unknown`; `unknown` is never
  folded into either "works" or "does not work", and the key is not silently dropped. When the build number
  could not be read, every `status` becomes `unverified` while `autoChainWindow` still lists all four
  channels - not filtered means not filtered, not "all supported".
- **`available` is not a guarantee.** It carries no promise that some specific window will capture: drivers,
  protected content and HDR mode are outside this layer. The `caveats` array at the end of the document
  exists precisely to pin that down.

| Section                               | Contents                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `contract` / `contractVersion`        | only these two documents carry a contract version (currently 1). The capture JSON stays as lean as 《Output format》 describes and gains no top-level metadata from this                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| `program`                             | name, the file name `ECAPTURE.EXE` itself (no directory), version, architecture, `buildId`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| `os`                                  | this machine's build (that group reads `unknown` when `known` is false), `declaredMinBuild`, `encoderMinBuild`, `testedMinBuild` + `testedArch` (the environment this project actually tested) and `matchesTestedEnvironment`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `session`                             | attached to the console session, remote desktop, screen topology present and how many monitors, whether this process is elevated, this process's mandatory integrity level (`integrityLevel`, an ASCII token), `consentDialogExpected` (inferred; `consentDialogProbed: false` says no dialog was ever shown)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `authorization`                       | `yesSkips: "window-content"`, `desktopPixelsAlwaysAsk: true`, unregistered paths treated as `desktop`, plus the whole internal-path registry with `scope` and `consentWithoutYes` / `consentWithYes` per row - the machine-readable form of the table in 《Screenshot authorization and \`--yes\`》                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `backends`                            | per route: `compiled` / `status` / `reason` / `minBuild` / `verifiedOnThisMachine`, plus which internal path it takes for window and for screen targets (`dwm`'s screen fallback included, so `--yes` cannot be read as covering more than it does)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `formats`                             | per format: `compiled` / `status` / `reason` / `minBuild` / `registered`, one row for each of `png` / `jpeg` / `bmp` / `tiff` / `gif` (all five are compiled, and every one of them goes through the same WinRT `BitmapEncoder` call with a different encoder id). `registered` is always `unknown` because this layer does not exercise encoders (doing so would be probing capability by producing an image, the same reason we never probe by capturing). `webp` / `ico`, once advertised and then removed for lack of an encoder, stay here as `compiled: false` + `reason: "not_compiled"` so a caller gets a definite answer                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| `cursor`                              | the `--cursor` story: `default` (what happens when the option is absent), the three values, that one switch as `compiled` / `status` / `reason` / `minBuild` (19041) / `verifiedOnThisMachine`, then one row per registered internal path with `capability` (`settable` / `excludes_cursor` / `pointer_state_unverified` / `unregistered`), `reason` and `include` / `exclude` each as `yes` / `no` / `unknown`, plus `pointerShapeCompositing: "never"` and `pixelRetouching: "never"`. The two duplication paths read `capability: pointer_state_unverified` with `include` and `exclude` both `no` — a desktop frame may already have the pointer drawn into it, and the tool never composites the pointer shape, which is not proof the pixels hold no pointer. Nothing is probed by capturing, so a path that is not in the registry reads `unknown` rather than a guessed answer                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `color`                               | the `--hdr` story: `default` (what happens when the option is absent), the three values (`auto` / `tonemap` / `refuse`), `compiled` / `status` / `reason` / `verifiedOnThisMachine`. `status` is about "can this build bring back a wide-gamut frame and how does it map", it does **not** ask whether this screen is currently in HDR mode (reason `hdr_display_mode_not_probed`); `verifiedOnThisMachine` is always `no` (this project has no HDR display, so it never claims color acceptance). One row per registered internal path, with `capability` = `wide_gamut_capable` (only `wgc` / `screen.wgc`) / `wide_gamut_unverified` (`duplication.frame` / `screen.duplication`: the desktop surface _can_ arrive FP16 or 10-bit, but this build never asks the display's color space before `DuplicateOutput`, so it cannot prove what it got) / `sdr_source_only` (everything that reads an 8-bit DC) / `unregistered`, plus `honorsExplicitPolicy` - `true` only for the two `wgc` rows, which is the machine-readable form of "only wgc can fulfil `tonemap` / `refuse` in this build". `toneMapping` / `floatIntermediateFrame: "per_pixel_registers"` / `encoderOutput: "sdr_bgra8"` (HDR is always mapped to 8-bit SDR for delivery; no native-HDR output)                                                                                              |
| `autoChainWindow` / `autoChainScreen` | the `auto` chain this machine can take now. Computed by the **same** `GateChannels` call that fills `input.captureChain` for a real run, and `tests\capabilities.ps1` compares the two                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `history`                             | the **rules** of the screenshot history archive, self-described (on by default): `enabledByDefault: true`, `relativeTo: "executable-directory"` (beside the exe that actually ran — not the working directory, not the directory of `--out`), `location: "history/YYYY-MM-DD/"`, `naming: "YYYYMMDD-HHMMSS-<pid>-<token>-<seq>.<ext>"`, `source: "same-encoded-bytes"` (the copy uses the bytes the primary delivery already encoded: no re-capture, no re-encode, no reading the primary output file, no hard link), `commit: "exclusive-create"` (exclusive creation, never overwriting an existing history file), `created: "after-first-delivered-image"` (read-only queries and runs that delivered nothing create not one directory), `retention: "never-pruned-automatically"`, `uploads: false`, `backgroundPruning: false`, `writabilityProbed: false`, plus the partial-success exit code for a failed copy: `partialSuccessExit: 7`. Not one field here asserts "this run wrote successfully" — that answer is in the capture result's `images[].history`, and this section does not even try whether that location can be written (so `caveats` always carries `history_root_writability_not_probed`); every value is written relative to the program directory and no absolute path appears |
| `limits`                              | maximum frame side and bytes, `--timeout-ms` ceiling, built-in isolated-call ceiling, WGC frame-pool rebuild count, ordinal and PID ceilings, `stdoutTargetsMax: 1`, JPEG quality range                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `privacy`                             | what this query declares it did not do: no screen captured, no dialog shown, nothing uploaded, no user files enumerated, no environment variables read, no usernames, no paths                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| `caveats`                             | stable ASCII tokens listing what this report does **not** assert: `available_is_not_a_guarantee`, `no_capture_performed`, `no_consent_dialog_shown`, `encoder_state_not_probed`, `device_capability_not_predicted`, `consent_dialog_state_inferred_not_probed`, `subsystem_version_is_linker_default`, plus per machine `os_version_unavailable` / `display_topology_absent` / `display_topology_unavailable` / `remote_session_observed` / `desktop_paths_need_answerable_dialog` / `unelevated_process_may_miss_elevated_targets` / `process_integrity_below_medium` / `build_identity_unavailable` / `this_environment_not_tested` / `tested_environment_unknown`, and always `cursor_effective_is_a_setting_not_a_pixel_check` + `pointer_shape_never_composited_nor_erased` + `duplication_desktop_frame_pointer_not_guaranteed` (the cursor fields stop at the setting and the source, never at "this picture visibly has or has no pointer", and a duplication desktop frame is not guaranteed pointer-free), and always `hdr_tone_mapping_not_verified_on_hdr_display` + `hdr_output_is_tone_mapped_to_sdr_bgra8` + `hdr_explicit_policy_only_fulfilled_by_wgc` (the HDR mapping math is verified offline but there is no HDR display to test end-to-end, HDR is always mapped down to 8-bit SDR, and an explicit `tonemap` / `refuse` request is only ever fulfilled by the two `wgc` paths), and always `history_root_writability_not_probed` (the `history` section states the archiving rules; this query never tried writing that location and does not predict that any one write will succeed) |

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

`--dry-run` answers the "which windows would this hit?" question with one human-readable line per candidate inside
`note.dry_run` (`hwnd=0x… pid=… 1261x614+681+22 class=… title=…`) - a prose shape that was never promised to stay
stable, so a caller has to parse handles and rectangles back out of a sentence. These two commands are the
structured answer to the same question. They run **the same condition evaluation** as a
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

- **Nothing is captured, nothing is asked, nothing is written.** No backend is called, no consent dialog is shown,
  no file is created, no network access, no environment variable read - the document itself states this in
  `authorization` (`pixelsRead: 0`, `consentDialogShown: false`, `filesWritten: false`). The query also **never
  touches a target window**: no restore, no activation, no z-order change, because "let me look at what is open"
  must not change what is on the screen. `caveats` carries `no_capture_performed` and `no_window_touched` for this.
- **Several matches are not a capture ambiguity.** `--list` pages them (`--offset` / `--limit`, default 50 per
  batch) and reports the real total in `pagination.matched`, so a short list never reads as "there are only these".
  Zero matches is a normal answer: `windows: []` and exit code `0`, not `match.no_window` + `4`. `--inspect` needs
  one target, so it applies **the same** selection policy a capture uses: if that policy still leaves several, it is
  `match.ambiguous_window` + exit code `5` - it never picks one for you, and it never grabs a look-alike instead.
- **The list is a snapshot and it expires.** Handles get reused, titles change, processes exit, so the `hwnd` / `pid`
  / class in here are **not a credential** you may hold on to. Every successful query carries
  `note.window_query_stale`, and the `identity` block of each row states `verificationRequired: true`,
  `isAuthorizationToken: false` and `raceWindowReducedNotEliminated: true`. A later capture re-checks the target
  identity before reading a pixel (that is `capture.target_gone` / `capture.target_changed` /
  `capture.target_unverifiable`), and consent is still decided by where the pixels come from: **`--yes` changes
  nothing here** (`authorization.yesAffectsResult: false`) - it neither unlocks a field nor skips a dialog that this
  query never shows.
- **An unreadable field says so.** Cross-process questions have three outcomes, written per field: `readable`,
  `denied` (the system refused this caller), `failed` (asked and it did not answer), each with the raw Win32 code.
  Unreadable values are sentinels (`0` / empty) plus that state, never silent absences, and the document does not
  advise running elevated: `caveats` carries `unreadable_fields_are_not_a_prediction`.
- **The visibility policy is stated, not implied.** Invisible and zero-sized windows are excluded - the same rule the
  capture enumeration uses - and `policy` says so (`invisibleExcluded`, `zeroSizedExcluded`); minimized windows are
  excluded by default too, are counted in `policy.minimizedExcluded`, and `--list=all` merges them into the same
  z-order. There is **no** claim about "system windows": Windows exposes no attribute that means "this is a system
  window", so `policy.systemWindowAssertion` is `false`.

Fields per row (window inspect returns the same object as `window` instead of the `windows` array):

```json
{
  "contract": "windowquery",
  "contractVersion": 1,
  "query": "list",
  "program": { "version": "0.4.0" },
  "authorization": {
    "readOnly": true,
    "pixelsRead": 0,
    "consentDialogShown": false,
    "filesWritten": false,
    "yesAffectsResult": false,
    "identityFieldsAreNotConsent": true
  },
  "policy": {
    "invisibleExcluded": true,
    "zeroSizedExcluded": true,
    "minimizedIncluded": false,
    "minimizedExcluded": 2,
    "systemWindowAssertion": false,
    "order": "zOrder"
  },
  "pagination": {
    "offset": 0,
    "limit": 50,
    "limitDefaulted": true,
    "defaultLimit": 50,
    "maxLimit": 8192,
    "matched": 52,
    "returned": 50,
    "truncated": true,
    "nextOffset": 50
  },
  "windows": [
    {
      "hwnd": "0x001A0B4C",
      "pid": 31468,
      "title": "…",
      "class": "CabinetWClass",
      "image": "explorer.exe",
      "rect": { "x": 681, "y": 22, "width": 1261, "height": 614 },
      "visible": true,
      "minimized": false,
      "zOrder": 3,
      "readability": {
        "process": { "state": "readable" },
        "imagePath": { "state": "denied", "win32": 5 },
        "processStart": { "state": "readable" },
        "rect": { "state": "readable" }
      },
      "identity": {
        "hwnd": "0x001A0B4C",
        "pid": 31468,
        "class": "CabinetWClass",
        "processStartTicks": 134351142668527401,
        "selectionNeedsRecheck": false,
        "verificationRequired": true,
        "isAuthorizationToken": false,
        "raceWindowReducedNotEliminated": true
      }
    }
  ],
  "caveats": [
    "no_capture_performed",
    "no_consent_dialog_shown",
    "no_window_touched",
    "snapshot_expires",
    "identity_fields_are_not_a_token",
    "invisible_and_zero_sized_excluded",
    "unreadable_fields_are_not_a_prediction",
    "list_may_be_partial"
  ]
}
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

Naming "that monitor" used to have exactly one spelling: `--monitor <n>`, where `n` is the position in _this run's_
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

| Field               | What it is                            | Stable across            | Selector                |
| ------------------- | ------------------------------------- | ------------------------ | ----------------------- |
| `ordinal`           | position in this run's enumeration    | `this_invocation`        | `--monitor <n>`         |
| `deviceName`        | GDI view device name, `\\.\DISPLAY1`  | `this_desktop_attach`    | `device:`               |
| `monitorDevicePath` | monitor devnode device interface path | `cross_session_expected` | `id:`                   |
| `adapterLuid`       | adapter's locally unique id           | `this_session`           | none - association only |

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

| Value         | Channel                                             | Covered window          | Hardware-accelerated content           | Pointer (`--cursor`)                                                                                             | API history floor                                                                               |
| ------------- | --------------------------------------------------- | ----------------------- | -------------------------------------- | ---------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| `wgc`         | Windows.Graphics.Capture                            | yes (DWM cache)         | normal                                 | a real switch, set and read back (19041+)                                                                        | Win10 1903 (18362) — the `CreateForWindow` / `CreateForMonitor` interop, not the 1803 namespace |
| `dwm`         | DwmRegisterThumbnail                                | yes                     | mostly normal, protected windows black | never in the source pixels                                                                                       | Win8.1 (9600) — registering is older, but the read-back is `PrintWindow(PW_RENDERFULLCONTENT)`  |
| `printwindow` | PrintWindow + PW_RENDERFULLCONTENT                  | yes (window self-draw)  | often fully black                      | never in the source pixels                                                                                       | Win8.1 (9600) for that flag                                                                     |
| `bitblt`      | BitBlt from a screen DC                             | no, visible pixels only | partly black                           | never in the source pixels                                                                                       | no floor of its own                                                                             |
| `duplication` | DXGI desktop duplication frame, cropped to the rect | no, visible pixels only | normal                                 | no switch: the pointer may already be drawn into the desktop frame, so neither include nor exclude is guaranteed | Win8 (9200); RDP / virtual GPUs often yield nothing                                             |
| `auto`        | falls back wgc → dwm → printwindow → bitblt         | best effort             | best effort                            | `include` narrows it to `wgc`                                                                                    | the chain minus whatever this build gates out                                                   |

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
- A single-colour result is not treated as a failed capture: the image is delivered and `note.frame_uniform` records
  it. `dwm`'s desktop route is entered only when the thumbnail route itself failed — never because the picture came
  back flat, which would be a separate pixel source needing its own authorization.
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
    requires. A screen driven by a second GPU is therefore reachable. There is no WARP fallback on this route: a
    software device owns no physical output, so it would hand back an empty frame while still reporting the right
    size.
  - **Only one output per target.** A window that straddles two monitors (or hangs off the edge) is captured where it
    overlaps the output with the largest overlap; the rest is _not_ in the image. That shows up as
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
  `IGraphicsCaptureSession2::IsCursorCaptureEnabled`, introduced in Windows build 19041. That is a _higher_ floor
  than the `wgc` channel's own 18362: a 1903 machine can capture with `wgc` and still be unable to say anything
  about the pointer.
- `printwindow` (a window painting itself into a DC), `dwm.thumbnail` (the DWM redirection surface), and
  `dwm.screen` / `bitblt.screen` / `screen.bitblt` (a screen DC, where the system cursor is drawn outside the DC's
  content) contain no pointer at all. So `exclude` is true of them as a fact about the source, and `include` is
  simply not something they can do.
- `duplication.frame` / `screen.duplication` are a different case: they deliver a whole desktop frame, and the
  pointer may **already be drawn into** it. The tool never fetches or composites the duplication pointer shape, but
  "we did not composite it" is not proof the pixels hold no pointer — per the Windows docs the pointer is either
  baked into the desktop image or GPU-overlaid, and the per-frame `PointerPosition` / `PointerShapeBufferSize`
  fields describe only the hardware pointer, so no per-frame assertion is honest here. These two paths are registered
  as `pointer_state_unverified`: **neither** `include` **nor** `exclude` is something they can promise.

Two rules follow, and both exist so that "I asked for it" can never be read as "I got it":

- **A request a route cannot deliver is refused, not re-routed.** `--cursor include` with `printwindow`, `dwm` or
  `bitblt` — sources that hold no pointer — is `capture.cursor_unsupported` (exit `1`) at parse time, before any
  dialog, before any output name is planned, before a pixel. On `duplication` **both** `include` and `exclude` are
  `capture.cursor_unsupported` (exit `1`) at parse time, with two different wordings (include: that source gives no
  pointer switch; exclude: the desktop frame's pointer state cannot be proven). Switching to a channel that reads the
  screen would neither add a pointer nor guarantee removing one, and would not be a frame anybody approved. With
  `--capture auto` the routes that cannot honour the request are dropped from the chain and each leaves a
  `note.cursor_channel_skipped`; when nothing is left, or when this machine's Windows build is below 19041 and the
  request was explicit, the code is `env.cursor_unsupported` (exit `7`) and nothing is captured. `--verbose` echoes
  the surviving chain as `input.captureChain`, computed by the same function the run uses, so `--cursor include` +
  `auto` shows `["wgc"]`.
- **No pixel retouching.** The tool does not fetch the duplication pointer shape to draw it, never `DrawIcon`s a
  cursor into a frame, and never tries to erase a pointer that is already there - those are image patching and none
  of them is verifiable. `tests\cursor.ps1` fails if such a call ever appears anywhere in `src/`.

For every delivered image, and only when `--cursor` was written at all, three fields say what happened:

| Field             | Values                                                                                                                                                                                     | Meaning                           |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------- |
| `cursorRequested` | `default` / `include` / `exclude`                                                                                                                                                          | what was asked for                |
| `cursorEffective` | `include` / `exclude` / `unverified`                                                                                                                                                       | what this path actually delivered |
| `cursorBasis`     | `wgc_session_property_set` / `wgc_session_property_read` / `path_excludes_cursor` / `path_pointer_state_unverified` / `wgc_cursor_property_unavailable` / `path_capability_not_registered` | on what evidence                  |

`wgc_session_property_set` means the switch was set for this request and the value read back matched it;
`wgc_session_property_read` means nothing was set (`--cursor default`) and only the current value was read;
`path_excludes_cursor` means this path's source pixels contain no pointer; `path_pointer_state_unverified` means a
duplication desktop frame whose pointer state cannot be proven (so `cursorEffective` is `unverified`);
`wgc_cursor_property_unavailable` means that question gave no answer; and `path_capability_not_registered` means the
route was not in the registry and was treated strictly rather than guessed. In every unverified case
`cursorEffective` is `unverified` rather than either answer.

`cursorEffective` stops at exactly that evidence. It says this session was set to draw the pointer, or that this
source holds none - it does **not** claim a pointer happened to be sitting over the target in these pixels. This
SDK's session interface has no read-only `IsCursorVisible`, so the tool makes no pixel-level assertion, and
`--capabilities` states that boundary as the `cursor_effective_is_a_setting_not_a_pixel_check` caveat. If an explicit
`include` / `exclude` request cannot be confirmed on a `wgc` capture (the interface cannot be obtained, the set call
fails, or the value read back is the opposite one), the code is `capture.cursor_unverifiable` (exit `7`) _before_
`StartCapture`, so a frame that contradicts the request is never delivered; under `--cursor default` the same
situation is reported as `unverified` instead of being folded into either answer.

Asking about the pointer changes nothing about authorization. The tier is still decided by where the pixels come
from, so `--cursor exclude` on a `bitblt` screen route or any whole-screen target, and `--cursor default` on a
duplication route, still show the dialog and `--yes` still does not cover them; `wgc` without `--yes` still asks. (An
`--cursor exclude` + `duplication` request is refused at parse time and never reaches a dialog at all.) Those consent
cases are checked on a real window in `tests\cursor.ps1`. Like `path` / `scope` / `rect` and the crop fields, the three
cursor fields are locating evidence and `--quiet` does not suppress them.

`--capabilities` answers all of the above without capturing anything, in its `cursor` section: the default value, the
three values, that single switch as `compiled` / `status` / `minBuild` / `verifiedOnThisMachine`, one row per
registered path with `capability` / `reason` / `include` / `exclude` (each of the last two `yes` / `no` / `unknown`),
and `pointerShapeCompositing: "never"` plus `pixelRetouching: "never"`.

## HDR color handling (`--hdr`)

When a display is in HDR mode the captured frame can carry luminance beyond SDR and a different transfer function. Interpreting such a frame as plain 8-bit BGRA yields a washed-out, desaturated image whose highlights are blown — and it "looks like a normal picture", which is exactly the result this tool refuses to treat as correct. `--hdr auto|tonemap|refuse` makes that a decision you state. Two states that behave the same while **capturing** must not be blended while **reporting**:

- **`--hdr` not written at all** — the tool changes **nothing** about color: it does not probe the display state, does not change the capture format, does not tone-map, **and the color key group does not appear in the result** — the output is byte-for-byte what it was before this option existed.
- **`--hdr auto` written explicitly** — the same capture behaviour (no probing, no format change, no tone-map), **but the color key group is emitted**, reporting only what the frame passively came back as. On the `wgc` path an 8-bit delivered frame reports `hdrEffective: unverified` / `hdrBasis: bgra8_source_unverified`, because `auto` never asked the display whether it was in HDR mode and an 8-bit surface does not prove the source was SDR (see the field table below).

`auto` is also the default _value_ used when the option is omitted — but omitting it and writing it are distinguishable in the result via `input.hdrGiven` (`-v`), and that is exactly the omission-versus-explicit split this tool refuses to collapse.

Which channels can bring back a wide-gamut frame, and which can actually _act_ on a stated policy, are two different
questions, judged by the same rule as everything else: **where the pixels come from**, not the channel name. The
registry lives in `src/HdrColor.h`, one row per `images[].path`, and `--capabilities` prints it as `color.paths` with
`capability` plus `honorsExplicitPolicy`:

- `wgc` / `screen.wgc` — `wide_gamut_capable`, `honorsExplicitPolicy: true`. The source follows the display mode and
  this route reads back the format it really got (FP16 scRGB linear, or 8-bit BGRA), so it can say which one and map
  it. These two are the only paths that fulfil `tonemap` / `refuse` in this build.
- `duplication.frame` / `screen.duplication` — `wide_gamut_unverified`, `honorsExplicitPolicy: false`. The desktop
  surface _can_ arrive as FP16 scRGB or 10-bit ST.2084 (PQ) / HLG BT.2020, but this build never asks the display's
  color space before `DuplicateOutput`, so it cannot prove what it got and cannot promise a policy. Asking for
  `tonemap` / `refuse` here is therefore refused rather than quietly answered.
- `printwindow` (the window self-draws into an 8-bit DC), `dwm.thumbnail` / `dwm.screen`, `bitblt.screen` /
  `screen.bitblt` — `sdr_source_only`, `honorsExplicitPolicy: false`. They can only bring back 8-bit SDR, so HDR
  handling has nothing to act on there — for them the mapping is the identity, not "impossible so switch channels".

The three values:

- `tonemap` — map an HDR frame to SDR before delivering. This tool builds a **per-pixel float intermediate** before encoding (it never allocates a whole float frame, which would blow the 1 GiB per-frame budget by 4×) and applies a fixed curve: decode the transfer function (scRGB linear / PQ → absolute nits → relative linear / HLG inverse OETF) → BT.2020→BT.709 primaries matrix → **extended-Reinhard** luminance tone map (deterministic, monotonic; it degenerates to the identity when `white=1`) → sRGB encode → opaque alpha passed through. For an already-SDR source it is an identity passthrough.
- `refuse` — once the source is confirmed to be HDR, error out and write not a single pixel; never deliver a BGRA8-washed image.
- `auto` (the default value) — do not engage the mapping path above: no display probe, no format change, no
  tone-map. When `--hdr auto` is written explicitly it still emits the color key group, just reporting passively
  what the frame came back as (an 8-bit `wgc` frame is `unverified`, not a claim of SDR); when the option is omitted
  the group does not appear at all (see the intro).

Two rules follow from that table, both sharing the `--cursor` principle:

- **If a channel cannot do it, refuse; never silently reroute.** `--hdr tonemap` / `refuse` naming `printwindow` /
  `dwm` / `bitblt` / `duplication` explicitly is `capture.hdr_unsupported` at parse time (exit code `1`) and does
  **not** switch channels (a desktop-reading channel has no more HDR to map and would only capture a screen no one
  approved). With `--capture auto` the request narrows the fallback chain instead — the same `FilterChainForHdr`
  judgement that `--verbose` echoes as `input.captureChain`, so `auto` + `tonemap` leaves only `wgc`: each dropped
  channel leaves its own `note.hdr_channel_skipped`, and if nothing is left the run stops with
  `env.hdr_unsupported` (exit code `7`) rather than delivering an unmapped frame. A capture that comes back
  `capture.hdr_refused` stops the chain outright — it is never retried on another backend, because a different
  backend would only produce a different unapproved picture.
- **Unrecognized is unrecognized.** A wide-gamut pixel format this build cannot name is `capture.hdr_unverifiable` (exit code `7`) — it is neither forced into BGRA8 nor "mapped by guessing"; `--hdr refuse` on a confirmed-HDR source is `capture.hdr_refused` (exit code `7`). All three are given before encoding and write nothing.

Whenever `--hdr` is written, each delivered image carries this group of fields (with the option absent none appear, byte-identical to before):

| Field              | Values                                                                                                                                                                                                                                | What it says                          |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------- |
| `hdrRequested`     | `auto` / `tonemap` / `refuse`                                                                                                                                                                                                         | which policy was asked                |
| `hdrEffective`     | `sdr_passthrough` / `tone_mapped` / `unverified`                                                                                                                                                                                      | what this frame actually went through |
| `hdrBasis`         | `delivered_bgra8_sdr` / `scrgb_float_tone_mapped` / `pq_bt2020_tone_mapped` / `hlg_bt2020_tone_mapped` / `path_sdr_source` / `format_unrecognized` / `transfer_function_unknown` / `bgra8_source_unverified` / `tone_map_not_applied` | what that conclusion rests on         |
| `sourceColorSpace` | `srgb_bgra8` / `scrgb_float` / `pq_bt2020` / `hlg_bt2020` / `rgb10a2_unverified` / `unknown`                                                                                                                                          | the source before encoding            |
| `sourceBitDepth`   | `8` / `10` / `16` (omitted entirely when unrecognized)                                                                                                                                                                                | bits per channel of the source        |

In this build only the `scrgb_float_tone_mapped` path is reachable on real captures (the FP16 `wgc` pool), because a
10-bit packed frame is classified `rgb10a2_unverified` rather than assumed PQ or HLG — so `pq_bt2020_tone_mapped` /
`hlg_bt2020_tone_mapped` and `transfer_function_unknown` are enum values exercised offline, not a promise that a real
PQ/HLG panel was tone-mapped here. A wide-gamut-labelled source with no tone-map record is `unverified` /
`tone_map_not_applied`, never "mapped" by assumption.

When processing was explicitly asked (`tonemap` / `refuse`) and the frame is delivered as 8-bit BGRA, the image is
still delivered (mapping an 8-bit source is the identity), but which note is recorded depends on what was actually
answered before capture: if the read-only display question ran and answered SDR, the note is `note.hdr_source_sdr`
("confirmed 8-bit SDR"); if that question was never asked or gave no answer, the note is `note.hdr_source_unverified`
("delivered as 8-bit, but the source was not confirmed SDR") — an unverified 8-bit frame is never written up as a
confirmed-SDR one. Either way "I asked for HDR handling" and "this frame had no confirmed HDR" stay distinguishable
instead of a silent pass masquerading as "HDR was mapped". `--hdr auto` does not emit either note (it only reports
passively via the machine fields above).

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

| Route (`images[].path`)                             | Where the pixels come from                       | Without `--yes` | With `--yes`    |
| --------------------------------------------------- | ------------------------------------------------ | --------------- | --------------- |
| `wgc`, `printwindow`, `dwm.thumbnail`               | the selected window itself                       | asked once      | not asked       |
| `dwm.screen`, `bitblt.screen`, `duplication.frame`  | the area of the screen behind/around that window | asked           | **still asked** |
| `screen.wgc`, `screen.bitblt`, `screen.duplication` | a whole monitor                                  | asked           | **still asked** |

A route that is unknown or cannot be proven is treated as a desktop route, so a new channel that forgot to register
itself ends up stricter rather than looser. `--monitor` together with window conditions filters **windows**, so those
images follow the window rows above.

- **Nothing skips a desktop route**: not `--yes`, not `--quiet`, not an environment variable, not stdin, not who the
  caller is. That is the whole point of the tiering.
- One confirmation covers the batch of targets this request listed, so several backends or windows do not each
  re-prompt. That permission is a **snapshot**, not a standing grant: it binds the listed targets with the areas the
  dialog showed, the absolute output names the batch resolved to, and a fingerprint of the screen topology, and it is
  re-verified twice — once before the permit is issued (a topology that moved while the box was open means a new
  question, not a reused answer) and once before each step that would sample pixels (the area has to still be inside
  what was approved). Consent is never cached across requests, never widened to a target the dialog did not show, and
  approving a window-content capture is never approval of a desktop one: `--capture auto` with `--yes` may run the
  window routes silently, but asks before it enters a desktop route.
- The dialog is a plain `MB_YESNO` box — two buttons, Yes and No, with default focus on **No**. Only an explicit Yes
  approves; every other outcome is a refusal. Decline it by clicking **No**. Do not rely on `X` or `Esc`: with
  `MB_YESNO` the title-bar `X` is shown but disabled and there is no Cancel button for `Esc` to trigger, so neither is
  a dependable "No" gesture — the box simply has to be answered, and the tool treats any non-Yes result as a refusal
  anyway. On top of that, nobody answering within `--consent-timeout-ms` is its own refusal (`capture.consent_timeout`).
- After a refusal ("No"), a timeout (nobody answered within `--consent-timeout-ms`), or an unavailable interactive
  desktop, the rest of that request stops — the remaining links of the `auto` fallback chain too, not just the
  remaining `--all` targets — with no retry and no second ask, while every image already completed stays in `images`.
  A human answer is a boundary, not a hint to try a different route until one of them gets through.
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

| Code                          | Exit | Meaning                                                                                                                 | Next step                                                                                |
| ----------------------------- | ---- | ----------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------- |
| `capture.target_gone`         | 7    | the handle was destroyed during this request                                                                            | enumerate the windows again                                                              |
| `capture.target_changed`      | 7    | that handle value now belongs to another object (or no longer satisfies the conditions)                                 | select the target again — **the permission you gave is not transferred to a new object** |
| `capture.target_unverifiable` | 7    | one of the questions could not be answered (process information unreadable, the condition re-evaluation did not finish) | check the execution environment (rights, policy, antivirus), or raise `--timeout-ms`     |

A changed identity never becomes a licence to "relax the conditions and grab a look-alike" — same rule as `--yes`: consent is bound to the object that was listed for the human.

**How far that goes:** this re-check narrows the race window, it does not claim to close it. Checking and capturing are not one atomic operation, an `HWND` is not a waitable object, and there is no public API that pins a window into existence. A change in the instant between the check and the frame is still possible — it just no longer has a whole planning-plus-consent span of time to happen in.

## Deadlines and calls that block (`--timeout-ms` / `--consent-timeout-ms`)

`--timeout-ms <ms>` is a **total** budget for the automatic part of the run, measured on a monotonic clock from
the moment targets start being selected. Window/screen matching (including `--title-regex`), the `auto` fallback
chain, frame waits, encoding and the final commit all spend **the same** budget: no step and no further target of
the batch gets a fresh copy, so four backends cannot each wait 2 seconds and two targets cannot each wait again.
Omitted or `0` means no overall budget; even then every isolated call is bounded by a built-in 5000 ms per-call cap.
When the budget runs out, **a step that has not started yet
is refused and its image is not written** — `match.timeout` (`stage=match`) when the budget died while evaluating
conditions, `capture.timeout` with `stage=capture` for a frame that never arrived and with `stage=encode` when the
budget died in the encoder (there is no separate `encode.timeout` code; the stage is what separates the two),
`io.timeout` (`stage=write` / `stdout`) when the write step never got to start — exit code `8` when that leaves the
run with not a single image delivered. The remaining targets of the batch are not started, and images already written
stay in `images`. Partial batches therefore behave exactly like partial capture failures: exit code non-zero, whatever
already landed is still delivered.

That gate is deliberately a _pre-step_ gate, and the honest limit is the flip side of it: the atomic file write has no
cancellation point, so it cannot be interrupted mid-flight and a budget that expires **during** a commit still lets
that file land. Nothing is rolled back or deleted on timeout — a file that exists on disk was written because the
caller asked for it, and quietly removing it would be a second, unasked-for write. After the write returns, a
deadline **re-check** runs and, if the budget has been crossed, records an extra `io.timeout` (message saying the
image was delivered in full but the budget had run out when that write finished) — but the committed image stays in
`images`, `captured` still counts it, and the run reports partial success with exit code `7` (delivered + error), not
the `8` of a write that never landed. Delivery facts and deadline compliance are kept deliberately separate.

Waiting for a person is a **separate** clock: `--consent-timeout-ms <ms>` bounds the confirmation dialog only and
never eats the automatic budget (somebody stepping away is not "the machine is slow"). The wait is paused out of the
automatic budget while it runs, so a long read does not spend the picture's own deadline. If nobody answers in time
the request is **refused** — `capture.consent_timeout`, exit code `6`, `stage=consent` — and never treated as
consent, and the rest of the batch stops just like after an explicit "No". Omitted or `0` waits forever, as before.
The bound is a deadline that is _polled_, not one that pre-empts the dialog: expiry is noticed on the next wait
slice, and the dialog gets a close grace of about 3 s, so a box can stay on screen a little past the number that was
asked for. The ~1 second buffer after "Yes", which keeps the dialog's close animation out of the picture, belongs to
the human stage too and is never skipped to make a deadline: what is bounded is the waiting for an answer, not the
settle time after one.

**Where the blocking actually goes.** `PrintWindow` hands the target window a draw request and waits for its
thread; `--capture printwindow` and the `dwm` read-back do exactly that, and there is no interrupt point inside
the call to check a deadline against. So does `std::regex`: a pattern such as `(a+)+$` against a long title can
backtrack for minutes, and a length limit on the pattern is not an execution deadline. Those calls now run in a
helper process of the same `ECAPTURE.EXE`, fed one already-parsed task over a private pipe; when the deadline
expires the parent stops **its own** helper process and reports the timeout. The target application's window is
never killed, and no worker can outlive the parent (a kill-on-close job object plus a broken-pipe check and a
budget-aware watchdog: it caps only the await-task handshake, then tracks the parent's remaining budget plus a small
deliver grace, and never shortens an explicitly accepted long budget).

The closing boundary matters as much as the timeout itself, because the pipe can answer at the wrong moment. Three
cases are told apart on connect — a helper that already connected (no I/O pending to cancel), a write that
completed synchronously, and one that returned `ERROR_IO_PENDING`. `CancelIoEx` only _requests_ cancellation, so the
parent waits for the completion to be reported and keeps each `OVERLAPPED` and its event alive until then; if the
completion never arrives it gives the helper about 2 s to leave on its own and then `TerminateProcess`s it, and that
grace runs **outside** the request's budget. A reply that shows up after the verdict is never read: a picture that landed
too late cannot rewrite a reported timeout into a success, and it cannot be delivered as an image either. What that
does **not** change: the helper only ever reads a single window's own picture or lists
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

| Placeholder | Meaning                                                                                                                                                                                                                                                                                                                                                                                      |
| ----------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `%i`        | ordinal, starting at 1 (`--all` windows, `--monitor all` screens)                                                                                                                                                                                                                                                                                                                            |
| `%h`        | window handle, shaped like `0x001B0C48`; screen targets give 0                                                                                                                                                                                                                                                                                                                               |
| `%p`        | process id; screen targets give 0                                                                                                                                                                                                                                                                                                                                                            |
| `%n`        | window title, or the device name without the `\\.\` prefix for screen targets (e.g. `DISPLAY1`) — cleaned into a file-name fragment: characters illegal in a name become `_`, trailing dots and spaces are dropped, a result that is exactly a reserved device name (`CON`, `NUL`, `COM1`, `LPT1`, …) gets a `_` prefix, and it is cut to 80 UTF-16 units without splitting a surrogate pair |
| `%d`        | local date `YYYYMMDD`                                                                                                                                                                                                                                                                                                                                                                        |
| `%t`        | local time `HHMMSS`                                                                                                                                                                                                                                                                                                                                                                          |
| `%%`        | one literal `%`; any other `%x` is kept verbatim                                                                                                                                                                                                                                                                                                                                             |

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

## Screenshot history archive (on by default)

Every image whose **primary delivery completed** gets, in addition to the output the user asked for, an independent
copy in `history\YYYY-MM-DD\` (one folder per local date) **inside the directory of the `ECAPTURE.EXE` that is
actually running**. The development build lives in `build\`, so history taken while developing is in
`build\history\`; install it into a custom folder and the history is in that folder — it follows the program itself,
not the working directory, not the directory of `--out`, and not any fixed default install location.

| Item | Behaviour |
| ---- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Source | the **already encoded bytes** of the primary delivery: no re-capture, no re-encode, and no reading the primary output file back (another program may be changing it), and no hard link. Deleting or overwriting the primary image afterwards leaves the history copy alone. |
| Entry point | window, whole screen, `--all` / `--monitor all` batches, `--roi` / `--client-area`, `--scale`, all five encodings and `--out -` all go through this one archive entry point. |
| When | the folder is created and the file written only after a real capture's primary delivery succeeded. `--capabilities` / `--diagnostics` / `--screens` / `--list` / `--inspect` / `--help` / `--version`, a bad command line, no match, `--dry-run`, a human answering "No", and any run that produced no valid image create not one directory and do not probe whether that location can be written. |
| Naming | one archive decision reads the local clock **once**, and both the folder name and the file name come from it (so crossing midnight cannot put today in the directory and tomorrow in the name): `YYYYMMDD-HHMMSS-<PID>-<token>-<seq>.<ext>`. The extension follows the container this image was actually encoded as. The file name holds no window title, no device name, no user name and no path of any kind. |
| Commit | reuses the production "never replace" atomic rename: exclusive creation; on a name that already exists it takes the next sequence number and tries again, and when none is left it reports `history.file_exists` truthfully. **It never overwrites an existing history file**, so neither a concurrent capture from another process nor a clock rolled backwards can replace an image already saved. |
| Relation to the primary delivery | two deliveries in two places (possibly on different volumes), each with its own verdict; no all-or-nothing transaction across those two is promised. While the primary output is not finished no copy is published (a half-emitted stdout stream does not count as finished either); when the primary image landed and the copy failed, the primary is not deleted, the stdout already emitted is not rolled back and nothing is re-captured, `images` and `captured` stay as they are, and the exit code is the partial-success `7`. |
| Retention | no rotation, nothing deleted by age or by size, no background scan, no upload. History is persistent screenshot data, not a cache that can be rebuilt at will: `.\clean.ps1` and `.\build.ps1 -Clean` clear build outputs but **preserve `build\history`** (when it cannot be preserved safely they refuse and say why, rather than silently deleting or moving it), the installer neither carries it nor lists it in a release manifest, and neither upgrade nor uninstall touches it. Pruning is the user's own explicit job. |
| Changing the install directory | the archive follows whichever exe ran at the time, so changing folder **does not** migrate the old history; that tree stays in the old folder, and to find it again read `history.location` from `--capabilities` against the old location. |

The member in the result is `images[].history` (a delivery fact, so `--quiet` cannot hide it):

| Key | Meaning |
| -------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `status` | `saved` = the copy was committed exclusively; `failed` = it was started and never committed; `skipped` = writing a copy never started for this one |
| `file`   | appears only with `saved`, and then that file really is on disk; on failure no "name we would have used" is passed off as an existing copy |
| `code`   | the stable code when `failed` / `skipped`: `history.unavailable` / `history.write_failed` / `history.file_exists` / `history.budget_spent` / `history.same_file` |

The details of a failure (the raw Win32 value, which route could not be completed) are on the `errors` entry carrying
the same code, whose `stage` is `history`; the two `skipped` cases have no "which step failed" to report (that run
never started), so they are stated in this field only.

- `history.budget_spent`: the `--timeout-ms` automatic budget is one allowance shared by the whole batch; archiving
  does not draw a separate one and leaves no background writer that nobody can wait for. For a run whose deadline was
  crossed right after the primary image landed, the copy records this code and **the primary image and its delivery
  facts stay exactly as they were**.
- `history.same_file`: when `--out` writes straight into the `history\` tree and the planned name is precisely the
  archive name this decision would use, no copy is written — writing it would overwrite the image that was just
  delivered. Want both? Put the primary output somewhere else.
- When the primary output sits inside the `history\` tree under a *different* name, the copy is still saved under its
  own separate name: neither a self-overwrite nor a copying loop, because the copy's bytes come from the encoding
  buffer in memory and never read the primary output file.

The confirmation dialog states, before a person answers, that the program will also save a further persistent copy in
its own directory (a route for which that archive cannot be stated coherently leaves the sentence out, instead of
promising a copy that would never exist). The `history` section of `--capabilities` describes these rules themselves, not the
outcome of any one run — it creates no directory and attempts no write, which is why `caveats` always carries
`history_root_writability_not_probed`. An archive failure never changes the authorization tiers, and nothing is
elevated, ACL-changed or re-labelled to make archiving succeed.

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

## Boundaries and unverified items

One place that answers "what does this tool _not_ promise". The per-feature sections explain why each line exists;
this list is what an automated caller should treat as a residual risk instead of a bug.

**Design boundaries, not defects:**

- The confirmation dialog is a plain `MessageBox`. It is cooperative misuse prevention: it cannot tell a human from a
  script and it is not an OS security boundary. What it does guarantee is that a caller following these rules gets
  asked at least once per desktop route.
- Checking and capturing are never one atomic operation. Identity and topology re-checks shrink the window for an
  `HWND` swap, they do not close it; Windows has no waitable-handle or pin-the-window API to do that with.
- The deadline budget bites at interruptible points and by stopping the helper process. A budget that expires during
  an atomic commit lets that file land — nothing is rolled back or deleted on timeout. The consent deadline is polled
  (about a 3 s close grace), not preempted.
- `cursorEffective` stops at "this session was set to draw a pointer" or "this source holds none". The SDK's session
  interface exposes no read-only `IsCursorVisible`, so no pixel-level claim about a pointer being visible is ever
  made, and no pointer is ever drawn into or erased from a frame.
- `--hdr tonemap` always delivers 8-bit SDR (`encoderOutput: "sdr_bgra8"`): there is no native-HDR or 10-bit output
  path in this build, and in this build only the two `wgc` paths honour an explicit `tonemap` / `refuse` request —
  the duplication surface is `wide_gamut_unverified`, so it is refused rather than guessed at.
- `duplication` captures one output at a time: a window straddling two monitors is taken where it overlaps the larger
  overlap and comes back with `clipped` + `note.capture_clipped`. Stitching one window across adapters is not
  implemented. RDP and virtual GPUs frequently yield no duplication frame at all.
- DRM and protected content is always black, and some players' driver-level black bars survive some channels. A
  single-colour result is reported (`note.frame_uniform`) and still delivered, never refused as a failure.
- The declared support floor (build 18362) and the tested environment (build 19045, x64) are different numbers, and
  `verifiedOnThisMachine` compares this machine against the second one — it is an environment match, not a per-device
  test record. The binary cannot even load on Windows 7, and on Windows 8.1 it loads but has no encoder to use.
- The primary output and the history copy are **not one cross-volume transaction**: they are two independent
  deliveries, possibly on different volumes, so each side reports its own result. A failed copy never deletes the
  primary image, never rolls back stdout that was already emitted, and never re-captures to "complete" the copy; in
  reverse, an unfinished primary delivery publishes no copy. Archiving draws no separate budget and runs no unbounded
  background writer. History is not rotated or deleted automatically, and this version offers no switch to turn it
  off — what it gives is exactly "one extra copy by default" plus dependable failure semantics.

**Recorded as unverified rather than inferred** (each row is what the named test reports as SKIP / "not verified" on
this development machine; none of them is claimed as a pass):

| Not verified here                                                                                                                       | Why, and where the gap is recorded                                                                                                                          |
| --------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Real HDR capture, `refuse` on a real HDR frame, the FP16 frame pool, HLG end to end                                                     | no HDR display attached; the mapping math is judged offline (`tests\hdr.ps1`, `build\ecapture-hdr-tests.exe`), `color.verifiedOnThisMachine` is always `no` |
| HDR combined with `--scale`, and mixed DPI across monitors for `--roi` / `--scale`                                                      | one monitor only, and no HDR mode to enable (`tests\scale.ps1`, `tests\crop.ps1`)                                                                           |
| A delivered frame whose side exceeds 16384 on the device; rotated panels; hot-unplugging a monitor                                      | no such window can be staged and displays are never re-arranged by a test (`tests\image.ps1`, `tests\dup.ps1`)                                              |
| Deliberate `HWND` / PID recycling, and the identity span across a real consent dialog                                                   | would mean killing someone else's process, and the dialog answer comes from a person (`tests\identity.ps1` needs `-SimulateConsent`)                        |
| Pixel-level "the pointer is or is not visible in this picture"; machines below build 19041                                              | the SDK gives no pixel-level answer and this machine is newer (`tests\cursor.ps1`)                                                                          |
| Windows builds other than 19045 · ARM64 · Server · Remote Desktop · a session with no interactive desktop · a genuinely missing encoder | a second OS cannot be arranged here; the judgements are made against injected fake builds offline instead (`tests\compat.ps1`, `tests\capabilities.ps1`)    |
| `tiff` and `gif` under `--scale`, and every other encoder-only behaviour                                                                | `tests\scale.ps1` checks `png` / `bmp` / `jpeg`; the two others share the encoder call but are not covered                                                  |
| `--capabilities` probing by producing an image, and the consent dialog being actually displayed                                         | probing either would be doing the thing it only reports on (`encoder_state_not_probed`, `consent_dialog_state_inferred_not_probed`)                         |
| The history copy meeting a genuinely full disk or a genuinely refused permission                                                       | staging those two would change this machine's storage settings or its ACLs, which this task does not allow; the offline layer judges the **classification** (the win32 value and the stable code that call returned go into `errors` unchanged, not folded into another cause and not overwritten by a deadline seen afterwards), while the on-device layer uses the two situations that can be created honestly: "that name is a file" and "that name is a reparse point" (`tests\history.ps1`) |
| A real person answering "No" creating no history                                                                                       | the judgement never opens a real dialog and never answers one on anybody's behalf. "No copy is published unless the primary delivery succeeded" is judged offline (both the half-emitted-stdout and the write-failure cases assert the archive was never called), while "the directory really gained nothing" is covered by `tests\consent.ps1` and by section 7 of `tests\history.ps1` respectively |

## Guide for AI and scripts

The tool is designed for programmatic calls; following these conventions is the cheapest way to use it. The
repository also ships a skill that teaches an agent to drive it: `.agents/skills/yashi-evernight-capture/` (contains
`SKILL.md`, `references/cli-contract.md`, and a copy of the exe). Installing the skill (see "Install") puts it at
`%UserProfile%\.agents\skills\yashi-evernight-capture` by default; an earlier release called it `ecapture-screenshot`.

1. **Ask the capability question first, then discover windows with `--list` / `--inspect`, then capture for real.**
   `--capabilities` is read-only - no pixel taken, no consent dialog, no file written - so it never disturbs anybody
   and belongs at the front of an automation flow. It hands back this machine's version, session and screen topology,
   every route as `available` / `unavailable` / `unverified`, every format, exactly which internal paths `--yes`
   covers, and the value ceilings. That answers "which `--capture` should I ask for", "is this failure about the
   channel or about the machine" and "would a dialog here ever be answered" _before_ you take an image. For a bug
   report run `--diagnostics` (same judgements, plus a checkable build id; `-v` expands each raw answer). Neither
   document changes with `--lang` (all ASCII), so comparisons stay stable. `available` is **not** "this window will
   capture": drivers, protected content and HDR are outside that layer, and the `caveats` array says so.
   Then `--dry-run` as usual: it takes no frame, writes no file and shows no dialog; candidates are in
   `notes[0].value`, shaped like `hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=…`.
   `--dry-run` does not need an output path either (with none it just means "nothing to deliver", so the JSON goes
   to stderr with `note.output_defaulted_stdout`); **`--dry-run` alone with no window condition = text help +
   exit code 2**.
   When you need the _list_ rather than one line of prose, use `--list` (structured, paged, several matches are not
   an error, zero matches is an empty list + exit `0`) and `--inspect` (one window, and several matches stay an
   ambiguity - the tool will not pick one for you). Both read no pixel and open no dialog, and neither is changed by
   `--yes`. What they hand back is a snapshot: capture still re-checks the target identity, so pass the handle from a
   fresh `--inspect` rather than one cached from an earlier run. See the section
   《Structured window discovery and inspection》 above.
2. **Branch on `errors[].code`, never on `message` text** (that follows `--lang`) and never on whether you passed
   `--out` — the two ways of asking for stdout report the same codes. The codes an automated caller actually hits:

   | Codes                                                                                                                                                                                                                                                                                                                                                                                                                                                               | Exit | Next step                                                                                                                                                                                                                                                                                                                                                                                                                       |
   | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
   | `cli.invalid_number`, `cli.invalid_value`, `cli.invalid_format`, `cli.unknown_option`, `cli.unknown_language`, `cli.crop_conflict`, `cli.query_conflict`, `cli.window_query_conflict`, `cli.monitor_conflict`, `cli.monitor_selector_empty`, `cli.monitor_selector_kind`, `cli.stdout_multiple_targets`                                                                                                                                                             | 1    | fix the command line — nothing was captured, no dialog was shown, no file was written                                                                                                                                                                                                                                                                                                                                           |
   | `match.index_out_of_range`, `match.monitor_out_of_range`, `match.roi_out_of_range`                                                                                                                                                                                                                                                                                                                                                                                  | 1    | `hint` lists every candidate; the requested crop cannot fit the target as selected                                                                                                                                                                                                                                                                                                                                              |
   | `match.no_window`                                                                                                                                                                                                                                                                                                                                                                                                                                                   | 4    | widen the conditions, or the window is minimized (minimized windows are never captured)                                                                                                                                                                                                                                                                                                                                         |
   | `match.ambiguous_window`, `match.monitor_ambiguous_id`                                                                                                                                                                                                                                                                                                                                                                                                              | 5    | several things match and the tool will not pick one — disambiguate with `--index` / `--topmost-match` / `--all`, or with a `--screens` identifier                                                                                                                                                                                                                                                                               |
   | `match.monitor_unknown_id`                                                                                                                                                                                                                                                                                                                                                                                                                                          | 4    | that identifier is not on the desktop any more; run `--screens` again                                                                                                                                                                                                                                                                                                                                                           |
   | `capture.access_denied`, `capture.consent_timeout`, `capture.consent_unavailable`                                                                                                                                                                                                                                                                                                                                                                                   | 6    | a human boundary: the answer was not Yes (on this `MB_YESNO` box the only refusal a person can give is "No"), nobody answered in time, or no interactive desktop. The rest of the request was not attempted — asking again is a new request, not a retry                                                                                                                                                                        |
   | `match.timeout`, `capture.worker_failed`, `capture.failed`, `capture.frame_timeout`, `capture.window_gone`, `capture.frame_invalid`, `capture.roi_invalid`, `capture.roi_unmeasurable`, `capture.monitor_changed`, `capture.monitor_unverifiable`, `capture.monitor_id_unverifiable`, `capture.consent_stale`, `capture.timeout`, `capture.hdr_refused`, `capture.hdr_unverifiable`, `capture.target_gone`, `capture.target_changed`, `capture.target_unverifiable` | 7    | read `stage` (`match` / `capture` / `encode`) before deciding; a fresh `--list` / `--screens` is usually the next call, not another `--capture`                                                                                                                                                                                                                                                                                 |
   | `env.os_too_old`, `env.channel_unsupported`, `env.hdr_unsupported`, `env.cursor_unsupported`                                                                                                                                                                                                                                                                                                                                                                        | 7    | this **machine** cannot do what was asked — see the paragraph below                                                                                                                                                                                                                                                                                                                                                             |
   | `io.write_failed`, `io.file_exists`, `io.output_collision`, `io.timeout`                                                                                                                                                                                                                                                                                                                                                                                            | 8    | create the directory, or pick a name that cannot collide; `io.file_exists` is `--no-overwrite` doing its job; the `io.timeout` here is the write that never got to start — when a file _did_ land and only the post-write deadline re-check crossed, that image stays in `images` and the run reports exit `7` (delivered + error) instead, see [Deadlines](#deadlines-and-calls-that-block---timeout-ms----consent-timeout-ms) |
   | `history.unavailable`, `history.write_failed`, `history.file_exists`, `history.budget_spent`, `history.same_file` | 7 | **the image WAS delivered, only that copy did not land** (the last two mean this run never started writing a copy at all). `images[].history.status` plus the `errors` entry with the same code (`stage=history`) say which route; **do not re-capture for it** — re-capturing neither supplies the copy nor avoids asking a person for one more picture. What to change is that location (writable, a directory, not a reparse point), not the command line |
   | `capture.hdr_unsupported`, `capture.cursor_unsupported`, `capture.unsupported`                                                                                                                                                                                                                                                                                                                                                                                      | 1    | that option pair was refused while parsing, before any dialog                                                                                                                                                                                                                                                                                                                                                                   |

   The four `env.*` codes are about this machine, not about the target, so retrying the same window is pointless:
   `env.os_too_old` means the Windows build is under the one encoder every format uses (changing `--capture` changes
   nothing), and `env.channel_unsupported` / `env.hdr_unsupported` / `env.cursor_unsupported` mean the requirement that
   was stated explicitly cannot be met by what this build and this OS offer — the tool will not substitute a channel
   to make the error go away. `note.channel_unavailable` says the same about one link of an `auto` chain that was
   dropped while the rest still captured. Read `input.osBuild` / `input.captureChain` with `--verbose` to ask which
   channels this machine offers before capturing anything. See
   [System support](#system-support) for the floors and for what has actually been tested.
   Every error also carries `target` / `backend` / `stage` / `hresult` / `win32` (see the output rules above) as far as
   that step really had them, so there is no need to dig values out of `message`.

3. **Read the right stream**: with `--out <file>` the JSON is on stdout and stderr is empty, so parse stdout
   directly. With `--out -` (or no output path) the image bytes occupy stdout and the whole JSON moves to stderr.
   stdout hands over one image at a time, so write several targets to files. Keep the two streams apart in the shell
   too, and know what your shell does to bytes: `cmd` and PowerShell 7.4+ preserve a native command's stdout byte
   stream, Windows PowerShell 5.1 does not (it re-encodes stdout as text and renders native stderr through its own
   error records), so from 5.1 ask for a file, or use `Start-Process -RedirectStandardOutput` / `-RedirectStandardError`,
   or run the command under `cmd /c`. `2>&1` is never the answer for an image. Details and the measured numbers are in
   [Writing image bytes to stdout, per shell](#writing-image-bytes-to-stdout-per-shell).
4. **Do not treat a non-zero exit code as total failure**: on partial success `captured` is greater than 0 while the
   exit code is 7, the images already on disk are perfectly usable, and `images[].source` / `path` / `scope` name the
   channel, the route inside it, and whether that frame is the window's own pixels or desktop pixels.
   `images[].history.status` is **a different question again**: that image was delivered, while its history copy may
   be `failed` or `skipped`. Use the picture as it is and **do not re-capture** (a re-capture neither supplies the
   copy nor comes free — it asks for one more approval); what needs attention is whether that location can be written
   at all, or accept that this one has only its primary output. See
   [Screenshot history archive](#screenshot-history-archive-on-by-default).
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

## Install (Windows, no developer tools needed)

Running the program needs no toolchain at all - no Visual Studio, no CMake, no source tree. Those are only needed to
*build* an installer, which is the next section's job.

### 1. Get the installer

The repository root script `build-installer.ps1` produces
`build\installer\EvernightCapture-<version>-<arch>-setup.exe` and writes a `.sha256` next to it. **This project has no
release attachment yet, so there is no download URL to quote here** - until there is one, an installer has to be
produced from a checkout, and the file should be checked against its `.sha256` before it is run.

### 2. System requirements

- Windows 10 build **18362** or newer, **x64**. `ECAPTURE.EXE --capabilities` reports the floor this binary enforces
  itself (`os.declaredMinBuild`, plus `os.build` for the machine you are on); see "System support" above for what was
  actually tested.
- **No Visual C++ redistributable**: the runtime is linked statically, so there is nothing else to install.
- **No administrator rights**: setup installs for the current user only. It never elevates itself just because you
  chose a folder it cannot write to - it reports the failure instead of installing somewhere else.
- HDR, multiple monitors, and the desktop-route confirmation dialog are *runtime* features, not installer
  requirements; nothing about them changes what the installer needs.

### 3. Run the wizard and choose a folder

The default directory is `%UserProfile%\.agents\skills\yashi-evernight-capture`, and the wizard lets you pick any
folder. **The folder you choose *is* the skill root**: `SKILL.md` sits directly in it, with `ECAPTURE.EXE` beside it,
and no extra nested folder is created. Setup remembers the folder you chose when you upgrade.

```bat
:: Read-only self-check after installing (cmd.exe). Quote the path: the default contains no spaces, a custom one may.
powershell -NoProfile -ExecutionPolicy Bypass -File "%UserProfile%\.agents\skills\yashi-evernight-capture\verify-install.ps1"
```

```powershell
# PowerShell: the same thing, plus the optional post-install offline test run
& "$env:USERPROFILE\.agents\skills\yashi-evernight-capture\verify-install.ps1" -RunOfflineTests
# A custom install directory works the same way - pass it explicitly, do not assume the default:
& "D:\My Tools\ecapture\verify-install.ps1" -InstallDir "D:\My Tools\ecapture"
```

### 4. Read-only install self-check

`verify-install.ps1` (installed at the root of the chosen folder) needs **no Visual Studio, no source tree and no
network**. It checks, in order: every file listed in `install-manifest.json` for presence, size and SHA-256; that the
relative links in the four READMEs and `SKILL.md` resolve inside the install folder; that `ECAPTURE.EXE --version`
exits 0 with the declared version, that `--capabilities` / `--diagnostics` exit 0 as parseable JSON whose
`program.version` / `arch` / `buildId` match the manifest, and that `--help` exits 3 (that is its contract, not a
failure). With `-RunOfflineTests` it then runs the declared offline test set through the bundled `test-all.ps1`,
pointed at **this** folder's `ECAPTURE.EXE` - never at an older copy on `PATH` - with logs in the system temp
directory. Exit codes: `0` all passed, `1` something failed, `2` a precondition was missing (wrong folder, no
manifest). The real window / HDR / multi-monitor / consent-dialog tests are **not** run here; those stay a job for a
person in a suitable environment.

### 5. Let an AI tool find and use the skill

Three different things, and mixing them up is the usual reason "it does not work":

- **File installation** - where `SKILL.md` and `ECAPTURE.EXE` physically are. That is the folder you picked.
- **Skill discovery** - whether the tool finds that folder on its own. **Not every tool scans `.agents/skills`.**
- **Command execution permission** - installing a file grants no authority to capture anything. Everything in
  "Screenshot authorization and `--yes`" still applies; in particular a person still has to answer the desktop
  dialog.

**OpenCode** (checked against its official Agent Skills documentation): besides `.opencode/skills/<name>/SKILL.md` and
the Claude-compatible paths, it loads a global *agent-compatible* path `~/.agents/skills/<name>/SKILL.md` - on Windows
that is `%UserProfile%\.agents\skills\<name>\SKILL.md`, i.e. exactly the default folder this installer uses, so the
default install is discovered automatically at the next start. Its rules also require `name` to match the folder name
(here `yashi-evernight-capture`), so do not rename one without the other. **A custom folder is not auto-discovered** -
point the tool at it.

For a tool that does not auto-discover, hand it the folder explicitly (this is a pointer, **not** registration):

```text
Read the SKILL.md at <absolute path to the installed SKILL.md> and follow it. Call the ECAPTURE.EXE in that same
directory by absolute path, with the path quoted. Start with read-only checks (--version, --capabilities). Do not try
to prove the install works by taking a screenshot.
```

### 6. Upgrade, uninstall, troubleshooting

- **Upgrade**: run a newer setup and keep the same folder - it upgrades in place and remembers your choice. If the
  copy fails or is interrupted, setup rolls back rather than leaving a half-written folder.
- **Changing the folder** installs a second, independent copy. The old folder is left untouched (nothing is migrated
  or deleted for you); if both copies remain, both uninstallers are listed separately - remove the one you do not
  want, or point your tool at exactly one of them.
- **Unknown folder with the same name**: setup says `install-manifest.json` is missing and asks before adding its
  files; it never silently overwrites whatever was already there. An unattended run (`/VERYSILENT`) has nobody to ask,
  so it writes **no file at all** and stops with a non-zero exit code, recording the reason in its `/LOG` file.
- **Uninstall** removes only the files the installer recorded. Files you added, logs, and neighbouring skills stay;
  nothing recursively deletes the parent folders (`.agents`, `skills`) or your chosen folder's other contents.
- **Screenshot history** (`<install folder>\history\<date>\`, see
  [Screenshot history archive](#screenshot-history-archive-on-by-default)) counts as the same class as "files you
  added later" in the installer's eyes: it is not in the managed file list and not in the release manifest
  (`install-manifest.json` / `payload.sha256.txt`). **Upgrade, reinstall and uninstall never delete it, never modify
  it and never back it up as a user edit**; its presence does not make the installer treat the folder as one it does
  not own (ownership is decided by the ownership marker alone). After uninstall the history is still there in that
  folder, and the location is stated on the finish page and in this README; to clear it, delete that tree yourself
  explicitly.
- **History does not migrate when you change the install folder**: the old pictures stay beside the old folder's own
  `history\` (the archive follows the exe that ran at the time). To keep using the old batch, move it over yourself
  or let the tool know about both locations.
- **A file is in use**: setup reports the locked file and rolls back. It does not kill a running program to force the
  install through, and it does not claim success.
- **The install folder is blocked by a security policy**: when the chosen folder sits under a policy that forbids
  programs living in it from creating a temporary directory (common with endpoint protection), an in-place uninstall
  fails with `Setup was unable to create the directory "…-uninstall.tmp". Error 5`, and the bundled `delivery` layer of
  the self-test fails its "temporary directory can be created" checks. That is the environment refusing, not the
  product misbehaving: install into a different folder, or have an administrator allow this one. The read-only part of
  `verify-install.ps1` (file hashes, document references, binary identity) is unaffected and still checks out.
- **"It installed but the AI ignores it"**: confirm the folder is one the tool actually scans (see above), then check
  the binary identity rather than the version string - compare `install-manifest.json`'s `version`, `arch` and
  `buildId` with `ECAPTURE.EXE --capabilities`. An older `ECAPTURE.EXE` next to a source checkout can be stale; a
  freshly installed one cannot be, because it comes from the installer's own build.

## Build and test (maintainers)

| Command                     | What it proves                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| --------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `.\build.ps1`               | Release build, output `build\ecapture.exe`; `-Config Debug` and `-Clean` available                                                                                                                                                                                                                                                                                                                                                                                                             |
| `.\tests\cli.ps1`           | The output contract: parse errors and exit codes, every `--yes` / `--no-overwrite` boolean form, the query-versus-capture conflict groups, stream separation, "no `--out`" equivalence with `--out -`, and the four message languages. Runs `--dry-run` only — no capture, no file                                                                                                                                                                                                            |
| `.\tests\windows.ps1`       | `--list` / `--inspect`: paging, the visibility policy, field-level readability, document shape, privacy, ambiguity, and that the query really touches nothing (offline `build\ecapture-windows-tests.exe` plus self-made windows)                                                                                                                                                                                                                                                              |
| `.\tests\screens.ps1`       | `--screens` and `--monitor=device:` / `id:`: which identity is stable how far, that a reported selector selects that panel, and that an unknown identifier never degrades to the primary monitor                                                                                                                                                                                                                                                                                               |
| `.\tests\capabilities.ps1`  | `--capabilities` / `--diagnostics` against fake probes (no screen at all, just under a channel's floor, unreadable build number, one encoder missing, the `--yes` scope versus the registry, both queries from one judgement set), plus the real-machine half: the query never blocks on a dialog, writes nothing, agrees with WMI and with `--dry-run -v`, and is all ASCII so `--lang` cannot change it                                                                                      |
| `.\tests\compat.ps1`        | The version floors: fake Windows builds injected into the production gate, a gated explicit channel never being substituted, which link an `auto` chain loses, and the shipped binary's Windows 8-era WinRT / job API-set imports                                                                                                                                                                                                                                                              |
| `.\tests\channels.ps1`      | Six channels against its own windows, with an occlusion control. Window-content routes run with `--yes` and fail if a dialog appears; `bitblt` / `duplication` image judgements need `-SimulateConsent`                                                                                                                                                                                                                                                                                        |
| `.\tests\wgc.ps1`           | WGC live sizing: content size versus texture size, frame-pool rebuild, and never a clipped frame reported as the whole window (offline `build\ecapture-wgc-tests.exe` plus a window it resizes)                                                                                                                                                                                                                                                                                                |
| `.\tests\image.ps1`         | Frame-shape validation and pixel operations over hand-built layouts (stripes, checkerboard, alpha, row padding, over-large and short buffers, out-of-range crops) plus on-device single-colour captures                                                                                                                                                                                                                                                                                        |
| `.\tests\crop.ps1`          | `--roi` / `--client-area`: geometry against three independent Win32 questions and against pixel content, refusal before any dialog or file, and a tiny crop on a desktop route still asking                                                                                                                                                                                                                                                                                                    |
| `.\tests\scale.ps1`         | `--scale`: no upscaling, the tightest ceiling, rounding down, the nearest-neighbour mapping point by point, the crop-then-scale order, and `png` / `bmp` / `jpeg` writing the scaled size                                                                                                                                                                                                                                                                                                      |
| `.\tests\dup.ps1`           | Desktop duplication: the four rotations checked against the production geometry judges, a fake two-adapter output table, `requestedRect` / `capturedRect` / `clipped` / `rotation` on real images, and the "that monitor changed after the confirmation" cases. Displays are never re-arranged or re-oriented                                                                                                                                                                                  |
| `.\tests\consent.ps1`       | The tier table and how a refusal travels: the whole `ConsentGate` state machine offline against an injected fake prompt, and every dialog answered "No" on-device — which paths must ask, what a refusal reports, that nothing lands on disk. Never answers "Yes" on a human's behalf                                                                                                                                                                                                         |
| `.\tests\identity.ps1`      | Identity re-check grades in the order they run, `capture.target_gone` / `capture.target_changed`, and z-order selection (`--topmost-match` / `--bottommost-match`)                                                                                                                                                                                                                                                                                                                             |
| `.\tests\timeout.ps1`       | The deadline and the helper process: one budget that later steps can only spend the remainder of, the worker wire format rejecting anything malformed instead of "looking like success", and the helper mode refusing to be launched as a public option                                                                                                                                                                                                                                        |
| `.\tests\isolation.ps1`     | Ownership: a same-named process it did not start stays alive and is never the target, two concurrent runs do not cross, an aborted run cleans up only itself                                                                                                                                                                                                                                                                                                                                   |
| `.\tests\cursor.ps1`        | `--cursor`: the per-path registry, how the chain narrows on both sides of the 19041 line, how `cursorRequested` / `cursorEffective` / `cursorBasis` are composed, and a source scan that fails if any pointer-shape fetching, cursor drawing or pointer-moving call ever appears                                                                                                                                                                                                               |
| `.\tests\hdr.ps1`           | `--hdr`: DXGI format and display color-space classification (unrecognized stays `unknown`), the tone-curve properties, `ConvertWideFrameToSdrBgra8` point by point, the result-key group, and the honest SDR on-device case (keys absent by default, `note.hdr_source_sdr` when processing was asked of an SDR frame)                                                                                                                                                                          |
| `.\tests\save.ps1`          | File delivery: every `--no-overwrite` boolean form against a real file, batch name planning and collision detection, atomic commit against a locked target / a directory / a missing directory / a killed run, and the concurrent `--no-overwrite` race                                                                                                                                                                                                                                        |
| `.\tests\history.ps1`       | Screenshot history archive: the offline layer against the production `HistoryArchive` + `Delivery` + `FileSave` — naming and the local date from one clock reading, two images in the same instant / a clock rolled back / crossing midnight, exclusive commit on a taken name and the retry under the next one, failure classification per route, no self-overwrite when the planned primary name sits inside the history tree, plus "primary succeeded and the copy failed", "a failed primary and a half-emitted stdout publish no copy", "the copy never starts when the budget is spent after the primary landed" and "an exception from the archive does not erase the delivery facts" (`build\ecapture-history-tests.exe`); the on-device layer copies the exe into a per-run folder and checks with self-made windows that the landing point follows the program itself, that primary and copy match byte for byte / by SHA-256, the batch and the `--out -` route, that read-only and argument failures create no history at all, and the partial-success `7` at a reparse point and when "that name is a file" |
| `.\clean.ps1`               | Shares the one judgement in `scripts\build-clean.psm1` with `.\build.ps1 -Clean`: clears the outputs under `build\` but **preserves `build\history`** beside the development exe (real screenshot data, not a build artifact); refuses, with the reason stated, when `build\` or `build\history` is a reparse point or when that name is held by a file, never silently deleting or moving it (`tests\history.ps1` section 10 checks that same body) |
| `.\tests\screen.ps1`        | Whole-screen capture over the three desktop routes with a red-block placement and a negative control. Only `-SimulateConsent` answers, and only on a desktop dedicated to testing                                                                                                                                                                                                                                                                                                              |
| `.\tests\smoke.ps1`         | End to end: capture its own test window, validate PNG size and pixel content                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `.\tests\invoker.ps1`       | The shared test invoker itself: argv quoting, both streams drained at once, binary output, a hung child, per-run scratch dirs                                                                                                                                                                                                                                                                                                                                                                  |
| `.\tests\orchestration.ps1` | The `test-all.ps1` runner itself: an empty plan after filtering is reported instead of passing, a planned suite that never started (missing script, unparsable source) fails the exit code, while deliberate exclusions, `-StopOnFail` remainders and each suite's own SKIP stay separate                                                                                                                                                                                                      |
| `.\tests\build-path.ps1`    | Building from a directory holding CJK text, spaces, parentheses and `%` (plus a CJK `%TEMP%`), and that the temporary batch body stays ASCII                                                                                                                                                                                                                                                                                                                                                   |
| `.\tests\streams.ps1`       | Stream and result reliability: one image on stdout for a single target, a batch that resolves to several targets refused before the dialog, the judgement using the number of targets actually hit, images already captured when a batch fails halfway staying in `images`, a result that cannot reach the agreed stream giving exit code `8`, and the no-`--out` / `--out -` equivalence across success, no match, ambiguity, bad arguments, a backend failure, a refusal and a broken stdout |
| `.\tests\window_shot.bat`   | Human walkthrough: every channel against a compiled test window, a person clicking the dialogs, then the whole-screen step                                                                                                                                                                                                                                                                                                                                                                     |
| `.\scripts\check-lang.ps1`  | The four string tables align on keys and placeholders, and the exe really carries four resources                                                                                                                                                                                                                                                                                                                                                                                               |
| `.\scripts\mkreadme.ps1`    | Regenerates the help block of all four READMEs from each language's `--help`; `-Check` fails instead of writing, which is how a stale block gets caught                                                                                                                                                                                                                                                                                                                                        |

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

### One entry point: `test-all.ps1`

`test-all.ps1` orchestrates the suites above; it contains no judgements of its own. It builds first (unless told not
to), then runs the suites **serially** through the shared `tests\harness.psm1` invoker, and prints one table plus the
log directory.

```powershell
.\test-all.ps1                       # build Release, run everything
.\test-all.ps1 -Quick                # skip build-path.ps1, and timeout.ps1's long waits
.\test-all.ps1 -Only cli,windows     # just these
.\test-all.ps1 -Except build-path    # everything except the slowest
.\test-all.ps1 -Offline              # only each suite's offline layer
.\test-all.ps1 -List                 # print the plan; do not build, do not run anything
.\test-all.ps1 -NoBuild              # use the existing build\ecapture.exe
.\test-all.ps1 -Exe D:\tools\ECAPTURE.EXE   # or an explicit binary
.\test-all.ps1 -StopOnFail           # stop at the first suite that fails
```

- **`-List` only previews.** It prints the plan and starts nothing - a filter that ends in no runnable suite is
  reported as a missing precondition, and "empty plan" is never silently a success.
- **`-Offline`** routes by per-suite metadata, not by name. Purely offline suites (read-only CLI queries, pure
  functions and protocols, temporary files) run as usual; mixed suites get the switch they declare
  (`-SkipReal` / `-OfflineOnly`), typed into their command line **before anything starts** - a mixed script that would
  otherwise open a window never gets the chance; suites that are real-only and declare no offline entry are removed
  from the plan up front and each one gets a reason in the summary (`OFFLINE-EXCLUDED`), and not one byte of them is
  started. A suite that claims an offline entry but does not accept it is a metadata mismatch: it does **not** run and
  the exit code fails - it is never quietly degraded into running the real path, and never booked as a reasonable
  SKIP. `-Offline` may still require the Windows toolchain (one suite compiles a helper window program with the
  bundled .NET Framework `csc` in a scratch folder); it is not a portable/offline-in-the-network sense claim.
- **Consent is never assumed.** `-Force` only skips the "press Enter to start" pause: it **cannot** approve a real
  desktop confirmation, and it cannot be combined with the acceptance switches. `-Offline` together with
  `-SimulateConsent` / `-TimeoutConsent` is refused before anything executes (`OFFLINE-CONSENT-CONFLICT`), and so is
  answering a dialog by name that the target suite does not accept (`-TimeoutConsent` is only valid for the suite that
  documents it). Simulated consent needs `yes` typed at the prompt: with redirected input, no console or a
  non-interactive host, the run stops as *not approved* instead of defaulting to agreement.
- **Exit codes**: `0` = every planned suite ran and passed (a suite's own SKIP is unverified, not a failure); `1` =
  at least one FAIL / TIMEOUT / NO EXIT / NO START, **or a suite the plan named never started** (missing script,
  unparsable source, offline metadata mismatch); `2` = a precondition was missing (build failed, no binary, unknown
  option, no runnable test after filtering, conflicting switches, or another run already holding the lock). A build
  failure ends with code 2 and never reuses a stale artifact.
- **The summary counts only real results.** "Delivered a result" means the suite actually produced a final state:
  NO START / NO EXIT are not counted as delivered. Three kinds of "did not run / not verified" stay separate and are
  never merged into one number: deliberately not planned (`-Quick` / `-Only` / `-Except` / `-Offline`), the remainder
  after `-StopOnFail`, and suites that unexpectedly never started (only the last kind fails the exit code). A suite's
  own SKIP stays an environment/safety boundary - it is neither promoted to a failure nor booked as a pass.
- **Logs** go to `build\test-logs\<timestamp>\` (one file per suite plus a combined one); `build\` is gitignored.

### Building the installer: `build-installer.ps1`

The root script is meant to be called by absolute path from any working directory; it locates the repository from its
own location, never from the current directory.

```powershell
# PowerShell (maintainer machine, from anywhere)
& "$env:USERPROFILE\src\EvernightCapture\build-installer.ps1"                 # Release build + installer
& "$env:USERPROFILE\src\EvernightCapture\build-installer.ps1" -Clean          # clean rebuild first
& "$env:USERPROFILE\src\EvernightCapture\build-installer.ps1" -StageOnly      # build + collect/verify payload, no compiler needed
& "$env:USERPROFILE\src\EvernightCapture\build-installer.ps1" -SkipBuild      # reuse build\, identity still verified
```

```bat
:: cmd.exe - the same, quoted; %UserProfile% is expanded by cmd, not by the script
powershell -NoProfile -ExecutionPolicy Bypass -File "%UserProfile%\src\EvernightCapture\build-installer.ps1"
```

- **It builds first and stops on failure** (exit code 11): `build.ps1` with `-Config` (default `Release`), producing
  the binary *and* the test programs. `-SkipBuild` is an explicit request to reuse `build\`, and it still checks the
  source of truth (version from `src\Version.h`, PE architecture, `program.buildId`) - a failed build never falls back
  to an old artifact, and "the file exists" is never treated as "the current build succeeded".
- **Payload collection** follows `installer\payload.manifest.json` into a private staging directory under `build\`:
  `ECAPTURE.EXE`, `SKILL.md` + `references\`, all four READMEs, `LICENSE`, `resources\icon.ico`,
  `verify-install.ps1`, `test-all.ps1`, `tests\` (scripts, `harness.psm1`, helper source, batch walkthrough) and the
  test binaries under `build\`. A content guard rejects source files, developer-only `AGENTS.md` / `MEMORY.md`,
  logs and screenshots, and the check refuses to stage a different `ECAPTURE.EXE` than the one just built.
- **The staged manifest** `install-manifest.json` (plus `payload.sha256.txt`) records version, architecture, config,
  `buildId` and the SHA-256 of every file, which is what the installed `verify-install.ps1` checks against.
- **The compiler** is Inno Setup 6.3 or newer (the script needs `ArchitecturesAllowed=x64compatible`). It is found on
  `PATH` or in the usual installation folders, or given explicitly with `-IsccPath "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"`.
  The script never downloads or installs a tool: without a compiler it prints what is missing and exits `32`
  (`-StageOnly` still works and needs no compiler at all). `build.ps1`'s own dependency is Visual Studio with the C++
  desktop workload, located through `vswhere`.
- **Output**: `build\installer\EvernightCapture-<version>-<arch>-setup.exe` plus `<...>.exe.sha256`. Version comes from
  `src\Version.h` (there is no second hand-written version), the name carries version and architecture, and the
  printed summary repeats the path, version, `buildId` and SHA-256. `build\` (staging, installer, logs) is gitignored
  and contains no tracked release artifact. Timestamped artifacts are not byte-for-byte reproducible and are not
  promised to be.
- Neither script asks for administrator rights and neither changes the machine's execution policy; the examples above
  use `-ExecutionPolicy Bypass` for one process only, which is not a global setting.

## License

EvernightCapture is licensed under the [Mulan Permissive Software License v2 (Mulan PSL v2)](http://license.coscl.org.cn/MulanPSL2).
The complete bilingual (Chinese and English) text is in [LICENSE](LICENSE).

```
Copyright (c) 2025 KagurazakaYashi (KagurazakaMiyabi)
EvernightCapture is licensed under Mulan PSL v2.
```
