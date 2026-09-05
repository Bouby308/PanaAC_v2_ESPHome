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
      // Legacy mode only (Quiet as a fan speed); special-mode models never hold this level.
      return climate::CLIMATE_FAN_QUIET;
    case PANAAC_FAN_AUTO:
    default:
      return climate::CLIMATE_FAN_AUTO;
  }
}

inline bool mode_supports_preset(climate::ClimateMode mode) {
  return mode == climate::CLIMATE_MODE_AUTO || mode == climate::CLIMATE_MODE_COOL ||
         mode == climate::CLIMATE_MODE_DRY;
}

inline climate::ClimatePreset preset_to_climate(Preset preset) {
  switch (preset) {
    case PANAAC_PRESET_POWERFUL:
      return climate::CLIMATE_PRESET_BOOST;
    case PANAAC_PRESET_QUIET:
      // Quiet is a special mode on this fork's target models; the standard climate preset enum
      // has no "quiet", so SLEEP (shown as "Sleep" in Home Assistant) carries it.
      return climate::CLIMATE_PRESET_SLEEP;
    case PANAAC_PRESET_ECO:
      return climate::CLIMATE_PRESET_ECO;
    case PANAAC_PRESET_NONE:
    default:
      return climate::CLIMATE_PRESET_NONE;
  }
}

inline bool climate_preset_to_special(climate::ClimatePreset preset, Preset &special) {
  switch (preset) {
    case climate::CLIMATE_PRESET_BOOST:
      special = PANAAC_PRESET_POWERFUL;
      return true;
    case climate::CLIMATE_PRESET_SLEEP:
      special = PANAAC_PRESET_QUIET;
      return true;
    case climate::CLIMATE_PRESET_ECO:
      special = PANAAC_PRESET_ECO;
      return true;
    case climate::CLIMATE_PRESET_NONE:
      special = PANAAC_PRESET_NONE;
      return true;
    default:
      return false;
  }
}

inline bool preset_is_supported(Preset preset, bool supports_powerful, bool supports_quiet, bool supports_eco) {
  return preset == PANAAC_PRESET_NONE || (preset == PANAAC_PRESET_POWERFUL && supports_powerful) ||
         (preset == PANAAC_PRESET_QUIET && supports_quiet) || (preset == PANAAC_PRESET_ECO && supports_eco);
}

/// The fixed 8-byte "wake" frame every Panasonic AC command burst starts with.
const std::array<uint8_t, 8> PANAAC_FIRST_FRAME = {{0x02, 0x20, 0xE0, 0x04, 0x00, 0x00, 0x00, 0x06}};

/// Emit one complete IR frame (header + LSB-first bits + end mark).
void write_frame(remote_base::RemoteTransmitData *data, const uint8_t *bytes, size_t len) {
  data->mark(PANAAC_HEADER_MARK);
  data->space(PANAAC_HEADER_SPACE);
  for (size_t i = 0; i < len; i++) {
    for (uint8_t i_bit = 0; i_bit < 8; i_bit++) {
      data->mark(PANAAC_BIT_MARK);
      bool bit = bytes[i] & (1 << i_bit);
      data->space(bit ? PANAAC_ONE_SPACE : PANAAC_ZERO_SPACE);
    }
  }
  data->mark(PANAAC_BIT_MARK);
  data->space(PANAAC_FRAME_END);
}

