"""
Direct Intel IPU3 CIO2 + OmniVision OV7251 + TI TPS68470 IR camera & strobe controller.
Supports Surface Go 1, Surface Go 2, Surface Go 3 without v4l2loopback-dkms.
"""
import ctypes
import fcntl
import glob
import os
import re
import select
import signal
import struct
import subprocess

import numpy as np

W, H, BPL = 640, 480, 832  # V4L2_PIX_FMT_IPU3_Y10 ('ip3y'): 25 pixels per 32 bytes
FRAME_BYTES = BPL * H

KNOWN_BOARDS = {
    "Surface Go": {"pmic_addr": 0x4D, "pmic_revid": 0x21, "led": 2},
    "Surface Go 2": {"pmic_addr": 0x4D, "pmic_revid": 0x21, "led": 2},
    "Surface Go 3": {"pmic_addr": 0x4D, "pmic_revid": 0x21, "led": 2},
}

# TI TPS68470 PMIC registers (SLVSCJ1B)
GSTAT, WLEDMAXF, WLEDTO, WLEDC1, WLEDC2, WLEDCTL, REVID = 0x01, 0x2F, 0x30, 0x34, 0x35, 0x36, 0xFF
PMIC_SAVED_REGS = (0x28, 0x2D, WLEDMAXF, WLEDTO, WLEDC1, WLEDC2, WLEDCTL)
WLED_EN, WLED_DISLED1, WLED_DISLED2 = 0x04, 0x08, 0x10
WLEDTO_852MS = 0x07
MA_PER_CODE = 32.5
MAX_CODE = 16  # 520 mA hardware cap

# OmniVision OV7251 16-bit strobe registers
PAD_OEN, STROBE_MASK, STROBE_SPAN = 0x3005, 0x3B81, 0x3B8C
PAD_STROBE = 0x08
SPAN_MARGIN = 20
EXPOSURE_CAP = 400  # <= 420 lines (~8.1 ms pulse out of 33.3 ms frame)

V4L2_CID_EXPOSURE = 0x00980911
V4L2_CID_GAIN = 0x00980913
V4L2_CID_ANALOGUE_GAIN = 0x009E0903
VIDIOC_G_CTRL, VIDIOC_S_CTRL, VIDIOC_QUERYCTRL = 0xC008561B, 0xC008561C, 0xC0445624

_I_IDX = np.arange(25) * 10
_B_IDX, _S_IDX = _I_IDX // 8, (_I_IDX % 8).astype(np.uint16)

CAM_LOCK_PATH = "/run/surface-ir-hello.cam.lock"


