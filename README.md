<div align="center">

<img src="assets/logo.png" width="128" height="128" alt="EvernightCapture logo">

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
- **Screenshot authorization**: any capture that really grabs a frame asks in a modal dialog first, reliable
  window paths included; `--yes` skips that ask **only** for paths whose frame is bound to the selected window
  itself — anything sampling desktop pixels always needs a person and no switch can skip it
- **Four message languages**: `zh-CN` / `zh-TW` / `en` / `ja`, defaulting to the system display language, all
  embedded as resources inside the exe
- **Machine-readable JSON**: capture results and errors only — no tool name, version, schema or argument echo

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
  --monitor, -m [<n|primary|all>] Monitor number, 1-based (the order shown by Windows display settings); primary = main monitor, all = one image per monitor. With no window conditions it captures that whole screen; with window conditions only windows overlapping it are matched. The value may be omitted (= primary), and then nothing after it is eaten, so --monitor out.png still works. A whole screen is desktop pixels and always asks; --yes cannot skip that

Window match conditions (repeat one option for OR, combine different options with AND)
  --hwnd <handle>                 Window handle. Plain digits are decimal; a 0x prefix or a-f digits are hexadecimal - prefer 0x
  --pid <pid>                     Process id, decimal and greater than 0
  --process, -p <image-name>      Image file name (no path), case-insensitive; without an extension .exe is assumed
  --exe <full-path>               Full image path, case-insensitive
  --title, -t <exact-title>       Window title, exact match
  --title-contains, -T <text>     Window title contains this substring
  --title-regex, -R <regex>       Window title regular-expression match (ECMAScript), validated while parsing
  --class, -c <class-name>        Window class name, case-insensitive, e.g. Notepad / CabinetWClass

When several windows match (mutually exclusive)
  --index, -i <n>                 Take the n-th window, 1-based, ordered by visibility and z-order
  --newest                        Take the most recently created window
  --oldest                        Take the oldest created window
  --all, -a                       Save one image per matched window

Capture channel (default wgc; may fail because of the OS version or the window itself)
  --capture, -C <method>          wgc (default, works through occlusion) / dwm (DWM thumbnail, works through occlusion) / printwindow (window paints itself) / bitblt (copies visible screen pixels) / duplication (desktop duplication cropped to the rect) / auto (falls back wgc-dwm-printwindow-bitblt; a whole screen only uses wgc-duplication-bitblt)

Capture authorization (a real capture asks first; --yes skips window-content paths)
  --yes, -y                       Skip the confirmation for window-content paths (wgc / printwindow / the dwm thumbnail route). Anything reading the screen (bitblt, duplication, a whole screen, dwm screen fallback) always asks; --yes cannot skip it. --yes=false asks on purpose

Output
  --out, -o <path|->              Output path; the special value - writes the image bytes to stdout. A positional argument works too, and giving no path at all is the same as --out -. Every name for the batch is planned before any frame is taken: two targets resolving to the same name is an error, never a silent overwrite. stdout carries only one image per run, so a batch that resolves to more than one target is a parameter error and nothing is captured
  --format, -f <name>             Force the encoding format; otherwise it comes from the output file extension, and png when that fails too
  --quality <1-100>               JPEG quality, default 100
  --no-overwrite                  Fail instead of overwriting an existing target (no value means the prohibition is on). --no-overwrite=false (0 / no / n / off) cancels it; =true / 1 / yes / y / on means the same as giving no value. When repeated, the last one wins

Miscellaneous
  --dry-run, -d                   Parse and list candidate windows only - no capture, no file written
  --json, -j                      Deprecated compatibility switch, no effect: success and errors are already JSON
  --verbose, -v                   Add the input section to the JSON (all input, normalized) and keep notes
  --quiet, -q                     Drop notes; errors are always returned whatever this says
  --lang, -l <language>           Message language. auto (default, follows the system display language) / zh-CN / zh-TW / en / ja; unsupported system languages fall back to en
  --help, -h                      Print this text help
  --version                       Print version and stage

Syntax: --opt=value / -opt / /opt all work; when a value itself starts with - write --title=-x, or end option parsing with --
Output: success and failure are both JSON, holding only captured / images (plus errors / notes, and input only with --verbose)
       --help / --version and the no-conditions case are plain text
Exit codes: 0 success / 1 bad arguments / 2 no condition given / 3 --help / 4 no matching window / 5 several matches /
        6 target protected or refused / 7 capture failed / 8 write failed / 9 internal error
Current build: every --capture value is implemented (wgc / dwm / printwindow / bitblt / duplication, auto falls back wgc-dwm-printwindow-bitblt; a whole screen uses wgc-duplication-bitblt); the output directory must already exist

