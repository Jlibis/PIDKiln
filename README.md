# pidkiln
PIDKiln is PID temperature controller, for any task involving high temperatures - like burning ceramic, glass fusing, soft metal melting, burning lost wax casting forms - and probably much more.

## Key features:
- Interface accessible both from LCD screen and WWW Webpage
- Unlimited (limited only by storage) temperature programs number, up to 40 steps per program, program file size limited to 10KiB (but this is artificial limit - can be extended)
- Internal ESP flash storage (LittleFS) for programs, data, logs, configuration
- Local preferences on disk, editable with Web interface
- Online monitoring, program management, editing, graphs and kiln control
- Build in clock synchronised with NTP servers (if Internet connected)
- Safety features built in: min/max kiln temperature, housing temperature (kiln insulation failure), thermocouple failure, heating stall (Thermal_Runaway, off by default) and stuck SSR detection; an optional EMR relay cuts power when the SSR fails closed
- Online PIDKiln firmware upgrade with web interface
- Syslog type, over the network (UDP) logging to remote server
- ...simply cool and cheap (comparing to commercially available products) all in one solution

### Sample LCD screens
![LCD menu sample](https://raw.githubusercontent.com/Saur0o0n/pidkiln/master/Documentation/images/PIDKiln_LCD_sample2.png)
### Sample Web interface window
![Web interface](https://github.com/Saur0o0n/PIDKiln/blob/master/Documentation/images/PIDKiln-web-chart.jpg)

## Required components:
- ESP32-Wrover board (I've used TTGO with MicroSD)
- MAX31855 breakout board
- K-type thermocouple
- DC->AC solid state relay

Kind of optional, but recommended:
- 128x65 dot matrix LCD 12864B v2
- Rotary encoder with button

Optional:
- DC/AC secondary relay - like SLA-05VDC-SL-C (240V/30A) mechanical relay
- Additional MAX31855 board with K-type thermocouple for housing temperature measuring
- Coil power meter - 30A/1V. This is contact less power meter that generates small current when energy flows inside it's coil. Mine already had burden resistor - so on output it produces -1V to 1V, proportionally it's coil rated for 30A. We just need to measure it to know how much juice flows to the heater.
- Perhaps a kiln :)

## BOM and expenses

Total expenses for this set should be around 30-40$
- ESP32-Wrover board: 11-14$ if exactly like mine, but other ESP32-Wrover you can bought for 6$+
- MAX31855 board: ~2$
- K-type thermocouple: 1$-10$ - depending on max temperature it can withstand
- LCD 12864B: ~5$
- Encoder: 1$
- SSR: 4$ + 4$ for radiator
- 2x10nF capacitors (at least 10nF - with higher capacitance you will have bigger hysteresis, but this actually may be favorable - for now I use 100nF on both MAX31855)

- Mechanical relay EMR SLA-05VDC-SL-C (Songle): 3$


## Why this configuration?

I've already used "kind of" controller made on Linux server and cron procedures, and it was ok.. for a while. Then I bought cheap Chinese controller PC410, just to find out, it is missing most of required stuff (that original PC410 should have), and I already was accustom to be able to see everything over Internet.
So I've made few attempts with Arduino - it was fine, but since I need remote access - ESP will be much better choice. I've started to work on ESP8266, but being afraid that I will be lacking some GPIOs - I've moved to ESP32. And since price difference is negligible, this is the platform of choice for this project.
It is also beneficial, because it has build in flash memory, that I can use for all required data - without need of connecting additional SD cards. Since I had ESP-Wrover (not ESP32-Wroom), I've also utilised it's PSRAM, but since PIDKiln 1.2 you can also use Wroom module (see Wiki).

MAX31855 comparing to more available MAX6675, is better choice since it allow us to work up to 1350C and it has 3,3V logic.

LC12864B is perhaps not the best choice, but I simply had this one already (I've used if before for my 3D printer). Perhaps later I'll change it. Problem with this LCD is that it has 5V logic. Sometimes it works on 3,3V (depending on version), in my case it does. But since it's one way communication (only MISO) hooking it up to 5V for both logic and back light works and does not crash my board. Clean solution would be to use logic voltage translator (there is plenty of them for 1$).

Relays - Main relay is SSR (Solid State Relay) type. It's because we need to switch it fast and often - SSR can do it, but it will get hot, so make sure you have good radiator. Also if you are going to use cheap Chinese knock offs, make sure it's rated twice the output current of your heater(s).
All relays may fail, and they may fail in closed (conductive) state. Because of it, I've also implemented second stage EMR (Electromechanical Relay) relay in case of SSR failure. It's mechanical, so it won't get hot too much. This will allow to turn off the kiln, in case of SSR failure, with EMR (and other way around too). This additional relay (SLA-05VDC-SL-C) is optional.

Initially for kiln housing temperature readout I wanted to use thermistors, but since I've already had MAX31855 connected (and it costs 2$), using additional one requires only 1 new GPIO on ESP. Without hassle to provide reference voltage etc. Low temperature thermocouples are also darn cheap.

## GPIO/PINs connection

**LCD**

Connected to one of three SPI on ESP32 - called VSPI (MOSI-23, MISO-19, CLK-18, CS-5)

ESP32	| LCD
--------|---------
+3.3V	| BLA (this can be also +5V if you wish)
GND	| BLK
4	| RST
GND	| PSB
+5V	| VCC (This should be - for ESP sake - 3,3V, but my LCD doesn't work with lower voltage. Try first with 3,3V)
GND	| GND
5	| RS
18	| E
23	| R/W

**Encoder**

ESP32	| Encoder
--------|---------
+3.3V	| 5V/VCC
GND	| GND
32	| Key
34	| S2
35	| S1

**MAX31855**

Connected to one of three SPI on ESP32 - called HSPI (MOSI-13, MISO-12, CLK-14) CS-27/15
Add 10nF capacitor to thermocouple input pins - it's a must to have reliable readout.

EPS32	| MAX31855 A
--------|---------
+3.3V	| VCC
GND	| GND
12	| SO/DO (slave output/data output)
14	| SCK (clock)
27	| CS (chip select)

EPS32	| MAX31855 B
--------|---------
+3.3V	| VCC
GND	| GND
12	| SO/DO (slave output/data output)
14	| SCK (clock)
15	| CS (chip select)

**Relays**

ESP32	| SSR
--------|-------
GND	| GND
19**    | IN

ESP32   | EMR (SLA-05VDC-SL-C)
--------|----------------
GND     | GND
21      | IN
5-48V*  | VCC

(*) Do not connect 5V from ESP - use external source. This can be any 5V-48V power supply with around 1W power.

(**) For additional SSR relay (works simultaneously with first one) use PIN 22 - see documentation for explanation

**Power meter**

See the documentation for detailed instruction.

ESP   | 30A/1V
------|--------
3,3V  | 3,3V
GND   | GND
33    | most outside mini jack connector

**Alarm**

This ALARM_PIN goes HIGH for `Alarm_Timeout` seconds (0 = alarm off, which is the default) when a program aborts (something is wrong, or user triggered), and - while no program runs - when the kiln or the housing gets hotter than its limit or PIDKiln detects a stuck SSR. You can connect this pin to buzzer or small relay to trigger some other action on failure.

ESP   | Relay/Buzzer
------|-------------
GND   | GND
26    | +3,3V

**Minimal configuration of PIDKiln**

![PIDkiln minimal wiring](https://raw.githubusercontent.com/Saur0o0n/pidkiln/master/Documentation/PIDKiln_Wiring-min.png)

**Standard configuration of PIDKiln**

![PIDkiln wiring](https://raw.githubusercontent.com/Saur0o0n/pidkiln/master/Documentation/PIDKiln_Wiring.png)

## Power consideration

Preferably you should power your PIDKiln device with regulated 5V. This way you can power ESP32 board through VIN (do not use VIN and USB at once!) pin and use 5V to directly power EMR relay (around 185mA) and LCD backlight (depends of brightness).
You could power board with just USB, but 5V output from my board (ESP32-Wrover TTGO with microsd) is too weak to handle EMR and LCD and most of other boards even do not have 5V out. You could also use VIN as 5V Vout (this pin should be connected directly to USB 5V output) - but then you are limited by USB output and how much board traces can handle.

## Installation

This is short version - for detailed one, please see [Wiki documentation](https://github.com/Saur0o0n/pidkiln/wiki/Hardware-assembly)

- Assemble the hardware, as specified above.
- Clone with submodules: `git clone --recursive <repo url>` (or `git submodule update --init` in an
  existing clone). The submodules in `libraries/` are the two libraries that are not in the Arduino
  library index: [the MAX31855 fork](https://github.com/Saur0o0n/MAX31855) (adds a second hardware SPI
  for ESP32) and [Syslog](https://github.com/arcao/Syslog).
- Open `data/etc/pidkiln.conf` and set your WiFi credentials and, if you want, other parameters.
- For production use, disable serial debug in PIDKiln.ino (`#define DEBUG false`).

**With arduino-cli (recommended, reproducible).** `sketch.yaml` pins the ESP32 core (3.3.x) and every
library version, and arduino-cli (1.3 or newer) installs them on the first build:

```sh
arduino-cli compile --profile wrover --build-property "compiler.cpp.extra_flags=-DTEMPLATE_PLACEHOLDER='~'" .
arduino-cli upload  --profile wrover -p <your serial port> .
```

The CI build (GitHub Actions) uploads ready-made `PIDKiln.ino.bin` and `PIDKiln.littlefs.bin` (web
interface and default files) as artifacts.

**With Arduino IDE 2.x.** Install the ESP32 core 3.x, and the libraries ESP Async WebServer and Async
TCP (by ESP32Async), PID (Brett Beauregard), U8g2 and, if you use the power meter, EmonLib - the
versions listed in `sketch.yaml`. Copy the two folders from `libraries/` into your Arduino libraries
folder. The web templates need `~` as placeholder: either add `-DTEMPLATE_PLACEHOLDER='~'` to your
build flags, or set `TEMPLATE_PLACEHOLDER` to `'~'` in the ESPAsyncWebServer `WebResponseImpl.h`.

**Upload the data directory** as a LittleFS image: with the
[arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload) plugin in IDE 2,
by flashing the CI-built `PIDKiln.littlefs.bin` (e.g. `esptool.py --chip esp32 write_flash 0x290000
PIDKiln.littlefs.bin` for the default partition table), or through the web flash page.

**Upgrading from PIDKiln 1.5 or older (SPIFFS):** the filesystem is now LittleFS, which cannot read the
old SPIFFS partition. The first boot of the new firmware formats it, so **download your programs and
`/etc/pidkiln.conf` from the web interface first**. Then flash the new firmware, open the device in a
browser - it shows a built-in upload page because the web interface files are gone - and upload
`PIDKiln.littlefs.bin`. Restore your programs and settings afterwards.

**Security.** The factory web login is `admin` / `hotashell`, and the access point PIDKiln creates
when it can't join your WiFi (`PIDKiln_AP`, 192.168.10.1) uses `hotashell` as well. Change both in
the preferences - the page warns while either is still the default. The web interface uses HTTP
Basic auth over plain HTTP, so keep PIDKiln on a network you trust and don't expose it to the
Internet. The `OTA_Install/` sketch and binaries are outdated (v1.3) and download firmware over plain
HTTP without any signature check - build from source or use the CI artifacts instead.

## Development

- `CLAUDE.md` is the contributor guide: architecture, the locking rules between tasks, safety rules and
  recipes (adding a preference, program file format, releasing).
- The program step parser (`src/program_step.h`) has host unit tests in `tests/`. Run them with
  `g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/test_program_step.cpp -o t && ./t`. CI runs them
  together with the firmware build.
- `AUDIT.md` is a code review of the firmware and records which branch fixed which finding.
- `sim/` runs the firmware in the [Wokwi](https://wokwi.com) simulator: LCD (as an SSD1306), encoder,
  web interface and a simulated kiln with fault injection. See [sim/README.md](sim/README.md).

## Documentation

- Most of the documentation you can find on [PIDKiln github page](https://github.com/Saur0o0n/pidkiln/wiki) - so please use it
- Some less formal updates information and step by step instructions will be on [my webpage](https://adrian.siemieniak.net/portal/tag/PIDKiln/)

## Some future ideas

Almost all, what I had in mind is done.. so not much to write here :)
Perhaps...
- RTC clock for better timing and no Internet installations
- MQTT reporting
- HomeAssistant integration