def unpack_ip3y(raw) -> np.ndarray:
    """Unpack IPU3_Y10 (32 bytes -> 25 10-bit pixels) into (480, 640) uint16 [0..1023]."""
    blk = np.frombuffer(raw, np.uint8).reshape(H, BPL // 32, 32)
    v = (blk[:, :, _B_IDX].astype(np.uint16) | (blk[:, :, _B_IDX + 1].astype(np.uint16) << 8)) >> _S_IDX & 1023
    return v.reshape(H, -1)[:, :W]


class I2CDevice:
    I2C_RDWR, I2C_M_RD = 0x0707, 0x0001

    class Msg(ctypes.Structure):
        _fields_ = [
            ("addr", ctypes.c_uint16),
            ("flags", ctypes.c_uint16),
            ("len", ctypes.c_uint16),
            ("buf", ctypes.POINTER(ctypes.c_uint8)),
        ]

    class Data(ctypes.Structure):
        _fields_ = [("msgs", ctypes.c_void_p), ("nmsgs", ctypes.c_uint32)]

    def __init__(self, bus: int, addr: int, reg16: bool):
        self.fd = os.open(f"/dev/i2c-{bus}", os.O_RDWR)
        self.addr = addr
        self.reg16 = reg16

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def _reg_bytes(self, reg: int) -> list[int]:
        return [reg >> 8, reg & 0xFF] if self.reg16 else [reg]

    def _xfer(self, *msgs):
        bufs = [(ctypes.c_uint8 * len(b))(*b) for _, b in msgs]
        arr = (self.Msg * len(msgs))(
            *[
                self.Msg(self.addr, flags, len(b), ctypes.cast(buf, ctypes.POINTER(ctypes.c_uint8)))
                for (flags, b), buf in zip(msgs, bufs)
            ]
        )
        fcntl.ioctl(self.fd, self.I2C_RDWR, self.Data(ctypes.addressof(arr), len(msgs)))
        return bufs

    def read(self, reg: int, n: int = 1) -> list[int]:
        return list(self._xfer((0, self._reg_bytes(reg)), (self.I2C_M_RD, [0] * n))[1])

    def write(self, reg: int, *vals: int):
        for i, v in enumerate(vals):
            self._xfer((0, self._reg_bytes(reg + i) + [v]))


class V4L2Subdev:
    def __init__(self, path: str):
        self.fd = os.open(path, os.O_RDWR)
        self.gain_cid = next((c for c in (V4L2_CID_GAIN, V4L2_CID_ANALOGUE_GAIN) if self.has_ctrl(c)), None)
        if self.gain_cid is None:
            os.close(self.fd)
            raise RuntimeError(f"{path}: no V4L2 gain control found")

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def has_ctrl(self, cid: int) -> bool:
        try:
            self.query_range(cid)
            return True
        except OSError:
            return False

    def query_range(self, cid: int) -> tuple[int, int]:
        b = bytearray(struct.pack("II32siiiiI8x", cid, 0, b"", 0, 0, 0, 0, 0))
        fcntl.ioctl(self.fd, VIDIOC_QUERYCTRL, b)
        return struct.unpack_from("ii", b, 40)

    def get(self, cid: int) -> int:
        b = bytearray(struct.pack("Ii", cid, 0))
        fcntl.ioctl(self.fd, VIDIOC_G_CTRL, b)
        return struct.unpack("Ii", b)[1]

    def set(self, cid: int, val: int):
        fcntl.ioctl(self.fd, VIDIOC_S_CTRL, bytearray(struct.pack("Ii", cid, int(val))))


def _v4l_by_name(pattern: str, expected: str):
    for f in sorted(glob.glob(f"/sys/class/video4linux/{pattern}/name")):
        try:
            if open(f).read().strip() == expected:
                return "/dev/" + f.split("/")[-2]
        except OSError:
            pass
    return None


def discover_topology() -> dict:
    product = (
        open("/sys/class/dmi/id/product_name").read().strip()
        if os.path.exists("/sys/class/dmi/id/product_name")
        else "Unknown"
    )
    board = KNOWN_BOARDS.get(product, {"pmic_addr": 0x4D, "pmic_revid": 0x21, "led": 2})

    found_media = None
    graph = ""
    m = None
    for md in sorted(glob.glob("/dev/media[0-9]*")):
        r = subprocess.run(["media-ctl", "-d", md, "-p"], capture_output=True, text=True)
        if r.returncode != 0:
            continue
        m = re.search(r"^- entity \d+: (ov7251 (\d+)-([0-9a-f]{4})) ", r.stdout, re.M)
        if m:
            found_media = md
            graph = r.stdout
            break
    if not found_media or not m:
        raise RuntimeError("No ov7251 IR sensor found in any /dev/media[0-9]* device")

    sensor_entity = m.group(1)
    sbus, saddr = int(m.group(2)), int(m.group(3), 16)
    block = graph[m.end() :].split("\n- entity")[0]
    lnk = re.search(r'-> "(ipu3-csi2 (\d+))":0 \[([^\]]*)\]', block)
    if not lnk:
        raise RuntimeError(f"No media link from {sensor_entity} to ipu3-csi2")

    csi_entity, csi_idx = lnk.group(1), lnk.group(2)
    was_enabled = "ENABLED" in lnk.group(3)
    video_dev = _v4l_by_name("video*", f"ipu3-cio2 {csi_idx}")
    subdev_dev = _v4l_by_name("v4l-subdev*", sensor_entity)
    if not video_dev or not subdev_dev:
        raise RuntimeError(f"Missing video ({video_dev}) or subdev ({subdev_dev}) node")

    pmic_matches = glob.glob("/sys/bus/i2c/devices/i2c-INT3472:*/tps68470-gpio")
    if not pmic_matches:
        raise RuntimeError("No TPS68470 PMIC (i2c-INT3472 with tps68470-gpio) found")
    pbus_name = os.path.basename(os.path.realpath(os.path.dirname(pmic_matches[0]) + "/.."))
    pbus = int(pbus_name.removeprefix("i2c-"))

    return {
        "product": product,
        "board": board,
        "media": found_media,
        "sensor_entity": sensor_entity,
        "sbus": sbus,
        "saddr": saddr,
        "csi_entity": csi_entity,
        "was_enabled": was_enabled,
        "video": video_dev,
        "subdev": subdev_dev,
        "pbus": pbus,
        "pmic_addr": board["pmic_addr"],
        "pmic_revid": board["pmic_revid"],
        "led": board["led"],
    }


class IRSensorSession:
    """Context manager that opens IPU3-CIO2 OV7251 stream, pulses TPS68470 IR emitter safely,
    adjusts auto-exposure, and guarantees register restoration & emitter shutoff on exit."""

    def __init__(
        self,
        emitter_ma: float = 390.0,
        exposure_max: int = 380,
        gain_max: float = 8.0,
        target_p95: float = 600.0,
        led_channel: int | None = None,
        enable_emitter: bool = True,
    ):
        self.emitter_ma = min(max(emitter_ma, MA_PER_CODE), MAX_CODE * MA_PER_CODE)
        self.code = max(1, min(MAX_CODE, round(self.emitter_ma / MA_PER_CODE)))
        self.exposure_max = min(max(16, exposure_max), EXPOSURE_CAP)
        self.gain_max = gain_max
        self.target_p95 = target_p95
        self.led_override = led_channel
        self.enable_emitter = enable_emitter

        self.lock_fp = None
        self.topo = None
        self.pmic = None
        self.sensor = None
        self.sub = None
        self.stream = None
        self.pmic_saved = {}
        self.sensor_saved = {}
        self.ctrl_saved = {}
        self.raw_buf = bytearray(FRAME_BYTES)
        self.raw_view = memoryview(self.raw_buf)
        self.level = 220.0
        self.cur_exp = None
        self.cur_gain = None
        self.ae_hold = 0

    def __enter__(self):
        self.lock_fp = open(CAM_LOCK_PATH, "w")
        fcntl.flock(self.lock_fp, fcntl.LOCK_EX)

        self.topo = discover_topology()
        self.pmic = I2CDevice(self.topo["pbus"], self.topo["pmic_addr"], False)
        rev = self.pmic.read(REVID)[0]
        if rev != self.topo["pmic_revid"]:
            self.pmic.close()
            raise RuntimeError(f"Unexpected TPS68470 REVID 0x{rev:02x} (expected 0x{self.topo['pmic_revid']:02x})")

        self.sensor = I2CDevice(self.topo["sbus"], self.topo["saddr"], True)
        self.sub = V4L2Subdev(self.topo["subdev"])

        self.pmic_saved = {r: self.pmic.read(r)[0] for r in PMIC_SAVED_REGS}
        self.pmic_saved[WLEDCTL] = 0
        self.ctrl_saved = {c: self.sub.get(c) for c in (V4L2_CID_EXPOSURE, self.sub.gain_cid)}

        link_spec = f'"{self.topo["sensor_entity"]}":0->"{self.topo["csi_entity"]}":0'
        if not self.topo["was_enabled"]:
            subprocess.run(["media-ctl", "-d", self.topo["media"], "-l", f"{link_spec}[1]"], check=True, capture_output=True)
        for pad in (f'"{self.topo["sensor_entity"]}":0', f'"{self.topo["csi_entity"]}":0', f'"{self.topo["csi_entity"]}":1'):
            subprocess.run(["media-ctl", "-d", self.topo["media"], "-V", f"{pad}[fmt:Y10_1X10/{W}x{H}]"], check=True, capture_output=True)
        subprocess.run(
            ["v4l2-ctl", "-d", self.topo["video"], f"--set-fmt-video=width={W},height={H},pixelformat=ip3y"],
            check=True,
            capture_output=True,
        )

        libc = ctypes.CDLL(None)
        self.stream = subprocess.Popen(
            ["v4l2-ctl", "-d", self.topo["video"], "--stream-mmap=4", "--stream-to=-"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
            start_new_session=True,
            preexec_fn=lambda: libc.prctl(1, signal.SIGTERM),
        )
        self.read_raw_frame()
        return self

    def read_raw_frame(self, timeout: float = 1.5) -> np.ndarray:
        n = 0
        while n < FRAME_BYTES:
            if not select.select([self.stream.stdout], [], [], timeout)[0]:
                raise TimeoutError("Timed out waiting for IPU3 CIO2 IR frame")
            k = self.stream.stdout.readinto(self.raw_view[n:])
            if not k:
                raise RuntimeError("v4l2-ctl IR stream ended unexpectedly")
            n += k
        return unpack_ip3y(self.raw_buf)

    def _set_span(self, old_span: int, new_span: int):
        hi, lo = (STROBE_SPAN + 2, new_span >> 8), (STROBE_SPAN + 3, new_span & 0xFF)
        for r, v in ((lo, hi) if new_span > old_span else (hi, lo)):
            self.sensor.write(r, v)

    def apply_ae(self, strobe_active: bool = True):
        emin, emax = self.sub.query_range(V4L2_CID_EXPOSURE)
        gmin, gmax = self.sub.query_range(self.sub.gain_cid)
        ecap = min(self.exposure_max, emax)
        gcap = min(int(self.gain_max * gmin), gmax)

        e = int(max(emin, min(self.level, ecap)))
        g = int(max(gmin, min(gmin * self.level / e, gcap)))
        if e != self.cur_exp:
            if not strobe_active:
                self.sub.set(V4L2_CID_EXPOSURE, e)
            else:
                if self.cur_exp is None:
                    self.sensor.write(STROBE_SPAN, 0, 0, 0, 0)
                old_s = (self.cur_exp or -SPAN_MARGIN) + SPAN_MARGIN
                if self.cur_exp is None or e > self.cur_exp:
                    self._set_span(old_s, e + SPAN_MARGIN)
                    self.sub.set(V4L2_CID_EXPOSURE, e)
                else:
                    self.sub.set(V4L2_CID_EXPOSURE, e)
                    self._set_span(old_s, e + SPAN_MARGIN)
        if g != self.cur_gain:
            self.sub.set(self.sub.gain_cid, g)
        self.cur_exp, self.cur_gain = e, g

    def update_ae(self, frame10: np.ndarray, strobe_active: bool = True) -> float:
        p95 = max(float(np.percentile(frame10[::8, ::8], 95)), 8.0)
        if self.ae_hold > 0:
            self.ae_hold -= 1
            return p95
        emin, emax = self.sub.query_range(V4L2_CID_EXPOSURE)
        gmin, gmax = self.sub.query_range(self.sub.gain_cid)
        ecap = min(self.exposure_max, emax)
        gcap = min(int(self.gain_max * gmin), gmax)
        top = ecap * gcap / gmin
        ratio = min(max(self.target_p95 / p95, 0.5), 2.0) ** 0.5
        new_level = min(max(self.level * ratio, emin), top)
        if abs(new_level / self.level - 1.0) > 0.04:
            self.level = new_level
            self.ae_hold = 2
            self.apply_ae(strobe_active=strobe_active)
        return p95

    def start_emitter(self, led_num: int | None = None):
        if not self.enable_emitter:
            self.apply_ae(strobe_active=False)
            return
        if not self.sensor_saved:
            self.sensor_saved = {
                PAD_OEN: self.sensor.read(PAD_OEN),
                STROBE_MASK: self.sensor.read(STROBE_MASK),
                STROBE_SPAN: self.sensor.read(STROBE_SPAN, 4),
            }
        self.cur_exp = None
        self.apply_ae(strobe_active=True)
        self.sensor.write(STROBE_MASK, 0xFF)
        self.sensor.write(PAD_OEN, self.sensor_saved[PAD_OEN][0] | PAD_STROBE)

        ch = led_num or self.led_override or self.topo["led"]
        dis_flag, cur_reg = (WLED_DISLED1, WLEDC2) if ch == 2 else (WLED_DISLED2, WLEDC1)
        self.pmic.write(WLEDCTL, 0)
        self.pmic.write(GSTAT, 0xFE)
        self.pmic.write(WLEDTO, WLEDTO_852MS)
        self.pmic.write(WLEDMAXF, self.code)
        self.pmic.write(WLEDC1, 0)
        self.pmic.write(WLEDC2, 0)
        self.pmic.write(cur_reg, self.code)
        self.pmic.write(WLEDCTL, dis_flag)
        self.pmic.write(WLEDCTL, dis_flag | WLED_EN)

    def read_gstat(self) -> int:
        return self.pmic.read(GSTAT)[0]

    def __exit__(self, exc_type, exc, tb):
        old_handlers = {s: signal.signal(s, signal.SIG_IGN) for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
        try:
            if self.pmic and self.pmic.fd >= 0:
                try:
                    self.pmic.write(WLEDCTL, 0)
                    for r, v in self.pmic_saved.items():
                        self.pmic.write(r, v)
                    self.pmic.write(GSTAT, 0xFE)
                except OSError:
                    pass
            if self.sensor and self.sensor.fd >= 0 and self.sensor_saved:
                try:
                    for r, vals in self.sensor_saved.items():
                        self.sensor.write(r, *vals)
                except OSError:
                    pass
            if self.sub and self.sub.fd >= 0 and self.ctrl_saved:
                try:
                    for c, v in self.ctrl_saved.items():
                        self.sub.set(c, v)
                except OSError:
                    pass
            if self.stream:
                self.stream.terminate()
                try:
                    self.stream.wait(timeout=1.5)
                except subprocess.TimeoutExpired:
                    self.stream.kill()
            if self.topo and not self.topo["was_enabled"]:
                link_spec = f'"{self.topo["sensor_entity"]}":0->"{self.topo["csi_entity"]}":0'
                subprocess.run(["media-ctl", "-d", self.topo["media"], "-l", f"{link_spec}[0]"], capture_output=True)
        finally:
            if self.sub:
                self.sub.close()
            if self.sensor:
                self.sensor.close()
            if self.pmic:
                self.pmic.close()
            if self.lock_fp:
                fcntl.flock(self.lock_fp, fcntl.LOCK_UN)
                self.lock_fp.close()
            for s, h in old_handlers.items():
                signal.signal(s, h)
