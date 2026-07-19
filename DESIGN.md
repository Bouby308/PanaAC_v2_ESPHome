# PanaAC v2 — Design document

## Two modes (v1-compatibility branch)

This branch unifies PanaAC v1 and v2 behind one component, switched by `topic_prefix`:

- **v1 native mode** (`topic_prefix` unset, `USE_MQTT` undefined): native `climate` whose Fan Mode
  carries the full Panasonic fan levels (Auto / Level 1…5 / Quiet) as custom fan modes (named
  `"<name> (v1)"`) + a Swing Vertical `select` and, when enabled, a Swing Horizontal `select` over the
  ESPHome native API / standard MQTT discovery. No broker required. Climate + companion selects sit at the
  root of the ESPHome device, like PanaAC v1.
- **v2 MQTT mode** (`topic_prefix` set, `USE_MQTT` defined): the full-featured v2 climate is
  exposed over the custom `<prefix>/...` MQTT JSON topics (PanaAC v2 HA custom integration,
  single card). The same on-device `"<name> (v1)"` climate + companion swing
  selects are ALSO kept visible on the native API, at the root of the ESPHome device — exactly
  like PanaAC v1. The native climate carries the standard swing modes (Off/Vertical/Horizontal/
  Both) so its card looks/behaves like PanaAC v1; the granular swing POSITIONS still live on the
  Swing V/H selects / the v2 MQTT topics. The custom MQTT topics are independent of the native
  `ClimateTraits` (`publish_traits_()` is hand-rolled JSON), so the v2 HA card is unaffected.

A canonical `ClimateState ac_state` (mode/temp/fan_level/fan_mode/swing_mode/swing_v_pos/
swing_h_pos/preset/last_swing_*) is the single source of truth in both modes. The Climate base fields
(and, in v2 mode, the custom fan/swing strings) are derived from it via `sync_to_climate_()`.
All v2 MQTT code is wrapped in `#ifdef USE_MQTT` so v1-mode builds link without the mqtt
component. See [`README.md`](README.md) for the user-facing summary.

## Goal

Provide a Panasonic AC remote controller on an ESP8266 that can be controlled
from Home Assistant as a single native climate entity, exposing:

- full Panasonic fan levels (`Auto`, `Level 1` … `Level 5`, `Quiet`);
- vertical swing positions (`Auto`, `Highest`, `High`, `Middle`, `Low`, `Lowest`);
- a separate horizontal swing axis (`Auto`, `Left Max`, `Left`, `Middle`, `Right`, `Right Max`).

The component still exchanges control and state over the custom MQTT topics so
that the PanaAC v2 Home Assistant custom integration can expose arbitrary Panasonic
fan and swing strings.  In addition, `PanaACV2Climate` inherits from the ESPHome
`climate::Climate` base class, which gives it the standard climate automation
surface (`id(ac).make_call()`, `on_state`, `on_control`, lambda accessors, ...) while
using Climate's custom fan/swing mode lists to represent the arbitrary strings.

## Architecture

```
┌──────────────────────────────────────┐
│  Home Assistant                      │
│  ┌────────────────────────────────┐  │
│  │ custom integration panaac_v2   │  │
│  │   one ClimateEntity            │  │
│  └──────────────┬─────────────────┘  │
│                 │ MQTT publish/subscribe
│  ┌──────────────┴─────────────────┐  │
│  │ built-in MQTT integration      │  │
│  └────────────────────────────────┘  │
└──────────────────────────────────────┘
                │
                │ MQTT broker
                │
┌──────────────────────────────────────┐
│  ESP8266 (D1 mini)                   │
│  ┌────────────────────────────────┐  │
│  │ panaac_v2 custom component     │  │
│  │   climate::Climate            │  │
│  │   Component                   │  │
│  │   mqtt::CustomMQTTDevice      │  │
│  │   RemoteReceiverListener      │  │
│  │   RemoteTransmittable         │  │
│  └──────────────┬─────────────────┘  │
│                 │
│  ┌──────────────┴─────────────────┐  │
│  │ remote_receiver / transmitter │  │
│  └──────────────┬─────────────────┘  │
│                 │ 38 kHz IR
│        ┌────────┴────────┐           │
│     IR LED          IR receiver       │
│   (GPIO13)          (GPIO14)          │
└──────────────────────────────────────┘
```