Examples:
  ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
  ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png
  ECAPTURE.EXE --pid 12345 --title-contains Report --all D:\shots\rpt_%i.png
  ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite out.png
  ECAPTURE.EXE --process notepad.exe --out - > snap.png
  ECAPTURE.EXE --monitor all D:\shots\screen_%i.png
  ECAPTURE.EXE --process notepad.exe --yes D:\shots\epad.png
```
<!-- END ECAPTURE-HELP -->

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

## Output format

`--help`, `--version` and the "no conditions given" case are plain text. Everything else is JSON carrying only the
capture result and the errors.

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

An error (`--hwnd` given a garbage value):

```json
{
  "captured": 0,
  "images": [],
  "errors": [
    {
      "code": "cli.invalid_number",
      "message": "--hwnd needs a valid handle (decimal, or hexadecimal with a 0x prefix)",
      "option": "--hwnd",
      "value": "zzz",
      "hint": "plain digits parse as decimal; write hexadecimal as 0x..., or it is taken as hexadecimal when it contains a-f"
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
   of the screen that route was authorized to sample; omitted only when it cannot be measured).
3. `code` values are stable: `cli.*` / `note.*` / `match.*` / `capture.*` / `io.*`, append-only, never renamed.
   Among frame failures, "the frame never arrived" (`capture.frame_timeout`) and "the target window is gone"
   (`capture.window_gone`) each have their own code instead of being lumped in with the generic `capture.failed` —
   the next step differs between them (wait and retry versus enumerate the windows again).
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
   pixels while `dwm.screen` (that channel's internal fallback) reads the screen.
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

## Exit codes

`0` success / `1` bad arguments / `2` no condition given / `3` `--help` / `4` no matching window /
`5` several matches / `6` target protected, refused on the confirmation dialog, or no dialog could be shown /
`7` capture failed / `8` write failed / `9` internal error.
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

## Capture channels

| Value | Channel | Covered window | Hardware-accelerated content | Minimum OS |
| --- | --- | --- | --- | --- |
| `wgc` | Windows.Graphics.Capture | yes (DWM cache) | normal | Win10 1803+ |
| `dwm` | DwmRegisterThumbnail | yes | mostly normal, protected windows black | Win7+ |
| `printwindow` | PrintWindow + PW_RENDERFULLCONTENT | yes (window self-draw) | often fully black | Win8.1+ |
| `bitblt` | BitBlt from a screen DC | no, visible pixels only | partly black | all versions |
| `duplication` | DXGI desktop duplication frame, cropped to the rect | no, visible pixels only | normal | Win8+; RDP / virtual GPUs often yield nothing |
| `auto` | falls back wgc → dwm → printwindow → bitblt | best effort | best effort | — |

- Want "the window's own content", even if something is on top of it: keep the default `wgc`. Want "what the screen
  looks like right now", occluder included: use `bitblt` or `duplication`.
- Whether a capture has to ask a person is decided by the route it really takes, not by the value you typed — see
  the next section.
- A bad `--capture` value fails during parsing with `cli.unknown_capture_method` (exit code 1) and **never degrades
  to the default channel**; only `auto` may fall back, and a successful fallback emits `note.capture_channel`.
- DRM / protected content is always black. Driver-level black bars (some players) are defeated by some channels and
  not by others — nothing is guaranteed.
- Whole-screen capture only uses `wgc` / `duplication` / `bitblt`; `--monitor` with `dwm` or `printwindow` fails
  during parsing with `capture.unsupported` (exit code 1). In screen mode `auto` falls back wgc → duplication →
  bitblt.

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
- Being refused is **not** folded into `cli.missing_output`, so you see "a person said no" even without an output
  path. Every other failure on that shortcut path still collapses into `cli.missing_output` + exit code `1` with the
  real reason held back, and the only other exception stays `cli.stdout_multiple_targets` — several targets wanting
  to share one stdout is a bad argument, and "missing output path" would only steer people towards adding `--out`.
- Several monitors plus stdout (`--monitor all --out -`) is refused before any dialog appears: one confirmation
  cannot buy "one image per monitor squeezed into the same stream". With only one monitor attached, `--monitor all`
  is a single target and that path still delivers one image on stdout.
- Honest limit: this is a plain `MessageBox`. It is mis-click protection for cooperative automation — it cannot tell
  whether a human or a script pressed the button, and it does not defend against a same-privilege process that
  intends to bypass it. What it does guarantee is that a caller following these rules gets asked at least once.

`--monitor` (value omitted) and `--monitor primary` are the main monitor, `--monitor 2` the second one,
`--monitor all` one image per monitor. Numbers follow the `EnumDisplayMonitors` order and start at 1; out of range
gives `match.monitor_out_of_range` (exit code 1) with every local monitor listed in `hint`. `--monitor <n>` together
with window conditions means "filter windows by monitor" (a window overlapping that monitor matches, and a window
spanning monitors matches on both), still producing window images, so the window rows of the table above apply.
`--monitor all` is mutually exclusive with any window **matching** condition (`cli.monitor_conflict`, exit code 1),
but disambiguation options such as `--all` and `--index` do not count as matching conditions and may accompany it.

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

1. **Probe with `--dry-run` first**, then disambiguate, then capture for real. `--dry-run` takes no frame, writes no
   file and shows no consent dialog; candidates are in `notes[0].value`, shaped like
   `hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=…`. Note that `--dry-run` still requires
   `--out`, otherwise `cli.missing_output` + 1; and **`--dry-run` alone with no window condition = text help + exit
   code 2**.
2. **Branch on `errors[].code`, never on `message` text** (that follows `--lang`). The codes you actually hit:
   `match.no_window` (4, conditions too narrow or the window is minimized), `match.ambiguous_window` (5, choose
   from the candidates in `hint`), `match.index_out_of_range` / `match.monitor_out_of_range` (1, `hint` lists all
   candidates), `cli.missing_output` (1), `cli.invalid_format` (1), `cli.stdout_multiple_targets` (1, several targets
   want to share one stdout), `capture.failed` (7), `capture.frame_timeout` (7, waiting for the frame ran out),
   `capture.window_gone` (7, the target is already gone — enumerate again), `capture.access_denied` (6, somebody
   answered "No"), `capture.consent_unavailable` (6, this session has no interactive desktop, so nobody could answer),
   `capture.consent_stale` (7, the target moved after consent — select it again and expect a fresh ask),
   `io.write_failed` (8, directory missing or the commit failed), `io.file_exists` (8, with `--no-overwrite`),
   `io.output_collision` (8, two targets expand to the same output name — nothing was captured).
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
   frames while reporting success. Verify pixels to be sure — for instance put a solid-colour window on top of the
   target and capture again, then check whether you got the target's content or the cover; at minimum compare
   `width`/`height` against the target window rectangle.
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
| `.\tests\cli.ps1` | 135 output-contract assertions (the `--yes` and `--no-overwrite` boolean forms included) + stream separation + multi-language checks (all `--dry-run`, no capture) |
| `.\scripts\check-lang.ps1` | Verifies the four string tables align on keys/placeholders and that the exe really carries four resources |
| `.\tests\invoker.ps1` | Offline checks for the shared test process invoker: argv quoting, both streams at once, binary output, hung child, per-run scratch dirs (no capture) |
| `.\tests\build-path.ps1` | Build-path checks: offline layer (the temporary batch body must stay ASCII, VS environment import failures reported before cmake runs) + on-device layer (Release / Debug / RelWithDebInfo and `-Clean` built from a directory holding CJK text, spaces, parentheses and `%`, plus a CJK `%TEMP%`; no capture, `-OfflineOnly` skips the on-device layer) |
| `.\tests\smoke.ps1` | On-device smoke: capture its own test window → validate PNG size and pixel content |
| `.\tests\save.ps1` | On-device file saving and overwrite protection: every `--no-overwrite` boolean form against a real file, batch output-name planning + collision detection (`%p` / `%n` / `%d` / `%t` / `%%` / unknown `%x` / case / cleaning / truncation), atomic commit (locked target, target is a directory, missing directory, killed mid-run), concurrent `--no-overwrite` race |
| `.\tests\channels.ps1` | On-device channel comparison: six channels + occlusion control, against its own windows. The window-content channels run with `--yes` and fail if a dialog appears; `bitblt` / `duplication` sample the desktop, so their image judgements need `-SimulateConsent` and are recorded as SKIP ("not verified") without it |
| `.\tests\consent.ps1` | Consent tiers: an offline layer runs the whole `ConsentGate` state machine against an injected fake prompt (`build\ecapture-consent-tests.exe`, from `tests\consent_state.cpp`), and the on-device layer answers every dialog "No" to check which paths must ask, what a refusal reports (`code` / `stage` / `target` / `value`), that nothing lands on disk, and that `images[].path` / `scope` / `rect` are right. Never answers "Yes" on a human's behalf |
| `.\tests\isolation.ps1` | On-device resource isolation: a same-named process it did not start stays alive and is never the target, two concurrent runs don't cross, an aborted run cleans up only itself |
| `.\tests\screen.ps1` | On-device whole-screen test: three screen channels (all desktop routes, so every one of them must ask) + red-block placement + negative control. Only `-SimulateConsent` answers the consent dialog, and only for a desktop dedicated to testing; without it the judgements that need an answer are recorded as SKIP |
| `.\tests\streams.ps1` | On-device stream and structured-result reliability: one image on stdout for a single target, a batch that resolves to several targets is refused, the judgement uses the number of targets actually hit, `--monitor all` to stdout is refused with no dialog shown, the diagnostic locator fields, images already captured when a batch fails halfway are kept, and a result that cannot reach the agreed stream gives exit code 8 (only its own windows are captured, the desktop-route case again needs `-SimulateConsent`) |
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
