---
name: ecapture-screenshot
description: Capture window or full-screen images on Windows with ECAPTURE.EXE (EvernightCapture) by selecting targets through conditions. Use when taking a screenshot, grabbing one specific app / window / dialog, discovering which windows match (--list / --inspect as structured JSON), naming a monitor by a stable identifier (--screens), capturing several windows at once, capturing one whole monitor, or interpreting ECAPTURE's JSON result, exit codes and stable diagnostic codes. Prefer read-only queries over probing by capturing. Any route that samples desktop pixels (whole screen, bitblt, duplication, DWM's screen fallback) requires the user's own confirmation - present it to the user instead of answering for them.
argument-hint: <window conditions> <output path>
---

# ECAPTURE screenshot guide

## What this is

`ECAPTURE.EXE` (EvernightCapture) is a Windows command-line screenshot tool: it selects windows by conditions and
writes that window's picture - or a whole monitor's - to an image. Success and failure both come back as JSON with
stable exit codes and stable diagnostic `code` values, so it is meant to be driven programmatically. Current build:
`0.4.0`, stage `capture-channels` (`--version` prints both).

Three kinds of call, and the difference matters:

- **Read-only queries** - `--capabilities`, `--diagnostics`, `--screens`, `--list`, `--inspect`, `--dry-run`. They take
  no pixel, show no dialog, write no file, change no display setting, and never probe by capturing something. Use them
  to decide. They need no output path.
- **A real capture of a window's own content** (`wgc` / `printwindow` / `dwm.thumbnail`) - runs unattended once you
  pass `--yes`.
- **A real capture that samples desktop pixels** (any whole screen, `bitblt`, `duplication`, DWM's `dwm.screen`
  fallback) - **a person has to answer the dialog**. Say what will be captured, start it, and wait for them.

## Where the executable is

The bundled copy is `ECAPTURE.EXE` in this directory. **Call it by absolute path**; `$PSScriptRoot` only has a value
*inside* a `.ps1` file, so interactively it expands to nothing.

```powershell
# PowerShell (interactive: use the literal path, not $PSScriptRoot)
$exe = "P:\yashi\EvernightCapture\.agents\skills\ecapture-screenshot\ECAPTURE.EXE"
& $exe --process notepad.exe --out D:\shots\epad.png
```

```bat
:: cmd.exe - the same absolute path
P:\yashi\EvernightCapture\.agents\skills\ecapture-screenshot\ECAPTURE.EXE --capabilities
```

Substitute your own checkout if the repository lives elsewhere, and do not assume the tool is on `PATH`.

- `ECAPTURE.EXE --help` is the authority on the option list. **`--help` exits with code 3 and `--version` with 0;
  neither is a failure.** Add `--lang en` for English message text (`zh-CN` / `zh-TW` / `en` / `ja`).
- If the copy here is missing, or refuses an option this guide documents (for example `--scale`), it predates the
  guide. Building (`.\build.ps1` -> `build\ecapture.exe`) and replacing the bundled binary changes a shipped artifact,
  so **report it and let the user decide** - do not build, install or overwrite anything on your own.
- Git Bash mangles `/help` as a path: `export MSYS2_ARG_CONV_EXCL='*'`, and pass Windows-style paths.

## Call flow

1. **Ask what this machine can do, read-only.** `--capabilities` gives version, OS and session, each backend as
   `available` / `unavailable` / `unverified`, formats, exactly which internal paths `--yes` covers, the chain `auto`
   would walk here, and the value ceilings. Three facts per row, never to be blended: `compiled` (this binary has the
   route), `status` (this machine right now - `unverified` means a question went unanswered, it is **not** "no"), and
   `verifiedOnThisMachine` (whether this project has exercised that route on a machine matching the tested build 19045
   x64 - anything else honestly says `no`). `available` is **not** "this window will capture": drivers, protected
   content and HDR are outside that layer, and the `caveats` array lists what the report does not assert.
   For a bug report use `--diagnostics` (same judgements plus a checkable build id). Both take only `--lang`, `-v`,
   `-q`; mixing them with capture intent is `cli.query_conflict` + exit 1.