## MQTT topic design

Topic prefix is configurable; the default is `panaac_v2/esphome-panaac-v2`.

| Topic suffix | Direction | Retained | Purpose |
|--------------|-----------|----------|---------|
| `availability` | device → HA | yes | `online` / `offline` LWT-style |
| `traits` | device → HA | yes | device capabilities (JSON) |
| `state` | device → HA | yes | current state (JSON) |
| `set` | HA → device | no | partial command (JSON) |

### Retained topics

`traits`, `availability` and `state` are retained so that Home Assistant can build
the climate card and restore the entity's current state immediately after startup
— including after an HA restart or a broker reconnect — before any live state
message arrives. The device republishes the retained `traits` and `state` on
every MQTT reconnect (see Startup behaviour) so retained-message loss or a broker
restart cannot leave HA with a stale or empty entity.

### Traits payload

```json
{
  "hvac_modes": ["off", "cool", "heat", "fan_only", "dry", "auto"],
  "fan_modes": ["Auto", "Level 1", "Level 2", "Level 3", "Level 4", "Level 5", "Quiet", "Powerful"],
  "swing_modes": ["Auto", "Highest", "High", "Middle", "Low", "Lowest"],
  "swing_horizontal_modes": ["Auto", "Left Max", "Left", "Middle", "Right", "Right Max"],
  "preset_modes": ["None", "Powerful", "Eco"],
  "min_temp": 16,
  "max_temp": 30,
  "temp_step": 0.5,
  "temperature_unit": "C"
}
```

### State payload

```json
{
  "mode": "cool",
  "target_temperature": 24.0,
  "fan_mode": "Level 2",
  "swing_mode": "Middle",
  "swing_horizontal_mode": "Right",
  "preset_mode": "Powerful",
  "current_temperature": 26.5,
  "available": true
}
```

### Command payload

Commands are **partial**: any subset of the state keys may be supplied. Only the
keys that are present are applied; the rest of the internal state is preserved.

```json
{"fan_mode": "Level 2"}
```

Preset commands use the canonical Panasonic names and are validated against the configured capabilities and HVAC mode:

```json
{"preset_mode": "Eco"}
```

The native ESPHome climate exposes built-in `NONE` (`None`), `BOOST` (`Boost`), and `ECO` (`Eco`) presets. The v2 MQTT interface uses `None`, `Powerful`, and `Eco`; `Powerful` maps to native `BOOST` and IR byte 13 bit 0, while `Eco` maps to native `ECO` and IR byte 17 bit 4. `None` clears either preset.

## C++ class design

`PanaACV2Climate` inherits:
- `esphome::climate::Climate` — standard climate entity interface, `make_call()`,
  `publish_state()`, `on_state`/`on_control` callbacks, lambda accessors;
- `esphome::Component` — lifecycle hooks (`setup`, `loop`, `dump_config`);
- `esphome::mqtt::CustomMQTTDevice` — `publish`, `publish_json`, `subscribe_json`;
- `esphome::remote_base::RemoteReceiverListener` — `on_receive(...)` for IR decoding;
- `esphome::remote_base::RemoteTransmittable` — `transmitter_->transmit()` for IR output.

The climate base class stores the public state (`mode`, `target_temperature`,
`current_temperature`, `custom_fan_mode`, `custom_swing_mode`,
`swing_horizontal_mode`).  These are mapped to/from the Panasonic IR byte values
from `definitions.h`, while the custom MQTT strings are derived from the canonical positions when transmitting or decoding.
from `definitions.h`, so their lifetime matches the firmware.

The `last_swing_*` fields retain the most recently selected fixed positions for canonical state continuity; they are not exposed as separate climate-base accessors.nnKey remaining internal state:
- `last_swing_v_pos_`, `last_swing_h_pos_` — retained canonical positions used when the component state is synchronized.
  command can fall back to a previously selected fixed position.

## IR protocol

The component reuses the Panasonic AC IR packet layout already validated in the
v1 component:

- 8-byte fixed first frame (`02 20 E0 04 00 00 00 06`);
- 19-byte variable second frame carrying mode, power, temperature, fan, vertical
  swing, horizontal swing and checksum.

