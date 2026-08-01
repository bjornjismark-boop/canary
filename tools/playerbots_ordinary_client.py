#!/usr/bin/env python3
"""Minimal loopback-only Tibia 8.60 protocol fixture for PlayerBots live soak."""

from __future__ import annotations

import argparse
import os
import select
import signal
import socket
import struct
import sys
import time
import zlib
from dataclasses import dataclass
from pathlib import Path

MAX_PACKET = 65535
CLIENT_VERSION = 860
RSA_P = int("14299623962416399520070177382898895550795403345466153217470516082934737582776038882967213386204600674145392845853859217990626450972452084065728686565928113")
RSA_Q = int("7630979195970404721891201847792002125535401292779123937207447574596692788513647179235335529307251350570728407373705564708871762033017096809910315212884101")
RSA_N = RSA_P * RSA_Q
RSA_E = 65537
ASSET_SIGNATURES = (0x44363843, 0x53363843, 0x50363843)


class ClientError(RuntimeError):
    pass


def load_env(path: Path) -> dict[str, str]:
    if not path.is_file() or path.stat().st_mode & 0o077:
        raise ClientError("client environment must be an existing mode-0600 file")
    result: dict[str, str] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw or raw.startswith("#"): continue
        if "=" not in raw: raise ClientError(f"malformed client environment line {number}")
        key, value = raw.split("=", 1); result[key.strip()] = value.strip()
    return result


@dataclass(frozen=True)
class Config:
    host: str
    login_port: int
    game_port: int
    account: str
    password: str
    character: str
    timeout: float = 15.0
    heartbeat: float = 5.0
    ready_file: Path | None = None

    @staticmethod
    def from_values(v: dict[str, str]) -> "Config":
        required = ("SOAK_HOST", "SOAK_LOGIN_PORT", "SOAK_GAME_PORT", "SOAK_ACCOUNT", "SOAK_PASSWORD", "SOAK_CHARACTER")
        if any(not v.get(k) for k in required): raise ClientError("missing ordinary-client credentials or endpoint")
        if v["SOAK_HOST"] not in ("127.0.0.1", "::1", "localhost"): raise ClientError("ordinary client requires loopback")
        try:
            login, game = int(v["SOAK_LOGIN_PORT"]), int(v["SOAK_GAME_PORT"])
            timeout, heartbeat = float(v.get("SOAK_TIMEOUT_SECONDS", "15")), float(v.get("SOAK_HEARTBEAT_SECONDS", "5"))
        except ValueError as exc: raise ClientError("invalid client numeric configuration") from exc
        if not (1 <= login <= 65535 and 1 <= game <= 65535 and .1 <= timeout <= 60 and .1 <= heartbeat <= 60):
            raise ClientError("client endpoint or timeout outside bounds")
        ready = Path(v["SOAK_READY_FILE"]) if v.get("SOAK_READY_FILE") else None
        return Config(v["SOAK_HOST"], login, game, v["SOAK_ACCOUNT"], v["SOAK_PASSWORD"], v["SOAK_CHARACTER"], timeout, heartbeat, ready)


def string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) > 4096: raise ClientError("protocol string too large")
    return struct.pack("<H", len(encoded)) + encoded


def frame(body: bytes) -> bytes:
    if not body or len(body) > MAX_PACKET: raise ClientError("packet size outside bounds")
    return struct.pack("<H", len(body)) + body


def recv_exact(sock: socket.socket, count: int) -> bytes:
    if count < 0 or count > MAX_PACKET: raise ClientError("packet size outside bounds")
    data = bytearray()
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part: raise ClientError("unexpected disconnect")
        data.extend(part)
    return bytes(data)


def recv_frame(sock: socket.socket) -> bytes:
    size = struct.unpack("<H", recv_exact(sock, 2))[0]
    if not size or size > MAX_PACKET: raise ClientError("packet size outside bounds")
    return recv_exact(sock, size)