2. **Discover the target as data.** `--list --process app.exe` (several matches are the normal answer; it pages with
   `--offset` / `--limit`, total in `pagination.matched`), `--inspect --hwnd 0x…` (one window - it uses the same
   selection policy as a capture and answers `match.ambiguous_window` + 5 rather than pick one for you), `--screens`
   (monitor identities; write back `screens[].selectors.device` or `.id` into `--monitor`). Read fields, not prose.
3. **Select it with conditions that actually pin it down**, taken from the snapshot you just ran: `--class` /
   `--process` / `--exe` are case-insensitive and steadier than `--title*` (case-sensitive); name a window by handle
   from a fresh query when the choice matters. Re-query instead of reusing a handle from an earlier session - `0x…`
   values get recycled.
4. **Capture, pointing the output at a real file.** `--out D:\shots\name.png` (the directory must already exist),
   `--timeout-ms 5000` when your caller must not hang, and `--yes` only when the route stays a window-content one.
5. **Branch on `errors[].code`.** Check `errors` before `images`; `captured` always equals the number of `images`
   entries. `images[].source` / `path` / `scope` say which channel and which internal route really produced the frame
   and whether its pixels are the window's own or the screen's. Never branch on `message` (follows `--lang`) or on the
   exit code alone.

## Snapshots, identity and matching (what a query does *not* buy you)

- **Matching**: different options are ANDed, repeated values of one option are ORed, and conditions are never combined
  across two different windows.
- **A question that could not be answered never becomes a match.** A window whose title or process information the
  system refuses (`denied` / `failed`) simply does not satisfy the conditions that need it. A `--title-regex` that
  throws part-way discards every hit it had collected and reports the failure (`match.timeout`, or `capture.worker_failed`
  when its helper process did not come back - exit 7 either way) instead of returning a half-evaluated list.
- A `readability` entry that could not be read is **absent or `denied` / `failed` with the raw code**, never written as
  empty, `0` or `false`. `denied` means the owner runs at another privilege level; it is not a prediction about the
  capture and not a request to run as administrator - nothing here escalates.
- **Every query result is a snapshot of this moment** (`note.window_query_stale` / `note.screen_query_stale`). In
  `identity`, `processStartTicks` is what tells a recycled PID from the same process, and
  `isAuthorizationToken: false` / `raceWindowReducedNotEliminated: true` are stated, not implied.
- **A query never authorizes a capture.** Consent is decided per request by where the pixels come from, and the capture
  re-verifies its target before reading a pixel (`capture.target_gone` / `capture.target_changed` /
  `capture.target_unverifiable`). Monitor ordinals are only this run's enumeration order - name a monitor by a
  `--screens` selector instead of caching a number.
- Minimized windows cannot be captured; `--list=all` only merges them into the listing. Invisible and zero-sized
  windows are excluded by default (`policy.*` states this).
- Ambiguity: `--index` / `--topmost-match` / `--bottommost-match` select a **z-order position in the current stacking
  order** (`--newest` / `--oldest` are aliases of topmost / bottommost - Windows exposes no window creation timestamp -
  and add `note.deprecated_option`); `--all` takes every match, so put `%i` or `%h` in the output name.
- Privacy: the full image path is not emitted unless you ask (`--inspect=path`).

## Authorization rules you cannot waive

| Pixels come from | Route in `images[].path` | Without `--yes` | With `--yes` |
| --- | --- | --- | --- |
| the selected window itself | `wgc`, `printwindow`, `dwm.thumbnail` | one modal dialog | not asked |
| the screen at that rectangle | `bitblt.screen`, `duplication.frame`, `dwm.screen` | **asked** | **still asked** |
| a whole monitor | `screen.wgc`, `screen.bitblt`, `screen.duplication` | **asked** | **still asked** |

- The tier is decided by **where the pixels actually come from**, never by the channel name you typed. An unregistered
  or unprovable route counts as a desktop route, so a new channel that forgot to register itself ends up stricter.
