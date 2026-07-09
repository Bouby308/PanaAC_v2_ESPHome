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
#include <cstring>
#include <span>
#include <vector>

namespace esphome::panaac_v2 {

namespace {
// Map a granular FanLevel to the lossy standard ClimateFanMode enum (climate card in v1 mode).
inline climate::ClimateFanMode fan_level_to_standard(FanLevel level) {
  switch (level) {
    case PANAAC_FAN_LEVEL_1:
    case PANAAC_FAN_LEVEL_2:
      return climate::CLIMATE_FAN_LOW;
    case PANAAC_FAN_LEVEL_3:
    case PANAAC_FAN_LEVEL_4:
      return climate::CLIMATE_FAN_MEDIUM;
    case PANAAC_FAN_LEVEL_5:
      return climate::CLIMATE_FAN_HIGH;
    case PANAAC_FAN_QUIET:
      return climate::CLIMATE_FAN_QUIET;
    case PANAAC_FAN_AUTO:
    default:
      return climate::CLIMATE_FAN_AUTO;
  }
}
}  // namespace

PanaACV2Climate::PanaACV2Climate() {
  // Fresh-boot defaults (used when no state is restored from flash, e.g. after a factory erase):
  // off, 26 °C, fan Auto, vertical swing Auto, horizontal swing Auto. setup() re-derives ac_state
  // from these base fields, so set the base swing_mode to BOTH → swing_v_pos/swing_h_pos = AUTO
  // (clamped to VERTICAL by setup() when swing_horizontal_ is false).
  this->target_temperature = 26.0f;
  this->swing_mode = climate::CLIMATE_SWING_BOTH;
  this->ac_state.mode = climate::CLIMATE_MODE_OFF;
  this->ac_state.temp = 26.0f;
  this->ac_state.fan_mode = climate::CLIMATE_FAN_AUTO;
  this->ac_state.fan_level = PANAAC_FAN_AUTO;
  this->ac_state.swing_mode = climate::CLIMATE_SWING_BOTH;
  this->ac_state.swing_v_pos = PANAAC_SWINGV_AUTO;
  this->ac_state.swing_h_pos = PANAAC_SWINGH_AUTO;
  this->ac_state.last_swing_v_pos = PANAAC_SWINGV_MIDDLE;
  this->ac_state.last_swing_h_pos = PANAAC_SWINGH_MIDDLE;
}

// ---------------- setup / loop ----------------

void PanaACV2Climate::setup() {
  ESP_LOGCONFIG(TAG, "Setting up PanaAC v2 '%s'", this->get_name().c_str());
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->mqtt_enabled_ ? "v2 MQTT" : "v1 native");
  if (this->mqtt_enabled_)
    ESP_LOGCONFIG(TAG, "  Topic prefix: %s", this->topic_prefix_.c_str());

  // Register the Panasonic fan-level strings as Climate custom fan modes (BOTH modes) so the fan
  // levels are selectable directly from the climate card's Fan Mode. Auto/Quiet are NOT custom —
  // they are the standard CLIMATE_FAN_AUTO/CLIMATE_FAN_QUIET enums (advertised in traits()) so the
  // card shows each once (no duplicate custom strings) and `set_fan_mode("Auto"/"Quiet")` routes
  // to the enum and passes validate_(). Only the levels (which the standard enum can't represent)
  // are custom. This makes the standalone Fan Level select redundant, so it is no longer created.
  {
    // A 3-level AC exposes Level 1/3/5 (low/medium/high); a 5-level AC adds Level 2 and 4.
    std::vector<const char *> fan_modes = {STR_FAN_L1};
    if (this->fan_5level_)
      fan_modes.push_back(STR_FAN_L2);
    fan_modes.push_back(STR_FAN_L3);
    if (this->fan_5level_)
      fan_modes.push_back(STR_FAN_L4);
    fan_modes.push_back(STR_FAN_L5);
    this->set_supported_custom_fan_modes(fan_modes);
  }

