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
3. Drop `--dry-run` and point the output at the real path. **For a window capture also pass `--yes`** so the
   tool does not stop on a confirmation dialog you cannot answer from a script (`--yes` only ever skips the
   dialog for paths that read the selected window itself - see "When you must ask a human first").
   **The output directory must already exist**, otherwise `io.write_failed` + exit 8.
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
  next step differs: wait and retry versus enumerate the windows again. A frame whose own memory layout does
  not add up (zero size, a side over 16384 px, a row pitch that cannot hold one row, a buffer shorter than
  pitch x height, more than 1 GiB in total) is `capture.frame_invalid` (7), decided before anything is
  allocated; crop, row repack, the single-colour check, encoding and the GPU->CPU copy all run that check.
- Errors and notes carry optional location keys, present only when that step really got the value:
  `target` (which target - `0x…` handle for a window, device name for a screen), `backend` (which channel;
  on a fully failed `auto` chain it lists the channels actually tried), `stage` (`match` / `consent` /
  `capture` / `encode` / `write` / `stdout`), `hresult` (e.g. `0x80070005`), `win32` (raw `GetLastError`). These do not
  follow `--lang`. That is how a refusal by the user (`capture.access_denied` + `stage=consent`) stays
  distinguishable from a technical access denial (`capture.failed` + `hresult=0x80070005`).
- `images[].source` names the channel that really produced the frame, so under `--capture auto` it is the
  winning channel rather than `auto` (and a fallback also raises `note.capture_channel`). A backend that
  throws instead of returning an error only voids its own target: earlier images survive, the remaining
  targets are still attempted, and the failure is reported as a structured error. Out-of-memory and a lost
  GPU device end the batch on purpose instead of cycling through backends.
- **`images[].path` / `images[].scope` / `images[].rect` say where those pixels came from**, and `--quiet`
  never removes them (the whole `images` array never is): `path` is the internal route actually taken
  (`wgc`, `printwindow`, `dwm.thumbnail`, `dwm.screen`, `bitblt.screen`, `duplication.frame`, `screen.wgc`,
  `screen.bitblt`, `screen.duplication`), `scope` is `window` or `desktop` derived from it (the two can never
  disagree), and `rect` is the screen area that route was authorized to sample. **A `--capture dwm --yes` run
  that fell back to the screen route reports `path=dwm.screen, scope=desktop`** - read these three before
  deciding what you may forward, show, upload or delete.
- Exit codes: `0` success / `1` bad arguments / `2` no condition given / `3` `--help` / `4` no match /
  `5` several matches / `6` protected target, or the confirmation was refused (`capture.access_denied`),
  could not be shown (`capture.consent_unavailable`) or nobody answered it within `--consent-timeout-ms`
  (`capture.consent_timeout`) / `7` capture failed, an exhausted `--timeout-ms` budget included
  (`match.timeout` / `capture.timeout`) / `8` write failed, budget exhausted in the write/stdout stage
  (`io.timeout`) included / `9` internal error. `2`/`3`/`4`/`5` are normal control flow, not crashes.
- **`--all` and `--monitor all` allow partial success**: when some targets fail, the images already
  written still appear in `images` (`captured` can be greater than 0) while the exit code stays 7.
  Never throw away what you already got just because the code is non-zero.

Window image (`--class CabinetWClass --index 1`):

```json
{ "captured": 1,
  "images": [ { "file": "D:\\shots\\epad.png", "bytes": 60198, "width": 1247, "height": 607,
                "format": "png", "source": "wgc", "path": "wgc", "scope": "window",
                "rect": { "x": 237, "y": 418, "width": 1261, "height": 614 },
                "hwnd": "0x000A1146", "pid": 31468,
                "title": "… - 文件资源管理器", "class": "CabinetWClass",
                "image": "explorer.exe", "elapsedMs": 156 } ] }
```

