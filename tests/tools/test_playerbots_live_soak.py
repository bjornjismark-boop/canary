import importlib.util
import sys
import tempfile
import unittest
import os
import argparse
import subprocess
import json
from unittest import mock
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("soak", Path(__file__).parents[2] / "tools/playerbots_live_soak.py")
soak = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = soak
SPEC.loader.exec_module(soak)


class LiveSoakTest(unittest.TestCase):
    def test_rss(self): self.assertEqual((42, 80), soak.parse_proc_status("VmRSS:\t42 kB\nVmHWM: 80 kB"))
    def test_malformed_rss(self):
        with self.assertRaises(soak.SoakError): soak.parse_proc_status("VmRSS: nope")
    def test_percentiles_are_nearest_rank(self):
        values = list(range(1, 101)); self.assertEqual(50, soak.percentile(values, .50)); self.assertEqual(99, soak.percentile(values, .99))
    def test_empty_percentile(self):
        with self.assertRaises(soak.SoakError): soak.percentile([], .99)
    def test_database_guard(self):
        self.assertEqual("playerbots_soak_1", soak.database_name({"TEST_DB_NAME":"playerbots_soak_1", "TEST_DB_ALLOW_RESET":"1"}))
        with self.assertRaises(soak.SoakError): soak.database_name({"TEST_DB_NAME":"production", "TEST_DB_ALLOW_RESET":"1"})
    def test_redaction(self): self.assertEqual("[redacted]", soak.redact({"MYSQL_PASSWORD":"x"})["MYSQL_PASSWORD"])
    def test_client_exit_diagnostics_are_bounded_and_redacted(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); root.joinpath("ordinary-client-events.jsonl").write_text(json.dumps({"connectedRuntimeSeconds":1001,"pongCount":200,"activityCount":3,"connectedGeneration":1})+'\n')
            root.joinpath("client.log").write_text("x"*900+"\npassword=do-not-leak\n")
            process=mock.Mock(); process.poll.return_value=1
            result=soak.client_exit_diagnostics(root,process,"unexpected_disconnect",False)
            self.assertEqual(3,result["activityCount"]); self.assertLessEqual(len(result["stderrTail"]),1000); self.assertNotIn("do-not-leak",result["stderrTail"])
    def test_invariants(self):
        base = {k: 0 for k in ("offline","loginQueued","loading","placementPending","placed","draining","saving","logoutPending","failed","ordinaryPlayers","lifecycleQueueDepth","commandQueueDepth","coordinationReservations")}
        self.assertEqual([], soak.validate_snapshot(base, 4)); base["duplicateSessions"] = 1
        self.assertIn("duplicate_playerbot_session", soak.validate_snapshot(base, 4))
    def test_managed_lifecycle_rejects_hidden_replacement(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); root.joinpath("managed-lifecycle.jsonl").write_text(
                '\n'.join(json.dumps(event) for event in (
                    {"action":"login","reason":"reconciliation","memberId":1,"name":"bot","sessionGeneration":1},
                    {"action":"unexpected_loss","reason":"world_removal","memberId":1,"name":"bot","sessionGeneration":1},
                    {"action":"login","reason":"reconciliation","memberId":1,"name":"bot","sessionGeneration":2},
                ))+'\n')
            root.joinpath("fleet-snapshots.jsonl").write_text(json.dumps({"managedMembers":[{"id":1,"generation":2,"authoritativelyPlaced":True}]})+'\n')
            failures=soak.ManagedLifecycleMonitor().observe(root)
            self.assertIn("population_churn_hidden_by_reconciliation",failures)
            self.assertIn("unscheduled_session_replacement",failures)
    def test_managed_lifecycle_rejects_ping_timeout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); root.joinpath("managed-lifecycle.jsonl").write_text(json.dumps(
                {"action":"logout","reason":"ping_timeout","memberId":7,"name":"bot","sessionGeneration":1})+'\n')
            root.joinpath("fleet-snapshots.jsonl").write_text(json.dumps({"managedMembers":[]})+'\n')
            monitor=soak.ManagedLifecycleMonitor(); failures=monitor.observe(root)
            self.assertIn("managed_ping_timeout",failures); self.assertEqual(1,monitor.managed_ping_timeouts)
    def test_controlled_restart_allows_new_generation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); root.joinpath("managed-lifecycle.jsonl").write_text(json.dumps(
                {"action":"login","reason":"reconciliation","memberId":1,"name":"bot","sessionGeneration":1})+'\n')
            root.joinpath("fleet-snapshots.jsonl").write_text(json.dumps({"managedMembers":[{"id":1,"generation":1,"authoritativelyPlaced":True}]})+'\n')
            monitor=soak.ManagedLifecycleMonitor(); self.assertEqual([],monitor.observe(root))
            with root.joinpath("managed-lifecycle.jsonl").open("a") as output: output.write(json.dumps(
                {"action":"login","reason":"reconciliation","memberId":1,"name":"bot","sessionGeneration":1})+'\n')
            self.assertEqual([],monitor.observe(root,restart_window=True))
    def test_invariants_document_is_valid_when_samples_are_empty(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); root.joinpath("fleet-snapshots.jsonl").write_text(""); root.joinpath("ticks.jsonl").write_text("")
            _,failures=soak.evaluate_adapter_artifacts(root,4)
            document=json.loads(root.joinpath("invariants.json").read_text())
            self.assertTrue(failures); self.assertEqual(failures,document["failures"])
    def test_loopback(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/"config.lua"; path.write_text('ip = "0.0.0.0"')
            with self.assertRaises(soak.SoakError): soak.check_loopback_config(path)
    def test_isolated_working_directory_generation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); config=root/"config.lua"; config.write_text('ip = "127.0.0.1"\n')
            output=root/"output"; output.mkdir()
            env={"TEST_DB_USER":"u", "TEST_DB_PASSWORD":'p"q'}
            with mock.patch.object(soak,"allocate_ports",return_value=[17171,17172,17173,17174,17175]):
                runtime,ports=soak.create_runtime_directory(config,output,root,env,"playerbots_test_live",2)
            self.assertEqual(0o600, runtime.joinpath("config.lua").stat().st_mode & 0o777)
            self.assertEqual(5,len(set(ports.values())))
            self.assertIn('mysqlPass = "p\\"q"',runtime.joinpath("config.lua").read_text())
            fleet=__import__("json").loads(runtime.joinpath("config/playerbots.json").read_text())
            self.assertEqual(2,len(fleet["members"]))
            self.assertNotIn("Ordinary Soak",[member["name"] for member in fleet["members"]])
    def test_lua_config_rejects_controls(self):
        with self.assertRaises(soak.SoakError): soak.lua_string("bad\nvalue")
    def test_pid_identity_and_exit(self):
        self.assertTrue(soak.proc_identity(os.getpid()).isdigit())
        with self.assertRaises(FileNotFoundError): soak.proc_identity(999999999)
    def test_pid_reuse_protection_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); process=root/"7"; process.mkdir(); process.joinpath("stat").write_text("7 (name) S " + "0 "*18 + "42\n")
            self.assertEqual("42",soak.proc_identity(7,root))
    def test_dispatcher_snapshot(self):
        value=soak.parse_tick_snapshot('{"samples":4,"p50Us":100,"p95Us":500,"p99Us":500,"maxUs":700}')
        self.assertEqual(4,value["samples"])
        with self.assertRaises(soak.SoakError): soak.parse_tick_snapshot('{"samples":4,"p50Us":9,"p95Us":8,"p99Us":10,"maxUs":10}')
        self.assertEqual(250,soak.parse_tick_snapshot('{"samples":1,"p50Us":250,"p95Us":250,"p99Us":250,"maxUs":157}')["p99Us"])
    def test_merged_dispatcher_histogram_percentile(self):
        ticks=[{"samples":2,"maxUs":80,"buckets":[1,1,0],"bucketUpperBoundsUs":[50,100]},
               {"samples":2,"maxUs":200,"buckets":[0,1,1],"bucketUpperBoundsUs":[50,100]}]
        self.assertEqual(100,soak.merged_histogram_percentile(ticks,.50)); self.assertEqual(200,soak.merged_histogram_percentile(ticks,.99))
    def test_checksum_generation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in soak.ARTIFACTS: root.joinpath(name).write_text(name)
            soak.write_checksums(root)
            self.assertEqual(len(soak.ARTIFACTS),len(root.joinpath("SHA256SUMS").read_text().splitlines()))
    def test_graceful_shutdown_targets_exact_child(self):
        child=mock.Mock(pid=321,returncode=0); child.poll.return_value=None; child.wait.return_value=0
        run=soak.Run(argparse.Namespace(),{},Path("."),server=child,identity="77")
        with mock.patch.object(soak,"proc_identity",return_value="77"),mock.patch.object(os,"kill") as kill:
            run.stop(); kill.assert_called_once_with(321,soak.signal.SIGTERM)
    def test_abnormal_controlled_shutdown_is_rejected(self):
        child=mock.Mock(pid=432,returncode=-6); child.poll.return_value=None; child.wait.return_value=-6
        run=soak.Run(argparse.Namespace(),{},Path("."),server=child,identity="78")
        with mock.patch.object(soak,"proc_identity",return_value="78"),mock.patch.object(os,"kill"):
            with self.assertRaisesRegex(soak.SoakError,"status -6"): run.stop()
    def test_forced_timeout_cleanup_targets_same_child(self):
        child=mock.Mock(pid=654,returncode=-9); child.poll.return_value=None; child.wait.side_effect=[subprocess.TimeoutExpired("server",30),0]
        run=soak.Run(argparse.Namespace(),{},Path("."),server=child,identity="88")
        with mock.patch.object(soak,"proc_identity",return_value="88"),mock.patch.object(os,"kill") as kill:
            with self.assertRaisesRegex(soak.SoakError,"did not complete"): run.stop()
            self.assertEqual([mock.call(654,soak.signal.SIGTERM),mock.call(654,soak.signal.SIGKILL)],kill.call_args_list)


if __name__ == "__main__": unittest.main()