  // Fill the companion Swing V/H selects' options at runtime. (The Fan Level select is gone — fan
  // levels are climate custom fan modes now.) The selects exist in BOTH modes.
  if (this->swingv_ != nullptr) {
    this->swingv_->traits.set_options(
        {STR_SWINGV_AUTO, STR_SWINGV_HIGHEST, STR_SWINGV_HIGH, STR_SWINGV_MIDDLE, STR_SWINGV_LOW, STR_SWINGV_LOWEST});
  }
  if (this->swing_horizontal_ && this->swingh_ != nullptr) {
    this->swingh_->traits.set_options({STR_SWINGH_AUTO, STR_SWINGH_LEFTMAX, STR_SWINGH_LEFT, STR_SWINGH_MIDDLE,
                                       STR_SWINGH_RIGHT, STR_SWINGH_RIGHTMAX});
  }

  // v2 mode: subscribe to the MQTT set topic + current-temperature sensor callback.
  if (this->mqtt_enabled_) {
#ifdef USE_MQTT
    this->subscribe_json(this->set_topic_(), &PanaACV2Climate::on_set_json_);
#endif
  }
  if (this->sensor_ != nullptr) {
    this->sensor_->add_on_state_callback([this](float state) {
      if (!std::isnan(state)) {
        this->current_temperature = state;
        this->publish_state_by_mode_();
      }
    });
  }

  // Restore previous Climate state (mode/target_temperature/fan_mode/swing_mode) from flash.
  if (auto state = this->restore_state_())
    state->apply(this);

  // Derive canonical ac_state from the restored Climate fields (do NOT transmit on boot — the
  // AC must not be commanded just because the ESP booted).
  this->ac_state.mode = this->mode;
  this->ac_state.temp = this->target_temperature;
  if (this->has_custom_fan_mode()) {
    this->ac_state.fan_level = fan_level_from_str(this->get_custom_fan_mode().c_str());
  } else if (this->fan_mode.has_value()) {
    switch (this->fan_mode.value()) {
      case climate::CLIMATE_FAN_LOW:
        this->ac_state.fan_level = PANAAC_FAN_LEVEL_1;
        break;
      case climate::CLIMATE_FAN_MEDIUM:
        this->ac_state.fan_level = PANAAC_FAN_LEVEL_3;
        break;
      case climate::CLIMATE_FAN_HIGH:
        this->ac_state.fan_level = PANAAC_FAN_LEVEL_5;
        break;
      case climate::CLIMATE_FAN_QUIET:
        this->ac_state.fan_level = this->supports_quiet_ ? PANAAC_FAN_QUIET : PANAAC_FAN_AUTO;
        break;
      default:
        this->ac_state.fan_level = PANAAC_FAN_AUTO;
        break;
    }
  } else {
    this->ac_state.fan_level = PANAAC_FAN_AUTO;
    this->fan_mode = climate::CLIMATE_FAN_AUTO;
  }
  this->ac_state.fan_mode = fan_level_to_standard(this->ac_state.fan_level);

  // Clamp an unsupported restored swing mode (horizontal not enabled) to a supported one.
  if (!this->swing_horizontal_ &&
      (this->swing_mode == climate::CLIMATE_SWING_BOTH || this->swing_mode == climate::CLIMATE_SWING_HORIZONTAL)) {
    this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
  }
  this->ac_state.swing_mode = this->swing_mode;
  bool swing_v_auto =
      (this->swing_mode == climate::CLIMATE_SWING_VERTICAL || this->swing_mode == climate::CLIMATE_SWING_BOTH);
  bool swing_h_auto = this->swing_horizontal_ && (this->swing_mode == climate::CLIMATE_SWING_HORIZONTAL ||
                                                  this->swing_mode == climate::CLIMATE_SWING_BOTH);
  this->ac_state.swing_v_pos = swing_v_auto ? PANAAC_SWINGV_AUTO : PANAAC_SWINGV_MIDDLE;
  this->ac_state.swing_h_pos =
      swing_h_auto ? PANAAC_SWINGH_AUTO : (this->swing_horizontal_ ? PANAAC_SWINGH_MIDDLE : PANAAC_SWINGH_NONE);
  this->ac_state.last_swing_v_pos = PANAAC_SWINGV_MIDDLE;
  this->ac_state.last_swing_h_pos = this->swing_horizontal_ ? PANAAC_SWINGH_MIDDLE : PANAAC_SWINGH_NONE;

  // Push the derived state back to the Climate fields / custom strings and the selects (no TX).
  this->sync_to_climate_();
  this->update_selects_();
  if (!this->mqtt_enabled_)
    this->publish_state();
}

