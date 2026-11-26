"""
Command-line management utility `surface-ir` for hardware diagnostics, face enrollment,
verification, image capture, and safe PAM configuration.
"""
import argparse
import os
import shutil
import socket
import sys
import time

import cv2
import numpy as np

from .daemon import SOCK_PATH, load_config
from .hardware import IRSensorSession, discover_topology
from .recognizer import (
    FACES_DIR,
    IRFaceEngine,
    load_user_embeddings,
    normalize_ir10_to_bgr8,
    save_user_embeddings,
)

PAM_LINE = "auth       sufficient                  pam_surface_ir.so quiet"
PAM_TARGETS = [
    ("/etc/pam.d/sudo", "/usr/lib/pam.d/sudo"),
    ("/etc/pam.d/kde", "/usr/lib/pam.d/kde"),
    ("/etc/pam.d/sddm", "/usr/lib/pam.d/sddm"),
]


def require_root():
    if os.geteuid() != 0:
        sys.exit("Error: this command requires root privileges (run with sudo).")


def cmd_hwcheck(args):
    require_root()
    topo = discover_topology()
    print("=== Surface IR Hardware Topology ===")
    for k, v in topo.items():
        print(f"  {k:15s}: {v}")

    cfg = load_config()["ir"]
    with IRSensorSession(
        emitter_ma=float(cfg["emitter_current_ma"]),
        exposure_max=int(cfg["exposure_max"]),
        led_channel=int(cfg["led_channel"]),
        enable_emitter=True,
    ) as cam:
        cam.apply_ae(strobe_active=False)
        for _ in range(3):
            amb = cam.read_raw_frame()
        print(f"  Ambient frame  : mean={amb.mean():.1f}, p95={np.percentile(amb, 95):.1f}, max={amb.max()}")

        chosen_led = int(cfg["led_channel"])
        cam.start_emitter(led_num=chosen_led)
        for _ in range(8):
            lit = cam.read_raw_frame()
            cam.update_ae(lit, strobe_active=True)
        gstat = cam.read_gstat()
        print(f"  Emitter LED{chosen_led}   : mean={lit.mean():.1f}, p95={np.percentile(lit, 95):.1f}, max={lit.max()}, GSTAT=0x{gstat:02x}")
        if gstat & 0x08:
            alt = 1 if chosen_led == 2 else 2
            print(f"  [!] GSTAT fault 0x{gstat:02x} on LED{chosen_led}, testing LED{alt}...")
            cam.start_emitter(led_num=alt)
            for _ in range(8):
                lit = cam.read_raw_frame()
                cam.update_ae(lit, strobe_active=True)
            gstat2 = cam.read_gstat()
            print(f"  Emitter LED{alt}   : mean={lit.mean():.1f}, p95={np.percentile(lit, 95):.1f}, max={lit.max()}, GSTAT=0x{gstat2:02x}")

    engine = IRFaceEngine()
    det = engine.detect_and_embed(lit, check_rotations=True)
    if det:
        print(f"  Face detected  : YES (rotation={det['angle']} deg, score={det['det_score']:.3f}, bbox={det['bbox']})")
    else:
        print("  Face detected  : None in frame (camera & IR illuminator working normally)")

    if args.save_debug:
        bgr = normalize_ir10_to_bgr8(lit)
        cv2.imwrite(args.save_debug, bgr)
        print(f"  Saved IR image : {args.save_debug}")


def cmd_enroll(args):
    require_root()
    user = args.user or os.environ.get("SUDO_USER") or "root"
    n_target = args.samples
    cfg = load_config()["ir"]
    engine = IRFaceEngine()
    existing = load_user_embeddings(user)
    samples = [{"embedding": e.tolist(), "ts": int(time.time())} for e in existing] if args.append else []

    print(f"=== Enrolling IR Face for user '{user}' ({n_target} samples) ===")
    print("Please look directly at the top bezel IR camera (slightly vary your angle as samples capture)...")
    captured_embs = []

    with IRSensorSession(
        emitter_ma=float(cfg["emitter_current_ma"]),
        exposure_max=int(cfg["exposure_max"]),
        gain_max=float(cfg["gain_max"]),
        target_p95=float(cfg["target_p95"]),
        led_channel=int(cfg["led_channel"]),
        enable_emitter=True,
    ) as cam:
        cam.start_emitter()
        for _ in range(5):
            cam.update_ae(cam.read_raw_frame(), strobe_active=True)

        deadline = time.monotonic() + 25.0
        while len(captured_embs) < n_target and time.monotonic() < deadline:
            f10 = cam.read_raw_frame()
            cam.update_ae(f10, strobe_active=True)
            det = engine.detect_and_embed(f10, check_rotations=True)
            if not det or det["det_score"] < 0.72:
                continue
            feat = det["feature"]
            if captured_embs and max(engine.match_cosine(feat, prev) for prev in captured_embs) > 0.96:
                time.sleep(0.25)
                continue
            captured_embs.append(feat)
            samples.append(
                {
                    "embedding": [round(float(x), 6) for x in feat],
                    "det_score": round(det["det_score"], 4),
                    "angle": det["angle"],
                    "ts": int(time.time()),
                }
            )
            print(f"  [{len(captured_embs)}/{n_target}] Captured clear IR face (score={det['det_score']:.3f}, rot={det['angle']} deg)")
            time.sleep(0.35)

    if not captured_embs:
        sys.exit("Enrollment timed out: no clear face detected in front of the IR camera.")

    save_user_embeddings(user, samples)
    print(f"Successfully saved {len(samples)} IR face template(s) for '{user}' in {FACES_DIR}/{user}.json!")


