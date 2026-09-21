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
& $exe --process notepad.exe --yes --timeout-ms 5000 --out D:\shots\epad.png
```

```bat
:: cmd.exe - the same absolute path
P:\yashi\EvernightCapture\.agents\skills\ecapture-screenshot\ECAPTURE.EXE --capabilities
```

Substitute your own checkout if the repository lives elsewhere, and do not assume the tool is on `PATH`.

- `ECAPTURE.EXE --help` is the authority on the option list. **`--help` exits with code 3 and `--version` with 0;
  neither is a failure.** Add `--lang en` for English message text (`zh-CN` / `zh-TW` / `en` / `ja`).
- **The bundled copy can be older than this guide while still printing `0.4.0`.** Ask it read-only and compare the
  contract, not the version: `--capabilities` -> `cursor.paths[]` rows for `duplication.frame` /
  `screen.duplication` say `capability: "pointer_state_unverified"` (an old copy says `excludes_cursor`), and
  `color.paths[]` rows carry `honorsExplicitPolicy` (an old copy has no such key). `program.buildId` differs between
  builds. In this checkout the copy next to this file is exactly such an old one: it refuses `--scale` as
  `cli.unknown_option`, and it accepts `--cursor exclude` with `--capture duplication` instead of refusing it.
  **A stale binary quietly accepting something this guide says is refused is not evidence the requirement was met** -
  it reports the older, now-known-unreliable basis values, so treat its output as unverified and say so.
  If the bundled binary contradicts this guide or refuses a documented option, **report it and let the user decide**:
  building (`.\build.ps1` -> `build\ecapture.exe`) and replacing a shipped
  binary is a change to a published artifact, so do not build, install or overwrite anything on your own.
- Git Bash mangles `/help` as a path and **collapses backslashes** in arguments like `--monitor id:...`:
  `export MSYS2_ARG_CONV_EXCL='*'`, quote the value, and pass Windows-style paths.

## Call flow

1. **Ask what this machine can do, read-only.** `--capabilities` gives version and session, each backend as
   `available` / `unavailable` / `unverified`, formats, exactly which internal paths `--yes` covers, the chain `auto`
   would walk here, and the value ceilings. See *Evidence layers* below before you read anything into it.
   `--diagnostics` is the same judgement plus a checkable build id, for a bug report. Both take only `--lang`, `-v`,
   `-q`; mixing them with capture intent is `cli.query_conflict` + 1. Do not add `-q` when you need the `caveats`
   array - `--quiet` drops it.
2. **Discover the target as data.** `--list --process app.exe` (several matches are the normal answer; it pages with
   `--offset` / `--limit`, total in `pagination.matched`), `--inspect --hwnd 0x…` (one window - it uses the same
   selection policy as a capture and answers `match.ambiguous_window` + 5 rather than pick one for you), `--screens`
   (monitor identities; copy `screens[].selectors.device` or `.id` verbatim into `--monitor`).
   **Read `errors` even when the list looks empty**: a query whose evaluation could not finish still prints
   `windows: []` with `matched: 0`, and "the question failed" is not "nothing matches".
3. **Select it with conditions that actually pin it down**, taken from the snapshot you just ran: `--class` /
   `--process` / `--exe` are case-insensitive and steadier than `--title*` (case-sensitive); name a window by handle
   from a fresh query when the choice matters. Re-query instead of reusing a handle from an earlier session - `0x…`
   values get recycled.
4. **Capture, pointing the output at a real file.** `--out D:\shots\name.png` (the directory must already exist),
   `--yes` only when the route stays a window-content one, and a budget only after reading *Time* below.
5. **Branch on `errors[].code`.** Check `errors` before `images`; `captured` always equals the number of `images`
   entries. `images[].source` / `path` / `scope` say which channel and which internal route really produced the frame
   and whether its pixels are the window's own or the screen's. Never branch on `message` (follows `--lang`) or on the
   exit code alone.

## Evidence layers: `--capabilities`, and what it never said

Three facts per row, and they are three different kinds of evidence:

| Field | What it is evidence of | What it is not |
| --- | --- | --- |
| `compiled` | this binary contains the route or format | anything about this machine |
| `status` | this machine right now, by the version floor and screen topology that were readable - `unverified` means a question went unanswered, which is **not** "no" | proof that *this* window captures; drivers, protected content and HDR display state are outside this layer (`available_is_not_a_guarantee`, `device_capability_not_predicted`) |
| `verifiedOnThisMachine` / `os.matchesTestedEnvironment` | this machine matches the one OS build and architecture the project ran its on-device tests on (recorded as 19045 x64) | proof that your GPU, driver, monitor or HDR display was ever exercised. A matching label is an environment match; `no` is an honest "not measured here", not a failure |

`caveats` lists what the report does not assert; `privacy` self-documents that no pixel, dialog, file or upload was
involved. Read-only queries never probe by capturing - that is why some rows stay `unverified` instead of being
answered by taking a screenshot.

## Snapshots, identity and matching (what a query does *not* buy you)

- **Matching**: different options are ANDed, repeated values of one option are ORed, and conditions are never combined
  across two different windows.
- **A question that could not be answered never becomes a match.** A window whose title or process information the
  system refuses (`denied` / `failed`) simply does not satisfy the conditions that need it, and a failed
  `--title-regex` discards every hit it had collected instead of returning a half-evaluated list. The regex is
  compiled and run **at match time** (nothing is pre-compiled while parsing) inside the bounded helper, and its ways
  of failing are kept apart - see the table in *Handling a result*.
- A `readability` entry that could not be read is **absent or `denied` / `failed` with the raw code**, never written as
  empty, `0` or `false`. The facts here are the state plus the `GetLastError` value taken at the failing call:
  `denied` says this one query was refused. The owner running at a different privilege or integrity level is *one
  possible* reason for a refusal - the field does not establish it, so do not report a root cause the data does not
  carry, do not conclude the target is protected, and do not retry elevated. It is not a prediction about the capture
  and not a request to run as administrator - nothing here escalates.
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
  calling. There is no hidden bypass switch and no public `--worker` entry point. The repository does get past the
  dialog in two different ways during testing, and they are not the same thing - do not read one as covering the
  other:
  * an **in-process fake**: `tests/consent_state.cpp` answers `ConsentGate` with a scripted `FakePrompt`. No window
    appears and no pixel is read, and it is linked only into test binaries - it is not reachable from `ECAPTURE.EXE`.
  * a **real dialog being clicked**: `Click-EcDialogButton` in `tests/harness.psm1` posts `BM_CLICK` to a control of
    the actual `#32770` box, and under `-SimulateConsent` the desktop suites (`channels.ps1`, `screen.ps1`, `dup.ps1`,
    `identity.ps1`) answer `IDYES` to the same box that would be asking about the live screen. That runs only because
    the user deliberately chose a dedicated desktop and asked for those tests; it is a machine clicking a machine-made
    decision, not a person agreeing.
  So: "how a test gets a Yes" is never a way to call the tool, and neither mechanism is evidence that anyone
  authorized *this* request. The rule below applies to both.
