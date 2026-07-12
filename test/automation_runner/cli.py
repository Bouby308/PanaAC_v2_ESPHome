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
from pathlib import Path
import sys

from .core import Runner, TestFailure, resolve_suite_selection
from .data import DEFAULT_MQTT_HOST, DEFAULT_MQTT_PORT, DEFAULT_TOPIC_PREFIX, SUITE_CHOICES, SUITE_LABELS


def build_parser() -> argparse.ArgumentParser:
    repo_root = Path(__file__).resolve().parents[2]
    workspace_root = repo_root.parents[1]
    parser = argparse.ArgumentParser(description="PanaAC v2 ESPHome automated test runner")
    subparsers = parser.add_subparsers(dest="command")

    def add_common_arguments(target: argparse.ArgumentParser, *, require_mqtt: bool) -> None:
        target.add_argument("--esphome-workspace-path", default=str(workspace_root / "esphome"))
        target.add_argument("--ha-repo-path", default=str(workspace_root / "ha" / "PanaAC_v2_HA"))
        target.add_argument("--topic-prefix", default=DEFAULT_TOPIC_PREFIX)
        target.add_argument("--mqtt-host", default=DEFAULT_MQTT_HOST)
        target.add_argument("--mqtt-port", type=int, default=DEFAULT_MQTT_PORT)
        target.add_argument("--mqtt-user", required=require_mqtt)
        target.add_argument("--mqtt-pass", required=require_mqtt)
        target.add_argument(
            "--output-dir",
            default=str(repo_root / "test" / "results" / datetime.now().strftime("%Y%m%d-%H%M%S")),
        )
        target.add_argument("--mode", choices=("auto", "ha-crosscheck", "full-hil"), default="auto")

    run_parser = subparsers.add_parser("run", help="Run selected automated suites")
    add_common_arguments(run_parser, require_mqtt=True)
    run_parser.add_argument(
        "--suite",
        dest="suite_values",
        action="append",
        choices=("all", *SUITE_CHOICES),
        help="Select one or more suites. Repeat to choose multiple.",
    )

    setup_parser = subparsers.add_parser("setup-env", help="Validate and prepare the local test environment")
    add_common_arguments(setup_parser, require_mqtt=True)
    setup_parser.add_argument("--no-verify-mqtt", action="store_true", help="Skip MQTT broker round-trip validation")
    setup_parser.add_argument("--no-verify-ha", action="store_true", help="Skip validating the HA repo path")
    setup_parser.add_argument("--no-verify-variants", action="store_true", help="Skip validating test variant YAML files")

    subparsers.add_parser("list", help="List available suites")

    menu_parser = subparsers.add_parser("menu", help="Interactive menu")
    add_common_arguments(menu_parser, require_mqtt=False)

    return parser


def normalize_argv(argv: list[str]) -> list[str]:
    if not argv:
        return ["run"]
    if argv[0] in {"run", "setup-env", "list", "menu"}:
        return argv
    return ["run", *argv]


def prompt_required(value: str | None, label: str) -> str:
    if value:
        return value
    entered = input(f"{label}: ").strip()
    if not entered:
        raise TestFailure(f"Missing required value for {label}")
    return entered


def run_menu(args: argparse.Namespace) -> int:
    print("PanaAC v2 ESPHome automated test runner")
    print("")
    print("1. Run all suites")
    for index, suite in enumerate(SUITE_CHOICES, start=2):
        print(f"{index}. Run {suite} - {SUITE_LABELS[suite]}")
    print(f"{len(SUITE_CHOICES) + 2}. Setup environment only")
    print("q. Quit")
    selection = input("Select option: ").strip().lower()
    if selection == "q":
        return 0

    args.mqtt_user = prompt_required(args.mqtt_user, "MQTT user")
    args.mqtt_pass = prompt_required(args.mqtt_pass, "MQTT password")

    setup_choice = len(SUITE_CHOICES) + 2
    if selection == "1":
        args.command = "run"
        args.suites = list(SUITE_CHOICES)
    elif selection == str(setup_choice):
        args.command = "setup-env"
        args.no_verify_mqtt = False
        args.no_verify_ha = False
        args.no_verify_variants = False
        args.suites = []
    else:
        try:
            suite = SUITE_CHOICES[int(selection) - 2]
        except (ValueError, IndexError) as err:
            raise TestFailure(f"Invalid menu selection: {selection}") from err
        args.command = "run"
        args.suites = [suite]
    return dispatch(args)


def dispatch(args: argparse.Namespace) -> int:
    if args.command == "list":
        for suite in SUITE_CHOICES:
            print(f"{suite}: {SUITE_LABELS[suite]}")
        return 0

    if args.command == "setup-env":
        args.suites = []
        runner = Runner(args)
        status = runner.setup_environment(
            verify_mqtt=not args.no_verify_mqtt,
            verify_ha=not args.no_verify_ha,
            verify_variants=not args.no_verify_variants,
        )
        for check in status.checks:
            print(f"- {check}")
        return 0

    if args.command == "menu":
        return run_menu(args)

    args.suites = resolve_suite_selection(getattr(args, "suite_values", None))
    runner = Runner(args)
    return runner.run()


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(normalize_argv(sys.argv[1:] if argv is None else argv))
    return dispatch(args)
