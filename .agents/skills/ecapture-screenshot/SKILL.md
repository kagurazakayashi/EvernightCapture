---
name: ecapture-screenshot
description: Capture window or full-screen images on Windows with ECAPTURE.EXE (EvernightCapture) by selecting targets through conditions. Use when taking screenshots, grabbing a specific app / window / dialog, capturing several windows at once, capturing one monitor whole, or parsing ECAPTURE's JSON output, exit codes and diagnostic codes.
argument-hint: <window conditions> <output path>
---

# ECAPTURE screenshot guide

## Overview

ECAPTURE.EXE (build 0.4.0, stage `capture-channels`) is a Windows command-line screenshot tool: it
selects windows by conditions and writes that window's picture - or a whole monitor's - to an image.
Both success and failure come back as JSON with stable exit codes, so it is meant to be called
programmatically.

## Where the executable is

- Same directory as this SKILL.md: `<skill dir>\ECAPTURE.EXE`. **Call it by absolute path.**
- If it is missing, build the repository (`.\build.ps1`) and copy `build\ecapture.exe` here under the
  same name. Refresh that copy after any rebuild that changes options, codes or the output contract.
- `ECAPTURE.EXE --help` is the authority on the option list; **`--help` exits with code 3, that is not a
  failure**. Add `--lang en` for English message text (`zh-CN` / `zh-TW` / `en` / `ja`).

  ```powershell
  & "$PSScriptRoot\ECAPTURE.EXE" --process notepad.exe --out D:\shots\epad.png
  ```

## Three steps: dry-run, disambiguate, capture

1. `--dry-run` with the window conditions: it stops right after target selection - no frame, no file.
   Candidates are in the `note.dry_run` entry of `notes[]`, under `value`, shaped like
   `hwnd=0x000A1146 pid=31468 1261x614+237+418 class=CabinetWClass title=…`.
   Pass `--out` anyway: without an output path the JSON moves to **stderr**, and every failure on that
   path collapses into `cli.missing_output` + exit 1, which hides the real reason (`match.no_window`,
   `match.ambiguous_window`, …).
   Conditions alone with nothing else still means "no condition": `--dry-run` by itself prints the text
   help and exits 2. `--monitor` counts as a target, so `--monitor --dry-run` lists screens instead.
2. Several matches (exit 5): read the candidate list from `errors[0].hint`
   (`0x… title [image.exe] | 0x… …`), then narrow with `--class` / `--process` / `--title`
   (note `--title*` is **case-sensitive**, `--class` / `--process` / `--exe` are not), or pick one with
   `--index 1` / `--newest` / `--oldest`, or take them all with `--all` (put `%i` in the output name,
   otherwise `note.all_without_placeholder` fires and `_N` is appended).
3. Drop `--dry-run` and point the output at the real path. **The output directory must already exist**,
   otherwise `io.write_failed` + exit 8.
4. **Every output name of the batch is planned before the first frame is taken.** Two targets that expand to
   the same path give `io.output_collision` + exit 8 and nothing at all is captured - the tool never renames
   your template behind your back. A placeholder must actually separate the targets: `%i` or `%h` do, while
   `%d`, `%t`, `%%`, an unknown `%x`, `%p` for two windows of one process, and `%n` for equal (or
   case-equivalent, or truncation-equivalent) titles do not. `%d` / `%t` come from one clock per batch.
   `--out -` is not a path: no expansion and no collision check - and it carries **only one image per run**.
   If the conditions select more than one target while the output is stdout (explicit `--out -` or no output
   path at all), the whole batch is rejected with `cli.stdout_multiple_targets` + exit 1 *before* the consent
   dialog and before the first frame: nothing is captured, no file is written. The check uses the number of
   targets actually matched, so `--all` that hits a single window may still write stdout. Several PNGs
   concatenated into one stream are not a decodable image, and `-` is never used as a file-name prefix.
5. **Writes are atomic and `--no-overwrite` is checked by the write itself.** Bytes land in a unique
   temporary file in the target directory and are renamed onto the target only after everything is written
   and flushed, so a failed write leaves the previous file exactly as it was, and only that run's own
   temporary file is ever removed. `--no-overwrite` (bare, or `=true` / `1` / `yes` / `y` / `on`) makes the
   final rename refuse an existing target (`io.file_exists` + exit 8); `--no-overwrite=false` / `0` / `no` /
   `n` / `off` cancels the prohibition. Repeated occurrences: the last one wins.