- Cropping, scaling, HDR handling, cursor settings, channel fallback and worker execution all run **after** or
  independently of the decision, so none of them lowers the bar: `bitblt` with `--roi 0,0,8,8` and `--yes` still opens
  the dialog. `--capture auto` with `--yes` may walk the window-content routes silently but asks before entering any
  desktop route - approving window content is never approval of the desktop.
- **The dialog is a `MB_YESNO` box**: two buttons, Yes and No, with the default focus on **No**. Only an explicit Yes
  approves; every other result is a refusal. Declining means **clicking No**. Do not tell the user to press `Esc` or
  click `X`: with this button set the title-bar `X` is shown but disabled and there is no Cancel button for `Esc` to
  trigger, so neither is a dependable "No" gesture - the box has to be answered. The tool treats any non-Yes result
  as a refusal anyway.
- **Do not answer the dialog for the user** (no `SendMessage`, no UI automation, no scripted click, no simulated Yes -
  the repository's own `Click-EcDialogButton` included; the fact that a desktop test suite can drive it is not a
  permission for you to) and do not fake an answer. Before starting a desktop route, tell the user what will be in the
  picture and wait for them to click Yes.
- **A refusal is a decision to preserve, not a technical failure.** "No" (`capture.access_denied`, 6), nobody
  answering within `--consent-timeout-ms` (`capture.consent_timeout`, 6) or a dialog that cannot be shown
  (`capture.consent_unavailable`, 6) stop the rest of that request - including the remaining links of an `auto` chain,
  not just the remaining `--all` targets. No retry, no second ask, no other backend. Images already delivered stay
  delivered, and you report partial completion truthfully.
