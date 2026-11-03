// FruitSnack — ESP32-S3 hotend controller
// Compile-time configuration.
//
// WIRING (default, ESP32-S3-DevKitC-1-N8R2)
//   Heater MOSFET gate .... GPIO21  (PWM out, drives heater SSR/MOSFET)
//   NTC100K divider node ... GPIO1   (3V3 -- 4.7k -- GPIO1 -- NTC -- GND)
//   UART1 TX/RX ........... GPIO43 / GPIO44 @ 921600 baud (fallback/debug)
//   USB CDC ............... native USB (Serial) @ 921600 baud
//
// Every pin/limit below can be overridden at build time, e.g.
//   pio run -e esp32s3_custom --project-option="build_flags=-DPIN_HEATER_PWM=6"
#pragma once

#include <Arduino.h>
#include <stdint.h>

// ---------------------------------------------------------------- identity
#define FW_NAME "FruitSnack"
#define FW_VERSION "1.0.0"
#define FW_COMPILE_DATE __DATE__ " " __TIME__

// ---------------------------------------------------------------- network
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME "fruitsnack"
#endif
#ifndef MDNS_SERVICE_NAME
#define MDNS_SERVICE_NAME "FruitSnack Hotend"
#endif
#ifndef WIFI_FALLBACK_AP_SSID
#define WIFI_FALLBACK_AP_SSID "FruitSnack-Setup"
#endif
#ifndef WIFI_FALLBACK_AP_PASSWORD
#define WIFI_FALLBACK_AP_PASSWORD "fruitsnack"
#endif
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_RETRY_INTERVAL_MS 5000
#define HTTP_PORT 80

// ---------------------------------------------------------------- heater
#ifndef PIN_HEATER_PWM
#define PIN_HEATER_PWM GPIO_NUM_21
#endif
#ifndef HEATER_PWM_FREQ_HZ
#define HEATER_PWM_FREQ_HZ 1000
#endif
#ifndef HEATER_PWM_BITS
#define HEATER_PWM_BITS 8
#endif
/// Safety cut-off: heater is forced off above this temperature (deg C).
#ifndef HEATER_MAX_TEMPERATURE_C
#define HEATER_MAX_TEMPERATURE_C 250.0f
#endif
/// NTC100K B3950 becomes unreliable above this temperature (deg C).
#ifndef SENSOR_MAX_VALID_C
#define SENSOR_MAX_VALID_C 350.0f
#endif
/// Temperature below this is treated as a shorted/failed sensor (deg C).
#ifndef SENSOR_MIN_VALID_C
#define SENSOR_MIN_VALID_C -20.0f
#endif
/// Hard ceiling for a user settable setpoint or safety temperature (deg C).
///
/// It is the top of the sensor's valid range: a higher limit could never be
/// honoured, because the controller treats anything above SENSOR_MAX_VALID_C as
/// an out-of-range sensor and faults instead of heating.
#define TEMPERATURE_CEILING_C SENSOR_MAX_VALID_C

// ---------------------------------------------------------------- thermistor
#ifndef PIN_NTC
#define PIN_NTC GPIO_NUM_2
#endif
#ifndef NTC_NOMINAL_RESISTANCE_OHMS
#define NTC_NOMINAL_RESISTANCE_OHMS 100000.0f
#endif
#ifndef NTC_NOMINAL_TEMP_C
#define NTC_NOMINAL_TEMP_C 25.0f
#endif
#ifndef NTC_BETA
#define NTC_BETA 3950.0f
#endif
/// Pull-up resistor between 3V3 and the divider node (the NTC is on the low
/// side, between the node and GND). Must match the resistor on the board.
#ifndef NTC_PULLUP_OHMS
#define NTC_PULLUP_OHMS 4700.0f
#endif
#ifndef NTC_ADC_MAX
#define NTC_ADC_MAX 4095.0f
#endif
#ifndef NTC_ADC_REFERENCE_V
#define NTC_ADC_REFERENCE_V 3.3f
#endif
/// ESP32-S3 datasheet table 5-6: the calibrated 11 dB range is specified up to
/// 2900 mV. Above that the reading is undefined, which with a 4.7k pull-up
/// happens below roughly 50 degC - the reading is then only an upper bound.
#ifndef NTC_ADC_MAX_VALID_MV
#define NTC_ADC_MAX_VALID_MV 2900.0f
#endif
/// Raw ADC code at which the converter has railed (12 bit full scale = 4095).
#ifndef NTC_ADC_RAIL_RAW
#define NTC_ADC_RAIL_RAW 4090
#endif
/// Node voltage the ADC reports once it has railed (raw = 4095). Anything
/// above this is an extrapolation, so the conversion clamps to it.
#ifndef NTC_ADC_SATURATION_MV
#define NTC_ADC_SATURATION_MV 3100.0f
#endif
/// Median window size for raw sample rejection.
#ifndef NTC_MEDIAN_WINDOW
#define NTC_MEDIAN_WINDOW 5
#endif
/// Exponential moving average smoothing factor (0..1).
#ifndef NTC_EMA_ALPHA
#define NTC_EMA_ALPHA 0.25f
#endif

// ---------------------------------------------------------------- control loop
/// PID loop period in milliseconds (20 Hz).
#ifndef CONTROL_PERIOD_MS
#define CONTROL_PERIOD_MS 50
#endif
/// Telemetry/graph sample period in milliseconds (4 Hz).
#ifndef TELEMETRY_PERIOD_MS
#define TELEMETRY_PERIOD_MS 250
#endif
/// Websocket state broadcast period in milliseconds (5 Hz).
#ifndef WS_BROADCAST_PERIOD_MS
#define WS_BROADCAST_PERIOD_MS 200
#endif
/// Default PID integral clamp (anti-windup) in output percent.
#ifndef PID_INTEGRAL_LIMIT
#define PID_INTEGRAL_LIMIT 100.0f
#endif

// ---------------------------------------------------------------- storage
#define LITTLEFS_PARTITION_LABEL "littlefs"
#define LITTLEFS_BASE_PATH "/littlefs"
#define CONFIG_FILE_PATH "/config.json"
#define LITTLEFS_MOUNT_PATH "/littlefs"

// ---------------------------------------------------------------- uart
// `Serial` is the on-chip USB-Serial/JTAG port (GPIO19/20, ARDUINO_USB_MODE=1):
// its baud rate is fixed by the USB standard, so the number is only cosmetic.
// `Serial1` is a real hardware UART and runs at UART_BAUD. It is remapped onto
// the DevKitC-1 UART header pins (GPIO43 = TX, GPIO44 = RX) instead of the
// core defaults (GPIO16/15) so it lines up with the silkscreen.
#define UART_BAUD 921600
#define UART_TX_PIN 43
#define UART_RX_PIN 44
#define UART_CONSOLE_BUFFER 512

// ---------------------------------------------------------------- websocket
#define WS_PATH "/ws"
#define WS_MAX_CLIENTS 4

// ---------------------------------------------------------------- helper
inline constexpr uint32_t millisElapsed(uint32_t now, uint32_t since) {
  return (uint32_t)(now - since);
}