Encoding and decoding follow the byte positions and masks defined in
`definitions.h`:

- power/mode byte 5;
- temperature byte 6;
- fan and vertical-swing byte 8;
- horizontal-swing byte 9;
- quiet bit in byte 13.

The carrier is 38 kHz when `ir_control: true`.

## Startup behaviour

On `setup()` the component:
1. subscribes to the `set` topic;
2. waits for MQTT to be connected in `loop()`;
3. publishes retained `availability = online`;
4. publishes retained `traits`;
5. publishes a retained `state`.

It does **not** transmit IR on boot, so a restart cannot unexpectedly turn the
AC off or change its settings.

`loop()` tracks the MQTT connection: while disconnected it marks the retained
bootstrap as due, and on every reconnect it republishes the retained `traits`
and `state` (not only on the first boot-time connect). This keeps Home Assistant
able to rebuild the entity after a broker restart, retained-message loss, or a
late device reconnect.

If a `sensor` is configured, its filtered state updates are copied into
`current_temperature` and published in the next state message.

## Climate automation support

Because the component inherits from `climate::Climate`, the standard ESPHome
climate automation features work:

- **Lambda calls** from any ESPHome automation:

  ```cpp
  auto call = id(panaac_v2_climate).make_call();
  call.set_mode("COOL");
  call.set_target_temperature(25.0);
  call.set_fan_mode("Level 2");
  call.set_swing_mode("BOTH");
  // Use the Swing Vertical/Horizontal select entities for granular positions.
  call.perform();
  ```

- **State accessors** in lambdas:

  ```cpp
  id(panaac_v2_climate).mode
  id(panaac_v2_climate).target_temperature
  id(panaac_v2_climate).current_temperature
  id(panaac_v2_climate).action
  id(panaac_v2_climate).get_custom_fan_mode()
  ```

  `action` (`ClimateAction`) is *derived* from the commanded mode by
  `update_action_()` — the controller is a one-way IR transmitter and cannot
  read back whether the compressor is actually running, so it assumes the
  commanded mode is the unit's real state:

  | Commanded mode | `action` |
  |----------------|----------|
  | `OFF`          | `OFF`    |
  | `COOL`         | `COOLING`|
  | `HEAT`         | `HEATING`|
  | `DRY`          | `DRYING` |
  | `FAN_ONLY`     | `FAN`    |
  | `AUTO`         | `COOLING` if room > setpoint, `HEATING` if room < setpoint, else `IDLE` |

  `update_action_()` runs at the end of `sync_to_climate_()` (so every state
  publish carries a fresh action) and on the current-temperature sensor callback
  (so the `AUTO` inference tracks the room temperature). The native-API climate
  entity therefore reports the same action the lambda sees.

- **`on_state` trigger** fires every time the climate state is published (for
  example after a command, an IR decode, or a sensor update).

- **`on_control` trigger** fires before `on_state`, whenever a control call
  is performed (including commands from Home Assistant).

The custom MQTT topics and JSON payloads remain unchanged, so the existing
PanaAC v2 Home Assistant custom integration continues to work.

## Command handling

`on_set_json_()` is invoked for every message on `.../set`. It checks for the
presence of each supported key, validates the value against the configured
capabilities (e.g. reject `heat` if `supports_heat: false`), updates the internal
state, then calls `transmit_()` to send the corresponding IR packet and
`publish_state_()` to report the new state.

## Decoding received IR

`on_receive()` receives raw timings from `remote_receiver`. `decode_data_()`
converts timings to bytes; `decode_and_apply_()` validates the Panasonic protocol,
checksum, and mode support, then updates the internal state. A successful decode
triggers `publish_state_()` so that Home Assistant reflects a command sent from
a physical remote.

## Safety / lifetime

- All custom-mode strings are stored as `const char*` literals from `definitions.h`,
  not heap copies, so pointers remain valid for the lifetime of the device.
- The component does not allocate per-message; JSON payloads are built on
  ESPHome’s reusable ArduinoJson buffer.
- IR transmit uses the reusable `RemoteTransmitterBase::TransmitCall` pattern.

## Future extensions

Possible additions that fit the same MQTT contract:
- power-consumption sensor via separate topic;
- discovery using HA MQTT discovery instead of a custom integration.
