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

#include "panaac_v2.h"

#include <array>
#include <cstdio>
#include <cstddef>
#include <span>
#include <vector>

namespace esphome::panaac_v2 {

PanaACV2Climate::PanaACV2Climate() {
  this->target_temperature = 24.0f;
}

void PanaACV2Climate::setup() {
  ESP_LOGCONFIG(TAG, "Setting up PanaAC v2 '%s'", this->get_name().c_str());
  ESP_LOGCONFIG(TAG, "  Topic prefix: %s", this->topic_prefix_.c_str());

  // Register the arbitrary Panasonic fan/swing strings as Climate custom modes.
  // Note: set_supported_custom_fan_modes() replaces the whole list, so build one vector.
  std::vector<const char *> fan_modes = {STR_FAN_AUTO, STR_FAN_L1};
  if (this->fan_5level_) {
    fan_modes.insert(fan_modes.end(), {STR_FAN_L2, STR_FAN_L3, STR_FAN_L4});
  }
  fan_modes.push_back(STR_FAN_L5);
  if (this->supports_quiet_) {
    fan_modes.push_back(STR_FAN_QUIET);
  }
  this->set_supported_custom_fan_modes(fan_modes);

  // Initialize defaults.
  this->set_custom_fan_mode_(STR_FAN_AUTO, strlen(STR_FAN_AUTO));
  this->custom_swing_mode_ = STR_SWINGV_MIDDLE;
  if (this->swing_horizontal_) {
    this->swing_horizontal_mode_ = STR_SWINGH_MIDDLE;
  } else {
    this->swing_horizontal_mode_ = nullptr;
  }

  // Initialize horizontal swing defaults based on feature support.
  if (!this->swing_horizontal_) {
    this->last_swing_h_pos_ = PANAAC_SWINGH_NONE;
  } else {
    this->last_swing_h_pos_ = PANAAC_SWINGH_MIDDLE;
  }
  this->last_swing_v_pos_ = PANAAC_SWINGV_MIDDLE;

  this->subscribe_json(this->set_topic_(), &PanaACV2Climate::on_set_json_);

  if (this->sensor_ != nullptr) {
    this->sensor_->add_on_state_callback([this](float state) {
      if (!std::isnan(state)) {
        this->current_temperature = state;
        this->publish_state_();
      }
    });
  }

  // Restore previous Climate state (indices reference the supported-mode vectors above).
  if (auto state = this->restore_state_()) {
    state->apply(this);
  }

  // Publish retained availability + traits once MQTT is up (loop() handles the delay).
}

void PanaACV2Climate::loop() {
  if (!this->is_connected())
    return;

  if (!this->traits_published_) {
    this->publish_traits_();
    this->traits_published_ = true;
    this->publish_state_();
  }
}

climate::ClimateTraits PanaACV2Climate::traits() {
  climate::ClimateTraits traits;
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF});
  if (this->supports_cool_)
    traits.add_supported_mode(climate::CLIMATE_MODE_COOL);
  if (this->supports_heat_)
    traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
  if (this->supports_fan_only_)
    traits.add_supported_mode(climate::CLIMATE_MODE_FAN_ONLY);
  traits.add_supported_mode(climate::CLIMATE_MODE_DRY);
  traits.add_supported_mode(climate::CLIMATE_MODE_AUTO);

  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);

  traits.set_visual_min_temperature(PANAAC_TEMP_MIN);
  traits.set_visual_max_temperature(PANAAC_TEMP_MAX);
  traits.set_visual_target_temperature_step(this->temp_step_);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