- `capture.consent_unavailable` is not a human saying no: it needs a session with an interactive desktop (a service,
  scheduled task or lock screen cannot answer). That is the user's call, not a retry.
- Consent is a **snapshot** taken before the box: the targets listed with their screen areas, the batch's resolved
  absolute output names, and a fingerprint of the screen topology. It is re-checked before the permit is issued and
  again before each step that would sample pixels. If the topology or a target's area changed while you waited,
  `capture.consent_stale` (7) means nothing was sampled and no permit was issued - the tool deliberately does not
  re-pop the dialog inside the same call. Unlike a refusal, this code does not stop the whole batch, **and re-asking
  is a new request**: re-discover the target, tell the user what the new picture contains, and let them confirm.
  Nothing about it is "automatic retry" material.
- The dialog is plain `MessageBox` mis-click protection for cooperative automation. It does not verify that a human
  clicked and it is not an OS security boundary.

## Output: prefer a file

- **Default to an explicit `--out <absolute path>`.** Then the whole JSON is on stdout and stderr stays empty - the
  easy case for a caller. The directory must already exist; the tool never creates it.
- Omitting `--out` **is** `--out -`: image bytes on stdout, the whole JSON on stderr, the two streams never mixed, and
  the real codes and exit codes come back unchanged. stdout carries **one** image per run, so a request that resolves
  to more than one target is rejected as `cli.stdout_multiple_targets` + 1 before any dialog or frame. `--dry-run`
  without `--out` writes nothing and puts its JSON on stderr with `note.output_defaulted_stdout`.
- Shells differ in whether they preserve a native byte stream: `cmd` and PowerShell 7.4+ do; **Windows PowerShell 5.1
  corrupts it** (bytes decoded as text, rewritten as UTF-16LE, native stderr rendered as error records) and 7.0-7.3
  decode stdout too. From those, write a file, or run under `cmd /c`, or use `Start-Process`
  `-RedirectStandardOutput` / `-RedirectStandardError`. `2>&1` is never the answer for an image.
- **Exit 0 does not prove the pixels are right.** DRM and some player drivers hand back black while reporting success;
  `note.frame_uniform` (colour as `0xAARRGGBB`) is a quality hint, not a failure, and `--quiet` hides notes without
  changing any fact. At minimum compare `width` / `height` with the target rectangle, and for a screen-reading route
  check `clipped` / `capturedRect`: a window straddling two monitors is captured only on the output it overlaps most,
  so the image may not be the whole window.
- **Partial success is real**: with `--all` or `--monitor all`, `captured` can be greater than 0 while the exit code is
  7. Never throw away what already landed.
- Every output name of the batch is planned before the first frame, and writes are atomic. Two targets expanding to the
  same name is `io.output_collision` + 8 and nothing at all is captured (use `%i` / `%h`); `--no-overwrite` is enforced
  by the final rename (`io.file_exists` + 8); a missing directory or a held-open target is `io.write_failed` + 8. A
  failed write leaves the previous file exactly as it was.

