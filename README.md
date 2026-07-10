# PanaAC v2 — Panasonic AC controller for ESPHome

Custom ESPHome component that drives a Panasonic AC over infrared. It runs in **two modes**,
selected by whether you set `topic_prefix` on the `panaac_v2:` block:

- **v1 native mode** (`topic_prefix` omitted) — behaves like the original
  [`PanaAC_ESPHome`](https://github.com/hoangminh1109/PanaAC_ESPHome): a native `climate`
  entity (named `"<name> (PanaAC v1)"`) whose **Fan Mode** offers the full Panasonic fan levels
  (Auto / Level 1…5 / Quiet) as custom fan modes, plus **two companion `select` entities** —
  Swing Vertical, Swing Horizontal — for the granular swing positions. Exposed via the ESPHome
  native API (`api:`) and/or standard MQTT discovery. **No MQTT broker is required** — all v2
  MQTT code is compiled out (`USE_MQTT` undefined). The climate + selects sit at the root of the
  ESPHome device, exactly like PanaAC_ESPHome (optionally regroup them under a sub-device via
  `device_id` — [issue #15](https://github.com/hoangminh1109/PanaAC_ESPHome/issues/15)).
- **v2 MQTT mode** (`topic_prefix` set, requires a `mqtt:` block) — the original v2 behaviour:
  the full-featured climate is exposed over custom `<prefix>/state|traits|availability|set`
  MQTT JSON topics consumed by the
  [PanaAC v2 HA custom integration](https://github.com/hoangminh1109/PanaAC_v2_HA), showing
  every fan level / swing position / horizontal-swing axis on a single climate card. The same
  on-device `"<name> (PanaAC v1)"` climate + two `(PanaAC v1)` Swing V/H selects are **also kept
  visible** on the native API, at the root of the ESPHome device — exactly like PanaAC v1 (the
  v2 HA integration's climate is a separate device).

The IR encode/decode core and the canonical `ac_state` are shared between both modes.

## Wiring

Default device YAML assumes a Wemos D1 mini / ESP8266 with non-invasive wiring:

- **GPIO14 (D5)** — IR receiver (TSOP38xxx) data pin, inverted
- **GPIO13 (D7)** — IR LED anode via transistor (38 kHz carrier when `ir_control: true`)

## The `(PanaAC v1)` selects

Created in **both** modes (named with a `(PanaAC v1)` suffix). Fan levels are **not** a select —
they are the climate's **Fan Mode** (custom fan modes: Auto / Level 1…5 / Quiet). Only the swing
positions need selects (the standard Climate swing enum can't represent them):

| Select | Options |
|--------|---------|
| Swing Vertical (PanaAC v1) | Auto, Highest, High, Middle, Low, Lowest |
| Swing Horizontal (PanaAC v1) | Auto, Left Max, Left, Middle, Right, Right Max (when `swing_horizontal`) |

By default the climate and the two selects sit at the root of the ESPHome device (no
sub-device), exactly like PanaAC_ESPHome. Optionally set `device_id` on the `panaac_v2:` block
(and define the sub-device under `esphome.devices`) to regroup them under a named sub-device
in the ESPHome WebUI / Home Assistant (issue #15).

## v2 MQTT topics

With `topic_prefix: panaac_v2/esphome-panaac-v2`:

| Direction | Topic | Retained | Payload |
|-----------|-------|----------|---------|
| Publish | `panaac_v2/esphome-panaac-v2/availability` | yes | `online` / `offline` |
| Publish | `panaac_v2/esphome-panaac-v2/traits` | yes | JSON supported modes |
| Publish | `panaac_v2/esphome-panaac-v2/state` | yes | JSON current state |
| Subscribe | `panaac_v2/esphome-panaac-v2/set` | — | JSON command (partial) |

`availability` uses MQTT birth/last-will **and** a `shutdown_message` so HA sees `offline` on
graceful ESPHome restarts too (not only hard crashes). `state` and `traits` are retained so HA
restores the entity immediately after an HA restart.

## State / command JSON (v2 mode)

**State:**
```json
{
  "mode": "cool",
  "target_temperature": 24.0,
  "fan_mode": "Level 2",
  "swing_mode": "Middle",
  "swing_horizontal_mode": "Right",
  "current_temperature": 26.5,
  "available": true
}
```

**Command** (any subset of fields):
```json
{"fan_mode": "Level 2"}
```

## Configuration keys

| Key | Default | Effect |
|-----|---------|--------|
| `topic_prefix` | _(unset → v1 mode)_ | Set to enable v2 MQTT mode. |
| `hide_legacy_comps` | false | v2 mode only: make the on-device `(PanaAC v1)` climate + Swing V/H selects `internal` so they are hidden from Home Assistant (the v2 climate card comes from the HA custom integration over MQTT). No effect in v1 mode — the climate + selects stay visible. |
| `receiver_id` / `transmitter_id` | required | The `remote_receiver` / `remote_transmitter` ids. |
| `supports_cool` / `supports_heat` / `supports_fan_only` | true/false | Advertise those HVAC modes. |
| `supports_quiet` | false | Add the Quiet fan level. |
| `fan_5level` | false | 5 fan levels (Level 1…5) vs 3 (Level 1/3/5). |
| `swing_horizontal` | false | Enable horizontal swing + the SwingH select. |
| `temp_step` | 1.0 | Visual temperature step (0.5 or 1.0). |
| `ir_control` | false | `true` = real IR LED (38 kHz carrier); `false` = direct-wired. |
| `sensor` | _(none)_ | Current-temperature sensor. |
| `device_id` | _(none)_ | Sub-device id for issue #15 grouping of the selects. |

## Example configs

- [`esphome/esphome-panaac-v2.yaml`](esphome/esphome-panaac-v2.yaml) — **v2 MQTT mode**
  (`topic_prefix` + `mqtt:` + birth/will/shutdown_message + `api:` for the selects).
- [`esphome/esphome-panaac-v2-v1mode.yaml`](esphome/esphome-panaac-v2-v1mode.yaml) — **v1
  native mode** (`api:`, no `topic_prefix`, no `mqtt:` required, `device_id` grouping demo).

Required secrets (v2 mode) in your `secrets.yaml`:
```yaml
wifi_ssid: "..."
wifi_password: "..."
wifi_ap_password: "..."
mqtt_broker: "..."
mqtt_user: "..."
mqtt_pass: "..."
```

## Files

```
esphome/components/panaac_v2/
  __init__.py      — ESPHome config schema + codegen (mode switch, selects, device_id grouping)
  panaac_v2.h      — component class declaration
  panaac_v2.cpp    — climate logic, Panasonic IR encode/decode, v2 MQTT pub/sub (#ifdef USE_MQTT)
  extra.h/.cpp     — the two companion Swing V/H select entities
  definitions.h    — IR constants, enums, ClimateState struct, string helpers
```

## Notes

- Both modes expose the full fan levels (Auto / Level 1…5 / Quiet) as the climate's Fan Mode
  (custom fan modes) — no lossy standard-only enum, no separate Fan Level select. v2 mode
  additionally publishes the same levels over the custom MQTT topics for the all-in-one HA card.
- The device does not transmit IR on boot (the AC must not be commanded just because the ESP
  restarted); it only restores/publishes state.
- Driving a select in either mode transmits one IR frame and re-publishes state (MQTT in v2
  mode, native in v1 mode), so the climate card and the selects stay in sync.

## More documentation

- [`DESIGN.md`](DESIGN.md) — architecture, MQTT topic contract, IR protocol.
- [`INSTALL.md`](INSTALL.md) — step-by-step hardware, compile, flash and verify.