void PanaACV2Climate::dump_config() {
  ESP_LOGCONFIG(TAG, "PanaAC v2:");
  LOG_CLIMATE("  ", "Climate", this);
  ESP_LOGCONFIG(TAG, "  Topic prefix: %s", this->topic_prefix_.c_str());
  ESP_LOGCONFIG(TAG, "  Temp step: %.1f", this->temp_step_);
  ESP_LOGCONFIG(TAG, "  Fan 5-level: %s", YESNO(this->fan_5level_));
  ESP_LOGCONFIG(TAG, "  Supports cool: %s", YESNO(this->supports_cool_));
  ESP_LOGCONFIG(TAG, "  Supports heat: %s", YESNO(this->supports_heat_));
  ESP_LOGCONFIG(TAG, "  Supports fan-only: %s", YESNO(this->supports_fan_only_));
  ESP_LOGCONFIG(TAG, "  Supports quiet: %s", YESNO(this->supports_quiet_));
  ESP_LOGCONFIG(TAG, "  Swing horizontal: %s", YESNO(this->swing_horizontal_));
  ESP_LOGCONFIG(TAG, "  IR control (38kHz): %s", YESNO(this->ir_control_));
  if (this->sensor_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Current temperature sensor:");
    LOG_SENSOR("    ", "Sensor", this->sensor_);
  }
}

// ---------------- Climate control ----------------

void PanaACV2Climate::control(const climate::ClimateCall &call) {
  bool changed = false;

  if (call.get_mode().has_value()) {
    auto mode = *call.get_mode();
    // Reject unsupported modes even though validate_ already checked.
    if (mode == climate::CLIMATE_MODE_HEAT && !this->supports_heat_) {
      ESP_LOGW(TAG, "Heat mode not supported");
      mode = climate::CLIMATE_MODE_OFF;
    }
    if (mode == climate::CLIMATE_MODE_FAN_ONLY && !this->supports_fan_only_) {
      ESP_LOGW(TAG, "Fan-only mode not supported");
      mode = climate::CLIMATE_MODE_OFF;
    }
    if (mode == climate::CLIMATE_MODE_COOL && !this->supports_cool_) {
      ESP_LOGW(TAG, "Cool mode not supported");
      mode = climate::CLIMATE_MODE_OFF;
    }
    if (this->mode != mode) {
      this->mode = mode;
      changed = true;
    }
  }

  if (call.get_target_temperature().has_value()) {
    float value = *call.get_target_temperature();
    if (value < PANAAC_TEMP_MIN)
      value = PANAAC_TEMP_MIN;
    if (value > PANAAC_TEMP_MAX)
      value = PANAAC_TEMP_MAX;
    if (this->target_temperature != value) {
      this->target_temperature = value;
      changed = true;
    }
  }

  if (call.has_custom_fan_mode() || call.get_fan_mode().has_value()) {
    FanLevel level = PANAAC_FAN_AUTO;
    if (call.has_custom_fan_mode()) {
      level = fan_level_from_str(call.get_custom_fan_mode().c_str());
    } else {
      switch (*call.get_fan_mode()) {
        case climate::CLIMATE_FAN_AUTO:
          level = PANAAC_FAN_AUTO;
          break;
        case climate::CLIMATE_FAN_QUIET:
          level = PANAAC_FAN_QUIET;
          break;
        case climate::CLIMATE_FAN_LOW:
          level = PANAAC_FAN_LEVEL_1;
          break;
        case climate::CLIMATE_FAN_MEDIUM:
          level = PANAAC_FAN_LEVEL_3;
          break;
        case climate::CLIMATE_FAN_HIGH:
          level = PANAAC_FAN_LEVEL_5;
          break;
        default:
          ESP_LOGW(TAG, "Standard fan mode %s not supported; use custom fan modes",
                   LOG_STR_ARG(climate_fan_mode_to_string(*call.get_fan_mode())));
          level = PANAAC_FAN_AUTO;
          break;
      }
    }
    if (level == PANAAC_FAN_QUIET && !this->supports_quiet_) {
      ESP_LOGW(TAG, "Quiet fan mode not supported");
      level = PANAAC_FAN_AUTO;
    }
    if ((level == PANAAC_FAN_LEVEL_2 || level == PANAAC_FAN_LEVEL_4) && !this->fan_5level_) {
      ESP_LOGW(TAG, "Fan level %s requires 5-level support", fan_level_to_str(level));
      level = PANAAC_FAN_AUTO;
    }
    const char *desired = fan_level_to_str(level);
    if (this->get_custom_fan_mode() != desired) {
      this->set_custom_fan_mode_(desired, strlen(desired));
      changed = true;
    }
  }

  if (call.get_swing_mode().has_value()) {
    ESP_LOGW(TAG, "Standard swing modes are not supported; use the panaac_v2 swing services or MQTT commands");
  }

  if (changed) {
    this->transmit_();
    this->publish_state_();
  }
}

