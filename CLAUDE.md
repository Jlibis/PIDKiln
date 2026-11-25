# CLAUDE.md

PIDKiln is ESP32 firmware (Arduino framework) for a PID kiln controller. It switches a mains-powered
heater through solid-state and mechanical relays, so **a bug here can start a fire**. Treat every change
to control flow, relay handling, temperature reading or the web API as safety-relevant.

This repo is a fork of `Saur0o0n/PIDKiln` (remote `upstream`). Keep fork-specific changes in focused
commits so they can still be compared with or sent upstream.

## Layout

| Path | What it is |
|------|------------|
| `PIDKiln.ino` | Entry point: `setup()`, config switches (`DEBUG`, `MALLOC`), shared helpers |
| `PIDKiln.h` | Pin map, global state, enums, preference table. Included once, by `PIDKiln.ino` |
| `PIDKiln_<module>.ino` | One subsystem per file: `program` (PID + program runner), `addons` (relays, thermocouples, power meter, alarm), `http`, `LCD`, `input`, `net`, `prefs`, `logs` |
| `src/` | Plain C++ headers without Arduino dependencies (program step parser, ramp math) - host-testable |
| `tests/` | Host unit tests for `src/` (`g++`, run by CI) |
| `libraries/` | Git submodules: the MAX31855 fork and Syslog |
| `data/` | LittleFS image: web UI, `etc/pidkiln.conf` (default config), sample programs |
| `OTA_Install/` | Separate installer sketch, plus prebuilt binaries (these can lag behind the source) |
| `Documentation/` | Wiring diagrams, component photos, PID tuning results |

The Arduino build joins all `.ino` files into one translation unit (main sketch first, then the rest
alphabetically) and generates prototypes for them. All modules share one global namespace. There are
no headers per module and no `static` boundaries, so search all `.ino` files before you rename or
remove a symbol.

## Build and verify

- Reproducible build: `sketch.yaml` pins the ESP32 core and every library. Run
  `git submodule update --init`, then
  `arduino-cli compile --profile wrover --build-property "compiler.cpp.extra_flags=-DTEMPLATE_PLACEHOLDER='~'" .`
  (arduino-cli 1.3 or newer). CI (`.github/workflows/build.yml`) runs the same command. Bump pinned
  versions one at a time and on purpose.
- The app partition is about 92% full (1.21 MB of 1.31 MB). Watch the size the compiler reports
  when adding code or libraries.
- Toolchain: Arduino IDE 2.x or `arduino-cli` with the **ESP32 Arduino core 3.x** (v1.5+ needs it).
- Libraries: ESPAsyncWebServer + AsyncTCP (the ESP32Async/mathieucarbou forks, API 3.x), PID_v1
  (br3ttb), U8g2, arcao Syslog, `Saur0o0n/MAX31855` (a fork that adds HSPI support, not the upstream
  library), and EmonLib only if `ENERGY_MON_PIN` is defined.
- ESPAsyncWebServer must use `~` as `TEMPLATE_PLACEHOLDER`, because the HTML/JS templates in `data/`
  use `~VAR~`. Set it with a build flag (`-DTEMPLATE_PLACEHOLDER='~'`), or edit the library header as
  the README says.
- Tests: only the plain C++ in `src/` has unit tests (`tests/`, run with
  `g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/test_program_step.cpp -o t && ./t`; CI runs them).
  Logic that can live without Arduino types belongs in `src/` with a test. Everything else is only
  compiled by CI. If you cannot compile in the current environment, **say so plainly** (or push and
  check CI). Never say a change "works" or "compiles" unless you ran it.
- Hardware behaviour (relays, thermocouples, LCD, encoder) can only be checked on a real device. When
  a change needs that, end with a short manual test checklist for the user.

## Architecture

FreeRTOS tasks, created in `setup()`. `loop()` deletes itself.

- `Program_Loop` (core 0, prio 1): a 1 s hardware-timer tick drives temperature reads, the safety
  checks, program stepping and logging. The PID compute and SSR time-proportioning run every 10 ms.
  It is subscribed to the task watchdog (15 s): if it hangs, the ESP32 restarts with relays off.
- `Input_Loop` (core 0, prio 1): rotary encoder and button. **The only task that draws on the LCD**;
  `Program_Loop` asks for main-view redraws through `LCD_refresh_v1/v2`.
- AsyncTCP task: every HTTP handler runs here.
- `Power_Loop` (core 1): only when the power meter is enabled. The only reader of the energy meter.