def rsa_encrypt(payload: bytes) -> bytes:
    if len(payload) > 127: raise ClientError("RSA payload exceeds 127 bytes")
    block = b"\0" + payload + bytes(127 - len(payload))
    value = int.from_bytes(block, "big")
    if value >= RSA_N: raise ClientError("RSA payload outside modulus")
    return pow(value, RSA_E, RSA_N).to_bytes(128, "big")


def xtea_transform(data: bytes, key: tuple[int, int, int, int], encrypt: bool) -> bytes:
    if len(data) % 8: raise ClientError("XTEA data is not block aligned")
    out = bytearray(data); delta = 0x9E3779B9
    for offset in range(0, len(out), 8):
        v0, v1 = struct.unpack_from("<II", out, offset)
        if encrypt:
            total = 0
            for _ in range(32):
                v0 = (v0 + ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ ((total + key[total & 3]) & 0xFFFFFFFF))) & 0xFFFFFFFF
                total = (total + delta) & 0xFFFFFFFF
                v1 = (v1 + ((((v0 << 4) ^ (v0 >> 5)) + v0) ^ ((total + key[(total >> 11) & 3]) & 0xFFFFFFFF))) & 0xFFFFFFFF
        else:
            total = (delta * 32) & 0xFFFFFFFF
            for _ in range(32):
                v1 = (v1 - ((((v0 << 4) ^ (v0 >> 5)) + v0) ^ ((total + key[(total >> 11) & 3]) & 0xFFFFFFFF))) & 0xFFFFFFFF
                total = (total - delta) & 0xFFFFFFFF
                v0 = (v0 - ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ ((total + key[total & 3]) & 0xFFFFFFFF))) & 0xFFFFFFFF
        struct.pack_into("<II", out, offset, v0, v1)
    return bytes(out)


def encrypted_frame(payload: bytes, key: tuple[int, int, int, int]) -> bytes:
    clear = struct.pack("<H", len(payload)) + payload
    clear += b"\x33" * ((-len(clear)) % 8)
    encrypted = xtea_transform(clear, key, True)
    body = struct.pack("<I", zlib.adler32(encrypted) & 0xFFFFFFFF) + encrypted
    return frame(body)


def decrypt_frame(body: bytes, key: tuple[int, int, int, int]) -> bytes:
    if len(body) < 12: raise ClientError("encrypted packet is too short")
    checksum, encrypted = struct.unpack_from("<I", body)[0], body[4:]
    if checksum != zlib.adler32(encrypted) & 0xFFFFFFFF: raise ClientError("packet checksum mismatch")
    clear = xtea_transform(encrypted, key, False)
    size = struct.unpack_from("<H", clear)[0]
    if size > len(clear) - 2: raise ClientError("malformed encrypted packet")
    return clear[2:2 + size]


def parse_challenge(body: bytes) -> tuple[int, int]:
    if len(body) != 12: raise ClientError("malformed challenge packet")
    checksum, inner = struct.unpack_from("<IH", body)
    payload = body[6:]
    if checksum != zlib.adler32(body[4:]) & 0xFFFFFFFF or inner != 6 or payload[0] != 0x1F:
        raise ClientError("invalid challenge packet")
    return struct.unpack_from("<I", payload, 1)[0], payload[5]


def parse_character_list(payload: bytes, expected: str) -> None:
    position = 0
    while position < len(payload):
        opcode = payload[position]; position += 1
        if opcode == 0x14:
            size = struct.unpack_from("<H", payload, position)[0]; position += 2 + size
        elif opcode == 0x0B:
            size = struct.unpack_from("<H", payload, position)[0]
            raise ClientError("login failed: " + payload[position + 2:position + 2 + size].decode(errors="replace"))
        elif opcode == 0x64:
            count = payload[position]; position += 1
            names = []
            for _ in range(count):
                size = struct.unpack_from("<H", payload, position)[0]; position += 2
                names.append(payload[position:position + size].decode()); position += size
                for _ in range(1):
                    size = struct.unpack_from("<H", payload, position)[0]; position += 2 + size
                position += 6
            if expected not in names: raise ClientError("ordinary character missing from login response")
            return
        else: raise ClientError(f"unexpected login opcode {opcode:#x}")
    raise ClientError("character list missing")