## Time: one budget, plus three other clocks

- `--timeout-ms` is **one** budget for the whole automatic stage: matching (including `--title-regex`), `auto` retries,
  frame waits, encode and write share it; no step and no further target gets a fresh copy. Omitted or `0` = no overall
  budget, while each isolated helper call is still capped at 5000 ms; **when you do give a budget, that whole remaining
  budget is what the helper gets** - there is no second built-in cap and the helper no longer carries a fixed 30-second
  self-destruct stopwatch of its own. When the budget is spent, a step that has not started is refused:
  `match.timeout` (`stage=match`) / `capture.timeout` (`stage=capture` for a frame that never arrived, `stage=encode`
  when the budget died in the encoder) / `io.timeout` (`stage=write` / `stdout`).
- **A file that already landed stays delivered.** The atomic write has no cancellation point, so a budget that expires
  during a commit still lets that file land, and the deadline is re-checked *after* the write: if it crossed, an extra
  `io.timeout` is recorded **while** the image stays in `images`, `captured` still counts it, and the run exits **7**
  (delivered + error) rather than the 8 of a write that never started. Delivery facts and deadline compliance are two
  deliberately separate records - do not report "nothing was written" about a file that is on disk, and do not hide the
  timeout either.
- **Waiting for a human is a separate clock.** `--consent-timeout-ms` bounds the dialog only; the automatic budget is
  paused while the person is deciding and is never refunded by that pause; expiry is a refusal (6), never consent. The
  bound is polled (a ~50 ms slice, then a ~3 s close grace), not preempted, so a box can linger past the number; the
  ~1 s settle after Yes belongs to the human stage and is not skipped to meet a deadline.
- **Do not promise the caller that a timeout makes this unhangable.** What is bounded: interruptible waits, and the
  helper process this tool started (a 30 s handshake window capped by the remaining budget, then the handed-down
  budget plus a 1 s deliver grace; ~2 s of parent-side reaping and cancel-drain run outside the budget). What is not
  preempted mid-call: the atomic write, a stdout pipe nobody is draining, and a WinRT encoder that ignores the cancel
  request - those are gated before they start and timed after they finish. A dialog with no `--consent-timeout-ms`
  waits indefinitely, and a hung helper is killed rather than answered. Put your own outer timeout in the caller.
- Leave enough budget for the encode+write tail, so a captured picture is not left reporting a timeout.

## Handling a result: stop, retry, or hand it to the user

**Stop and hand it back.** These are a human decision, a vanished fact or a stated policy - not "this backend failed,
try another" - and the tool itself never tries another route after them: `capture.access_denied`,
`capture.consent_timeout`, `capture.consent_unavailable`, `capture.consent_stale`, `capture.target_gone`,
`capture.target_changed`, `capture.target_unverifiable`, `capture.monitor_changed`,
`capture.monitor_unverifiable`, `capture.hdr_refused`, `capture.hdr_unverifiable`. After one of them, the next capture
is a **new request the user has to authorize**, not an automatic retry: re-discover, tell them what will be captured,
and wait.

**Retrying is legitimate only after the named condition is fixed**: `capture.failed`, `capture.timeout`,
`capture.frame_timeout`, `capture.window_gone`, `capture.frame_invalid`, `capture.roi_invalid`,
`capture.roi_unmeasurable`, `capture.cursor_unverifiable`, `match.timeout` - raise the budget, simplify the regex,
re-measure the rectangle, switch to `--capture wgc` when the target's UI thread looks stuck, or re-enumerate. Do not
answer "capture failed" with "--capture auto and hope".

