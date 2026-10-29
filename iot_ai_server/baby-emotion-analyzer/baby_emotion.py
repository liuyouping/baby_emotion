# Copyright (c) 2026 稚声慧眸
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.


import argparse
import os
import sys
import warnings
from glob import glob

# ---- 路径 & 依赖准备 ----
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
# 优先加载本地 .deps (可选, 如果用户有本地依赖)
_LOCAL_DEPS = os.path.join(BASE_DIR, ".deps")
if os.path.isdir(_LOCAL_DEPS) and _LOCAL_DEPS not in sys.path:
    sys.path.insert(0, _LOCAL_DEPS)

os.environ.setdefault("GLOG_minloglevel", "3")   # 屏蔽 MediaPipe INFO 日志
warnings.filterwarnings("ignore")

import cv2
import numpy as np

try:
    import mediapipe as mp
    from mediapipe.tasks.python import vision
    from mediapipe.tasks.python.core import base_options as bo
except ImportError as e:
    print(f"[错误] 缺少依赖: {e}")
    print("请先执行:  pip install -r requirements.txt")
    sys.exit(1)


# =====================================================
# 1. MediaPipe FaceLandmarker 初始化
# =====================================================
MODEL_PATH = os.path.join(BASE_DIR, "face_landmarker.task")

if not os.path.exists(MODEL_PATH):
    print(f"[错误] 缺少模型文件 {MODEL_PATH}")
    print("请从下面地址下载 face_landmarker.task 放到本目录:")
    print("  https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task")
    sys.exit(1)

_options = vision.FaceLandmarkerOptions(
    base_options=bo.BaseOptions(model_asset_path=MODEL_PATH),
    running_mode=vision.RunningMode.IMAGE,
    num_faces=2,                          # 支持多人合照 (取置信度最高那张)
    min_face_detection_confidence=0.15,   # 更宽松: 检出小/暗/侧脸
    min_face_presence_confidence=0.15,
    output_face_blendshapes=False,
    output_facial_transformation_matrixes=False,
)
landmarker = vision.FaceLandmarker.create_from_options(_options)

# 478 关键点索引
IDX = {
    "left_brow_inner": 46,  "left_brow_outer": 52,
    "right_brow_inner": 276, "right_brow_outer": 282,
    "left_eye_top": 159,    "left_eye_bottom": 145,
    "left_eye_left": 33,    "left_eye_right": 133,
    "right_eye_top": 386,   "right_eye_bottom": 374,
    "right_eye_left": 362,  "right_eye_right": 263,
    "mouth_top": 13,        "mouth_bottom": 14,
    "mouth_left": 78,       "mouth_right": 308,
    "face_left": 234,       "face_right": 454,
    "face_top": 10,         "face_bottom": 152,
}

EMOTIONS = ["平静", "开心", "困倦", "烦躁", "身体不适"]


# =====================================================
# 2. 关键点 & 几何特征
# =====================================================
def extract_landmarks(image_bgr):
    """多策略尝试: 原图 / 放大 / 对比度增强 → 提高检出率"""
    h, w = image_bgr.shape[:2]
    candidates = [image_bgr]

    # 小图 (最长边 < 800) 放大 1.5x, MediaPipe 对小人脸更敏感
    if max(h, w) < 1200:
        scale = 1.5
        big = cv2.resize(image_bgr, None, fx=scale, fy=scale, interpolation=cv2.INTER_CUBIC)
        candidates.append(big)

    # CLAHE 对比度增强 (LAB 空间 L 通道)
    try:
        lab = cv2.cvtColor(image_bgr, cv2.COLOR_BGR2LAB)
        l, a, b = cv2.split(lab)
        clahe = cv2.createCLAHE(clipLimit=2.5, tileGridSize=(8, 8))
        l = clahe.apply(l)
        enhanced = cv2.cvtColor(cv2.merge([l, a, b]), cv2.COLOR_LAB2BGR)
        candidates.append(enhanced)
    except Exception:
        pass

    # 灰度过曝补偿 (直方图均衡化)
    try:
        eq = cv2.cvtColor(image_bgr, cv2.COLOR_BGR2YCrCb)
        eq[:, :, 0] = cv2.equalizeHist(eq[:, :, 0])
        eq = cv2.cvtColor(eq, cv2.COLOR_YCrCb2BGR)
        candidates.append(eq)
    except Exception:
        pass

    for img in candidates:
        rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        mp_img = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        result = landmarker.detect(mp_img)
        if result.face_landmarks:
            lm = result.face_landmarks[0]
            return np.array([[p.x, p.y, p.z] for p in lm], dtype=np.float32)
    return None


