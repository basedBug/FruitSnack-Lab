// FruitSnack — ESP32-S3 hotend controller
//
// System wiring (see src/config.h for the full pin map):
//   * FreeRTOS task "control" runs the PID loop at CONTROL_PERIOD_MS (20 Hz)
//   * FreeRTOS task "net"     drives WiFi/mDNS and the web server
//   * FreeRTOS task "uart"    reads the 921600 baud fallback console
//
// The Arduino loop() is intentionally idle: everything runs in FreeRTOS tasks
// so the control loop is never delayed by network or filesystem work.
//
// The task pinning is load-bearing, not decoration: "control" is pinned to
// core 1 at priority 3, above the network and console tasks on core 0, so
// SD/WiFi/websocket jitter can never reach the PID loop.  Keep it that way.
//
// Each module owns its singleton (gLogger, gController, gSettings, gNetwork,
// gConsole, gWeb).  This file only brings them up, in boot order, and runs the
// three tasks.
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "config.h"
#include "console.h"
#include "heater_controller.h"
#include "logger.h"
#include "network_manager.h"
#include "settings.h"
#include "web_server.h"

/// Defaults from the build flags are only used when nothing was stored yet.
static void applyCompileTimeDefaults(AppSettings &settings) {
  if (strlen(settings.wifiSsid) == 0 && strlen(WIFI_SSID) > 0) {
    strncpy(settings.wifiSsid, WIFI_SSID, sizeof(settings.wifiSsid) - 1);
  }
  if (strlen(settings.wifiPassword) == 0 && strlen(WIFI_PASSWORD) > 0) {
    strncpy(settings.wifiPassword, WIFI_PASSWORD, sizeof(settings.wifiPassword) - 1);
  }
  if (strlen(settings.hostname) == 0 || strcmp(settings.hostname, "fruitsnack") == 0) {
    if (strlen(MDNS_HOSTNAME) > 0) {
      strncpy(settings.hostname, MDNS_HOSTNAME, sizeof(settings.hostname) - 1);
    }
  }
}

// ------------------------------------------------------------------ tasks

void controlTask(void *) {
  uint32_t nextRun = millis();
  for (;;) {
    gController.loop(millis());
    nextRun += CONTROL_PERIOD_MS;
    const int32_t wait = (int32_t)(nextRun - millis());
    vTaskDelay(pdMS_TO_TICKS(wait > 0 ? wait : 1));
  }
}