/// The command frame to TOGGLE for a special-mode transition old -> new. The physical buttons are
/// toggles: enabling a mode and disabling it send the SAME frame, and pressing the other special
/// mode's button switches to it directly. So the frame is the new mode's when activating and the
/// old mode's when deactivating (new == NONE).
inline const std::array<uint8_t, 8> *special_mode_toggle_frame(Preset previous, Preset next) {
  const Preset target = next != PANAAC_PRESET_NONE ? next : previous;
  if (target == PANAAC_PRESET_POWERFUL)
    return &PANAAC_CMD_POWERFUL;
  if (target == PANAAC_PRESET_QUIET)
    return &PANAAC_CMD_QUIET;
  return nullptr;  // eco / none
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
  this->ac_state.preset = PANAAC_PRESET_NONE;
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
  // levels are climate custom fan modes now.) The SwingV select only exists when the unit has
  // vertical swing; the SwingH select only when swing_horizontal is enabled.
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

    // Auto-configure the MQTT availability (birth / last-will / shutdown) on the PanaAC v2
    // availability topic so the PanaAC v2 HA custom integration reliably sees online/offline
    // (incl. broker restart, device crash, and graceful shutdown) WITHOUT the user having to set
    // birth_message/will_message/shutdown_message in the `mqtt:` block. This runs in setup(),
    // before the MQTT client connects in loop(), so the last-will is part of the CONNECT packet.
    // It overrides anything ESPHome defaulted (to the mqtt component's `<prefix>/status`) or the
    // user set in the `mqtt:` block — in v2 mode this component owns the availability topic.
    // global_mqtt_client is assigned in the MQTTClientComponent constructor, which the generated
    // setup() runs before App.setup() dispatches this component's setup(), so it is non-null here.
    if (mqtt::global_mqtt_client != nullptr) {
      const std::string avail_topic = this->availability_topic_();
      mqtt::global_mqtt_client->set_last_will(mqtt::MQTTMessage{avail_topic, "offline", 0, true});
      mqtt::global_mqtt_client->set_birth_message(mqtt::MQTTMessage{avail_topic, "online", 0, true});
      mqtt::global_mqtt_client->set_shutdown_message(mqtt::MQTTMessage{avail_topic, "offline", 0, true});
    }
#endif
  }
  if (this->sensor_ != nullptr) {
    this->sensor_->add_on_state_callback([this](float state) {
      if (!std::isnan(state)) {
        this->current_temperature = state;
        // Room temperature drives the AUTO action inference, so recompute before publishing.
        this->update_action_();
        this->publish_state_by_mode_();
      }
    });
  }

  // Restore previous Climate state (mode/target_temperature/fan_mode/swing_mode) from flash.
  if (auto state = this->restore_state_())
    state->apply(this);

  // Restore the special mode + powerful countdown from our own preferences (these are the source
  // of truth across ESP restarts, not the climate's generic preset restore, which cannot express
  // a toggle-mode's remaining timer). In legacy mode no special mode exists, so these prefs are
  // never loaded or saved (the generic climate restore above already carries the preset).
  this->pref_special_mode_ = global_preferences->make_preference<uint32_t>(fnv1_hash("paacv2_spm"), true);
  this->pref_powerful_remaining_ = global_preferences->make_preference<uint32_t>(fnv1_hash("paacv2_pwr"), true);

  // SM_PERSIST_NONE / legacy: the special mode is not persisted through our prefs at all.
  uint32_t stored_mode = PANAAC_PRESET_NONE;
  const bool has_stored_mode = this->powerful_quiet_mode_ != PQ_MODE_LEGACY &&
                               this->special_mode_persistence_ != SM_PERSIST_NONE &&
                               this->pref_special_mode_.load(&stored_mode);

  // Derive canonical ac_state from the restored Climate fields (do NOT transmit on boot — the
  // AC must not be commanded just because the ESP booted).
  this->ac_state.mode = this->mode;
  Preset restored_special = PANAAC_PRESET_NONE;
  if (has_stored_mode) {
    restored_special = static_cast<Preset>(stored_mode);
  } else if (this->powerful_quiet_mode_ == PQ_MODE_LEGACY && this->preset.has_value()) {
    // Legacy: map the preset from the climate's generic flash restore. Quiet is a fan level in
    // legacy mode (never a preset), so SLEEP is ignored here.
    restored_special = *this->preset == climate::CLIMATE_PRESET_BOOST ? PANAAC_PRESET_POWERFUL
                     : *this->preset == climate::CLIMATE_PRESET_ECO   ? PANAAC_PRESET_ECO
                                                                      : PANAAC_PRESET_NONE;
  }
  if (!preset_is_supported(restored_special, this->supports_powerful_, this->supports_quiet_,
                           this->supports_eco_) ||
      !mode_supports_preset(this->ac_state.mode))
    restored_special = PANAAC_PRESET_NONE;
  if (this->powerful_quiet_mode_ != PQ_MODE_LEGACY && restored_special == PANAAC_PRESET_POWERFUL) {
    // Re-arm the 4h countdown. SM_PERSIST_FULL has the persisted remaining time;
    // SM_PERSIST_MODE_ONLY does not store it, so resume with a fresh full countdown (the mode
    // itself — what MODE_ONLY guarantees — stays correct, only the timer restarts).
    uint32_t remaining_s = PANAAC_POWERFUL_TIMEOUT_MS / 1000u;
    if (this->special_mode_persistence_ == SM_PERSIST_FULL)
      this->pref_powerful_remaining_.load(&remaining_s);
    const uint32_t remaining_ms = remaining_s * 1000u;
    if (remaining_ms == 0 || remaining_ms > PANAAC_POWERFUL_TIMEOUT_MS) {
      restored_special = PANAAC_PRESET_NONE;
    } else {
      this->powerful_deadline_ms_ = millis() + remaining_ms;
      this->next_powerful_persist_ms_ = millis() + PANAAC_POWERFUL_PERSIST_INTERVAL_MS;
    }
  }
  this->ac_state.preset = restored_special;
  if (this->powerful_quiet_mode_ != PQ_MODE_LEGACY && this->special_mode_persistence_ != SM_PERSIST_NONE &&
      (!has_stored_mode || restored_special != stored_mode)) {
    // Boot-time demotion (unsupported/garbage value, or an expired/stale countdown): rewrite flash
    // so the corrected value survives the next restart too (one write, only when demoted).
    this->persist_special_mode_();
  }
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
        // Quiet is a fan level only in legacy mode; in special mode it is a special mode, so an
        // old restored "Quiet" fan enum falls back to Auto.
        this->ac_state.fan_level =
            this->powerful_quiet_mode_ == PQ_MODE_LEGACY ? PANAAC_FAN_QUIET : PANAAC_FAN_AUTO;
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

  // Clamp unsupported restored swing modes to supported ones. Units without vertical swing keep
  // the swing nibble at a fixed harmless position (AUTO) and never expose vertical/both swing.
  if (!this->swing_vertical_ &&
      (this->swing_mode == climate::CLIMATE_SWING_BOTH || this->swing_mode == climate::CLIMATE_SWING_VERTICAL)) {
    this->swing_mode = this->swing_horizontal_ ? climate::CLIMATE_SWING_HORIZONTAL : climate::CLIMATE_SWING_OFF;
  }
  if (!this->swing_horizontal_ &&
      (this->swing_mode == climate::CLIMATE_SWING_BOTH || this->swing_mode == climate::CLIMATE_SWING_HORIZONTAL)) {
    this->swing_mode = this->swing_vertical_ ? climate::CLIMATE_SWING_VERTICAL : climate::CLIMATE_SWING_OFF;
  }
  this->ac_state.swing_mode = this->swing_mode;
  bool swing_v_auto = this->swing_vertical_ &&
                      (this->swing_mode == climate::CLIMATE_SWING_VERTICAL ||
                       this->swing_mode == climate::CLIMATE_SWING_BOTH);
  bool swing_h_auto = this->swing_horizontal_ && (this->swing_mode == climate::CLIMATE_SWING_HORIZONTAL ||
                                                  this->swing_mode == climate::CLIMATE_SWING_BOTH);
  this->ac_state.swing_v_pos = !this->swing_vertical_   ? PANAAC_SWINGV_AUTO
                               : swing_v_auto           ? PANAAC_SWINGV_AUTO
                                                        : PANAAC_SWINGV_MIDDLE;
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
  // Special-mode timer runs in BOTH modes (v1 native and v2 MQTT).
  this->update_powerful_timer_();

  if (!this->mqtt_enabled_)
    return;
