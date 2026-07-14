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

"""Framework and execution engine for the PanaAC v2 ESPHome automated tests."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass, field
from datetime import datetime
import json
import os
import selectors
import socket
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from typing import Any, Callable

from .data import (
    BUTTON_TOPICS,
    DEFAULT_MQTT_PORT,
    DEFAULT_TOPIC_PREFIX,
    MQTT_INVALID_CASES,
    MQTT_SET_CASES,
    RETAINED_REFERENCE_STATE,
    RETAINED_STATE_REQUIRED_KEYS,
    RETAINED_REFERENCE_TRAITS,
    SUITE_CHOICES,
    SUITE_LABELS,
    VARIANT_EXPECTATIONS,
    VARIANT_ORDER,
)


class TestFailure(RuntimeError):
    """Raised when a required automated check fails."""


@dataclass
class CaseResult:
    id: str
    title: str
    status: str
    expected: Any = None
    actual: Any = None
    evidence: dict[str, Any] = field(default_factory=dict)
    duration_s: float = 0.0


@dataclass
class GroupResult:
    id: str
    title: str
    cases: list[CaseResult] = field(default_factory=list)

    def add(self, case: CaseResult) -> None:
        self.cases.append(case)


@dataclass
class EnvironmentStatus:
    checks: list[str] = field(default_factory=list)

    def add(self, message: str) -> None:
        self.checks.append(message)


class Runner:
    """Main orchestrator for the ESPHome automated test plan."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.repo_root = Path(__file__).resolve().parents[2]
        self.workspace_root = self.repo_root.parents[1]
        self.esphome_workspace_path = Path(args.esphome_workspace_path).resolve()
        self.ha_repo_path = Path(args.ha_repo_path).resolve()
        self.output_dir = Path(args.output_dir).resolve()
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.raw_capture_dir = self.output_dir / "captures"
        self.raw_capture_dir.mkdir(parents=True, exist_ok=True)
        self.mqtt_log_path = self.output_dir / "mqtt-broker.log"
        self.mqtt_config_path = self.output_dir / "mosquitto.conf"
        self.topic_prefix = args.topic_prefix
        self.esphome_device = getattr(args, "esphome_device", None)
        self.esphome_run_extra_args = list(getattr(args, "esphome_run_extra_args", []))
        self.flush_mqtt_before_runtime = not getattr(args, "no_flush_mqtt", False)
        self.flash_dut_before_runtime = not getattr(args, "no_flash_dut", False)
        self.mqtt_broker_mode = getattr(args, "mqtt_broker_mode", "external")
        self.mqtt_host = args.mqtt_host
        self.mqtt_port = args.mqtt_port
        self.mqtt_user = args.mqtt_user
        self.mqtt_pass = args.mqtt_pass
        self.wifi_ssid = getattr(args, "wifi_ssid", None)
        self.wifi_password = getattr(args, "wifi_password", None)
        self.wifi_ap_password = getattr(args, "wifi_ap_password", None)
        if self.mqtt_broker_mode == "spawn":
            self.mqtt_host = "127.0.0.1"
            if self.mqtt_port == DEFAULT_MQTT_PORT:
                self.mqtt_port = self._allocate_free_tcp_port()
            self.mqtt_user = None
            self.mqtt_pass = None
        self.runtime_broker_host = self._detect_runtime_broker_host()
        self.mode = args.mode
        self.selected_suites = set(args.suites or SUITE_CHOICES)
        self.timestamp = datetime.now().astimezone()
        self.report_json_path = self.output_dir / "report.json"
        self.report_md_path = self.output_dir / "report.md"
        self.groups: list[GroupResult] = []
        self._mqtt_broker_proc: subprocess.Popen[bytes] | None = None

    def run(self) -> int:
        try:
            self._log("Starting PanaAC v2 ESPHome automated tests")
            if self.mqtt_broker_mode == "spawn":
                self._ensure_mqtt_broker_ready()
            self._validate_environment()
            if "esphome.g1" in self.selected_suites:
                self.groups.append(self._run_esphome_group_1())
            if self.selected_suites & {"esphome.g2", "esphome.g3"}:
                self._prepare_runtime_suites()
            if "esphome.g2" in self.selected_suites:
                self.groups.append(self._run_esphome_group_2())
            if "esphome.g3" in self.selected_suites:
                self.groups.append(self._run_esphome_group_3())
            self._write_reports()
            self._log(f"Completed automated tests with overall status: {'pass' if self._overall_status() else 'fail'}")
            return 0 if self._overall_status() else 1
        except Exception as err:  # noqa: BLE001
            self._log(f"Runner failed: {err}")
            failure_group = GroupResult("runner", "Runner")
            failure_group.add(
                CaseResult(
                    id="runner.failure",
                    title="Runner failure",
                    status="fail",
                    expected="Runner completes all requested suites",
                    actual=str(err),
                    evidence={"exception_type": type(err).__name__},
                )
            )
            self.groups.append(failure_group)
            self._write_reports()
            return 1
        finally:
            self._cleanup()

    def setup_environment(
        self,
        *,
        verify_mqtt: bool,
        verify_ha: bool,
        verify_variants: bool,
    ) -> EnvironmentStatus:
        status = EnvironmentStatus()
        if self.mqtt_broker_mode == "spawn":
            self._ensure_mqtt_broker_ready()
            status.add(f"Started isolated MQTT broker on {self.mqtt_host}:{self.mqtt_port}")
        self._validate_environment()
        status.add("Validated required binaries and local paths")
        if verify_variants:
            self._validate_variant_files()
            status.add("Validated variant YAML inventory")
        if verify_mqtt:
            self._verify_mqtt_round_trip()
            status.add("Verified MQTT broker publish/subscribe round-trip")
        if verify_ha:
            if not self.ha_repo_path.exists():
                raise TestFailure(f"Home Assistant repo path not found: {self.ha_repo_path}")
            status.add("Validated Home Assistant repo path")
        return status

    def validate_dev_environment(self) -> EnvironmentStatus:
        if self.mqtt_broker_mode == "spawn":
            self._ensure_mqtt_broker_ready()
        status = EnvironmentStatus()
        self._validate_environment()
        status.add("Validated required binaries and local paths")
        self._validate_variant_files()
        status.add("Validated variant YAML inventory")
        if self.ha_repo_path.exists():
            status.add("Validated Home Assistant repo path")
        else:
            status.add(f"Home Assistant repo path not found: {self.ha_repo_path}")
        return status

    def _allocate_free_tcp_port(self) -> int:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(("127.0.0.1", 0))
            sock.listen(1)
            return int(sock.getsockname()[1])

    def _detect_runtime_broker_host(self) -> str:
        if self.mqtt_broker_mode == "spawn":
            return self.mqtt_host
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.connect(("8.8.8.8", 80))
                host = sock.getsockname()[0]
                if host and not host.startswith("127."):
                    return host
        except OSError:
            pass
        try:
            for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET, socket.SOCK_DGRAM):
                host = info[4][0]
                if host and not host.startswith("127."):
                    return host
        except OSError:
            pass
        if self.mqtt_host and not str(self.mqtt_host).startswith("127."):
            return self.mqtt_host
        raise TestFailure(
            "Could not determine a non-loopback LAN IP for the ESPHome DUT broker. Set a reachable local IP in your environment or pass a non-loopback MQTT host."
        )

    def _ensure_mqtt_broker_ready(self) -> None:
        if self.mqtt_broker_mode != "spawn" or self._mqtt_broker_proc is not None:
            return
        mosquitto_bin = shutil.which("mosquitto")
        if mosquitto_bin is None:
            raise TestFailure("Required command not found: mosquitto")
        self._log(f"Starting isolated MQTT broker on {self.mqtt_host}:{self.mqtt_port}")
        self.mqtt_config_path.write_text(
            "\n".join(
                (
                    f"listener {self.mqtt_port} {self.mqtt_host}",
                    "allow_anonymous true",
                    "persistence false",
                )
            )
            + "\n"
        )
        log_file = self.mqtt_log_path.open("a")
        self._mqtt_broker_proc = subprocess.Popen(
            [mosquitto_bin, "-c", str(self.mqtt_config_path)],
            stdout=log_file,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if self._mqtt_broker_proc.poll() is not None:
                break
            try:
                with socket.create_connection((self.mqtt_host, self.mqtt_port), timeout=1.0):
                    return
            except OSError:
                time.sleep(0.2)
        broker_log = self.mqtt_log_path.read_text() if self.mqtt_log_path.exists() else ""
        raise TestFailure(
            f"Spawned MQTT broker failed to become ready on {self.mqtt_host}:{self.mqtt_port}: {broker_log.strip()}"
        )

    def _stop_mqtt_broker(self) -> None:
        if self._mqtt_broker_proc is None:
            return
        self._log("Stopping isolated MQTT broker")
        proc = self._mqtt_broker_proc
        self._mqtt_broker_proc = None
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)

    def _prepare_runtime_suites(self) -> None:
        if self.mqtt_broker_mode != "external":
            raise TestFailure(
                "DUT-backed suites require --mqtt-broker-mode external so the flashed device can reconnect to the broker under test"
            )
        variant = "C3-automation" if "esphome.g3" in self.selected_suites else "C3"
        if self.flush_mqtt_before_runtime:
            self._log(f"Clearing retained MQTT topics under {self.topic_prefix} before runtime suites")
            self._clear_runtime_topics()
        if self.flash_dut_before_runtime:
            self._flash_runtime_variant(variant)
        self._wait_for_runtime_availability()

    def _flash_runtime_variant(self, variant: str) -> None:
        esphome_bin = self.esphome_workspace_path / ".venv" / "bin" / "esphome"
        yaml_path = self.repo_root / "test" / "variants" / f"{variant}.yaml"
        if not yaml_path.exists():
            raise TestFailure(f"Required runtime variant YAML missing: {yaml_path}")
        runtime_yaml_path = self._prepare_runtime_variant_bundle(yaml_path)
        yaml_arg = str(runtime_yaml_path.relative_to(self.esphome_workspace_path))
        cmd = [str(esphome_bin), "run", "--no-logs", yaml_arg]
        if self.esphome_device:
            cmd.extend(["--device", self.esphome_device])
        cmd.extend(self.esphome_run_extra_args)
        self._log(f"Flashing DUT with runtime variant {variant} using broker host {self.runtime_broker_host}")
        result = self._run_command(cmd, cwd=self.esphome_workspace_path, check=False)
        if result.returncode != 0:
            raise TestFailure(f"Failed to flash DUT with {variant}: {result.stderr.strip() or result.stdout.strip()}")

    def _prepare_runtime_variant_bundle(self, source_yaml_path: Path) -> Path:
        source_secrets_path = self.repo_root / "test" / "variants" / "secrets.yaml"
        if not source_secrets_path.exists():
            raise TestFailure(f"Missing ESPHome test secrets file: {source_secrets_path}")
        runtime_dir = self.output_dir / "runtime_variants"
        runtime_dir.mkdir(parents=True, exist_ok=True)
        runtime_yaml_path = runtime_dir / source_yaml_path.name
        secrets = self._load_runtime_secrets(source_secrets_path)
        secrets["mqtt_broker"] = self.runtime_broker_host
        materialized = source_yaml_path.read_text()
        component_source = (self.repo_root / "esphome" / "components").resolve()
        materialized = materialized.replace('../../esphome/components', str(component_source))
        for key, value in secrets.items():
            materialized = materialized.replace(f'!secret {key}', f'"{value}"')
        runtime_yaml_path.write_text(materialized)
        return runtime_yaml_path

    def _load_runtime_secrets(self, source_secrets_path: Path) -> dict[str, str]:
        secrets = self._parse_simple_secrets(source_secrets_path)
        wifi_values = {
            "wifi_ssid": self.wifi_ssid,
            "wifi_password": self.wifi_password,
            "wifi_ap_password": self.wifi_ap_password,
        }
        placeholder_map = {
            "wifi_ssid": "YOUR_WIFI_SSID",
            "wifi_password": "YOUR_WIFI_PASSWORD",
            "wifi_ap_password": "YOUR_WIFI_AP_PASSWORD",
        }
        for key, value in wifi_values.items():
            if value:
                secrets[key] = value
        missing = [key for key in wifi_values if not secrets.get(key) or secrets.get(key) == placeholder_map[key]]
        if missing:
            raise TestFailure(
                "Missing Wi-Fi secrets for runtime flash: " + ", ".join(missing)
            )
        return secrets

    def _parse_simple_secrets(self, secrets_path: Path) -> dict[str, str]:
        secrets: dict[str, str] = {}
        for line in secrets_path.read_text().splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#") or ":" not in stripped:
                continue
            key, value = stripped.split(":", 1)
            secrets[key.strip()] = value.strip().strip('"').strip("'")
        return secrets

    def _clear_runtime_topics(self) -> None:
        topics = [
            f"{self.topic_prefix}/availability",
            f"{self.topic_prefix}/traits",
            f"{self.topic_prefix}/state",
            f"{self.topic_prefix}/set",
        ]
        for topic in topics:
            self._clear_retained_topic(topic)

    def _clear_retained_topic(self, topic: str) -> None:
        cmd = ["mosquitto_pub", *self._mosquitto_common(), "-t", topic, "-r", "-n"]
        result = self._run_command(cmd, check=False)
        if result.returncode != 0:
            raise TestFailure(f"Failed to clear retained MQTT topic {topic}: {result.stderr.strip()}")

    def _run_esphome_group_1(self) -> GroupResult:
        group = GroupResult("esphome.g1", SUITE_LABELS["esphome.g1"])
        esphome_bin = self.esphome_workspace_path / ".venv" / "bin" / "esphome"
        env = os.environ.copy()
        env["PLATFORMIO_CORE_DIR"] = env.get("PLATFORMIO_CORE_DIR", "/tmp/platformio")
        env["XDG_CACHE_HOME"] = env.get("XDG_CACHE_HOME", "/tmp/.cache")

        for variant in VARIANT_ORDER:
            started = time.monotonic()
            self._log(f"[esphome.g1] {variant}: config and compile")
            yaml_path = self.repo_root / "test" / "variants" / f"{variant}.yaml"
            expectation = VARIANT_EXPECTATIONS[variant]
            if not yaml_path.exists():
                group.add(
                    CaseResult(
                        id=f"esphome.g1.{variant.lower()}",
                        title=f"{variant} variant automation",
                        status="fail",
                        expected=str(yaml_path),
                        actual="Variant YAML missing",
                    )
                )
                continue

            yaml_arg = str(yaml_path.relative_to(self.esphome_workspace_path))
            config_cmd = [str(esphome_bin), "config", yaml_arg]
            compile_cmd = [str(esphome_bin), "compile", yaml_arg]
            config_result = self._run_command(config_cmd, cwd=self.esphome_workspace_path, env=env, check=False)
            compile_result = self._run_command(compile_cmd, cwd=self.esphome_workspace_path, env=env, check=False)
            config_checks = self._validate_variant_config_output(expectation, config_result.stdout)
            memory = self._extract_compile_metrics(compile_result.stdout + "\n" + compile_result.stderr)
            status = (
                "pass"
                if config_result.returncode == 0 and compile_result.returncode == 0 and not config_checks
                else "fail"
            )

            capture_path = self.raw_capture_dir / f"esphome-{variant}.log"
            capture_path.write_text(
                "\n".join(
                    [
                        f"$ {' '.join(config_cmd)}",
                        config_result.stdout,
                        config_result.stderr,
                        "",
                        f"$ {' '.join(compile_cmd)}",
                        compile_result.stdout,
                        compile_result.stderr,
                    ]
                )
            )

            expected = {
                "config_rc": 0,
                "compile_rc": 0,
                "config_mode": expectation["config_mode"],
                "topic_prefix": expectation["topic_prefix"],
                "supports_cool": expectation["supports_cool"],
                "supports_heat": expectation["supports_heat"],
                "supports_fan_only": expectation["supports_fan_only"],
                "supports_quiet": expectation["supports_quiet"],
                "fan_5level": expectation["fan_5level"],
                "swing_horizontal": expectation["swing_horizontal"],
                "temp_step": expectation["temp_step"],
                "has_sensor": expectation["has_sensor"],
            }
            actual = {
                "config_rc": config_result.returncode,
                "compile_rc": compile_result.returncode,
                "flash_percent": memory.get("flash_percent"),
                "flash_used_bytes": memory.get("flash_used_bytes"),
                "flash_total_bytes": memory.get("flash_total_bytes"),
                "ram_percent": memory.get("ram_percent"),
                "ram_used_bytes": memory.get("ram_used_bytes"),
                "ram_total_bytes": memory.get("ram_total_bytes"),
            }
            evidence: dict[str, Any] = {"log_path": str(capture_path)}
            if config_checks:
                evidence["mismatches"] = config_checks
            if memory:
                evidence["memory"] = memory

            group.add(
                CaseResult(
                    id=f"esphome.g1.{variant.lower()}",
                    title=f"{variant} config/compile + config contract",
                    status=status,
                    expected=expected,
                    actual=actual,
                    evidence=evidence,
                    duration_s=time.monotonic() - started,
                )
            )
        return group

    def _run_esphome_group_2(self) -> GroupResult:
        group = GroupResult("esphome.g2", SUITE_LABELS["esphome.g2"])
        self._log("[esphome.g2] MQTT retained state and set handling checks")
        for suffix, topic_suffix, expected_payload in (
            ("2.1.availability", "availability", "online"),
            ("2.1.traits", "traits", RETAINED_REFERENCE_TRAITS),
            ("2.1.state", "state", None),
        ):
            started = time.monotonic()
            self._log(f"[esphome.g2] retained {topic_suffix}")
            actual_payload = self._read_retained_topic(topic_suffix)
            if topic_suffix == "state":
                status = "pass" if self._state_has_required_keys(actual_payload) else "fail"
                expected: Any = {"required_keys": list(RETAINED_STATE_REQUIRED_KEYS), "available": True}
            else:
                status = "pass" if self._payload_matches(expected_payload, actual_payload) else "fail"
                expected = expected_payload
            group.add(
                CaseResult(
                    id=f"esphome.g{suffix}",
                    title=f"Retained {topic_suffix} payload",
                    status=status,
                    expected=expected,
                    actual=actual_payload,
                    duration_s=time.monotonic() - started,
                )
            )

        for case in MQTT_SET_CASES:
            started = time.monotonic()
            self._log(f"[esphome.g2] set case: {case['id']}")
            try:
                self._ensure_reference_state()
                actual_state = self._publish_set_and_capture_state(case["payload"])
                mismatches = self._compare_expected(case["expected_state"], actual_state)
                actual: Any = actual_state
                evidence = {"mismatches": mismatches} if mismatches else {}
                status = "pass" if not mismatches else "fail"
            except TestFailure as err:
                actual = str(err)
                evidence = {"exception_type": type(err).__name__}
                status = "fail"
            group.add(
                CaseResult(
                    id=f"esphome.g2.2.{case['id']}",
                    title=case["id"],
                    status=status,
                    expected=case["expected_state"],
                    actual=actual,
                    evidence=evidence,
                    duration_s=time.monotonic() - started,
                )
            )

        for case in MQTT_INVALID_CASES:
            started = time.monotonic()
            self._log(f"[esphome.g2] invalid set case: {case['id']}")
            self._ensure_reference_state()
            unexpected_state = self._publish_invalid_set_and_capture_state(case["payload_text"])
            invalid_status = "pass" if unexpected_state is None or not self._compare_expected(RETAINED_REFERENCE_STATE, unexpected_state) else "fail"
            group.add(
                CaseResult(
                    id=f"esphome.g2.3.{case['id']}",
                    title=case["id"],
                    status=invalid_status,
                    expected=case["expected"],
                    actual=unexpected_state if unexpected_state is not None else "No state publish observed",
                    duration_s=time.monotonic() - started,
                )
            )

        group.add(
            CaseResult(
                id="esphome.g2.2.atomic_ir_transmit",
                title="Atomic multi-field command transmit",
                status="pass"
                if (
                    lambda result: self._count_log_fragment(result["logs"], "Sending remote code") == 1
                    and not self._compare_expected({"mode": "cool", "target_temperature": 23}, result["state"])
                )(atomic_result := self._publish_set_and_capture_state_and_debug({"mode": "cool", "target_temperature": 23}))
                else "fail",
                expected={
                    "state": {"mode": "cool", "target_temperature": 23},
                    "debug_log_count": {"Sending remote code": 1},
                },
                actual=atomic_result,
                evidence={
                    "sending_remote_code_count": self._count_log_fragment(atomic_result["logs"], "Sending remote code"),
                },
            )
        )
        return group

    def _run_esphome_group_3(self) -> GroupResult:
        group = GroupResult("esphome.g3", SUITE_LABELS["esphome.g3"])
        self._log("[esphome.g3] Button and lambda integration checks")
        started = time.monotonic()
        control_debug, control_state = self._press_button_and_capture(
            BUTTON_TOPICS["control_cool_24c"],
            capture_state=True,
        )
        control_expected_state = {
            "mode": "cool",
            "target_temperature": 24,
            "fan_mode": "Auto",
            "swing_mode": "Auto",
        }
        control_state_mismatches = self._compare_expected(control_expected_state, control_state or {})
        control_expected_logs = ("on_control fired", "on_state fired", "Sending remote code")
        control_log_mismatches = self._missing_log_fragments(control_debug, control_expected_logs)
        group.add(
            CaseResult(
                id="esphome.g3.1",
                title="ESPHome climate.control action",
                status="pass" if not control_state_mismatches and not control_log_mismatches else "fail",
                expected={"state": control_expected_state, "logs": list(control_expected_logs)},
                actual={"state": control_state, "logs": control_debug},
                evidence={
                    key: value
                    for key, value in {
                        "state_mismatches": control_state_mismatches,
                        "log_mismatches": control_log_mismatches,
                    }.items()
                    if value
                },
                duration_s=time.monotonic() - started,
            )
        )

        started = time.monotonic()
        self._log("[esphome.g3] lambda state accessors")
        lambda_log_debug, _ = self._press_button_and_capture(
            BUTTON_TOPICS["lambda_action_log"],
            capture_state=False,
        )
        lambda_expected_logs = ("mode=", "action=", "cur=", "tgt=")
        lambda_log_mismatches = self._missing_log_fragments(lambda_log_debug, lambda_expected_logs)
        group.add(
            CaseResult(
                id="esphome.g3.4",
                title="ESPHome lambda state accessors",
                status="pass" if not lambda_log_mismatches else "fail",
                expected={"logs": list(lambda_expected_logs)},
                actual={"logs": lambda_log_debug},
                evidence={"log_mismatches": lambda_log_mismatches} if lambda_log_mismatches else {},
                duration_s=time.monotonic() - started,
            )
        )
        derived_action_ok = any("action=2" in line for line in lambda_log_debug)
        group.add(
            CaseResult(
                id="esphome.g3.5",
                title="ESPHome derived action",
                status="pass" if derived_action_ok else "fail",
                expected="Lambda action log reports action=2 for cooling mode",
                actual=lambda_log_debug,
            )
        )

        started = time.monotonic()
        self._log("[esphome.g3] lambda make_call")
        make_call_debug, make_call_state = self._press_button_and_capture(
            BUTTON_TOPICS["lambda_make_call_cool_24c"],
            capture_state=True,
        )
        make_call_expected_state = {
            "mode": "cool",
            "target_temperature": 24,
            "fan_mode": "Level 2",
        }
        make_call_state_mismatches = self._compare_expected(make_call_expected_state, make_call_state or {})
        make_call_expected_logs = ("on_control fired", "on_state fired", "Sending remote code")
        make_call_log_mismatches = self._missing_log_fragments(make_call_debug, make_call_expected_logs)
        group.add(
            CaseResult(
                id="esphome.g3.6",
                title="ESPHome make_call lambda",
                status="pass" if not make_call_state_mismatches and not make_call_log_mismatches else "fail",
                expected={"state": make_call_expected_state, "logs": list(make_call_expected_logs)},
                actual={"state": make_call_state, "logs": make_call_debug},
                evidence={
                    key: value
                    for key, value in {
                        "state_mismatches": make_call_state_mismatches,
                        "log_mismatches": make_call_log_mismatches,
                    }.items()
                    if value
                },
                duration_s=time.monotonic() - started,
            )
        )

        group.add(
            CaseResult(
                id="esphome.g3.2",
                title="ESPHome on_state trigger",
                status="pass" if any("on_state fired" in line for line in control_debug + lambda_log_debug + make_call_debug) else "fail",
                expected="DUT logs show on_state fired during control and lambda helper actions",
                actual=control_debug + lambda_log_debug + make_call_debug,
            )
        )
        group.add(
            CaseResult(
                id="esphome.g3.3",
                title="ESPHome on_control trigger",
                status="pass" if any("on_control fired" in line for line in control_debug + make_call_debug) else "fail",
                expected="DUT logs show on_control fired during control and make_call helper actions",
                actual=control_debug + make_call_debug,
            )
        )
        return group

    def _validate_environment(self) -> None:
        required_commands = ["mosquitto_pub", "mosquitto_sub", "python3"]
        if self.mqtt_broker_mode == "spawn":
            required_commands.append("mosquitto")
        for cmd in required_commands:
            if shutil.which(cmd) is None:
                raise TestFailure(f"Required command not found: {cmd}")
        esphome_bin = self.esphome_workspace_path / ".venv" / "bin" / "esphome"
        if not esphome_bin.exists():
            raise TestFailure(f"Missing ESPHome CLI under {self.esphome_workspace_path}")
        self._validate_variant_files()

    def _validate_variant_files(self) -> None:
        missing = [
            variant
            for variant in VARIANT_ORDER
            if not (self.repo_root / "test" / "variants" / f"{variant}.yaml").exists()
        ]
        if missing:
            raise TestFailure(f"Missing variant YAMLs: {', '.join(missing)}")

    def _validate_variant_config_output(self, expectation: dict[str, Any], output: str) -> dict[str, dict[str, Any]]:
        component_block = self._extract_component_block(output, "panaac_v2:")
        checks = {
            "supports_cool": self._find_bool(component_block, "supports_cool"),
            "supports_heat": self._find_bool(component_block, "supports_heat"),
            "supports_fan_only": self._find_bool(component_block, "supports_fan_only"),
            "supports_quiet": self._find_bool(component_block, "supports_quiet"),
            "fan_5level": self._find_bool(component_block, "fan_5level"),
            "swing_horizontal": self._find_bool(component_block, "swing_horizontal"),
            "temp_step": self._find_float(component_block, "temp_step"),
            "has_sensor": "sensor: room_temp" in component_block,
            "has_automation_helpers": "on_state:" in component_block and "on_control:" in component_block and "Lambda make_call cool 24C" in output,
        }
        if expectation["config_mode"] == "v2":
            checks["topic_prefix"] = self._find_string(component_block, "topic_prefix")
        else:
            checks["topic_prefix"] = None

        expected = {
            "supports_cool": expectation["supports_cool"],
            "supports_heat": expectation["supports_heat"],
            "supports_fan_only": expectation["supports_fan_only"],
            "supports_quiet": expectation["supports_quiet"],
            "fan_5level": expectation["fan_5level"],
            "swing_horizontal": expectation["swing_horizontal"],
            "temp_step": expectation["temp_step"],
            "has_sensor": expectation["has_sensor"],
            "topic_prefix": expectation["topic_prefix"],
            "has_automation_helpers": expectation.get("automation_helpers", False),
        }
        return {
            key: {"expected": value, "actual": checks.get(key)}
            for key, value in expected.items()
            if checks.get(key) != value
        }

    def _extract_component_block(self, output: str, header: str) -> str:
        match = re.search(rf"^{re.escape(header)}\n((?:^[ ]+.*\n?)*)", output, re.MULTILINE)
        if not match:
            return ""
        return match.group(1)

    def _find_bool(self, output: str, key: str) -> bool | None:
        match = re.search(rf"^\s*{re.escape(key)}:\s+(true|false)\s*$", output, re.MULTILINE)
        if not match:
            return None
        return match.group(1) == "true"

    def _find_float(self, output: str, key: str) -> float | None:
        match = re.search(rf"^\s*{re.escape(key)}:\s+([0-9]+(?:\.[0-9]+)?)\s*$", output, re.MULTILINE)
        if not match:
            return None
        return float(match.group(1))

    def _find_string(self, output: str, key: str) -> str | None:
        match = re.search(rf"^\s*{re.escape(key)}:\s+([^\n]+)\s*$", output, re.MULTILINE)
        if not match:
            return None
        value = match.group(1).strip()
        if value.startswith('"') and value.endswith('"'):
            value = value[1:-1]
        return value

    def _extract_compile_metrics(self, output: str) -> dict[str, str]:
        sanitized = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", output)
        metrics: dict[str, str] = {}
        flash_match = re.search(
            r"Flash:\s+\[[^\]]+\]\s+([0-9.]+%)\s+\(used\s+([0-9]+)\s+bytes\s+from\s+([0-9]+)\s+bytes\)",
            sanitized,
        )
        ram_match = re.search(
            r"RAM:\s+\[[^\]]+\]\s+([0-9.]+%)\s+\(used\s+([0-9]+)\s+bytes\s+from\s+([0-9]+)\s+bytes\)",
            sanitized,
        )
        if flash_match:
            metrics["flash_percent"] = flash_match.group(1)
            metrics["flash_used_bytes"] = flash_match.group(2)
            metrics["flash_total_bytes"] = flash_match.group(3)
        if ram_match:
            metrics["ram_percent"] = ram_match.group(1)
            metrics["ram_used_bytes"] = ram_match.group(2)
            metrics["ram_total_bytes"] = ram_match.group(3)
        return metrics

    def _verify_mqtt_round_trip(self) -> None:
        topic = f"{self.topic_prefix}/test_runner_probe"
        payload = f"probe-{int(time.time())}"
        self._log(f"Checking MQTT round-trip on {topic}")
        sub_cmd = self._mosquitto_sub_command(topic=topic, count=1, timeout=4)
        proc = subprocess.Popen(sub_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.2)
        self._publish_text(topic, payload, retain=False)
        stdout, stderr = proc.communicate(timeout=6)
        if proc.returncode != 0 or stdout.strip() != payload:
            raise TestFailure(f"MQTT broker verification failed for {topic}: {stderr.strip() or stdout.strip()}")

    def _publish_text(self, topic: str, payload: str, *, retain: bool) -> None:
        cmd = self._mosquitto_pub_command(topic=topic, payload=payload, retain=retain)
        result = self._run_command(cmd, check=False)
        if result.returncode != 0:
            raise TestFailure(f"Failed to publish MQTT message to {topic}: {result.stderr.strip()}")

    def _read_retained_topic(self, suffix: str, *, timeout: int = 4) -> Any:
        cmd = self._mosquitto_sub_command(topic=f"{self.topic_prefix}/{suffix}", count=1, timeout=timeout)
        result = self._run_command(cmd, check=False)
        if result.returncode != 0 or not result.stdout.strip():
            raise TestFailure(f"Failed to read retained MQTT payload for {suffix}: {result.stderr.strip() or result.stdout.strip()}")
        payload = result.stdout.strip()
        if suffix == "availability":
            return payload
        return json.loads(payload)

    def _wait_for_runtime_availability(self, timeout: int = 60) -> None:
        deadline = time.monotonic() + timeout
        last_error = "Timed out"
        self._log(f"Waiting for DUT to reconnect and publish retained availability (up to {timeout}s)")
        while time.monotonic() < deadline:
            remaining = max(1, min(5, int(deadline - time.monotonic())))
            try:
                availability = self._read_retained_topic("availability", timeout=remaining)
            except TestFailure as err:
                last_error = str(err)
                time.sleep(1.0)
                continue
            if availability == "online":
                self._log("DUT retained availability is online")
                return
            last_error = f"Unexpected availability payload: {availability}"
            time.sleep(1.0)
        raise TestFailure(f"DUT did not publish retained availability after runtime preparation: {last_error}")

    def _publish_set_and_capture_state(self, payload: dict[str, Any]) -> dict[str, Any]:
        sub_cmd = self._mosquitto_sub_command(
            topic=f"{self.topic_prefix}/state",
            count=1,
            timeout=8,
            suppress_retained=True,
        )
        proc = subprocess.Popen(sub_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.2)
        self._publish_text(f"{self.topic_prefix}/set", json.dumps(payload, separators=(",", ":")), retain=False)
        stdout, stderr = proc.communicate(timeout=10)
        if proc.returncode != 0 or not stdout.strip():
            raise TestFailure(f"Failed to capture state publish for set payload {payload}: {stderr.strip() or stdout.strip()}")
        return json.loads(stdout.strip())

    def _publish_set_and_capture_state_and_debug(self, payload: dict[str, Any]) -> dict[str, Any]:
        debug_cmd = self._mosquitto_sub_command(topic="esphome-panaac-v2/debug", count=6, timeout=6)
        debug_proc = subprocess.Popen(debug_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        state_cmd = self._mosquitto_sub_command(
            topic=f"{self.topic_prefix}/state",
            count=1,
            timeout=8,
            suppress_retained=True,
        )
        state_proc = subprocess.Popen(state_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.2)
        self._publish_text(f"{self.topic_prefix}/set", json.dumps(payload, separators=(",", ":")), retain=False)
        state_stdout, state_stderr = state_proc.communicate(timeout=10)
        debug_stdout, debug_stderr = debug_proc.communicate(timeout=8)
        if state_proc.returncode != 0 or not state_stdout.strip():
            raise TestFailure(f"Failed to capture state publish for atomic set payload {payload}: {state_stderr.strip() or state_stdout.strip()}")
        if debug_proc.returncode != 0 and not debug_stdout.strip():
            raise TestFailure(f"Failed to capture DUT debug logs for atomic set payload {payload}: {debug_stderr.strip() or debug_stdout.strip()}")
        return {
            "state": json.loads(state_stdout.strip()),
            "logs": [line.strip() for line in debug_stdout.splitlines() if line.strip()],
        }

    def _ensure_reference_state(self) -> None:
        reference = {
            "mode": RETAINED_REFERENCE_STATE["mode"],
            "target_temperature": RETAINED_REFERENCE_STATE["target_temperature"],
            "fan_mode": RETAINED_REFERENCE_STATE["fan_mode"],
            "swing_mode": RETAINED_REFERENCE_STATE["swing_mode"],
            "swing_horizontal_mode": RETAINED_REFERENCE_STATE["swing_horizontal_mode"],
        }
        current_state = self._read_retained_topic("state")
        if not self._compare_expected(reference, current_state):
            return
        self._publish_text(f"{self.topic_prefix}/set", json.dumps(reference, separators=(",", ":")), retain=False)
        actual_state = self._wait_for_retained_state_subset(reference)
        mismatches = self._compare_expected(reference, actual_state)
        if mismatches:
            raise TestFailure(f"Failed to restore reference state before MQTT set case: {mismatches}")

    def _wait_for_retained_state_subset(self, expected: dict[str, Any], timeout: float = 8.0, interval: float = 0.4) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        latest: dict[str, Any] = {}
        while time.monotonic() < deadline:
            latest = self._read_retained_topic("state")
            if not self._compare_expected(expected, latest):
                return latest
            time.sleep(interval)
        return latest

    def _publish_invalid_set_and_capture_state(self, payload_text: str) -> dict[str, Any] | None:
        sub_cmd = self._mosquitto_sub_command(
            topic=f"{self.topic_prefix}/state",
            count=1,
            timeout=3,
            suppress_retained=True,
        )
        proc = subprocess.Popen(sub_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.2)
        self._publish_text(f"{self.topic_prefix}/set", payload_text, retain=False)
        stdout, _ = proc.communicate(timeout=5)
        if proc.returncode == 0 and stdout.strip():
            return json.loads(stdout.strip())
        return None

    def _press_button_and_capture(self, topic: str, *, capture_state: bool) -> tuple[list[str], dict[str, Any] | None]:
        debug_cmd = self._mosquitto_sub_command(topic="esphome-panaac-v2/debug", count=6, timeout=5)
        debug_proc = subprocess.Popen(debug_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        state_proc: subprocess.Popen[str] | None = None
        if capture_state:
            state_cmd = self._mosquitto_sub_command(
                topic=f"{self.topic_prefix}/state",
                count=1,
                timeout=5,
                suppress_retained=True,
            )
            state_proc = subprocess.Popen(state_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.2)
        self._publish_text(topic, "PRESS", retain=False)
        state_payload: dict[str, Any] | None = None
        if state_proc is not None:
            state_stdout, state_stderr = state_proc.communicate(timeout=7)
            if state_proc.returncode == 0 and state_stdout.strip():
                state_payload = json.loads(state_stdout.strip())
            else:
                raise TestFailure(f"Failed to capture state publish after pressing {topic}: {state_stderr.strip() or state_stdout.strip()}")
        debug_stdout, _ = debug_proc.communicate(timeout=7)
        debug_lines = [line.strip() for line in debug_stdout.splitlines() if line.strip()]
        return debug_lines, state_payload

    def _payload_matches(self, expected: Any, actual: Any) -> bool:
        if isinstance(expected, dict) and isinstance(actual, dict):
            return not self._compare_expected(expected, actual)
        return expected == actual

    def _state_has_required_keys(self, payload: Any) -> bool:
        if not isinstance(payload, dict):
            return False
        if payload.get("available") is not True:
            return False
        return all(key in payload for key in RETAINED_STATE_REQUIRED_KEYS)

    def _missing_log_fragments(self, lines: list[str], fragments: tuple[str, ...]) -> list[str]:
        return [fragment for fragment in fragments if not any(fragment in line for line in lines)]

    def _count_log_fragment(self, lines: list[str], fragment: str) -> int:
        return sum(1 for line in lines if fragment in line)

    def _mqtt_command_host(self) -> str:
        if self.mqtt_broker_mode == "external" and str(self.mqtt_host).startswith("127."):
            return self.runtime_broker_host
        return self.mqtt_host

    def _mosquitto_common(self) -> list[str]:
        cmd = ["-h", self._mqtt_command_host(), "-p", str(self.mqtt_port)]
        if self.mqtt_user:
            cmd.extend(["-u", self.mqtt_user])
        if self.mqtt_pass:
            cmd.extend(["-P", self.mqtt_pass])
        return cmd

    def _mosquitto_pub_command(self, *, topic: str, payload: str, retain: bool) -> list[str]:
        cmd = ["mosquitto_pub", *self._mosquitto_common(), "-t", topic, "-m", payload]
        if retain:
            cmd.append("-r")
        return cmd

    def _mosquitto_sub_command(self, *, topic: str, count: int, timeout: int, suppress_retained: bool = False) -> list[str]:
        cmd = ["mosquitto_sub", *self._mosquitto_common(), "-C", str(count), "-t", topic, "-W", str(timeout)]
        if suppress_retained:
            cmd.append("-R")
        return cmd

    def _run_command(
        self,
        cmd: list[str],
        *,
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
        check: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        self._log(f"$ {' '.join(cmd)}")
        proc = subprocess.Popen(
            cmd,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        stdout_chunks: list[str] = []
        stderr_chunks: list[str] = []
        selector = selectors.DefaultSelector()
        if proc.stdout is not None:
            selector.register(proc.stdout, selectors.EVENT_READ, ("stdout", stdout_chunks))
        if proc.stderr is not None:
            selector.register(proc.stderr, selectors.EVENT_READ, ("stderr", stderr_chunks))
        while selector.get_map():
            for key, _ in selector.select():
                stream_name, chunks = key.data
                line = key.fileobj.readline()
                if line == "":
                    selector.unregister(key.fileobj)
                    continue
                chunks.append(line)
                target = sys.stdout if stream_name == "stdout" else sys.stderr
                target.write(line)
                target.flush()
        returncode = proc.wait()
        result = subprocess.CompletedProcess(cmd, returncode, "".join(stdout_chunks), "".join(stderr_chunks))
        if stderr_chunks and returncode == 0:
            self._log(f"[warn] command wrote to stderr: {' '.join(cmd)}")
        if check and result.returncode != 0:
            raise TestFailure(f"Command failed: {' '.join(cmd)}\n{result.stderr}")
        return result

    def _cleanup(self) -> None:
        self._stop_mqtt_broker()

    def _overall_status(self) -> bool:
        failing_statuses = {"fail", "blocked"} if self.mode == "full-hil" else {"fail"}
        return not any(case.status in failing_statuses for group in self.groups for case in group.cases)

    def _compare_expected(self, expected: dict[str, Any], actual: dict[str, Any]) -> dict[str, dict[str, Any]]:
        return {
            key: {"expected": value, "actual": actual.get(key)}
            for key, value in expected.items()
            if actual.get(key) != value
        }

    def _summary_counts(self) -> dict[str, int]:
        counts = {"pass": 0, "fail": 0, "skip": 0, "blocked": 0}
        for group in self.groups:
            for case in group.cases:
                if case.status in counts:
                    counts[case.status] += 1
        return counts

    def _write_reports(self) -> None:
        self._log(f"Writing JSON report to {self.report_json_path}")
        self._log(f"Writing Markdown report to {self.report_md_path}")
        payload = {
            "timestamp": self.timestamp.isoformat(),
            "mode": self.mode,
            "suites": sorted(self.selected_suites, key=SUITE_CHOICES.index),
            "topic_prefix": self.topic_prefix,
            "mqtt_broker_mode": self.mqtt_broker_mode,
            "mqtt_host": self.mqtt_host,
            "mqtt_port": self.mqtt_port,
            "runtime_broker_host": self.runtime_broker_host,
            "esphome_device": self.esphome_device,
            "flush_mqtt_before_runtime": self.flush_mqtt_before_runtime,
            "flash_dut_before_runtime": self.flash_dut_before_runtime,
            "summary": {"overall": "pass" if self._overall_status() else "fail", **self._summary_counts()},
            "groups": [self._group_to_json(group) for group in self.groups],
        }
        self.report_json_path.write_text(json.dumps(payload, indent=2) + "\n")
        self.report_md_path.write_text(self._report_markdown(payload))

    def _log(self, message: str) -> None:
        print(message, flush=True)

    def _group_to_json(self, group: GroupResult) -> dict[str, Any]:
        return {
            "id": group.id,
            "title": group.title,
            "cases": [asdict(case) for case in group.cases],
        }

    def _report_markdown(self, payload: dict[str, Any]) -> str:
        lines = [
            "# PanaAC v2 automated test report",
            "",
            f"Timestamp: `{payload['timestamp']}`",
            f"Mode: `{payload['mode']}`",
            f"Suites: `{', '.join(payload['suites'])}`",
            f"Topic prefix: `{payload['topic_prefix']}`",
            f"MQTT broker: `{payload['mqtt_broker_mode']}` @ {payload['mqtt_host']}:{payload['mqtt_port']}",
            f"DUT broker host: `{payload['runtime_broker_host']}`",
            f"DUT reflashing: `{payload['flash_dut_before_runtime']}`",
            f"MQTT flush before runtime: `{payload['flush_mqtt_before_runtime']}`",
            "",
            "## Summary",
            "",
        ]
        for key in ("overall", "pass", "fail", "skip", "blocked"):
            lines.append(f"- `{key}`: {payload['summary'][key]}")
        for group in payload["groups"]:
            lines.extend(["", f"## {group['title']}", ""])
            for case in group["cases"]:
                lines.append(f"### {case['id']} — {case['status']}")
                lines.append("")
                lines.append(f"- Expected: `{json.dumps(case['expected'], ensure_ascii=True) if isinstance(case['expected'], (dict, list)) else case['expected']}`")
                lines.append(f"- Actual: `{json.dumps(case['actual'], ensure_ascii=True) if isinstance(case['actual'], (dict, list)) else case['actual']}`")
                if case["evidence"]:
                    lines.append(f"- Evidence: `{json.dumps(case['evidence'], ensure_ascii=True)}`")
                lines.append(f"- Duration: `{case['duration_s']:.2f}s`")
                lines.append("")
        return "\n".join(lines).rstrip() + "\n"


def resolve_suite_selection(values: list[str] | None) -> list[str]:
    if not values:
        return list(SUITE_CHOICES)
    resolved: list[str] = []
    for value in values:
        if value == "all":
            resolved.extend(SUITE_CHOICES)
            continue
        if value not in SUITE_CHOICES:
            raise TestFailure(f"Unknown suite selection: {value}")
        resolved.append(value)
    return sorted(set(resolved), key=lambda item: SUITE_CHOICES.index(item))
