// MAX31855 thermocouple converter for the PIDKiln Wokwi simulation, with a simple kiln model.
//
// SPI (read only): on CS low the chip shifts out the 32-bit MAX31855 frame on SO, MSB first.
// Temperature source ("mode" control):
//   0 = kiln model: the kiln heats while HEAT (SSR output) and EMR are both high, and loses heat
//       to the room otherwise - so the firmware's PID loop actually closes
//   1 = manual: the "temperature" slider is the reading (use this for the housing sensor)
// "fault" control (sensor and heater failures, to test the safety checks):
//   0 none, 1 thermocouple open, 2 short to GND, 3 short to VCC,
//   4 dead heater (HEAT ignored), 5 SSR welded on (heats whenever EMR is closed)
//
// SPDX-License-Identifier: GPL-2.0-only

#include "wokwi-api.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define AMBIENT_C       20.0
#define MAX_KILN_C      1400.0   // temperature the model settles at with the heater always on
#define COLD_JUNCTION_C 25.0     // chip's own (board) temperature

typedef struct {
  pin_t cs, heat, emr;
  spi_dev_t spi;
  uint8_t buffer[4];
  uint32_t attr_mode, attr_temperature, attr_fault, attr_heat_rate;
  double kiln_c;        // modelled kiln temperature
  uint64_t model_ns;    // simulation time the model was last advanced to
  bool heating;         // heater state since model_ns
} chip_state_t;

static bool heater_on(chip_state_t *chip){
  uint32_t fault = attr_read(chip->attr_fault);
  bool emr = pin_read(chip->emr) == HIGH;
  if(fault == 4) return false;           // dead heating element
  if(fault == 5) return emr;             // welded SSR: only the EMR can cut the power
  return emr && pin_read(chip->heat) == HIGH;
}

// Advance the kiln model to "now" with the heater state that held since the last call
static void advance_model(chip_state_t *chip){
  uint64_t now = get_sim_nanos();
  double dt = (double)(now - chip->model_ns) / 1e9;
  double power = attr_read_float(chip->attr_heat_rate) / 60.0;   // C per second at full power, cold kiln
  double loss = power / (MAX_KILN_C - AMBIENT_C);                // so that full power settles at MAX_KILN_C
  chip->model_ns = now;
  while(dt > 0){                          // Euler steps of at most 0.5 s - plenty for kiln time constants
    double step = dt > 0.5 ? 0.5 : dt;
    chip->kiln_c += ((chip->heating ? power : 0.0) - loss * (chip->kiln_c - AMBIENT_C)) * step;
    dt -= step;
  }
  chip->heating = heater_on(chip);
}

static uint32_t encode_frame(double thermocouple_c, uint32_t fault){
  if(thermocouple_c < -270) thermocouple_c = -270;
  if(thermocouple_c > 1800) thermocouple_c = 1800;
  uint32_t tc = (uint32_t)((int32_t)(thermocouple_c * 4.0)) & 0x3FFF;    // D31..D18, 0.25 C per bit
  uint32_t cj = (uint32_t)((int32_t)(COLD_JUNCTION_C * 16.0)) & 0x0FFF;  // D15..D4, 0.0625 C per bit
  uint32_t frame = (tc << 18) | (cj << 4);                               // D17 and D3 stay 0 (chip ID)
  if(fault >= 1 && fault <= 3){
    frame |= 1u << 16;                    // D16: any fault
    if(fault == 1) frame |= 1u << 0;      // D0: open circuit
    if(fault == 2) frame |= 1u << 1;      // D1: short to GND
    if(fault == 3) frame |= 1u << 2;      // D2: short to VCC
  }
  return frame;
}

static void on_pin_change(void *user_data, pin_t pin, uint32_t value){
  chip_state_t *chip = (chip_state_t *)user_data;
  if(pin == chip->cs){
    if(value == LOW){
      advance_model(chip);
      double reading = attr_read(chip->attr_mode) == 1 ? attr_read_float(chip->attr_temperature) : chip->kiln_c;
      uint32_t frame = encode_frame(reading, attr_read(chip->attr_fault));
      chip->buffer[0] = frame >> 24;
      chip->buffer[1] = frame >> 16;
      chip->buffer[2] = frame >> 8;
      chip->buffer[3] = frame;
      spi_start(chip->spi, chip->buffer, sizeof(chip->buffer));
    }else{
      spi_stop(chip->spi);
    }
  }else{
    advance_model(chip);                  // HEAT or EMR changed: close the old interval first
  }
}

static void on_spi_done(void *user_data, uint8_t *buffer, uint32_t count){
  // The firmware reads one 32-bit frame per CS low - nothing more to send
}

void chip_init(void){
  chip_state_t *chip = calloc(1, sizeof(chip_state_t));

  chip->attr_mode = attr_init("mode", 0);
  chip->attr_temperature = attr_init_float("temperature", AMBIENT_C);
  chip->attr_fault = attr_init("fault", 0);
  chip->attr_heat_rate = attr_init_float("heatRate", 20.0);

  chip->cs = pin_init("CS", INPUT_PULLUP);
  chip->heat = pin_init("HEAT", INPUT_PULLDOWN);
  chip->emr = pin_init("EMR", INPUT_PULLDOWN);
  chip->kiln_c = AMBIENT_C;
  chip->model_ns = get_sim_nanos();

  const spi_config_t spi_config = {
    .sck = pin_init("SCK", INPUT),
    .mosi = NO_PIN,
    .miso = pin_init("SO", INPUT),
    .mode = 0,
    .done = on_spi_done,
    .user_data = chip,
  };
  chip->spi = spi_init(&spi_config);

  const pin_watch_config_t watch_config = {
    .edge = BOTH,
    .pin_change = on_pin_change,
    .user_data = chip,
  };
  pin_watch(chip->cs, &watch_config);
  pin_watch(chip->heat, &watch_config);
  pin_watch(chip->emr, &watch_config);
}
