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
from esphome.components import climate, remote_base, sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MQTT_ID, CONF_SENSOR
from esphome.components.remote_base import CONF_RECEIVER_ID, CONF_TRANSMITTER_ID

AUTO_LOAD = ["mqtt", "climate"]
DEPENDENCIES = ["mqtt", "remote_transmitter", "sensor", "climate"]

panaac_v2_ns = cg.esphome_ns.namespace('panaac_v2')
PanaACV2Climate = panaac_v2_ns.class_(
    'PanaACV2Climate',
    climate.Climate,
    cg.Component,
    remote_base.RemoteReceiverListener,
    remote_base.RemoteTransmittable,
)

CONF_TOPIC_PREFIX = "topic_prefix"
CONF_SUPPORTS_COOL = "supports_cool"
CONF_SUPPORTS_HEAT = "supports_heat"
CONF_SUPPORTS_FAN_ONLY = "supports_fan_only"
CONF_SUPPORTS_QUIET = "supports_quiet"
CONF_FAN_5LEVEL = "fan_5level"
CONF_SWING_HORIZONTAL = "swing_horizontal"
CONF_TEMP_STEP = "temp_step"
CONF_IR_CONTROL = "ir_control"

CONFIG_SCHEMA = climate.climate_schema(PanaACV2Climate).extend({
    cv.Required(CONF_RECEIVER_ID): cv.use_id(remote_base.RemoteReceiverBase),
    cv.Required(CONF_TRANSMITTER_ID): cv.use_id(remote_base.RemoteTransmitterBase),
    cv.Required(CONF_TOPIC_PREFIX): cv.string,
    cv.Optional(CONF_SUPPORTS_COOL, default=True): cv.boolean,
    cv.Optional(CONF_SUPPORTS_HEAT, default=False): cv.boolean,
    cv.Optional(CONF_SUPPORTS_FAN_ONLY, default=False): cv.boolean,
    cv.Optional(CONF_SUPPORTS_QUIET, default=False): cv.boolean,
    cv.Optional(CONF_FAN_5LEVEL, default=False): cv.boolean,
    cv.Optional(CONF_SWING_HORIZONTAL, default=False): cv.boolean,
    cv.Optional(CONF_TEMP_STEP, default=1.0): cv.float_range(min=0.5, max=1.0),
    cv.Optional(CONF_IR_CONTROL, default=False): cv.boolean,
    cv.Optional(CONF_SENSOR): cv.use_id(sensor.Sensor),
}).extend(cv.COMPONENT_SCHEMA).extend(remote_base.REMOTE_TRANSMITTABLE_SCHEMA).extend(remote_base.REMOTE_LISTENER_SCHEMA)


async def to_code(config):
    # panaac_v2 uses its own CustomMQTTDevice topics; prevent the climate platform
    # from creating a second standard MQTT climate component.
    config.pop(CONF_MQTT_ID, None)

    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await remote_base.register_listener(var, config)
    await remote_base.register_transmittable(var, config)

    cg.add(var.set_topic_prefix(config[CONF_TOPIC_PREFIX]))
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