void netTask(void *) {
  for (;;) {
    gNetwork.loop();
    gWeb.loop();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void uartTask(void *) {
  String line;
  uint32_t last_telemetry_ms = 0;
  bool binaryMode = false;
  for (;;) {
    while (Serial1.available() > 0) {
      const int c = Serial1.read();

      if (c == 0x02) {  // STX: switch to binary telemetry for host scopes.
        if (!binaryMode) {
          binaryMode = true;
          gLogger.setUartEnabled(false);
          line = "";
          // Send the first packet on this pass rather than waiting a period.
          last_telemetry_ms = millis() - TELEMETRY_PERIOD_MS;
        }
        continue;
      }
      if (c == 0x03) {  // ETX: return to the ASCII console and telemetry.
        if (binaryMode) {
          binaryMode = false;
          gLogger.setUartEnabled(true);
          line = "";
          Serial1.print("\nfruitsnack> ");
        }
        continue;
      }
      if (binaryMode) {
        continue;  // Ignore all other bytes until ETX.
      }

      if (c == '\n' || c == '\r') {
        if (line.length() > 0) {
          Serial1.printf("\n%s\n", line.c_str());
          const Console::Result result = gConsole.execute(line);
          if (result.text.length() > 0) {
            Serial1.printf("%s\n", result.text.c_str());
          }
          line = "";
        }
        Serial1.print("fruitsnack> ");
      } else if (c >= 32 && c < 127) {
        if (line.length() < 200) line += (char)c;
        Serial1.write((uint8_t)c);
      }
    }

    // Minimal one-way telemetry tap for host-side logging/plotting.  ASCII by
    // default; a packed binary packet while the host has selected binary mode.
    const uint32_t now = millis();
    const bool ready = binaryMode || line.length() == 0;
    if (ready && now - last_telemetry_ms >= TELEMETRY_PERIOD_MS) {
      last_telemetry_ms = now;
      const SystemState state = gController.snapshot();
      if (binaryMode) {
        const float values[4] = {
            static_cast<float>(state.uptimeMs) / 1000.0f,
            state.temperatureC,
            state.setpointC,
            static_cast<float>(state.duty)};
        uint8_t packet[2 + sizeof(values)];
        packet[0] = 0xAA;
        packet[1] = 0x55;
        memcpy(packet + 2, values, sizeof(values));
        if (Serial1.availableForWrite() >= static_cast<int>(sizeof(packet))) {
          Serial1.write(packet, sizeof(packet));
        }
      } else {
        char row[64];
        const int len = snprintf(row, sizeof(row), "%lu,%.2f,%.2f,%.0f\n",
                                 static_cast<unsigned long>(state.uptimeMs),
                                 state.temperatureC, state.setpointC, state.duty);
        if (len > 0 && static_cast<size_t>(len) < sizeof(row) &&
            Serial1.availableForWrite() >= len) {
          Serial1.write(reinterpret_cast<const uint8_t *>(row), static_cast<size_t>(len));
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  // ---------------------------------------------------------------- serial
  Serial.begin(UART_BAUD);
  Serial1.setRxBufferSize(1024);
  Serial1.setTxBufferSize(1024);
  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  delay(50);

  gLogger.begin(&Serial, &Serial1);
  gLogger.info("");
  gLogger.info("%s %s starting", FW_NAME, FW_VERSION);
  gLogger.info("built %s %s", FW_COMPILE_DATE, __TIME__);

  // ---------------------------------------------------------------- storage
  const bool storage_ok = gSettings.begin();
  AppSettings settings;
  const bool restored = gSettings.load(settings);
  applyCompileTimeDefaults(settings);
  if (!storage_ok) {
    gLogger.error(
        "littlefs not mounted: configuration cannot be restored or saved and the "
        "dashboard is unavailable (running on defaults)");
  } else if (restored) {
    gLogger.info("configuration restored from flash");
  } else if (gSettings.exists()) {
    gLogger.warn(
        "config.json is present but unreadable: running on defaults and NOT "
        "overwriting the stored file");
  } else {
    gLogger.info("configuration using defaults (nothing stored yet)");
  }

  // ---------------------------------------------------------------- control
  gController.begin(settings);

  // ---------------------------------------------------------------- network
  gNetwork.begin(settings);

  // ---------------------------------------------------------------- web
  gWeb.begin();

  // ---------------------------------------------------------------- tasks
  // Every task creation is checked: a silent failure would look like a healthy
  // boot while the heater is unsupervised or the network stack is dead.
  const BaseType_t control_task_ok =
      xTaskCreatePinnedToCore(controlTask, "control", 4096, nullptr, 3, nullptr, 1);
  const BaseType_t net_task_ok =
      xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 2, nullptr, 0);
  const BaseType_t uart_task_ok =
      xTaskCreatePinnedToCore(uartTask, "uart", 4096, nullptr, 2, nullptr, 0);

  if (control_task_ok != pdPASS) {
    gLogger.error(
        "FATAL: control task could not be created - the heater is NOT being "
        "controlled (it stays off)");
  }
  if (net_task_ok != pdPASS) {
    gLogger.error("net task could not be created - wifi management and the web server are dead");
  }
  if (uart_task_ok != pdPASS) {
    gLogger.error("uart task could not be created - the serial console is dead");
  }

  gLogger.info("control loop %u ms, telemetry %u ms", (unsigned)CONTROL_PERIOD_MS,
               (unsigned)TELEMETRY_PERIOD_MS);
  if (control_task_ok == pdPASS) {
    gLogger.info("ready");
  } else {
    gLogger.error("not ready: the control loop is not running");
  }
  Serial.println("fruitsnack> ");
  Serial1.println("fruitsnack> ");
}

void loop() {
  // All real work happens in the FreeRTOS tasks.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
