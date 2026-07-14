# PanaAC v2 ESPHome — test plan

Full-test plan for the `panaac_v2` ESPHome custom component. These documents
describe how to validate the firmware against a real device and a Home
Assistant instance.

Workspace layout is assumed to be relative to a portable `HA/` root:

```text
HA/
  ha/
    core/
    PanaAC_v2_HA/
  esphome/
    PanaAC_v2_ESPHome/
```

The plan is split into two documents:

- [`test-specification.md`](test-specification.md) — **what** to test: the test
  groups, inputs, expected behaviour, and pass/fail criteria. Read this to
  understand scope and to decide what is in/out.
- [`test-execution.md`](test-execution.md) — **how** to run it: prerequisites,
  exact commands (including a dev-environment-only validation path, spawned MQTT
  setup for compile-only checks, and flashing the device), example test YAML,
  and how to read results.
- [`run_full_test.py`](run_full_test.py) — entrypoint for the automation
  runner.
- [`automation_runner/data.py`](automation_runner/data.py) — static suite and
  variant expectations.
- [`automation_runner/core.py`](automation_runner/core.py) — test framework,
  environment validation, report generation, and suite execution.
- [`automation_runner/cli.py`](automation_runner/cli.py) — CLI and interactive
  menu for selecting suites and preparing the environment.

## Scope (three groups)

1. **Configuration YAML combinations** — every meaningful combination of the
   component's config items compiles and advertises the expected climate traits
   / companion selects / MQTT topics.
2. **Two-way MQTT with the Home Assistant side** — the device publishes
   `availability` / `traits` / `state` (retained) and applies commands on `set`;
   the HA `PanaAC_v2_HA` integration reflects them.
3. **ESPHome climate automation surface** — `climate.control` action,
   `on_state` / `on_control` triggers, and lambda state accessors (incl. the
   derived `action`) behave as documented on <https://esphome.io/components/climate/>.

## Device under test

- ESP8266 (d1_mini) with a Panasonic AC IR receiver (GPIO14/D5) and IR LED
  (GPIO13/D7). Reference config: `esphome/esphome-panaac-v2.yaml`.
- Climate id: `panaac_v2_climate`. v2 MQTT topic prefix: `panaac_v2/esphome-panaac-v2`.

## Status

Not yet executed. After execution, record results inline in
`test-execution.md` (each step has a "Result:" line) and commit to this branch.

## Runner config

Create `test/runner_config.json` for local MQTT settings. Start from `test/runner_config.example.json`:

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

The real `test/runner_config.json` is ignored by git. Interactive menu flows will save prompted MQTT credentials there automatically. Keep `broker_mode` as `external` when you want to target your existing DUT-facing broker.

Put the DUT Wi-Fi credentials in the `wifi` block of `test/runner_config.json`. During a DUT-backed runtime flash, the runner injects `wifi.ssid`, `wifi.password`, and `wifi.ap_password` into the temporary test YAML and still replaces `mqtt_broker` at flash time with the current workstation LAN IP. If a DUT-backed run is started without those Wi-Fi values, the interactive flow prompts for them before the test starts; non-interactive runs fail with a clear config error.

`setup-env` and compile-only runs can use a spawned isolated MQTT broker on a random localhost port. DUT-backed runtime suites still default to `external`, because the flashed device must already be connected to the broker under test. Those runtime suites also clear retained topics under the test prefix and reflash the DUT with the required test image before execution unless you pass `--no-flush-mqtt` or `--no-flash-dut`.

## Runner usage

- `python3 test/run_full_test.py list`
- `python3 test/run_full_test.py dev-env`
- `python3 test/run_full_test.py setup-env`
- `python3 test/run_full_test.py run --suite esphome.g1`
- `python3 test/run_full_test.py run --suite esphome.g2 --mqtt-broker-mode external --esphome-device /dev/ttyUSB0`
- `python3 test/run_full_test.py run --suite esphome.g3 --mqtt-broker-mode external --esphome-device /dev/ttyUSB0`
- `python3 test/run_full_test.py menu`

By default, `setup-env`, `dev-env`, and compile-only runs spawn an isolated MQTT broker on a random localhost port. Runs that include `esphome.g2` or `esphome.g3` keep the external-broker default so the flashed DUT can stay connected to the broker under test.