#ifdef USE_MQTT
  if (!this->is_connected()) {
    // While disconnected, mark the retained bootstrap as due so the traits + state payloads are
    // republished on the next reconnect — not only on the first boot-time connect (Codex review
    // issue 1). This keeps Home Assistant able to rebuild the entity after a broker restart,
    // retained-message loss, or a late device reconnect: the bootstrap runs again as soon as the
    // broker connection comes back.
    this->traits_published_ = false;
    return;
  }
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

  // Fan modes (BOTH modes): standard Auto enum + custom Level 1..5 strings (registered via
  // set_supported_custom_fan_modes() in setup()). In powerful_quiet: special, Quiet is NOT a fan
  // mode — it is a special mode exposed through the preset list below. In powerful_quiet: legacy
  // (upstream semantics), Quiet IS a fan speed: the standard CLIMATE_FAN_QUIET enum.
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_AUTO);
  if (this->supports_quiet_ && this->powerful_quiet_mode_ == PQ_MODE_LEGACY)
    traits.add_supported_fan_mode(climate::CLIMATE_FAN_QUIET);
  // The climate card carries the standard swing modes; the granular swing POSITIONS stay on the
  // Swing V/H selects / the v2 MQTT topics. Units without vertical swing advertise neither
  // VERTICAL nor BOTH (only OFF + HORIZONTAL when horizontal swing exists).
  traits.set_supported_swing_modes({climate::CLIMATE_SWING_OFF});
  if (this->swing_vertical_)
    traits.add_supported_swing_mode(climate::CLIMATE_SWING_VERTICAL);
  if (this->swing_horizontal_) {
    traits.add_supported_swing_mode(climate::CLIMATE_SWING_HORIZONTAL);
    if (this->swing_vertical_)
      traits.add_supported_swing_mode(climate::CLIMATE_SWING_BOTH);
  }
  // Special modes (Powerful / Quiet) + Eco are exposed as climate presets. The standard preset
  // enum has no "Quiet", so CLIMATE_PRESET_SLEEP ("Sleep" in Home Assistant) carries it — only in
  // powerful_quiet: special. In legacy mode Powerful stays a preset (it rides the state-frame bit)
  // but Quiet is advertised as a fan mode instead, so SLEEP is not offered.
  const bool legacy_pq = this->powerful_quiet_mode_ == PQ_MODE_LEGACY;
  if (this->supports_powerful_ || (this->supports_quiet_ && !legacy_pq) || this->supports_eco_) {
    traits.add_supported_preset(climate::CLIMATE_PRESET_NONE);
    if (this->supports_powerful_)
      traits.add_supported_preset(climate::CLIMATE_PRESET_BOOST);
    if (this->supports_quiet_ && !legacy_pq)
      traits.add_supported_preset(climate::CLIMATE_PRESET_SLEEP);
    if (this->supports_eco_)
      traits.add_supported_preset(climate::CLIMATE_PRESET_ECO);
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
  ESP_LOGCONFIG(TAG, "  Supports quiet (special mode): %s", YESNO(this->supports_quiet_));
  ESP_LOGCONFIG(TAG, "  Supports powerful (special mode): %s", YESNO(this->supports_powerful_));
  ESP_LOGCONFIG(TAG, "  Supports eco: %s", YESNO(this->supports_eco_));
  ESP_LOGCONFIG(TAG, "  Swing vertical: %s", YESNO(this->swing_vertical_));
  ESP_LOGCONFIG(TAG, "  Swing horizontal: %s", YESNO(this->swing_horizontal_));
  static const char *const PERSIST_NAMES[] = {"none", "mode_only", "full"};
  ESP_LOGCONFIG(TAG, "  Powerful/quiet: %s",
                this->powerful_quiet_mode_ == PQ_MODE_LEGACY ? "legacy (quiet fan speed, state-frame powerful bit)"
                                                             : "special (toggle command frames)");
  if (this->powerful_quiet_mode_ != PQ_MODE_LEGACY)
    ESP_LOGCONFIG(TAG, "  Special mode persistence: %s", PERSIST_NAMES[this->special_mode_persistence_]);
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

  // Fan (BOTH modes): Auto → standard enum; levels → custom fan-mode string. Using the
  // protected setters (not assigning `this->fan_mode` directly) clears the opposite field so the
  // card never shows both an enum and a custom value. In powerful_quiet: special, Powerful/Quiet
  // are NOT fan modes (they ride the climate preset below); in legacy mode the Quiet fan level is
  // exposed as the standard CLIMATE_FAN_QUIET enum instead of a custom string.
  if (ac_state.fan_level == PANAAC_FAN_AUTO ||
      (this->powerful_quiet_mode_ == PQ_MODE_LEGACY && ac_state.fan_level == PANAAC_FAN_QUIET)) {
    this->set_fan_mode_(ac_state.fan_level == PANAAC_FAN_QUIET ? climate::CLIMATE_FAN_QUIET
                                                               : climate::CLIMATE_FAN_AUTO);
  } else {
    const char *fan_str = fan_level_to_str(ac_state.fan_level);
    this->set_custom_fan_mode_(fan_str, strlen(fan_str));
  }

  if (this->mqtt_enabled_) {
    this->custom_swing_mode_ = swing_v_pos_to_str(ac_state.swing_v_pos);
    this->swing_horizontal_mode_ = this->swing_horizontal_ ? swing_h_pos_to_str(ac_state.swing_h_pos) : nullptr;
    // The visible "(v1)" climate card advertises the standard swing modes, so mirror
    // ac_state.swing_mode (derived from the v/h positions by recompute_swing_mode_()) into the
    // Climate base field so the card reflects the current swing state.
    this->swing_mode = ac_state.swing_mode;
  } else {
    this->swing_mode = ac_state.swing_mode;
  }

  this->set_preset_(preset_to_climate(ac_state.preset));
  this->update_action_();
}

