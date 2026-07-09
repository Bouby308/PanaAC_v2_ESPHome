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

#include "extra.h"
#include "panaac_v2.h"

namespace esphome::panaac_v2 {

// ---------------- PanaACV2SwingV ----------------

void PanaACV2SwingV::dump_config() {
  ESP_LOGCONFIG(TAG, "PanaACV2SwingV (PanaAC v1):");
  LOG_SELECT("  Swing Vertical: ", "swingv", this);
}

void PanaACV2SwingV::control(const std::string &value) {
  ESP_LOGI(TAG, "Swing Vertical selected: %s", value.c_str());
  this->climate_->apply_swingv_select_(swing_v_pos_from_str(value.c_str()));
}

void PanaACV2SwingV::setup() {}

void PanaACV2SwingV::set_swingvpos(SwingVPos swingvpos) {
  this->publish_state(swing_v_pos_to_str(swingvpos));
}

// ---------------- PanaACV2SwingH ----------------

void PanaACV2SwingH::dump_config() {
  ESP_LOGCONFIG(TAG, "PanaACV2SwingH (PanaAC v1):");
  LOG_SELECT("  Swing Horizontal: ", "swingh", this);
}

void PanaACV2SwingH::control(const std::string &value) {
  ESP_LOGI(TAG, "Swing Horizontal selected: %s", value.c_str());
  SwingHPos pos = swing_h_pos_from_str(value.c_str());
  if (pos == PANAAC_SWINGH_NONE)
    return;
  this->climate_->apply_swingh_select_(pos);
}

void PanaACV2SwingH::setup() {}

void PanaACV2SwingH::set_swinghpos(SwingHPos swinghpos) {
  if (swinghpos == PANAAC_SWINGH_NONE)
    return;
  this->publish_state(swing_h_pos_to_str(swinghpos));
}

}  // namespace esphome::panaac_v2