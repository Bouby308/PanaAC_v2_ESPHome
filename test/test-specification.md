# PanaAC v2 ESPHome — test specification

Authoritative description of **what** the tests check. See
`test-execution.md` for how to run them. Firmware reference: the active repo
branch under test.

## Conventions

- "DUT" = device under test (the ESP8266 running the `panaac_v2` firmware).
- "Broker" = the local mosquitto broker (`127.0.0.1:1883` from HA/WSL,
  `10.105.1.86:1883` from the device; `mqtt_user` / `mqtt_pass`).
- Topic prefix `<p>` = `panaac_v2/esphome-panaac-v2` (override per config).
- "v2 mode" = MQTT mode, enabled by setting `topic_prefix`. "v1 mode" = native
  API mode (no `topic_prefix`).
- Pass = every asserted expectation in a step holds. Fail = any expectation
  breaks; record the actual value.

## Config items under test (`panaac_v2` schema)

| Key | Default | Domain | Effect |
|-----|--------|--------|--------|
| `receiver_id` | required | remote receiver id | IR receive |
| `transmitter_id` | required | remote transmitter id | IR transmit |
| `topic_prefix` | absent | string | present ⇒ v2 MQTT mode |
| `hide_legacy_comps` | false | bool | hide legacy fan/swing selects in v2 |
| `supports_cool` | true | bool | advertise `cool` mode |
| `supports_heat` | false | bool | advertise `heat` mode |
| `supports_fan_only` | false | bool | advertise `fan_only` mode |
| `supports_quiet` | false | bool | advertise `Quiet` fan enum |
| `fan_5level` | false | bool | 5 fan levels (else 3: L1/L3/L5) |
| `swing_horizontal` | false | bool | advertise horizontal swing axis + select |
| `temp_step` | 1.0 | float 0.5–1.0 | target temperature step |
| `ir_control` | false | bool | 38 kHz IR carrier control |
| `sensor` | absent | sensor id | current-temperature source |

Always advertised: `off`, `dry`, `auto` modes; `Auto` fan; `Off`/`Vertical`
swing; companion `Swing V` select. `Cool`/`Heat`/`Fan_only`/`Quiet`/5-level/
horizontal are opt-in.

---

## Group 1 — Configuration YAML combinations

**Goal:** every meaningful combination validates and, after compile, advertises
exactly the expected traits / selects / topics.

**Method:** `esphome config <yaml>` (validation) and `esphome compile <yaml>`
(build) for each variant; inspect `dump_config` output and, in v2 mode, the
retained `traits` payload.

### 1.1 Validation matrix

For each row: `esphome config` must succeed; `esphome compile` must succeed;
record flash/RAM. Variants are built from the reference YAML by toggling only
the listed keys.

| ID | topic_prefix | supports_* | fan_5level | swing_horizontal | temp_step | sensor |
|----|--------------|------------|------------|------------------|-----------|--------|
| C1 | set (v2)     | cool only  | false (3L) | false            | 1.0       | none   |
| C2 | set (v2)     | cool+heat  | false      | false            | 0.5       | none   |
| C3 | set (v2)     | cool+heat+fan_only+quiet | true (5L) | true | 0.5 | room_temp |
| C4 | absent (v1)  | cool+heat  | true       | true             | 0.5       | room_temp |
| C5 | set (v2)     | cool       | true       | true             | 1.0       | room_temp |
| C6 | set (v2)     | cool+heat+quiet | false | false        | 0.5       | none   |

### 1.2 Expected traits per variant

For each variant, the device's `traits` payload (v2) / native traits (v1) must
contain exactly:

- **HVAC modes**: `off`, `dry`, `auto`, plus each `supports_*` that is true.
  `heat_cool` is never advertised.
- **Fan modes**: standard `Auto`, plus `Quiet` only if `supports_quiet` is true.
  Custom fan strings: `fan_5level=false` ⇒ `Level 1`, `Level 3`, `Level 5`;
  `fan_5level=true` ⇒ `Level 1`–`Level 5`. No duplicate custom/enum entries.