- **Nothing skips a desktop route**: not `--yes`, not `--quiet`, not an environment variable, not stdin, not who is
  calling. There is no hidden bypass switch and no public `--worker` entry point. The fake consent driver used by the
  tests is linked only into test binaries and is not reachable from `ECAPTURE.EXE` - "how a test approves" is not a
  way to call the tool.
- Cropping, scaling, HDR handling, cursor settings, channel fallback and worker execution all run **after** or
  independently of the decision, so none of them lowers the bar: `bitblt` with `--roi 0,0,8,8` and `--yes` still opens
  the dialog. `--capture auto` with `--yes` may walk the window-content routes silently but asks before entering any
  desktop route - approving window content is never approval of the desktop.
- **Do not answer the dialog for the user** (no `SendMessage`, no UI automation, no scripted click) and do not fake an
  answer. Before starting a desktop route, tell the user what will be in the picture and wait for them to click "Yes".
- **A refusal is a decision to preserve, not a technical failure.** "No", closing the box (`X` and `Esc` both answer
  No), nobody answering within `--consent-timeout-ms` (`capture.consent_timeout`) or a dialog that cannot be shown
  (`capture.consent_unavailable`) stop the rest of that request - including the remaining links of an `auto` chain, not
  just the remaining `--all` targets. No retry, no second ask, no other backend. Images already delivered stay
  delivered, and you report partial completion truthfully.
- Consent is a **snapshot** taken before the box: the targets listed with their screen areas, the batch's resolved
  absolute output names, and a fingerprint of the screen topology. It is re-checked before the permit is issued and
  again before each step that would sample pixels. If the topology or a target's area changed while you waited,
  `capture.consent_stale` (exit 7) means nothing was sampled and no permit was issued - the tool deliberately does not
  re-pop the dialog inside the same call, so re-discover the target and let the user confirm in a **new** request.
  Unlike a refusal, this code does not stop the whole batch.
- `capture.consent_unavailable` is not a human saying no: it needs a session with an interactive desktop (a service,
  scheduled task or lock screen cannot answer). That is the user's call, not a retry.
- The dialog is plain `MessageBox` mis-click protection for cooperative automation. It does not verify that a human
  clicked and it is not an OS security boundary.

## Output: prefer a file

- **Default to an explicit `--out <absolute path>`.** Then the whole JSON is on stdout and stderr stays empty - the
  easy case for a caller.
- Omitting `--out` **is** `--out -`: image bytes on stdout, the whole JSON on stderr, the two streams never mixed, and
  the real codes and exit codes come back unchanged. stdout carries **one** image per run, so a request that resolves
  to more than one target is rejected as `cli.stdout_multiple_targets` + 1 before any dialog or frame.
- Shells differ in whether they preserve a native byte stream: `cmd` and PowerShell 7.4+ do; **Windows PowerShell 5.1
  corrupts it** (bytes decoded as text, rewritten as UTF-16LE, native stderr rendered as error records) and 7.0-7.3
  decode stdout too. From those, write a file, or run under `cmd /c`, or use `Start-Process`
  `-RedirectStandardOutput` / `-RedirectStandardError`. `2>&1` is never the answer for an image.
- **Exit 0 does not prove the pixels are right.** DRM and some player drivers hand back black while reporting success;
  `note.frame_uniform` (colour as `0xAARRGGBB`) is a quality hint, not a failure, and `--quiet` hides notes. At minimum
  compare `width` / `height` with the target rectangle, and for a screen-reading route check `clipped` /
  `capturedRect`: a window straddling two monitors is captured only on the output it overlaps most, so the image may
  not be the whole window.
- **Partial success is real**: with `--all` or `--monitor all`, `captured` can be greater than 0 while the exit code is
  7. Never throw away what already landed.
- Every output name of the batch is planned before the first frame, and writes are atomic. Two targets expanding to the
  same name is `io.output_collision` + 8 and nothing at all is captured (use `%i` / `%h`); `--no-overwrite` is enforced
  by the final rename (`io.file_exists` + 8); a missing directory or a held-open target is `io.write_failed` + 8. A
  failed write leaves the previous file exactly as it was.

## Deadlines

