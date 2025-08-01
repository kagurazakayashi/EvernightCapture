---
name: ecapture-screenshot
description: Capture window or full-screen images on Windows with ECAPTURE.EXE (EvernightCapture) by selecting targets through conditions. Use when taking screenshots, grabbing a specific app / window / dialog, discovering which windows match (--list / --inspect as structured JSON), listing monitors and naming one by a stable identifier (--screens), capturing several windows at once, capturing one monitor whole, or parsing ECAPTURE's JSON output, exit codes and diagnostic codes.
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

## Step 0: ask what this machine can do (`--capabilities`)

Before choosing a channel, a target kind, or deciding whether a dialog will ever be answered here, run the
read-only query. It takes **no pixel, shows no consent dialog, writes no file**, needs no window condition and
never probes by capturing something:

```powershell
& "$PSScriptRoot\ECAPTURE.EXE" --capabilities        # routing decisions: backends / formats / --yes scope / limits
& "$PSScriptRoot\ECAPTURE.EXE" --diagnostics         # for a bug report: build id + platform + backend status
& "$PSScriptRoot\ECAPTURE.EXE" --capabilities -v     # plus `probes`: each raw answer and which API produced it
```

How to read it - three separate facts per row, never to be blended:

- `compiled` - this binary implements the route at all.
- `status` - `available` / `unavailable` / `unverified` for **this machine right now** (version floors plus
  screen topology). `unverified` means one of those questions could not be answered; it is not "no".
- `verifiedOnThisMachine` - whether this project has actually exercised that route on a machine like this one
  (only the development build 19045 x64, so anything else honestly says `no`).

`available` is **not** a promise that some window will capture: drivers, protected content and HDR are outside
this layer, and the `caveats` array at the end lists exactly what the report does not assert
(`available_is_not_a_guarantee`, `no_capture_performed`, `encoder_state_not_probed`, …).
`authorization.paths` is the machine-readable version of the consent table: `consentWithYes: true` means
`--yes` cannot skip that dialog. `autoChainWindow` / `autoChainScreen` are produced by the same judgement as a
real run's `-v` `input.captureChain`, so the chain you read here is the chain you would get there.
`cursor` answers the `--cursor` question per internal path (`capability` / `reason` / `include` / `exclude`, each of
the last two `yes` / `no` / `unknown`, and `unknown` when a question gave no answer), plus the single switch with its
`minBuild` of 19041, and `pointerShapeCompositing: "never"` / `pixelRetouching: "never"`. Note that
`verifiedOnThisMachine` covers the route, not a pixel-level claim about the pointer: this layer never inspects
whether a pointer is visible in an image.
`color` answers the `--hdr` question per internal path (`capability` = `wide_gamut_capable` / `sdr_source_only` /
`unregistered`) plus the tone-mapping self-description (`toneMapping` / `floatIntermediateFrame` /
`encoderOutput: "sdr_bgra8"`). Its `verifiedOnThisMachine` is always `no`: this project has no HDR display, so the
mapping math is verified offline while color correctness on a real HDR frame is not claimed as accepted, and the
layer never probes whether this screen is currently in HDR mode (`reason` `hdr_display_mode_not_probed`).

Both documents are pure ASCII, so they do not change with `--lang` and you may diff them between runs. They are
the only JSON that carries `contract` / `contractVersion`; the capture result stays as lean as the section below
describes. `--capabilities` and `--diagnostics` accept only `--lang`, `-v` and `-q` - combining them with any
capture option or an output path is `cli.query_conflict` + exit 1, and nothing is captured.

## Step 0b: find the target as data (`--list` / `--inspect`)

These two are the read-only window discovery entry points. They take **no pixel, call no capture backend, show no
consent dialog, write no file, and activate or restore no window** - and they need **no output path at all**.
They reuse the exact same matching semantics as a capture (`OR` within one option, `AND` across options,
`--monitor` filtering, `--title-regex` in the same isolated helper), and they report **fields**, not a sentence
you have to parse again.

```powershell
& "$PSScriptRoot\ECAPTURE.EXE" --list --process notepad.exe            # contract: windowquery  - windows[]
& "$PSScriptRoot\ECAPTURE.EXE" --list=all --class Notepad              # merge minimised windows into the Z axis
& "$PSScriptRoot\ECAPTURE.EXE" --list --offset 50 --limit 50           # page; total is pagination.matched
& "$PSScriptRoot\ECAPTURE.EXE" --inspect --hwnd 0x001A0B4C             # contract: windowinspect - one window object
& "$PSScriptRoot\ECAPTURE.EXE" --inspect=path --title 订单             # also emit the full image path (opt-in)
```

- **`--list` never reports ambiguity** - several matches are the normal answer, and it pages. **`--inspect` uses the
  same selection policy as a capture** (`--index` / `--topmost-match` / `--bottommost-match`), and if that policy
  cannot single out exactly one window it says so: `match.ambiguous_window` + exit 5. **It will never pick one for
  you.** `--all` with `--inspect` is a usage conflict, and no match there is `match.no_window` + exit 4.
- Fields per record: `hwnd` / `pid` / `class` / `title` / `image` / `rect` (physical pixels) / `visible` /
  `minimized` / `zOrder`, plus `readability` and `identity`. The default visibility policy is stated, not implied:
  `policy.invisibleExcluded` and `zeroSizedExcluded` are `true`, `minimizedIncluded` is `false` unless you ask with
  `--list=all`, and `systemWindowAssertion: false` means the list makes **no** claim about which entries are system
  windows - it reports what it read and classifies nothing for you. `caveats.list_may_be_partial` appears whenever
  `truncated` or some minimised window was left out, so never count `windows[]` as "all windows on this machine".