Screen image (a whole-screen capture swaps the window fields for `monitor` / `device` / `primary`, and
`hwnd` / `pid` / `title` / `class` / `image` do not appear at all - tell the two shapes apart by whether
`monitor` is present. `path` / `scope` / `rect` are there for both shapes; on a screen image `rect` is that
monitor's rectangle and the scope is always `desktop`):

```json
{ "captured": 1,
  "images": [ { "file": "D:\\shots\\screen.png", "bytes": 269354, "width": 1920, "height": 1080,
                "format": "png", "source": "wgc", "path": "screen.wgc", "scope": "desktop",
                "rect": { "x": 0, "y": 0, "width": 1920, "height": 1080 },
                "monitor": 1, "device": "\\\\.\\DISPLAY1",
                "primary": true, "elapsedMs": 156 } ] }
```

## When you must ask a human first

Screenshot authorization has two levels, decided by **where the pixels actually come from**, not by how the
request was phrased.

- **Window-content paths** - `wgc`, `printwindow`, and DWM's thumbnail route (`dwm.thumbnail`): the frame is
  bound to the selected window itself, nothing is sampled off the desktop. Without `--yes` these still open a
  modal Yes/No dialog (one confirmation covers the batch of targets it lists). **Pass `--yes` when you only
  need a window's own picture**; then no dialog appears at all and the run stays unattended.
- **Desktop paths** - `bitblt`, `duplication`, every whole-screen target (including whole-screen `wgc`), and
  DWM's internal fallback `dwm.screen`: they read what is on the screen at that rectangle right now, so other
  windows, open documents and notifications can end up in the image. **These always ask, and `--yes`,
  `--quiet`, environment variables, stdin or who calls it cannot skip that.** Before invoking one, say out loud
  what will be captured and wait for the human to click "Yes" themselves.
- `--capture auto` with `--yes` may walk the window-content channels without asking, but it asks before
  entering any desktop channel. Approving window content is never approval of the desktop; a scope upgrade
  asks again.
- Refusal (clicking "No", closing the dialog) => `capture.access_denied` + exit 6. A dialog that cannot be
  shown at all (service session, no interactive desktop) => `capture.consent_unavailable` + exit 6, which is
  *not* a human saying no: change the session, do not re-ask. Both carry `stage=consent`, `target`,
  `backend` (channel) and `value` (the actual path). **After a refusal the rest of that request stops** - no
  other backend, no retry; images already finished are kept. Never treat a refusal as a technical failure and
  retry it, and never answer the dialog for the user (no SendMessage, no UI automation, no scripted click).
- Nobody answered the dialog within `--consent-timeout-ms` => `capture.consent_timeout` + exit 6, `stage=consent`:
  an unanswered dialog is a **refusal**, never treated as consent, and it stops the rest of the request like any
  other refusal. This wait is timed separately and does not consume the `--timeout-ms` budget; the ~1 s
  dialog-close animation after "Yes" belongs to the human stage and is never skipped to meet a deadline.
  (Omitted or `0` = the dialog waits forever.)
- A consent refusal is reported as itself even when no output path was given - it no longer collapses into
  `cli.missing_output`, so adding `--out` is not the fix for "a human declined".
- If the target moves or resizes after you were granted desktop consent, that capture is skipped with
  `capture.consent_stale` (exit 7): re-select the target and let the human confirm again.
- After "Yes" the tool waits about a second before grabbing a frame so the dialog's closing animation cannot
  be captured, and the dialog itself is gone before the first frame.
- Nothing that never captures asks: `--dry-run`, no match (4), several matches (5), bad arguments (1) and
  output-plan failures such as `io.output_collision` (8) all answer before any dialog. `--yes` is not a
  target selection condition - with no conditions you still get text help plus exit 2, never a desktop shot.
- `--monitor <n>` **together with window conditions** merely filters windows by screen: the result is window
  pixels, so it follows the window rules above (`--yes` skips its dialog).
- A plain MessageBox is mis-click protection for cooperative automation. It does not verify that a human
  clicked, and it is not a defence against a same-privilege process.

## Deadlines: never block forever (`--timeout-ms`)

The full contract (option ranges, worker isolation, honest limits) is in `references/cli-contract.md`; what a
caller must do:

- **Script / AI callers that must not hang pass `--timeout-ms`** (e.g. `5000`) plus `--yes` for window-only
  captures. It is one total millisecond budget for the whole run's automatic stage - matching (including
  `--title-regex`), `auto` backend retries, frame waits, encoding and writing share it; no step and no target
  gets a fresh copy. Omitted or `0` = no overall budget, but each isolated (helper-process) call is still
  bounded by a built-in 5000 ms cap, so `--capture printwindow` / `dwm` can no longer stall forever.
- **When the budget is exhausted the affected image is NOT written**: you get `match.timeout` (`stage=match`),
  `capture.timeout` (`stage=capture`, encoding included) or `io.timeout` (`stage=write`/`stdout`, exit 8);
  the remaining targets are not started and already-written images are kept.