- `--timeout-ms` is **one** budget for the whole automatic stage: matching (including `--title-regex`), `auto` retries,
  frame waits, encode and write share it; no step and no further target gets a fresh copy. Omitted or `0` = no overall
  budget, while each isolated helper call is still capped at 5000 ms. When it is spent, a step that has not started is
  refused and its image is not written: `match.timeout` (`stage=match`) / `capture.timeout` (`stage=capture` for a
  frame that never arrived, `stage=encode` when the budget died in the encoder) / `io.timeout` (`stage=write` /
  `stdout`, exit 8). Already-written images stay; a file already mid-commit is not rolled back.
- **Waiting for a human is a separate clock.** `--consent-timeout-ms` bounds the dialog only, the automatic budget is
  paused while it runs and is never refunded by that pause, and expiry is a refusal (exit 6) - never consent. The
  deadline is polled (about a 50 ms slice, plus a close grace of about 3 s), not preempted; the ~1 s settle after "Yes"
  belongs to the human stage and is not skipped to meet a deadline.
- `printwindow`, DWM's read-back and regex/budgeted matching run in a hidden same-exe helper that the tool stops on its
  own deadline; your target window is never killed. The budget bites at interruptible points and by stopping that
  helper - the atomic write, a blocked stdout pipe and a WinRT encoder that ignores the cancel request are gated before
  they start and timed after they finish.
- Leave enough budget for the encode+write tail, or a captured picture is lost to `io.timeout`.

## Handling a result: stop, retry, or hand it to the user

**Stop and hand it back.** These are a human decision, a vanished fact or a stated policy - not "this backend failed,
try another" - and the tool itself never tries another route after them: `capture.access_denied`,
`capture.consent_timeout`, `capture.consent_unavailable`, `capture.consent_stale`, `capture.target_gone`,
`capture.target_changed`, `capture.target_unverifiable`, `capture.hdr_refused`, `capture.hdr_unverifiable`.

**Retrying is legitimate only after the named condition is fixed**: `capture.failed`, `capture.timeout`,
`capture.frame_timeout`, `capture.window_gone`, `capture.frame_invalid`, `capture.roi_invalid`,
`capture.roi_unmeasurable`, `capture.cursor_unverifiable`, `match.timeout` - raise the budget, simplify the regex,
re-measure the rectangle, switch to `--capture wgc` when the target's UI thread looks stuck, or re-enumerate. Do not
answer "capture failed" with "--capture auto and hope".

**"No answer" is not a pass and not a "no".** `unverified` in `status` / `cursorEffective` / `hdrEffective`, an
`*_unverifiable` code, a `readability` state of `denied` / `failed`, or `verifiedOnThisMachine: no` each mean that
question was never answered on this machine - report them as unverified instead of folding them into success or into a
capability claim, and say which stream you read the result from when you report one.

