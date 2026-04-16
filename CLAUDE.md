# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**ALC Hub ESP** — an ESP32-C6 WiFi + (future) BLE hub complementing the Nordic cellular `alc_hub` (`~/nordic/ncs/v3.2.4/alc_hub`). The Nordic design is the long-term target on a custom PCB based on Thingy91X, but nRF7002-DK integration has been problematic, so this ESP32-C6 variant exists as an alternative hub for home deployments where cellular is not required.

- **Board:** ESP32-C6 (WiFi 6 + BLE 5.3 + IEEE 802.15.4).
- **Framework:** ESP-IDF v5.5.4, C++20.
- **Role:** Home WiFi gateway — publishes BLE-peripheral data (Friend Finder, Fridge Master, etc.) to `mqtt1.alc-carefree.com`.
- **Counterpart:** Same MQTT interface as Nordic `alc_hub`, same `alc/<serial>/...` topic tree.

BLE integration path is undecided: either native ESP32-C6 NimBLE stack, or a UART link to an nRF54L15-DK running BLE firmware. Current code is WiFi + MQTT only.

## Build Commands

```bash
# Source the IDF environment (every new shell).
source ~/esp/esp-idf/export.sh

# First-time or re-target.
idf.py -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;credentials.sdkconfig' set-target esp32c6

# Build / flash / monitor.
idf.py -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;credentials.sdkconfig' build
idf.py -p /dev/cu.usbmodem* flash monitor

# Clean build.
rm -rf build sdkconfig && idf.py -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;credentials.sdkconfig' set-target esp32c6
```

**Always pass `SDKCONFIG_DEFAULTS` with both files** — the second file (`credentials.sdkconfig`) overlays device secrets. Without it, the broker auth and WiFi association will both fail. Consider adding a shell alias or `idf.py`-wrapping script if this becomes tedious.

## Credentials

Secrets are NOT committed. Two-file pattern:

- `sdkconfig.defaults` — committed. Chip config, WiFi/MQTT tuning, partition table.
- `credentials.sdkconfig` — **gitignored**. Contains `CONFIG_ALC_HUB_SERIAL`, `CONFIG_ALC_HUB_SECRET`, `CONFIG_ALC_WIFI_SSID`, `CONFIG_ALC_WIFI_PASSWORD`.
- `credentials.sdkconfig.template` — committed. Copy, fill in, rename.

Kconfig entries are declared in `main/Kconfig.projbuild` and become `CONFIG_*` preprocessor macros in C/C++ code.

Device serial format: `HUB-MMYY-Wnnn` (W = WiFi hub variant, to distinguish from Nordic cellular hubs which use `HUB-MMYY-Cnnn`). MMYY = registration month/year, nnn = device number 001–999.

## Architecture

### Boot Sequence (`main.cpp`)

1. `nvs_flash_init()` — required by WiFi driver.
2. `WifiSta::Init()` → creates netif, registers event handlers.
3. `MqttClient::Init()` — configures broker URI, client ID, credentials, LWT.
4. `WifiSta::Connect()` — triggers WPA2/WPA3 association.
5. On `IP_EVENT_STA_GOT_IP`: callback fires → `MqttClient::Start()`.
6. On `MQTT_EVENT_CONNECTED`: publishes retained `{"status":"online"}` to `alc/<serial>/status`.

### WiFi Station (`wifi_sta.cpp`)

Thin wrapper over esp-wifi. Singleton with an event-driven state machine: on disconnect, retries up to 10 times via `esp_wifi_connect()`. Calls user-provided callback on connect/disconnect transitions. PMF capable but not required (compatible with WPA2-only APs).

### MQTT Client (`mqtt_client.cpp`)

Wraps `esp-mqtt` with TLS:

- Broker: `mqtts://mqtt1.alc-carefree.com:8883`.
- CA: ISRG Root X1 (Let's Encrypt), embedded as `ISRG_ROOT_X1_PEM` in `main/certificate.h`.
- Auth: `client_id = username = CONFIG_ALC_HUB_SERIAL`; `password = CONFIG_ALC_HUB_SECRET`.
- LWT: topic `alc/<serial>/status`, payload `{"status":"offline"}`, QoS 1, retained.
- Online publish: same topic, `{"status":"online"}`, QoS 1, retained. Fires from the `MQTT_EVENT_CONNECTED` handler, not from application code.
- Event callback bridges `esp_mqtt_event_id_t` to a simple connected/disconnected bool for the app.

### MQTT Topic Structure (matches Nordic `alc_hub`)

```
alc/<serial>/status    # {"status":"online"} retained; LWT {"status":"offline"}
alc/<serial>/<...>     # Future: BLE peripheral data, room telemetry, presence
```

All topics are prefixed with the device serial, not `alc/hub/<serial>/` — this is the Nordic-agreed convention.

## Conventions

Inherited from global style (`~/.claude/CLAUDE.md`):

- C++20, classes in `alc::` namespace, `.cpp`/`.hpp` pairs, `#pragma once`.
- `PascalCase` public methods, `camelCase` private methods, `m_` member prefix, `M_` module-scope constants.
- Brace initialisation, `constexpr` constants, fixed-width integer types.
- `ESP_LOGE` / `ESP_LOGI` / `ESP_LOGW` (never `printf`). Error logs end with `!`.

ESP-IDF specifics:

- `extern "C" void app_main()` is the entry point.
- Header pulls in `<esp_log.h>`, `<esp_event.h>`, `<esp_wifi.h>`, `<nvs_flash.h>`, `<mqtt_client.h>` etc.
- Do not confuse **Espressif's** `mqtt_client.h` (the library) with our `main/mqtt_client.hpp` (the wrapper). Include ordering matters — system/framework headers first, then local headers.
- `clangd` will flag ESP-IDF headers as missing until a build generates `build/compile_commands.json`. This is expected.

## Future Work

- **BLE** — decide between ESP32-C6 NimBLE stack (single-chip) or UART link to nRF54L15-DK (reuses existing Nordic BLE firmware). The UART approach keeps the BLE firmware identical across both hub variants.
- **SNTP** — time sync not yet implemented; will be needed before any telemetry publish that requires timestamps.
- **OTA** — single-app partition layout in use. Switch to dual-OTA partitions when OTA is added (`CONFIG_PARTITION_TABLE_TWO_OTA` or custom `partitions.csv`).

## Related Projects

- `~/nordic/ncs/v3.2.4/alc_hub/` — the Nordic cellular counterpart. Mirror its MQTT conventions when adding new topics.
- `alc_friend_finder`, `alc_fridge_master`, `alc_drawer_master`, `alc_room_master` — BLE peripherals this hub will aggregate.
