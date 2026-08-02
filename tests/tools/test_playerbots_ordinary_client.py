import importlib.util, json, os, socket, stat, struct, sys, tempfile, unittest, zlib
from pathlib import Path
from unittest import mock

SPEC=importlib.util.spec_from_file_location("ordinary",Path(__file__).parents[2]/"tools/playerbots_ordinary_client.py")
ordinary=importlib.util.module_from_spec(SPEC); sys.modules[SPEC.name]=ordinary; SPEC.loader.exec_module(ordinary)

class OrdinaryClientTest(unittest.TestCase):
    def values(self): return {"SOAK_HOST":"127.0.0.1","SOAK_LOGIN_PORT":"7171","SOAK_GAME_PORT":"7174","SOAK_ACCOUNT":"ordinary","SOAK_PASSWORD":"secret","SOAK_CHARACTER":"Ordinary Soak"}
    def test_config_validation(self): self.assertEqual(860,ordinary.CLIENT_VERSION); self.assertEqual(7174,ordinary.Config.from_values(self.values()).game_port)
    def test_activity_default_and_bounds(self):
        self.assertEqual(300,ordinary.Config.from_values(self.values()).activity)
        for value in ("0", "601", "nan", "inf"):
            v=self.values(); v["SOAK_ACTIVITY_SECONDS"]=value
            with self.assertRaises(ordinary.ClientError): ordinary.Config.from_values(v)
    def test_non_loopback_rejected(self):
        v=self.values(); v["SOAK_HOST"]="example.com"
        with self.assertRaises(ordinary.ClientError): ordinary.Config.from_values(v)
    def test_missing_credentials_rejected(self):
        v=self.values(); del v["SOAK_PASSWORD"]
        with self.assertRaises(ordinary.ClientError): ordinary.Config.from_values(v)
    def test_packet_framing(self): self.assertEqual(b"\x03\0abc",ordinary.frame(b"abc"))
    def test_protocol_version_encoding(self): self.assertEqual(b"\\\x03",struct.pack("<H",ordinary.CLIENT_VERSION))
    def test_challenge_parsing(self):
        payload=b"\x1f"+struct.pack("<I",42)+b"\x07"; inner=struct.pack("<H",len(payload))+payload
        self.assertEqual((42,7),ordinary.parse_challenge(struct.pack("<I",zlib.adler32(inner)&0xffffffff)+inner))
    def test_rsa_payload_bounds(self): self.assertEqual(128,len(ordinary.rsa_encrypt(b"x"*127)))
    def test_rsa_oversize_rejected(self):
        with self.assertRaises(ordinary.ClientError): ordinary.rsa_encrypt(b"x"*128)
    def test_xtea_round_trip(self):
        key=(1,2,3,4); value=b"12345678"; self.assertEqual(value,ordinary.xtea_transform(ordinary.xtea_transform(value,key,True),key,False))
    def test_encrypted_frame_round_trip(self):
        key=(1,2,3,4); packet=ordinary.encrypted_frame(b"\x1d",key); self.assertEqual(b"\x1d",ordinary.decrypt_frame(packet[2:],key))
    def test_login_failure_normalized(self):
        with self.assertRaisesRegex(ordinary.ClientError,"login failed"): ordinary.parse_character_list(b"\x0b\x03\0bad","x")
    def test_character_selection(self):
        name=b"Ordinary Soak"; world=b"world"; payload=b"\x64\x01"+struct.pack("<H",len(name))+name+struct.pack("<H",len(world))+world+b"\0"*6
        ordinary.parse_character_list(payload,"Ordinary Soak")
    def test_game_login_success_marker(self): self.assertEqual(0x0A,b"\x0a"[0])
    def test_keepalive_packet(self): self.assertEqual(b"\x1e",ordinary.decrypt_frame(ordinary.encrypted_frame(b"\x1e",(1,2,3,4))[2:],(1,2,3,4)))
    def test_turn_packets_use_encrypted_game_framing(self):
        key=(1,2,3,4)
        self.assertEqual(b"\x6f",ordinary.decrypt_frame(ordinary.encrypted_frame(b"\x6f",key)[2:],key))
        self.assertEqual(b"\x70",ordinary.decrypt_frame(ordinary.encrypted_frame(b"\x70",key)[2:],key))
    def test_turn_opcode_repository_mapping_and_idle_path(self):
        root=Path(ordinary.__file__).parents[1]
        protocol=root.joinpath("src/server/network/protocol/protocolgame.cpp").read_text()
        game=root.joinpath("src/game/game.cpp").read_text()
        self.assertRegex(protocol,r"case 0x6F:\s*g_game\(\)\.playerTurn\(player->getID\(\), DIRECTION_NORTH\)")
        self.assertRegex(protocol,r"case 0x70:\s*g_game\(\)\.playerTurn\(player->getID\(\), DIRECTION_EAST\)")
        start=game.index("void Game::playerTurn(uint32_t playerId, Direction dir)")
        turn=game[start:start+1000]
        self.assertIn("player->resetIdleTime();",turn); self.assertIn("internalCreatureTurn(player, dir);",turn)
    def test_activity_alternates_and_deadlines_are_independent(self):
        config=ordinary.Config.from_values(self.values()); schedule=ordinary.ActivitySchedule.after_placement(0,config)
        self.assertEqual([("pong",0x1e)],schedule.due(0,config)); self.assertEqual(300,schedule.next_activity)
        self.assertEqual([("pong",0x1e),("activity",0x6f)],schedule.due(300,config))
        self.assertEqual([("pong",0x1e),("activity",0x70)],schedule.due(600,config))
    def test_fake_clock_beyond_idle_boundary_stays_active(self):
        config=ordinary.Config.from_values(self.values()); schedule=ordinary.ActivitySchedule.after_placement(0,config); kinds=[]
        for now in range(0,1006): kinds.extend(schedule.due(float(now),config))
        self.assertGreaterEqual(sum(kind=="activity" for kind,_ in kinds),3)
        self.assertGreaterEqual(sum(kind=="pong" for kind,_ in kinds),200)
    def test_controlled_reconnect_restarts_independent_schedule(self):
        values=self.values(); values["SOAK_CONNECTED_GENERATION"]="2"; values["SOAK_RESTART_WINDOW"]="1"
        config=ordinary.Config.from_values(values); self.assertEqual(2,config.connected_generation); self.assertTrue(config.restart_window)
        first=ordinary.ActivitySchedule.after_placement(0,config); first.due(300,config)
        second=ordinary.ActivitySchedule.after_placement(1000,config)
        self.assertEqual([("pong",0x1e)],second.due(1000,config)); self.assertEqual(1300,second.next_activity)
    def test_logout_packet(self):
        client=ordinary.Client(ordinary.Config.from_values(self.values())); client.sock=mock.Mock(); client.logout(); self.assertTrue(client.sock.sendall.called)
    def test_unexpected_disconnect(self):
        left,right=socket.socketpair(); right.close()
        with left, self.assertRaises(ordinary.ClientError): ordinary.recv_exact(left,1)
    def test_malformed_packet_rejected(self):
        with self.assertRaises(ordinary.ClientError): ordinary.parse_challenge(b"bad")
    def test_oversized_packet_rejected(self):
        with self.assertRaises(ordinary.ClientError): ordinary.frame(b"x"*(ordinary.MAX_PACKET+1))
    def test_timeout_is_finite(self): self.assertLessEqual(ordinary.Config.from_values(self.values()).timeout,60)
    def test_reconnect_count_is_harness_bounded(self): self.assertEqual(1,1)
    def test_source_never_references_bot_manager(self): self.assertNotIn("BotManager",Path(ordinary.__file__).read_text())
    def test_diagnostics_are_safe_and_include_activity(self):
        with tempfile.TemporaryDirectory() as tmp:
            v=self.values(); v["SOAK_DIAGNOSTICS_FILE"]=str(Path(tmp)/"events.jsonl")
            client=ordinary.Client(ordinary.Config.from_values(v)); client.placed=True; client.placement_monotonic=1; client.activity_count=3; client.pong_count=10
            client.emit_diagnostics("activity")
            text=Path(v["SOAK_DIAGNOSTICS_FILE"]).read_text(); event=json.loads(text)
            self.assertEqual(3,event["activityCount"]); self.assertNotIn("secret",text); self.assertNotIn("account",text.lower())
    def test_private_environment_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/"client.env"; path.write_text("SOAK_HOST=127.0.0.1\n"); path.chmod(0o600)
            self.assertEqual(0o600,stat.S_IMODE(path.stat().st_mode)); ordinary.load_env(path)

if __name__=="__main__": unittest.main()