- **Waiting for a human never eats the budget** - consent is timed by `--consent-timeout-ms` (omitted/`0` =
  forever) and expiry is a refusal, `capture.consent_timeout` + exit 6. `--yes` still never skips the desktop
  paths (bitblt / duplication / any whole screen / dwm's screen fallback): they always ask and can now also
  expire into that code.
- **Reacting**: `capture.timeout` with `backend=printwindow` or `dwm` means the target's UI thread is likely
  stuck - retrying the same backend may just time out again; prefer `--capture wgc` or raise the budget.
  `match.timeout` means the budget was spent before/while evaluating conditions (regex work, or fetching a
  hung window's title) - raise `--timeout-ms` or simplify the regex. A catastrophic-backtracking pattern is
  reported honestly as `cli.invalid_regex` + exit 1 (`stage=match`, backtracking complexity): raising the
  budget does not help, rewrite it or use `--title-contains`.
- Internally, `printwindow` / the DWM read-back and the regex/budgeted matching run in a hidden same-exe
  helper the tool kills on its own deadline; your target window is never killed. There is no public
  `--worker` entry point, it cannot skip consent, the helper never reads desktop pixels, and helper exit
  codes are not part of the contract. Worker machinery failures surface as `capture.worker_failed` (exit 7, `hint`
  carries the helper's last exit code).
- **Limit, not a guarantee**: the budget bites at interruptible points and by killing the helper. Calls with
  no cancellation point (the atomic file write, a blocked stdout pipe, a WinRT encoder ignoring the cancel
  request) are checked before they start and timed after they finish - not preempted mid-call.

## Choosing a capture channel (`--capture`)

| Goal | Use |
| --- | --- |
| The window itself, even when covered | `wgc` (default) or `dwm` - both read the DWM-cached surface; neither sees through DRM protection |
| What the screen looks like right now, occluder included | `bitblt` or `duplication` - they copy visible pixels only, and both need a human confirmation every single time |
| Try the next channel if one comes back empty | `auto`: wgc→dwm→printwindow→bitblt; for a whole screen wgc→duplication→bitblt. A successful fallback reports `note.capture_channel`. With `--yes` the first three ask nothing; entering `bitblt` asks |
| A whole screen | Only `wgc` / `duplication` / `bitblt`; `dwm` and `printwindow` are rejected while parsing with `capture.unsupported` + exit 1. Whole-screen `wgc` is a desktop path too: it always asks |

`images[].path` / `images[].scope` tell you which of those two families actually produced the frame
(`dwm.thumbnail` vs `dwm.screen` are the same channel on different sides of that line). `dwm` enters its
`dwm.screen` route only when the thumbnail step itself failed (PrintWindow returned FALSE, the bitmap could not
be created); a frame that comes back a single colour is kept as a window-content image, so a solid-colour target
never escalates into a desktop capture that would have to be authorized separately.

`wgc` reports the window's **live** size: it reads each frame's own content rectangle rather than the size the
capture item had when the frame pool was built. So `images[].width`/`height` match the window at the moment the
frame was grabbed even if it was resized in between — a smaller window yields just the valid rectangle (never the
undefined edge left in the larger surface texture), and a window that outgrew the pool is re-grabbed after the
pool is recreated within the `--timeout-ms` budget. A frame is never delivered clipped yet reported as whole; if
that ever happens it surfaces as `capture.frame_invalid` / `capture.frame_timeout`, not a wrong-sized success.

- `printwindow` frequently returns an all-black image for hardware-accelerated content (players, games,
  GPU-composited windows); DRM-protected windows are black on most channels.
- **Exit code 0 does not mean the pixels are right**: a black or single-colour frame can also return 0.
  The tool says so when it applies: it compares every pixel of the delivered image against the top-left one
  (all four BGRA bytes count, row padding does not) and emits `note.frame_uniform` with the colour as
  `0xAARRGGBB`, while still delivering that image - a single colour is a quality hint, not a failure, since a
  solid window or a plain wallpaper looks exactly like that. `--quiet` drops notes, so ask for them when this
  matters. Judge the pixels yourself too - cover the target with a plain-coloured window and capture again to
  see whether you got the target's content or the occluder, and at minimum check that `width` / `height` match
  the target rectangle.

## What to do about the common codes

| code | exit | handling |
| --- | --- | --- |
| `match.no_window` | 4 | Conditions too narrow, or the target is minimised (minimised windows cannot be captured); relax with `--title-contains` |
| `match.ambiguous_window` | 5 | Disambiguate as described above |
| `match.index_out_of_range` / `match.monitor_out_of_range` | 1 | `--index` / `--monitor` out of range; `hint` lists everything on this machine |
| `match.timeout` | 7 | The `--timeout-ms` budget was spent before/while evaluating conditions (`stage=match`; regex work or a hung window's title fetch) - raise `--timeout-ms` or simplify the regex |
| `cli.missing_output` | 1 | No output path on the stdout-shortcut path and something failed; pass `--out <path inside an existing directory>` |
| `cli.invalid_format` | 1 | `--format` accepts only png / jpg / jpeg / bmp / tiff / gif (no webp, no ico, no `auto`) |
| `cli.unknown_capture_method` / `cli.unknown_language` | 1 | Bad value, caught while parsing - it never degrades to the default |
| `cli.invalid_regex` | 1 | `--title-regex` too complex for the engine (`stage=match`, message says backtracking complexity) - raising `--timeout-ms` does not help; rewrite the pattern or use `--title-contains` |
| `cli.monitor_conflict` | 1 | `--monitor all` plus window match conditions; use a single monitor number to filter instead |
| `capture.failed` | 7 | Target protected, gone, or unsupported by the OS; retry once with `--capture auto`, and if it fails again nothing is reachable |
| `capture.worker_failed` | 7 | This tool's own hidden helper could not run (spawn blocked, pipe broke, message did not match the protocol, task invalid) - `cap.worker.*` wording, the helper's last exit code in `hint`; its exit codes are not part of the contract. Check the execution environment (policy, antivirus, permissions), not the target window |
| `capture.timeout` | 7 | `--timeout-ms` budget exhausted during capture/encode (`stage=capture`); with `backend=printwindow` / `dwm` the target's UI thread is likely stuck - same-backend retry may time out again, prefer `--capture wgc` or raise the budget |
| `capture.access_denied` | 6 | Someone answered "No" or closed the dialog (`stage=consent`, `value` names the path) - stop, do not retry, do not switch channel, never answer it for the user; or the target window itself is protected |
| `capture.consent_unavailable` | 6 | The dialog could not be shown at all (service session, scheduled task, lock screen). Nobody refused - run it in an interactive session instead of asking a second time |
| `capture.consent_timeout` | 6 | Nobody answered the dialog within `--consent-timeout-ms` - treated as a refusal, never as consent; stop like after any "No" (and note: this wait does not consume `--timeout-ms`) |
| `capture.consent_stale` | 7 | The target moved or resized after desktop consent was granted, so nothing was sampled. Re-select the target and let the human confirm again |
| `capture.frame_invalid` | 7 | The frame that came back does not describe its own memory correctly (zero size, a side over 16384 px, a row pitch that cannot hold one row, a buffer shorter than pitch x height, more than 1 GiB). Detected before allocating anything; a target-side problem on that channel - re-check the size, or `--capture wgc` |
| `io.write_failed` | 8 | Output directory does not exist, the file name is invalid, or the finished temporary file could not be renamed onto the target (it is held open elsewhere, the target name is a directory, …) |
| `io.file_exists` | 8 | `--no-overwrite` (or `=true`) was given and the target already exists; decided by the final rename, not by a pre-check |
| `io.output_collision` | 8 | Two targets expand to the same output name; the whole batch is refused before any frame is taken, so nothing is written - put `%i` / `%h` into `--out` |
| `io.timeout` | 8 | The budget ran out at the write/stdout stage (`stage=write` / `stdout`); the finished image is not written - keep enough budget for the encode+write tail |

## Resources

- `references/cli-contract.md` - every option and value (including `--timeout-ms` /
  `--consent-timeout-ms` and the hidden helper process behind them), the full JSON field tables (window image /
  screen image), the complete diagnostic-code list, the output-name placeholders
  (`%i` `%h` `%p` `%n` `%d` `%t`; `%n` is the window title for a window target and the device name such
  as `DISPLAY1` for a screen target), plus the shell-specific traps measured under PowerShell and Git
  Bash.
- `ECAPTURE.EXE` - the tool itself, in this same directory.
