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

