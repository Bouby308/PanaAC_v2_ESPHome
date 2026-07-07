/*
 * Copyright 2025 Hoang Minh
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
#include "esphome/components/climate/climate.h"
#include "esphome/components/mqtt/custom_mqtt_device.h"
#include "esphome/components/remote_base/remote_base.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace esphome::panaac_v2 {

/** PanaAC v2 — Panasonic AC controller driven entirely over MQTT.
 *
 * This component inherits from the ESPHome Climate base class so that it exposes
 * the standard climate automation surface: lambdas (`id(ac).make_call()`,
 * `id(ac).mode`, `id(ac).target_temperature`, ...), `on_state` triggers and
 * `on_control` triggers.  It still publishes its state, traits and availability
 * to the custom `<prefix>/...` MQTT topics so that the PanaAC v2 Home Assistant
 * custom integration can expose arbitrary Panasonic fan levels, vertical-swing
 * positions and a separate horizontal-swing axis.
 */
class PanaACV2Climate : public climate::Climate,
                        public Component,
                        public mqtt::CustomMQTTDevice,
                        public remote_base::RemoteReceiverListener,
                        public remote_base::RemoteTransmittable {
 public:
  PanaACV2Climate();

  void set_topic_prefix(const std::string &topic_prefix) { this->topic_prefix_ = topic_prefix; }
  void set_supports_cool(bool supports) { this->supports_cool_ = supports; }
  void set_supports_heat(bool supports) { this->supports_heat_ = supports; }
  void set_supports_fan_only(bool supports) { this->supports_fan_only_ = supports; }
  void set_supports_quiet(bool supports) { this->supports_quiet_ = supports; }
  void set_fan_5level(bool fan_5level) { this->fan_5level_ = fan_5level; }
  void set_swing_horizontal(bool swing_horizontal) { this->swing_horizontal_ = swing_horizontal; }
  void set_temp_step(float temp_step) { this->temp_step_ = temp_step; }
  void set_ir_control(bool ir_control) { this->ir_control_ = ir_control; }
  void set_sensor(sensor::Sensor *sensor) { this->sensor_ = sensor; }

  void setup() override;
  void dump_config() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  /// Direct accessors for Panasonic-specific vertical/horizontal swing strings.
  /// These are NOT backed by the standard Climate enum because ESPHome's core
  /// climate component does not support arbitrary swing strings. They are kept
  /// inside this custom component so the component compiles against unmodified
  /// ESPHome source.
  void set_swing_mode_str(const char *mode) { this->custom_swing_mode_ = mode; }
  void set_swing_horizontal_mode_str(const char *mode) { this->swing_horizontal_mode_ = mode; }
  const char *get_swing_mode_str() const { return this->custom_swing_mode_; }
  const char *get_swing_horizontal_mode_str() const { return this->swing_horizontal_mode_; }

 protected:
  /// Climate integration interface.
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  bool on_receive(remote_base::RemoteReceiveData data) override;

  void publish_state_();
  void publish_traits_();

  void on_set_json_(const std::string &topic, JsonObject root);

  bool set_swing_mode_if_supported_(const char *mode);
  bool set_swing_horizontal_mode_if_supported_(const char *mode);

  void transmit_();
  bool decode_data_(remote_base::RemoteReceiveData data, std::array<uint8_t, 27> &state_bytes, size_t &state_len);
  bool decode_and_apply_(std::span<const uint8_t> state_bytes);

  std::string state_topic_() const { return this->topic_prefix_ + "/state"; }
  std::string traits_topic_() const { return this->topic_prefix_ + "/traits"; }
  std::string set_topic_() const { return this->topic_prefix_ + "/set"; }

  std::string topic_prefix_{"panaac_v2/panaac_v2"};

  bool supports_cool_{true};
  bool supports_heat_{false};
  bool supports_fan_only_{false};
  bool supports_quiet_{false};
  bool fan_5level_{false};
  bool swing_horizontal_{false};
  float temp_step_{1.0f};
  bool ir_control_{false};
  sensor::Sensor *sensor_{nullptr};

  // Panasonic-specific swing state, stored internally instead of in Climate base class.
  const char *custom_swing_mode_{STR_SWINGV_MIDDLE};
  const char *swing_horizontal_mode_{STR_SWINGH_MIDDLE};

  // Last fixed swing positions used when "swing off" is requested.
  SwingVPos last_swing_v_pos_{PANAAC_SWINGV_MIDDLE};
  SwingHPos last_swing_h_pos_{PANAAC_SWINGH_MIDDLE};

  bool traits_published_{false};
};

}  // namespace esphome::panaac_v2
