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
from pathlib import Path
import re
import shutil
import subprocess
import time
from typing import Any, Callable

from .data import (
    BUTTON_TOPICS,
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
        self.topic_prefix = args.topic_prefix
        self.mqtt_host = args.mqtt_host
        self.mqtt_port = args.mqtt_port
        self.mqtt_user = args.mqtt_user
        self.mqtt_pass = args.mqtt_pass
        self.mode = args.mode
        self.selected_suites = set(args.suites or SUITE_CHOICES)
        self.timestamp = datetime.now().astimezone()
        self.report_json_path = self.output_dir / "report.json"
        self.report_md_path = self.output_dir / "report.md"
        self.groups: list[GroupResult] = []

    def run(self) -> int:
        try:
            self._validate_environment()
            if "esphome.g1" in self.selected_suites:
                self.groups.append(self._run_esphome_group_1())
            if "esphome.g2" in self.selected_suites:
                self.groups.append(self._run_esphome_group_2())
            if "esphome.g3" in self.selected_suites:
                self.groups.append(self._run_esphome_group_3())
            self._write_reports()
            return 0 if self._overall_status() else 1
        except Exception as err:  # noqa: BLE001
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

    def setup_environment(
        self,
        *,
        verify_mqtt: bool,
        verify_ha: bool,
        verify_variants: bool,
    ) -> EnvironmentStatus:
        status = EnvironmentStatus()
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

    def _run_esphome_group_1(self) -> GroupResult:
        group = GroupResult("esphome.g1", SUITE_LABELS["esphome.g1"])
        esphome_bin = self.esphome_workspace_path / ".venv" / "bin" / "esphome"
        env = os.environ.copy()
        env["PLATFORMIO_CORE_DIR"] = env.get("PLATFORMIO_CORE_DIR", "/tmp/platformio")
        env["XDG_CACHE_HOME"] = env.get("XDG_CACHE_HOME", "/tmp/.cache")

        for variant in VARIANT_ORDER:
            started = time.monotonic()
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
        for suffix, topic_suffix, expected_payload in (
            ("2.1.availability", "availability", "online"),
            ("2.1.traits", "traits", RETAINED_REFERENCE_TRAITS),
            ("2.1.state", "state", None),
        ):
            started = time.monotonic()
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
        lambda_log_debug, _ = self._press_button_and_capture(
            BUTTON_TOPICS["lambda_action_log"],
            capture_state=False,
        )
        lambda_expected_logs = ("on_state fired", "mode=", "action=", "cur=", "tgt=")
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
        for cmd in ("mosquitto_pub", "mosquitto_sub"):
            if self.mqtt_user and shutil.which(cmd) is None:
                raise TestFailure(f"Required command not found: {cmd}")
        if shutil.which("python3") is None:
            raise TestFailure("Required command not found: python3")
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

    def _read_retained_topic(self, suffix: str) -> Any:
        cmd = self._mosquitto_sub_command(topic=f"{self.topic_prefix}/{suffix}", count=1, timeout=4)
        result = self._run_command(cmd, check=False)
        if result.returncode != 0 or not result.stdout.strip():
            raise TestFailure(f"Failed to read retained MQTT payload for {suffix}: {result.stderr.strip() or result.stdout.strip()}")
        payload = result.stdout.strip()
        if suffix == "availability":
            return payload
        return json.loads(payload)

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

    def _mosquitto_common(self) -> list[str]:
        return ["-h", self.mqtt_host, "-p", str(self.mqtt_port), "-u", self.mqtt_user, "-P", self.mqtt_pass]

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
        result = subprocess.run(cmd, cwd=cwd, env=env, text=True, capture_output=True)
        if check and result.returncode != 0:
            raise TestFailure(f"Command failed: {' '.join(cmd)}\n{result.stderr}")
        return result

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
        payload = {
            "timestamp": self.timestamp.isoformat(),
            "mode": self.mode,
            "suites": sorted(self.selected_suites, key=SUITE_CHOICES.index),
            "topic_prefix": self.topic_prefix,
            "summary": {"overall": "pass" if self._overall_status() else "fail", **self._summary_counts()},
            "groups": [self._group_to_json(group) for group in self.groups],
        }
        self.report_json_path.write_text(json.dumps(payload, indent=2) + "\n")
        self.report_md_path.write_text(self._report_markdown(payload))

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
