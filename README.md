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
  --monitor, -m [<n|primary|all>] Monitor number, 1-based and decimal (the order of this run's enumeration, not guaranteed to match the id in Display settings; use device in the result to identify a monitor); primary = main one, all = one image per monitor. Without window conditions = capture that whole screen; with them = only windows overlapping it. The value may be omitted (= primary), and then the next argument is not swallowed, so --monitor out.png still works. A whole screen is desktop pixels, so a person must confirm it and --yes cannot skip that; filtering windows by monitor still yields window images

Window match conditions (repeat one option for OR, combine different options with AND)
  --hwnd <handle>                 Window handle. Plain digits are decimal; a 0x prefix or a-f digits are hexadecimal - prefer 0x. No sign and no whitespace; an underscore may only sit between two hexadecimal digits
  --pid <pid>                     Process id, decimal only and greater than 0
  --process, -p <image-name>      Image file name (no path), case-insensitive; without an extension .exe is assumed
  --exe <full-path>               Full image path, case-insensitive
  --title, -t <exact-title>       Window title, exact match
  --title-contains, -T <text>     Window title contains this substring
  --title-regex, -R <regex>       Window title regular-expression match (ECMAScript), validated while parsing
  --class, -c <class-name>        Window class name, case-insensitive, e.g. Notepad / CabinetWClass

When several windows match (mutually exclusive)
  --index, -i <n>                 Take the n-th window, 1-based decimal, ordered by visibility and z-order
  --topmost-match                 Take the topmost matched window in the current z-order
  --bottommost-match              Take the bottommost matched window in the current z-order
  --newest                        Deprecated alias of --topmost-match: it picks by z-order, not by creation time
  --oldest                        Deprecated alias of --bottommost-match: it picks by z-order, not by creation time
  --all, -a                       Save one image per matched window

Capture channel (default wgc; may fail because of the OS version or the window itself)
  --capture, -C <method>          wgc (default, works through occlusion) / dwm (DWM thumbnail, works through occlusion) / printwindow (window paints itself) / bitblt (copies visible screen pixels) / duplication (desktop duplication cropped to the rect; corrected for the monitor's rotation, and it only takes the one output overlapping the target most - partial captures come back with capturedRect/clipped) / auto (falls back wgc-dwm-printwindow-bitblt; a whole screen only uses wgc-duplication-bitblt)

Capture authorization (a real capture asks first; --yes skips window-content paths)
  --yes, -y                       Skip the confirmation for window-content paths (wgc / printwindow / the dwm thumbnail route). Anything reading the screen (bitblt, duplication, a whole screen, dwm screen fallback) always asks; --yes cannot skip it. --yes=false asks on purpose

Deadlines (a total budget for the automatic stage; waiting for consent is timed separately)
  --timeout-ms <ms>               Total budget in milliseconds for the automatic stage: from target selection on, matching, backend retries, frame capture, encoding and writing share this one remaining budget and no step gets a fresh copy. Omitted or 0 = no overall budget, and every isolated call is then still bounded by the built-in 5000 ms limit. Waiting for your consent is not counted here - see --consent-timeout-ms. When the budget runs out the image is not written; you get match.timeout / capture.timeout / io.timeout per stage
  --consent-timeout-ms <ms>       How long the consent dialog may wait for an answer, in milliseconds. Omitted or 0 = wait forever. On expiry the capture is refused - never treated as consent - and reported as capture.consent_timeout. This wait is timed separately and does not consume the --timeout-ms budget; the ~1s dialog close animation after "Yes" is counted here and is never skipped to meet a deadline

Output
  --out, -o <path|->              Output path; the special value - writes the image bytes to stdout. A positional argument works too, and giving no path at all is the same as --out -. Every name for the batch is planned before any frame is taken: two targets resolving to the same name is an error, never a silent overwrite. stdout carries only one image per run, so a batch that resolves to more than one target is a parameter error and nothing is captured
  --format, -f <name>             Force the encoding format; otherwise it comes from the output file extension, and png when that fails too
  --quality <1-100>               JPEG quality, decimal 1-100, default 100
  --no-overwrite                  Fail instead of overwriting an existing target (no value means the prohibition is on). --no-overwrite=false (0 / no / n / off) cancels it; =true / 1 / yes / y / on means the same as giving no value. When repeated, the last one wins

Miscellaneous
  --dry-run, -d                   Parse and list candidate windows only - no capture, no file written
  --json, -j                      Deprecated compatibility switch, no effect: success and errors are already JSON
  --verbose, -v                   Add the input section to the JSON (all input, normalized) and keep notes
  --quiet, -q                     Drop notes; errors are always returned whatever this says. --verbose wins when both are given
  --lang, -l <language>           Message language. auto (default, follows the system display language) / zh-CN / zh-TW / en / ja; unsupported system languages fall back to en
  --help, -h                      Print this text help
  --version                       Print version and stage

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

## Capture channels

| Value | Channel | Covered window | Hardware-accelerated content | API history floor |
| --- | --- | --- | --- | --- |
| `wgc` | Windows.Graphics.Capture | yes (DWM cache) | normal | Win10 1903 (18362) — the `CreateForWindow` / `CreateForMonitor` interop, not the 1803 namespace |
| `dwm` | DwmRegisterThumbnail | yes | mostly normal, protected windows black | Win8.1 (9600) — registering is older, but the read-back is `PrintWindow(PW_RENDERFULLCONTENT)` |
| `printwindow` | PrintWindow + PW_RENDERFULLCONTENT | yes (window self-draw) | often fully black | Win8.1 (9600) for that flag |
| `bitblt` | BitBlt from a screen DC | no, visible pixels only | partly black | no floor of its own |
| `duplication` | DXGI desktop duplication frame, cropped to the rect | no, visible pixels only | normal | Win8 (9200); RDP / virtual GPUs often yield nothing |
| `auto` | falls back wgc → dwm → printwindow → bitblt | best effort | best effort | the chain minus whatever this build gates out |

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
reshuffle it, so do not store a number to identify a screen across runs. Use `images[].device` (the
`\\.\DISPLAY1`-shaped name) when you need to recognize the same monitor again. Out of range gives
`match.monitor_out_of_range` (exit code 1) with every local monitor listed in `hint`. Before a screen target is
captured, the tool re-checks that monitor by name: if it left the desktop the capture stops with
`capture.monitor_changed`, and if its rectangle or position changed, the new rectangle is what a person is asked to
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

1. **Probe with `--dry-run` first**, then disambiguate, then capture for real. `--dry-run` takes no frame, writes no
   file and shows no consent dialog; candidates are in `notes[0].value`, shaped like
   `hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=…`. `--dry-run` does not need an output path
   either (with none it just means "nothing to deliver", so the JSON goes to stderr with
   `note.output_defaulted_stdout`); **`--dry-run` alone with no window condition = text help + exit code 2**.
2. **Branch on `errors[].code`, never on `message` text** (that follows `--lang`) and never on whether you passed
   `--out` — the two ways of asking for stdout report the same codes. The codes you actually hit:
   `match.no_window` (4, conditions too narrow or the window is minimized), `match.ambiguous_window` (5, choose
   from the candidates in `hint`), `match.index_out_of_range` / `match.monitor_out_of_range` (1, `hint` lists all
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
| `.\tests\cli.ps1` | 495 output-contract assertions (the `--yes` and `--no-overwrite` boolean forms included) + stream separation + "no `--out`" against `--out -` equivalence + multi-language checks (all `--dry-run`, no capture) |
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