void PanaACV2Climate::loop() {
  if (!this->mqtt_enabled_)
    return;
#ifdef USE_MQTT
  if (!this->is_connected())
    return;
  if (!this->traits_published_) {
    this->publish_traits_();
    this->traits_published_ = true;
    this->publish_state_();
  }
#endif
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

  // Fan modes (BOTH modes): standard Auto/Quiet enums + custom Level 1..5 strings (registered via
  // set_supported_custom_fan_modes() in setup()). Auto/Quiet as enums keeps the card duplicate-free
  // and lets ClimateCall::set_fan_mode("Auto"/"Quiet") route to the enum and pass validate_().
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_AUTO);
  if (this->supports_quiet_)
    traits.add_supported_fan_mode(climate::CLIMATE_FAN_QUIET);
  // The climate card carries the standard swing modes; the granular swing POSITIONS stay on the
  // Swing V/H selects / the v2 MQTT topics.
  traits.set_supported_swing_modes({climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL});
  if (this->swing_horizontal_) {
    traits.add_supported_swing_mode(climate::CLIMATE_SWING_HORIZONTAL);
    traits.add_supported_swing_mode(climate::CLIMATE_SWING_BOTH);
  }

  traits.set_visual_min_temperature(PANAAC_TEMP_MIN);
  traits.set_visual_max_temperature(PANAAC_TEMP_MAX);
  traits.set_visual_target_temperature_step(this->temp_step_);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

void PanaACV2Climate::dump_config() {
  ESP_LOGCONFIG(TAG, "PanaAC v2:");
  LOG_CLIMATE("  ", "Climate", this);
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->mqtt_enabled_ ? "v2 MQTT" : "v1 native");
  if (this->mqtt_enabled_)
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

// ---------------- State synchronization ----------------

void PanaACV2Climate::sync_to_climate_() {
  this->mode = ac_state.mode;
  this->target_temperature = ac_state.temp;

  // Fan (BOTH modes): Auto/Quiet → standard enum; levels → custom fan-mode string. Using the
  // protected setters (not assigning `this->fan_mode` directly) clears the opposite field so the
  // card never shows both an enum and a custom value.
  if (ac_state.fan_level == PANAAC_FAN_AUTO) {
    this->set_fan_mode_(climate::CLIMATE_FAN_AUTO);
  } else if (ac_state.fan_level == PANAAC_FAN_QUIET) {
    this->set_fan_mode_(climate::CLIMATE_FAN_QUIET);
  } else {
    const char *fan_str = fan_level_to_str(ac_state.fan_level);
    this->set_custom_fan_mode_(fan_str, strlen(fan_str));
  }

  if (this->mqtt_enabled_) {
    this->custom_swing_mode_ = swing_v_pos_to_str(ac_state.swing_v_pos);
    this->swing_horizontal_mode_ = this->swing_horizontal_ ? swing_h_pos_to_str(ac_state.swing_h_pos) : nullptr;
    // The visible "(PanaAC v1)" climate card advertises the standard swing modes, so mirror
    // ac_state.swing_mode (derived from the v/h positions by recompute_swing_mode_()) into the
    // Climate base field so the card reflects the current swing state.
    this->swing_mode = ac_state.swing_mode;
  } else {
    this->swing_mode = ac_state.swing_mode;
  }
}

void PanaACV2Climate::update_selects_() {
  if (this->swingv_ != nullptr)
    this->swingv_->set_swingvpos(ac_state.swing_v_pos);
  if (this->swing_horizontal_ && this->swingh_ != nullptr)
    this->swingh_->set_swinghpos(ac_state.swing_h_pos);
}

void PanaACV2Climate::publish_state_by_mode_() {
  if (this->mqtt_enabled_) {
#ifdef USE_MQTT
    this->publish_state_();
#else
    ESP_LOGE(TAG, "v2 MQTT mode requires a mqtt: block (USE_MQTT undefined)");
#endif
  } else {
    this->publish_state();
  }
}

// ---------------- Climate control ----------------