def _d(landmarks, a, b):
    return float(np.linalg.norm(landmarks[a] - landmarks[b]))


def compute_features(landmarks):
    L = IDX
    face_w = _d(landmarks, L["face_left"], L["face_right"])
    face_h = _d(landmarks, L["face_top"], L["face_bottom"])
    if face_w < 1e-6:
        return None

    brow_inner_gap = _d(landmarks, L["left_brow_inner"], L["right_brow_inner"]) / face_w

    lh = _d(landmarks, L["left_eye_top"], L["left_eye_bottom"])
    lw = _d(landmarks, L["left_eye_left"], L["left_eye_right"])
    rh = _d(landmarks, L["right_eye_top"], L["right_eye_bottom"])
    rw = _d(landmarks, L["right_eye_left"], L["right_eye_right"])
    eye_openness = (lh / max(lw, 1e-6) + rh / max(rw, 1e-6)) / 2.0

    mouth_openness = _d(landmarks, L["mouth_top"], L["mouth_bottom"]) / face_w
    mouth_width = _d(landmarks, L["mouth_left"], L["mouth_right"]) / face_w
    mouth_center_y = (landmarks[L["mouth_top"], 1] + landmarks[L["mouth_bottom"], 1]) / 2.0
    smile_score = mouth_center_y - (landmarks[L["mouth_left"], 1] + landmarks[L["mouth_right"], 1]) / 2.0
    mouth_aspect = _d(landmarks, L["mouth_top"], L["mouth_bottom"]) / max(
        _d(landmarks, L["mouth_left"], L["mouth_right"]), 1e-6)

    return {
        "brow_inner_gap": brow_inner_gap,
        "eye_openness": eye_openness,
        "mouth_openness": mouth_openness,
        "mouth_width": mouth_width,
        "smile_score": smile_score,
        "mouth_aspect": mouth_aspect,
    }


# =====================================================
# 3. 规则式情绪打分
# =====================================================
def _gaussian(x, mu, sigma):
    return np.exp(-0.5 * ((x - mu) / sigma) ** 2)


def _sigmoid(x):
    return 1.0 / (1.0 + np.exp(-np.clip(x, -30, 30)))


def score_emotions(feat):
    f = feat

    # 开心: smile↑ + 嘴宽 + 眼睛稍眯
    happy = (_sigmoid((f["smile_score"] - 0.015) * 80) * 0.55 +
             _sigmoid((0.22 - f["eye_openness"]) * 15) * 0.25 +
             _sigmoid((f["mouth_width"] - 0.45) * 15) * 0.20)
    # 困倦: 眼闭合 + 嘴闭
    sleepy = (_sigmoid((0.22 - f["eye_openness"]) * 25) * 0.75 +
              _sigmoid((0.04 - f["mouth_openness"]) * 30) * 0.25)
    # 烦躁: 眉内聚 + (嘴紧抿 或 张嘴)
    angry = (_sigmoid((0.28 - f["brow_inner_gap"]) * 20) * 0.50 +
             max(_sigmoid((0.30 - f["mouth_aspect"]) * 10),
                 _sigmoid((f["mouth_openness"] - 0.08) * 15)) * 0.50)
    # 身体不适: 嘴角下拉 + 皱眉 + 可能张嘴
    pain = (_sigmoid((-0.005 - f["smile_score"]) * 60) * 0.45 +
            _sigmoid((0.32 - f["brow_inner_gap"]) * 12) * 0.25 +
            _sigmoid((f["mouth_openness"] - 0.10) * 12) * 0.30)
    # 平静: 各特征在中性区间
    neutral = (_gaussian(f["smile_score"], 0.0, 0.015) * 0.30 +
               _gaussian(f["eye_openness"], 0.25, 0.04) * 0.25 +
               _gaussian(f["mouth_openness"], 0.03, 0.02) * 0.20 +
               _gaussian(f["brow_inner_gap"], 0.38, 0.04) * 0.25)

    raw = {"平静": float(neutral), "开心": float(happy), "困倦": float(sleepy),
           "烦躁": float(angry), "身体不适": float(pain)}

    vals = np.array(list(raw.values()), dtype=np.float32) / 0.8
    exp_vals = np.exp(vals - np.max(vals))
    probs = exp_vals / exp_vals.sum()
    return {k: round(float(p), 4) for k, p in zip(raw.keys(), probs)}