bool PanaACV2Climate::set_swing_mode_if_supported_(const char *mode) {
  auto pos = swing_v_pos_from_str(mode);
  const char *desired = swing_v_pos_to_str(pos);
  if (this->custom_swing_mode_ != desired) {
    this->custom_swing_mode_ = desired;
    if (pos != PANAAC_SWINGV_AUTO)
      this->last_swing_v_pos_ = pos;
    return true;
  }
  return false;
}

bool PanaACV2Climate::set_swing_horizontal_mode_if_supported_(const char *mode) {
  if (!this->swing_horizontal_) {
    ESP_LOGW(TAG, "Horizontal swing not supported");
    return false;
  }
  auto pos = swing_h_pos_from_str(mode);
  if (pos == PANAAC_SWINGH_NONE) {
    ESP_LOGW(TAG, "Unsupported horizontal swing mode: %s", mode);
    return false;
  }
  const char *desired = swing_h_pos_to_str(pos);
  if (this->swing_horizontal_mode_ != desired) {
    this->swing_horizontal_mode_ = desired;
    if (pos != PANAAC_SWINGH_AUTO)
      this->last_swing_h_pos_ = pos;
    return true;
  }
  return false;
}

// ---------------- MQTT publish helpers ----------------

void PanaACV2Climate::publish_state_() {
  this->publish_json(this->state_topic_(), [this](JsonObject root) {
    root["mode"] = mode_to_str(climate_mode_to_mode(this->mode));
    root["target_temperature"] = this->target_temperature;

    const char *fan_str = this->get_custom_fan_mode().c_str();
    if (fan_str == nullptr || strlen(fan_str) == 0) {
      if (this->fan_mode.has_value()) {
        switch (this->fan_mode.value()) {
          case climate::CLIMATE_FAN_QUIET:
            fan_str = STR_FAN_QUIET;
            break;
          case climate::CLIMATE_FAN_LOW:
            fan_str = STR_FAN_L1;
            break;
          case climate::CLIMATE_FAN_MEDIUM:
            fan_str = STR_FAN_L3;
            break;
          case climate::CLIMATE_FAN_HIGH:
            fan_str = STR_FAN_L5;
            break;
          case climate::CLIMATE_FAN_AUTO:
          default:
            fan_str = STR_FAN_AUTO;
            break;
        }
      } else {
        fan_str = STR_FAN_AUTO;
      }
    }
    root["fan_mode"] = fan_str;

    const char *swing_str = this->custom_swing_mode_;
    if (swing_str == nullptr || strlen(swing_str) == 0)
      swing_str = STR_SWINGV_AUTO;
    root["swing_mode"] = swing_str;

    if (this->swing_horizontal_ && this->swing_horizontal_mode_ != nullptr && strlen(this->swing_horizontal_mode_) > 0) {
      root["swing_horizontal_mode"] = this->swing_horizontal_mode_;
    }
    if (!std::isnan(this->current_temperature))
      root["current_temperature"] = this->current_temperature;
    root["available"] = true;
  });

  // Fire the Climate on_state trigger and save state.
  this->publish_state();
}

