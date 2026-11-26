"""
SSE4.2-compatible (non-AVX) IR Face Detection & Recognition using OpenCV YuNet + SFace.
Works natively on Intel Pentium Gold 4415Y / 4425Y (Surface Go) and Python 3.14+.
"""
import fcntl
import hashlib
import json
import os
import subprocess
import tempfile

import cv2
import numpy as np

MODEL_DIR = "/usr/share/surface-ir-hello/models"
FACES_DIR = "/etc/surface-ir-hello/faces"
DETECTION_ANGLES = (0, 180, 90, 270)

YUNET_FILE = "face_detection_yunet_2023mar.onnx"
SFACE_FILE = "face_recognition_sface_2021dec.onnx"

MODEL_SOURCES = {
    YUNET_FILE: (
        "face_detection_yunet",
        "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4",
    ),
    SFACE_FILE: (
        "face_recognition_sface",
        "0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79",
    ),
}


def model_matches(path: str, expected_hash: str) -> bool:
    try:
        with open(path, "rb") as model:
            return hashlib.file_digest(model, "sha256").hexdigest() == expected_hash
    except FileNotFoundError:
        return False


def ensure_models(model_dir: str = MODEL_DIR) -> tuple[str, str]:
    os.makedirs(model_dir, exist_ok=True)
    paths = []
    for filename, (model, expected_hash) in MODEL_SOURCES.items():
        dest = os.path.join(model_dir, filename)
        if not model_matches(dest, expected_hash):
            urls = (
                f"https://huggingface.co/opencv/{model}/resolve/main/{filename}",
                f"https://media.githubusercontent.com/media/opencv/opencv_zoo/main/models/{model}/{filename}",
                f"https://hf-mirror.com/opencv/{model}/resolve/main/{filename}",
            )
            with tempfile.NamedTemporaryFile(dir=model_dir, prefix=f".{filename}-", delete=False) as tmp:
                tmp_path = tmp.name
            try:
                for url in urls:
                    print(f"[models] Downloading {filename} from {url} ...", flush=True)
                    result = subprocess.run(["curl", "-fL", "--connect-timeout", "10", "--max-time", "180",
                                             "-o", tmp_path, url])
                    if result.returncode == 0 and model_matches(tmp_path, expected_hash):
                        os.chmod(tmp_path, 0o644)
                        os.replace(tmp_path, dest)
                        break
                else:
                    raise RuntimeError(f"Could not download a SHA-256 verified copy of {filename} into {model_dir}")
            finally:
                if os.path.exists(tmp_path):
                    os.unlink(tmp_path)
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

    def detect_and_embed(
        self,
        frame10: np.ndarray,
        check_rotations: bool = True,
        *,
        angles: tuple[int, ...] | None = None,
        half_resolution: bool = False,
    ):
        """Detect the largest valid face and align its landmarks in the original image."""
        bgr0 = normalize_ir10_to_bgr8(frame10, self.clahe)
        if angles is None:
            angles = DETECTION_ANGLES if check_rotations else (0,)
        rotations = {
            180: cv2.ROTATE_180,
            90: cv2.ROTATE_90_CLOCKWISE,
            270: cv2.ROTATE_90_COUNTERCLOCKWISE,
        }

        for angle in angles:
            img = cv2.rotate(bgr0, rotations[angle]) if angle else bgr0
            h, w = img.shape[:2]
            detection_image = img
            if half_resolution:
                detection_image = cv2.resize(img, (w // 2, h // 2), interpolation=cv2.INTER_AREA)
            dh, dw = detection_image.shape[:2]
            self.detector.setInputSize((dw, dh))
            _, faces = self.detector.detect(detection_image)
            if faces is None or len(faces) == 0:
                continue
            if half_resolution:
                faces = faces.copy()
                faces[:, 0:14:2] *= w / dw
                faces[:, 1:14:2] *= h / dh
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


def load_user_samples(user: str, faces_dir: str = FACES_DIR) -> list[dict]:
    path = os.path.join(faces_dir, f"{user}.json")
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f).get("samples", [])
    except FileNotFoundError:
        return []


def load_user_embeddings(user: str, faces_dir: str = FACES_DIR) -> list[np.ndarray]:
    return [np.asarray(sample["embedding"], dtype=np.float32) for sample in load_user_samples(user, faces_dir)]


def save_user_embeddings(user: str, samples: list[dict], faces_dir: str = FACES_DIR):
    os.makedirs(faces_dir, mode=0o700, exist_ok=True)
    path = os.path.join(faces_dir, f"{user}.json")
    with tempfile.NamedTemporaryFile(mode="w", dir=faces_dir, encoding="utf-8", delete=False) as f:
        tmp = f.name
        try:
            json.dump({"user": user, "version": 1, "samples": samples}, f, indent=2)
            f.close()
            os.replace(tmp, path)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)


def update_user_embeddings(user: str, samples: list[dict], *, append: bool, faces_dir: str = FACES_DIR) -> int:
    os.makedirs(faces_dir, mode=0o700, exist_ok=True)
    with open(os.path.join(faces_dir, f".{user}.lock"), "a", encoding="utf-8") as guard:
        fcntl.flock(guard, fcntl.LOCK_EX)
        previous = load_user_samples(user, faces_dir) if append else []
        combined = previous + samples
        save_user_embeddings(user, combined, faces_dir)
        return len(combined)
