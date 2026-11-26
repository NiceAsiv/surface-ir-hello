"""
SSE4.2-compatible (non-AVX) IR Face Detection & Recognition using OpenCV YuNet + SFace.
Works natively on Intel Pentium Gold 4415Y / 4425Y (Surface Go) and Python 3.14+.
"""
import json
import os
import subprocess

import cv2
import numpy as np

MODEL_DIR = "/usr/share/surface-ir-hello/models"
FACES_DIR = "/etc/surface-ir-hello/faces"

YUNET_FILE = "face_detection_yunet_2023mar.onnx"
SFACE_FILE = "face_recognition_sface_2021dec.onnx"

MODEL_MIRRORS = {
    YUNET_FILE: (
        100_000,
        [
            "https://hf-mirror.com/opencv/face_detection_yunet/resolve/main/face_detection_yunet_2023mar.onnx",
            "https://huggingface.co/opencv/face_detection_yunet/resolve/main/face_detection_yunet_2023mar.onnx",
        ],
    ),
    SFACE_FILE: (
        30_000_000,
        [
            "https://hf-mirror.com/opencv/face_recognition_sface/resolve/main/face_recognition_sface_2021dec.onnx",
            "https://huggingface.co/opencv/face_recognition_sface/resolve/main/face_recognition_sface_2021dec.onnx",
        ],
    ),
}


def ensure_models(model_dir: str = MODEL_DIR) -> tuple[str, str]:
    os.makedirs(model_dir, exist_ok=True)
    paths = []
    for fname, (min_sz, urls) in MODEL_MIRRORS.items():
        dest = os.path.join(model_dir, fname)
        if os.path.exists(dest) and os.path.getsize(dest) >= min_sz:
            paths.append(dest)
            continue
        tmp = f"{dest}.part"
        for url in urls:
            print(f"[models] Downloading {fname} from {url} ...")
            r = subprocess.run(["curl", "-fL", "--connect-timeout", "10", "--max-time", "180", "-o", tmp, url])
            if r.returncode == 0 and os.path.exists(tmp) and os.path.getsize(tmp) >= min_sz:
                os.replace(tmp, dest)
                break
        else:
            raise RuntimeError(f"Failed to download {fname} into {model_dir}")
        paths.append(dest)
    return paths[0], paths[1]


def normalize_ir10_to_bgr8(frame10: np.ndarray, clahe=None) -> np.ndarray:
    """Convert 10-bit [0..1023] IR grayscale frame to contrast-normalized 8-bit BGR."""
    lo, hi = np.percentile(frame10[::4, ::4], (1.0, 99.5))
    if hi - lo >= 32.0:
        scaled = np.clip((frame10.astype(np.float32) - lo) * (255.0 / (hi - lo)), 0, 255).astype(np.uint8)
    else:
        scaled = np.clip(frame10 >> 2, 0, 255).astype(np.uint8)
    if clahe is None:
        clahe = cv2.createCLAHE(clipLimit=2.2, tileGridSize=(8, 8))
    enh = clahe.apply(scaled)
    return cv2.cvtColor(enh, cv2.COLOR_GRAY2BGR)


class IRFaceEngine:
    def __init__(self, model_dir: str = MODEL_DIR, det_score_thresh: float = 0.60):
        yunet_path, sface_path = ensure_models(model_dir)
        self.detector = cv2.FaceDetectorYN.create(yunet_path, "", (640, 480), det_score_thresh, 0.3, 5000)
        self.recognizer = cv2.FaceRecognizerSF.create(sface_path, "")
        self.clahe = cv2.createCLAHE(clipLimit=2.2, tileGridSize=(8, 8))

    def detect_and_embed(self, frame10: np.ndarray, check_rotations: bool = True):
        """Detect largest valid face across landscape/portrait orientations and return 128-D SFace feature."""
        bgr0 = normalize_ir10_to_bgr8(frame10, self.clahe)
        rotations = [(0, bgr0)]
        if check_rotations:
            rotations.extend(
                [
                    (180, cv2.rotate(bgr0, cv2.ROTATE_180)),
                    (90, cv2.rotate(bgr0, cv2.ROTATE_90_CLOCKWISE)),
                    (270, cv2.rotate(bgr0, cv2.ROTATE_90_COUNTERCLOCKWISE)),
                ]
            )

        for angle, img in rotations:
            h, w = img.shape[:2]
            self.detector.setInputSize((w, h))
            _, faces = self.detector.detect(img)
            if faces is None or len(faces) == 0:
                continue
            best = max(faces, key=lambda f: float(f[2] * f[3]))
            if float(best[2]) < 55 or float(best[3]) < 55:
                continue
            aligned = self.recognizer.alignCrop(img, best)
            feat = self.recognizer.feature(aligned)
            x, y, bw, bh = [max(0, int(v)) for v in best[:4]]
            return {
                "angle": angle,
                "bbox": (x, y, bw, bh),
                "det_score": float(best[-1]),
                "feature": feat.flatten().astype(np.float32),
                "bgr": img,
            }
        return None

    def match_cosine(self, feat_a: np.ndarray, feat_b: np.ndarray) -> float:
        a = feat_a.reshape(1, -1).astype(np.float32)
        b = feat_b.reshape(1, -1).astype(np.float32)
        return float(self.recognizer.match(a, b, cv2.FaceRecognizerSF_FR_COSINE))


def load_user_embeddings(user: str, faces_dir: str = FACES_DIR) -> list[np.ndarray]:
    path = os.path.join(faces_dir, f"{user}.json")
    if not os.path.exists(path):
        return []
    data = json.loads(open(path, "r", encoding="utf-8").read())
    return [np.array(item["embedding"], dtype=np.float32) for item in data.get("samples", [])]


def save_user_embeddings(user: str, samples: list[dict], faces_dir: str = FACES_DIR):
    os.makedirs(faces_dir, mode=0o700, exist_ok=True)
    path = os.path.join(faces_dir, f"{user}.json")
    tmp = f"{path}.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"user": user, "version": 1, "samples": samples}, f, indent=2)
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)