**"No answer" is not a pass and not a "no".** `unverified` in `status` / `cursorEffective` / `hdrEffective`, an
`*_unverifiable` code, a `readability` state of `denied` / `failed`, or `verifiedOnThisMachine: no` each mean that
question was never answered on this machine - report them as unverified instead of folding them into success or into a
capability claim, and say which stream you read the result from when you report one. A note that `--quiet` hides cannot
repair a field you then read as an affirmation.

| What happened | Codes and exit | What to do next |
| --- | --- | --- |
| Command line does not fit the contract | `cli.*` (1) - `unknown_option`, `missing_value`, `switch_takes_no_value`, `invalid_number`, `invalid_value`, `invalid_format`, `unknown_capture_method`, `unknown_language`, `conflicting_options`, `crop_conflict`, `monitor_conflict`, `monitor_selector_empty`, `monitor_selector_kind`, `query_conflict`, `window_query_conflict`, `stdout_multiple_targets` | fix the invocation; nothing was captured, no dialog, no file. Full list in the reference |
| Nothing matched / several matched / a selector is out of range | `match.no_window` 4, `match.ambiguous_window` 5, `match.index_out_of_range` / `match.monitor_out_of_range` / `match.roi_out_of_range` 1 | re-list and widen (a minimized window is never capturable); disambiguate with a handle from a fresh query, or `--index` / `--all`; `--roi` is judged **before** the dialog. The tool will not pick one for you |
| No selection condition at all | exit 2 with plain **text** help, not JSON | `--yes` is not a condition: giving only it never means "just grab the desktop" |
| That channel cannot serve that target kind | `capture.unsupported` 1 | e.g. `dwm` / `printwindow` on a whole monitor - choose a route that can, deliberately rather than by fallback |
| `--title-regex` **would not compile** on this machine's regex library | `cli.invalid_regex`, 1, `stage=match`, `backend=match` | the pattern is wrong (it is judged in the match stage, not while parsing). Rewrite it; a budget change is irrelevant - nothing was captured |
| `--title-regex` **hit the engine's complexity / backtracking limit** | same `cli.invalid_regex`, same exit 1, but the "too complex" message | **raising `--timeout-ms` does not help** - this is a bounded resource stop, not a slow answer. Rewrite the pattern or use `--title-contains` |
| Condition evaluation **spent the budget** (regex backtracking, or a title read from a hung window) | `match.timeout`, 7, `stage=match` | raise the budget or simplify the conditions. In a query it means "this question was not answered", and no channel can be switched there |
| **This tool's own helper** misbehaved (spawn blocked, never connected, pipe broke, protocol mismatch, invalid task; helper exit code in `hint`, and helper codes are not part of the contract) | `capture.worker_failed`, 7 | check the execution environment (policy, antivirus, permissions) - it is not the target window, not "no such window" and not a refusal; report the blocker instead of switching tactics, and never treat it as permission to escalate. In queries the original code is preserved; in a *capture* request a helper failure during the **matching** step arrives as `capture.failed` with the helper's own message |
| Selector names a monitor that is not on the desktop now | `match.monitor_unknown_id`, 4 | re-run `--screens`. First check `-v` `input.monitor`: in Git Bash a backslash-collapsed `id:` value arrives mangled and legitimately reads "not found". It never falls back to the primary monitor |
| Selector matches several monitors / identity question unanswered | `match.monitor_ambiguous_id` 5 / `match.monitor_id_unverifiable` 7 | pick a more specific selector, or re-check the display topology. `match.monitor_unknown_id` (not found) and `match.monitor_id_unverifiable` (could not ask) are different conclusions |
| Screen left the desktop or changed after consent / re-check could not answer | `capture.monitor_changed` 7 / `capture.monitor_unverifiable` 7 | stop, re-enumerate and re-confirm in a new request. **Nothing was substituted** - a different screen was never approved |
| Explicit cursor or HDR state the named channel cannot deliver | `capture.cursor_unsupported` / `capture.hdr_unsupported`, 1 | **no channel was substituted**: change the requirement or the channel deliberately; see below for what duplication can and cannot promise |
| Policy verdict after the frame | `capture.hdr_refused` / `capture.hdr_unverifiable`, 7 | the user's own requirement produced it; the chain stops, no silent downgrade on another backend |
| Machine-level requirement | `env.os_too_old` 7 (whole tool, `--capture` cannot help) / `env.channel_unsupported` 7 (that channel only) / `env.cursor_unsupported`, `env.hdr_unsupported` 7 (requirement unmet here) | read `--capabilities`; retrying the same target changes nothing |
| Output problem | `io.write_failed`, `io.file_exists`, `io.output_collision` (8) | create the directory, or choose a name that cannot collide; keep what already landed |
| Timeout at the write stage | `io.timeout` 8 when nothing landed; **7 with the image kept** when the budget only crossed after the commit | fix the budget tail; do not re-capture a picture that is already delivered |
| Encode step could not run at all | `capture.encoder_unavailable`, 7 | no image can be produced here whatever the target; check the media components / this session |