- **Swing modes**: `Off`, `Vertical`; plus `Horizontal`, `Both` only if
  `swing_horizontal` is true.
- **Companion selects**: `Swing V` always; `Swing H` only if
  `swing_horizontal` is true. `hide_legacy_comps=true` in v2 hides the legacy
  fan/swing selects (the climate card carries them instead).
- **Temperature**: min 16, max 30, step = `temp_step`. `current_temperature`
  trait present only if `sensor` is set.

### 1.3 Expected topics / mode selection

- v2 (any variant with `topic_prefix`): the component subscribes to
  `<p>/set` and publishes `<p>/availability`, `<p>/traits`, `<p>/state`
  (retained). The standard ESPHome MQTT climate component is NOT also published
  (`CONF_MQTT_ID` is dropped).
- v1 (no `topic_prefix`): native API only; no custom MQTT topics; the climate
  entity name is suffixed ` (v1)` and is `internal` (hidden from HA native
  discovery) so it does not duplicate the v2 entity.

**Pass:** every variant in 1.1 compiles; 1.2 traits and 1.3 topics/mode match
for each.

---

## Group 2 — Two-way MQTT with the Home Assistant side (v2 mode)

**Goal:** the DUT and the HA `PanaAC_v2_HA` integration exchange state/commands
correctly over the four MQTT topics. Run with variant **C3** (the reference).

### 2.1 Device → HA (publish)

- On connect, the DUT publishes retained `availability` = `online` (and a
  `last-will`/`shutdown` = `offline` is registered so a crash/broker restart
  surfaces as unavailable).
- On connect (and on reconnect), the DUT publishes retained `traits` with the
  keys from §1.2 (variant C3) plus `min_temp`, `max_temp`, `temp_step`,
  `temperature_unit: "C"`.
- The DUT publishes `state` (retained) after every command, IR decode, or
  current-temperature sensor update, with keys `mode`, `target_temperature`,
  `fan_mode`, `swing_mode`, `swing_horizontal_mode` (only if the axis is
  enabled), `current_temperature` (only if `sensor` set), `available: true`.
  `fan_mode` is `Auto`/`Quiet` (enum) or `Level N` (custom).

### 2.2 HA → device (command round-trip)

Publish each partial JSON to `<p>/set` and assert the DUT applies it (IR
transmit + state republish) and the HA climate entity reflects the new value:

| `set` payload field | Valid values | Asserted effect |
|---------------------|--------------|-----------------|
| `mode` | `off`/`cool`/`heat`/`fan_only`/`dry`/`auto` | mode changes; state republish has new `mode` |
| `target_temperature` | 16–30 (clamped) | target changes; out-of-range clamps to 16/30 |
| `fan_mode` | `Auto`/`Quiet`/`Level 1`–`5` | fan level changes; `Quiet` rejected if `supports_quiet=false` |
| `swing_mode` | `Auto`/`Highest`/`High`/`Middle`/`Low`/`Lowest` | vertical position changes |
| `swing_horizontal_mode` | `Auto`/`Left Max`/`Left`/`Middle`/`Right`/`Right Max` | horizontal position changes (only if axis enabled) |

A single `set` message carrying multiple fields must result in exactly **one**
IR transmit (not one per field) — the atomic-command guarantee.

### 2.3 Unsupported / malformed input

- `set` with `target_humidity`, `preset`, `custom_preset`,
  `target_temperature_low/high`, `custom_fan_mode` (non-Panasonic strings) is
  ignored (no IR transmit, no state change).
- Malformed JSON on `set` is logged and ignored.
- A `mode` the device does not support (e.g. `cool` when `supports_cool=false`)
  is rejected with a warning and clamped to `off`.

**Pass:** every round-trip in 2.2 reflects correctly in HA; 2.3 inputs are
no-ops; availability/traits/state in 2.1 match the schema and retain flags.