- **A field that could not be read is never written as empty, `0` or `false`.** `readability` answers each
  cross-process question separately - `state` is `readable` / `denied` / `failed` with the raw `win32` code:

  ```json
  "readability": { "process": { "state": "denied", "win32": 5 },
                   "imagePath": { "state": "readable" },
                   "processStart": { "state": "readable" },
                   "rect": { "state": "readable" } }
  ```

  `denied` means what it says - the owner runs at another privilege level. It is **not** a prediction about whether
  the capture will work and it is **not** a request to run as administrator; nothing here escalates, retries with
  elevation, or guesses. `unreadable_fields_are_not_a_prediction` is in `caveats` for that reason.
- **Privacy: the full image path never appears unless you ask.** Default output carries `image` (file name only);
  `--inspect=path` is the opt-in that adds `exePath`, and it also writes `exePathRequested` / `exePathReadable` so
  "not requested", "requested but unreadable" and "readable" stay distinguishable. Titles and class names are
  delivered verbatim because they are the target's own identity.
- **`--yes` has no effect here**: `authorization.yesAffectsResult: false` is a measured statement - the document is
  byte-for-byte the same with or without it, including which fields come back. It is accepted (not a usage error)
  because `--yes` belongs to capture authorization, and a query that reads no pixels has nothing to authorize.