Codes not listed here (including every `note.*`) are in `references/cli-contract.md`, which is the single full table.

## Cursor, HDR, crop and scale in brief

- `--cursor include` needs a route with a real switch: only `wgc` / `screen.wgc`, and that switch needs build **19041**
  (higher than `wgc` itself). With `printwindow` / `dwm` / `bitblt` it is `capture.cursor_unsupported` + 1 (their
  source pixels contain no pointer).
- **`duplication.frame` / `screen.duplication` are `pointer_state_unverified`**: the desktop duplication source may
  already have the pointer drawn into the desktop image it hands back, and there is no switch to set or read. So it can
  promise neither include nor exclude - asking for either explicitly is `capture.cursor_unsupported` + 1 (two
  different messages), `--capture auto` drops those links (`note.cursor_channel_skipped`, `env.cursor_unsupported` if
  none is left), and the `--cursor default` picture is still delivered with `cursorEffective: "unverified"` +
  `cursorBasis: "path_pointer_state_unverified"`. The tool never composites a pointer shape into a frame or erases one:
  `cursorEffective` reaches only as far as "this session was set to draw it" / "this source holds none" - it is not a
  pixel check, and an unverified value is not evidence either way.
- `--hdr tonemap|refuse`: **in this build only `wgc` and `screen.wgc` fulfil an explicit policy** (check
  `--capabilities` -> `color.paths[].honorsExplicitPolicy`). Asking for one with `printwindow` / `dwm` / `bitblt` /
  `duplication` is `capture.hdr_unsupported` + 1 at parse time, and with `auto` the same judgement narrows the chain
  (`note.hdr_channel_skipped` per dropped link, `env.hdr_unsupported` + 7 if nothing is left). `capture.hdr_refused` /
  `capture.hdr_unverifiable` (7) write no pixel **and stop the chain**.
- **Omitting `--hdr` is not the same report as writing `--hdr auto`.** Omitting it: no colour keys at all, output
  byte-identical to a build without the option. Explicit `--hdr auto`: still no colour processing (it does not probe
  the display, change the capture format or map anything), **but the colour keys are emitted** as a passive reading -
  an 8-bit `wgc` / `screen.wgc` frame then reads `hdrEffective: "unverified"` + `hdrBasis: "bgra8_source_unverified"`,
  because a passive 8-bit surface does not prove the original content was SDR. `sdr_passthrough` is only claimed when
  the display was actually asked, and that is what `note.hdr_source_sdr` means; when that question was not answered the
  note is `note.hdr_source_unverified` instead - which is not "no HDR here" either. `-v` shows `input.hdrGiven` to tell
  the two states apart.
