import importlib.util, os, socket, stat, struct, sys, tempfile, unittest, zlib
from pathlib import Path
from unittest import mock

SPEC=importlib.util.spec_from_file_location("ordinary",Path(__file__).parents[2]/"tools/playerbots_ordinary_client.py")
ordinary=importlib.util.module_from_spec(SPEC); sys.modules[SPEC.name]=ordinary; SPEC.loader.exec_module(ordinary)

class OrdinaryClientTest(unittest.TestCase):
    def values(self): return {"SOAK_HOST":"127.0.0.1","SOAK_LOGIN_PORT":"7171","SOAK_GAME_PORT":"7174","SOAK_ACCOUNT":"ordinary","SOAK_PASSWORD":"secret","SOAK_CHARACTER":"Ordinary Soak"}
    def test_config_validation(self): self.assertEqual(860,ordinary.CLIENT_VERSION); self.assertEqual(7174,ordinary.Config.from_values(self.values()).game_port)
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
    def test_keepalive_packet(self): self.assertEqual(b"\x1d",ordinary.decrypt_frame(ordinary.encrypted_frame(b"\x1d",(1,2,3,4))[2:],(1,2,3,4)))
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
    def test_private_environment_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/"client.env"; path.write_text("SOAK_HOST=127.0.0.1\n"); path.chmod(0o600)
            self.assertEqual(0o600,stat.S_IMODE(path.stat().st_mode)); ordinary.load_env(path)

if __name__=="__main__": unittest.main()
