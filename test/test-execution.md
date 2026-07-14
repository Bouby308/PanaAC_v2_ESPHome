# PanaAC v2 ESPHome — test execution instructions

How to run the tests in `test-specification.md`. The ESPHome side now has an
automated runner for compile/config checks, MQTT runtime checks, and the
automation-specific DUT interactions.

Record each step's outcome on its `Result:` line and commit the updated result
file to the repo branch you are using for the test run.

## Workspace layout

These instructions assume the portable workspace root is `HA/` and the repos
sit under:

```text
HA/
  ha/
    core/
    PanaAC_v2_HA/
  esphome/
    PanaAC_v2_ESPHome/
```

Use paths relative to the workspace root so the whole `HA/` tree can be moved
without rewriting the docs or scripts.

## Recommended order

1. Validate the environment.
2. Run `esphome.g1` for config/compile coverage.
3. Flash the DUT with `C3-automation.yaml`.
4. Run the runtime suites that need the DUT online.
5. Record the result in a timestamped `test-execution-<date-time>.md`.

## Fast path

From `esphome/PanaAC_v2_ESPHome`:

```bash
python3 test/run_full_test.py dev-env
python3 test/run_full_test.py setup-env
python3 test/run_full_test.py run --suite esphome.g1
python3 test/run_full_test.py run --suite esphome.g2 --mqtt-broker-mode external --esphome-device /dev/ttyUSB0
python3 test/run_full_test.py run --suite esphome.g3 --mqtt-broker-mode external --esphome-device /dev/ttyUSB0
```

## Environment setup from scratch

These steps assume a new developer is starting with an empty `HA/` workspace
and needs both the firmware and the HA-side integration test environment.

1. Create the workspace layout and clone the required repos:

   ```bash
   mkdir -p HA/ha HA/esphome
   cd HA/esphome
   git clone <ESPHome upstream or local fork> esphome
   git clone <PanaAC_v2_ESPHome remote> PanaAC_v2_ESPHome
   cd ../ha
   git clone https://github.com/home-assistant/core.git core
   git clone <PanaAC_v2_HA remote> PanaAC_v2_HA
   ```

2. Create the ESPHome development environment:

   ```bash
   cd HA/esphome/esphome
   python3 -m venv ../.venv
   ../.venv/bin/pip install -e .
   ```

   The test commands in this repo expect the CLI at
   `HA/esphome/.venv/bin/esphome`.

3. Install local utilities used by the runner:

   ```bash
   sudo apt-get update
   sudo apt-get install -y mosquitto mosquitto-clients
   sudo systemctl enable --now mosquitto
   ```

4. Decide the MQTT host address the device can reach:

   - From WSL or native Linux, the runner uses `127.0.0.1:1883`.
   - The ESP device must use a reachable LAN IP for the host running
     mosquitto. Replace the example host IP in the test YAMLs if your machine
     is not using the same address as the original lab.

5. Review the variant YAMLs under
   `HA/esphome/PanaAC_v2_ESPHome/test/variants/` and set the local secrets they
   need:

   - Wi-Fi SSID/password
   - MQTT username/password
   - MQTT broker host reachable by the device

6. Prepare the HA side for Group 2 and Group 3 cross-checks by following
   `HA/ha/PanaAC_v2_HA/test/test-execution.md`. The same topic prefix and MQTT
   credentials must be used on both sides.

7. Connect the DUT hardware:

   - ESP8266 d1_mini
   - IR receiver on GPIO14 / D5
   - IR LED on GPIO13 / D7
   - Panasonic AC unit in line of sight for the IR transmitter

8. Validate compile/config first, before flashing:

   ```bash
   cd HA/esphome/PanaAC_v2_ESPHome
   python3 test/run_full_test.py run --suite esphome.g1
   ```

