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
#include "esphome/components/select/select.h"
#include "esphome/core/component.h"
#include <cinttypes>

namespace esphome::panaac_v2 {

class PanaACV2Climate;

/// Companion `select` entities that expose the granular Panasonic swing positions that the
/// standard Climate swing enum can't represent. These are created in BOTH v1 (native) and v2
/// (MQTT) modes — they are the "PanaAC v1" controls (named "Swing Vertical"/"Swing Horizontal"). Each
/// `control()` delegates to the parent climate, which mutates the canonical `ac_state`,
/// transmits the IR frame, publishes, and re-syncs the other select.
/// (Fan levels are NOT a select — they are the climate's custom fan modes.)
class PanaACV2SwingV : public select::Select, public Component {
 public:
  void setup() override;
  void dump_config() override;
  void control(const std::string &value) override;
  void set_parent_climate(PanaACV2Climate *climate) { this->climate_ = climate; }
  void set_swingvpos(SwingVPos swingvpos);

 protected:
  PanaACV2Climate *climate_{nullptr};
};

class PanaACV2SwingH : public select::Select, public Component {
 public:
  void setup() override;
  void dump_config() override;
  void control(const std::string &value) override;
  void set_parent_climate(PanaACV2Climate *climate) { this->climate_ = climate; }
  void set_swinghpos(SwingHPos swinghpos);

 protected:
  PanaACV2Climate *climate_{nullptr};
};

}  // namespace esphome::panaac_v2