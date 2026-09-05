/*
 * Copyright 2026 Hoang Minh
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "definitions.h"
#include "extra.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/remote_base/remote_base.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include <array>
#include <cstddef>
#include <span>
#include <string>

#ifdef USE_MQTT
#include "esphome/components/mqtt/custom_mqtt_device.h"
#endif

namespace esphome::panaac_v2 {

/** PanaAC v2 — Panasonic AC controller with two exposure modes.
 *
 * The IR encode/decode core and the canonical `ac_state` (mode/temp/fan_level/swing positions)
 * are shared. What differs is how the state is exposed to Home Assistant:
 *
 * - **v1 native mode** (`topic_prefix` unset, `mqtt_enabled_ = false`): a normal native climate
 *   whose Fan Mode offers the full Panasonic fan levels (Auto / Level 1..5 / Quiet) as custom fan
 *   modes plus standard Auto/Quiet enums, visible on the ESPHome API / standard MQTT discovery,
 *   plus two companion `select` entities (Swing Vertical / Swing Horizontal) for the granular swing
 *   positions. Behaves like PanaAC v1. No MQTT broker is required — all MQTT code is compiled out
 *   (`USE_MQTT` undefined). The climate + selects sit at the root of the ESPHome device, like
 *   PanaAC v1.
 * - **v2 MQTT mode** (`topic_prefix` set, `mqtt_enabled_ = true`, requires a `mqtt:` block so
 *   `USE_MQTT` is defined): the climate is exposed over the custom
 *   `<prefix>/state|traits|availability|set` MQTT JSON topics consumed by the PanaAC v2 HA custom
 *   integration (the single all-in-one v2 climate card). It is ALSO kept visible on the native
 *   API as a "(v1)" climate — standard swing modes + custom fan-level strings + the same
 *   two Swing V/H selects — at the root of the ESPHome device, exactly like PanaAC
 *   v1. The custom MQTT topics are independent of the native ClimateTraits (publish_traits_() is
 *   hand-rolled JSON), so the v2 HA-integration card is unaffected by the visible native climate.
 *
 * `ac_state` is the single source of truth; the Climate base fields and (in v2 mode) the custom
 * fan/swing strings are derived from it via `sync_to_climate_()`.
 */
class PanaACV2Climate : public climate::Climate,
                        public Component,
#ifdef USE_MQTT
                        public mqtt::CustomMQTTDevice,