def login_account(config: Config, key: tuple[int, int, int, int]) -> None:
    rsa = struct.pack("<IIII", *key) + string(config.account) + string(config.password)
    payload = b"\x01" + struct.pack("<HHIII", 2, CLIENT_VERSION, *ASSET_SIGNATURES) + rsa_encrypt(rsa)
    body = struct.pack("<I", zlib.adler32(payload) & 0xFFFFFFFF) + payload
    with socket.create_connection((config.host, config.login_port), config.timeout) as sock:
        sock.settimeout(config.timeout); sock.sendall(frame(body))
        parse_character_list(decrypt_frame(recv_frame(sock), key), config.character)


class Client:
    def __init__(self, config: Config):
        self.config = config; self.key = (0x11223344, 0x55667788, 0x10203040, 0x50607080)
        self.sock: socket.socket | None = None; self.stopping = False; self.placed = False

    def connect(self) -> None:
        login_account(self.config, self.key)
        self.sock = socket.create_connection((self.config.host, self.config.game_port), self.config.timeout)
        self.sock.settimeout(self.config.timeout)
        timestamp, random = parse_challenge(recv_frame(self.sock))
        rsa = struct.pack("<IIII", *self.key) + b"\0" + string(self.config.account) + string(self.config.character) + string(self.config.password) + struct.pack("<IB", timestamp, random)
        first = b"\0" + struct.pack("<HH", 2, CLIENT_VERSION) + rsa_encrypt(rsa)
        body = struct.pack("<I", zlib.adler32(first) & 0xFFFFFFFF) + first
        self.sock.sendall(frame(body))
        deadline = time.monotonic() + self.config.timeout
        while time.monotonic() < deadline:
            payload = decrypt_frame(recv_frame(self.sock), self.key)
            if payload and payload[0] == 0x14:
                size = struct.unpack_from("<H", payload, 1)[0]
                raise ClientError("game login failed: " + payload[3:3 + size].decode(errors="replace"))
            if payload and payload[0] == 0x0A:
                self.placed = True; print("ORDINARY_CLIENT_PLACED=YES", flush=True); return
        raise ClientError("game placement timeout")

    def logout(self) -> None:
        if self.sock:
            try: self.sock.sendall(encrypted_frame(b"\x14", self.key))
            except OSError: pass

    def run(self) -> int:
        self.connect(); assert self.sock
        if self.config.ready_file:
            self.config.ready_file.write_text("ORDINARY_CLIENT_PLACED=YES\n", encoding="ascii")
        next_heartbeat = time.monotonic()
        while not self.stopping:
            timeout = max(0.0, min(1.0, next_heartbeat - time.monotonic()))
            readable, _, _ = select.select([self.sock], [], [], timeout)
            if readable:
                # Reading every server frame is sufficient for this fixture.
                # Login/map payloads are opcode streams, so never scan arbitrary
                # payload bytes as if each occurrence were a standalone ping.
                decrypt_frame(recv_frame(self.sock), self.key)
            if time.monotonic() >= next_heartbeat:
                self.sock.sendall(encrypted_frame(b"\x1d", self.key)); next_heartbeat = time.monotonic() + self.config.heartbeat
        self.logout(); return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("--env-file", type=Path, required=True)
    args = parser.parse_args(argv); client = Client(Config.from_values(load_env(args.env_file)))
    def stop(_signal, _frame): client.stopping = True
    signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
    return client.run()


if __name__ == "__main__":
    try: raise SystemExit(main())
    except (ClientError, OSError, TimeoutError) as exc:
        print(f"ordinary-client: {exc}", file=sys.stderr); raise SystemExit(1)
