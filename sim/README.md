# PIDKiln in the Wokwi simulator

Runs the real firmware on a simulated ESP32 in [Wokwi](https://wokwi.com): the LCD screens, the
rotary encoder, the web interface, and a simulated kiln that heats up when the firmware switches the
SSR. Use it to try menus and programs and to exercise the safety checks without a kiln. It does not
replace testing on the hardware: SPI timing, relays and a real kiln behave differently.

## What is simulated

| Real hardware | In the simulation |
|---|---|
| ST7920 128x64 LCD | SSD1306 128x64 OLED (Wokwi has no ST7920). The firmware is built with `-DPIDKILN_SIM`, which switches the u8g2 driver; the screens are drawn by the same code |
| Rotary encoder + button | KY-040 encoder (GPIO 34/35, button 32) |
| MAX31855 + thermocouple in the kiln | Custom chip `sim/chips/max31855.chip.c` with a kiln model: it heats while the SSR (GPIO 19) **and** the EMR (GPIO 21) are on, and cools towards 20 °C otherwise |
| MAX31855 on the housing | The same chip in manual mode, fixed at 25 °C |
| SSR, EMR, alarm outputs | Red, yellow and orange LEDs |
| Your WiFi | Wokwi's open `Wokwi-GUEST` network (the simulation build always joins it as a client) |

The data partition (web UI, `pidkiln.conf`, sample programs) is in the flash image, so the device
starts like a freshly installed one: login `admin` / `hotashell`.

## Run it

1. **Get the build files** into `sim/build/`, either way:
   - From CI: every push builds them as the `PIDKiln-sim` artifact.
     `gh run download <run id> -R Jlibis/PIDKiln -n PIDKiln-sim -D sim/build`
     (find the run id with `gh run list -R Jlibis/PIDKiln --branch <branch>`).
   - Locally (Linux/WSL, needs `arduino-cli` and [`wokwi-cli`](https://github.com/wokwi/wokwi-cli)):
     `bash sim/build.sh`
2. **VS Code**: install the Wokwi extension, run `Wokwi: Request a new License` once (needs a free
   Wokwi account), open the `sim/` folder (it holds `wokwi.toml`) and run `Wokwi: Start Simulator`.
   The web interface is at <http://localhost:8180> while it runs.
3. **Or headless** with `wokwi-cli` (needs a `WOKWI_CLI_TOKEN` from wokwi.com/dashboard/ci; the free
   plan has a monthly minute limit):
   `wokwi-cli sim --timeout 60000 --screenshot-part oled1 --screenshot-time 59000`

## Controls on the kiln chip

Click the "kiln" chip in the diagram to get its sliders:

- **mode**: 0 = kiln model (default), 1 = the temperature slider is the reading.
- **heatRate**: how fast the cold kiln heats at full power, in °C per minute (default 20). The model
  settles at about 1400 °C with the heater always on. Programs run in real time, so use short steps
  (for example a quick program to 200 °C in 10 minutes) and a high heat rate.
- **fault**: 0 none, 1 thermocouple open, 2 short to GND, 3 short to VCC, 4 dead heater,
  5 SSR welded on.

## Things to try

- Start a quick program and watch the SSR LED pulse and the temperature follow the set point.
- **fault 1** while a program runs: after `MAX31855_Error_Grace_Count` bad reads it aborts with
  error 8, both relay LEDs go out.
- **fault 4** with `Thermal_Runaway` set (e.g. 2 minutes): abort with error 16 (heating stall).
- **fault 5** during a cooling or dwell step: the kiln keeps heating with the SSR off, so it aborts
  with error 17 (stuck SSR) and opens the EMR, which stops the heating.
- Use the web interface and the encoder at the same time during a run - nothing should freeze.
- Set the housing chip to mode 1 and drag its temperature above `MAX_Housing_Temperature`.