void PanaACV2Climate::control(const climate::ClimateCall &call) {
  bool changed = false;

  if (call.get_mode().has_value()) {
    auto mode = *call.get_mode();
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
    if (this->ac_state.mode != mode) {
      this->ac_state.mode = mode;
      changed = true;
    }
  }

  if (call.get_target_temperature().has_value()) {
    float value = *call.get_target_temperature();
    if (value < PANAAC_TEMP_MIN)
      value = PANAAC_TEMP_MIN;
    if (value > PANAAC_TEMP_MAX)
      value = PANAAC_TEMP_MAX;
    if (this->ac_state.temp != value) {
      this->ac_state.temp = value;
      changed = true;
    }
  }

  if (call.has_custom_fan_mode() || call.get_fan_mode().has_value()) {
    FanLevel level = this->ac_state.fan_level;  // preserve existing granular level within a group
    climate::ClimateFanMode std_mode = this->ac_state.fan_mode;
    if (call.has_custom_fan_mode()) {
      level = fan_level_from_str(call.get_custom_fan_mode().c_str());
      std_mode = fan_level_to_standard(level);
    } else {
      std_mode = *call.get_fan_mode();
      switch (std_mode) {
        case climate::CLIMATE_FAN_LOW:
          if (level != PANAAC_FAN_LEVEL_1 && level != PANAAC_FAN_LEVEL_2)
            level = PANAAC_FAN_LEVEL_1;
          break;
        case climate::CLIMATE_FAN_MEDIUM:
          if (level != PANAAC_FAN_LEVEL_3 && level != PANAAC_FAN_LEVEL_4)
            level = PANAAC_FAN_LEVEL_3;
          break;
        case climate::CLIMATE_FAN_HIGH:
          level = PANAAC_FAN_LEVEL_5;
          break;
        case climate::CLIMATE_FAN_QUIET:
          if (this->supports_quiet_) {
            level = PANAAC_FAN_QUIET;
          } else {
            level = PANAAC_FAN_AUTO;
            std_mode = climate::CLIMATE_FAN_AUTO;
          }
          break;
        case climate::CLIMATE_FAN_AUTO:
        default:
          level = PANAAC_FAN_AUTO;
          break;
      }
    }
    if (level == PANAAC_FAN_QUIET && !this->supports_quiet_) {
      ESP_LOGW(TAG, "Quiet fan mode not supported");
      level = PANAAC_FAN_AUTO;
      std_mode = climate::CLIMATE_FAN_AUTO;
    }
    if ((level == PANAAC_FAN_LEVEL_2 || level == PANAAC_FAN_LEVEL_4) && !this->fan_5level_) {
      ESP_LOGW(TAG, "Fan level %s requires 5-level support", fan_level_to_str(level));
      level = PANAAC_FAN_AUTO;
      std_mode = climate::CLIMATE_FAN_AUTO;
    }
    if (this->ac_state.fan_level != level) {
      this->ac_state.fan_level = level;
      this->ac_state.fan_mode = std_mode;
      changed = true;
    }
  }

  if (call.get_swing_mode().has_value()) {
    // Standard swing modes are handled in BOTH modes — the visible "(PanaAC v1)" climate card
    // has the same Off/Vertical/Horizontal/Both swing controls as PanaAC v1 (the granular swing
    // positions still live on the Swing V/H selects / the v2 MQTT topics). The v2 MQTT set path
    // never reaches here: it applies the Panasonic swing position strings directly in
    // on_set_json_() (via set_swing_mode_if_supported_ / set_swing_horizontal_mode_if_supported_).
    auto sm = *call.get_swing_mode();
    this->ac_state.swing_mode = sm;
    switch (sm) {
      case climate::CLIMATE_SWING_OFF:
        if (this->ac_state.swing_v_pos == PANAAC_SWINGV_AUTO)
          this->ac_state.swing_v_pos = PANAAC_SWINGV_MIDDLE;
        if (this->swing_horizontal_) {
          if (this->ac_state.swing_h_pos == PANAAC_SWINGH_AUTO)
            this->ac_state.swing_h_pos = PANAAC_SWINGH_MIDDLE;
        } else {
          this->ac_state.swing_h_pos = PANAAC_SWINGH_NONE;
        }
        break;
      case climate::CLIMATE_SWING_VERTICAL:
        this->ac_state.swing_v_pos = PANAAC_SWINGV_AUTO;
        if (this->swing_horizontal_) {
          if (this->ac_state.swing_h_pos == PANAAC_SWINGH_AUTO)
            this->ac_state.swing_h_pos = PANAAC_SWINGH_MIDDLE;
        } else {
          this->ac_state.swing_h_pos = PANAAC_SWINGH_NONE;
        }
        break;
      case climate::CLIMATE_SWING_HORIZONTAL:
        if (this->ac_state.swing_v_pos == PANAAC_SWINGV_AUTO)
          this->ac_state.swing_v_pos = PANAAC_SWINGV_MIDDLE;
        if (this->swing_horizontal_) {
          this->ac_state.swing_h_pos = PANAAC_SWINGH_AUTO;
        } else {
          this->ac_state.swing_h_pos = PANAAC_SWINGH_NONE;
          this->ac_state.swing_mode = climate::CLIMATE_SWING_OFF;
        }
        break;
      case climate::CLIMATE_SWING_BOTH:
      default:
        this->ac_state.swing_v_pos = PANAAC_SWINGV_AUTO;
        if (this->swing_horizontal_) {
          this->ac_state.swing_h_pos = PANAAC_SWINGH_AUTO;
        } else {
          this->ac_state.swing_h_pos = PANAAC_SWINGH_NONE;
          this->ac_state.swing_mode = climate::CLIMATE_SWING_VERTICAL;
        }
        break;
    }
    changed = true;
  }

  if (changed) {
    this->sync_to_climate_();
    // Publish state BEFORE the (blocking ~260 ms) IR transmit so Home Assistant's climate entity
    // reflects the change immediately; the physical IR send follows. transmit_data_() busy-waits
    // with interrupts disabled for the whole signal, so any publish after it would lag by the
    // full signal duration (the main lag the user saw).
    this->publish_state_by_mode_();
    this->update_selects_();
    this->transmit_data_();
  }
}

