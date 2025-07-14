@ECHO off
REM Console must be UTF-8 so ECAPTURE's Chinese messages stay readable.
REM Keep this file ASCII-only: non-ASCII bytes plus a codepage switch can break
REM cmd's goto/label parsing.
CHCP 65001 >nul

REM =====================================================================
REM  EvernightCapture real-machine test (own minimal test window)
REM
REM  Flow: compile tests\helper\ec_window.cs into the scratch folder
REM        -> start that helper as a window ECAPTURE can target by PID/class
REM        -> capture that window with EVERY --capture channel
REM        -> verify each PNG really has content (size + distinct colors)
REM        -> optional: capture the whole screen with --monitor (see below)
REM        -> end ONLY the helper process this script started (by PID)
REM
REM  Usage: tests\window_shot.bat ["channel list"] [noscreen]
REM  Example: tests\window_shot.bat "wgc bitblt"        (only these channels)
REM  Example: tests\window_shot.bat "" noscreen         (all channels, skip --monitor)
REM
REM  Why not fontview.exe any more: a system app may hand a new launch request
REM  to an instance that is already running, so the process this script started
REM  is not necessarily the one that owns the window. Finding the window by
REM  image name then grabs whatever the user happened to have open, and the
REM  cleanup ("taskkill /IM fontview.exe /F") closes all of those. This script
REM  therefore only ever deals with the one process it started, identified by
REM  the PID the helper writes into a pid file for us.
REM
REM  Channels below must stay in sync with kCaptureValues in src/CliOptions.cpp
REM  (tests\channels.ps1 does the same sweep with stricter occlusion checks,
REM  tests\isolation.ps1 checks the same "only touch my own objects" rule).
REM
REM  The whole-screen step is gated by a plain PAUSE, because ECAPTURE has no
REM  switch that skips its consent dialog: past the pause it blocks until you
REM  click "yes" (or "no", which is reported as REFUSED and is not a channel
REM  failure). So a run with no one at the keyboard stops there - pause itself
REM  returns immediately when stdin is redirected, it is the dialog that waits.
REM  tests\screen.ps1 only answers that dialog when you pass -SimulateConsent
REM  on a desktop that is dedicated to testing.
REM
REM  Exit: 0 all channels ok / 1 environment or window problem /
REM        7 at least one channel failed (capture or empty frame)
REM =====================================================================
SETLOCAL EnableExtensions

SET "CHANNELS=wgc dwm printwindow bitblt duplication auto"
IF not "%~1"=="" SET "CHANNELS=%~1"

SET "ECAPTURE=%~dp0..\build\ecapture.exe"
SET "HELPER_SRC=%~dp0helper\ec_window.cs"
SET "CSC=%SYSTEMROOT%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
IF not exist "%CSC%" SET "CSC=%SYSTEMROOT%\Microsoft.NET\Framework64\v2.0.50727\csc.exe"

REM A fresh scratch folder per run: an image left over from an earlier run looks
REM exactly like a fresh success, and reusing a folder is how that gets missed.
SET "SHOTDIR=%TEMP%\ecapture-tests\run-shot-cmd-%RANDOM%%RANDOM%"
SET "HELPER=%SHOTDIR%\ecwindow.exe"
SET "PIDFILE=%SHOTDIR%\ecwindow.pid"
SET "TCLASS=ec-cmd-shot"
SET "PROBE=%SHOTDIR%\probe.png"

SET "RC=0"
SET "BAD=0"
SET "WTRY=0"
SET "DONE=0"
SET "HPID="

IF not exist "%ECAPTURE%"  (ECHO [FAIL] no build\ecapture.exe, run .\build.ps1 & SET "RC=1" & GOTO :report)
IF not exist "%HELPER_SRC%" (ECHO [FAIL] no "%HELPER_SRC%" & SET "RC=1" & GOTO :report)
IF not exist "%CSC%"       (ECHO [FAIL] no csc.exe under "%SYSTEMROOT%\Microsoft.NET\Framework64" & SET "RC=1" & GOTO :report)

ECHO [1/6] compile the test window helper
MKDIR "%SHOTDIR%" >nul 2>&1
IF not exist "%SHOTDIR%" (ECHO [FAIL] cannot create "%SHOTDIR%" & SET "RC=1" & GOTO :report)
"%CSC%" /nologo /target:exe /optimize+ /r:System.Drawing.dll /out:"%HELPER%" "%HELPER_SRC%"
IF not exist "%HELPER%" (ECHO [FAIL] helper did not compile & SET "RC=1" & GOTO :kill)