## Reading the output

- Check `errors` before `images`; `captured` always equals the number of `images` entries.
- By default all JSON goes to **stdout** and stderr stays empty. As soon as the image occupies stdout
  (explicit `--out -`, or simply no output path at all), the **whole JSON moves to stderr** - the two
  streams never mix. To parse it from a shell, write the image to a file instead of using `--out -`.
  That routing is decided before any image byte is written, and the emergency document for "even rendering
  the result threw" follows it too (always stderr; the tool does not re-parse argv to guess). If the result
  cannot be delivered on the agreed stream, the exit code is **8** even when the other stream took the
  text - reading the agreed stream is what counts.
- Keys: `captured` / `images[]` / `errors[]` / `notes[]` / `input` (only with `--verbose`).
  `--quiet` drops `notes` but **never suppresses `errors`**. **Empty fields are omitted entirely**, so
  `option`, `value` and `hint` may simply be absent - never assume a key is there.
- Branch on `errors[].code`, never on `message` text (that follows `--lang`); code values are only ever
  added, never renamed. A capture failure now tells a frame timeout (`capture.frame_timeout`) and a window
  that disappeared (`capture.window_gone`) apart from a plain `capture.failed` - all still exit 7, but the
  next step differs: wait and retry versus enumerate the windows again.
- Errors and notes carry optional location keys, present only when that step really got the value:
  `target` (which target - `0x…` handle for a window, device name for a screen), `backend` (which channel;
  on a fully failed `auto` chain it lists the channels actually tried), `stage` (`consent` / `capture` /
  `encode` / `write` / `stdout`), `hresult` (e.g. `0x80070005`), `win32` (raw `GetLastError`). These do not
  follow `--lang`. That is how a refusal by the user (`capture.access_denied` + `stage=consent`) stays
  distinguishable from a technical access denial (`capture.failed` + `hresult=0x80070005`).
- `images[].source` names the channel that really produced the frame, so under `--capture auto` it is the
  winning channel rather than `auto` (and a fallback also raises `note.capture_channel`). A backend that
  throws instead of returning an error only voids its own target: earlier images survive, the remaining
  targets are still attempted, and the failure is reported as a structured error. Out-of-memory and a lost
  GPU device end the batch on purpose instead of cycling through backends.
- Exit codes: `0` success / `1` bad arguments / `2` no condition given / `3` `--help` / `4` no match /
  `5` several matches / `6` protected target or refused by the user / `7` capture failed /
  `8` write failed / `9` internal error. `2`/`3`/`4`/`5` are normal control flow, not crashes.
- **`--all` and `--monitor all` allow partial success**: when some targets fail, the images already
  written still appear in `images` (`captured` can be greater than 0) while the exit code stays 7.
  Never throw away what you already got just because the code is non-zero.

Window image (`--class CabinetWClass --index 1`):

```json
{ "captured": 1,
  "images": [ { "file": "D:\\shots\\epad.png", "bytes": 60198, "width": 1247, "height": 607,
                "format": "png", "source": "wgc", "hwnd": "0x000A1146", "pid": 31468,
                "title": "… - 文件资源管理器", "class": "CabinetWClass",
                "image": "explorer.exe", "elapsedMs": 156 } ] }
```

Screen image (a whole-screen capture swaps the window fields for `monitor` / `device` / `primary`, and
`hwnd` / `pid` / `title` / `class` / `image` do not appear at all - tell the two shapes apart by
whether `monitor` is present):

```json
{ "captured": 1,
  "images": [ { "file": "D:\\shots\\screen.png", "bytes": 269354, "width": 1920, "height": 1080,
                "format": "png", "source": "wgc", "monitor": 1, "device": "\\\\.\\DISPLAY1",
                "primary": true, "elapsedMs": 156 } ] }
```

## When you must ask a human first

- **A whole-screen capture (a `--monitor` target with no window match conditions) always opens a modal
  Yes/No dialog first.** There is no command-line bypass and no environment-variable bypass; the
  process blocks until someone answers. Before invoking it, say out loud that this captures the entire
  screen - every other window, open documents, notifications - and wait for the other side to confirm
  they can give up the screen, then remind them to click "Yes". The dialog focuses "No" by default.