void PanaACV2Climate::publish_traits_() {
  this->publish_json(this->traits_topic_(), [this](JsonObject root) {
    JsonArray hvac_modes = root["hvac_modes"].to<JsonArray>();
    hvac_modes.add("off");
    if (this->supports_cool_)
      hvac_modes.add("cool");
    if (this->supports_heat_)
      hvac_modes.add("heat");
    if (this->supports_fan_only_)
      hvac_modes.add("fan_only");
    hvac_modes.add("dry");
    hvac_modes.add("auto");

    JsonArray fan_modes = root["fan_modes"].to<JsonArray>();
    fan_modes.add(STR_FAN_AUTO);
    fan_modes.add(STR_FAN_L1);
    if (this->fan_5level_) {
      fan_modes.add(STR_FAN_L2);
      fan_modes.add(STR_FAN_L3);
      fan_modes.add(STR_FAN_L4);
    }
    fan_modes.add(STR_FAN_L5);
    if (this->supports_quiet_)
      fan_modes.add(STR_FAN_QUIET);

    JsonArray swing_modes = root["swing_modes"].to<JsonArray>();
    swing_modes.add(STR_SWINGV_AUTO);
    swing_modes.add(STR_SWINGV_HIGHEST);
    swing_modes.add(STR_SWINGV_HIGH);
    swing_modes.add(STR_SWINGV_MIDDLE);
    swing_modes.add(STR_SWINGV_LOW);
    swing_modes.add(STR_SWINGV_LOWEST);

    if (this->swing_horizontal_) {
      JsonArray h_modes = root["swing_horizontal_modes"].to<JsonArray>();
      h_modes.add(STR_SWINGH_AUTO);
      h_modes.add(STR_SWINGH_LEFTMAX);
      h_modes.add(STR_SWINGH_LEFT);
      h_modes.add(STR_SWINGH_MIDDLE);
      h_modes.add(STR_SWINGH_RIGHT);
      h_modes.add(STR_SWINGH_RIGHTMAX);
    }

    root["min_temp"] = PANAAC_TEMP_MIN;
    root["max_temp"] = PANAAC_TEMP_MAX;
    root["temp_step"] = this->temp_step_;
    root["temperature_unit"] = "C";
  }, 0, true);
}

// ---------------- Command handling ----------------

void PanaACV2Climate::on_set_json_(const std::string &topic, JsonObject root) {
  ESP_LOGD(TAG, "Received command on %s", topic.c_str());

  auto call = this->make_call();
  bool changed = false;

  if (root["mode"].is<const char *>()) {
    const char *v = root["mode"];
    call.set_mode(v, strlen(v));
  }
  if (root["target_temperature"].is<float>()) {
    call.set_target_temperature(static_cast<float>(root["target_temperature"]));
  }
  if (root["fan_mode"].is<const char *>()) {
    const char *v = root["fan_mode"];
    call.set_fan_mode(v, strlen(v));
  }

  call.perform();

  // Panasonic-specific swing strings are handled directly, not through the standard
  // ClimateCall, because ESPHome's core climate component does not support arbitrary
  // swing strings.
  if (root["swing_mode"].is<const char *>()) {
    if (this->set_swing_mode_if_supported_(root["swing_mode"]))
      changed = true;
  }
  if (root["swing_horizontal_mode"].is<const char *>()) {
    if (this->set_swing_horizontal_mode_if_supported_(root["swing_horizontal_mode"]))
      changed = true;
  }

  if (changed) {
    this->transmit_();
    this->publish_state_();
  }
}

// ---------------- IR transmit ----------------