9. Flash the DUT with the required test image:

   - use `test/variants/C3.yaml` for the baseline runtime suites
   - use `test/variants/C3-automation.yaml` for the automation suites

   Example:

   ```bash
   cd HA/esphome
   .venv/bin/esphome run PanaAC_v2_ESPHome/test/variants/C3-automation.yaml
   ```

## Prerequisites

- Workspace ESPHome venv with the esphome CLI built from the local platform
  source: `esphome/.venv/bin/esphome` (version 2026.8.x-dev). See
  `esphome/DEV_ENVIRONMENT.md`.
- The `panaac_v2` component is the local source under
  `esphome/PanaAC_v2_ESPHome/esphome/components/panaac_v2/` (referenced by the
  test YAMLs via `external_components`/`custom_components` — check the example
  YAML's `external_components` source path).
- A real ESP8266 (d1_mini) with IR receiver (GPIO14/D5) + IR LED (GPIO13/D7),
  pointed at a Panasonic AC. USB-serial or OTA access for flashing.
- Local mosquitto broker: `127.0.0.1:1883` from WSL, `mqtt_user`/`mqtt_pass`.
  Start it (sudo password `mnhmnh`): `echo 'mnhmnh' | sudo -S systemctl start
  mosquitto`. The device reaches the broker at the host LAN IP of the machine
  running mosquitto; verify that address locally and update the test YAML if
  needed. If you are using WSL with a Windows-hosted network stack, ensure the
  host firewall allows inbound TCP `1883`.
- A Home Assistant dev instance with the `PanaAC_v2_HA` integration configured
  for the same topic prefix (for the Group 2 cross-check). See the HA repo's
  `test/test-execution.md` to bring it up.

All `esphome` commands run from the workspace `esphome/` dir:
`cd esphome`.

## Runner config

Before HIL commands, create `test/runner_config.json` from `test/runner_config.example.json` and fill in the broker settings you want to reuse:

```json
{
  "mqtt": {
    "broker_mode": "external",
    "host": "127.0.0.1",
    "port": 1883,
    "user": "mqtt_user",
    "pass": "mqtt_pass"
  },
  "wifi": {
    "ssid": "YOUR_WIFI_SSID",
    "password": "YOUR_WIFI_PASSWORD",
    "ap_password": "YOUR_WIFI_AP_PASSWORD"
  }
}
```

When you use `python3 test/run_full_test.py menu`, the runner will also save prompted MQTT and Wi-Fi credentials into `test/runner_config.json`. Keep `broker_mode` as `external` when you want to target your existing DUT-facing broker. For `esphome.g2` and `esphome.g3`, the runner injects the `wifi` block into the temporary flash YAML and replaces `mqtt_broker` at flash time with the current workstation LAN IP. If the `wifi` block is missing, interactive runs prompt before starting and non-interactive runs fail early with a clear config error.

`dev-env`, `setup-env`, and compile-only runs can use a spawned isolated MQTT broker on a random localhost port. DUT-backed runtime suites still default to `external`, because the flashed device must already be connected to the broker under test. Those runtime suites now also clear retained topics under the test prefix and reflash the DUT with the required test image before execution unless you pass `--no-flush-mqtt` or `--no-flash-dut`.

## Automated runner entrypoints

From `esphome/PanaAC_v2_ESPHome`:

```bash
python3 test/run_full_test.py list
python3 test/run_full_test.py menu
python3 test/run_full_test.py dev-env
python3 test/run_full_test.py setup-env
python3 test/run_full_test.py run --suite esphome.g1
python3 test/run_full_test.py run --suite esphome.g2 --mqtt-broker-mode external --esphome-device /dev/ttyUSB0
python3 test/run_full_test.py run --suite esphome.g3 --mqtt-broker-mode external --esphome-device /dev/ttyUSB0
```

From the workspace root `HA/`, the same commands are:

```bash
cd esphome/PanaAC_v2_ESPHome
python3 test/run_full_test.py list
```

Suite meaning:

- `dev-env` validates the local ESPHome developer environment only. It defaults to a spawned isolated broker and does not need DUT credentials.
- `setup-env` validates the runner prerequisites and MQTT round-trip. It defaults to a spawned isolated broker.
- `esphome.g1` covers variant YAML config + compile and defaults to a spawned isolated broker.
- `esphome.g2` covers retained MQTT topics, command round-trip, malformed payloads, and atomic multi-field command behavior against the DUT. Use the external broker mode so the flashed device stays connected to the broker under test. Before these suites run, the runner clears retained topics under the test prefix and flashes `C3.yaml` for `esphome.g2` or `C3-automation.yaml` for `esphome.g3`.
- `esphome.g3` covers `climate.control`, lambda `make_call`, `on_state`, `on_control`, and debug-log-assisted DUT observations. It also expects the external broker mode for the live DUT path. The runner reflashes `C3-automation.yaml` before the suite unless you pass `--no-flash-dut`.

## Current automation status

- `esphome.g1`: automated and stable
- `esphome.g2`: automated and intended to run with the DUT flashed and connected
- `esphome.g3`: automated and intended to run with `C3-automation.yaml`

## Variant YAMLs (Group 1)

The variant YAMLs already exist at `test/variants/C1.yaml` …
`test/variants/C6.yaml`, plus `test/variants/C3-automation.yaml` for Group 3.
They were derived from `esphome/esphome-panaac-v2.yaml` and only change the
keys listed in `test-specification.md` §1.1.

For a config-only (no hardware) check you may drop `mqtt`/`wifi` pins, but keep
`remote_receiver`/`remote_transmitter` pin blocks so the component loads.

## Group 1 — Configuration combinations

Automated path:

```bash
python3 test/run_full_test.py run --suite esphome.g1
```

For each variant `V` in C1..C6:

```
.venv/bin/esphome config  PanaAC_v2_ESPHome/test/variants/V.yaml   # must exit 0
.venv/bin/esphome compile PanaAC_v2_ESPHome/test/variants/V.yaml   # must exit 0
```

Then verify traits/topics without flashing — use a compile + inspect the
generated `dump_config`, and for v2 variants start the firmware once on
hardware (or a stub) and read the retained `traits` from the broker:

```
mosquitto_sub -h 127.0.0.1 -u mqtt_user -P mqtt_pass -t 'panaac_v2/+/traits' -C 1
```

Compare the payload to `test-specification.md` §1.2 and §1.3.

Result C1: …  Result C2: …  Result C3: …  Result C4: …  Result C5: …  Result C6: …

## Flashing the device (needed for Groups 2 & 3)

Flash once with the variant **C3** reference config (the full-feature set):

```
.venv/bin/esphome run PanaAC_v2_ESPHome/test/variants/C3.yaml
```

`esphome run` compiles, then uploads over the chosen method (OTA if the device
is already on the network, else USB-serial via `--device /dev/ttyUSB0` — see
memory `wsl-windows-esptool-flash` for the WSL USB-serial path). Watch the
serial/log until `WiFi connected` and `MQTT connected`.

For the automation suites, flash the dedicated automation build:

```bash
.venv/bin/esphome run PanaAC_v2_ESPHome/test/variants/C3-automation.yaml
```

## Group 2 — Two-way MQTT with HA

Automated path:

```bash
python3 test/run_full_test.py run --suite esphome.g2 --mqtt-broker-mode external --esphome-device /dev/ttyUSB0
```

Subscribe to all DUT topics in one terminal (leave it running):

```
mosquitto_sub -h 127.0.0.1 -u mqtt_user -P mqtt_pass -t 'panaac_v2/esphome-panaac-v2/#' -v
```

### 2.1 Device → HA
After boot confirm retained messages arrive on `.../availability`,
`.../traits`, `.../state` matching §2.1. Result: …

### 2.2 HA → device round-trip
For each row in §2.2, publish the command and confirm the DUT republishes
`state` with the new value and (if HA is running) the HA climate entity
reflects it:

```
mosquitto_pub -h 127.0.0.1 -u mqtt_user -P mqtt_pass \
  -t panaac_v2/esphome-panaac-v2/set -m '{"mode":"cool"}'
```

To confirm the atomic-command guarantee (one IR transmit per multi-field
`set`), enable `DEBUG` logging on the DUT (`logger: level: DEBUG`) and count
`Transmitting`/IR-burst log lines after:
```
mosquitto_pub ... -m '{"mode":"cool","target_temperature":24,"fan_mode":"Level 2"}'
```
Expect exactly one transmit burst. Result: …

### 2.3 Unsupported / malformed input
Publish `{"preset":"ECO"}`, `{"target_humidity":50}`, `{not json`, and
`{"mode":"cool"}` on a `supports_cool=false` variant (C1); assert no IR
transmit and no state change (check the subscribe terminal + DUT log). Result: …

## Group 3 — ESPHome climate automation

Automated path:

```bash
python3 test/run_full_test.py run --suite esphome.g3 --mqtt-broker-mode external --esphome-device /dev/ttyUSB0
```

Add the following to a test build of the C3 config (e.g. an extra
`test/variants/C3-automation.yaml`) and flash it, so the on-device triggers
and lambdas are present.

```yaml
climate:
  - platform: panaac_v2
    id: panaac_v2_climate
    # ... full C3 config ...
    on_state:
      - logger.log: "on_state fired"
    on_control:
      - logger.log: "on_control fired"

button:
  - platform: template
    name: "Control cool 24C"
    on_press:
      - climate.control:
          id: panaac_v2_climate
          mode: COOL
          target_temperature: 24.0
          fan_mode: AUTO
          swing_mode: VERTICAL

  - platform: template
    name: "Lambda action log"
    on_press:
      - lambda: |-
          ESP_LOGI("test", "mode=%d action=%d cur=%.1f tgt=%.1f",
                   (int) id(panaac_v2_climate).mode,
                   (int) id(panaac_v2_climate).action,
                   id(panaac_v2_climate).current_temperature,
                   id(panaac_v2_climate).target_temperature);
```

### 3.1 `climate.control`
Press "Control cool 24C" (HA button entity or `esphome` API). Expect one IR
transmit and a `state` republish with `mode=cool, target_temperature=24,
fan_mode=Auto, swing_mode=Vertical`. Repeat for each param in §3.1. Result: …

### 3.2 / 3.3 `on_state` / `on_control`
Drive a change from HA MQTT `set`, from the "Control cool 24C" button, and
from a sensor update. Check the DUT log shows `on_control` then `on_state` for
control inputs, and `on_state` only for a temperature update. Result: …

### 3.4 / 3.6 Lambda accessors & make_call
Press "Lambda action log"; verify the logged `mode`/`action`/`cur`/`tgt`
match the entity. Add a second template button whose `on_press` is a
`lambda` using `make_call().perform()` (§3.6) and confirm it transmits like
`climate.control`. Result: …

### 3.5 Derived action
For each mode (set via HA `set` or `climate.control`), press "Lambda action
log" and assert `action` matches the table in §3.5. For AUTO: set
`mode=auto`, `target_temperature=24`; with the room sensor reading > 24
expect `COOLING`, < 24 expect `HEATING`, == 24 expect `IDLE`. To force the
room reading without hardware, temporarily bind `sensor` to a template
sensor you can set, or warm/cool the sensor. Result: …

## Finishing

- If any step fails, capture the DUT log (`logger` DEBUG) and the broker
  subscribe output, and record the actual vs expected under that step.
- Recommended artifacts to keep for each automated run:
  - output dir `report.md`
  - output dir `report.json`
  - any debug or capture logs under the output dir
- Commit results to this file on the repo branch you are using for the test
  run. Do not push unless asked.