- Pixel layout is not a colour space: a 10-bit packed frame reports `sourceColorSpace: "rgb10a2_unverified"` rather
  than being assumed PQ or HLG, and `pq_bt2020_tone_mapped` / `hlg_bt2020_tone_mapped` / `transfer_function_unknown`
  are enumerated values no real capture in this build produces (only the FP16 scRGB path maps) - do not read them as
  "a PQ panel was tone-mapped here". This project has no HDR display, so `color.verifiedOnThisMachine` is `no`: the
  mapping maths is verified offline with known colour blocks, while colour correctness on a real HDR frame is **not**
  claimed as accepted.
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

# Safe to run as-is: all four are read-only (no pixel, no dialog, no file, no output path needed).
& $exe --capabilities                                   # what can this machine do
& $exe --list   --process notepad.exe                   # who matches, as structured JSON (paged)
& $exe --inspect --hwnd 0x001A0B4C                      # confirm one window + its identity constraints
& $exe --screens                                        # monitor identities -> copy a selector verbatim

# These are capture templates: adapt the conditions to a target you actually established, and expect a
# dialog on any desktop-pixel route (the last one below always asks).
& $exe --hwnd 0x001A0B4C --yes --timeout-ms 5000 --out D:\shots\one.png
& $exe --class CabinetWClass --index 1 --capture wgc --yes --roi 20,40,120,80 --out D:\shots\part.png
& $exe --pid 12345 --title-contains Report --all --yes --out "D:\shots\rpt_%i.png"
& $exe --monitor device:DISPLAY1 --consent-timeout-ms 60000 --out D:\shots\screen.png   # whole screen: the user answers
```

Those handles and class names come from a query you just ran in the same session - re-run it rather than reusing an
old value. Every `--out` above points into `D:\shots\`, which has to **exist already** (the tool never creates a
directory; a missing one is `io.write_failed` + 8, not a capture problem). The `--monitor device:…` line reads desktop
pixels: tell the user what will be in the picture before starting it, and do not add `--yes` there - it changes nothing
on that route. To name a monitor by its cross-session `id:` selector, copy `screens[].selectors.id` **verbatim** and
quote it; in Git Bash export `MSYS2_ARG_CONV_EXCL='*'` first, or the backslashes collapse and the run reports
`match.monitor_unknown_id` + 4 even though the screen is right there.

```cmd
:: cmd.exe - image bytes on stdout, JSON on stderr, both byte-exact. Never 2>&1 for an image.
P:\yashi\EvernightCapture\.agents\skills\ecapture-screenshot\ECAPTURE.EXE --process notepad.exe --yes --out - 1> D:\shots\snap.png 2> D:\shots\result.json
```

## Resources

`references/cli-contract.md` (Chinese) is the single detailed contract; load the section you need instead of
guessing. Its section headings, usable as grep patterns: **全部选项** (every option, value range and the AND/OR plus
never-a-match rules closing that section) · **取值写法** (numbers, positional values, repeats) ·
**运行环境与能力检查** (per-route API floors vs the declared floor vs what was actually tested) ·
**只读的能力查询** · **只读的窗口查询** (`windowquery` / `windowinspect` field tables, pagination, per-field
`readability`) · **只读的屏幕枚举** · **窗口内部裁剪** · **等比缩小** · **截图授权** (the two tiers, the full
`images[].path` / `scope` registry, what the dialog lists and how the consent snapshot is bound) ·
**期限与阻塞隔离** (the four clocks, the helper's segmented deadlines, the pipe shutdown boundary, the honest limits) ·
**JSON 结构** (complete key tables for a window image and a screen image) · **退出码** · **行为变更** (why an omitted
`--out` no longer hides the real code) · **诊断码全表** (every code, including the ones above) · **帧的形状与像素上限** ·
**输出名占位符** (`%i %h %p %n %d %t`) · **各 shell 的坑** · **常用配方**.

For humans and for the product story, the repository README (`README.md`, plus `README.zh-CN.md`, `README.zh-TW.md`,
`README.ja-JP.md`) carries the same contract with background. `--help` stays the authority on the option list.