void PanaACV2Climate::transmit_() {
  static const std::array<uint8_t, 8> FIRST_FRAME = {0x02, 0x20, 0xE0, 0x04, 0x00, 0x00, 0x00, 0x06};
  std::array<uint8_t, 19> second_frame = {0x02, 0x20, 0xE0, 0x04, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
                                            0x00, 0x0E, 0xE0, 0x00, 0x00, 0x89, 0x00, 0x00, 0x00};

  Mode pana_mode = climate_mode_to_mode(this->mode);

  // power & mode
  switch (pana_mode) {
    case MODE_COOL:
      second_frame[PANAAC_BYTEPOS_POWER] |= PANAAC_POWER_ON;
      second_frame[PANAAC_BYTEPOS_MODE] |= PANAAC_MODE_COOL;
      break;
    case MODE_HEAT:
      second_frame[PANAAC_BYTEPOS_POWER] |= PANAAC_POWER_ON;
      second_frame[PANAAC_BYTEPOS_MODE] |= PANAAC_MODE_HEAT;
      break;
    case MODE_DRY:
      second_frame[PANAAC_BYTEPOS_POWER] |= PANAAC_POWER_ON;
      second_frame[PANAAC_BYTEPOS_MODE] |= PANAAC_MODE_DRY;
      break;
    case MODE_FAN_ONLY:
      second_frame[PANAAC_BYTEPOS_POWER] |= PANAAC_POWER_ON;
      second_frame[PANAAC_BYTEPOS_MODE] |= PANAAC_MODE_FAN_ONLY;
      break;
    case MODE_AUTO:
      second_frame[PANAAC_BYTEPOS_POWER] |= PANAAC_POWER_ON;
      second_frame[PANAAC_BYTEPOS_MODE] |= PANAAC_MODE_AUTO;
      break;
    case MODE_OFF:
    default:
      second_frame[PANAAC_BYTEPOS_POWER] |= PANAAC_POWER_OFF;
      second_frame[PANAAC_BYTEPOS_MODE] |= PANAAC_MODE_COOL;
      break;
  }

  // temperature
  uint8_t encoded_temp = static_cast<uint8_t>(this->target_temperature) - PANAAC_TEMP_MIN;
  encoded_temp &= 0x0F;
  second_frame[PANAAC_BYTEPOS_TEMP] = 0x20 | (encoded_temp << 1);
  if (static_cast<uint8_t>(this->target_temperature) < this->target_temperature)
    second_frame[PANAAC_BYTEPOS_TEMP] |= 0x01;

  // fan
  FanLevel fan_level = PANAAC_FAN_AUTO;
  if (this->has_custom_fan_mode()) {
    fan_level = fan_level_from_str(this->get_custom_fan_mode().c_str());
  } else if (this->fan_mode.has_value()) {
    switch (this->fan_mode.value()) {
      case climate::CLIMATE_FAN_QUIET:
        fan_level = PANAAC_FAN_QUIET;
        break;
      case climate::CLIMATE_FAN_LOW:
        fan_level = PANAAC_FAN_LEVEL_1;
        break;
      case climate::CLIMATE_FAN_MEDIUM:
        fan_level = PANAAC_FAN_LEVEL_3;
        break;
      case climate::CLIMATE_FAN_HIGH:
        fan_level = PANAAC_FAN_LEVEL_5;
        break;
      case climate::CLIMATE_FAN_AUTO:
      default:
        fan_level = PANAAC_FAN_AUTO;
        break;
    }
  }
  if (fan_level == PANAAC_FAN_QUIET && this->supports_quiet_) {
    second_frame[PANAAC_BYTEPOS_QUIET] |= PANAAC_FAN_QUIET;
    second_frame[PANAAC_BYTEPOS_FAN] |= fan_level;
  } else {
    second_frame[PANAAC_BYTEPOS_FAN] |= fan_level;
  }

  // swing
  SwingVPos swing_v = (this->custom_swing_mode_ != nullptr)
                          ? swing_v_pos_from_str(this->custom_swing_mode_)
                          : PANAAC_SWINGV_AUTO;
  second_frame[PANAAC_BYTEPOS_SWINGV] |= swing_v;
  if (this->swing_horizontal_) {
    SwingHPos swing_h = (this->swing_horizontal_mode_ != nullptr)
                            ? swing_h_pos_from_str(this->swing_horizontal_mode_)
                            : PANAAC_SWINGH_MIDDLE;
    second_frame[PANAAC_BYTEPOS_SWINGH] |= swing_h;
  } else {
    second_frame[PANAAC_BYTEPOS_SWINGH] |= PANAAC_SWINGH_NONE;
  }

  // checksum
  for (uint8_t i = 0; i < 18; i++) {
    second_frame[18] += second_frame[i];
  }

#if (ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE)
  char hex[3 * 19 + 1];
  int p = 0;
  for (uint8_t b : second_frame) {
    p += snprintf(hex + p, sizeof(hex) - p, "%02X ", b);
  }
  ESP_LOGV(TAG, "Sending Panasonic AC IR state: len = %d, data = [ %s]", second_frame.size(), hex);
#endif

  auto transmit = this->transmitter_->transmit();
  auto *data = transmit.get_data();

  if (this->ir_control_)
    data->set_carrier_frequency(PANAAC_IR_TRANSMIT_FREQ);

  // First frame
  data->mark(PANAAC_HEADER_MARK);
  data->space(PANAAC_HEADER_SPACE);
  for (uint8_t b : FIRST_FRAME) {
    for (uint8_t i_bit = 0; i_bit < 8; i_bit++) {
      data->mark(PANAAC_BIT_MARK);
      bool bit = b & (1 << i_bit);
      data->space(bit ? PANAAC_ONE_SPACE : PANAAC_ZERO_SPACE);
    }
  }
  data->mark(PANAAC_BIT_MARK);
  data->space(PANAAC_FRAME_END);

  // 2nd frame
  data->mark(PANAAC_HEADER_MARK);
  data->space(PANAAC_HEADER_SPACE);
  for (uint8_t b : second_frame) {
    for (uint8_t i_bit = 0; i_bit < 8; i_bit++) {
      data->mark(PANAAC_BIT_MARK);
      bool bit = b & (1 << i_bit);
      data->space(bit ? PANAAC_ONE_SPACE : PANAAC_ZERO_SPACE);
    }
  }
  data->mark(PANAAC_BIT_MARK);
  data->space(PANAAC_FRAME_END);

  transmit.perform();
}