# =====================================================
# 4. 单帧 / 单图分析入口
# =====================================================
def analyze_frame(img_bgr):
    """返回 dict: {emotion, confidence, probs, features, landmarks, status}"""
    result = {"emotion": "-", "confidence": 0.0, "probs": {},
              "features": None, "landmarks": None, "status": "ok"}

    landmarks = extract_landmarks(img_bgr)
    if landmarks is None:
        result["status"] = "未检测到人脸"
        return result

    feat = compute_features(landmarks)
    if feat is None:
        result["status"] = "特征计算异常"
        return result

    probs = score_emotions(feat)
    best = max(probs, key=probs.get)

    result.update({
        "landmarks": landmarks, "features": feat,
        "emotion": best, "confidence": probs[best], "probs": probs,
    })
    return result


# =====================================================
# 5. 可视化
# =====================================================
LABEL_COLORS = {
    "平静": (180, 180, 180), "开心": (0, 220, 120),
    "困倦": (180, 140, 255), "烦躁": (0, 80, 255),
    "身体不适": (80, 160, 255),
}


def draw_result(img_bgr, result, show_probs=True):
    """在图像上画关键点 + 情绪/概率条, 返回标注后的图像副本"""
    vis = img_bgr.copy()
    h, w = vis.shape[:2]

    # 关键点 (稀疏采样)
    if result["landmarks"] is not None:
        lm = result["landmarks"]
        for name in IDX:
            pt = lm[IDX[name]]
            cv2.circle(vis, (int(pt[0] * w), int(pt[1] * h)), 3, (0, 255, 255), -1)

    # 顶部标题
    cv2.putText(vis,
                f"Emotion: {result['emotion']}  ({result['confidence']:.0%})",
                (20, 35), cv2.FONT_HERSHEY_SIMPLEX, 0.85, (0, 255, 255), 2)

    if show_probs and result["probs"]:
        y0 = 70
        for i, (emotion, p) in enumerate(result["probs"].items()):
            y = y0 + i * 22
            cv2.rectangle(vis, (10, y - 14), (int(w * 0.42), y + 4), (60, 60, 60), -1)
            bw = int((w * 0.40) * p)
            cv2.rectangle(vis, (10, y - 14), (10 + bw, y + 4),
                          LABEL_COLORS.get(emotion, (255, 255, 255)), -1)
            cv2.putText(vis, f"{emotion} {p:.0%}", (int(w * 0.44), y),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.55, (230, 230, 230), 1)
    return vis


# =====================================================
# 6. 批量图片处理
# =====================================================
IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


def scan_images(input_dir):
    if not os.path.isdir(input_dir):
        return []
    paths = []
    for fname in os.listdir(input_dir):
        if os.path.splitext(fname)[1].lower() in IMAGE_EXTS:
            paths.append(os.path.join(input_dir, fname))
    paths.sort()
    return paths