---

## Group 3 — ESPHome climate automation surface

**Goal:** the ESPHome-side automation features documented at
<https://esphome.io/components/climate/#climate-automation> work on the DUT.
Run with variant **C3** (so all features are present).

### 3.1 `climate.control` action

Call `climate.control` (from a `button`, `on_boot`, or an ESPHome automation)
on `id(panaac_v2_climate)` and assert the DUT transmits IR and republishes
state, for each parameter that applies:

- `mode` (OFF/COOL/HEAT/FAN_ONLY/DRY/AUTO) → mode + action update.
- `target_temperature` → target update (clamped 16–30).
- `fan_mode` (ON/OFF/AUTO/LOW/MEDIUM/HIGH/MIDDLE/FOCUS/DIFFUSE/QUIET) → maps to
  the nearest Panasonic level; QUIET only if `supports_quiet`.
- `custom_fan_mode` (`Level 1`–`5`) → that level directly.
- `swing_mode` (OFF/BOTH/VERTICAL/HORIZONTAL) → combined swing; HORIZONTAL/BOTH
  rejected (clamped) if `swing_horizontal=false`.

Unsupported params — `target_temperature_low/high`, `target_humidity`,
`preset`, `custom_preset` — must be **ignored** (no-op, no error).

### 3.2 `climate.on_state` trigger

Fires every time the climate state is published: after a command (from HA MQTT
`set`, from `climate.control`, from a lambda `make_call().perform()`), after an
IR-decode of the physical remote, and after a current-temperature sensor
update. Assert via an `on_state` automation that logs / sets a helper.

### 3.3 `climate.on_control` trigger

Fires before `on_state` whenever a `ClimateCall` is performed — including a
command arriving from Home Assistant over MQTT `set` and a local
`climate.control` / `make_call().perform()`. It does **not** fire on
temperature-only sensor updates. Assert via an `on_control` automation.

### 3.4 Lambda state accessors

In a lambda, every documented read-only accessor returns the current value:

- `id(panaac_v2_climate).mode` — `ClimateMode`
- `id(panaac_v2_climate).current_temperature` — float (only meaningful if
  `sensor` set; else NaN)
- `id(panaac_v2_climate).target_temperature` — float
- `id(panaac_v2_climate).action` — `ClimateAction`, **derived** from the mode
  (see 3.5)
- `id(panaac_v2_climate).fan_mode` / `get_custom_fan_mode()` /
  `has_custom_fan_mode()` — enum for Auto/Quiet, custom string for levels
- `id(panaac_v2_climate).swing_mode` / `get_custom_swing_mode()` /
  `get_swing_horizontal_mode()`

### 3.5 Derived `action`

`update_action_()` sets `Climate::action` from the commanded mode (one-way IR:
assume commanded state = real state):

| Mode | action |
|------|--------|
| OFF | `CLIMATE_ACTION_OFF` |
| COOL | `CLIMATE_ACTION_COOLING` |
| HEAT | `CLIMATE_ACTION_HEATING` |
| DRY | `CLIMATE_ACTION_DRYING` |
| FAN_ONLY | `CLIMATE_ACTION_FAN` |
| AUTO | `COOLING` if room>setpoint, `HEATING` if room<setpoint, else `IDLE` |

Assert via a lambda logging `id(panaac_v2_climate).action` after setting each
mode; for AUTO, vary `current_temperature` (heat/cool the sensor or fake it)
and assert the action flips COOLING/HEATING/IDLE with the setpoint.

### 3.6 `make_call` from a lambda

```
auto call = id(panaac_v2_climate).make_call();
call.set_mode("COOL");
call.set_target_temperature(24.0f);
call.perform();   // fires on_control, then on_state
```
Assert it behaves like `climate.control` (IR transmit + state republish).

**Pass:** every action/trigger/lambda assertion holds; unsupported params are
no-ops; `action` matches the table for all modes incl. AUTO temp-inference.