#endif
                        public remote_base::RemoteReceiverListener,
                        public remote_base::RemoteTransmittable {
 public:
  PanaACV2Climate();

  void set_topic_prefix(const std::string &topic_prefix) { this->topic_prefix_ = topic_prefix; }
  void set_mqtt_enabled(bool enabled) { this->mqtt_enabled_ = enabled; }
  void set_supports_cool(bool supports) { this->supports_cool_ = supports; }
  void set_supports_heat(bool supports) { this->supports_heat_ = supports; }
  void set_supports_fan_only(bool supports) { this->supports_fan_only_ = supports; }
  void set_supports_quiet(bool supports) { this->supports_quiet_ = supports; }
  void set_supports_powerful(bool supports) { this->supports_powerful_ = supports; }
  void set_supports_eco(bool supports) { this->supports_eco_ = supports; }
  void set_fan_5level(bool fan_5level) { this->fan_5level_ = fan_5level; }
  void set_swing_vertical(bool swing_vertical) { this->swing_vertical_ = swing_vertical; }
  void set_swing_horizontal(bool swing_horizontal) { this->swing_horizontal_ = swing_horizontal; }
  void set_temp_step(float temp_step) { this->temp_step_ = temp_step; }
  void set_ir_control(bool ir_control) { this->ir_control_ = ir_control; }
  void set_sensor(sensor::Sensor *sensor) { this->sensor_ = sensor; }
  void set_special_mode_persistence(SpecialModePersistence persistence) {
    this->special_mode_persistence_ = persistence;
  }
  void set_powerful_quiet_mode(PowerfulQuietMode mode) { this->powerful_quiet_mode_ = mode; }

  void set_swingv(PanaACV2SwingV *swingv) { this->swingv_ = swingv; }
  void set_swingh(PanaACV2SwingH *swingh) { this->swingh_ = swingh; }

  /// Entry points used by the companion Swing V/H selects (both modes). Each validates, mutates
  /// `ac_state`, transmits the IR frame, publishes (by mode), and re-syncs the other select.
  void apply_swingv_select_(SwingVPos pos);
  void apply_swingh_select_(SwingHPos pos);

  void setup() override;
  void dump_config() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  /// Public by design: the companion select entities read/write it directly (like v1).
  ClimateState ac_state;

 protected:
  /// Climate integration interface.
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  bool on_receive(remote_base::RemoteReceiveData data) override;

  // IR core (operate on ac_state).
  void transmit_data_();
  /// Send the fixed 8-byte wake frame followed by one short command frame (powerful / quiet).
  void transmit_command_frame_(const std::array<uint8_t, 8> &command);
  bool decode_data_(remote_base::RemoteReceiveData data, std::array<uint8_t, 27> &state_bytes, size_t &state_len);
  bool decode_state_(std::span<const uint8_t> state_bytes, ClimateState &state);
  bool decode_and_apply_(std::span<const uint8_t> state_bytes);
  /// Decode a fixed-size 8-byte short frame (wake frame or command frame) from a 132-item capture.
  bool decode_short_frame_(remote_base::RemoteReceiveData &data, std::array<uint8_t, 8> &bytes);

  // State synchronization helpers.
  void sync_to_climate_();         // derive Climate base fields (+ v2 custom strings) from ac_state
  void update_selects_();          // push ac_state fan/swing positions to the select entities
  void publish_state_by_mode_();   // v2: MQTT publish_state_() + publish_state(); v1: publish_state()
  void recompute_swing_mode_();    // derive ac_state.swing_mode from swing_v_pos / swing_h_pos
  void update_action_();           // derive Climate action from the commanded mode (one-way IR)

  // Special-mode (Powerful / Quiet) management.
  /// Set ac_state.preset, (re)arm/clear the 4h powerful timer, and persist mode + remaining time.
  /// Returns true when the mode actually changed. Does NOT publish or transmit.
  bool set_special_mode_(Preset next);
  /// loop() hook: auto-clear powerful after 4h (AC does it itself; state-only update, no IR) and
  /// periodically re-persist the remaining time so an ESP restart keeps the countdown accurate.
  void update_powerful_timer_();
  /// Persist the special mode + powerful remaining seconds to flash (no-ops while unchanged).
  void persist_special_mode_();

#ifdef USE_MQTT
  // v2 MQTT publish helpers (only used when mqtt_enabled_).
  void publish_state_();
  void publish_traits_();
  void on_set_json_(const std::string &topic, JsonObject root);
  bool set_swing_mode_if_supported_(const char *mode);
  bool set_swing_horizontal_mode_if_supported_(const char *mode);

  std::string state_topic_() const { return this->topic_prefix_ + "/state"; }
  std::string traits_topic_() const { return this->topic_prefix_ + "/traits"; }
  std::string set_topic_() const { return this->topic_prefix_ + "/set"; }
  std::string availability_topic_() const { return this->topic_prefix_ + "/availability"; }
#endif

  std::string topic_prefix_;
  bool mqtt_enabled_{false};

  bool supports_cool_{true};
  bool supports_heat_{false};
  bool supports_powerful_{false};
  bool supports_eco_{false};
  bool supports_fan_only_{false};
  bool supports_quiet_{false};
  bool fan_5level_{false};
  bool swing_vertical_{true};
  bool swing_horizontal_{false};
  float temp_step_{1.0f};
  bool ir_control_{false};
  sensor::Sensor *sensor_{nullptr};

  PanaACV2SwingV *swingv_{nullptr};
  PanaACV2SwingH *swingh_{nullptr};

  // v2 custom swing strings (derived from ac_state; published over MQTT). Kept as members so
  // publish_state_() and the v2 set path can read them without re-deriving each time.
  const char *custom_swing_mode_{STR_SWINGV_MIDDLE};
  const char *swing_horizontal_mode_{STR_SWINGH_MIDDLE};

  bool traits_published_{false};

  // v2 MQTT set-command atomicity (Codex review issue 2). While mqtt_command_active_ is set,
  // control() (reached via call.perform() from on_set_json_()) updates ac_state and records a
  // pending change in pending_change_ but does NOT publish or transmit, so a single MQTT
  // command mixing standard climate fields with Panasonic swing fields emits at most one IR
  // burst. on_set_json_() performs the single publish/transmit once all fields are applied.
  bool mqtt_command_active_{false};
  bool pending_change_{false};

  // Special-mode persistence (survives an ESP restart). Home Assistant restarts need nothing
  // here: the ESP device is the source of truth and republishes its full climate state when HA
  // reconnects to the native API. How much flash that costs is configurable
  // (special_mode_persistence: none / mode_only / full).
  // Built in setup() via global_preferences->make_preference<uint32_t>(fnv1_hash(...), true).
  SpecialModePersistence special_mode_persistence_{SM_PERSIST_FULL};
  // How Powerful/Quiet are exposed: PQ_MODE_SPECIAL (default, this fork's toggle-frame models)
  // or PQ_MODE_LEGACY (upstream semantics: Quiet = fan speed, Powerful = state-frame bit; all
  // special-mode machinery — command frames, power-on clear, 4h timer, persistence, short-frame
  // RX mirroring — is disabled in legacy mode).
  PowerfulQuietMode powerful_quiet_mode_{PQ_MODE_SPECIAL};
  ESPPreferenceObject pref_special_mode_;
  ESPPreferenceObject pref_powerful_remaining_;
  // 4h Powerful countdown in the millis() domain; only meaningful while preset == POWERFUL.
  uint32_t powerful_deadline_ms_{0};
  uint32_t next_powerful_persist_ms_{0};

  // Self-reception guard. The IR receiver can pick up our own transmitted burst (RX/TX sit close
  // together on the Athom). Powerful/Quiet are TOGGLE command frames, so mirroring our own echo
  // would flip the mode right back and HA would show the wrong (reset) state. After every
  // transmit we ignore received frames until this millis() deadline.
  uint32_t rx_ignore_until_ms_{0};
};

}  // namespace esphome::panaac_v2
