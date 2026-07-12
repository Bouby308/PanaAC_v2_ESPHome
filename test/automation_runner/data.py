"""Static test data for the PanaAC v2 ESPHome automation runner."""

from __future__ import annotations

from typing import Any

DEFAULT_TOPIC_PREFIX = "panaac_v2/esphome-panaac-v2"
DEFAULT_MQTT_HOST = "127.0.0.1"
DEFAULT_MQTT_PORT = 1883

SUITE_CHOICES = ("esphome.g1", "esphome.g2", "esphome.g3")
SUITE_LABELS = {
    "esphome.g1": "ESPHome Group 1 - Variant config/compile and config contract",
    "esphome.g2": "ESPHome Group 2 - MQTT publish/command round-trip",
    "esphome.g3": "ESPHome Group 3 - ESPHome automation surface",
}

VARIANT_ORDER = ("C1", "C2", "C3", "C4", "C5", "C6", "C3-automation")

VARIANT_EXPECTATIONS: dict[str, dict[str, Any]] = {
    "C1": {
        "config_mode": "v2",
        "topic_prefix": DEFAULT_TOPIC_PREFIX,
        "supports_cool": True,
        "supports_heat": False,
        "supports_fan_only": False,
        "supports_quiet": False,
        "fan_5level": False,
        "swing_horizontal": False,
        "temp_step": 1.0,
        "has_sensor": False,
    },
    "C2": {
        "config_mode": "v2",
        "topic_prefix": DEFAULT_TOPIC_PREFIX,
        "supports_cool": True,
        "supports_heat": True,
        "supports_fan_only": False,
        "supports_quiet": False,
        "fan_5level": False,
        "swing_horizontal": False,
        "temp_step": 0.5,
        "has_sensor": False,
    },
    "C3": {
        "config_mode": "v2",
        "topic_prefix": DEFAULT_TOPIC_PREFIX,
        "supports_cool": True,
        "supports_heat": True,
        "supports_fan_only": True,
        "supports_quiet": True,
        "fan_5level": True,
        "swing_horizontal": True,
        "temp_step": 0.5,
        "has_sensor": True,
    },
    "C4": {
        "config_mode": "v1",
        "topic_prefix": None,
        "supports_cool": True,
        "supports_heat": True,
        "supports_fan_only": False,
        "supports_quiet": False,
        "fan_5level": True,
        "swing_horizontal": True,
        "temp_step": 0.5,
        "has_sensor": True,
    },
    "C5": {
        "config_mode": "v2",
        "topic_prefix": DEFAULT_TOPIC_PREFIX,
        "supports_cool": True,
        "supports_heat": False,
        "supports_fan_only": False,
        "supports_quiet": False,
        "fan_5level": True,
        "swing_horizontal": True,
        "temp_step": 1.0,
        "has_sensor": True,
    },
    "C6": {
        "config_mode": "v2",
        "topic_prefix": DEFAULT_TOPIC_PREFIX,
        "supports_cool": True,
        "supports_heat": True,
        "supports_fan_only": False,
        "supports_quiet": True,
        "fan_5level": False,
        "swing_horizontal": False,
        "temp_step": 0.5,
        "has_sensor": False,
    },
    "C3-automation": {
        "config_mode": "v2",
        "topic_prefix": DEFAULT_TOPIC_PREFIX,
        "supports_cool": True,
        "supports_heat": True,
        "supports_fan_only": True,
        "supports_quiet": True,
        "fan_5level": True,
        "swing_horizontal": True,
        "temp_step": 0.5,
        "has_sensor": True,
        "automation_helpers": True,
    },
}

RETAINED_REFERENCE_TRAITS = {
    "hvac_modes": ["off", "cool", "heat", "fan_only", "dry", "auto"],
    "fan_modes": ["Auto", "Level 1", "Level 2", "Level 3", "Level 4", "Level 5", "Quiet"],
    "swing_modes": ["Auto", "Highest", "High", "Middle", "Low", "Lowest"],
    "swing_horizontal_modes": ["Auto", "Left Max", "Left", "Middle", "Right", "Right Max"],
    "min_temp": 16,
    "max_temp": 30,
    "temp_step": 0.5,
    "temperature_unit": "C",
}

RETAINED_REFERENCE_STATE = {
    "mode": "heat",
    "target_temperature": 24,
    "fan_mode": "Level 2",
    "swing_mode": "Middle",
    "swing_horizontal_mode": "Left",
    "current_temperature": 26.5,
    "available": True,
}

RETAINED_STATE_REQUIRED_KEYS = (
    "mode",
    "target_temperature",
    "fan_mode",
    "swing_mode",
    "swing_horizontal_mode",
    "current_temperature",
    "available",
)

MQTT_SET_CASES = [
    {
        "id": "set_mode_cool",
        "payload": {"mode": "cool"},
        "expected_state": {"mode": "cool"},
    },
    {
        "id": "set_target_temperature_26",
        "payload": {"target_temperature": 26},
        "expected_state": {"target_temperature": 26},
    },
    {
        "id": "set_fan_mode_auto",
        "payload": {"fan_mode": "Auto"},
        "expected_state": {"fan_mode": "Auto"},
    },
    {
        "id": "set_swing_mode_auto",
        "payload": {"swing_mode": "Auto"},
        "expected_state": {"swing_mode": "Auto"},
    },
    {
        "id": "set_swing_horizontal_mode_auto",
        "payload": {"swing_horizontal_mode": "Auto"},
        "expected_state": {"swing_horizontal_mode": "Auto"},
    },
    {
        "id": "set_mode_cool_temp23_multi",
        "payload": {"mode": "cool", "target_temperature": 23},
        "expected_state": {"mode": "cool", "target_temperature": 23},
    },
]

MQTT_INVALID_CASES = [
    {
        "id": "unsupported_preset",
        "payload_text": '{"preset":"ECO"}',
        "expected": "No new state publish after unsupported preset payload",
    },
    {
        "id": "unsupported_target_humidity",
        "payload_text": '{"target_humidity":50}',
        "expected": "No new state publish after unsupported target_humidity payload",
    },
    {
        "id": "malformed_json",
        "payload_text": '{not json',
        "expected": "No new state publish after malformed JSON payload",
    },
]

BUTTON_TOPICS = {
    "control_cool_24c": "esphome-panaac-v2/button/control_cool_24c/command",
    "lambda_action_log": "esphome-panaac-v2/button/lambda_action_log/command",
    "lambda_make_call_cool_24c": "esphome-panaac-v2/button/lambda_make_call_cool_24c/command",
}
