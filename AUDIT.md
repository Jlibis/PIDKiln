# PIDKiln code audit

Audited: `master` @ `072fe1b` (v1.5, 2024-12-18). The fork has no changes of its own: it is 0 commits
ahead of and 0 behind `Saur0o0n/PIDKiln`.
Method: I read all firmware sources (~3.9k lines of C++), the web UI, the config and the OTA installer.
**Nothing was compiled or run on hardware.** No ESP32 toolchain was available, so every finding comes
from reading the code. The line numbers refer to that commit.

## Status (updated 2026-11-25)

The audit text below is the original snapshot. Fixes so far (each branch builds on the previous one;
all compile in CI, none is hardware-tested yet):

| Branch | Fixes |
|--------|-------|
| `build/pinned-toolchain-ci` | Reproducible build and CI (section 5 "no build manifest, no CI") |
| `modernize/littlefs-deprecated-apis` | SPIFFS → LittleFS, `IRAM_ATTR`, efuse API (section 6); rejected uploads now really deleted |
| `fix/web-auth-and-program-safety` | S1, S2, S3, S4, S5 for POSTs (same-origin check; `GET /load.html` can still be triggered cross-site, but now only loads a program while none is active), S6 (escaping/validation), M6, F0, F7, F8, F9 |
| `fix/program-parser-and-runtime-guards` | F4 (0-minute step jumps the set point; NaN/inf aborts), M1, M2, M3, M4, M5, and the empty-program part of M12 |
| `fix/thermal-runaway` | F1 (heating stall via `Thermal_Runaway`, stuck-SSR detection; README/conf now describe what is implemented) |
| `fix/watchdogs-and-safety-cadence` | F2, F3, F5, F6 error codes (the grace counter is unchanged: it tolerates sporadic errors by design, and the last good temperature is used for at most `MAX31855_Error_Grace_Count` reads) |
| `fix/correctness-m7-m12` | M7, M8, M9, M10, M11 (handlers registered once; static pages use an auth middleware that follows login changes), M12 |
| `fix/concurrency` | C1 (State_mutex / FS_mutex, lock order State→FS; PID+SSR switching under the lock), C2 (only Input_Loop draws), C3, C4 (index generation locked, `template_str` local), syslog pointer |
| `fix/security-leftovers` | S7 (passwords not sent back, factory-password warning, configurable AP name/password; plain HTTP documented), S8 (documented as outdated and unsafe - not rewritten), S9 |

Still open: plain HTTP (no TLS) and the rest of sections 5–6. Also noticed:
`DIRECTORY.filesize` is `uint16_t`, so log files over 64 KiB show a wrong size.

## Verdict

It is a capable hobby project with a good feature set: LCD and web UI, programs, logging, syslog, OTA.
The code is readable and the commits are small and steady. But it does not meet the bar for something
that switches a kiln heater unattended:

- **The web API can be used without authentication** (firmware flashing, program upload, a stack
  overflow) by anyone on the same network.
- **One unauthenticated GET during a firing can leave the heater stuck on**, with safety checks
  stopped (F0).
- **Two safety features are documented but not implemented** (thermal runaway, SSR failure detection).
  At the same time **every watchdog is disabled**.
- **Several memory-safety bugs** are reachable from uploaded program files.
- **No synchronisation** between the four tasks that share all state.
- No tests, no CI, no reproducible build, and a manual library patch is required.

Rough maintainability score: **4/10**. Modernity: **5/10**. The ESP32 core 3.x port is current, but the
filesystem, the JS stack and the build tooling are 2019-era.

---

## 1. Critical: security (network attacker on the LAN or AP)

