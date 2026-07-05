# PanaAC v2 — Panasonic AC controller for ESPHome

Custom ESPHome component that drives a Panasonic AC over infrared and exposes
all controls via MQTT. It intentionally **does not use the ESPHome climate
platform or `climate_ir`**, so the companion Home Assistant integration can show
the full Panasonic fan levels, vertical swing positions, and a separate
horizontal swing axis on a single climate card.

## Wiring

Default device YAML assumes a Wemos D1 mini / ESP8266 with non-invasive wiring:

- **GPIO14 (D5)** — IR receiver (TSOP38xxx) data pin, inverted
- **GPIO13 (D7)** — IR LED anode via transistor

## Component topics

With `topic_prefix: panaac_v2/esphome-panaac-v2`:

| Direction | Topic | Retained | Payload |
|-----------|-------|----------|---------|
| Publish | `panaac_v2/esphome-panaac-v2/availability` | yes | `online` / `offline` |
| Publish | `panaac_v2/esphome-panaac-v2/traits` | yes | JSON supported modes |
| Publish | `panaac_v2/esphome-panaac-v2/state` | no | JSON current state |
| Subscribe | `panaac_v2/esphome-panaac-v2/set` | — | JSON command (partial) |

## State / command JSON

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

## Device YAML

See [`esphome/esphome-panaac-v2.yaml`](esphome/esphome-panaac-v2.yaml). Minimum
required secrets in your `secrets.yaml`:

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
  __init__.py      — ESPHome config schema and codegen
  panaac_v2.h      — component class declaration
  panaac_v2.cpp    — MQTT pub/sub + Panasonic IR encode/decode
  definitions.h    — IR constants, enums, string helpers
```

## Notes

- The component uses `mqtt::CustomMQTTDevice`, `remote_base::RemoteTransmittable`,
  and `remote_base::RemoteReceiverListener` directly.
- No climate platform is created; all control flows through MQTT.
- The device does not transmit IR on boot (the AC must not be commanded just
  because the ESP restarted). It only publishes traits/availability/state.

## More documentation

- [`DESIGN.md`](DESIGN.md) — architecture, MQTT topic contract, IR protocol.
- [`INSTALL.md`](INSTALL.md) — step-by-step hardware, compile, flash and verify.