// Recompute ac_state.swing_mode from the granular v/h positions (used by the selects).
void PanaACV2Climate::recompute_swing_mode_() {
  bool v_auto = (this->ac_state.swing_v_pos == PANAAC_SWINGV_AUTO);
  bool h_auto = this->swing_horizontal_ && (this->ac_state.swing_h_pos == PANAAC_SWINGH_AUTO);
  if (v_auto && h_auto)
    this->ac_state.swing_mode = climate::CLIMATE_SWING_BOTH;
  else if (v_auto)
    this->ac_state.swing_mode = climate::CLIMATE_SWING_VERTICAL;
  else if (h_auto)
    this->ac_state.swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
  else
    this->ac_state.swing_mode = climate::CLIMATE_SWING_OFF;
}

// ---------------- Select entry points (both modes) ----------------

void PanaACV2Climate::apply_swingv_select_(SwingVPos pos) {
  this->ac_state.swing_v_pos = pos;
  if (pos != PANAAC_SWINGV_AUTO)
    this->ac_state.last_swing_v_pos = pos;
  this->recompute_swing_mode_();
  this->sync_to_climate_();
  this->publish_state_by_mode_();  // reflect before the blocking IR send
  this->update_selects_();
  this->transmit_data_();
}

void PanaACV2Climate::apply_swingh_select_(SwingHPos pos) {
  this->ac_state.swing_h_pos = pos;
  if (pos != PANAAC_SWINGH_AUTO)
    this->ac_state.last_swing_h_pos = pos;
  this->recompute_swing_mode_();
  this->sync_to_climate_();
  this->publish_state_by_mode_();  // reflect before the blocking IR send
  this->update_selects_();
  this->transmit_data_();
}

// ---------------- v2 MQTT swing set helpers ----------------