ECHO [2/6] start ecwindow.exe and wait for its window
START "" /min "%HELPER%" --mode window --class %TCLASS% --title "cmd screenshot target" --rect 200,200,680,540 --seed 9 --pid-file "%PIDFILE%" --max-life 600

:wait_pid
SET /a WTRY+=1
IF exist "%PIDFILE%" GOTO :have_pid
IF %WTRY% geq 25 (ECHO [FAIL] helper never wrote its pid file & SET "RC=1" & GOTO :kill)
ping -n 2 127.0.0.1 >nul
GOTO :wait_pid

:have_pid
SET /p HPID=<"%PIDFILE%"
IF not defined HPID (ECHO [FAIL] empty pid file & SET "RC=1" & GOTO :kill)
ECHO       helper pid %HPID%

REM Target by the PID this script started plus this run's window class: no other
REM process on the machine can match, and no pre-existing same-named app is ever
REM picked up as the target.
SET "TARG=--pid %HPID% --class %TCLASS%"

:wait_win
SET /a WTRY+=1
"%ECAPTURE%" %TARG% --dry-run --quiet --out "%PROBE%" >nul 2>&1
IF "%ERRORLEVEL%"=="0" GOTO :win_up
IF %WTRY% geq 50 (ECHO [FAIL] no %TCLASS% window after %WTRY% tries & SET "RC=1" & GOTO :kill)
ping -n 2 127.0.0.1 >nul
GOTO :wait_win

:win_up
ECHO       target window up, waited %WTRY% polls

ECHO [3/6] capture with every channel
FOR %%c in (%CHANNELS%) do CALL :shoot %%c

IF "%BAD%"=="0" (ECHO       all %DONE% channels ok) else (ECHO       %BAD% of %DONE% channels failed)

ECHO [4/6] whole screen (--monitor)
IF /i not "%~2"=="noscreen" GOTO :do_screen
ECHO       skipped on purpose: this step waits for a human at ECAPTURE's consent dialog.
ECHO       Leave arg 2 empty to get it back.
GOTO :open_folder

:do_screen
CALL :shoot_screen

:open_folder
ECHO [5/6] open the screenshot folder
START "" explorer "%SHOTDIR%"

:kill
ECHO [6/6] end only the helper this script started
IF not defined HPID (
    ECHO       no pid file was written, nothing to end here
    ECHO       the helper exits on its own after --max-life 600, so no window is left behind
    GOTO :report
)
TASKKILL /PID %HPID% /F >nul 2>&1
SET "TKRC=%ERRORLEVEL%"
REM 0 = killed, 128 = it had already exited; both are fine, anything else is not
IF "%TKRC%"=="0" GOTO :check_gone
IF "%TKRC%"=="128" (ECHO       pid %HPID% had already exited & GOTO :report)
ECHO [FAIL] taskkill rc=%TKRC% for pid %HPID%
SET "RC=1"
GOTO :report

:check_gone
powershell -NoProfile -Command "exit ([int][bool](Get-Process -Id %HPID% -ErrorAction SilentlyContinue))"
IF "%ERRORLEVEL%"=="1" (ECHO [FAIL] pid %HPID% is still running & SET "RC=1" & GOTO :report)
ECHO       stopped pid %HPID%

:report
ECHO.
IF not "%BAD%"=="0" SET "RC=7"
IF "%RC%"=="0" (ECHO RESULT: PASS) else (ECHO RESULT: FAIL rc=%RC%)
ECHO IMAGES: %SHOTDIR%\cmdshot_*.png
REM The scratch folder is unique to this run, so it can never be confused with an
REM earlier run's output. It is left in place on purpose: %TEMP% is the right place
REM to throw it away from.
exit /b %RC%

REM ---------------------------------------------------------------------
REM  :shoot <channel> - capture once, then reject blank frames.
REM  A frame that is one flat color must never count as success: an error
REM  dialog, an unpainted window and a failed GPU copy all look like that.
REM ---------------------------------------------------------------------
:shoot
SET "CH=%~1"
SET "SHOT=%SHOTDIR%\cmdshot_%CH%.png"
SET "ATTEMPT=0"
IF exist "%SHOT%" DEL "%SHOT%"

