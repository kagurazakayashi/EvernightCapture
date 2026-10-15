---
name: yashi-evernight-capture
description: Capture window or full-screen images on Windows with ECAPTURE.EXE (EvernightCapture) by selecting targets through conditions. Use when taking a screenshot, grabbing one specific app / window / dialog, discovering which windows match (--list / --inspect as structured JSON), naming a monitor by a stable identifier (--screens), capturing several windows at once, capturing one whole monitor, or interpreting ECAPTURE's JSON result, exit codes and stable diagnostic codes. Prefer read-only queries over probing by capturing. Any route that samples desktop pixels (whole screen, bitblt, duplication, DWM's screen fallback) requires the user's own confirmation - present it to the user instead of answering for them.
argument-hint: <window conditions> <output path>
license: MulanPSL-2.0
---

# ECAPTURE screenshot guide

## What this is

`ECAPTURE.EXE` (EvernightCapture) is a Windows command-line screenshot tool: it selects windows by conditions and
writes that window's picture - or a whole monitor's - to an image. Success and failure both come back as JSON with
stable exit codes and stable diagnostic `code` values, so it is meant to be driven programmatically. Current build:
`1.0.0`, stage `release` (`--version` prints both).

Three kinds of call, and the difference matters:

- **Read-only queries** - `--capabilities`, `--diagnostics`, `--screens`, `--list`, `--inspect`, `--show-config`,
  `--dry-run`. They take no pixel, show no dialog, write no file, create not even the history folder, change no
  display setting, and never probe by capturing something. Use them to decide. They need no output path.
- **A real capture of a window's own content** (`wgc` / `printwindow` / `dwm.thumbnail`) - runs unattended once you
  pass `--yes`.
- **A real capture that samples desktop pixels** (any whole screen, `bitblt`, `duplication`, DWM's `dwm.screen`
  fallback) - **a person has to answer the dialog**. Say what will be captured, start it, and wait for them.

## Installing and deploying this skill

This skill is delivered by the Windows installer produced by the repository's `build-installer.ps1`. Three things are
separate and must not be conflated:

- **File installation** - where `SKILL.md` and `ECAPTURE.EXE` physically are. The installer defaults to
  `%UserProfile%\.agents\skills\yashi-evernight-capture` but the user can choose **any** directory; that chosen
  directory *is* the skill root, with `SKILL.md` directly in it (no extra nested folder of the same name).
- **Skill discovery** - whether an AI tool finds it automatically. **Do not assume every tool scans `.agents/skills`.**
  Only a tool whose documented mechanism is verified to include this location discovers it on its own; otherwise the
  user has to point the tool at the directory explicitly.
- **Command execution permission** - whether you are allowed to run the executable at all. Installing a file grants no
  extra authority: every capture rule in this guide (especially "a person answers the desktop dialog") still applies.

A generic way to hand it to a tool that does not auto-discover: tell the tool to **read the `SKILL.md` in the actual
install directory explicitly**, then call the `ECAPTURE.EXE` **in that same directory by absolute path** (see below).
This is a manual pointer, **not** automatic registration. The first verification step you ask for should be read-only
only (`--version` / `--capabilities`) - it must not prove a working install by taking a screenshot.

## Where the executable is