// ---------------- IR receive ----------------

bool PanaACV2Climate::on_receive(remote_base::RemoteReceiveData data) {
  const auto &raw_data = data.get_raw_data();
  ESP_LOGV(TAG, "Received raw data size = %d", raw_data.size());

  if (raw_data.size() == 132) {  // fixed 1st frame
    ESP_LOGV(TAG, "Ignored first frame!");
    return false;
  }
  if (raw_data.size() != 308 && raw_data.size() != 440) {
    ESP_LOGV(TAG, "Unexpected data length received: %d", raw_data.size());
    return false;
  }

  std::array<uint8_t, 27> state_bytes{};
  size_t state_len = 0;
  if (!this->decode_data_(data, state_bytes, state_len)) {
    ESP_LOGV(TAG, "Decode IR data failed");
    return false;
  }

  if (!this->decode_and_apply_(std::span<const uint8_t>(state_bytes.data(), state_len))) {
    ESP_LOGV(TAG, "Decode state failed");
    return false;
  }

  this->publish_state_();
  return true;
}

bool PanaACV2Climate::decode_data_(remote_base::RemoteReceiveData data, std::array<uint8_t, 27> &state_bytes,
                                   size_t &state_len) {
  const auto &raw_data = data.get_raw_data();

  if (raw_data.size() != 308 && raw_data.size() != 440)
    return false;

  if (!data.expect_item(PANAAC_HEADER_MARK, PANAAC_HEADER_SPACE)) {
    ESP_LOGV(TAG, "Invalid data - expected header");
    return false;
  }

  state_len = 0;
  while (data.get_index() + 2 < raw_data.size()) {
    uint8_t byte = 0;
    for (uint8_t a_bit = 0; a_bit < 8; a_bit++) {
      if (data.expect_item(PANAAC_BIT_MARK, PANAAC_FRAME_END)) {
        if (!data.expect_item(PANAAC_HEADER_MARK, PANAAC_HEADER_SPACE)) {
          ESP_LOGV(TAG, "Invalid data - expected header at index = %d", data.get_index());
          return false;
        }
      }

      if (data.expect_item(PANAAC_BIT_MARK, PANAAC_ONE_SPACE)) {
        byte |= 1 << a_bit;
      } else if (data.expect_item(PANAAC_BIT_MARK, PANAAC_ZERO_SPACE)) {
        // zero, nothing to do
      } else {
        ESP_LOGV(TAG, "Invalid bit %d of byte %d, index = %d", a_bit, state_len, data.get_index());
        return false;
      }
    }
    if (state_len >= state_bytes.size()) {
      ESP_LOGV(TAG, "Decoded frame too long");
      return false;
    }
    state_bytes[state_len++] = byte;
  }

#if (ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE)
  char hex[3 * 27 + 1];
  int p = 0;
  for (size_t i = 0; i < state_len; i++) {
    p += snprintf(hex + p, sizeof(hex) - p, "%02X ", state_bytes[i]);
  }
  ESP_LOGV(TAG, "Command decoded: len = %d, data = [ %s]", state_len, hex);
#endif

  // full frame: crop the fixed first 8 bytes
  if (state_len == 27) {
    for (size_t i = 0; i < 19; i++) {
      state_bytes[i] = state_bytes[i + 8];
    }
    state_len = 19;
  }

  return true;
}