def cmd_verify(args):
    user = args.user or os.environ.get("SUDO_USER") or os.environ.get("USER")
    if os.path.exists(SOCK_PATH):
        t0 = time.monotonic()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(6.0)
            s.connect(SOCK_PATH)
            s.sendall(f"AUTH {user}\n".encode())
            reply = s.recv(128).decode().strip()
        dt = int((time.monotonic() - t0) * 1000)
        print(f"Daemon response ({dt} ms): {reply}")
        sys.exit(0 if reply.startswith("OK") else 1)
    else:
        require_root()
        from .daemon import verify_user

        engine = IRFaceEngine()
        ok, msg = verify_user(engine, user, load_config())
        print(msg)
        sys.exit(0 if ok else 1)


def cmd_list(_args):
    require_root()
    if not os.path.exists(FACES_DIR):
        print("No enrolled users yet.")
        return
    files = sorted(f for f in os.listdir(FACES_DIR) if f.endswith(".json"))
    if not files:
        print("No enrolled users yet.")
        return
    for fn in files:
        u = fn[:-5]
        embs = load_user_embeddings(u)
        print(f"  {u}: {len(embs)} IR template(s)")


def cmd_clear(args):
    require_root()
    user = args.user or os.environ.get("SUDO_USER")
    if not user:
        sys.exit("Specify a username to clear.")
    p = os.path.join(FACES_DIR, f"{user}.json")
    if os.path.exists(p):
        os.unlink(p)
        print(f"Removed enrolled face templates for '{user}'.")
    else:
        print(f"No face templates found for '{user}'.")


def cmd_pam_enable(_args):
    require_root()
    for etc_p, usr_p in PAM_TARGETS:
        if not os.path.exists(etc_p) and os.path.exists(usr_p):
            shutil.copy2(usr_p, etc_p)
        if not os.path.exists(etc_p):
            continue
        txt = open(etc_p, "r", encoding="utf-8").read()
        if "pam_surface_ir.so" in txt:
            print(f"  Already enabled in {etc_p}")
            continue
        shutil.copy2(etc_p, f"{etc_p}.surface-ir.bak")
        lines = txt.splitlines()
        out = []
        inserted = False
        for ln in lines:
            out.append(ln)
            if not inserted and ln.startswith("#%PAM"):
                out.append(PAM_LINE)
                inserted = True
        if not inserted:
            out.insert(0, PAM_LINE)
        with open(etc_p, "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")
        print(f"  Enabled pam_surface_ir.so in {etc_p} (backup: {etc_p}.surface-ir.bak)")


def cmd_pam_disable(_args):
    require_root()
    for etc_p, _ in PAM_TARGETS:
        if not os.path.exists(etc_p):
            continue
        txt = open(etc_p, "r", encoding="utf-8").read()
        if "pam_surface_ir.so" not in txt:
            continue
        lines = [ln for ln in txt.splitlines() if "pam_surface_ir.so" not in ln]
        with open(etc_p, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        print(f"  Disabled pam_surface_ir.so in {etc_p}")


def main():
    ap = argparse.ArgumentParser(prog="surface-ir", description="Surface IPU3 Infrared Face Unlock CLI")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_hw = sub.add_parser("hwcheck", help="Check IPU3/OV7251/TPS68470 hardware and test IR emitter")
    p_hw.add_argument("--save-debug", metavar="FILE", help="Save captured 8-bit enhanced IR frame to PNG/JPG")
    p_hw.set_defaults(func=cmd_hwcheck)

    p_en = sub.add_parser("enroll", help="Enroll IR face templates for a user")
    p_en.add_argument("user", nargs="?", help="Target username (defaults to SUDO_USER)")
    p_en.add_argument("--samples", type=int, default=5, help="Number of IR face samples to capture (default: 5)")
    p_en.add_argument("--append", action="store_true", help="Append to existing templates instead of replacing")
    p_en.set_defaults(func=cmd_enroll)

    p_ve = sub.add_parser("verify", help="Test live IR face verification")
    p_ve.add_argument("user", nargs="?", help="Target username")
    p_ve.set_defaults(func=cmd_verify)

    p_ls = sub.add_parser("list", help="List enrolled users")
    p_ls.set_defaults(func=cmd_list)

    p_cl = sub.add_parser("clear", help="Remove enrolled face templates for a user")
    p_cl.add_argument("user", nargs="?", help="Target username")
    p_cl.set_defaults(func=cmd_clear)

    p_pe = sub.add_parser("pam-enable", help="Enable Surface IR face unlock in sudo, KDE lockscreen, and SDDM")
    p_pe.set_defaults(func=cmd_pam_enable)

    p_pd = sub.add_parser("pam-disable", help="Disable Surface IR face unlock in PAM")
    p_pd.set_defaults(func=cmd_pam_disable)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