- Answering "No" - and a dialog that cannot be shown at all (service session, no interactive desktop) -
  both count as refusal: `capture.access_denied` + exit 6, nothing captured, no file written.
  **A bare `--monitor` with nothing after it is also a whole-screen target**, so it pops the dialog and
  blocks; do not use it as a cheap exit-code probe.
- After "Yes" the tool waits 1 second before grabbing a frame, so the dialog's closing animation does
  not end up inside the image, and the dialog itself is gone before the first frame.
- Refusal collapses to `cli.missing_output` + 1 if no output path was given. To tell "a human said no"
  apart from "the path was wrong", pass `--out` explicitly.
- Only want one window? Do not escalate to a whole screen. `--monitor <n>` **together with window
  conditions** merely filters windows by screen - no dialog, window-shaped JSON.
- `--dry-run` takes no frame and never shows the dialog, but it does not skip parse-time checks either
  (`--monitor all` with window conditions is still rejected).

## Choosing a capture channel (`--capture`)

| Goal | Use |
| --- | --- |
| The window itself, even when covered | `wgc` (default) or `dwm` - both read the DWM-cached surface; neither sees through DRM protection |
| What the screen looks like right now, occluder included | `bitblt` or `duplication` - they copy visible pixels only |
| Try the next channel if one comes back empty | `auto`: wgc→dwm→printwindow→bitblt; for a whole screen wgc→duplication→bitblt. A successful fallback reports `note.capture_channel` |
| A whole screen | Only `wgc` / `duplication` / `bitblt`; `dwm` and `printwindow` are rejected while parsing with `capture.unsupported` + exit 1 |

- `printwindow` frequently returns an all-black image for hardware-accelerated content (players, games,
  GPU-composited windows); DRM-protected windows are black on most channels.
- **Exit code 0 does not mean the pixels are right**: a black or single-colour frame can also return 0.
  Judge the pixels - cover the target with a plain-coloured window and capture again to see whether you
  got the target's content or the occluder, and at minimum check that `width` / `height` match the
  target rectangle.

## What to do about the common codes

| code | exit | handling |
| --- | --- | --- |
| `match.no_window` | 4 | Conditions too narrow, or the target is minimised (minimised windows cannot be captured); relax with `--title-contains` |
| `match.ambiguous_window` | 5 | Disambiguate as described above |
| `match.index_out_of_range` / `match.monitor_out_of_range` | 1 | `--index` / `--monitor` out of range; `hint` lists everything on this machine |
| `cli.missing_output` | 1 | No output path on the stdout-shortcut path and something failed; pass `--out <path inside an existing directory>` |
| `cli.invalid_format` | 1 | `--format` accepts only png / jpg / jpeg / bmp / tiff / gif (no webp, no ico, no `auto`) |
| `cli.unknown_capture_method` / `cli.unknown_language` | 1 | Bad value, caught while parsing - it never degrades to the default |
| `cli.monitor_conflict` | 1 | `--monitor all` plus window match conditions; use a single monitor number to filter instead |
| `capture.failed` | 7 | Target protected, gone, or unsupported by the OS; retry once with `--capture auto`, and if it fails again nothing is reachable |
| `capture.access_denied` | 6 | Whole-screen dialog answered "No" or not showable, or the target window is protected |
| `io.write_failed` | 8 | Output directory does not exist, the file name is invalid, or the finished temporary file could not be renamed onto the target (it is held open elsewhere, the target name is a directory, …) |
| `io.file_exists` | 8 | `--no-overwrite` (or `=true`) was given and the target already exists; decided by the final rename, not by a pre-check |
| `io.output_collision` | 8 | Two targets expand to the same output name; the whole batch is refused before any frame is taken, so nothing is written - put `%i` / `%h` into `--out` |

## Resources

- `references/cli-contract.md` - every option and value, the full JSON field tables (window image /
  screen image), the complete diagnostic-code list, the output-name placeholders
  (`%i` `%h` `%p` `%n` `%d` `%t`; `%n` is the window title for a window target and the device name such
  as `DISPLAY1` for a screen target), plus the shell-specific traps measured under PowerShell and Git
  Bash.
- `ECAPTURE.EXE` - the tool itself, in this same directory.
