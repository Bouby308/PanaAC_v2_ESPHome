# Codex Review

## Issue 1: v2 MQTT traits/state are only published once per boot

**Problem**

In v2 MQTT mode, the device publishes `traits` and the initial `state` only once after the first successful MQTT connection. If the broker restarts, retained messages are lost, or the device disconnects and reconnects later, Home Assistant can come back without the retained capability/state payloads it needs to rebuild the entity correctly.

**Technical root cause**

`PanaACV2Climate::loop()` gates the v2 bootstrap publish behind `traits_published_`, and that flag is set to `true` permanently after the first publish sequence. There is no code path that resets it on MQTT disconnect/reconnect, so the bootstrap publish never runs again during the lifetime of the firmware process.

Relevant code:

- `esphome/components/panaac_v2/panaac_v2.cpp:192`
- `esphome/components/panaac_v2/panaac_v2.h:153`

**Proposed fix**

Republish the retained bootstrap payloads on every MQTT reconnect, not only on the first boot-time connect. The cleanest fix is to track connection edges or register an MQTT reconnect callback, reset `traits_published_` on disconnect, and republish `traits` plus `state` when the broker connection comes back.

## Issue 2: one JSON command can trigger two IR transmissions

**Problem**

A single MQTT command that mixes standard climate fields with Panasonic-specific swing fields can send two back-to-back IR commands and publish state twice. For example, a payload containing `mode` and `swing_mode` can first transmit through `call.perform()` and then transmit again through the custom swing branch.

**Technical root cause**

`on_set_json_()` splits command handling into two phases. It applies `mode` / `target_temperature` / `fan_mode` through `ClimateCall::perform()`, which already updates state, publishes, and transmits. It then processes `swing_mode` / `swing_horizontal_mode` separately and, if either changed, performs a second publish/transmit cycle.

Relevant code:

- `esphome/components/panaac_v2/panaac_v2.cpp:620`
- `esphome/components/panaac_v2/panaac_v2.cpp:632`
- `esphome/components/panaac_v2/panaac_v2.cpp:646`

**Proposed fix**

Parse the full JSON payload into one desired target state first, apply all supported fields to the canonical state in memory, and then emit at most one publish/transmit cycle if anything actually changed. That keeps the MQTT command path atomic and avoids redundant IR bursts.

## Issue 3: `topic_prefix` enables v2 mode without validating that `mqtt:` exists

**Problem**

A user can configure `topic_prefix` and believe they are in working v2 mode even when the YAML has no global `mqtt:` block. The firmware still builds, but the runtime path logs an error and the HA-facing MQTT contract never comes up.

**Technical root cause**

The code treats `CONF_TOPIC_PREFIX` as the switch for v2 mode in `to_code()`, but the schema does not reject configurations where `topic_prefix` is present and the ESPHome `mqtt` component is absent. At runtime, `publish_state_by_mode_()` detects that `USE_MQTT` is not compiled in and only logs `v2 MQTT mode requires a mqtt: block`.

Relevant code:

- `esphome/components/panaac_v2/__init__.py:99`
- `esphome/components/panaac_v2/__init__.py:137`
- `esphome/components/panaac_v2/panaac_v2.cpp:298`

**Proposed fix**

Add config validation that fails generation when `topic_prefix` is set but the global `mqtt:` component is not configured. This should be a build-time error, not a runtime log message.

## Issue 4: repository documentation disagrees with the implemented MQTT state contract

**Problem**

The repository documents the `state` topic inconsistently. Some docs say `state` is non-retained, while the implementation and README describe it as retained. This will mislead anyone debugging startup behavior, HA recovery after restart, or broker-retention problems.

**Technical root cause**

`publish_state_()` publishes the state topic as retained, but `DESIGN.md` still describes the startup sequence and topic table as non-retained. The docs were not updated to match the final implementation.

Relevant code and docs:

- `esphome/components/panaac_v2/panaac_v2.cpp:542`
- `README.md:53`
- `DESIGN.md:88`
- `DESIGN.md:177`

**Proposed fix**

Choose one contract and make all code and documentation agree. If retained `state` is intentional, update `DESIGN.md` accordingly and document the broker-recovery behavior explicitly. If non-retained `state` is the intended design, change the publish call to match and review HA startup expectations.