This skill ships `ECAPTURE.EXE` **next to this `SKILL.md`**: in an installed copy they sit in the same directory (the
installer's chosen Skill root), so the workflow is: find this file, then take the executable beside it.

1. **Locate the directory that holds the `SKILL.md` you actually loaded** - it is not always the default one. The
   installer defaults to `%UserProfile%\.agents\skills\yashi-evernight-capture`, but the user may have picked any
   other folder; a repository checkout keeps it under `.agents\skills\yashi-evernight-capture`. Never hard-code a
   single path, and never fall back to a `PATH` lookup.
2. **Build the absolute executable path from that directory and always quote it.** `$PSScriptRoot` only has a value
   *inside* a `.ps1` file - interactively it expands to nothing, so use the literal path there.

An earlier release called this skill `ecapture-screenshot`; if you meet that name, treat it as the same skill and prefer
the current `yashi-evernight-capture` location. The old location is not a second, independent guide.

```powershell
# PowerShell. In a .ps1 file the directory is $PSScriptRoot; interactively paste the literal path.
$skillDir = "$env:USERPROFILE\.agents\skills\yashi-evernight-capture"   # <- your actual install directory
$exe = Join-Path $skillDir 'ECAPTURE.EXE'
& $exe --process notepad.exe --yes --timeout-ms 5000 --out D:\shots\epad.png
```

```bat
:: cmd.exe - the same absolute path, quoted
"%UserProfile%\.agents\skills\yashi-evernight-capture\ECAPTURE.EXE" --capabilities
```

Do not assume the tool is on `PATH`.

- `ECAPTURE.EXE --help` is the authority on the option list. **`--help` exits with code 3 and `--version` with 0;
  neither is a failure.** Add `--lang en` for English message text (`zh-CN` / `zh-TW` / `en` / `ja`).
- **A copy can be older than this guide while still printing `1.0.0`.** Ask it read-only and compare the **contract**
  and the **identity**, not the version string: `--capabilities` -> `cursor.paths[]` rows for `duplication.frame` /
  `screen.duplication` say `capability: "pointer_state_unverified"` (an old copy says `excludes_cursor`), and
  `color.paths[]` rows carry `honorsExplicitPolicy` (an old copy has no such key); `program.buildId` differs between
  builds. A **source checkout's** copy can be stale - that says nothing about a **newly installed** package, whose
  `ECAPTURE.EXE` comes from that installer's own build. Either way, read `program.buildId` / `--diagnostics` before
  trusting a copy.
  **A stale binary quietly accepting something this guide says is refused is not evidence the requirement was met** -
  it reports older, now-known-unreliable basis values, so treat its output as unverified and say so.
  If a binary contradicts this guide or refuses a documented option, **report it and let the user decide**: building
  (`.\build.ps1` -> `build\ecapture.exe`) or replacing an installed executable is a change to a published artifact, so
  do not build, install or overwrite anything on your own.
- Git Bash mangles `/help` as a path and **collapses backslashes** in arguments like `--monitor id:...`:
  `export MSYS2_ARG_CONV_EXCL='*'`, quote the value, and pass Windows-style paths.

## User preferences file: `ECAPTURE.ini` beside that executable

- **The first time you work in a directory here, run the read-only view** `& $exe --show-config` (no window picked,
  no pixel, no dialog, no file, not even an output or history folder created, and it needs no `--out`): it says which
  file this run reads, whether it was read (`absent` is the normal case - built-in defaults, and the tool never writes
  that file for you), and which level settled every setting - `cli` / `config` / `builtin`.
- **The command line always overrides the file** (explicit CLI > explicit INI > built-in default), and "not written"
  is different from an explicit `false` / `0` / `none` / `unset`. When you need reproducible conditions - a script, a
  self-check, an A/B comparison - pass `--no-config` explicitly: that run ignores the file entirely and deletes or
  rewrites nothing. `--config <path>` names one file for this run only and never falls back to the default one.
- **A file that cannot be read or does not parse fails the request before anything happens** - before the window is
  picked, before any dialog, before a single pixel - as a `config.*` code on **stderr** (exit 8 `config.not_found` /
  `config.read_failed`, exit 1 for every syntax / type / range / version / combination case), each naming the file it
  tried, with a `line` number where one applies and an ASCII `detail` token. It never degrades into "take a different
  kind of picture with defaults", and the tool never repairs the file. Order of work: `--show-config`, then
  `--no-config` to see whether the complaint concerns configuration at all, then fix that line yourself.
  `--help` (3) and `--version` (0) still
  work with a broken file - that path does not read it.
- **A preference is not a promise.** `cursor` / `hdr` / `crop` / `scale` written in the file are the same real
  requirements as when you type them, gated by the identical rules: they narrow the fallback chain or are refused
  (`capture.cursor_unsupported` / `capture.hdr_unsupported` / `env.*`) when the route cannot deliver them, and
  `unset` keeps the option genuinely unspecified. Never read "it is in the config" as "it was honoured" - read the
  result fields.
- **History on or off is a user decision, and you confirm it from JSON, not from `--capabilities`**:
  `images[].history.status` has four distinct values `saved` / `skipped` / `failed` / `disabled`, and `disabled`
  (`history.enabled = false`, or `--history off`) is **not** a failure - no `code`, no `errors` entry, and no existing
  history read, pruned or deleted. `--capabilities` only states the rule and the factory default (its
  `history_run_state_not_reported` caveat says this document does not report this run's state), so for the actual
  setting use `--show-config` (`history.enabled`, with its source) and for the actual artifact use that run's
  `images[].history`.
- **A notification is display state, never proof anyone saw it.** `notification.status = submitted` means one toast
  was handed to the Windows notification platform; Focus assist, notification settings and an unregistered
  AppUserModelID each leave the screen showing nothing while that field still reads `submitted`. So never treat it as
  "the user has been informed" - anything you need them to know still belongs in your own reply. And never re-shoot
  a delivered image to "fix" a `failed` / `disabled` notification: that step cannot change `images[]`, `errors[]` or
  the exit code, so there is nothing for a recapture to repair.
- **Never re-capture an image that already delivered** to "repair" a `config.*` complaint or a missing history copy -
  the primary file on disk is the delivery, and re-shooting costs the user another authorization and puts a second
  frame on disk. See *Handling a result*.

## Call flow

1. **Ask what this machine can do, read-only.** `--capabilities` gives version and session, each backend as
   `available` / `unavailable` / `unverified`, formats, exactly which internal paths `--yes` covers, the chain `auto`
   would walk here, and the value ceilings. See *Evidence layers* below before you read anything into it.
   `--diagnostics` is the same judgement plus a checkable build id, for a bug report. Both take only `--lang`, `-v`,
   `-q`; mixing them with capture intent is `cli.query_conflict` + 1. Do not add `-q` when you need the `caveats`
   array - `--quiet` drops it. `--config` / `--no-config` are accepted everywhere (choosing which file to read is not
   capture intent), and these queries do read that file - if a broken `ECAPTURE.ini` is in the way, add `--no-config`
   to see the half of the answer that has nothing to do with preferences. `--show-config` is the same read-only habit
   for preferences (see *User preferences file*).
2. **Discover the target as data.** `--list --process app.exe` (several matches are the normal answer; it pages with
   `--offset` / `--limit`, total in `pagination.matched` - the page size you actually got is `pagination.limit`, whose
   source is `pagination.limitSource` (`cli` / `config` / `builtin`: a preference can change that default, so do not
   assume 50), `--inspect --hwnd 0x…` (one window - it uses the same
   selection policy as a capture and answers `match.ambiguous_window` + 5 rather than pick one for you), `--screens`
   (monitor identities; copy `screens[].selectors.device` or `.id` verbatim into `--monitor`).
   **Read `errors` even when the list looks empty**: a query whose evaluation could not finish still prints
   `windows: []` with `matched: 0`, and "the question failed" is not "nothing matches".
3. **Select it with conditions that actually pin it down**, taken from the snapshot you just ran: `--class` /
   `--process` / `--exe` are case-insensitive and steadier than `--title*` (case-sensitive); name a window by handle
   from a fresh query when the choice matters. Re-query instead of reusing a handle from an earlier session - `0x…`
   values get recycled.
4. **Capture, pointing the output at a real file.** `--out D:\shots\name.png` (the directory must already exist),
   `--yes` only when the route stays a window-content one, and a budget only after reading *Time* below. The tool also
   keeps one more copy of every delivered image under `history\<local date>\` next to the executable it was actually
   run from - say so when you tell a user where their pictures went, and remember that it is persistent data this tool
   never prunes, uploads or deletes.
5. **Branch on `errors[].code`.** Check `errors` before `images`; `captured` always equals the number of `images`
   entries. `images[].source` / `path` / `scope` say which channel and which internal route really produced the frame
   and whether its pixels are the window's own or the screen's. `images[].history` says what happened to that extra
   copy - a *second* delivery, so `failed` / `skipped` there never means the image is missing, and never re-capture
   because of it (see the table below). Never branch on `message` (follows `--lang`) or on the exit code alone.

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

`session.integrityLevel` is a fourth kind of fact, and it is the **only** evidence about the mandatory integrity
level of *this* process - the one the executable actually runs at. Three things are often conflated and are not the
same fact: the label a folder carries (`icacls` on that folder), the label a file inherits from it, and
`whoami /groups` in the shell that launched the tool (that one describes your shell, not ECAPTURE). A low label on
the folder is one way a process ends up below Medium, not a proof either way - read this field itself.
Below Medium this tool is neither "supported" nor "broken": on the one development machine this project measured,
`wgc` and `printwindow` were refused, creating a file in a normal (Medium) directory failed even after the frame was
already acquired, while `dwm.thumbnail` and the two desktop-pixel routes still delivered - that is one measurement,
not a promise for every machine, and `dwm` escalates to the desktop-pixel route when the window hands back no
picture, which still always requires the person's own answer. Nor is `io.write_failed` + `win32: 5` proof of this
cause: an occupied target, a path that does not parse, or a directory that genuinely refuses writes produce the same
code pair. `status` says nothing about any of that - that column is the version floor and screen topology - so read
this field as its own layer, and treat an `unknown` there as a question that went unanswered rather than as either
a downgrade or an all-clear. Changing a label, an ACL, an install location, or elevation is a separate,
owner-authorized decision; this tool never does any of it, and it never elevates.

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
- **Every delivered image also gets a history copy** (factory default on; the user can turn it off with the
  `history.enabled` preference or `--history off` - see *User preferences file* below): an independent copy of the
  same already-encoded bytes in `history\<local date>\` **next to the
  `ECAPTURE.EXE` that is actually running** - next to the installed one when it is installed, next to
  `build\ECAPTURE.EXE` when you run the development binary. Not the working directory, not the `--out` directory.
  It is a second delivery, so it has its own verdict: `images[].history.status` is `saved` / `failed` / `skipped` /
  `disabled` (`disabled` = the user switched archiving off, **not** a failure, and it carries no `code`),
  `history.file` appears only when the copy really was committed, and `history.code` carries the stable
  `history.*` code on `failed` / `skipped` (details on the matching `errors` entry, `stage: history`). `--quiet`
  cannot hide any of it. Say so when you tell a user where their pictures went: the tool keeps a second persistent
  copy, and it never rotates, prunes, uploads or deletes it - removing history is the user's own deliberate action.
- **Each delivered image also hands the notification platform one toast** (factory default on; `notification.enabled`
  / `--notify off` to stop). Read `notification.status` as display state only: `submitted` (handed over; `submitted`
  counts how many, `withImage` how many of those carried a picture) / `failed` (note carries `notify.send_failed` or
  `notify.unavailable` with the raw HRESULT) / `disabled` (the user switched it off - not a failure) /
  `no_delivery` (nothing landed, so there was nothing to report). The image it shows **is** that history copy
  (`images[].history.file`), so a run with history off, or where the copy did not commit, sends the text with no
  picture - that is the documented consequence, not an error. How much leaves the process is the user's choice:
  `notification.detail` = `title` (window title included, readable on the lock screen) / `basic` / `minimal`.
  Read-only queries never send one and grow no such field; the helper process never can.
- **Say why you took the picture, on one line.** `--purpose "<text>"` puts your own sentence as the first body line of
  that notification, so a human glancing at the screen sees which task this capture belongs to. It is capped at 200
  characters, echoed verbatim as `input.purpose` under `-v`, and it decides nothing - it does not select a target,
  does not substitute for consent, and cannot change the result. Prefer a real purpose over an empty call: it is the
  only part of this feature that tells the human what your run was for.
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
| Command line does not fit the contract | `cli.*` (1) - `unknown_option`, `missing_value`, `switch_takes_no_value`, `invalid_number`, `invalid_value`, `invalid_format`, `unknown_capture_method`, `unknown_language`, `conflicting_options`, `crop_conflict`, `monitor_conflict`, `monitor_selector_empty`, `monitor_selector_kind`, `query_conflict`, `window_query_conflict`, `config_view_conflict`, `stdout_multiple_targets` | fix the invocation; nothing was captured, no dialog, no file. Full list in the reference |
| The preferences file cannot be read, or does not parse | `config.*` - **8** for `not_found` / `read_failed`, **1** for every syntax / encoding / size / duplicate / unknown-key / version / number / boolean / enum / range / combination / output-path case (`path_ambiguous`: `output.path` written as `C:shot.png` or `\shot.png`; `path_unresolvable`: this side could not settle the base); nothing is guessed and the caller's current directory is never used to complete such a path; `stage=parse`, with `line` (1-based) and an unlocalized `detail` token, on **stderr** | nothing was selected, asked or captured, and the tool never repairs the file. Run `--show-config`, then `--no-config` to see whether the complaint concerns preferences at all, then fix that line - and do not quietly point `--config` at some other file to make it go away |
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
| **`access denied` while `session.integrityLevel` says below Medium** - the executable may sit in a folder carrying a `Mandatory Label\Low Mandatory Level` label (`.agents` skill roots are a known case on the machine this project measured), and that round also carries `note.low_integrity` | `capture.failed` 7 with `hresult: 0x80070005` (`wgc`) or `win32: 5` (`printwindow`), and / or `io.write_failed` 8 with `win32: 5` **after the frame was already acquired**, and / or - image delivered, copy refused - `history.write_failed` 7 with `win32: 5`, `stage: history` (the archive root sits next to the running exe, so the same label blocks it; the `value` names the path that was refused, and the already-delivered picture must not be re-shot) | low integrity is **one possible cause of this code pair, not its only one**: an occupied target, a path that does not parse, or a directory that genuinely refuses writes look the same in the code, so check `session.integrityLevel` from `--capabilities` (that field, not `icacls` on the folder and not your parent shell's `whoami /groups`, is the statement about this process). Raising `--timeout-ms`, renaming the target, reinstalling codecs or looping through backends cannot change it. What was measured to still work on that one development machine: `--capture dwm` for a window's own picture - which is a measurement, not a guarantee elsewhere, and it escalates to the desktop-pixel route when the window gives no picture - and writing `--out` into a folder that carries the same low label. Desktop-pixel routes still need the person's own Yes; the level changes nothing about authorization. Telling the user to run as administrator is not the fix. Relocating the install, removing the label or changing an ACL is a security-setting change: **propose it and let the owner do it deliberately, never do it yourself as part of making a capture "pass"** |
| Output problem | `io.write_failed`, `io.file_exists`, `io.output_collision` (8) | create the directory, or choose a name that cannot collide; keep what already landed |
| **The image was delivered but its history copy was not** | `history.unavailable`, `history.write_failed`, `history.file_exists` (a copy never overwrites an existing history file), `history.budget_spent`, `history.same_file` - exit **7** with the image still in `images`, `images[].history.status` = `failed` / `skipped`, `stage: history` | **do not capture again**: re-shooting cannot produce that copy, it costs the user another authorization and it puts a second frame on disk. Use the image you already have, and say plainly that the extra copy did not land. `skipped` is not a failure of anything (`budget_spent` = the shared budget was already spent; `same_file` = the `--out` name *is* that archive name, so writing it would overwrite the deliverable). What could actually change it is the location - writable, a real directory, not a reparse point - never a different `--capture`, a different format or a bigger budget. Never escalate, re-label or re-ACL anything to make the archive pass |
| **`notification.status` is `failed`, or it says `submitted` yet the user saw nothing** | `notify.send_failed` (the platform refused this handover; `hresult` is that call's own value) or `notify.unavailable` (this route could not be answered at all - no `hresult`, because none was given). A missing picture on a `submitted` toast is usually `images[].history.status` not being `saved`, which is a separate fact and not an error | Do **not** re-shoot the image to "repair" it, and do not report the capture as failed: the picture already delivered and this step cannot reach `images[]`, `errors[]` or the exit code. What can actually change the outcome is the app identity - the AppUserModelID the installer registers on a Start Menu shortcut - so a copy run from `build\`, or an install whose shortcut was later removed, may hand over successfully while nothing appears. Check `images[].history` before promising a picture, and if the user needs to know something, say it in your own reply: `submitted` is a handover, never evidence anyone saw it |
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
- **The same requirement can arrive from the preferences file.** `capture.cursor` / `capture.hdr` and the `crop` /
  `scale` groups in `ECAPTURE.ini` pass the identical gates as when you type them (and `unset` in the file keeps the
  option genuinely unspecified, byte-for-byte the pre-option report). A CLI `--roi` / `--client-area` replaces the
  whole configured crop group, the first CLI `--scale` drops the whole configured scale group, and `--no-crop` /
  `--no-scale` cancel the configured group for that run - giving either of those together with its own CLI option is
  `cli.conflicting_options` + 1, not a silent precedence rule.

## Recipes

```powershell
$exe = Join-Path "$env:USERPROFILE\.agents\skills\yashi-evernight-capture" 'ECAPTURE.EXE'   # substitute your install dir

# Safe to run as-is: all five are read-only (no pixel, no dialog, no file, no output path needed, and no folder -
# not even history - is created). Run --show-config first in a directory you have not worked in.
& $exe --capabilities                                   # what can this machine do
& $exe --show-config                                    # which preferences file, and which level set each item
& $exe --list   --process notepad.exe                   # who matches, as structured JSON (paged)
& $exe --inspect --hwnd 0x001A0B4C                      # confirm one window + its identity constraints
& $exe --screens                                        # monitor identities -> copy a selector verbatim

# These are capture templates: adapt the conditions to a target you actually established, and expect a
# dialog on any desktop-pixel route (the last one below always asks). For a script or a self-check that needs
# reproducible conditions, add --no-config - it ignores the preferences file for that run and changes nothing on disk.
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
"%UserProfile%\.agents\skills\yashi-evernight-capture\ECAPTURE.EXE" --process notepad.exe --yes --out - 1> D:\shots\snap.png 2> D:\shots\result.json
```

## Resources

`references/cli-contract.md` (Chinese) is the single detailed contract; load the section you need instead of
guessing. Its section headings, usable as grep patterns: **全部选项** (every option - including `--config`,
`--no-config`, `--show-config`, `--history`, `--integrity-hints`, `--no-crop`, `--no-scale` - value range and the
AND/OR plus never-a-match rules closing that section) · **取值写法** (numbers, positional values, repeats) ·
**运行环境与能力检查** (per-route API floors vs the declared floor vs what was actually tested) ·
**只读的能力查询** · **进程完整性级别** (what a below-Medium process is refused, which routes still deliver, the two
reporting outlets and the three ways out) · **只读的窗口查询** (`windowquery` / `windowinspect` field tables, pagination
including `limitSource` and the effective `defaultLimit`, per-field
`readability`) · **只读的屏幕枚举** · **窗口内部裁剪** · **等比缩小** ·
**用户偏好配置** (the beside-the-exe location rule, absent-is-normal vs broken-fails-before-any-pixel, the full
section / key / factory-default / allowed-range / matching-CLI-option table, what is deliberately *not* a preference
and why, the CLI > INI > built-in chain plus "not written vs an explicit false / 0 / none / unset", the one
output-format precedence and its `formatBasis` values, which formats the encoder linked into **this** build
can actually write (one table, `png` / `jpg` / `jpeg` / `bmp` / `tiff` / `gif`, and the two very different
"not supported" answers: no writer on this codec path vs. the library not linked into this build), the
alpha and HDR boundaries of the encoding step, the two relative-path origins and the `%` template boundary,
whole-group crop / scale replacement and the two cancel switches, verbosity as one group, INI-written cursor / HDR as
real requirements, the syntax rules with the 256 KiB / 4096 bounds, the whole `config.*` table with its exit-code
attribution and the new `line` / `detail` fields, the complete `configview` contract, and the three-step
troubleshooting order) · **截图授权** (the two tiers, the full
`images[].path` / `scope` registry, what the dialog lists and how the consent snapshot is bound) ·
**期限与阻塞隔离** (the four clocks, the helper's segmented deadlines, the pipe shutdown boundary, the honest limits) ·
**JSON 结构** (complete key tables for a window image and a screen image, `images[].history` - four statuses - included,
and which streams carry configuration failures) · **退出码** · **行为变更** (why an omitted
`--out` no longer hides the real code) · **诊断码全表** (every code, including the ones above, the `config.*` family and
the `history.*` family) ·
**截图历史归档** (where the extra copy goes, the naming and exclusive-commit rules, the two-delivery partial success,
the `disabled` state when the user switched archiving off, retention and what the read-only queries do *not* probe) · **帧的形状与像素上限** ·
**输出名占位符** (`%i %h %p %n %d %t`) · **各 shell 的坑** · **常用配方**.

For humans and for the product story, the repository README (`README.md`, plus `README.zh-CN.md`, `README.zh-TW.md`,
`README.ja-JP.md`) carries the same contract with background. `--help` stays the authority on the option list.