| code | exit | what it actually says |
| --- | --- | --- |
| `cli.*` (`invalid_number`, `invalid_value`, `invalid_format`, `unknown_option`, `unknown_language`, `crop_conflict`, `monitor_conflict`, `query_conflict`, `window_query_conflict`, `stdout_multiple_targets`, `monitor_selector_*`) | 1 | the command line does not fit the contract; nothing captured, no dialog, no file |
| `cli.invalid_regex` | 1 | the pattern hit the engine's backtracking limit (`stage=match`) - **raising `--timeout-ms` does not help**; rewrite it or use `--title-contains` |
| `capture.unsupported` | 1 | that channel cannot serve that target kind (e.g. `dwm` / `printwindow` for a whole screen) |
| `capture.cursor_unsupported` / `capture.hdr_unsupported` | 1 | an explicitly required cursor or HDR state that the named channel cannot deliver; **no channel was substituted** |
| `match.no_window` | 4 | conditions too narrow, or the target is minimized (never capturable) - re-list and widen |
| `match.ambiguous_window` / `match.monitor_ambiguous_id` | 5 | several things match and the tool will not pick one - disambiguate, or name it from a fresh query |
| `match.index_out_of_range` / `match.monitor_out_of_range` / `match.roi_out_of_range` | 1 | out of range for what actually matched; `hint` lists what is there, `--roi` is judged before the dialog |
| `match.monitor_unknown_id` | 4 | that selector is not on the desktop now - re-run `--screens`; it does **not** fall back to the primary monitor |
| `capture.access_denied` | 6 | a person answered No or closed the box (`stage=consent`) - stop and ask them; or the target itself is protected |
| `capture.consent_timeout` | 6 | nobody answered in time - a refusal, never treated as consent |
| `capture.consent_unavailable` | 6 | no interactive desktop to show it on - a different session is the fix, not a second ask |
| `capture.consent_stale` | 7 | the facts shown to the person are gone (topology moved, or the area is outside what was approved) - re-discover and confirm in a new request |
| `capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable` | 7 | the handle is destroyed, now owned by another object, no longer satisfies its condition, or an identity question went unanswered - re-enumerate and re-select; consent does not transfer |
| `capture.monitor_changed` / `capture.monitor_unverifiable` / `match.monitor_id_unverifiable` | 7 | that monitor left the desktop or changed picture, or its identity could not be re-checked - re-run `--screens` and confirm again; nothing was substituted |
| `capture.failed` / `capture.timeout` / `capture.frame_timeout` / `capture.window_gone` | 7 | acquisition failed for this target; read `stage` and `backend` before deciding (`--verbose` echoes `input.captureChain`) |
| `capture.frame_invalid` | 7 | the frame does not describe its own memory (zero size, side over 16384, bad pitch, over 1 GiB), judged before allocation; also the code for a crop or scale that "should have fitted" and could not be applied |
| `capture.worker_failed` | 7 | **this tool's own helper process** failed (spawn blocked, pipe broke, protocol mismatch, invalid task; helper exit code in `hint`, helper codes not part of the contract) - check the execution environment (policy, antivirus, permissions), not the target window, and not a refusal |
| `capture.encoder_unavailable` | 7 | the encode step itself could not run: WinRT `BitmapEncoder` (which every format goes through) missing on this machine, a COM apartment that would not initialize, or an exception out of the encoder - no image can be produced here, whatever the target is |
| `env.os_too_old` | 7 | this machine is below build 10240, where the shared encoder does not exist - **`--capture` cannot help**, and neither can another target or a retry |
| `env.channel_unsupported` | 7 | the channel you named needs a newer build than this machine has - another value or `auto` may work, but that is the user's call |
| `env.cursor_unsupported` / `env.hdr_unsupported` | 7 | the stated cursor or HDR requirement cannot be met here at all (after version and policy gates nothing was left) - read `--capabilities`, do not retry the same target |
| `io.write_failed` / `io.file_exists` / `io.output_collision` / `io.timeout` | 8 | path, overwrite or batch-name problem, or the budget ran out at the write stage - fix the name or directory; keep what already landed |
| `note.*` | - | not errors: `note.frame_uniform`, `note.capture_clipped`, `note.channel_unavailable`, `note.capture_channel`, `note.cursor_channel_skipped`, `note.hdr_channel_skipped`, `note.window_query_stale`, `note.screen_query_stale`… (`--quiet` drops notes, so ask for them when they matter) |

## Cursor, HDR, crop and scale in brief

- `--cursor include` needs a route with a real switch: only `wgc` / `screen.wgc`, and that switch needs build **19041**
  (higher than `wgc` itself). With `printwindow` / `dwm` / `bitblt` / `duplication` it is `capture.cursor_unsupported` +
  1, and `auto` drops those links one by one (`note.cursor_channel_skipped`, `env.cursor_unsupported` if none is left).
  The tool never composites a pointer shape into a frame or erases one: `cursorEffective` reaches only as far as "this
  session was set to draw it" / "this source holds none" - it is not a pixel check.