:s_retry
SET /a ATTEMPT+=1
REM  --yes 只免掉"只取所选窗口画面"那几条通道的确认框（wgc / dwm 缩略图 / printwindow）。
REM  bitblt 与 duplication 取的是屏幕像素，仍然一定会弹框 - 那一步请人自己点"是"。
"%ECAPTURE%" %TARG% --capture %CH% --yes --out "%SHOT%" >nul 2>&1
SET "ERC=%ERRORLEVEL%"
IF "%ERC%"=="0" GOTO :s_file
IF %ATTEMPT% geq 2 GOTO :s_err
REM duplication can miss the first frame; one retry before calling it a failure
ping -n 3 127.0.0.1 >nul
GOTO :s_retry

:s_err
CALL :pad %CH%
ECHO %PCH%  FAIL  rc=%ERC%
SET /a BAD+=1
GOTO :s_done

:s_file
CALL :pad %CH%
IF not exist "%SHOT%" (ECHO %PCH%  FAIL  no image written & SET /a BAD+=1 & GOTO :s_done)
FOR %%F in ("%SHOT%") do SET "SIZE=%%~zF"
IF %SIZE% lss 1024 (ECHO %PCH%  FAIL  only %SIZE% bytes & SET /a BAD+=1 & GOTO :s_done)

powershell -NoProfile -Command "Add-Type -AssemblyName System.Drawing; $i=[Drawing.Bitmap]::new('%SHOT%'); try { $s=@{}; for($y=0; $y -lt $i.Height; $y+=7){ for($x=0; $x -lt $i.Width; $x+=7){ $p=$i.GetPixel($x,$y); $s[(($p.R -shl 16) -bor ($p.G -shl 8) -bor $p.B)]=1 } }; $w=$i.Width; $h=$i.Height } finally { $i.Dispose() }; Write-Host ('%PCH%  ' + $w + 'x' + $h + ', ' + $s.Count + ' colors, %SIZE% bytes'); if ($w -lt 200 -or $h -lt 150 -or $s.Count -lt 12) { exit 1 }"
IF not "%ERRORLEVEL%"=="0" (ECHO %PCH%  FAIL  flat or empty frame & SET /a BAD+=1)

:s_done
SET /a DONE+=1
GOTO :eof

REM ---------------------------------------------------------------------
REM  :shoot_screen - one whole-screen shot through --monitor, only after a
REM  human yes here. ECAPTURE has no option to skip its own consent dialog,
REM  so this call blocks until that dialog is answered on screen. The default
REM  channel (wgc) is used; tests\screen.ps1 sweeps every screen-capable
REM  channel and answers that dialog itself only when -SimulateConsent is given.
REM ---------------------------------------------------------------------
:shoot_screen
SET "SSHOT=%SHOTDIR%\cmdshot_screen.png"
IF exist "%SSHOT%" DEL "%SSHOT%"
ECHO       whole-screen shot next: ECAPTURE opens a consent dialog - click yes to capture,
ECHO       no to skip it (answering no is NOT counted as a failure).
PAUSE
"%ECAPTURE%" --monitor 1 --out "%SSHOT%" >nul 2>&1
SET "ERC=%ERRORLEVEL%"
IF "%ERC%"=="6" (ECHO screen        REFUSED  consent dialog answered no - not a channel failure & GOTO :eof)
IF not "%ERC%"=="0" (ECHO screen        FAIL  rc=%ERC% & SET /a BAD+=1 & GOTO :eof)
IF not exist "%SSHOT%" (ECHO screen        FAIL  no image written & SET /a BAD+=1 & GOTO :eof)
FOR %%F in ("%SSHOT%") do SET "SIZE=%%~zF"
IF %SIZE% lss 1024 (ECHO screen        FAIL  only %SIZE% bytes & SET /a BAD+=1 & GOTO :eof)

powershell -NoProfile -Command "Add-Type -AssemblyName System.Drawing; $i=[Drawing.Bitmap]::new('%SSHOT%'); try { $s=@{}; for($y=0; $y -lt $i.Height; $y+=11){ for($x=0; $x -lt $i.Width; $x+=11){ $p=$i.GetPixel($x,$y); $s[(($p.R -shl 16) -bor ($p.G -shl 8) -bor $p.B)]=1 } }; $w=$i.Width; $h=$i.Height } finally { $i.Dispose() }; Write-Host ('screen          ' + $w + 'x' + $h + ', ' + $s.Count + ' colors, %SIZE% bytes'); if ($s.Count -lt 12) { exit 1 }"
IF not "%ERRORLEVEL%"=="0" (ECHO screen        FAIL  flat or empty frame & SET /a BAD+=1)
GOTO :eof

REM ---- pad the channel name to a fixed column so the table lines up ----
:pad
SET "PCH=%~1             "
SET "PCH=%PCH:~0,13%"
GOTO :eof
