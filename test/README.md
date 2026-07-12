# PanaAC v2 ESPHome — test plan

Full-test plan for the `panaac_v2` ESPHome custom component. This branch
(`testing/full-test`) holds the plan only — the firmware source is unchanged
from `main`. A later agent (human or AI) executes the plan against a real
device and a Home Assistant instance.

The plan is split into two documents:

- [`test-specification.md`](test-specification.md) — **what** to test: the test
  groups, inputs, expected behaviour, and pass/fail criteria. Read this to
  understand scope and to decide what is in/out.
- [`test-execution.md`](test-execution.md) — **how** to run it: prerequisites,
  exact commands (incl. flashing the device), example test YAML, and how to
  read results.
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

## Runner usage

- `python3 test/run_full_test.py list`
- `python3 test/run_full_test.py setup-env --mqtt-user mqtt_user --mqtt-pass mqtt_pass`
- `python3 test/run_full_test.py run --suite esphome.g1 --mqtt-user mqtt_user --mqtt-pass mqtt_pass`
- `python3 test/run_full_test.py menu`