void PanaACV2Climate::update_action_() {
  // The controller is a one-way IR transmitter: it cannot read back whether the unit's compressor
  // is actually running, so assume the commanded mode is the unit's real state and mirror it into
  // `action` (the ClimateAction the lambda `id(x).action` and the native API expose). AUTO has no
  // fixed action, so infer it from the room vs. setpoint temperature — the same comparison the AC's
  // own thermostat makes — and fall back to IDLE when the room is at setpoint or the temperature is
  // unknown.
  switch (this->ac_state.mode) {
    case climate::CLIMATE_MODE_OFF:
      this->action = climate::CLIMATE_ACTION_OFF;
      break;
    case climate::CLIMATE_MODE_COOL:
      this->action = climate::CLIMATE_ACTION_COOLING;
      break;
    case climate::CLIMATE_MODE_HEAT:
      this->action = climate::CLIMATE_ACTION_HEATING;
      break;
    case climate::CLIMATE_MODE_DRY:
      this->action = climate::CLIMATE_ACTION_DRYING;
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      this->action = climate::CLIMATE_ACTION_FAN;
      break;
    case climate::CLIMATE_MODE_AUTO:
      if (!std::isnan(this->current_temperature) && this->target_temperature != 0.0f) {
        if (this->current_temperature > this->target_temperature)
          this->action = climate::CLIMATE_ACTION_COOLING;
        else if (this->current_temperature < this->target_temperature)
          this->action = climate::CLIMATE_ACTION_HEATING;
        else
          this->action = climate::CLIMATE_ACTION_IDLE;
      } else {
        this->action = climate::CLIMATE_ACTION_IDLE;
      }
      break;
    default:
      this->action = climate::CLIMATE_ACTION_IDLE;
      break;
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
  bool invalid_custom_fan = false;
  // Special-mode bookkeeping: special_before lets the tail detect a change; special_frame is the
  // ONLY toggle command that gets emitted (set explicitly in the preset branch — the power-on and
  // mode-incompatibility clears are state-only because the AC clears the mode itself there).
  const Preset special_before = this->ac_state.preset;
  const std::array<uint8_t, 8> *special_frame = nullptr;
  if (call.has_custom_fan_mode()) {
    FanLevel requested_fan;
    invalid_custom_fan = !parse_fan_level(call.get_custom_fan_mode().c_str(), requested_fan);
    if (invalid_custom_fan)
      ESP_LOGW(TAG, "Unsupported custom fan mode: %s", call.get_custom_fan_mode().c_str());
  }

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
      const bool power_on_edge = this->ac_state.mode == climate::CLIMATE_MODE_OFF &&
                                 mode != climate::CLIMATE_MODE_OFF;
      this->ac_state.mode = mode;
      changed = true;
      // Powering the AC on clears any previous special mode on the unit itself (per the CW-SU70AA
      // behavior): mirror that locally WITHOUT sending anything. Legacy models do not have the
      // toggle special modes, so nothing to clear there.
      if (power_on_edge && this->powerful_quiet_mode_ != PQ_MODE_LEGACY &&
          this->ac_state.preset != PANAAC_PRESET_NONE) {
        ESP_LOGD(TAG, "Power-on clears special mode %s", preset_to_str(this->ac_state.preset));
        this->set_special_mode_(PANAAC_PRESET_NONE);
      }
    }
  }

  // Special modes (Powerful / Quiet) + Eco: mutually exclusive, carried by the climate preset.
  if (!mode_supports_preset(this->ac_state.mode) && this->ac_state.preset != PANAAC_PRESET_NONE)
    this->set_special_mode_(PANAAC_PRESET_NONE);
  if (call.get_preset().has_value()) {
    Preset requested = PANAAC_PRESET_NONE;
    const bool valid_preset = climate_preset_to_special(*call.get_preset(), requested);
    if (!valid_preset ||
        !preset_is_supported(requested, this->supports_powerful_, this->supports_quiet_, this->supports_eco_) ||
        (requested != PANAAC_PRESET_NONE && !mode_supports_preset(this->ac_state.mode))) {
      ESP_LOGW(TAG, "Unsupported or incompatible preset command");
    } else if (requested != this->ac_state.preset) {
      if (this->powerful_quiet_mode_ == PQ_MODE_LEGACY) {
        // Legacy: the preset is plain state carried by the full state frame's byte-13 bits
        // (Powerful / Eco), so it just marks the frame dirty — no toggle command frames,
        // no 4h timer, no special-mode flash persistence (set_special_mode_ no-ops there).
        this->ac_state.preset = requested;
        changed = true;
      } else {
        special_frame = special_mode_toggle_frame(this->ac_state.preset, requested);
        this->set_special_mode_(requested);
      }
    }
  }
  if (call.get_target_temperature().has_value()) {
    float value = normalize_target_temperature(*call.get_target_temperature(), this->temp_step_);
    if (this->ac_state.temp != value) {
      this->ac_state.temp = value;
      changed = true;
    }
  }

  if ((call.has_custom_fan_mode() || call.get_fan_mode().has_value()) && !invalid_custom_fan) {
    FanLevel level = this->ac_state.fan_level;  // preserve existing granular level within a group
    climate::ClimateFanMode std_mode = this->ac_state.fan_mode;
    if (call.has_custom_fan_mode()) {
      parse_fan_level(call.get_custom_fan_mode().c_str(), level);
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
          if (this->powerful_quiet_mode_ == PQ_MODE_LEGACY) {
            // Legacy: Quiet is a real fan speed (byte-8 nibble 0x20).
            level = PANAAC_FAN_QUIET;
          } else {
            ESP_LOGW(TAG, "Quiet is a special mode here; use the preset instead. Falling back to Auto");
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
    // Standard swing modes are handled in BOTH modes — the visible "(v1)" climate card
    // has the same Off/Vertical/Horizontal/Both swing controls as PanaAC v1 (the granular swing
    // positions still live on the Swing V/H selects / the v2 MQTT topics). The v2 MQTT set path
    // never reaches here: it applies the Panasonic swing position strings directly in
    // on_set_json_() (via set_swing_mode_if_supported_ / set_swing_horizontal_mode_if_supported_).
    auto sm = *call.get_swing_mode();
    // Clamp requested swing to what the unit physically has. Without vertical swing the v
    // position keeps its fixed harmless value and is never touched.
    if (!this->swing_vertical_ &&
        (sm == climate::CLIMATE_SWING_VERTICAL || sm == climate::CLIMATE_SWING_BOTH)) {
      sm = this->swing_horizontal_ ? climate::CLIMATE_SWING_HORIZONTAL : climate::CLIMATE_SWING_OFF;
    }
    this->ac_state.swing_mode = sm;
    switch (sm) {
      case climate::CLIMATE_SWING_OFF:
        if (this->swing_vertical_ && this->ac_state.swing_v_pos == PANAAC_SWINGV_AUTO)
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
        if (this->swing_vertical_ && this->ac_state.swing_v_pos == PANAAC_SWINGV_AUTO)
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
        if (this->swing_vertical_)
          this->ac_state.swing_v_pos = PANAAC_SWINGV_AUTO;
        if (this->swing_horizontal_) {
          this->ac_state.swing_h_pos = PANAAC_SWINGH_AUTO;
        } else {
          this->ac_state.swing_h_pos = PANAAC_SWINGH_NONE;
          this->ac_state.swing_mode = this->swing_vertical_ ? climate::CLIMATE_SWING_VERTICAL
                                                            : climate::CLIMATE_SWING_OFF;
        }
        break;
    }
    changed = true;
  }

  const bool special_changed = this->ac_state.preset != special_before;
  if (changed || special_changed) {
    this->sync_to_climate_();
    if (this->mqtt_command_active_) {
      // A v2 MQTT set command is applying multiple fields atomically. control() is reached here
      // via call.perform() from on_set_json_(); while mqtt_command_active_ is set it updates
      // ac_state and records a pending change but does NOT publish or transmit, so a single MQTT
      // command can never trigger two back-to-back IR bursts (Codex review issue 2). The single
      // publish/transmit runs in on_set_json_() once all fields are applied.
      this->pending_change_ = true;
    } else {
      // Publish state BEFORE the (blocking ~260 ms) IR transmit so Home Assistant's climate entity
      // reflects the change immediately; the physical IR send follows. transmit_data_() busy-waits
      // with interrupts disabled for the whole signal, so any publish after it would lag by the
      // full signal duration (the main lag the user saw).
      this->publish_state_by_mode_();
      this->update_selects_();
      // Normal state changes go out as the full 19-byte state frame; special-mode transitions go
      // out as their short toggle command frame only (mirroring the physical remote buttons).
      if (changed)
        this->transmit_data_();
      if (special_frame != nullptr)
        this->transmit_command_frame_(*special_frame);
    }
  }
}

// Recompute ac_state.swing_mode from the granular v/h positions (used by the selects).
void PanaACV2Climate::recompute_swing_mode_() {
  bool v_auto = this->swing_vertical_ && (this->ac_state.swing_v_pos == PANAAC_SWINGV_AUTO);
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
  if (!this->swing_vertical_) {
    ESP_LOGW(TAG, "Vertical swing not supported on this unit");
    return;
  }
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
  if (!this->swing_vertical_) {
    ESP_LOGW(TAG, "Vertical swing not supported on this unit");
    return false;
  }
  SwingVPos pos;
  if (!parse_swing_v_pos(mode, pos)) {
    ESP_LOGW(TAG, "Unsupported vertical swing mode: %s", mode);
    return false;
  }
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
  SwingHPos pos;
  if (!parse_swing_h_pos(mode, pos)) {
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
    if (this->supports_powerful_ || this->supports_quiet_ || this->supports_eco_)
      root["preset_mode"] = preset_to_str(this->ac_state.preset);
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
    // Legacy mode: Quiet is a fan speed, not a special mode.
    if (this->supports_quiet_ && this->powerful_quiet_mode_ == PQ_MODE_LEGACY)
      fan_modes.add(STR_FAN_QUIET);

    JsonArray swing_modes = root["swing_modes"].to<JsonArray>();
    if (this->swing_vertical_) {
      swing_modes.add(STR_SWINGV_AUTO);
      swing_modes.add(STR_SWINGV_HIGHEST);
      swing_modes.add(STR_SWINGV_HIGH);
      swing_modes.add(STR_SWINGV_MIDDLE);
      swing_modes.add(STR_SWINGV_LOW);
      swing_modes.add(STR_SWINGV_LOWEST);
    }

    const bool legacy_pq = this->powerful_quiet_mode_ == PQ_MODE_LEGACY;
    if (this->supports_powerful_ || (this->supports_quiet_ && !legacy_pq) || this->supports_eco_) {
      JsonArray preset_modes = root["preset_modes"].to<JsonArray>();
      preset_modes.add(STR_PRESET_NONE);
      if (this->supports_powerful_)
        preset_modes.add(STR_PRESET_POWERFUL);
      if (this->supports_quiet_ && !legacy_pq)
        preset_modes.add(STR_PRESET_QUIET);
      if (this->supports_eco_)
        preset_modes.add(STR_PRESET_ECO);
    }
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

  // Apply the whole command to ac_state in memory, then emit at most ONE publish/transmit cycle
  // if anything actually changed. While mqtt_command_active_ is set, the control() path reached
  // via call.perform() below updates ac_state and records a pending change but does NOT publish
  // or transmit — so a command mixing standard climate fields (mode / target_temperature /
  // fan_mode) with Panasonic swing fields cannot trigger two back-to-back IR transmissions and
  // two state publishes (Codex review issue 2). The swing helpers already mutate ac_state without
  // transmitting; the single emit runs at the end.
  this->mqtt_command_active_ = true;
  this->pending_change_ = false;

  auto call = this->make_call();
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
  // perform() -> control() applies mode/temp/fan to ac_state (no transmit while active).
  call.perform();

  const Preset special_before = this->ac_state.preset;
  if (root["preset_mode"].is<const char *>()) {
    Preset requested = PANAAC_PRESET_NONE;
    const char *value = root["preset_mode"];
    if (!parse_preset(value, requested) ||
        !preset_is_supported(requested, this->supports_powerful_, this->supports_quiet_, this->supports_eco_) ||
        (requested != PANAAC_PRESET_NONE && !mode_supports_preset(this->ac_state.mode))) {
      ESP_LOGW(TAG, "Unsupported or incompatible preset command: %s", value);
    } else if (requested == PANAAC_PRESET_QUIET && this->powerful_quiet_mode_ == PQ_MODE_LEGACY) {
      ESP_LOGW(TAG, "Legacy mode: Quiet is a fan speed, not a preset; send it as fan_mode \"Quiet\" instead");
    } else if (requested != this->ac_state.preset) {
      this->set_special_mode_(requested);
      this->pending_change_ = true;
    }
  }

  // Panasonic-specific swing strings are applied directly (ESPHome core climate has no custom
  // swing strings); these helpers update ac_state and recompute the combined swing_mode but do
  // not transmit.
  bool swing_changed = false;
  if (root["swing_mode"].is<const char *>()) {
    if (this->set_swing_mode_if_supported_(root["swing_mode"]))
      swing_changed = true;
  }
  if (root["swing_horizontal_mode"].is<const char *>()) {
    if (this->set_swing_horizontal_mode_if_supported_(root["swing_horizontal_mode"]))
      swing_changed = true;
  }

  // Exactly one emit for the whole command, only if something actually changed.
  this->mqtt_command_active_ = false;
  const bool special_changed = this->ac_state.preset != special_before;
  if (this->pending_change_ || swing_changed) {
    this->sync_to_climate_();
    this->publish_state_by_mode_();  // reflect before the blocking IR send
    this->update_selects_();
    this->transmit_data_();
    if (special_changed && this->powerful_quiet_mode_ != PQ_MODE_LEGACY) {
      // Legacy: the preset change already rode the full state frame above.
      const auto *cmd = special_mode_toggle_frame(special_before, this->ac_state.preset);
      if (cmd != nullptr)
        this->transmit_command_frame_(*cmd);
    }
  }
}
#endif  // USE_MQTT

// ---------------- IR transmit ----------------

void PanaACV2Climate::transmit_command_frame_(const std::array<uint8_t, 8> &command) {
#if (ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE)
  char hex[3 * 8 + 1];
  int p = 0;
  for (uint8_t b : command)
    p += snprintf(hex + p, sizeof(hex) - p, "%02X ", b);
  ESP_LOGV(TAG, "Sending Panasonic AC command frame: [ %s]", hex);
#endif

  auto transmit = this->transmitter_->transmit();
  auto *data = transmit.get_data();
  if (this->ir_control_)
    data->set_carrier_frequency(PANAAC_IR_TRANSMIT_FREQ);

  write_frame(data, PANAAC_FIRST_FRAME.data(), PANAAC_FIRST_FRAME.size());
  write_frame(data, command.data(), command.size());

  transmit.perform();
  // Ignore the receiver echo of this burst (see rx_ignore_until_ms_).
  this->rx_ignore_until_ms_ = millis() + PANAAC_RX_SELF_IGNORE_MS;
}

void PanaACV2Climate::transmit_data_() {
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
  second_frame[PANAAC_BYTEPOS_FAN] |= this->ac_state.fan_level;

  // Panasonic preset bits. In powerful_quiet: special, POWERFUL and QUIET are special modes sent
  // as separate short command frames (transmit_command_frame_), so only the ECO bit rides the full
  // state frame. In powerful_quiet: legacy (upstream semantics), POWERFUL rides its byte-13 bit.
  if (this->ac_state.preset == PANAAC_PRESET_ECO && this->supports_eco_)
    second_frame[PANAAC_BYTEPOS_ECO] |= PANAAC_ECO;
  if (this->powerful_quiet_mode_ == PQ_MODE_LEGACY && this->ac_state.preset == PANAAC_PRESET_POWERFUL &&
      this->supports_powerful_)
    second_frame[PANAAC_BYTEPOS_POWERFUL] |= PANAAC_POWERFUL;

  // swing
  if (!this->swing_vertical_ && this->swing_horizontal_) {
    // Units with no vertical vane (e.g. CW-SU70AA): captures of the real remote's AIR SWING button
    // show it toggles the byte-8 vane nibble (0x0F = swinging, 0x03 = fixed at middle) while the
    // byte-9 horizontal position stays pinned at 0x0D AUTO. The single swing axis is therefore
    // driven through the vane nibble, NOT through the horizontal byte.
    second_frame[PANAAC_BYTEPOS_SWINGV] |= (this->ac_state.swing_h_pos == PANAAC_SWINGH_AUTO)
                                               ? PANAAC_SWINGV_AUTO
                                               : PANAAC_SWINGV_MIDDLE;
    second_frame[PANAAC_BYTEPOS_SWINGH] |= PANAAC_SWINGH_AUTO;
  } else {
    second_frame[PANAAC_BYTEPOS_SWINGV] |= this->ac_state.swing_v_pos;
    if (this->swing_horizontal_) {
      second_frame[PANAAC_BYTEPOS_SWINGH] |= this->ac_state.swing_h_pos;
    } else {
      second_frame[PANAAC_BYTEPOS_SWINGH] |= PANAAC_SWINGH_NONE;
    }
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

  // First frame + 2nd (state) frame.
  write_frame(data, PANAAC_FIRST_FRAME.data(), PANAAC_FIRST_FRAME.size());
  write_frame(data, second_frame.data(), second_frame.size());

  transmit.perform();
  // Ignore the receiver echo of this burst (see rx_ignore_until_ms_).
  this->rx_ignore_until_ms_ = millis() + PANAAC_RX_SELF_IGNORE_MS;
}

// ---------------- IR receive ----------------

bool PanaACV2Climate::on_receive(remote_base::RemoteReceiveData data) {
  // Drop the receiver's echo of a burst we just sent. Without this, the toggle command frames we
  // transmit for Powerful/Quiet would be mirrored straight back and flip the mode back to its
  // previous value, so Home Assistant would show the special mode as reset.
  if (static_cast<int32_t>(millis() - this->rx_ignore_until_ms_) < 0) {
    ESP_LOGV(TAG, "Ignored %d raw items (echo of our own transmit)", data.get_raw_data().size());
    return false;
  }
  const auto &raw_data = data.get_raw_data();
  ESP_LOGV(TAG, "Received raw data size = %d", raw_data.size());

  if (raw_data.size() == 132) {  // fixed 1st frame OR an 8-byte special-mode command frame
    std::array<uint8_t, 8> bytes{};
    if (!this->decode_short_frame_(data, bytes)) {
      ESP_LOGV(TAG, "Decode short frame failed");
      return false;
    }
    if (bytes == PANAAC_FIRST_FRAME) {
      ESP_LOGV(TAG, "Ignored first frame!");
      return false;
    }
    if (this->powerful_quiet_mode_ == PQ_MODE_LEGACY) {
      // Legacy models have no toggle special modes; upstream never consumes short command frames.
      ESP_LOGV(TAG, "Legacy powerful/quiet mode: short command frame ignored");
      return false;
    }
    // Special-mode toggle command from the physical remote: mirror the AC's own toggle semantics
    // in our tracked state so HA stays in sync even when the user uses the real remote.
    Preset next = PANAAC_PRESET_NONE;
    if (bytes == PANAAC_CMD_POWERFUL)
      next = this->ac_state.preset == PANAAC_PRESET_POWERFUL ? PANAAC_PRESET_NONE : PANAAC_PRESET_POWERFUL;
    else if (bytes == PANAAC_CMD_QUIET)
      next = this->ac_state.preset == PANAAC_PRESET_QUIET ? PANAAC_PRESET_NONE : PANAAC_PRESET_QUIET;
    else {
      ESP_LOGV(TAG, "Unknown short frame, ignored");
      return false;
    }
    if (!preset_is_supported(next, this->supports_powerful_, this->supports_quiet_, this->supports_eco_) ||
        !mode_supports_preset(this->ac_state.mode)) {
      ESP_LOGV(TAG, "Special mode not applicable in the current configuration/HVAC mode; ignored");
      return false;
    }
    if (this->set_special_mode_(next)) {
      this->sync_to_climate_();
      this->publish_state_by_mode_();
      this->update_selects_();
    }
    return true;
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

bool PanaACV2Climate::decode_short_frame_(remote_base::RemoteReceiveData &data, std::array<uint8_t, 8> &bytes) {
  if (!data.expect_item(PANAAC_HEADER_MARK, PANAAC_HEADER_SPACE)) {
    ESP_LOGV(TAG, "Invalid short frame - expected header");
    return false;
  }
  for (size_t i = 0; i < bytes.size(); i++) {
    uint8_t byte = 0;
    for (uint8_t a_bit = 0; a_bit < 8; a_bit++) {
      if (data.expect_item(PANAAC_BIT_MARK, PANAAC_ONE_SPACE)) {
        byte |= 1 << a_bit;
      } else if (data.expect_item(PANAAC_BIT_MARK, PANAAC_ZERO_SPACE)) {
        // zero
      } else {
        ESP_LOGV(TAG, "Invalid bit %d of byte %d, index = %d", a_bit, i, data.get_index());
        return false;
      }
    }
    bytes[i] = byte;
  }
  uint8_t checksum = 0;
  for (size_t i = 0; i + 1 < bytes.size(); i++)
    checksum += bytes[i];
  if (checksum != bytes.back()) {
    ESP_LOGV(TAG, "Invalid short frame checksum");
    return false;
  }
#if (ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE)
  char hex[3 * 8 + 1];
  int p = 0;
  for (uint8_t b : bytes)
    p += snprintf(hex + p, sizeof(hex) - p, "%02X ", b);
  ESP_LOGV(TAG, "Short frame decoded: [ %s]", hex);
#endif
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
    case PANAAC_FAN_QUIET:
      // Quiet fan speed only exists in legacy mode; special-mode remotes never send this nibble,
      // so treat it as Auto there.
      if (this->powerful_quiet_mode_ == PQ_MODE_LEGACY) {
        state.fan_mode = climate::CLIMATE_FAN_QUIET;
        state.fan_level = PANAAC_FAN_QUIET;
      } else {
        state.fan_mode = climate::CLIMATE_FAN_AUTO;
        state.fan_level = PANAAC_FAN_AUTO;
      }
      break;
    case PANAAC_FAN_AUTO:
    default:
      state.fan_mode = climate::CLIMATE_FAN_AUTO;
      state.fan_level = PANAAC_FAN_AUTO;
      break;
  }
  // Special modes: the full state frame only carries the legacy POWERFUL/ECO bits (Quiet rides
  // the short command frames on this fork's models). When no bit is set the caller's pre-seeded
  // state.preset (the currently tracked special mode) is preserved.
  const bool powerful = (state_bytes[PANAAC_BYTEPOS_POWERFUL] & PANAAC_POWERFUL) != 0;
  const bool eco = (state_bytes[PANAAC_BYTEPOS_ECO] & PANAAC_ECO) != 0;
  if (powerful && eco) {
    ESP_LOGV(TAG, "Invalid frame: POWERFUL and ECO are both active");
    return false;
  }
  if (powerful || eco) {
    state.preset = powerful ? PANAAC_PRESET_POWERFUL : PANAAC_PRESET_ECO;
    if (!preset_is_supported(state.preset, this->supports_powerful_, this->supports_quiet_,
                             this->supports_eco_) ||
        !mode_supports_preset(state.mode)) {
      ESP_LOGV(TAG, "Preset is unsupported or incompatible with the decoded HVAC mode");
      return false;
    }
  }
  // swing
  uint8_t swing_v = state_bytes[PANAAC_BYTEPOS_SWINGV] & 0x0F;
  uint8_t swing_h = state_bytes[PANAAC_BYTEPOS_SWINGH] & 0x0F;
  if (!this->swing_vertical_ && this->swing_horizontal_) {
    // Vane-less unit (e.g. CW-SU70AA): mirror of the transmit_data_ special case — the remote
    // drives the single swing axis via the byte-8 vane nibble (0x0F swinging / 1..5 fixed) and
    // leaves byte 9 at a constant 0x0D, so the vane nibble says whether the unit is swinging and
    // the horizontal byte says nothing.
    if (!is_valid_swing_v_pos(swing_v)) {
      ESP_LOGV(TAG, "Invalid vane swing value: 0x%02X", swing_v);
      return false;
    }
    state.swing_v_pos = PANAAC_SWINGV_AUTO;  // axis not present; keep the fixed harmless value
    state.swing_h_pos = (swing_v == PANAAC_SWINGV_AUTO) ? PANAAC_SWINGH_AUTO : PANAAC_SWINGH_MIDDLE;
    state.swing_mode =
        (swing_v == PANAAC_SWINGV_AUTO) ? climate::CLIMATE_SWING_HORIZONTAL : climate::CLIMATE_SWING_OFF;
    return true;
  }
  if (this->swing_vertical_) {
    if (!is_valid_swing_v_pos(swing_v)) {
      ESP_LOGV(TAG, "Invalid vertical swing value: 0x%02X", swing_v);
      return false;
    }
  } else {
    swing_v = PANAAC_SWINGV_AUTO;  // unit has no vertical swing; keep the fixed harmless value
  }
  if (this->swing_horizontal_) {
    if (!is_valid_swing_h_pos(swing_h) || swing_h == PANAAC_SWINGH_NONE) {
      ESP_LOGV(TAG, "Invalid horizontal swing value: 0x%02X", swing_h);
      return false;
    }
  } else {
    swing_h = PANAAC_SWINGH_NONE;
  }
  state.swing_v_pos = static_cast<SwingVPos>(swing_v);
  state.swing_h_pos = static_cast<SwingHPos>(swing_h);
  // "Auto" only means swinging when the axis is actually supported (v_pos is forced to AUTO on
  // units without the axis), so a vertical swing mode is never derived for a v-less unit.
  const bool v_swinging = this->swing_vertical_ && swing_v == PANAAC_SWINGV_AUTO;
  const bool h_swinging = this->swing_horizontal_ && swing_h == PANAAC_SWINGH_AUTO;
  if (v_swinging && h_swinging)
    state.swing_mode = climate::CLIMATE_SWING_BOTH;
  else if (v_swinging)
    state.swing_mode = climate::CLIMATE_SWING_VERTICAL;
  else if (h_swinging)
    state.swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
  else
    state.swing_mode = climate::CLIMATE_SWING_OFF;

  return true;
}

bool PanaACV2Climate::decode_and_apply_(std::span<const uint8_t> state_bytes) {
  const Preset previous_preset = this->ac_state.preset;
  ClimateState decoded{};
  // Special mode: a full state frame cannot express it (Powerful/Quiet ride the short toggle
  // command frames on this fork's models), so seed with the currently tracked preset;
  // decode_state_ overrides it only when the POWERFUL/ECO bit is set in the frame. In legacy mode
  // the preset is fully frame-expressed (byte-13 bits), so seed NONE instead.
  decoded.preset = this->powerful_quiet_mode_ == PQ_MODE_LEGACY ? PANAAC_PRESET_NONE : previous_preset;
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

  // The AC clears Powerful/Quiet by itself when it turns off and when it powers on again, so
  // mirror that as a state-only change on either power edge (no IR). The power-ON clear is
  // special-mode behavior only; legacy frames express the preset by their bits anyway.
  const bool power_on_edge = this->ac_state.mode == climate::CLIMATE_MODE_OFF &&
                             decoded.mode != climate::CLIMATE_MODE_OFF;
  if (decoded.mode == climate::CLIMATE_MODE_OFF ||
      (power_on_edge && this->powerful_quiet_mode_ != PQ_MODE_LEGACY))
    decoded.preset = PANAAC_PRESET_NONE;

  const Preset final_preset = decoded.preset;
  this->ac_state = decoded;
  // set_special_mode_ detects the change relative to the previous preset (re-route through it so
  // the 4h Powerful timer and flash persistence follow), then sync derives the Climate preset.
  this->ac_state.preset = previous_preset;
  this->set_special_mode_(final_preset);
  this->sync_to_climate_();
  this->update_selects_();
  return true;
}

// ---------------- Special-mode (Powerful / Quiet) management ----------------

bool PanaACV2Climate::set_special_mode_(Preset next) {
  if (this->ac_state.preset == next)
    return false;
  const bool was_powerful = this->ac_state.preset == PANAAC_PRESET_POWERFUL;
  this->ac_state.preset = next;
  if (this->powerful_quiet_mode_ == PQ_MODE_LEGACY)
    return true;  // no 4h timer, no flash persistence: legacy presets ride the state-frame bits
  if (next == PANAAC_PRESET_POWERFUL) {
    // (Re)arm the 4h auto-off timer the AC runs itself, and persist the full countdown.
    this->powerful_deadline_ms_ = millis() + PANAAC_POWERFUL_TIMEOUT_MS;
    this->next_powerful_persist_ms_ = millis() + PANAAC_POWERFUL_PERSIST_INTERVAL_MS;
  } else if (was_powerful) {
    this->powerful_deadline_ms_ = 0;
  }
  this->persist_special_mode_();
  return true;
}

void PanaACV2Climate::update_powerful_timer_() {
  if (this->powerful_quiet_mode_ == PQ_MODE_LEGACY)
    return;  // legacy models: Powerful is plain state, no 4h auto-clear is assumed
  if (this->ac_state.preset != PANAAC_PRESET_POWERFUL)
    return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - this->powerful_deadline_ms_) >= 0) {
    // The AC cancels Powerful on its own after 4h; only reflect it locally (no IR). Wraparound-
    // safe comparison; also catches the post-restart case where the deadline was in the past.
    ESP_LOGI(TAG, "Powerful auto-off after 4h (state-only, no IR)");
    this->set_special_mode_(PANAAC_PRESET_NONE);
    this->sync_to_climate_();
    this->publish_state_by_mode_();
    return;
  }
  // Only SM_PERSIST_FULL refreshes the remaining time periodically; MODE_ONLY / NONE never do.
  if (this->special_mode_persistence_ == SM_PERSIST_FULL &&
      static_cast<int32_t>(now - this->next_powerful_persist_ms_) >= 0) {
    // Re-persist the remaining time so the countdown survives an ESP restart.
    this->next_powerful_persist_ms_ = now + PANAAC_POWERFUL_PERSIST_INTERVAL_MS;
    this->persist_special_mode_();
  }
}

void PanaACV2Climate::persist_special_mode_() {
  if (this->powerful_quiet_mode_ == PQ_MODE_LEGACY || this->special_mode_persistence_ == SM_PERSIST_NONE)
    return;  // legacy has no special mode / RAM only: never touch flash (user opted out)
  const uint32_t mode = this->ac_state.preset;
  uint32_t stored_mode = UINT32_MAX;
  const bool mode_dirty = !this->pref_special_mode_.load(&stored_mode) || stored_mode != mode;
  const bool refresh_timer = this->special_mode_persistence_ == SM_PERSIST_FULL &&
                             this->ac_state.preset == PANAAC_PRESET_POWERFUL;
  if (!mode_dirty && !refresh_timer)
    return;
  this->pref_special_mode_.save(&mode);
  const uint32_t remaining_s = refresh_timer ? (this->powerful_deadline_ms_ - millis() + 999) / 1000
                                              : 0;
  this->pref_powerful_remaining_.save(&remaining_s);
}

}  // namespace esphome::panaac_v2