#ifdef USE_MQTT
bool PanaACV2Climate::set_swing_mode_if_supported_(const char *mode) {
  auto pos = swing_v_pos_from_str(mode);
  const char *desired = swing_v_pos_to_str(pos);
  if (this->ac_state.swing_v_pos != pos) {
    this->ac_state.swing_v_pos = pos;
    if (pos != PANAAC_SWINGV_AUTO)
      this->ac_state.last_swing_v_pos = pos;
    this->recompute_swing_mode_();
    this->custom_swing_mode_ = desired;
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
  if (this->ac_state.swing_h_pos != pos) {
    this->ac_state.swing_h_pos = pos;
    if (pos != PANAAC_SWINGH_AUTO)
      this->ac_state.last_swing_h_pos = pos;
    this->recompute_swing_mode_();
    this->swing_horizontal_mode_ = swing_h_pos_to_str(pos);
    return true;
  }
  return false;
}
#endif  // USE_MQTT

// ---------------- MQTT publish helpers (v2 only) ----------------

#ifdef USE_MQTT
void PanaACV2Climate::publish_state_() {
  this->publish_json(this->state_topic_(), [this](JsonObject root) {
    root["mode"] = mode_to_str(climate_mode_to_mode(this->ac_state.mode));
    root["target_temperature"] = this->ac_state.temp;
    root["fan_mode"] = fan_level_to_str(this->ac_state.fan_level);
    root["swing_mode"] = swing_v_pos_to_str(this->ac_state.swing_v_pos);
    if (this->swing_horizontal_)
      root["swing_horizontal_mode"] = swing_h_pos_to_str(this->ac_state.swing_h_pos);
    if (!std::isnan(this->current_temperature))
      root["current_temperature"] = this->current_temperature;
    root["available"] = true;
  }, 0, true);

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

    // 3-level ACs expose Level 1/3/5; 5-level ACs add Level 2 and 4 (see setup()).
    JsonArray fan_modes = root["fan_modes"].to<JsonArray>();
    fan_modes.add(STR_FAN_AUTO);
    fan_modes.add(STR_FAN_L1);
    if (this->fan_5level_)
      fan_modes.add(STR_FAN_L2);
    fan_modes.add(STR_FAN_L3);
    if (this->fan_5level_)
      fan_modes.add(STR_FAN_L4);
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
#endif  // USE_MQTT

// ---------------- Command handling (v2 MQTT set) ----------------

#ifdef USE_MQTT
void PanaACV2Climate::on_set_json_(const std::string &topic, JsonObject root) {
  ESP_LOGD(TAG, "Received command on %s", topic.c_str());

  auto call = this->make_call();
  bool swing_changed = false;

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

  // perform() -> control() applies mode/temp/fan to ac_state and transmits+p publishes if changed.
  call.perform();

  // Panasonic-specific swing strings are handled directly (ESPHome core climate has no custom
  // swing strings).
  if (root["swing_mode"].is<const char *>()) {
    if (this->set_swing_mode_if_supported_(root["swing_mode"]))
      swing_changed = true;
  }
  if (root["swing_horizontal_mode"].is<const char *>()) {
    if (this->set_swing_horizontal_mode_if_supported_(root["swing_horizontal_mode"]))
      swing_changed = true;
  }

  if (swing_changed) {
    this->sync_to_climate_();
    this->publish_state_by_mode_();  // reflect before the blocking IR send
    this->update_selects_();
    this->transmit_data_();
  }
}
#endif  // USE_MQTT

// ---------------- IR transmit ----------------

void PanaACV2Climate::transmit_data_() {
  static const std::array<uint8_t, 8> FIRST_FRAME = {0x02, 0x20, 0xE0, 0x04, 0x00, 0x00, 0x00, 0x06};
  std::array<uint8_t, 19> second_frame = {0x02, 0x20, 0xE0, 0x04, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
                                          0x00, 0x0E, 0xE0, 0x00, 0x00, 0x89, 0x00, 0x00, 0x00};

  // power & mode
  switch (this->ac_state.mode) {
    case climate::CLIMATE_MODE_COOL:
      second_frame[PANAAC_BYTEPOS_POWER] |= PANAAC_POWER_ON;
      second_frame[PANAAC_BYTEPOS_MODE] |= PANAAC_MODE_COOL;
      break;
    case climate::CLIMATE_MODE_HEAT:
      second_frame[PANAAC_BYTEPOS_POWER] |= PANAAC_POWER_ON;
      second_frame[PANAAC_BYTEPOS_MODE] |= PANAAC_MODE_HEAT;
      break;
    case climate::CLIMATE_MODE_DRY:
      second_frame[PANAAC_BYTEPOS_POWER] |= PANAAC_POWER_ON;
      second_frame[PANAAC_BYTEPOS_MODE] |= PANAAC_MODE_DRY;
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      second_frame[PANAAC_BYTEPOS_POWER] |= PANAAC_POWER_ON;
      second_frame[PANAAC_BYTEPOS_MODE] |= PANAAC_MODE_FAN_ONLY;
      break;
    case climate::CLIMATE_MODE_AUTO:
      second_frame[PANAAC_BYTEPOS_POWER] |= PANAAC_POWER_ON;
      second_frame[PANAAC_BYTEPOS_MODE] |= PANAAC_MODE_AUTO;
      break;
    case climate::CLIMATE_MODE_OFF:
    default:
      second_frame[PANAAC_BYTEPOS_POWER] |= PANAAC_POWER_OFF;
      second_frame[PANAAC_BYTEPOS_MODE] |= PANAAC_MODE_COOL;
      break;
  }

  // temperature
  uint8_t encoded_temp = static_cast<uint8_t>(this->ac_state.temp) - PANAAC_TEMP_MIN;
  encoded_temp &= 0x0F;
  second_frame[PANAAC_BYTEPOS_TEMP] = 0x20 | (encoded_temp << 1);
  if (static_cast<uint8_t>(this->ac_state.temp) < this->ac_state.temp)
    second_frame[PANAAC_BYTEPOS_TEMP] |= 0x01;

  // fan
  if (this->ac_state.fan_level == PANAAC_FAN_QUIET && this->supports_quiet_) {
    second_frame[PANAAC_BYTEPOS_QUIET] |= PANAAC_FAN_QUIET;
  }
  second_frame[PANAAC_BYTEPOS_FAN] |= this->ac_state.fan_level;

  // swing
  second_frame[PANAAC_BYTEPOS_SWINGV] |= this->ac_state.swing_v_pos;
  if (this->swing_horizontal_) {
    second_frame[PANAAC_BYTEPOS_SWINGH] |= this->ac_state.swing_h_pos;
  } else {
    second_frame[PANAAC_BYTEPOS_SWINGH] |= PANAAC_SWINGH_NONE;
  }

  // checksum
  for (uint8_t i = 0; i < 18; i++)
    second_frame[18] += second_frame[i];

#if (ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE)
  char hex[3 * 19 + 1];
  int p = 0;
  for (uint8_t b : second_frame)
    p += snprintf(hex + p, sizeof(hex) - p, "%02X ", b);
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

  this->publish_state_by_mode_();
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
        // zero
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
  for (size_t i = 0; i < state_len; i++)
    p += snprintf(hex + p, sizeof(hex) - p, "%02X ", state_bytes[i]);
  ESP_LOGV(TAG, "Command decoded: len = %d, data = [ %s]", state_len, hex);
#endif

  if (state_len == 27) {
    for (size_t i = 0; i < 19; i++)
      state_bytes[i] = state_bytes[i + 8];
    state_len = 19;
  }

  return true;
}

bool PanaACV2Climate::decode_state_(std::span<const uint8_t> state_bytes, ClimateState &state) {
  if (state_bytes.size() != 19)
    return false;

  if (state_bytes[0] != 0x02 || state_bytes[1] != 0x20 || state_bytes[2] != 0xE0 || state_bytes[3] != 0x04 ||
      state_bytes[4] != 0x00) {
    ESP_LOGV(TAG, "Invalid protocol");
    return false;
  }

  uint8_t checksum = 0;
  for (size_t i = 0; i < 18; i++)
    checksum += state_bytes[i];
  if (checksum != state_bytes[18]) {
    ESP_LOGV(TAG, "Invalid checksum");
    return false;
  }

  // operation mode
  if ((state_bytes[PANAAC_BYTEPOS_POWER] & PANAAC_POWER_MASK) == PANAAC_POWER_OFF) {
    state.mode = climate::CLIMATE_MODE_OFF;
  } else {
    switch (state_bytes[PANAAC_BYTEPOS_MODE] & 0xF0) {
      case PANAAC_MODE_DRY:
        state.mode = climate::CLIMATE_MODE_DRY;
        break;
      case PANAAC_MODE_COOL:
        state.mode = climate::CLIMATE_MODE_COOL;
        break;
      case PANAAC_MODE_HEAT:
        state.mode = climate::CLIMATE_MODE_HEAT;
        break;
      case PANAAC_MODE_FAN_ONLY:
        state.mode = climate::CLIMATE_MODE_FAN_ONLY;
        break;
      case PANAAC_MODE_AUTO:
      default:
        state.mode = climate::CLIMATE_MODE_AUTO;
        break;
    }
  }

  // temperature
  state.temp = ((state_bytes[PANAAC_BYTEPOS_TEMP] & 0x1E) >> 1) + PANAAC_TEMP_MIN;
  if ((state_bytes[PANAAC_BYTEPOS_TEMP] & 0x01) == 0x01)
    state.temp += 0.5;

  // fan
  switch (state_bytes[PANAAC_BYTEPOS_FAN] & 0xF0) {
    case PANAAC_FAN_LEVEL_1:
      state.fan_mode = climate::CLIMATE_FAN_LOW;
      state.fan_level = PANAAC_FAN_LEVEL_1;
      break;
    case PANAAC_FAN_LEVEL_2:
      state.fan_mode = climate::CLIMATE_FAN_LOW;
      state.fan_level = PANAAC_FAN_LEVEL_2;
      break;
    case PANAAC_FAN_LEVEL_3:
      state.fan_mode = climate::CLIMATE_FAN_MEDIUM;
      state.fan_level = PANAAC_FAN_LEVEL_3;
      break;
    case PANAAC_FAN_LEVEL_4:
      state.fan_mode = climate::CLIMATE_FAN_MEDIUM;
      state.fan_level = PANAAC_FAN_LEVEL_4;
      break;
    case PANAAC_FAN_LEVEL_5:
      state.fan_mode = climate::CLIMATE_FAN_HIGH;
      state.fan_level = PANAAC_FAN_LEVEL_5;
      break;
    case PANAAC_FAN_AUTO:
    default:
      state.fan_mode = climate::CLIMATE_FAN_AUTO;
      state.fan_level = PANAAC_FAN_AUTO;
      break;
  }
  if (this->supports_quiet_ && (state_bytes[PANAAC_BYTEPOS_QUIET] & 0xF0) == PANAAC_FAN_QUIET) {
    state.fan_mode = climate::CLIMATE_FAN_QUIET;
    state.fan_level = PANAAC_FAN_QUIET;
  }

  // swing
  uint8_t swing_v = state_bytes[PANAAC_BYTEPOS_SWINGV] & 0x0F;
  uint8_t swing_h = state_bytes[PANAAC_BYTEPOS_SWINGH] & 0x0F;
  state.swing_v_pos = static_cast<SwingVPos>(swing_v);
  state.swing_h_pos = static_cast<SwingHPos>(swing_h);
  if (!this->swing_horizontal_)
    swing_h = PANAAC_SWINGH_NONE;
  if (swing_v == PANAAC_SWINGV_AUTO && swing_h == PANAAC_SWINGH_AUTO)
    state.swing_mode = climate::CLIMATE_SWING_BOTH;
  else if (swing_v == PANAAC_SWINGV_AUTO)
    state.swing_mode = climate::CLIMATE_SWING_VERTICAL;
  else if (swing_h == PANAAC_SWINGH_AUTO)
    state.swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
  else
    state.swing_mode = climate::CLIMATE_SWING_OFF;

  return true;
}

bool PanaACV2Climate::decode_and_apply_(std::span<const uint8_t> state_bytes) {
  ClimateState decoded{};
  if (!this->decode_state_(state_bytes, decoded))
    return false;

  // Reject unsupported modes from the remote.
  if (decoded.mode == climate::CLIMATE_MODE_HEAT && !this->supports_heat_) {
    ESP_LOGV(TAG, "Heat mode not supported");
    return false;
  }
  if (decoded.mode == climate::CLIMATE_MODE_FAN_ONLY && !this->supports_fan_only_) {
    ESP_LOGV(TAG, "Fan-only mode not supported");
    return false;
  }
  if (decoded.mode == climate::CLIMATE_MODE_COOL && !this->supports_cool_) {
    ESP_LOGV(TAG, "Cool mode not supported");
    return false;
  }

  // A 3-level AC cannot represent Level 2/4: discard those and keep the current fan level so the
  // UI does not jump to "Auto" (the granular select / climate card keep their last value).
  if ((decoded.fan_level == PANAAC_FAN_LEVEL_2 || decoded.fan_level == PANAAC_FAN_LEVEL_4) && !this->fan_5level_) {
    ESP_LOGV(TAG, "Fan level %s requires 5-level support; ignoring", fan_level_to_str(decoded.fan_level));
    decoded.fan_level = this->ac_state.fan_level;
    decoded.fan_mode = this->ac_state.fan_mode;
  }

  this->ac_state = decoded;
  this->sync_to_climate_();
  this->update_selects_();
  return true;
}

}  // namespace esphome::panaac_v2