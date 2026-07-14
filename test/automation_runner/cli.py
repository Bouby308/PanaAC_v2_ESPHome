# Copyright 2026 Minh Hoang
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

"""CLI and menu interface for the PanaAC v2 ESPHome automation runner."""

from __future__ import annotations

import argparse
from datetime import datetime
import json
from pathlib import Path
import sys

from .core import Runner, TestFailure, resolve_suite_selection
from .data import DEFAULT_MQTT_HOST, DEFAULT_MQTT_PORT, DEFAULT_TOPIC_PREFIX, SUITE_CHOICES, SUITE_LABELS

RUNNER_CONFIG_BASENAME = "runner_config.json"


def default_config_path() -> Path:
    return Path(__file__).resolve().parents[1] / RUNNER_CONFIG_BASENAME


def build_parser() -> argparse.ArgumentParser:
    repo_root = Path(__file__).resolve().parents[2]
    workspace_root = repo_root.parents[1]
    parser = argparse.ArgumentParser(description="PanaAC v2 ESPHome automated test runner")
    subparsers = parser.add_subparsers(dest="command")

    def add_config_argument(target: argparse.ArgumentParser) -> None:
        target.add_argument("--config", default=str(default_config_path()), help="Path to runner JSON config")

    def add_common_arguments(target: argparse.ArgumentParser) -> None:
        add_config_argument(target)
        target.add_argument("--esphome-workspace-path", default=str(workspace_root / "esphome"))
        target.add_argument("--ha-repo-path", default=str(workspace_root / "ha" / "PanaAC_v2_HA"))
        target.add_argument("--topic-prefix", default=DEFAULT_TOPIC_PREFIX)
        target.add_argument("--mqtt-host", default=DEFAULT_MQTT_HOST)
        target.add_argument("--mqtt-port", type=int, default=DEFAULT_MQTT_PORT)
        target.add_argument(
            "--mqtt-broker-mode",
            choices=("external", "spawn"),
            default="external",
            help="Use an existing broker or spawn an isolated broker for this run",
        )
        target.add_argument("--mqtt-user")
        target.add_argument("--mqtt-pass")
        target.add_argument("--wifi-ssid")
        target.add_argument("--wifi-password")
        target.add_argument("--wifi-ap-password")
        target.add_argument(
            "--output-dir",
            default=str(repo_root / "test" / "results" / datetime.now().strftime("%Y%m%d-%H%M%S")),
        )
        target.add_argument("--esphome-device", help="ESPHome upload target such as /dev/ttyUSB0 or a device hostname/IP")
        target.add_argument(
            "--esphome-run-extra-arg",
            dest="esphome_run_extra_args",
            action="append",
            default=[],
            help="Extra argument to append to 'esphome run' for DUT reflashing; repeat as needed",
        )
        target.add_argument("--no-flush-mqtt", action="store_true", help="Skip clearing retained MQTT topics before DUT-backed suites")
        target.add_argument("--no-flash-dut", action="store_true", help="Skip reflashing the DUT before DUT-backed suites")
        target.add_argument("--mode", choices=("auto", "ha-crosscheck", "full-hil"), default="auto")

    run_parser = subparsers.add_parser("run", help="Run selected automated suites")
    add_common_arguments(run_parser)
    run_parser.add_argument(
        "--suite",
        dest="suite_values",
        action="append",
        choices=("all", *SUITE_CHOICES),
        help="Select one or more suites. Repeat to choose multiple.",
    )

    setup_parser = subparsers.add_parser("setup-env", help="Validate and prepare the local test environment")
    add_common_arguments(setup_parser)
    setup_parser.add_argument("--no-verify-mqtt", action="store_true", help="Skip MQTT broker round-trip validation")
    setup_parser.add_argument("--no-verify-ha", action="store_true", help="Skip validating the HA repo path")
    setup_parser.add_argument("--no-verify-variants", action="store_true", help="Skip validating test variant YAML files")

    dev_parser = subparsers.add_parser("dev-env", help="Validate the local developer environment only")
    add_common_arguments(dev_parser)

    list_parser = subparsers.add_parser("list", help="List available suites")
    add_config_argument(list_parser)

    menu_parser = subparsers.add_parser("menu", help="Interactive menu")
    add_common_arguments(menu_parser)

    return parser


def normalize_argv(argv: list[str]) -> list[str]:
    if not argv:
        return ["run"]
    if argv[0] in {"run", "setup-env", "dev-env", "list", "menu"}:
        return argv
    return ["run", *argv]


def load_runner_config(path_str: str) -> dict[str, object]:
    path = Path(path_str)
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text())
    except json.JSONDecodeError as err:
        raise TestFailure(f"Invalid JSON in runner config {path}: {err}") from err
    if not isinstance(data, dict):
        raise TestFailure(f"Runner config {path} must contain a JSON object")
    return data