- Exit codes are `0` (query produced, including zero matches) / `1` (`cli.window_query_conflict`, or a condition that
  does not parse) / `4` and `5` (`--inspect` only) / `9`, plus **`7` on exactly one path**: this run's condition
  evaluation did not finish (`match.timeout`, or that step's helper process failing). That `7` means "I could not
  finish asking", not "the capture failed" - and its `hint` says so in query terms, telling you that switching
  `--capture` is useless here because there is no channel to switch.
  **`6` and `8` cannot appear** - those describe a human refusing and a file that failed to land. The document always goes to **stdout** (no image competes for
  it, so the "JSON moves to stderr" rule does not trigger), stderr stays empty. `-q` drops `notes` only; `policy`,
  `authorization`, `readability` and `caveats` are judgements and are never suppressed. `-v` adds the normalized
  `input` section.
- On a parse-level misuse the failure document is **shaped like a capture result** (`captured: 0`, `images: []`,
  `errors: [...]`), so `switch ($errors[0].code)` does not need a second branch for queries.

### The snapshot expires; identity fields are not a security token

`--inspect` (and each `--list` record) carries the constraints a later capture will check:

```json
"identity": { "hwnd": "0x001A0B4C", "pid": 27256, "class": "Notepad",
              "processStartTicks": 134351164333279485,
              "selectionNeedsRecheck": true, "verificationRequired": true,
              "isAuthorizationToken": false, "raceWindowReducedNotEliminated": true }
```

`processStartTicks` is what separates "the PID was recycled by Windows" from "still that process"; when it could not
be read it is the string `"unknown"`, never `0`. `selectionNeedsRecheck: true` means the condition that picked this
window contains volatile parts (title, or filtering by `--monitor`), so the tool will re-run that condition rather
than compare strings; `false` means a class/handle selection.

Do **not** treat any of this as a credential to cache and present later. Every window query carries
`note.window_query_stale` saying so, and the real capture re-verifies the target before reading a single pixel
anyway (`capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable`) and still asks for consent
according to where the pixels come from. If the list is more than a moment old, run `--list` again.

## Step 0c: name a monitor (`--screens`)

`--monitor <n>` used to be the only way to say which screen, and that `n` is nothing but the position in *this
run's* enumeration - not the id in Windows Settings, and not stable across a replug or a resolution change.
Guessing it does not fail loudly: it captures a screen nobody approved. `--screens` hands the identities back as
data, and two of them can be written straight into `--monitor`:

```powershell
& "$PSScriptRoot\ECAPTURE.EXE" --screens                  # contract: screens - one record per monitor
& "$PSScriptRoot\ECAPTURE.EXE" --monitor device:DISPLAY1 --out shot.png
& "$PSScriptRoot\ECAPTURE.EXE" --monitor "id:\\?\\DISPLAY#GSM41A2#5&…#{…}" --out shot.png
```

- **Read-only**, like the other queries: no pixel taken, no consent dialog, no file written, and **no display
  setting touched** - rotating a screen in order to find out whether it is rotated would be editing the exam to
  read the answer. It needs no window condition and no output path, and it never falls into "no condition = help".
- `screens[].selectors.device` / `screens[].selectors.id` are the exact strings to write back, and `identity.*`
  states which kinds are selectors at all. Stability per kind: `ordinal` = `this_invocation`,
  `deviceName` = `this_desktop_attach`, `monitorDevicePath` = `cross_session_expected`,
  `adapterLuid` = `this_session` - association only, deliberately **no** selector form, because an LUID is unique
  only inside the current session and naming a monitor with it is a bet rather than a reference.
- `dpi` (effective and raw, through `shcore!GetDpiForMonitor`, Win8.1+), `rotation.degrees` (what a person sees)
  and `rotation.panel` (relative to the panel's native orientation) are separate questions with separate
  `readability` entries; a value that could not be read is absent rather than written as `0`, and the reason
  (`denied` / `failed` plus that API's own code) sits next to it. Nothing here asks you to run elevated.
- **Naming a monitor never replaces consent.** A whole screen is desktop pixels, so the confirmation dialog always
  appears and `--yes` does not skip it; the list is also a snapshot (`note.screen_query_stale`, and `caveats`
  include `device_names_are_not_persistent` and `cross_session_stability_not_tested`), so the pre-capture identity
  re-check still happens. Branch on `errors[].code` if an identifier no longer resolves:
  `match.monitor_unknown_id` (4 - not on the desktop now, list again), `match.monitor_ambiguous_id`
  (5 - several screens share it, the tool will not pick one), `match.monitor_id_unverifiable` (7 - the identity
  question returned no answer, so nothing was captured and nothing was substituted).
- `--screens` belongs to the environment-query family: it accepts only `--lang` / `-v` / `-q`, and anything else -
  `--yes`, `--monitor`, an output path, another query - is `cli.query_conflict` + exit code 1 with every conflicting
  flag named in `value`. Its own exit codes are only `0` and `1`: it selects no target, shows no dialog, writes no
  file, so `4`/`5`/`6`/`7`/`8` cannot appear here.

## Four steps: discover, disambiguate, select, capture

1. `--list` with the window conditions to see who matches, as structured data. `--dry-run` does the same target
   selection and stops right there - no frame, no file - with candidates in the `note.dry_run` entry of `notes[]`,
   under `value`, shaped like `hwnd=0x000A1146 pid=31468 1261x614+237+418 class=CabinetWClass title=…`. Prefer
   `--list`: you get one JSON object per window instead of having to parse that string. Omitting `--out` on a real
   capture is exactly `--out -`: the image bytes go to stdout and the JSON moves to **stderr**, and the failures are
   the real ones (`match.no_window`, `match.ambiguous_window`, `capture.access_denied`, `io.write_failed`, …) with
   their own exit codes - nothing is collapsed into a generic "missing output path" any more.
   Conditions alone with nothing else still means "no condition": `--dry-run` by itself prints the text
   help and exits 2 (a window query does not require conditions at all). `--monitor` counts as a target, so
   `--monitor --dry-run` lists screens instead.
2. Several matches (exit 5): read them from a `--list` run (`windows[].hwnd` / `title` / `class` / `image` /
   `rect` / `zOrder`), then narrow with `--class` / `--process` / `--title`
   (note `--title*` is **case-sensitive**, `--class` / `--process` / `--exe` are not), or pick one with
   `--index 1` / `--topmost-match` / `--bottommost-match`, or take them all with `--all` (put `%i` in the output name,
   otherwise `note.all_without_placeholder` fires and `_N` is appended). Those pick a **z-order position in the current
   stacking order** - `--newest` / `--oldest` are the older names for topmost / bottommost and behave identically
   (Windows exposes no window creation timestamp), and using one adds a `note.deprecated_option`.
   Confirm the one you mean with `--inspect --hwnd <that handle>` before capturing when the choice matters:
   `--inspect` applies the very same policy, so an exit 5 there means the policy genuinely does not single out
   one window - and `--list` then `--inspect` then capture is three calls that never touch a pixel.
3. Point the output at the real path and drop the query switches. **For a window capture also pass `--yes`** so the
   tool does not stop on a confirmation dialog you cannot answer from a script (`--yes` only ever skips the
   dialog for paths that read the selected window itself - see "When you must ask a human first").
   Prefer naming the target by handle, taken from a query you just ran - and re-query rather than reusing a handle
   from an earlier session, because a `0x…` value gets recycled:

   ```powershell
   & "$PSScriptRoot\ECAPTURE.EXE" --inspect --hwnd 0x001A0B4C          # confirm it is still the window you mean
   & "$PSScriptRoot\ECAPTURE.EXE" --hwnd 0x001A0B4C --yes --out D:\shots\one.png
   ```

   The capture re-checks the identity itself (`processStartTicks` / class / the original condition) and reports
   `capture.target_changed` rather than silently grabbing whatever now owns that handle.
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

## Cropping inside the window (`--roi` / `--client-area`)

Both options say which part of the delivered window image to keep. They are mutually exclusive
(`cli.crop_conflict` + 1), they cannot be used for a whole-screen target (`capture.unsupported` + 1 - the four
numbers are **never** re-read as desktop-absolute coordinates; use `--monitor` for a position on the screen), and
they conflict with every read-only query.

- **The rectangle is in the image's own pixels**: `(0,0)` is the top-left pixel of the delivered whole-window
  image (that image is the visible window frame, `DWMWA_EXTENDED_FRAME_BOUNDS`), far edge exclusive.
  `--client-area` is one step further in: the title bar and the three borders are dropped as well.
- **Physical pixels, no DPI maths.** The tool is per-monitor v2 aware, so nothing here is scaled: the same
  `--roi 0,0,200,120` takes 200x120 *pixels* on a 1x and on a 2x display. Convert DIP yourself if that is how you
  measure; the tool does not guess which monitor the window is on.
- **Four decimal integers, comma separated** (`[0-9]+` only - no sign, no spaces, no dot, no exponent, no
  underscores, no `0x`, no non-ASCII digits), exactly four fields. `x`/`y` may be 0, `w`/`h` are at least 1, and
  none may exceed 16384 (`--capabilities` reports that ceiling as `limits.roiMaxValue`).
- **Anything that does not fit is refused, never repaired**: `cli.invalid_value` (1) for bad syntax,
  `match.roi_out_of_range` (1) when it is already too large for the window as selected - that runs *before* the
  consent dialog and before output-name planning, so a doomed request never disturbs a person and never writes a
  file - `capture.roi_invalid` (7) when the frame that came back is too small (the target resized, or part of it
  hangs off the screen), and `capture.roi_unmeasurable` (7) when the question needed to locate the rectangle gave
  no answer. None of them slides the rectangle inside, crops to the edge, or hands back the uncropped window, and
  one window in a batch that cannot hold it stops the whole batch.
- **The crop does not change authorization.** It runs after the frame is captured, so the tier still depends on
  `images[].path`: a desktop-pixel channel with `--roi 0,0,8,8` still always asks, and `--yes` does not start
  applying because only a small piece is kept.
- **Read these fields to know what you got**: `cropMode`, `cropRect` (image pixels), `fullWidth` / `fullHeight`
  (before cropping; `width` / `height` are after), and `cropScreenRect` - the same rectangle in virtual-screen
  coordinates, which closes the mapping (`cropScreenRect - cropRect` is the image's own screen origin). That last
  key appears only when the origin can actually be established; otherwise it is absent together with a
  `note.crop_mapping_unavailable`, which is a missing locating field, not a failure.

```powershell
& "$PSScriptRoot\ECAPTURE.EXE" --hwnd 0x001A0B4C --capture wgc --yes --roi 20,40,120,80 --out D:\shots\part.png
& "$PSScriptRoot\ECAPTURE.EXE" --hwnd 0x001A0B4C --capture wgc --yes --client-area --out D:\shots\client.png
```


## Writing option values

- **Numbers are decimal.** `--pid` (1..4294967295), `--index` (1..65535), `--monitor <n>` (1..65535),
  `--quality` (1..100), both timeouts (0..86400000) and the two window-query page counts - `--offset`
  (0..8192) and `--limit` (1..8192, default 50) - accept `[0-9]+` only, and the range is checked while
  parsing. A sign, whitespace, a dot, a thousands comma, an exponent (`1e3`), digit-separator underscores, a
  `0x` prefix or non-ASCII digits is `cli.invalid_number` + exit 1. Nothing is cast, wrapped or re-read in
  another base: `--pid 1e3` is not 483, `--quality 1e` is not 30, `--hwnd -1` is not `UINT64_MAX`. Timeout
  `0` is a real value ("no budget for this"); whitespace is not `0`.
- **`--roi` is four decimals, comma separated, all in one token.** `--roi x,y,w,h` takes exactly four
  `[0-9]+` fields - no sign, no whitespace, no dot, no exponent, no underscores, no `0x`, no non-ASCII digits -
  with `x`/`y` allowed to be 0, `w`/`h` at least 1, and none above 16384 (the frame's single-side ceiling, echoed
  by `--capabilities` as `limits.roiMaxValue`). Anything else is `cli.invalid_value` + exit 1 with the offending
  token echoed verbatim in `value`; nothing is re-read in another base or clamped into range.
- **Value-taking switches: `--list` and `--inspect`.** Both may be written bare and then swallow nothing
  (`--list out.png` keeps `out.png` as a positional, which is a conflict - the query has no output path).
  `--list=all` merges minimised windows into the same Z axis; `--inspect=path` adds the full image path.
  Those are the only accepted values: `--list=allx` / `--inspect=xyz` are `cli.invalid_value` + exit 1 and
  never degrade to the default policy. Exactly one of the two per run, and neither combines with
  `--capabilities` / `--diagnostics` or with capture-level options.
- **`--hwnd` is the one option that takes hexadecimal**, in the three spellings it documents: plain digits are
  decimal, `0x` / `0X` prefix is hexadecimal, and a bare spelling containing `a-f` is hexadecimal (the Spy++
  form, so `--hwnd 1e3` means `0x1e3`). Sign, whitespace, overflow past 64 bits and handle `0` are still
  rejected. An underscore is allowed only in the hexadecimal forms and only between two hexadecimal digits
  (`0x001A_0B4C` fine; `0x_1A`, `1A__0B4C`, `1A0B4C_`, `12_34` not). When in doubt, always write the `0x`
  prefix.
- **`--monitor` decides whether it swallows the next argument with the same grammar it parses with**: a
  malformed number there is an error, never a file name, while `out.png`, `2.png`, `v2`, `D:\a\b.png` stay
  output paths (`--monitor out.png` = primary monitor, written to out.png). The identifier forms are
  `device:<name>` and `id:<monitor device path>`, and **only those two prefixes** are eaten from a bare
  argument - so `--monitor D:\shots\a.png` still means "primary monitor, write to that file", while an
  inline `--monitor=foo:1` is `cli.monitor_selector_kind` + 1 and `--monitor=id:` is
  `cli.monitor_selector_empty` + 1 (never "then use the primary monitor").
- **Whatever follows an option that takes a value is that value**, even if it looks like another option:
  `--title --lang ja` searches for the title `--lang` - and that also means the swallowed `--lang` never
  becomes the message language. Write `--title=-x` for a value starting with `-`, or `--` to stop option
  parsing (everything after it is positional, and `--` itself is dropped, so `-- --lang ja` changes nothing).
- **Repeating an option**: match conditions OR (`--title A --title B`), value options take the last
  occurrence (`--timeout-ms 9000 --timeout-ms 300` is 300), `--lang` too - where `auto` (or an omitted value)
  resets to the system display language instead of keeping the previous choice. Boolean short options may be
  clustered (`-vq`, `-yq`), but a cluster containing a short option that takes a value (`-vl`, `-qi`) is
  `cli.unknown_option`.
- **`--verbose` and `--quiet` together are handled as `--verbose`**: notes are still delivered plus one
  `note.flag_overrides_quiet`. `errors`, and `images[].source` / `path` / `scope`, are never hidden.

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
- When `--cursor` was given, each image also carries `cursorRequested` / `cursorEffective` / `cursorBasis` (what was asked, what this path actually delivered, and on what evidence). Without that option none of the three appears, and `--quiet` never suppresses them when they are there - see the `--cursor` section below.
- When `--hdr` was given, each image also carries `hdrRequested` / `hdrEffective` / `hdrBasis` / `sourceColorSpace` / `sourceBitDepth` (the policy asked, what this frame actually went through, on what evidence, and the pre-encode source color space and bit depth; `sourceBitDepth` is omitted entirely when the source is unrecognized). Without that option none of them appears, and `--quiet` never suppresses them when they are there - see the `--hdr` section below.
  The read-only queries use their **own** key sets (`contract` / `contractVersion` / `query` / `authorization` /
  `policy` / `pagination` / `windows[]` or `window` / `caveats` for `--list` / `--inspect`; see Step 0b) -
  they never add top-level metadata to the capture document above, and the capture rules for `captured` /
  `images` / `errors` / `notes` / `input` do not apply to them.
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
- **The screen-reading routes also report *where* in the desktop they got the pixels**: `duplication` (and the
  `bitblt` / `dwm` screen routes) add `requestedRect` (the area this route set out to capture) and `capturedRect`
  (the area it really captured), both in virtual-screen coordinates so they line up with `rect`; `clipped` appears
  only when they differ, together with a `note.capture_clipped` saying how much was lost on each side, and
  `rotation` appears only when the desktop frame had to be turned 90/180/270 degrees clockwise to match the
  orientation that monitor displays in. **A window straddling two monitors is captured on the one output it
  overlaps most, so the image is not the whole window - check `clipped` before treating it as one.** Window-content
  routes (`wgc`, `printwindow`, `dwm.thumbnail`) capture the target whole, so they emit none of these keys: absence
  means "nothing was left out", not "unknown". These are location judgements too, so `--quiet` does not hide them.
- `--monitor` numbers are **the position within this run's monitor enumeration**, starting at 1. They are not the ids
  Windows Settings shows, and unplugging a display or changing a resolution reshuffles them - never cache a number
  as a screen's identity across runs. To name the same monitor again, run `--screens` (Step 0c above) and write back
  one of its selectors: `--monitor device:DISPLAY1` (the name in this desktop attach) or
  `--monitor "id:\\?\\DISPLAY#…"` (the monitor devnode path - the identifier that carries across sessions,
  and the one to store). Before capturing a screen target the tool re-checks that monitor by **identity**: by devnode
  path when that was known at selection time, by name only when it was not. So a monitor that left the desktop, or a
  device name that now belongs to a different panel, stops with `capture.monitor_changed` and nothing is captured,
  while a re-check that cannot be answered stops with `capture.monitor_unverifiable` rather than falling back to the
  name; if the rectangle changed, the new rectangle is what a person is asked to approve - an earlier confirmation is
  never reused for a resized or relocated monitor.
- Exit codes: `0` success / `1` bad arguments / `2` no condition given / `3` `--help` / `4` no match /
  `5` several matches / `6` protected target, or the confirmation was refused (`capture.access_denied`),
  could not be shown (`capture.consent_unavailable`) or nobody answered it within `--consent-timeout-ms`
  (`capture.consent_timeout`) / `7` capture failed, an exhausted `--timeout-ms` budget included
  (`match.timeout` / `capture.timeout`) and the target monitor leaving the desktop or changing after the
  confirmation (`capture.monitor_changed` - re-enumerate and re-confirm, never switch channel or screen and hope) /
  `8` write failed, budget exhausted in the write/stdout stage
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
- **A crop does not move this line.** `--roi` / `--client-area` run *after* the frame is captured, so the tier is
  still decided by `images[].path`: `bitblt` or `duplication` with `--roi 0,0,8,8` and `--yes` still opens the
  dialog exactly like a full-screen grab, and `--yes` does not start applying because only a small piece is kept.
  The dialog lists the whole target, not the cropped result, so what a person approves always covers the image.
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
- A consent refusal is reported as itself - `capture.access_denied` + exit 6 with `stage=consent` - whether or not
  an output path was given. Adding `--out` was never the fix for "a human declined", and the tool no longer says it.
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

## Whether the pointer is in the image (`--cursor`)

`--cursor default|include|exclude` asks for the mouse pointer to be in the image or not. The **default value
changes nothing at all**: the tool touches no channel's cursor setting and the result carries none of the three
cursor fields, so output without this option is byte-for-byte what it was before the option existed. Writing
`--cursor default` on purpose is different - still no change, but the result reports what that path really gave.

What is possible is decided by **where the pixels of that path come from**, not by the channel name:

- `wgc` and `screen.wgc` have a switch that is really set and read back (`IsCursorCaptureEnabled`), and it needs
  **build 19041** - higher than `wgc`'s own 18362, so a 1903 machine can capture fine and still be unable to say
  anything about the pointer.
- `printwindow`, `dwm.thumbnail`, `dwm.screen`, `bitblt.screen`, `screen.bitblt`, `duplication.frame` and
  `screen.duplication` contain **no pointer in their source pixels** (a window painting itself, the DWM
  redirection surface, a screen DC, and desktop duplication whose pointer arrives as separate metadata).
  `exclude` is therefore true of them as a fact about the source; `include` is not something they can do.

So, as an AI caller:

- `--cursor include` + `--capture printwindow|dwm|bitblt|duplication` fails while parsing
  (`capture.cursor_unsupported` + exit 1) and the tool will **not** switch channels for you. If you need the
  pointer in the image, use `--capture wgc` (or `auto`, whose chain narrows to `wgc` alone); if you do not, ask
  for `exclude` or leave the option out.
- The tool never retouches pixels: it does not fetch and draw the pointer shape, does not draw a cursor into the
  frame, and does not erase a pointer that is already there. So `cursorEffective` reaches only as far as "this
  session was set to draw it" or "this source holds none" - it never claims the pointer is or is not visible in
  these pixels (this SDK's session has no read-only `IsCursorVisible`). Do not read `cursorEffective` as a pixel
  assertion, and do not "fix" the pointer afterwards by editing the image.
- Each delivered image carries three fields, and only when `--cursor` was written: `cursorRequested`
  (`default` / `include` / `exclude`), `cursorEffective` (`include` / `exclude` / `unverified`) and `cursorBasis`
  (`wgc_session_property_set` / `wgc_session_property_read` / `path_excludes_cursor` /
  `wgc_cursor_property_unavailable`). If an explicit request cannot be confirmed on `wgc`, the capture fails with
  `capture.cursor_unverifiable` before the frame is taken rather than delivering the wrong thing.
- Asking about the pointer **does not change authorization**: `--cursor exclude` on `bitblt`, `duplication` or any
  whole-screen target still shows the consent dialog and `--yes` still does not cover it.
- `--capabilities` answers this without capturing anything, in its `cursor` section (`default`, the three values,
  that one switch's `compiled` / `status` / `minBuild`, one row per registered path with `capability` / `reason` /
  `include` / `exclude` as `yes` / `no` / `unknown`, and `pointerShapeCompositing: "never"`).

## HDR color handling (`--hdr`)

`--hdr auto|tonemap|refuse` decides what to do when the display is in HDR mode and the captured frame carries a
wide gamut / high dynamic range (WGC can return FP16 scRGB; Desktop Duplication can return FP16 scRGB or 10-bit
ST.2084 (PQ) / HLG BT.2020). Forcing such a frame into 8-bit BGRA gives a washed-out, desaturated, blown-highlight
image that "looks like a normal picture" - this tool refuses to treat that as correct by default.

- `auto` (the default) **changes nothing**: no display probe, no format change, no mapping, and none of the color
  fields appear - output is byte-for-byte what it was before this option existed. It only reports the source color
  space the frame actually came back as.
- `tonemap` maps an HDR frame to 8-bit SDR before encoding through a per-pixel float intermediate (never a whole
  float frame) and a fixed, deterministic curve (decode transfer → BT.2020→709 matrix → extended-Reinhard on
  luminance → sRGB encode → alpha passthrough). An SDR source is an identity passthrough.
- `refuse` errors out and writes no pixel once the source is confirmed HDR.

Decide per internal path, not per channel name: `wgc` / `screen.wgc` / `duplication.frame` / `screen.duplication`
can carry a wide-gamut frame; `printwindow` / `dwm.*` / `bitblt.*` are 8-bit SDR only. So `--hdr tonemap` / `refuse`
with the latter is `capture.hdr_unsupported` + exit 1 at parse time, and the tool **never** reroutes to a
desktop-reading channel (unlike `--cursor`, this option does not narrow the `auto` chain - a channel that cannot
carry HDR just passes the SDR source through). A frame in a wide format this build cannot name is
`capture.hdr_unverifiable` + exit 7 (no forcing into BGRA8, no guessing a mapping); `refuse` on a confirmed HDR
source is `capture.hdr_refused` + exit 7. The display-HDR probe (`IDXGIOutput6::GetDesc1`) is read-only - it never
changes display settings, and an unknown answer is treated as "no HDR to act on", not guessed.

When `--hdr` was written, each image also carries `hdrRequested` / `hdrEffective` / `hdrBasis` / `sourceColorSpace` /
`sourceBitDepth` (and `--quiet` does not suppress them). `hdrEffective` is `sdr_passthrough` / `tone_mapped` /
`unverified`; asking for processing on an SDR source leaves a `note.hdr_source_sdr`. **This project has no HDR
display**, so `--capabilities` `color.verifiedOnThisMachine` is always `no`: the tone-mapping math is verified
offline with known color blocks and a brightness gradient, but color correctness on a real HDR frame is NOT claimed
as accepted until re-checked on an HDR display. HDR color does not change authorization: it runs after capture and
before encoding, desktop-pixel routes still prompt, and `--yes` still does not cover them.

## Choosing a capture channel (`--capture`)

**Ask first, do not probe by capturing:** `--capabilities` already lists each channel's `status`, its
`minBuild`, whether it is even compiled in, and the chain `auto` would walk on this machine - without taking a
frame, without a dialog and without touching a target window. Use it to choose below, and use
`--diagnostics` when you need to report what you found.

Each route has a Windows build it cannot work below (that is its **API history floor**, not what this
program claims - see `references/cli-contract.md`, "运行环境与能力检查", and the README's "System support"
section): any image at all needs 10.0.10240 because every format goes through the one WinRT encoder;
`duplication` 10.0.9200; `printwindow` / `dwm` 10.0.9600 (`PW_RENDERFULLCONTENT`); `wgc` 10.0.18362
(`CreateForWindow` / `CreateForMonitor` - the namespace is from 1803 but this tool never uses the picker).
Declared support: 64-bit Windows 10 1903 (18362) or later; tested only on 19045.

| Goal | Use | Needs (Windows build) |
| --- | --- | --- |
| The window itself, even when covered | `wgc` (default) or `dwm` - both read the DWM-cached surface; neither sees through DRM protection | `wgc` 18362, `dwm` 9600 |
| What the screen looks like right now, occluder included | `bitblt` or `duplication` - they copy visible pixels only, and both need a human confirmation every single time | `bitblt` none, `duplication` 9200 |
| Try the next channel if one comes back empty | `auto`: wgc→dwm→printwindow→bitblt; for a whole screen wgc→duplication→bitblt. A successful fallback reports `note.capture_channel`. With `--yes` the first three ask nothing; entering `bitblt` asks | whatever the remaining links need; a link this build cannot offer is dropped from the chain and reported as `note.channel_unavailable` |
| A whole screen | Only `wgc` / `duplication` / `bitblt`; `dwm` and `printwindow` are rejected while parsing with `capture.unsupported` + exit 1. Whole-screen `wgc` is a desktop path too: it always asks | `wgc` 18362, `duplication` 9200 |

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
- **A delivered image can be smaller than the target without being a failure.** `duplication` takes one monitor's
  composed output, so a window straddling two screens (or hanging off the edge) is only captured where it overlaps
  that output; the tool reports `capturedRect` != `requestedRect`, sets `clipped` and emits `note.capture_clipped`
  with the number of pixels lost on each side. The same reporting applies when `bitblt` clips to the virtual screen.
  Treat `clipped` as "this is not the whole target" - do not assume a full window, and never read
  `note.capture_clipped` (or `note.frame_uniform`) as an error or as a reason to escalate authorization.

## What to do about the common codes

**Separate "this target" from "this machine".** `env.os_too_old` and `env.channel_unsupported` are about the
Windows version installed here, and they are raised before any window is enumerated, before any consent dialog and
before a single pixel is read - so do not retry the same target, do not relax the conditions, and never read them as
DRM or as a refusal. Whether another channel can help is decided by which of the two it is (see the rows below).
Ask the capability question without capturing anything: the dedicated read-only query `--capabilities`
(see "Step 0" above) answers it in one call, and `--verbose` on a real run echoes
`input.osBuild` (the Windows build this machine really reports) and `input.captureChain` (the channels this run
can actually use for the requested target kind, in order) - the same judgement, from the same function.
Floors, the declared support range and what has actually been measured are in
`references/cli-contract.md` ("系统支持") and in the README's "System support" section.

| code | exit | handling |
| --- | --- | --- |
| `env.os_too_old` | 7 | This machine's Windows build is below 10.0.10240, where the one WinRT encoder every format goes through does not exist - **switching `--capture` cannot help**, and neither can another target or a retry. Report that the environment is unsupported |
| `env.channel_unsupported` | 7 | The channel you asked for explicitly needs a newer build than this machine has (message carries both numbers). **Another `--capture` value or `auto` can help**; the tool never substitutes the requested channel on its own |
| `note.channel_unavailable` | - | A link of the `auto` chain was dropped because this build is under its floor; the image may still have been captured by another channel (`images[].source` names it). Not an error |
| `note.os_unverifiable` | - | The Windows build could not be read, so nothing was filtered by version this time. No answer is neither "unsupported" nor "supported" - judge by whatever the step actually reports |
| `match.no_window` | 4 | Conditions too narrow, or the target is minimised (minimised windows cannot be captured); relax with `--title-contains` |
| `match.ambiguous_window` | 5 | Disambiguate as described above |
| `match.index_out_of_range` / `match.monitor_out_of_range` | 1 | `--index` / `--monitor` out of range; `hint` lists everything on this machine |
| `match.roi_out_of_range` | 1 | The `--roi` rectangle does not fit the window as selected (`option` / `value` / `target` are all filled, and it fires before the consent dialog and before any output plan). Re-measure the window (`--list` / `--dry-run -v` gives its rectangle) and ask for a rectangle that lies inside it - nothing is slid inside, cropped to the edge, or swapped for the whole window |
| `capture.roi_invalid` | 7 | The frame that came back is smaller than the requested crop (the target resized in between, or part of it hangs off the screen): that image is not written at all, the other targets in the batch are unaffected. Judge against `fullWidth` / `fullHeight` in an earlier result, then re-request |
| `capture.roi_unmeasurable` | 7 | The question needed to locate the crop gave no answer (`client_unmeasurable` = the client area could not be measured; `image_unmeasurable` = this delivered image could not be tied to a region of the screen). Different next step from "does not fit": retry with `--capture wgc`, or drop `--client-area` for an `--roi` that lies inside the image |
| `match.monitor_unknown_id` | 4 | The `--monitor device:…` / `id:…` identifier is not on the desktop right now (unplugged, disabled, or a stale value from an earlier `--screens`). Run `--screens` again; **the tool does not fall back to the primary monitor** |
| `match.monitor_ambiguous_id` | 5 | One identifier matches several monitors; every candidate is in `hint`. Pick a more specific identifier (the cross-session `id:` one) or a number - the tool never chooses for you |
| `match.monitor_id_unverifiable` | 7 | The screen identity could not be read at all (QueryDisplayConfig gave no answer), so naming a monitor by identifier is impossible. Check this machine's display topology - switching `--capture` is not the next step, since no pixel was read and no channel was chosen |
| `cli.monitor_selector_empty` / `cli.monitor_selector_kind` | 1 | `--monitor`'s identifier form is malformed: nothing after the colon, or a prefix other than `device:` / `id:` |
| `match.timeout` | 7 | The `--timeout-ms` budget was spent before/while evaluating conditions (`stage=match`; regex work or a hung window's title fetch) - raise `--timeout-ms` or simplify the regex |
| `cli.missing_output` | - | Retired: it used to replace every failure that happened while `--out` was omitted. Never produced now - read the real code (`match.no_window` / `capture.access_denied` / `io.write_failed` / …) instead |
| `cli.invalid_format` | 1 | `--format` accepts only png / jpg / jpeg / bmp / tiff / gif (no webp, no ico, no `auto`) |
| `cli.unknown_capture_method` / `cli.unknown_language` | 1 | Bad value, caught while parsing - it never degrades to the default |
| `cli.invalid_regex` | 1 | `--title-regex` too complex for the engine (`stage=match`, message says backtracking complexity) - raising `--timeout-ms` does not help; rewrite the pattern or use `--title-contains` |
| `cli.monitor_conflict` | 1 | `--monitor all` plus window match conditions; use a single monitor number to filter instead |
| `cli.query_conflict` | 1 | An environment query (`--capabilities` / `--diagnostics`) was combined with capture intent - window conditions, `--monitor`, `--capture`, `--out` or a positional path, `--yes`, `--dry-run`, either deadline, or the two queries together. `value` lists every offending name at once. Nothing was captured and no file written; run the query alone, then the capture separately. Queries accept only `--lang`, `-v`, `-q` |
| `cli.window_query_conflict` | 1 | A window query (`--list` / `--inspect`) was combined with capture-level options (`--out` / a positional path / `--format` / `--quality` / `--capture` / `--dry-run` / `--consent-timeout-ms`, or a selection policy that has no target to select for - `--all` with `--inspect`), or with an environment query. A window query accepts window conditions, `--monitor`, `--timeout-ms`, `--yes` (which changes nothing), `--offset` / `--limit`, its own values, and `--lang` / `-v` / `-q`. `value` lists all offenders; a positional is reported as `--out` and the path itself is never echoed |
| `note.window_query_stale` | - | Not an error: appended to every `--list` / `--inspect` result, saying the snapshot expires and its `hwnd` / `pid` / class are not a long-lived credential. Re-run the query rather than caching a handle |
| `capture.failed` | 7 | Target protected, gone, or unsupported by the OS; retry once with `--capture auto`, and if it fails again nothing is reachable |
| `capture.worker_failed` | 7 | This tool's own hidden helper could not run (spawn blocked, pipe broke, message did not match the protocol, task invalid) - `cap.worker.*` wording, the helper's last exit code in `hint`; its exit codes are not part of the contract. Check the execution environment (policy, antivirus, permissions), not the target window |
| `capture.timeout` | 7 | `--timeout-ms` budget exhausted during capture/encode (`stage=capture`); with `backend=printwindow` / `dwm` the target's UI thread is likely stuck - same-backend retry may time out again, prefer `--capture wgc` or raise the budget |
| `capture.access_denied` | 6 | Someone answered "No" or closed the dialog (`stage=consent`, `value` names the path) - stop, do not retry, do not switch channel, never answer it for the user; or the target window itself is protected |
| `capture.consent_unavailable` | 6 | The dialog could not be shown at all (service session, scheduled task, lock screen). Nobody refused - run it in an interactive session instead of asking a second time |
| `capture.consent_timeout` | 6 | Nobody answered the dialog within `--consent-timeout-ms` - treated as a refusal, never as consent; stop like after any "No" (and note: this wait does not consume `--timeout-ms`) |
| `capture.consent_stale` | 7 | The target moved or resized after desktop consent was granted, so nothing was sampled. Re-select the target and let the human confirm again |
| `capture.target_gone` | 7 | The selected window was destroyed before any pixel was read (`stage=capture`, no `backend`). Enumerate the windows again - re-trying another channel cannot reach a window that is gone |
| `capture.target_changed` | 7 | That handle value now belongs to another object (different owning process, recycled PID, different window class) or no longer satisfies the condition it was selected by (its title changed, it moved off the `--monitor` screen). The ASCII reason is inside `message`. **Re-select the target**: consent given to the old object is not transferred, and this tool will not grab a look-alike instead - switching channel or relaxing the conditions are both wrong moves |
| `capture.target_unverifiable` | 7 | One identity question could not be answered (process information unreadable, the condition re-evaluation did not finish), and no answer is never counted as a pass. Check the execution environment or raise `--timeout-ms`, then enumerate and select again |
| `capture.frame_invalid` | 7 | The frame that came back does not describe its own memory correctly (zero size, a side over 16384 px, a row pitch that cannot hold one row, a buffer shorter than pitch x height, more than 1 GiB). Detected before allocating anything; a target-side problem on that channel - re-check the size, or `--capture wgc` |
| `capture.monitor_changed` | 7 | That monitor left this machine's desktop during the request, or its picture (resolution / rotation / position) changed after the confirmation - so nothing was sampled and **no other monitor was substituted**. Re-enumerate with `--screens` and confirm again |
| `capture.monitor_unverifiable` | 7 | The pre-capture identity re-check got no answer this time while the target had been named by a cross-session identifier. It does **not** fall back to matching by device name (that name may already belong to another panel), so nothing is captured. Re-run `--screens`, then confirm again |
| `capture.cursor_unsupported` | 1 | The requested cursor state cannot be delivered by the channel you asked for (`--cursor include` with `printwindow` / `dwm` / `bitblt` / `duplication`, or with a screen target on a route that has no cursor switch). Nothing was captured, no dialog shown, and **no channel was substituted** - use `--capture wgc` for `include`, ask for `exclude`, or drop the option |
| `capture.cursor_unverifiable` | 7 | An explicit `include` / `exclude` request could not be confirmed on the `wgc` session (interface unavailable, set failed, or the value read back is the opposite one - the ASCII reason and the actual value are in `message`). Raised before `StartCapture`, so nothing contradicts the request; retry, raise `--timeout-ms`, or report the three parts of that message |
| `env.cursor_unsupported` | 7 | This machine cannot provide the requested cursor state at all (the cursor switch needs build 19041 and the machine is older, or nothing survived the cursor filter). Raised before enumerating targets, before any dialog, before any pixel; check the `cursor` section of `--capabilities` instead of retrying the same target |
| `note.cursor_channel_skipped` | - | Not an error: a route in the `auto` chain cannot honour this cursor request and was dropped from it (`backend` names it, `message` carries the ASCII reason such as `screen_dc_has_no_pointer` or `os_below_min_build:19041`). Distinct from `note.channel_unavailable`, which means the OS build blocked the channel itself. Read `images[].source` plus these notes to know what actually ran |
| `io.write_failed` | 8 | Output directory does not exist, the file name is invalid, or the finished temporary file could not be renamed onto the target (it is held open elsewhere, the target name is a directory, …) |
| `io.file_exists` | 8 | `--no-overwrite` (or `=true`) was given and the target already exists; decided by the final rename, not by a pre-check |
| `io.output_collision` | 8 | Two targets expand to the same output name; the whole batch is refused before any frame is taken, so nothing is written - put `%i` / `%h` into `--out` |
| `io.timeout` | 8 | The budget ran out at the write/stdout stage (`stage=write` / `stdout`); the finished image is not written - keep enough budget for the encode+write tail |

## Resources

- `references/cli-contract.md` - every option and value (including `--timeout-ms` /
  `--consent-timeout-ms` and the hidden helper process behind them), the full JSON field tables (window image /
  screen image), the read-only query documents (`--capabilities` / `--diagnostics`: field list, the
  `available` / `unavailable` / `unverified` rule, `unknown` handling, the `caveats` tokens and the privacy
  statement; `--list` / `--inspect`: the `windowquery` / `windowinspect` field tables, pagination and
  `nextOffset`, per-field `readable` / `denied` / `failed`, the expiring-snapshot rule and the `--yes`
  non-effect), the complete diagnostic-code list, the output-name placeholders
  (`%i` `%h` `%p` `%n` `%d` `%t`; `%n` is the window title for a window target and the device name such
  as `DISPLAY1` for a screen target), plus the shell-specific traps measured under PowerShell and Git
  Bash.
- `ECAPTURE.EXE` - the tool itself, in this same directory.
