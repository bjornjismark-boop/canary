#!/usr/bin/env python3
"""Safe operator-side controller for a mixed PlayerBots live soak.

The server-side observer and the ordinary client are deliberately explicit
dependencies.  This controller never guesses a process, database, or client.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import secrets
import shutil
import shlex
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

MAX_DURATION = 7 * 24 * 60 * 60
MAX_BOTS = 1024
SECRET = re.compile(r"(?i)(pass(word)?|secret|token|credential|key)")
TEST_NAME = re.compile(r"(?i)(test|playerbot|soak)")
PRODUCTION_NAME = re.compile(r"(?i)^(canary|production|prod|live|otservbr-global)$")
ARTIFACTS = (
    "metadata.json", "environment-redacted.json", "server-command.txt", "server.log",
    "client.log", "events.jsonl", "fleet-snapshots.jsonl", "sessions.csv", "rss.csv",
    "ticks.csv", "commands.jsonl", "restarts.jsonl", "faults.json", "invariants.json",
    "summary.json", "summary.md", "ticks.jsonl", "command-requests.jsonl",
    "ordinary-client-ready.txt", "managed-lifecycle.jsonl", "ordinary-client-events.jsonl",
)


class SoakError(RuntimeError):
    pass


def utc() -> str:
    return datetime.now(timezone.utc).isoformat()


def load_env(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise SoakError("explicit environment file does not exist")
    values: dict[str, str] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise SoakError(f"malformed environment line {number}")
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip("'\"")
    return values


def database_name(env: dict[str, str]) -> str:
    names = {env.get(k, "") for k in ("SOAK_DB_NAME", "TEST_DB_NAME", "MYSQL_DATABASE") if env.get(k)}
    if len(names) != 1:
        raise SoakError("exactly one explicit test database identity is required")
    name = names.pop()
    if not re.fullmatch(r"[A-Za-z0-9_]+", name) or not TEST_NAME.search(name) or PRODUCTION_NAME.fullmatch(name):
        raise SoakError("database identity is not visibly disposable")
    if env.get("TEST_DB_ALLOW_RESET") != "1":
        raise SoakError("TEST_DB_ALLOW_RESET=1 is required")
    return name


def redact(env: dict[str, str]) -> dict[str, str]:
    return {key: "[redacted]" if SECRET.search(key) else value for key, value in sorted(env.items())}


def proc_identity(pid: int, root: Path = Path("/proc")) -> str:
    stat = (root / str(pid) / "stat").read_text(encoding="ascii")
    close = stat.rfind(")")
    fields = stat[close + 2:].split()
    if close < 0 or len(fields) < 20:
        raise SoakError("malformed process stat")
    return fields[19]


def parse_proc_status(text: str) -> tuple[int, int | None]:
    found: dict[str, int] = {}
    for line in text.splitlines():
        match = re.fullmatch(r"(VmRSS|VmHWM):\s+(\d+)\s+kB", line)
        if match:
            found[match.group(1)] = int(match.group(2))
    if "VmRSS" not in found:
        raise SoakError("VmRSS missing from process status")
    return found["VmRSS"], found.get("VmHWM")


def percentile(values: Iterable[int], fraction: float) -> int:
    ordered = sorted(values)
    if not ordered:
        raise SoakError("latency series is empty")
    rank = max(1, int((fraction * len(ordered)) + 0.999999999))
    return ordered[rank - 1]


def parse_tick_snapshot(line: str) -> dict[str, Any]:
    try: value = json.loads(line)
    except json.JSONDecodeError as exc: raise SoakError("malformed dispatcher snapshot") from exc
    required = ("samples", "p50Us", "p95Us", "p99Us", "maxUs")
    if any(not isinstance(value.get(key), int) or value[key] < 0 for key in required):
        raise SoakError("invalid dispatcher snapshot")
    # Histogram percentiles are reported as bucket upper bounds and may be
    # greater than the exact maximum inside the selected bucket.
    if not value["p50Us"] <= value["p95Us"] <= value["p99Us"]:
        raise SoakError("inconsistent dispatcher percentiles")
    result = {key: value[key] for key in required}
    if "buckets" in value or "bucketUpperBoundsUs" in value:
        buckets=value.get("buckets"); bounds=value.get("bucketUpperBoundsUs")
        if not isinstance(buckets,list) or not isinstance(bounds,list) or len(buckets)!=len(bounds)+1 or sum(buckets)!=value["samples"]:
            raise SoakError("invalid dispatcher histogram")
        result.update({"buckets":buckets,"bucketUpperBoundsUs":bounds})
    return result


def merged_histogram_percentile(ticks: list[dict[str, Any]], fraction: float) -> int:
    supported=[item for item in ticks if "buckets" in item]
    if not supported: raise SoakError("dispatcher histograms are missing")
    bounds=supported[0]["bucketUpperBoundsUs"]; buckets=[0]*(len(bounds)+1); maximum=0
    for item in supported:
        if item["bucketUpperBoundsUs"]!=bounds: raise SoakError("dispatcher histogram bounds changed")
        buckets=[left+right for left,right in zip(buckets,item["buckets"],strict=True)]; maximum=max(maximum,item["maxUs"])
    total=sum(buckets)
    if not total: raise SoakError("latency series is empty")
    rank=max(1,int(fraction*total+0.999999999)); cumulative=0
    for index,count in enumerate(buckets):
        cumulative+=count
        if cumulative>=rank: return bounds[index] if index<len(bounds) else maximum
    return maximum


def write_checksums(directory: Path) -> None:
    with (directory / "SHA256SUMS").open("w", encoding="utf-8") as sums:
        for name in ARTIFACTS:
            data = (directory / name).read_bytes()
            sums.write(f"{hashlib.sha256(data).hexdigest()}  {name}\n")


def evaluate_adapter_artifacts(directory: Path, bots: int) -> tuple[int, list[str]]:
    snapshots = [json.loads(line) for line in (directory / "fleet-snapshots.jsonl").read_text().splitlines() if line]
    ticks = [parse_tick_snapshot(line) for line in (directory / "ticks.jsonl").read_text().splitlines() if line]
    failures: list[str] = []
    if not snapshots: failures.append("required_fleet_samples_missing")
    if not ticks or not any(item["samples"] for item in ticks): failures.append("required_latency_samples_missing")
    if snapshots and not any(item.get("ordinaryPlayers") == 1 for item in snapshots): failures.append("ordinary_protocol_player_missing")
    if snapshots and max((item.get("managedSessions", 0) for item in snapshots), default=0) < bots: failures.append("configured_fleet_never_reached")
    with (directory / "sessions.csv").open("w", newline="", encoding="utf-8") as output:
        writer=csv.writer(output); writer.writerow(["timestamp_ms","managed_sessions","ordinary_players","placed"])
        for item in snapshots: writer.writerow([item.get("timestampMilliseconds"),item.get("managedSessions"),item.get("ordinaryPlayers"),item.get("placed")])
    with (directory / "ticks.csv").open("w", newline="", encoding="utf-8") as output:
        writer=csv.writer(output); writer.writerow(["samples","p50_us","p95_us","p99_us","max_us"])
        for item in ticks: writer.writerow([item["samples"],item["p50Us"],item["p95Us"],item["p99Us"],item["maxUs"]])
    with (directory / "invariants.json").open("w", encoding="utf-8") as output:
        evaluations=[]
        for snapshot in snapshots:
            current=validate_snapshot(snapshot,bots); failures.extend(current)
            evaluations.append({"timestamp":snapshot.get("timestampMilliseconds"),"result":"FAIL" if current else "PASS","failures":current})
        json.dump({"evaluations":evaluations,"failures":sorted(set(failures))},output,indent=2); output.write("\n")
    return len(ticks), sorted(set(failures))


def validate_snapshot(s: dict[str, Any], bots: int) -> list[str]:
    failures: list[str] = []
    nonnegative = ("offline", "loginQueued", "loading", "placementPending", "placed", "draining",
                   "saving", "logoutPending", "failed", "ordinaryPlayers", "lifecycleQueueDepth",
                   "commandQueueDepth", "coordinationReservations")
    for key in nonnegative:
        if not isinstance(s.get(key), int) or s[key] < 0:
            failures.append(f"invalid_state_count:{key}")
    if s.get("duplicateSessions", 0): failures.append("duplicate_playerbot_session")
    if s.get("managedSessions", 0) > s.get("hardMaximum", MAX_BOTS): failures.append("hard_maximum_exceeded")
    if s.get("ordinaryInFleet", False): failures.append("ordinary_player_in_fleet")
    if s.get("placed", 0) > s.get("managedSessions", 0): failures.append("placed_without_authoritative_session")
    if s.get("managedSessions", 0) == 0 and s.get("coordinationReservations", 0): failures.append("stale_coordination_reservation")
    if s.get("pressureState") == "critical" and s.get("newLogins", 0): failures.append("login_during_critical_overload")
    if s.get("lifecycleQueueDepth", 0) > max(64, bots * 4): failures.append("unbounded_lifecycle_queue")
    if s.get("commandQueueDepth", 0) > 64: failures.append("unbounded_command_queue")
    return failures


@dataclass
class ManagedLifecycleMonitor:
    event_lines: int = 0
    seen_generations: dict[int, int] = None
    active_members: set[int] = None
    managed_ping_timeouts: int = 0
    unexpected_logouts: int = 0
    unexpected_logins: int = 0
    unscheduled_replacements: int = 0
    affected: set[str] = None

    def __post_init__(self) -> None:
        self.seen_generations = {} if self.seen_generations is None else self.seen_generations
        self.active_members = set() if self.active_members is None else self.active_members
        self.affected = set() if self.affected is None else self.affected

    def observe(self, directory: Path, restart_window: bool = False) -> list[str]:
        failures: list[str] = []
        path = directory / "managed-lifecycle.jsonl"
        lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
        if len(lines) < self.event_lines: return ["managed_lifecycle_truncated"]
        for line in lines[self.event_lines:]:
            try: event = json.loads(line)
            except json.JSONDecodeError: failures.append("malformed_managed_lifecycle_event"); continue
            member_id = event.get("memberId"); generation = event.get("sessionGeneration")
            identity = f"{member_id}:{event.get('name', '')}"
            if not isinstance(member_id, int) or not isinstance(generation, int):
                failures.append("malformed_managed_lifecycle_event"); continue
            action, reason = event.get("action"), event.get("reason")
            if reason == "ping_timeout": self.managed_ping_timeouts += 1; self.affected.add(identity); failures.append("managed_ping_timeout")
            if action == "login":
                previous = self.seen_generations.get(member_id)
                if previous is not None and not restart_window:
                    self.unexpected_logins += 1; self.affected.add(identity); failures.append("unexpected_managed_login")
                    if previous != generation:
                        self.unscheduled_replacements += 1; failures.append("unscheduled_session_replacement")
                self.seen_generations[member_id] = generation; self.active_members.add(member_id)
            elif action in ("logout", "unexpected_loss"):
                # The old process can report either its orderly manager drain or
                # world removal while the controlled SIGTERM is in progress.
                expected = restart_window
                # A world-removal logout is the deterministic cleanup companion
                # to the preceding unexpected_loss event, not a second loss.
                companion_cleanup = action == "logout" and reason == "world_removal"
                if not expected and not companion_cleanup:
                    self.unexpected_logouts += 1; self.affected.add(identity); failures.append("unexpected_managed_logout")
                if action == "unexpected_loss" and not restart_window:
                    self.unscheduled_replacements += 1; failures.append("population_churn_hidden_by_reconciliation")
                self.active_members.discard(member_id)
        self.event_lines = len(lines)

        snapshots = (directory / "fleet-snapshots.jsonl").read_text(encoding="utf-8").splitlines()
        if snapshots:
            try: latest = json.loads(snapshots[-1])
            except json.JSONDecodeError: return failures + ["malformed_fleet_snapshot"]
            members = latest.get("managedMembers")
            if not isinstance(members, list): return failures + ["managed_member_identities_missing"]
            current: set[int] = set()
            for member in members:
                if not isinstance(member, dict) or not isinstance(member.get("id"), int) or not isinstance(member.get("generation"), int):
                    failures.append("malformed_managed_member_identity"); continue
                member_id, generation = member["id"], member["generation"]
                current.add(member_id)
                previous = self.seen_generations.get(member_id)
                if previous is not None and previous != generation and not restart_window:
                    self.unscheduled_replacements += 1; self.affected.add(str(member_id)); failures.append("unscheduled_session_replacement")
                self.seen_generations[member_id] = generation
                if not member.get("authoritativelyPlaced", False) and not restart_window: failures.append("managed_session_not_authoritatively_placed")
            missing = self.active_members - current
            if missing and not restart_window:
                self.unexpected_logouts += len(missing); self.affected.update(map(str, missing)); failures.append("population_churn_hidden_by_reconciliation")
            self.active_members = current
        return sorted(set(failures))


def check_loopback_config(path: Path) -> None:
    if not path.is_file() or path.name not in ("config.lua", "config.lua.dist"):
        raise SoakError("--config must be an explicit config.lua or repository config.lua.dist")
    text = path.read_text(encoding="utf-8")
    match = re.search(r'^\s*ip\s*=\s*["\']([^"\']+)', text, re.MULTILINE)
    if not match or match.group(1) not in ("127.0.0.1", "::1", "localhost"):
        raise SoakError("test configuration must bind to loopback")


def ordinary_client_events(directory: Path) -> list[dict[str, Any]]:
    try:
        return [json.loads(line) for line in (directory / "ordinary-client-events.jsonl").read_text(encoding="utf-8").splitlines() if line]
    except (OSError, json.JSONDecodeError):
        return []


def client_exit_diagnostics(directory: Path, process: subprocess.Popen[bytes], reason: str, restart_window: bool) -> dict[str, Any]:
    latest = ordinary_client_events(directory)[-1] if ordinary_client_events(directory) else {}
    code = process.poll()
    try: tail = (directory / "client.log").read_text(encoding="utf-8", errors="replace")[-1000:]
    except OSError: tail = ""
    tail = re.sub(r"(?i)(password|account|token|secret)\s*[=:]\s*\S+", r"\1=[redacted]", tail)
    return {
        "exitCode": code, "signal": -code if isinstance(code, int) and code < 0 else None,
        "normalizedReason": reason, "connectedRuntimeSeconds": latest.get("connectedRuntimeSeconds", 0),
        "pongCount": latest.get("pongCount", 0), "activityCount": latest.get("activityCount", 0),
        "lastPongAgeSeconds": latest.get("lastPongAgeSeconds"), "lastActivityAgeSeconds": latest.get("lastActivityAgeSeconds"),
        "connectedGeneration": latest.get("connectedGeneration", 0), "restartWindow": restart_window,
        "stderrTail": tail,
    }


def lua_string(value: str) -> str:
    if "\n" in value or "\r" in value or "\0" in value:
        raise SoakError("configuration value contains a forbidden control character")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def allocate_ports(count: int = 3) -> list[int]:
    sockets: list[socket.socket] = []
    try:
        for _ in range(count):
            item = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            item.bind(("127.0.0.1", 0)); sockets.append(item)
        return [item.getsockname()[1] for item in sockets]
    finally:
        for item in sockets: item.close()


def create_runtime_directory(source_config: Path, run_directory: Path, repo: Path,
                             env: dict[str, str], db_name: str, bots: int) -> tuple[Path, dict[str, int]]:
    check_loopback_config(source_config)
    runtime = run_directory / "runtime"
    runtime.mkdir(mode=0o700)
    ports = dict(zip(("login", "game", "status", "legacy1100", "legacy860"), allocate_ports(5), strict=True))
    for name in ("data", "data-canary", "data-otservbr-global", "key.pem"):
        source = repo / name
        if source.exists(): (runtime / name).symlink_to(source, target_is_directory=source.is_dir())
    (runtime / "config").mkdir(mode=0o700)
    config = source_config.read_text(encoding="utf-8")
    overrides = {
        "ip": "127.0.0.1", "bindOnlyGlobalAddress": True,
        "loginProtocolPort": ports["login"], "gameProtocolPort": ports["game"],
        "legacy1100GameProtocolPort": ports["legacy1100"], "legacy860GameProtocolPort": ports["legacy860"],
        "statusProtocolPort": ports["status"], "mysqlHost": env.get("TEST_DB_HOST", "127.0.0.1"),
        "mysqlUser": env["TEST_DB_USER"], "mysqlPass": env["TEST_DB_PASSWORD"], "mysqlDatabase": db_name,
        "mysqlPort": int(env.get("TEST_DB_PORT", "3306")), "mysqlSock": env.get("TEST_DB_SOCKET", ""),
        "allowOldProtocol": True, "metricsEnablePrometheus": False,
        "metricsPrometheusAddress": "127.0.0.1:0", "authType": "password",
    }
    rendered = [config, "\n-- Generated by playerbots_live_soak.py; disposable loopback run only.\n"]
    for key, value in overrides.items():
        if isinstance(value, bool): encoded = "true" if value else "false"
        elif isinstance(value, int): encoded = str(value)
        else: encoded = lua_string(value)
        rendered.append(f"{key} = {encoded}\n")
    generated = runtime / "config.lua"
    generated.write_text("".join(rendered), encoding="utf-8"); generated.chmod(0o600)
    members = [{"id": 10001 + index, "name": f"Soak Bot {index + 1}", "vocationCategory": (index % 4) + 1,
                "levelRangeCategory": 1, "plannerPolicyId": 1, "coordinationGroupId": 1,
                "roleId": 1, "partyProfileId": 1, "allowedRegionIds": [1], "priority": 1}
               for index in range(bots)]
    fleet = {"schemaVersion": 1, "revision": 1,
             "population": {"enabled": True, "desiredOnline": bots, "minimumOnline": 0,
                            "maximumOnline": bots, "absoluteHardMaximum": max(bots, 4),
                            "maximumLoginsPerInterval": 1, "maximumLogoutsPerInterval": 1,
                            "maximumPendingLogins": 1, "maximumPendingLogouts": 1,
                            "maximumRetries": 3, "startupDelayTicks": 2000,
                            "maximumRetryBackoffTicks": 64, "drainTimeoutTicks": 1024,
                            "drainTarget": 0, "maximumSessionTicks": 0,
                            "overloadRecoveryIntervals": 2, "revision": 1, "overloadPause": True},
             "distribution": [], "members": members, "schedule": []}
    (runtime / "config" / "playerbots.json").write_text(json.dumps(fleet, indent=2) + "\n", encoding="utf-8")
    return runtime, ports


class DatabaseLifecycle:
    def __init__(self, env: dict[str, str], unique_name: str, run_directory: Path, bots: int, ordinary_password: str):
        self.env, self.name, self.run_directory = env, unique_name, run_directory
        self.bots = bots
        self.ordinary_password = ordinary_password
        self.client = env.get("TEST_DB_CLIENT") or shutil.which("mysql") or shutil.which("mariadb")
        if not self.client: raise SoakError("mysql or mariadb client is required")
        self.defaults = run_directory / "mysql-client.cnf"

    def _write_defaults(self) -> None:
        lines = ["[client]", f"host={self.env.get('TEST_DB_HOST', '127.0.0.1')}",
                 f"user={self.env['TEST_DB_USER']}", f"password={self.env['TEST_DB_PASSWORD']}",
                 f"port={self.env.get('TEST_DB_PORT', '3306')}"]
        if self.env.get("TEST_DB_SOCKET"): lines.append(f"socket={self.env['TEST_DB_SOCKET']}")
        self.defaults.write_text("\n".join(lines) + "\n", encoding="utf-8"); self.defaults.chmod(0o600)

    def _run(self, *args: str, stdin=None) -> None:
        completed = subprocess.run([self.client, f"--defaults-extra-file={self.defaults}", *args], stdin=stdin,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=stdin is None)
        if completed.returncode:
            detail = completed.stderr.decode(errors="replace") if isinstance(completed.stderr, bytes) else completed.stderr
            raise SoakError("disposable database command failed: " + detail[-400:])

    def create(self) -> None:
        self._write_defaults()
        try:
            self._run("--execute", f"DROP DATABASE IF EXISTS `{self.name}`")
            self._run("--execute", f"CREATE DATABASE `{self.name}` CHARACTER SET utf8")
            schema = Path(self.env["TEST_DB_SCHEMA"])
            if not schema.is_file(): raise SoakError("TEST_DB_SCHEMA does not exist")
            with schema.open("rb") as source: self._run(self.name, stdin=source)
            fixtures = []
            for index in range(self.bots):
                fixture_id = 10001 + index
                name = f"Soak Bot {index + 1}"
                fixtures.append(f"INSERT INTO accounts (id,name,email,password) VALUES ({fixture_id},'soak_bot_{index + 1}','bot{index + 1}@test.invalid','');")
                # The lifecycle soak is not a combat-survival test. Give only
                # disposable managed fixtures enough health that random world
                # encounters cannot create unrelated session churn.
                fixtures.append("INSERT INTO players (id,name,account_id,group_id,vocation,town_id,health,healthmax,mana,manamax,cap,conditions,posx,posy,posz,deletion) "
                                f"VALUES ({fixture_id},'{name}',{fixture_id},1,1,1,1000000,1000000,100,100,100000,X'',0,0,0,0);")
            ordinary_password = hashlib.sha1(self.ordinary_password.encode("utf-8"), usedforsecurity=False).hexdigest()
            fixtures.append(f"INSERT INTO accounts (id,name,email,password) VALUES (20001,'ordinary_soak','ordinary@test.invalid','{ordinary_password}');")
            fixtures.append("INSERT INTO players (id,name,account_id,group_id,level,vocation,town_id,health,healthmax,mana,manamax,cap,conditions,posx,posy,posz,deletion) "
                            "VALUES (20001,'Ordinary Soak',20001,1,8,1,1,185,185,90,90,47000,X'',0,0,0,0);")
            self._run("--execute", "".join(fixtures), self.name)
        except Exception:
            try: self._run("--execute", f"DROP DATABASE IF EXISTS `{self.name}`")
            except Exception: pass
            self.defaults.unlink(missing_ok=True)
            raise

    def cleanup(self) -> None:
        if not self.defaults.exists(): self._write_defaults()
        self._run("--execute", f"DROP DATABASE IF EXISTS `{self.name}`")
        self.defaults.unlink(missing_ok=True)


def make_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser()
    p.add_argument("--env", type=Path, required=True)
    p.add_argument("--config", type=Path, required=True)
    p.add_argument("--server", type=Path, required=True)
    p.add_argument("--profile", choices=("smoke", "release"), required=True)
    p.add_argument("--duration-seconds", type=int, required=True)
    p.add_argument("--bots", type=int, required=True)
    p.add_argument("--restarts", type=int, default=1)
    p.add_argument("--output-root", type=Path, required=True)
    p.add_argument("--sample-seconds", type=float, default=1.0)
    p.add_argument("--keep-database-on-failure", action="store_true")
    p.add_argument("--allow-no-client", action="store_true", help=argparse.SUPPRESS)
    return p


@dataclass
class Run:
    args: argparse.Namespace
    env: dict[str, str]
    directory: Path
    server: subprocess.Popen[bytes] | None = None
    identity: str = ""
    failed: bool = False
    runtime: Path | None = None
    status_port: int = 0

    def event(self, kind: str, **fields: Any) -> None:
        with (self.directory / "events.jsonl").open("a", encoding="utf-8") as out:
            out.write(json.dumps({"timestamp": utc(), "kind": kind, **fields}, sort_keys=True) + "\n")

    def start(self) -> None:
        snapshot = self.directory / "fleet-snapshots.jsonl"
        previous_size = snapshot.stat().st_size if snapshot.exists() else 0
        log = (self.directory / "server.log").open("ab", buffering=0)
        child_env = {**os.environ, **self.env, "PLAYERBOTS_SOAK_OUTPUT": str(self.directory), "PLAYERBOTS_SOAK_BOTS": str(self.args.bots)}
        self.server = subprocess.Popen([str(self.args.server)], cwd=self.runtime, env=child_env,
                                       stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
        self.identity = proc_identity(self.server.pid)
        (self.directory / "server-command.txt").write_text(shlex.join([str(self.args.server)]) + "\n", encoding="utf-8")
        self.event("server_started", pid=self.server.pid, startIdentity=self.identity)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            if self.server.poll() is not None: raise SoakError("server terminated before readiness")
            if snapshot.exists() and snapshot.stat().st_size > previous_size: return
            time.sleep(.25)
        raise SoakError("server readiness timeout")

    def stop(self) -> None:
        if not self.server: return
        prior_status = self.server.poll()
        if prior_status is not None:
            raise SoakError(f"server exited before controlled shutdown with status {prior_status}")
        if proc_identity(self.server.pid) != self.identity: raise SoakError("PID identity changed")
        os.kill(self.server.pid, signal.SIGTERM)
        try: self.server.wait(timeout=30)
        except subprocess.TimeoutExpired:
            os.kill(self.server.pid, signal.SIGKILL)
            self.server.wait(timeout=10)
            raise SoakError("server did not complete controlled shutdown")
        if self.server.returncode != 0:
            raise SoakError(f"server controlled shutdown failed with status {self.server.returncode}")

    def sample(self, writer: csv.writer) -> None:
        assert self.server
        if self.server.poll() is not None: raise SoakError("unexpected server termination")
        if proc_identity(self.server.pid) != self.identity: raise SoakError("PID identity changed")
        rss, hwm = parse_proc_status(Path(f"/proc/{self.server.pid}/status").read_text(encoding="ascii"))
        writer.writerow([utc(), self.server.pid, self.identity, rss, "" if hwm is None else hwm])


def classify_release_summary(summary: dict[str, Any]) -> tuple[bool, str]:
    """Return production qualification and release-soak state."""

    if summary.get("profile") != "release":
        return False, "NOT_RUN"

    restart_count = int(summary.get("restartCount", 0) or 0)

    qualified = all((
        summary.get("result") == "PASS",
        summary.get("durationSeconds") == 7200,
        summary.get("configuredBots") == 20,
        summary.get("peakManagedPopulation") == 20,
        int(summary.get("humanParticipants", 0) or 0) >= 1,
        int(summary.get("peakOrdinaryPlayers", 0) or 0) >= 1,
        restart_count >= 3,
        int(summary.get("ordinaryClientPlacements", 0) or 0)
            >= restart_count + 1,
        int(summary.get("ordinaryActivityCount", 0) or 0) > 0,
        summary.get("ordinaryConnectedPast1000Seconds") is True,
        summary.get("unexpectedOrdinaryClientExits") == 0,
        summary.get("managedPingTimeouts") == 0,
        summary.get("unexpectedManagedLogouts") == 0,
        summary.get("unexpectedManagedLogins") == 0,
        summary.get("unscheduledSessionReplacements") == 0,
        int(summary.get("duplicateSessions", 0) or 0) == 0,
        summary.get("invariantFailureCount") == 0,
        summary.get("rssInitialKb") is not None,
        summary.get("rssPeakKb") is not None,
        summary.get("rssFinalKb") is not None,
        int(summary.get("tickSampleCount", 0) or 0) > 0,
        summary.get("cleanup") == "PASS",
    ))

    return qualified, "PASS" if qualified else "FAIL"

def render_summary_markdown(summary: dict[str, Any]) -> str:
    """Render the authoritative human-readable soak summary."""

    production_marker = (
        "YES" if summary.get("productionSoak") is True else "NO"
    )
    return (
        "# PlayerBots live soak\n\n"
        + "\n".join(
            f"{key}: {value}" for key, value in summary.items()
        )
        + f"\n\nPRODUCTION_SOAK={production_marker}\n"
    )


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    if not args.server.is_file() or not os.access(args.server, os.X_OK): raise SoakError("server must be one explicit executable file")
    if not 1 <= args.duration_seconds <= MAX_DURATION: raise SoakError("duration outside finite safety bounds")
    if not 1 <= args.bots <= MAX_BOTS: raise SoakError("bot population outside compiled hard maximum")
    if not 0.1 <= args.sample_seconds <= 60: raise SoakError("sample interval outside bounded safety limits")
    if args.profile == "release" and args.restarts < 3: raise SoakError("release profile requires at least three restarts")
    check_loopback_config(args.config)
    env = load_env(args.env)
    db = database_name(env)
    for key in ("TEST_DB_USER", "TEST_DB_PASSWORD", "TEST_DB_SCHEMA"):
        if not env.get(key): raise SoakError(f"{key} is required")
    run_id = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S") + f"-{os.getpid()}"
    directory = args.output_root / run_id
    directory.mkdir(parents=True, mode=0o700, exist_ok=False)
    directory.chmod(0o700)
    for name in ARTIFACTS: (directory / name).touch()
    # The configured account is intentionally granted only on the explicitly
    # resettable test database. Recreate that exact name; never broaden grants.
    unique_db = db
    ordinary_password = secrets.token_urlsafe(32)
    database = DatabaseLifecycle(env, unique_db, directory, args.bots, ordinary_password)
    runtime, ports = create_runtime_directory(args.config, directory, Path(__file__).resolve().parents[1], env, unique_db, args.bots)
    client_environment = directory / "ordinary-client.env"
    client_values = {
        "SOAK_HOST": "127.0.0.1", "SOAK_LOGIN_PORT": str(ports["login"]),
        "SOAK_GAME_PORT": str(ports["legacy860"]), "SOAK_ACCOUNT": "ordinary_soak",
        "SOAK_PASSWORD": ordinary_password, "SOAK_CHARACTER": "Ordinary Soak",
        "SOAK_TIMEOUT_SECONDS": "15", "SOAK_HEARTBEAT_SECONDS": "5",
        "SOAK_ACTIVITY_SECONDS": env.get("SOAK_ACTIVITY_SECONDS", "300"),
        "SOAK_READY_FILE": str(directory / "ordinary-client-ready.txt"),
        "SOAK_DIAGNOSTICS_FILE": str(directory / "ordinary-client-events.jsonl"),
    }
    client_environment.write_text("".join(f"{key}={value}\n" for key,value in client_values.items()), encoding="utf-8")
    client_environment.chmod(0o600)
    if not env.get("SOAK_CLIENT_COMMAND"):
        client_program = Path(__file__).with_name("playerbots_ordinary_client.py")
        env["SOAK_CLIENT_COMMAND"] = shlex.join([sys.executable, str(client_program), "--env-file", str(client_environment)])
    if not env.get("SOAK_CLIENT_COMMAND") and not args.allow_no_client: raise SoakError("ordinary protocol client is required")
    run = Run(args, env, directory, runtime=runtime, status_port=ports["status"])
    def interrupted(signum, _frame): raise SoakError(f"received signal {signum}")
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    exit_code = 1
    client: subprocess.Popen[bytes] | None = None
    cleanup_complete = False
    lifecycle = ManagedLifecycleMonitor()
    invariant_failures: list[str] = []
    client_generation = 0
    unexpected_client_exits = 0
    try:
        (directory / "environment-redacted.json").write_text(json.dumps(redact(env), indent=2) + "\n", encoding="utf-8")
        commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
        metadata = {"runId": run_id, "gitCommit": commit, "database": unique_db,
                    "executable": str(args.server.resolve()), "commandLine": [str(args.server.resolve())],
                    "profile": args.profile, "durationSeconds": args.duration_seconds,
                    "bots": args.bots, "productionSoak": False}
        (directory / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        database.create()
        def start_client():
            nonlocal client_generation
            if not env.get("SOAK_CLIENT_COMMAND"): return None
            client_generation += 1
            ready = directory / "ordinary-client-ready.txt"; ready.unlink(missing_ok=True)
            client_log = (directory / "client.log").open("ab", buffering=0)
            process = subprocess.Popen(shlex.split(env["SOAK_CLIENT_COMMAND"]), cwd=runtime,
                                       env={**os.environ, **env, "SOAK_CONNECTED_GENERATION": str(client_generation),
                                            "SOAK_RESTART_WINDOW": "1" if client_generation > 1 else "0"},
                                       stdout=client_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    run.event("ordinary_client_exit", **client_exit_diagnostics(directory, process, "failed_before_placement", False))
                    raise SoakError("ordinary protocol client failed before placement")
                if ready.exists() and ready.read_text(encoding="ascii").strip() == "ORDINARY_CLIENT_PLACED=YES": return process
                time.sleep(.1)
            process.terminate(); process.wait(timeout=5)
            raise SoakError("ordinary protocol client placement timeout")
        with (directory / "rss.csv").open("w", newline="", encoding="utf-8") as rss_file:
            writer = csv.writer(rss_file); writer.writerow(["timestamp", "pid", "start_identity", "vmrss_kb", "vmhwm_kb"])
            run.start()
            metadata.update({"pid": run.server.pid if run.server else None, "processStartIdentity": run.identity,
                             "runtimeDirectory": str(runtime), "ports": ports})
            (directory / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
            client = start_client()
            deadline = time.monotonic() + args.duration_seconds
            while time.monotonic() < deadline:
                if client and client.poll() is not None:
                    unexpected_client_exits += 1
                    run.event("ordinary_client_exit", **client_exit_diagnostics(directory, client, "unexpected_disconnect", False))
                    raise SoakError("ordinary protocol client terminated unexpectedly")
                run.sample(writer); rss_file.flush()
                current_failures = lifecycle.observe(directory)
                if current_failures:
                    invariant_failures.extend(current_failures)
                    raise SoakError("managed lifecycle invariant failure: " + ",".join(current_failures))
                time.sleep(args.sample_seconds)
            for restart in range(args.restarts):
                if client:
                    client.terminate(); client.wait(timeout=10); client=None
                run.stop(); run.start(); client=start_client()
                restart_deadline = time.monotonic() + max(120, args.bots * 8 + 60)
                while time.monotonic() < restart_deadline:
                    if client and client.poll() is not None:
                        unexpected_client_exits += 1
                        run.event("ordinary_client_exit", **client_exit_diagnostics(directory, client, "restart_reconnect_failed", True))
                        raise SoakError("ordinary protocol client failed to reconnect")
                    run.sample(writer); rss_file.flush()
                    current_failures = lifecycle.observe(directory, restart_window=True)
                    if current_failures:
                        invariant_failures.extend(current_failures)
                        raise SoakError("managed restart lifecycle invariant failure: " + ",".join(current_failures))
                    snapshots = (directory / "fleet-snapshots.jsonl").read_text(encoding="utf-8").splitlines()
                    latest = json.loads(snapshots[-1]) if snapshots else {}
                    if latest.get("managedSessions") == args.bots and latest.get("placed") == args.bots: break
                    time.sleep(args.sample_seconds)
                else: raise SoakError("managed fleet did not recover after controlled restart")
                with (directory/"restarts.jsonl").open("a",encoding="utf-8") as output:
                    output.write(json.dumps({"timestamp":utc(),"restart":restart+1,"result":"PASS"})+"\n")
                run.event("restart_completed", restart=restart + 1)
            run.sample(writer)
        _, invariant_failures=evaluate_adapter_artifacts(directory,args.bots)
        if invariant_failures: raise SoakError("invariant failure: " + ",".join(invariant_failures))
        if client: client.terminate(); client.wait(timeout=10)
        run.stop(); database.cleanup(); cleanup_complete = True
        exit_code = 0
    except Exception as exc:
        run.failed = True; run.event("failure", failure=type(exc).__name__, detail=str(exc))
        if client and client.poll() is None: client.terminate()
        try: run.stop()
        except Exception: pass
        if not args.keep_database_on_failure:
            try: database.cleanup(); cleanup_complete = True
            except Exception: pass
    finally:
        client_environment.unlink(missing_ok=True)
        # Always publish a valid invariant document, including signal and cleanup failures.
        try:
            _, final_invariant_failures = evaluate_adapter_artifacts(directory, args.bots)
            invariant_failures = sorted(set(invariant_failures + final_invariant_failures))
            invariant_document = json.loads((directory / "invariants.json").read_text(encoding="utf-8"))
            invariant_document["failures"] = invariant_failures
            (directory / "invariants.json").write_text(json.dumps(invariant_document, indent=2) + "\n", encoding="utf-8")
        except Exception as exc:
            invariant_failures = sorted(set(invariant_failures + [f"invariant_evaluation_failed:{type(exc).__name__}"]))
            (directory / "invariants.json").write_text(json.dumps({"evaluations": [], "failures": invariant_failures}, indent=2) + "\n", encoding="utf-8")
        rss_values=[]
        try:
            with (directory/"rss.csv").open(newline="",encoding="utf-8") as source:
                rss_values=[int(row["vmrss_kb"]) for row in csv.DictReader(source)]
        except (OSError,ValueError,KeyError): pass
        tick_values=[]
        try: tick_values=[parse_tick_snapshot(line) for line in (directory/"ticks.jsonl").read_text().splitlines() if line]
        except (OSError,SoakError): pass
        snapshots=[]
        try: snapshots=[json.loads(line) for line in (directory/"fleet-snapshots.jsonl").read_text().splitlines() if line]
        except (OSError,json.JSONDecodeError): pass
        client_events = ordinary_client_events(directory)
        client_activity_count = sum(1 for item in client_events if item.get("event") == "activity")
        client_pong_count = sum(1 for item in client_events if item.get("event") == "pong")
        client_max_runtime = max((float(item.get("connectedRuntimeSeconds", 0)) for item in client_events), default=0.0)
        client_placements = sum(1 for item in client_events if item.get("event") == "placed")
        latest_client = client_events[-1] if client_events else {}
        duplicate_sessions = max((item.get("duplicateSessions", 0) for item in snapshots), default=0)
        run.event("ordinary_client_summary", placements=client_placements, pongCount=client_pong_count,
                  activityCount=client_activity_count, maxConnectedRuntimeSeconds=client_max_runtime,
                  connectedPast1000Seconds=client_max_runtime > 1000, unexpectedExits=unexpected_client_exits,
                  connectedGeneration=latest_client.get("connectedGeneration"),
                  restartWindow=latest_client.get("restartWindow", False),
                  lastReceivedPacketCategory=latest_client.get("lastReceivedPacketCategory"))
        summary = {"result": "FAIL" if run.failed else "PASS", "gitCommit": subprocess.check_output(["git","rev-parse","HEAD"],text=True).strip(),
                   "executable":str(args.server.resolve()), "profile": args.profile, "durationSeconds":args.duration_seconds,
                   "configuredBots":args.bots, "humanParticipants":1 if env.get("SOAK_CLIENT_COMMAND") else 0,
                   "peakManagedPopulation":max((item.get("managedSessions",0) for item in snapshots),default=0),
                   "peakOrdinaryPlayers":max((item.get("ordinaryPlayers",0) for item in snapshots),default=0),
                   "restartCount":sum(1 for line in (directory/"restarts.jsonl").read_text().splitlines() if line),
                   "rssInitialKb":rss_values[0] if rss_values else None,"rssPeakKb":max(rss_values) if rss_values else None,
                   "rssFinalKb":rss_values[-1] if rss_values else None,"tickSampleCount":sum(item["samples"] for item in tick_values),
                   "tickP50Us":merged_histogram_percentile(tick_values,.50) if tick_values and sum(item["samples"] for item in tick_values) else None,
                   "tickP95Us":merged_histogram_percentile(tick_values,.95) if tick_values and sum(item["samples"] for item in tick_values) else None,
                   "tickP99Us":merged_histogram_percentile(tick_values,.99) if tick_values and sum(item["samples"] for item in tick_values) else None,
                   "tickMaxUs":max((item["maxUs"] for item in tick_values),default=None), "productionSoak": False,
                   "managedPingTimeouts":lifecycle.managed_ping_timeouts,"unexpectedManagedLogouts":lifecycle.unexpected_logouts,
                   "unexpectedManagedLogins":lifecycle.unexpected_logins,"unscheduledSessionReplacements":lifecycle.unscheduled_replacements,
                   "affectedManagedBots":sorted(lifecycle.affected),"duplicateSessions":duplicate_sessions,
                   "invariantFailureCount":len(invariant_failures),
                   "ordinaryClientPlacements":client_placements,"ordinaryPongCount":client_pong_count,
                   "ordinaryActivityCount":client_activity_count,"ordinaryMaxConnectedRuntimeSeconds":client_max_runtime,
                   "ordinaryConnectedPast1000Seconds":client_max_runtime > 1000,"unexpectedOrdinaryClientExits":unexpected_client_exits,
                   "ordinaryLastPongTimestamp":latest_client.get("lastPongTimestamp"),"ordinaryLastActivityTimestamp":latest_client.get("lastActivityTimestamp"),
                   "ordinaryLastReceivedPacketCategory":latest_client.get("lastReceivedPacketCategory"),
                   "ordinaryReconnectGeneration":latest_client.get("connectedGeneration"),"ordinaryRestartWindow":latest_client.get("restartWindow",False),
                   "mode":"live_mixed" if env.get("SOAK_CLIENT_COMMAND") else "live_server_only",
                   "cleanup":"PASS" if cleanup_complete else "FAIL",
                   "releaseSoak": "NOT_RUN",
                   "liveSmoke": ("PASS" if env.get("SOAK_CLIENT_COMMAND") else "PARTIAL_SERVER_ONLY") if not run.failed and args.profile == "smoke" else "NOT_RUN"}
        production_soak, release_soak = classify_release_summary(summary)
        summary["productionSoak"] = production_soak
        summary["releaseSoak"] = release_soak
        (directory / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        (directory / "summary.md").write_text(
            render_summary_markdown(summary), encoding="utf-8"
        )
        write_checksums(directory)
        print(f"SOAK_OUTPUT={directory}")
    return exit_code


if __name__ == "__main__":
    try: raise SystemExit(main())
    except SoakError as exc:
        print(f"playerbots-live-soak: {exc}", file=sys.stderr); raise SystemExit(64)