- `--hdr tonemap|refuse`: **in this build only `wgc` and `screen.wgc` fulfil an explicit policy** (check
  `--capabilities` -> `color.paths[].honorsExplicitPolicy`). Asking for one with `printwindow` / `dwm` / `bitblt` /
  `duplication` is `capture.hdr_unsupported` + 1 at parse time, and with `auto` the same judgement narrows the chain
  (`note.hdr_channel_skipped` per dropped link, `env.hdr_unsupported` + 7 if nothing is left). `duplication` reports
  `wide_gamut_unverified`: its surface may be HDR, but this build does not ask the display colour space before
  `DuplicateOutput`, so it refuses instead of guessing. `capture.hdr_refused` / `capture.hdr_unverifiable` (7) write no
  pixel **and stop the chain** - a policy verdict is never converted into a silently downgraded picture on another
  backend. `--hdr auto` (the default) changes nothing and emits none of the colour keys. This project has no HDR
  display, so `color.verifiedOnThisMachine` is `no`: the mapping math is verified offline with known colour blocks,
  while colour correctness on a real HDR frame is **not** claimed as accepted.
- Explicit cursor and HDR requirements apply through the whole fallback chain; a refusal or an unverifiable required
  capability must not become success through a less capable route. Neither option changes authorization.
- `--roi` / `--client-area` are in the delivered image's **own physical pixels** (`(0,0)` = its top-left), never
  re-read as desktop coordinates; what does not fit is refused (`match.roi_out_of_range` + 1 before the dialog,
  `capture.roi_invalid` + 7 once the frame is in hand), never slid inside or swapped for the whole window. `--scale`
  only ever shrinks (nearest neighbour; crop, then scale, then encode) and cannot rescue a frame that failed the shape
  check. `cap.crop_apply_failed` / `cap.scale_apply_failed` are localized message text, **not** codes - branch on
  `capture.frame_invalid`.

## Recipes

```powershell
$exe = "P:\yashi\EvernightCapture\.agents\skills\ecapture-screenshot\ECAPTURE.EXE"   # substitute your checkout

& $exe --capabilities                                   # what can this machine do: read-only, no dialog
& $exe --list   --process notepad.exe                   # who matches, as structured JSON (paged)
& $exe --inspect --hwnd 0x001A0B4C                       # confirm one window + its identity constraints
& $exe --screens                                        # monitor identities -> copy a selector
& $exe --hwnd 0x001A0B4C --yes --timeout-ms 5000 --out D:\shots\one.png
& $exe --class CabinetWClass --index 1 --capture wgc --yes --roi 20,40,120,80 --out D:\shots\part.png
& $exe --pid 12345 --title-contains Report --all --yes --out "D:\shots\rpt_%i.png"
& $exe --monitor device:DISPLAY1 --consent-timeout-ms 60000 --out D:\shots\screen.png   # whole screen: the user answers
```

The `--monitor device:…` line reads desktop pixels: tell the user what will be in the picture before starting it, and
do not add `--yes` there - it changes nothing on that route. The `--hwnd` lines assume a handle taken from a query you
just ran in the same session.

## Resources

`references/cli-contract.md` (Chinese) is the single detailed contract; load the section you need instead of
guessing. Its section headings, usable as grep patterns: **全部选项** (every option, value range and the AND/OR plus
never-a-match rules closing that section) · **取值写法** (numbers, positional values, repeats) ·
**运行环境与能力检查** (per-route API floors vs the declared floor vs what was actually tested) ·
**只读的能力查询** · **只读的窗口查询** (`windowquery` / `windowinspect` field tables, pagination, per-field
`readability`) · **只读的屏幕枚举** · **窗口内部裁剪** · **等比缩小** · **截图授权** (the two tiers, the full
`images[].path` / `scope` registry, what the dialog lists and how the consent snapshot is bound) ·
**期限与阻塞隔离** (the helper process, the pipe shutdown boundary, the honest limits) · **JSON 结构** (complete key
tables for a window image and a screen image) · **退出码** · **行为变更** (why an omitted `--out` no longer hides the
real code) · **诊断码全表** · **帧的形状与像素上限** · **输出名占位符** (`%i %h %p %n %d %t`) · **各 shell 的坑** ·
**常用配方**.

For humans and for the product story, the repository README (`README.md`, plus `README.zh-CN.md`, `README.zh-TW.md`,
`README.ja-JP.md`) carries the same contract with background. `--help` stays the authority on the option list.
