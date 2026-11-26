# surface-ir-hello

**Windows Hello-style Infrared (IR) Face Authentication for Microsoft Surface Go (Gen 1 / 2 / 3) on Linux.**

## Why `surface-ir-hello`?

Existing Linux face unlock setups (`howdy` + `v4l2loopback`) fail on Surface Go devices for four separate reasons:

1. **No AVX on Pentium Gold 4415Y / 4425Y (`SIGILL`)**: Prebuilt `dlib` packages require AVX instructions, causing `howdy` to crash with `Illegal instruction (core dumped)` on Surface Go 1/2 CPUs, while `pam-python` / `pkg_resources` broke on Python 3.14+.
2. **Unmapped IPU3 Monochrome Pipeline & IR Flood Emitter**: `libcamera` skips the monochrome `OV7251` IR sensor, and mainline Linux lacks a kernel flash driver for the `TPS68470` PMIC `WLED2` infrared emitter.
3. **Zero `v4l2loopback-dkms` Needed (Secure Boot Friendly)**: Reads 10-bit `V4L2_PIX_FMT_IPU3_Y10` (`ip3y`) directly from `/dev/video*` via V4L2 `mmap` with vectorized 10-bit percentile normalization + CLAHE.
4. **KDE Wayland Lockscreen + Instant `< 350 ms` Unlock**: Unlike `howdy` (which fails inside non-root `kscreenlocker_greet` or takes ~2s cold-starting Python/models), `surface-ir-hellod` pre-warms **OpenCV YuNet + SFace** (`~48 MB` RAM) behind a `SO_PEERCRED`-verified Unix socket paired with a tiny native C PAM module (`pam_surface_ir.so`).

## Architecture

```text
[sudo / SDDM / KDE kscreenlocker]
        │  (C PAM module: pam_surface_ir.so, skips SSH & closed lid)
        ▼  Unix Socket (/run/surface-ir-hello/auth.sock, SO_PEERCRED checked)
[surface-ir-hellod.service]
        ├─► Media Controller: links ov7251 -> ipu3-csi2 -> /dev/videoX (ip3y 10-bit)
        ├─► I2C Strobe Sync : TPS68470 WLED2 (852ms HW watchdog) + OV7251 per-frame strobe
        ├─► Active IR Check : Ambient vs 850nm strobe reflectance liveness check
        └─► OpenCV DNN (SSE4.2): YuNet (detection + 5 landmarks) -> SFace (128-D cosine match)
```

## Quick Start (Arch Linux / Fedora / Ubuntu with `linux-surface`)

```bash
# 1. Build & install
make
sudo make install

# 2. Download OpenCV Zoo ONNX models & test IR camera + emitter
sudo surface-ir hwcheck --save-debug /tmp/ir-check.png

# 3. Start background authentication daemon
sudo systemctl enable --now surface-ir-hellod.service

# 4. Enroll your face (5 angles, ~3 seconds)
sudo surface-ir enroll $USER

# 5. Test live verification speed & score
surface-ir verify

# 6. Enable in PAM (sudo, KDE lockscreen, SDDM)
sudo surface-ir pam-enable
```

## Safety & Fallback
- Automatically bypassed over **SSH** (`PAM_RHOST` / `SSH_CONNECTION`) and when the **Type Cover lid is closed**.
- Hardware-protected IR emitter (`<= 8.1 ms` strobe pulse per `33.3 ms` frame + `TPS68470` `852 ms` hardware timeout + signal-masked register restoration).
- Instant fallback to standard password prompt if no face is enrolled or camera times out (`sudo surface-ir pam-disable` cleanly restores PAM at any time).

## Credits & References
- Hardware register insights for `OV7251` strobe & `TPS68470` WLED inspired by [`linux-surface`](https://github.com/linux-surface/linux-surface) community research (Issue #739) and [`pkarnakov/surface-go-ipu3-camera`](https://github.com/pkarnakov/surface-go-ipu3-camera).
- Face detection & recognition powered by [OpenCV Zoo YuNet & SFace](https://github.com/opencv/opencv_zoo) (Apache-2.0).