| # | Finding | Where |
|---|---------|-------|
| S1 | **Unauthenticated firmware flashing.** `_webAuth()` runs only in the request-complete lambda. The upload body callback `handleDoUpdate()` runs *first* and unconditionally calls `Update.begin/write/end` and then `Restart_ESP()`. Anyone who can reach port 80 can install arbitrary firmware. | `PIDKiln_http.ino:826-830`, `685-716` |
| S2 | **Unauthenticated program upload.** Same pattern on `/upload`: `handleUpload` writes to SPIFFS before auth is checked. | `PIDKiln_http.ino:791-794` |
| S3 | **Unauthenticated stack buffer overflow.** `/load.html` has no auth and does `strcpy(prname[30], <query param>)`. A long `prog_name` overwrites the stack. This is remote memory corruption on the device that drives the heater. | `PIDKiln_http.ino:524-534`, route `812` |
| S4 | **Unauthenticated program control through `/load.html`.** It loads any program as the run program (it doesn't start it, but it replaces what is loaded). `/delete.html` GET and `/screenshot.html` are unauthenticated too. | `PIDKiln_http.ino:808-814` |
| S5 | **CSRF can start the kiln.** HTTP Basic auth plus no CSRF token means any web page the owner opens can POST `prog_start` to `/index.html`. The browser resends the cached credentials. | `handleIndexPost`, `PIDKiln_http.ino:591` |
| S6 | **XSS.** `/delete.html` reflects `prog_name` raw (`:512`). Program descriptions (from uploaded files) go into `/programs/index.html` unescaped (`:229`). Preference values are templated unescaped into `preferences.html` (`:32-76`). | `PIDKiln_http.ino` |
| S7 | **Credentials.** Defaults are `admin/hotashell`, and the AP password is hard-coded as `hotashell`. Passwords are stored in plaintext, sent back in the preferences HTML, and served raw at `/etc/pidkiln.conf`. Everything is plain HTTP. | `PIDKiln_prefs.ino`, `PIDKiln_net.ino:100`, `PIDKiln_http.ino:836` |
| S8 | **Unsigned OTA installer.** It downloads firmware over plain `http://` from a personal domain, with no signature or hash check. | `OTA_Install/OTA_Install.ino` |
| S9 | **jQuery 3.4.1** has known XSS CVEs (CVE-2020-11022/11023, fixed in 3.5.0). The `chartjs-plugin-datasource` CDN URL has no version pin. | `PIDKiln.h:319-321`, `data/js/` |

## 2. Critical/high: functional safety

| # | Finding | Where |
|---|---------|-------|
| F0 | **Loading a program during a run can leave the heater latched on with no safety checks.** `Load_program_to_run()` (and the LCD Quick program path) frees `Program_run` and sets `Program_run_state=PR_READY` without calling `END_Program()`. `Program_Loop` only drives the SSR and runs `SAFETY_Check()` in RUNNING/PAUSED/THRESHOLD, so if the SSR was on at that moment it **stays on**, the EMR stays closed, and nothing checks the temperature any more. It can be triggered by an unauthenticated `GET /load.html?prog_name=<any>` (S4), or from the LCD "Load" / "Quick program" items. | `PIDKiln_program.ino:226-238`, `PIDKiln_http.ino:535-537`, `PIDKiln_LCD.ino:607,719-729` |
| F1 | **Thermal runaway protection does not exist.** `pidkiln.conf` describes it in detail and README lists "temperature run out protection" and "SSR failure" as built-in safety features. `PRF_THERMAL_RUN` is never read anywhere. Its code default (130) doesn't match the documented default (0), and it isn't in the web form. A dead element, a heater stuck on through a welded SSR, or a thermocouple pulled out of the kiln is caught only by the absolute MAX temperature. | `PIDKiln_prefs.ino:244-246`, `data/etc/pidkiln.conf:90-97` |
| F2 | **All watchdogs are disabled** (`rtc_wdt_disable`, `disableCore0WDT/1WDT`). If any task hangs while the SSR is on, the heater stays on. The likely reason they were disabled is F3. | `PIDKiln.ino:110-118` |
| F3 | **`Input_Loop` busy-spins.** It calls `yield()`, which does not let the lower-priority IDLE task run, so IDLE0 is starved and the task WDT would fire. That probably explains "AsyncTCP hangs → disable WDT". Fix: `vTaskDelay(pdMS_TO_TICKS(10))`, then turn the watchdogs back on. | `PIDKiln_input.ino:122-145` |
| F4 | **Divide by zero for a step with 0 minutes to reach it.** `temp_incr = Δtemp / (togo*60)`. If togo is 0, `set_temp` becomes ±inf, or NaN when Δ=0. With NaN, PID_v1's `outputSum` stays NaN forever (its clamp comparisons are false). The SSR goes off for the rest of the run while the state still says "Running", and the threshold logic is skipped too. The parser accepts `500:0:30`. | `PIDKiln_program.ino:413,418` |
| F5 | **The safety check runs only every 10 s, and only while a program runs.** `MIN_Temperature` is a `uint8` (0–255, no negatives). In a cold workshop (<10 °C) every program aborts within 10 s. | `PIDKiln_program.ino:474-486`, `533` |
| F6 | **Thermocouple faults report the wrong code.** Every fault aborts with `*_INT_ERR`. `PR_ERR_MAX31A/B_KPROBE` are never used. Between errors `kiln_temp` stays at its last value, and the grace counter only goes down by 1 per good read. | `PIDKiln_addons.ino:104-110` |
| F7 | **`Prefs_updated_hook` ignores a changed P-on-E/P-on-M setting.** *(Corrected: the first version of this audit said the mode gets reset. PID_v1's 3-argument `SetTunings()` keeps the current mode, so a changed `PID_POE` just doesn't apply until the next start.)* New tunings also aren't applied in the `PR_THRESHOLD` state. | `PIDKiln_prefs.ino:123` |
| F8 | **`START_Program()` while running restarts the program** from step 0 at the current temperature. It only rejects `NONE`/`PAUSED`. | `PIDKiln_program.ino:443` |
| F9 | *(Found later.)* **Aborts are ignored while waiting for the temperature threshold.** `ABORT_Program()` only acts in RUNNING/PAUSED, so in `PR_THRESHOLD` the safety check's over-temperature and housing aborts, thermocouple-failure aborts and the user's Abort button all do nothing, while the PID keeps driving the heater. The web "End" button has the same gap. Only reachable when `PID_Temp_Threshold` is enabled (it's off by default). | `PIDKiln_program.ino:314`, `PIDKiln_http.ino:619` |

## 3. High: memory safety and correctness

| # | Finding | Where |
|---|---------|-------|
| M1 | **Program parser overflows.** `char p_line[15]` receives lines of up to 15 characters plus the terminator. More than two `:` makes `uint8_t pos` wrap to 255, so `prg[255]` is written. There is no `Program_size < MAX_PRG_LENGTH` check, so a file with more than 40 steps overruns the global `Program[40]`. Values above 65535 wrap and get past the MAX temperature check. | `PIDKiln_program.ino:40-59,112` |
| M2 | **LCD stack overflow from the program description.** `char msg[125]; strcpy(msg, Program_desc)`. The description is the first comment line and can be up to the 10 KiB file limit. | `PIDKiln_LCD.ino:647` |
| M3 | **Directory loader overflow.** `strcpy` copies names of up to 31 characters into `filename[21]`. The loop bound is `<=count` instead of `<count`. It returns early without `dir.close()`. Files placed on SPIFFS by other means (data upload) can trigger this. | `PIDKiln_program.ino:150-157`, `PIDKiln_logs.ino:149-160` |
| M4 | **Index −1.** `Find_selected_program()` returns −1 when there are no programs. `Erase_program_file()` and `LCD_Display_program_summary()` (where `uint16_t sel` becomes 65535) then index `Programs_DIR` with it. Pressing the button on an empty program list reads out of bounds. | `PIDKiln_program.ino:287`, `PIDKiln_LCD.ino:617` |
| M5 | **`dbgLog()` reuses a consumed `va_list`** in its second `vsnprintf` without `va_copy`. That is undefined behaviour on the hottest code path (any message longer than its format string). | `PIDKiln_logs.ino:211-216` |
| M6 | **Upload filename off by one:** `char tmp_filename[20]` holds 20 characters plus the terminator. | `PIDKiln_http.ino:417-418` |
| M7 | **The GMT offset fix from 1.4 doesn't work.** The pref became `INT32`, but `configTime()` and the web form still read `.int16`, so offsets above 9.1 h (e.g. +10 h = 36000 s) wrap negative. | `PIDKiln_net.ino:142`, `PIDKiln_http.ino:49` |
| M8 | **`Setup_start_date`.** `strcpy` copies the user pref into `msg[20]`. `struct tm` is uninitialised (garbage `tm_isdst`), so `mktime` gives unpredictable results. | `PIDKiln_net.ino:29-60` |
| M9 | **Wrong data in the log footer.** It prints `uint16_t Energy_Wattage` with `%.1f` (UB/garbage; it should be `Energy_Usage`). The "CSV filename" line prints a date, because the buffer was reused. | `PIDKiln_logs.ino:103,47` |
| M10 | **Union type punning works by accident.** `Prefs[PRF_HTTP_JS_LOCAL].value.str` is tested where it's a `uint8` pref. The LCD prefs screen prints `.str` with `%d` and reads `Prefs[PRF_end]` (one past the end). | `PIDKiln_http.ino:752`, `PIDKiln_LCD.ino:908-912` |
| M11 | **Web server re-initialised on WiFi reconnect.** `Restart_WiFi()` and the LCD "Reconnect WiFi" item call `SETUP_WebServer()` again, which registers every handler a second time. Static handlers keep the *old* credentials after a password change until reboot. | `PIDKiln_net.ino:170`, `PIDKiln_LCD.ino:972` |
| M12 | **Smaller issues.** `LCD_display_mainv1` has `SCREEN_W-4/chw` (operator precedence) and `strcmp` where `strcpy` was meant. `mainv2` uses `scx`/`scy` uninitialised when the total time is 0. `Initialize_program_to_run()` returns early and leaks name/desc when the size is 0. The upload error path sends two responses. | `PIDKiln_LCD.ino:299-302,228`, `PIDKiln_program.ino:206`, `PIDKiln_http.ino:451-455` |

## 4. High: concurrency

Four tasks (program loop, input, AsyncTCP, power meter) share all state with no mutex, queue or
`volatile` beyond the encoder ISR.

- **C1.** HTTP and LCD handlers `free`/`realloc` `Program_run`, `Program_run_name` and `Prefs[].str`
  while `Program_Loop` and the JSON/chart handlers read them. That is a use-after-free risk.
  `syslog.server()` stores the `Prefs` string pointer, which is freed when that pref is saved.
- **C2.** `u8g2` is drawn from both `Program_Loop` (`mainv1/mainv2`) and `Input_Loop`, which garbles
  frames. `Program_Loop` also calls `LCD_display_mainv2()` whenever `LCD_Main==MAIN_VIEW2`, *without
  checking `LCD_State`*. That pulls the user out of menus every 10 s during a run.
  (`PIDKiln_program.ino:537`)
- **C3.** `Read_Energy_INPUT()` is called from `Power_Loop` *and* from `Program_Loop`
  (`PIDKiln_program.ino:523`). It shares static state, so energy gets counted twice.
- **C4.** The global `template_str` and SPIFFS index regeneration are used from several tasks at once.

## 5. Maintainability

- **Architecture.** It is a single translation unit built from `.ino` files, with about 100 mutable
  globals declared in a header, and modules reach into each other's state directly. There are no
  interfaces, so nothing can be unit-tested off-device.
- **Magic numbers and return codes.** Program error codes 1–4 are bare integers, while
  `PROGRAM_ERROR_STATE` exists for runtime errors. WiFi mode comments disagree:
  `PIDKiln.h:219` says something different from the code and `pidkiln.conf`.
- **Copy-paste.** Thermocouple A/B handlers, the 6× preference type switch, the programs/logs
  directory loaders and the index generators are all duplicated.
- **Unsafe C string handling everywhere** (`strcpy`/`sprintf` into fixed buffers), mixed with Arduino
  `String`, which fragments the heap.
- **Prefs.** Adding one means editing six places, and the enum order must match `PrefsName[]` by hand.
- **Dead code and loose ends.** `PRF_WIFI_AP_NAME/USERNAME/PASS` are declared but never initialised, so
  the AP name and password can't be configured. `/index_local.html` is routed but doesn't exist.
  `print_bits`, `printLocalTime`, `timerMux` and `content_len` are unused. There is commented-out
  test code.
- **Docs drift.** README promises safety features that aren't there (F1). `WiFi_Retry_cnt` says
  "seconds" but each try takes 1.5 s. `HTTP_Local_JS` "default is 1" in the conf but 0 in the code.
  The OTA binaries are v1.3 (2022) while the source is v1.5.
- **No `.gitignore`, no build manifest, no tests, no CI, no formatter config.** 2.4 MB of binaries
  and images are committed.

## 6. How modern is it

| Area | Status | Modern option |
|------|--------|---------------|
| ESP32 core | 3.x API (timer, task WDT config) ✔ | – |
| Filesystem | SPIFFS (deprecated by Espressif) | LittleFS (drop-in `FS` API) |
| Build | Arduino IDE + hand-installed libs + **library header patch** | PlatformIO or `arduino-cli` with `sketch.yaml`/`platformio.ini`, pinned lib versions, `-DTEMPLATE_PLACEHOLDER='~'` build flag |
| FS upload | ESP32FS plugin (IDE 1.x only) | `arduino-littlefs-upload` (IDE 2) or `pio run -t uploadfs` |
| PID | PID_v1 (2017, unmaintained) | Fine algorithmically. Consider QuickPID, or adding anti-windup and auto-tune |
| Web UI | jQuery 3.4.1, Chart.js 2.9.3 (2019), polling every 5 s | Vanilla JS + Chart.js 4, or a WebSocket/SSE push from AsyncWebServer |
| Config | Custom text file, plaintext secrets | NVS `Preferences` for secrets, JSON or the existing file for the rest |
| Ecosystem | Syslog only | MQTT / Home Assistant discovery (already on the README wish list) |
| C++ | C-style, `#define`, `byte`, `String` | `constexpr`, `enum class`, `std::array`, `snprintf`/`strlcpy`, RAII for `File` |

## 7. Suggested order of work

1. **Close the remote holes.** Check auth in the upload/body callbacks (S1, S2), add auth to every
   route, and replace `strcpy` with bounded copies in HTTP handlers (S3, M6). Change the default
   credentials on first boot, or force the user to set them.
2. **Make the safety story true.** Refuse to load or replace a program while one is active, or end it
   cleanly first. More generally, make every transition out of an active state go through
   `END_Program()` (F0). Implement thermal runaway, and "temperature rising while SSR off"
   (stuck SSR → open the EMR). Reject `togo==0` or treat it as a jump step, and add a NaN guard before
   `KilnPID.Compute()` (F1, F4). Fix the input busy loop and re-enable the task WDT for
   `Program_Loop` (F2, F3).
3. **Harden parsing.** Bound the program parser, steps, description and directory names (M1–M4). Fix
   `dbgLog` (M5).
4. **Concurrency.** Route all state-changing commands from HTTP/LCD through a FreeRTOS queue that
   `Program_Loop` consumes. Put one mutex around `u8g2` (C1–C4).
5. **Tooling.** Add `platformio.ini` (or `sketch.yaml`) with pinned libraries and the placeholder build
   flag, and a GitHub Action that compiles. Move the program parser and step calculator into plain
   C++ so host-side unit tests can run them. Migrate SPIFFS to LittleFS.
6. **Clean-up.** Write a table-driven prefs registry, deduplicate A/B sensor code, remove dead code,
   and fix the docs drift.