bool PanaACV2Climate::decode_and_apply_(std::span<const uint8_t> state_bytes) {
  if (state_bytes.size() != 19)
    return false;

  // check protocol
  if (state_bytes[0] != 0x02 || state_bytes[1] != 0x20 || state_bytes[2] != 0xE0 || state_bytes[3] != 0x04 ||
      state_bytes[4] != 0x00) {
    ESP_LOGV(TAG, "Invalid protocol");
    return false;
  }

  // verify checksum
  uint8_t checksum = 0;
  for (size_t i = 0; i < 18; i++) {
    checksum += state_bytes[i];
  }
  if (checksum != state_bytes[18]) {
    ESP_LOGV(TAG, "Invalid checksum");
    return false;
  }

  // operation mode
  Mode pana_mode = MODE_OFF;
  if ((state_bytes[PANAAC_BYTEPOS_POWER] & PANAAC_POWER_MASK) != PANAAC_POWER_OFF) {
    switch (state_bytes[PANAAC_BYTEPOS_MODE] & 0xF0) {
      case PANAAC_MODE_DRY:
        pana_mode = MODE_DRY;
        break;
      case PANAAC_MODE_COOL:
        pana_mode = MODE_COOL;
        break;
      case PANAAC_MODE_HEAT:
        pana_mode = MODE_HEAT;
        break;
      case PANAAC_MODE_FAN_ONLY:
        pana_mode = MODE_FAN_ONLY;
        break;
      case PANAAC_MODE_AUTO:
      default:
        pana_mode = MODE_AUTO;
        break;
    }
  }

  if (pana_mode == MODE_HEAT && !this->supports_heat_) {
    ESP_LOGV(TAG, "Heat mode not supported");
    pana_mode = MODE_OFF;
  }
  if (pana_mode == MODE_FAN_ONLY && !this->supports_fan_only_) {
    ESP_LOGV(TAG, "Fan-only mode not supported");
    pana_mode = MODE_OFF;
  }
  if (pana_mode == MODE_COOL && !this->supports_cool_) {
    ESP_LOGV(TAG, "Cool mode not supported");
    pana_mode = MODE_OFF;
  }

  this->mode = mode_to_climate_mode(pana_mode);

  // temperature
  this->target_temperature = ((state_bytes[PANAAC_BYTEPOS_TEMP] & 0x1E) >> 1) + PANAAC_TEMP_MIN;
  if ((state_bytes[PANAAC_BYTEPOS_TEMP] & 0x01) == 0x01)
    this->target_temperature += 0.5;

  // fan
  uint8_t fan = state_bytes[PANAAC_BYTEPOS_FAN] & 0xF0;
  FanLevel fan_level = static_cast<FanLevel>(fan);
  if (this->supports_quiet_) {
    if ((state_bytes[PANAAC_BYTEPOS_QUIET] & 0xF0) == PANAAC_FAN_QUIET)
      fan_level = PANAAC_FAN_QUIET;
  }
  const char *fan_str = fan_level_to_str(fan_level);
  this->set_custom_fan_mode_(fan_str, strlen(fan_str));

  // swing
  uint8_t swing_v = state_bytes[PANAAC_BYTEPOS_SWINGV] & 0x0F;
  uint8_t swing_h = state_bytes[PANAAC_BYTEPOS_SWINGH] & 0x0F;

  SwingVPos v_pos = static_cast<SwingVPos>(swing_v);
  this->custom_swing_mode_ = swing_v_pos_to_str(v_pos);
  if (v_pos != PANAAC_SWINGV_AUTO)
    this->last_swing_v_pos_ = v_pos;

  if (this->swing_horizontal_) {
    SwingHPos h_pos = static_cast<SwingHPos>(swing_h);
    if (h_pos != PANAAC_SWINGH_NONE) {
      this->swing_horizontal_mode_ = swing_h_pos_to_str(h_pos);
      if (h_pos != PANAAC_SWINGH_AUTO)
        this->last_swing_h_pos_ = h_pos;
    }
  } else {
    this->swing_horizontal_mode_ = nullptr;
  }

  return true;
}

}  // namespace esphome::panaac_v2
