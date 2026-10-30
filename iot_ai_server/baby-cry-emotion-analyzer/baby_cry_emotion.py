# Copyright (c) 2026 稚声慧眸
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


import os
import sys
import argparse
import warnings

# ---- 本地依赖 ----
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
_LOCAL_DEPS = os.path.join(BASE_DIR, ".deps")
if os.path.isdir(_LOCAL_DEPS) and _LOCAL_DEPS not in sys.path:
    sys.path.insert(0, _LOCAL_DEPS)

warnings.filterwarnings("ignore")
os.environ.setdefault("GLOG_minloglevel", "3")

import numpy as np

try:
    import librosa
    import soundfile as sf
    from scipy.signal import find_peaks
except ImportError as e:
    print(f"[错误] 缺少依赖: {e}")
    print("安装命令:  python -m pip install -r requirements.txt")
    sys.exit(1)

EMOTIONS = ["饥饿", "疼痛", "困倦", "身体不适", "平静"]


# =====================================================
# 1. 音频加载与预处理
# =====================================================
def load_audio(path, sr=16000, mono=True):
    """返回 y (np.float32), sr (int)"""
    y, sr_loaded = librosa.load(path, sr=sr, mono=mono)
    return y.astype(np.float32), sr


# =====================================================
# 2. 特征提取
# =====================================================
def extract_features(y, sr):
    frame_length = 512
    hop_length = 160

    f0, voiced_flag, _ = librosa.pyin(y, fmin=80, fmax=1200, sr=sr,
                                       frame_length=frame_length, hop_length=hop_length)
    voiced_f0 = f0[voiced_flag]
    if len(voiced_f0) == 0:
        voiced_f0 = np.array([0.0])

    f0_mean = float(np.mean(voiced_f0))
    f0_std = float(np.std(voiced_f0))
    f0_max = float(np.percentile(voiced_f0, 95))

    rms = librosa.feature.rms(y=y, frame_length=frame_length, hop_length=hop_length)[0]
    rms_db = float(np.mean(librosa.amplitude_to_db(rms, ref=1.0)))
    rms_peak_db = float(np.percentile(librosa.amplitude_to_db(rms, ref=1.0), 95))
    rms_std = float(np.std(rms))

    zcr = float(np.mean(librosa.feature.zero_crossing_rate(
        y=y, frame_length=frame_length, hop_length=hop_length)[0]))

    spec_centroid = float(np.mean(
        librosa.feature.spectral_centroid(y=y, sr=sr, n_fft=frame_length,
                                          hop_length=hop_length)[0]))

    mfcc = librosa.feature.mfcc(y=y, sr=sr, n_mfcc=13, n_fft=frame_length,
                                 hop_length=hop_length, win_length=frame_length)
    mfcc_0 = float(np.mean(mfcc[0]))
    mfcc_1 = float(np.mean(mfcc[1]))
    mfcc_2 = float(np.mean(mfcc[2]))

    # 节奏特征
    cry_frames = rms > np.percentile(rms, 60)
    cry_ratio = float(np.mean(cry_frames))

    rms_norm = (rms - rms.mean()) / (rms.std() + 1e-9)
    autocorr = np.correlate(rms_norm, rms_norm, mode="full")
    autocorr = autocorr[len(autocorr) // 2:]
    peaks, _ = find_peaks(autocorr, height=autocorr.max() * 0.3,
                          distance=int(0.2 * sr / hop_length))
    rhythm_period = float(peaks[0] * hop_length / sr) if len(peaks) > 0 else 0.0

    duration = float(len(y) / sr)
    cry_duration = float(cry_ratio * duration)

    return {
        "f0_mean": f0_mean, "f0_std": f0_std, "f0_max": f0_max,
        "rms_db": rms_db, "rms_peak_db": rms_peak_db, "rms_std": rms_std,
        "zcr": zcr, "spectral_centroid": spec_centroid,
        "mfcc_0": mfcc_0, "mfcc_1": mfcc_1, "mfcc_2": mfcc_2,
        "cry_ratio": cry_ratio, "rhythm_period": rhythm_period,
        "duration": duration, "cry_duration": cry_duration,
    }


# =====================================================
# 3. 规则引擎 (已用真实+合成音频校准)
# =====================================================
def _sigmoid(x):
    return 1.0 / (1.0 + np.exp(-np.clip(x, -30, 30)))


def _gaussian(x, mu, sigma):
    return np.exp(-0.5 * ((x - mu) / max(sigma, 1e-6)) ** 2)


def score_emotions(feat):
    f = feat

    # ---- 平静 ----
    quiet_novoice = _sigmoid((60 - f["f0_mean"]) * 0.05)
    quiet_lowenergy = _sigmoid((-35 - f["rms_peak_db"]) * 0.2)
    quiet_cryratio_low = _sigmoid((0.30 - f["cry_ratio"]) * 4)
    quiet_score = quiet_novoice * 0.50 + quiet_lowenergy * 0.30 + quiet_cryratio_low * 0.20

    # ---- 疼痛 ----
    pain_f0max = _sigmoid((f["f0_max"] - 550) * 0.008)
    pain_peak = _sigmoid((f["rms_peak_db"] - -15) * 0.20)
    pain_burst = _sigmoid((f["rms_std"] - 0.06) * 10)
    pain_score = pain_f0max * 0.50 + pain_peak * 0.25 + pain_burst * 0.25

    # ---- 饥饿 ----
    hunger_f0 = _gaussian(f["f0_mean"], 380, 70)
    hunger_rhythm = _gaussian(f["rhythm_period"], 0.5, 0.15)
    hunger_cryratio = _gaussian(f["cry_ratio"], 0.60, 0.15)
    hunger_score = hunger_f0 * 0.45 + hunger_rhythm * 0.30 + hunger_cryratio * 0.25

    # ---- 困倦 ----
    sleepy_f0 = _sigmoid((280 - f["f0_mean"]) * 0.015)
    sleepy_intermittent = _sigmoid((0.45 - f["cry_ratio"]) * 6)
    sleepy_rhythm = _sigmoid((f["rhythm_period"] - 0.8) * 3)
    sleepy_score = sleepy_f0 * 0.40 + sleepy_intermittent * 0.35 + sleepy_rhythm * 0.25

    # ---- 身体不适 ----
    discomfort_steady = _sigmoid((0.05 - f["rms_std"]) * 30)
    discomfort_norhythm = _sigmoid((0.15 - f["rhythm_period"]) * 15)
    discomfort_f0range = _gaussian(f["f0_mean"], 430, 80)
    discomfort_score = discomfort_steady * 0.40 + discomfort_norhythm * 0.30 + discomfort_f0range * 0.30

    raw = {
        "平静": float(quiet_score),
        "疼痛": float(pain_score),
        "饥饿": float(hunger_score),
        "困倦": float(sleepy_score),
        "身体不适": float(discomfort_score),
    }

    if f["f0_mean"] < 60:
        raw["平静"] *= 2.5
        raw["疼痛"] *= 0.3
        raw["饥饿"] *= 0.3
        raw["困倦"] *= 0.3
        raw["身体不适"] *= 0.3

    vals = np.array(list(raw.values()), dtype=np.float32) / 0.55
    exp_vals = np.exp(vals - np.max(vals))
    probs = exp_vals / exp_vals.sum()
    return {k: round(float(p), 4) for k, p in zip(raw.keys(), probs)}


# =====================================================
# 4. 单文件分析入口
# =====================================================
def analyze_file(audio_path):
    result = {"file": os.path.basename(audio_path),
              "emotion": "-", "confidence": 0.0, "probs": {},
              "features": None, "status": "ok"}
    try:
        y, sr = load_audio(audio_path)
    except Exception as e:
        result["status"] = f"加载失败: {e}"
        return result

    try:
        feat = extract_features(y, sr)
    except Exception as e:
        result["status"] = f"特征提取异常: {e}"
        return result

    probs = score_emotions(feat)
    best = max(probs, key=probs.get)
    result.update({"emotion": best, "confidence": probs[best],
                   "probs": probs, "features": feat})
    return result


# =====================================================
# 5. ASCII 波形 & 概率条
# =====================================================
def print_waveform(y, width=70):
    step = max(1, len(y) // width)
    sampled = y[::step][:width]
    peak = np.max(np.abs(sampled)) + 1e-6
    height = 12
    lines = []
    for row in range(height, -height, -1):
        threshold = row / height if row >= 0 else row / height
        line = ""
        for v in sampled:
            v_norm = v / peak
            if (threshold >= 0 and v_norm >= threshold) or \
               (threshold < 0 and v_norm <= threshold):
                line += "█"
            else:
                line += " "
        lines.append(line)
    return "\n".join(lines)


def print_result_bar(probs):
    maxw = 40
    lines = []
    for emotion, p in probs.items():
        fill = int(p * maxw)
        bar = "█" * fill + "░" * (maxw - fill)
        lines.append(f"  {emotion:<5s} [{bar}] {p:.1%}")
    return "\n".join(lines)


# =====================================================
# 6. 批量扫描
# =====================================================
AUDIO_EXTS = (".wav", ".mp3", ".flac", ".ogg", ".m4a", ".aac")


def scan_dir(d):
    if not os.path.isdir(d):
        return []
    return sorted([os.path.join(d, f) for f in os.listdir(d)
                   if f.lower().endswith(AUDIO_EXTS)])


# =====================================================
# 7. CLI 入口
# =====================================================
def main():
    parser = argparse.ArgumentParser(description="婴幼儿哭声情绪分析")
    parser.add_argument("path", nargs="?", default=None, help="音频文件路径")
    parser.add_argument("--batch", "-b", default=None, help="批量扫描目录")
    parser.add_argument("--mic", "-m", action="store_true", help="麦克风录音 5s 后分析")
    args = parser.parse_args()

    # 麦克风模式
    if args.mic:
        try:
            import sounddevice as sd
        except ImportError:
            print("[错误] 缺少 sounddevice, 请 pip install sounddevice")
            return
        print("🎙️  正在录音 5 秒...请让宝宝发声 (或模拟哭声)")
        sr = 16000
        rec = sd.rec(int(5 * sr), samplerate=sr, channels=1, dtype="float32")
        sd.wait()
        y = rec.flatten()
        out_path = os.path.join(BASE_DIR, "recording.wav")
        sf.write(out_path, y, sr, subtype="PCM_16")
        print(f"录音已保存: {out_path}\n")
        files = [out_path]
    elif args.batch:
        files = scan_dir(args.batch)
    elif args.path:
        files = [args.path]
    else:
        # 默认扫描 cries/
        default_dir = os.path.join(BASE_DIR, "cries")
        if os.path.isdir(default_dir):
            files = scan_dir(default_dir)
            print(f"(默认扫描 cries/ 目录, 共 {len(files)} 个文件)")
        else:
            print("请提供音频文件路径, 或用 --batch 扫描目录")
            parser.print_help()
            return

    if not files:
        print("[!] 没有找到音频文件 (支持 wav/mp3/flac/ogg/m4a)")
        return

    print("=" * 70)
    print(" 婴幼儿哭声情绪分析  |  类别: 饥饿 / 疼痛 / 困倦 / 身体不适 / 平静")
    print("=" * 70)

    for fp in files:
        print(f"\n📄  {os.path.basename(fp)}")
        res = analyze_file(fp)
        if res["status"] != "ok":
            print(f"   [!] {res['status']}")
            continue

        f = res["features"]
        print(f"   时长={f['duration']:.2f}s  F0均值={f['f0_mean']:.1f}Hz  "
              f"F0max={f['f0_max']:.1f}Hz  RMS={f['rms_db']:.1f}dB")
        print(f"   节奏周期={f['rhythm_period']:.2f}s  哭声占比={f['cry_ratio']:.1%}")
        print(f"\n   ➜ 情绪: 【{res['emotion']}】  置信度: {res['confidence']:.2%}")
        print(print_result_bar(res["probs"]))

    print("\n分析完成.")


if __name__ == "__main__":
    main()
