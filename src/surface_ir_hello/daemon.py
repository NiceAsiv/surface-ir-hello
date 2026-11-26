"""
Pre-warmed Unix domain socket authentication daemon (/run/surface-ir-hello/auth.sock).
Keeps YuNet + SFace loaded in memory (~48 MB RSS) so PAM face unlock completes in ~250-450 ms.
Enforces kernel SO_PEERCRED UID authorization so KDE kscreenlocker (non-root) and sudo/SDDM (root) both work safely.
"""
import os
import pwd
import signal
import socket
import struct
import sys
import time
import tomllib

import numpy as np

from .hardware import IRSensorSession
from .recognizer import IRFaceEngine, load_user_embeddings

SOCK_DIR = "/run/surface-ir-hello"
SOCK_PATH = f"{SOCK_DIR}/auth.sock"
CONFIG_PATH = "/etc/surface-ir-hello/config.toml"


def load_config() -> dict:
    cfg = {
        "ir": {"emitter_current_ma": 390, "exposure_max": 380, "gain_max": 8.0, "target_p95": 600.0, "led_channel": 2},
        "auth": {"cosine_threshold": 0.40, "timeout_sec": 2.5, "require_active_ir_liveness": True},
    }
    if os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH, "rb") as f:
                user_cfg = tomllib.load(f)
            for sec in ("ir", "auth"):
                if sec in user_cfg:
                    cfg[sec].update(user_cfg[sec])
        except Exception as e:
            print(f"[WARN] Failed reading {CONFIG_PATH}: {e}", file=sys.stderr)
    return cfg


def peer_authorized(conn: socket.socket, target_user: str) -> bool:
    creds = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
    _pid, uid, _gid = struct.unpack("3i", creds)
    if uid == 0:
        return True
    try:
        caller_name = pwd.getpwuid(uid).pw_name
        return caller_name == target_user
    except KeyError:
        return False


def verify_user(engine: IRFaceEngine, user: str, cfg: dict) -> tuple[bool, str]:
    templates = load_user_embeddings(user)
    if not templates:
        return False, "NO_MODEL"

    ir_cfg = cfg["ir"]
    auth_cfg = cfg["auth"]
    thresh = float(auth_cfg["cosine_threshold"])
    deadline = time.monotonic() + float(auth_cfg["timeout_sec"])
    require_live = bool(auth_cfg["require_active_ir_liveness"])

    t0 = time.monotonic()
    best_score = -1.0
    with IRSensorSession(
        emitter_ma=float(ir_cfg["emitter_current_ma"]),
        exposure_max=int(ir_cfg["exposure_max"]),
        gain_max=float(ir_cfg["gain_max"]),
        target_p95=float(ir_cfg["target_p95"]),
        led_channel=int(ir_cfg["led_channel"]),
        enable_emitter=True,
    ) as cam:
        cam.apply_ae(strobe_active=False)
        amb = cam.read_raw_frame()
        amb_p95 = float(np.percentile(amb[::8, ::8], 95))

        cam.start_emitter()
        frame_idx = 0
        while time.monotonic() < deadline and frame_idx < 45:
            f10 = cam.read_raw_frame()
            frame_idx += 1
            lit_p95 = cam.update_ae(f10, strobe_active=True)
            if frame_idx < 2:
                continue

            if require_live and amb_p95 < 750.0 and lit_p95 < amb_p95 * 1.12 and lit_p95 < 60.0:
                continue

            det = engine.detect_and_embed(f10, check_rotations=(frame_idx % 3 == 0))
            if not det:
                continue
            feat = det["feature"]
            score = max(engine.match_cosine(feat, t) for t in templates)
            best_score = max(best_score, score)
            if score >= thresh:
                ms = int((time.monotonic() - t0) * 1000)
                return True, f"OK score={score:.3f} ms={ms} frames={frame_idx}"

    ms = int((time.monotonic() - t0) * 1000)
    return False, f"FAIL best={best_score:.3f} ms={ms}"


def main():
    os.makedirs(SOCK_DIR, mode=0o755, exist_ok=True)
    if os.path.exists(SOCK_PATH):
        os.unlink(SOCK_PATH)

    print("[surface-ir-hellod] Pre-loading YuNet + SFace ONNX models...")
    engine = IRFaceEngine()
    print("[surface-ir-hellod] Models ready.")

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCK_PATH)
    os.chmod(SOCK_PATH, 0o666)
    srv.listen(8)

    def _shutdown(*_):
        try:
            os.unlink(SOCK_PATH)
        except OSError:
            pass
        sys.exit(0)

    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT, _shutdown)
    print(f"[surface-ir-hellod] Listening on {SOCK_PATH}")

    while True:
        conn, _ = srv.accept()
        with conn:
            try:
                conn.settimeout(2.0)
                line = conn.recv(128).decode("utf-8", errors="replace").strip()
                if line == "PING":
                    conn.sendall(b"PONG\n")
                    continue
                parts = line.split()
                if len(parts) != 2 or parts[0] != "AUTH":
                    conn.sendall(b"ERR invalid_request\n")
                    continue
                target_user = parts[1]
                if not peer_authorized(conn, target_user):
                    conn.sendall(b"ERR unauthorized_uid\n")
                    continue
                cfg = load_config()
                ok, msg = verify_user(engine, target_user, cfg)
                print(f"[auth] user={target_user} ok={ok} {msg}")
                conn.sendall((msg + "\n").encode("utf-8"))
            except Exception as e:
                print(f"[auth-err] {e}", file=sys.stderr)
                try:
                    conn.sendall(b"ERR internal\n")
                except OSError:
                    pass


if __name__ == "__main__":
    main()