State is shared through globals (`kiln_temp`, `set_temp`, `Program_run*`, `Prefs[]`, ...), guarded
by two recursive mutexes - see "Locking between tasks" in `PIDKiln.h`:
- `State_mutex`: the running program and its state changes, PID/relay switching, `Prefs[]`.
  `FS_mutex`: the program edit buffer, directory lists, index pages, log files.
- Take them with a scope guard (`Recursive_lock state_lock(State_mutex);`), **State before FS**, and
  never take `State_mutex` while holding `FS_mutex`.
- Hold `State_mutex` briefly and never across `delay()`: `Program_Loop` needs it every 10 ms to switch
  the SSR. Check and change state under one lock (e.g. "not active, so replace the program").
- Never block `Program_Loop` (no `delay()`, network or slow filesystem calls in its path). Never call
  `delay()` inside AsyncWebServer callbacks.
- If a task polls, it must give way to the scheduler with `vTaskDelay()`. `yield()` does not let
  lower-priority tasks run.

## Safety rules (non-negotiable)

1. Heater outputs change only through `Enable_SSR/Disable_SSR/Enable_EMR/Disable_EMR`. Every way out
   of an active program state (end, abort, error, loading or replacing a program) must go through
   `END_Program()`/`ABORT_Program()` and leave **both SSR and EMR off**.
2. Fail safe. On a bad sensor read, a parse error, NaN/inf, overflow or an unexpected state, stop
   heating. Never keep going on a stale or assumed value.
3. Never weaken, skip or slow down a safety check (min/max temperature, housing temperature, sensor
   errors, grace count) unless the user explicitly asks for it. Say so clearly in the summary.
4. Check user-supplied numbers (program steps, preferences, HTTP parameters) for range *and* for
   divide-by-zero before they reach the PID or the program runner.
5. Don't present a feature as a safety feature unless the code enforces it. Docs and `pidkiln.conf`
   comments must describe what is actually implemented.

## Coding conventions

- Follow the existing style. Globals are `Capitalized_Words`, macros and enums are `ALL_CAPS`, and
  each module keeps its prefix (`LCD_`, `Program_`, `Prefs_`, ...). Don't reformat code you aren't
  changing.
- Log with `DBG dbgLog(LOG_<LEVEL>, "[MODULE] ...\n", ...)`. Put the module tag in brackets.
- Memory: use the `MALLOC`/`REALLOC` macros (they switch between PSRAM and internal RAM). Pair every
  allocation with a `free` and set the pointer to `NULL` afterwards.
- Strings in new or touched code: use bounded operations (`snprintf`, `strlcpy`, size from
  `sizeof(buf)`). Never `strcpy`/`sprintf` external input (HTTP, files, prefs) into a fixed buffer.
- Prefer `constexpr`/`const` over new `#define`s, fixed-width integer types over `byte`/`int`, and
  enums over magic numbers.
- Keep changes small and focused. Fix the bug you came for, and list other problems you notice
  instead of fixing them on the side.

## Web interface

- Every route that reads or changes state needs authentication. For upload routes, check it **in the
  upload/body callback too**. AsyncWebServer calls that callback *before* the request handler, where
  the auth check normally sits.
- Escape values that come from users or files before they go into HTML or JS templates.
- Keep the gzipped JS in `data/js/` and the CDN URLs in `PIDKiln.h` (`JS_*`) on the same version.

## Recipes

**Adding a preference** touches six places. Leave one out and the preference silently does nothing:
`PREFERENCES` enum and `PrefsName[]` in `PIDKiln.h` (same order), a default in `Setup_prefs()`, the
parser in `Preferences_parser()` (http), the field in `data/preferences.html`, and the documented
default in `data/etc/pidkiln.conf`. Read numeric values through the union member that matches the
declared type.

**Program file format** (`data/programs/*.txt`): the first `#` line is the description, then one
`temp:minutes_to_reach:minutes_dwell` step per line (exactly three numbers, at most `MAX_PRG_LENGTH`
steps), and `#` starts a comment. `minutes_to_reach` 0 jumps the set point straight to `temp`. The
parser is `parse_program_step()` in `src/program_step.h` (unit tested), called from `Load_program()`/`add_program_line()` in `PIDKiln_program.ino`. The same checks apply to
web upload and LCD load.

**Releasing**: bump `PVer`/`PDate` in `PIDKiln.h`, add a `Changelog.txt` entry, and mention it if the
`OTA_Install/` binaries are now stale.

## Don't

- Don't commit real WiFi or auth credentials. `data/etc/pidkiln.conf` ships with placeholders.
- Don't rebuild or commit binaries in `OTA_Install/` unless asked.
- Don't change pin assignments in `PIDKiln.h` unless asked. They match the published wiring diagrams.