def apply_runner_config(args: argparse.Namespace) -> None:
    config = load_runner_config(getattr(args, "config", str(default_config_path())))
    mqtt = config.get("mqtt")
    if isinstance(mqtt, dict):
        if (
            getattr(args, "mqtt_broker_mode", "external") == "external"
            and isinstance(mqtt.get("broker_mode"), str)
            and mqtt["broker_mode"] in {"external", "spawn"}
            and not getattr(args, "mqtt_broker_mode_explicit", False)
        ):
            args.mqtt_broker_mode = mqtt["broker_mode"]
        if getattr(args, "mqtt_host", DEFAULT_MQTT_HOST) == DEFAULT_MQTT_HOST and isinstance(mqtt.get("host"), str):
            args.mqtt_host = mqtt["host"]
        if getattr(args, "mqtt_port", DEFAULT_MQTT_PORT) == DEFAULT_MQTT_PORT and isinstance(mqtt.get("port"), int):
            args.mqtt_port = mqtt["port"]
        if not getattr(args, "mqtt_user", None) and isinstance(mqtt.get("user"), str):
            args.mqtt_user = mqtt["user"]
        if not getattr(args, "mqtt_pass", None) and isinstance(mqtt.get("pass"), str):
            args.mqtt_pass = mqtt["pass"]
    wifi = config.get("wifi")
    if isinstance(wifi, dict):
        if not getattr(args, "wifi_ssid", None) and isinstance(wifi.get("ssid"), str):
            args.wifi_ssid = wifi["ssid"]
        if not getattr(args, "wifi_password", None) and isinstance(wifi.get("password"), str):
            args.wifi_password = wifi["password"]
        if not getattr(args, "wifi_ap_password", None) and isinstance(wifi.get("ap_password"), str):
            args.wifi_ap_password = wifi["ap_password"]


def save_runner_config(args: argparse.Namespace) -> None:
    path = Path(getattr(args, "config", str(default_config_path())))
    data = load_runner_config(str(path)) if path.exists() else {}
    mqtt = data.get("mqtt")
    if not isinstance(mqtt, dict):
        mqtt = {}
        data["mqtt"] = mqtt
    mqtt["broker_mode"] = args.mqtt_broker_mode
    mqtt["host"] = args.mqtt_host
    mqtt["port"] = args.mqtt_port
    mqtt["user"] = args.mqtt_user
    mqtt["pass"] = args.mqtt_pass
    wifi = data.get("wifi")
    if not isinstance(wifi, dict):
        wifi = {}
        data["wifi"] = wifi
    wifi["ssid"] = getattr(args, "wifi_ssid", None)
    wifi["password"] = getattr(args, "wifi_password", None)
    wifi["ap_password"] = getattr(args, "wifi_ap_password", None)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n")
    try:
        path.chmod(0o600)
    except OSError:
        pass
    print(f"Stored runner settings in {path}")


def prompt_required(value: str | None, label: str) -> str:
    if value:
        return value
    entered = input(f"{label}: ").strip()
    if not entered:
        raise TestFailure(f"Missing required value for {label}")
    return entered


def ensure_mqtt_credentials(args: argparse.Namespace) -> None:
    if args.mqtt_broker_mode == "spawn":
        return
    if args.mqtt_user and args.mqtt_pass:
        return
    raise TestFailure(
        "Missing MQTT credentials. Pass --mqtt-user/--mqtt-pass or create test/runner_config.json"
    )


def requires_runtime_wifi_credentials(args: argparse.Namespace) -> bool:
    return args.command == "run" and bool({"esphome.g2", "esphome.g3"} & set(getattr(args, "suites", []) or []))


def ensure_runtime_wifi_credentials(args: argparse.Namespace) -> None:
    if args.wifi_ssid and args.wifi_password and args.wifi_ap_password:
        return
    raise TestFailure(
        "Missing Wi-Fi secrets for DUT-backed suites. Pass --wifi-ssid/--wifi-password/--wifi-ap-password or add them under wifi in test/runner_config.json"
    )


def select_default_mqtt_broker_mode(command: str, suites: list[str], explicit: bool, current_mode: str) -> str:
    if explicit:
        return current_mode
    if command in {"setup-env", "dev-env"}:
        return "spawn"
    if command == "run" and not ({"esphome.g2", "esphome.g3"} & set(suites)):
        return "spawn"
    return current_mode