def run_batch(input_dir, output_dir):
    os.makedirs(output_dir, exist_ok=True)
    files = scan_images(input_dir)
    if not files:
        print(f"[!] {input_dir} 下没有找到图片 (支持 jpg/png/bmp/webp)")
        return

    print("=" * 70)
    print(f" 婴幼儿人脸情绪分析  |  扫描 {len(files)} 张图片")
    print(f" 输入: {input_dir}")
    print(f" 输出: {output_dir}")
    print("=" * 70)

    ok, fail = 0, 0
    for fp in files:
        name = os.path.basename(fp)
        img = cv2.imread(fp)
        if img is None:
            print(f"[!!] {name:16s}  读取失败")
            fail += 1
            continue

        res = analyze_frame(img)
        if res["status"] != "ok":
            print(f"[!!] {name:16s}  {res['status']}")
            fail += 1
            continue

        # 可视化保存
        vis = draw_result(img, res)
        out_name = os.path.splitext(name)[0] + "_result.jpg"
        cv2.imwrite(os.path.join(output_dir, out_name), vis)

        prob_str = "  ".join(f"{k}:{v:.0%}" for k, v in res["probs"].items())
        print(f"[OK] {name:16s}  情绪={res['emotion']:<5}  "
              f"置信度={res['confidence']:.2%}  |  {prob_str}")
        ok += 1

    print(f"\n完成: {ok} 张成功, {fail} 张失败. 结果 -> {output_dir}")


# =====================================================
# 7. 摄像头实时
# =====================================================
def run_camera(cam_id=0):
    cap = cv2.VideoCapture(cam_id)
    if not cap.isOpened():
        print(f"[错误] 无法打开摄像头 (id={cam_id})")
        return

    print("摄像头已启动, 按 q 退出, 按 s 保存当前帧.")
    frame_idx = 0
    while True:
        ret, frame = cap.read()
        if not ret:
            break
        res = analyze_frame(frame)
        vis = draw_result(frame, res, show_probs=(res["status"] == "ok"))
        cv2.imshow("Baby Emotion Analyzer - Press q to quit, s to save", vis)

        key = cv2.waitKey(1) & 0xFF
        if key == ord('q'):
            break
        elif key == ord('s'):
            out = f"camera_frame_{frame_idx:04d}.jpg"
            cv2.imwrite(out, vis)
            print(f"已保存: {out}")
            frame_idx += 1

    cap.release()
    cv2.destroyAllWindows()


# =====================================================
# 8. CLI 入口
# =====================================================
def main():
    parser = argparse.ArgumentParser(description="婴幼儿人脸情绪分析")
    parser.add_argument("--input", "-i", default=None,
                        help="图片目录 (默认: imgs/baby)")
    parser.add_argument("--output", "-o", default=None,
                        help="结果目录 (默认: output)")
    parser.add_argument("--image", default=None,
                        help="单张图片路径 (优先级高于 --input)")
    parser.add_argument("--camera", "-c", type=int, default=None,
                        help="摄像头 ID (通常 0)")
    args = parser.parse_args()

    if args.camera is not None:
        run_camera(args.camera)
        return

    if args.image:
        # 单张图片
        img = cv2.imread(args.image)
        if img is None:
            print(f"[错误] 无法读取 {args.image}")
            sys.exit(1)
        res = analyze_frame(img)
        if res["status"] == "ok":
            out_dir = args.output or os.path.join(BASE_DIR, "output")
            os.makedirs(out_dir, exist_ok=True)
            vis = draw_result(img, res)
            out_path = os.path.join(out_dir,
                os.path.splitext(os.path.basename(args.image))[0] + "_result.jpg")
            cv2.imwrite(out_path, vis)
            print(f"情绪={res['emotion']}  置信度={res['confidence']:.2%}")
            print(f"概率: {res['probs']}")
            print(f"已保存: {out_path}")
        else:
            print(f"[!] {res['status']}")
        return

    # 默认批量模式
    input_dir = args.input or os.path.join(BASE_DIR, "imgs", "baby")
    output_dir = args.output or os.path.join(BASE_DIR, "output")
    run_batch(input_dir, output_dir)


if __name__ == "__main__":
    main()
