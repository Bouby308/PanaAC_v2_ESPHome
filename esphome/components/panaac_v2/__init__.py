# Copyright 2025 Minh Hoang
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import esphome.codegen as cg
from esphome.components import climate, remote_base, select, sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_DISABLED_BY_DEFAULT,
    CONF_ICON,
    CONF_ID,
    CONF_MQTT_ID,
    CONF_NAME,
    CONF_SENSOR,
)
from esphome.components.remote_base import CONF_RECEIVER_ID, CONF_TRANSMITTER_ID

AUTO_LOAD = ["climate", "select"]
DEPENDENCIES = ["remote_transmitter", "climate"]
# `mqtt` is a soft dependency: the v2 MQTT features (topic_prefix) require a `mqtt:` block, which
# defines USE_MQTT and compiles in the CustomMQTTDevice code path. Without a `mqtt:` block
# (v1 native mode) USE_MQTT is undefined and all MQTT code is compiled out, so no broker is
# needed — the component behaves like PanaAC v1.

panaac_v2_ns = cg.esphome_ns.namespace('panaac_v2')
PanaACV2Climate = panaac_v2_ns.class_(
    'PanaACV2Climate',
    climate.Climate,
    cg.Component,
    remote_base.RemoteReceiverListener,
    remote_base.RemoteTransmittable,
)
PanaACV2FanLevel = panaac_v2_ns.class_('PanaACV2FanLevel', select.Select, cg.Component)
PanaACV2SwingV = panaac_v2_ns.class_('PanaACV2SwingV', select.Select, cg.Component)
PanaACV2SwingH = panaac_v2_ns.class_('PanaACV2SwingH', select.Select, cg.Component)

CONF_TOPIC_PREFIX = "topic_prefix"
CONF_SUPPORTS_COOL = "supports_cool"
CONF_SUPPORTS_HEAT = "supports_heat"
CONF_SUPPORTS_FAN_ONLY = "supports_fan_only"
CONF_SUPPORTS_QUIET = "supports_quiet"
CONF_FAN_5LEVEL = "fan_5level"
CONF_SWING_HORIZONTAL = "swing_horizontal"
CONF_TEMP_STEP = "temp_step"
CONF_IR_CONTROL = "ir_control"

CONF_FANLEVEL_ID = "fanlevel_id"
CONF_SWINGV_ID = "swingv_id"
CONF_SWINGH_ID = "swingh_id"

CONFIG_SCHEMA = climate.climate_schema(PanaACV2Climate).extend({
    cv.Required(CONF_RECEIVER_ID): cv.use_id(remote_base.RemoteReceiverBase),
    cv.Required(CONF_TRANSMITTER_ID): cv.use_id(remote_base.RemoteTransmitterBase),
    cv.Optional(CONF_TOPIC_PREFIX): cv.string,
    cv.Optional(CONF_SUPPORTS_COOL, default=True): cv.boolean,
    cv.Optional(CONF_SUPPORTS_HEAT, default=False): cv.boolean,
    cv.Optional(CONF_SUPPORTS_FAN_ONLY, default=False): cv.boolean,
    cv.Optional(CONF_SUPPORTS_QUIET, default=False): cv.boolean,
    cv.Optional(CONF_FAN_5LEVEL, default=False): cv.boolean,
    cv.Optional(CONF_SWING_HORIZONTAL, default=False): cv.boolean,
    cv.Optional(CONF_TEMP_STEP, default=1.0): cv.float_range(min=0.5, max=1.0),
    cv.Optional(CONF_IR_CONTROL, default=False): cv.boolean,
    cv.Optional(CONF_SENSOR): cv.use_id(sensor.Sensor),
    cv.GenerateID(CONF_FANLEVEL_ID): cv.declare_id(PanaACV2FanLevel),
    cv.GenerateID(CONF_SWINGV_ID): cv.declare_id(PanaACV2SwingV),
    cv.GenerateID(CONF_SWINGH_ID): cv.declare_id(PanaACV2SwingH),
}).extend(cv.COMPONENT_SCHEMA).extend(remote_base.REMOTE_TRANSMITTABLE_SCHEMA).extend(remote_base.REMOTE_LISTENER_SCHEMA)


async def _make_select(select_id, config, name, icon, parent):
    """Create one companion select with the (PanaAC v1) name suffix, an icon, and the climate's
    device_id (issue #15 grouping). Options are filled at runtime in PanaACV2Climate::setup()."""
    cfg = {CONF_ID: select_id, CONF_NAME: name, CONF_ICON: icon, CONF_DISABLED_BY_DEFAULT: False}
    if CONF_DEVICE_ID in config:  # issue #15: group selects with the climate's (sub-)device
        cfg[CONF_DEVICE_ID] = config[CONF_DEVICE_ID]
    sel = cg.new_Pvariable(select_id)
    await select.register_select(sel, cfg, options=[])
    await cg.register_component(sel, cfg)
    cg.add(sel.set_parent_climate(parent))
    return sel


async def to_code(config):
    mqtt_enabled = CONF_TOPIC_PREFIX in config

    # In v2 mode the climate is exposed via the custom PanaAC v2 MQTT topics; prevent the
    # climate platform from also creating a standard MQTT climate component. In v1 mode keep
    # it so the climate gets standard MQTT discovery (like PanaAC v1) when a `mqtt:` block is
    # present.
    if mqtt_enabled:
        config.pop(CONF_MQTT_ID, None)

    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await remote_base.register_listener(var, config)
    await remote_base.register_transmittable(var, config)

    cg.add(var.set_mqtt_enabled(mqtt_enabled))
    if mqtt_enabled:
        cg.add(var.set_topic_prefix(config[CONF_TOPIC_PREFIX]))
        # Hide the on-device climate from the native API / standard MQTT discovery so HA only
        # sees the PanaAC v2 custom-integration climate (over the custom MQTT topics). The
        # object stays usable by lambdas / restore_state.
        cg.add(var.set_internal(True))

    cg.add(var.set_supports_cool(config[CONF_SUPPORTS_COOL]))
    cg.add(var.set_supports_heat(config[CONF_SUPPORTS_HEAT]))
    cg.add(var.set_supports_fan_only(config[CONF_SUPPORTS_FAN_ONLY]))
    cg.add(var.set_supports_quiet(config[CONF_SUPPORTS_QUIET]))
    cg.add(var.set_fan_5level(config[CONF_FAN_5LEVEL]))
    cg.add(var.set_swing_horizontal(config[CONF_SWING_HORIZONTAL]))
    cg.add(var.set_temp_step(config[CONF_TEMP_STEP]))
    cg.add(var.set_ir_control(config[CONF_IR_CONTROL]))
    if sensor_id := config.get(CONF_SENSOR):
        sens = await cg.get_variable(sensor_id)
        cg.add(var.set_sensor(sens))

    # Three companion selects (PanaAC v1 features) — created in BOTH modes.
    fanlevel = await _make_select(config[CONF_FANLEVEL_ID], config, "Fan Level (PanaAC v1)",
                                  "mdi:fan", var)
    cg.add(var.set_fanlevel(fanlevel))
    swingv = await _make_select(config[CONF_SWINGV_ID], config, "Swing Vertical (PanaAC v1)",
                                "mdi:arrow-split-vertical", var)
    cg.add(var.set_swingv(swingv))
    if config[CONF_SWING_HORIZONTAL]:
        swingh = await _make_select(config[CONF_SWINGH_ID], config, "Swing Horizontal (PanaAC v1)",
                                    "mdi:arrow-split-horizontal", var)
        cg.add(var.set_swingh(swingh))