def run_menu(args: argparse.Namespace) -> int:
    print("PanaAC v2 ESPHome automated test runner")
    print("")
    print("1. Run all suites")
    for index, suite in enumerate(SUITE_CHOICES, start=2):
        print(f"{index}. Run {suite} - {SUITE_LABELS[suite]}")
    dev_choice = len(SUITE_CHOICES) + 2
    setup_choice = dev_choice + 1
    print(f"{dev_choice}. Validate dev environment only")
    print(f"{setup_choice}. Setup environment only")
    print("q. Quit")
    selection = input("Select option: ").strip().lower()
    if selection == "q":
        return 0

    if selection == str(dev_choice):
        args.command = "dev-env"
        args.suites = []
        return dispatch(args)

    suite_selection = selection.isdigit() and 2 <= int(selection) <= len(SUITE_CHOICES) + 1
    if selection == "1":
        target_command = "run"
        target_suites = list(SUITE_CHOICES)
    elif selection == str(setup_choice):
        target_command = "setup-env"
        target_suites = []
    else:
        try:
            suite = SUITE_CHOICES[int(selection) - 2]
        except (ValueError, IndexError) as err:
            raise TestFailure(f"Invalid menu selection: {selection}") from err
        target_command = "run"
        target_suites = [suite]

    args.mqtt_broker_mode = select_default_mqtt_broker_mode(
        target_command,
        target_suites,
        getattr(args, "mqtt_broker_mode_explicit", False),
        args.mqtt_broker_mode,
    )
    needs_mqtt = (selection == "1" or selection == str(setup_choice) or suite_selection) and args.mqtt_broker_mode != "spawn"
    if needs_mqtt:
        had_missing_credentials = not args.mqtt_user or not args.mqtt_pass
        args.mqtt_user = prompt_required(args.mqtt_user, "MQTT user")
        args.mqtt_pass = prompt_required(args.mqtt_pass, "MQTT password")
        if had_missing_credentials:
            save_runner_config(args)
    if target_command == "run" and ({"esphome.g2", "esphome.g3"} & set(target_suites)):
        had_missing_wifi = not args.wifi_ssid or not args.wifi_password or not args.wifi_ap_password
        args.wifi_ssid = prompt_required(args.wifi_ssid, "Wi-Fi SSID")
        args.wifi_password = prompt_required(args.wifi_password, "Wi-Fi password")
        args.wifi_ap_password = prompt_required(args.wifi_ap_password, "Wi-Fi AP password")
        if had_missing_wifi:
            save_runner_config(args)

    args.command = target_command
    args.suites = target_suites
    if target_command == "setup-env":
        args.no_verify_mqtt = False
        args.no_verify_ha = False
        args.no_verify_variants = False
    return dispatch(args)


def dispatch(args: argparse.Namespace) -> int:
    if args.command == "list":
        for suite in SUITE_CHOICES:
            print(f"{suite}: {SUITE_LABELS[suite]}")
        return 0

    if args.command == "dev-env":
        args.mqtt_broker_mode = select_default_mqtt_broker_mode(
            "dev-env", [], getattr(args, "mqtt_broker_mode_explicit", False), args.mqtt_broker_mode
        )
        args.suites = []
        runner = Runner(args)
        try:
            status = runner.validate_dev_environment()
        finally:
            runner._cleanup()
        for check in status.checks:
            print(f"- {check}")
        return 0

    if args.command == "setup-env":
        args.mqtt_broker_mode = select_default_mqtt_broker_mode(
            "setup-env", [], getattr(args, "mqtt_broker_mode_explicit", False), args.mqtt_broker_mode
        )
        ensure_mqtt_credentials(args)
        args.suites = []
        runner = Runner(args)
        try:
            status = runner.setup_environment(
                verify_mqtt=not args.no_verify_mqtt,
                verify_ha=not args.no_verify_ha,
                verify_variants=not args.no_verify_variants,
            )
        finally:
            runner._cleanup()
        for check in status.checks:
            print(f"- {check}")
        return 0

    if args.command == "menu":
        return run_menu(args)

    suite_values = getattr(args, "suite_values", None)
    if suite_values is not None:
        args.suites = resolve_suite_selection(suite_values)
    args.mqtt_broker_mode = select_default_mqtt_broker_mode(
        "run", list(args.suites or SUITE_CHOICES), getattr(args, "mqtt_broker_mode_explicit", False), args.mqtt_broker_mode
    )
    ensure_mqtt_credentials(args)
    if requires_runtime_wifi_credentials(args):
        if sys.stdin.isatty() and (not args.wifi_ssid or not args.wifi_password or not args.wifi_ap_password):
            args.wifi_ssid = prompt_required(args.wifi_ssid, "Wi-Fi SSID")
            args.wifi_password = prompt_required(args.wifi_password, "Wi-Fi password")
            args.wifi_ap_password = prompt_required(args.wifi_ap_password, "Wi-Fi AP password")
            save_runner_config(args)
        ensure_runtime_wifi_credentials(args)
    runner = Runner(args)
    return runner.run()


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    try:
        raw_argv = normalize_argv(sys.argv[1:] if argv is None else argv)
        args = parser.parse_args(raw_argv)
        args.mqtt_broker_mode_explicit = "--mqtt-broker-mode" in raw_argv
        apply_runner_config(args)
        return dispatch(args)
    except TestFailure as err:
        print(f"Error: {err}", file=sys.stderr)
        return 1
