# 稚声慧眸 —— 基于 OpenHarmony 的婴幼儿音视频融合情感计算与智能看护系统

基于 **OpenHarmony WS63E 开发板** + **AI 规则引擎** 的婴幼儿智能看护系统。通过摄像头和麦克风采集婴幼儿的面部图像与哭声，在 AI 服务端实时分析其情绪状态（开心 / 困倦 / 烦躁 / 不适 / 平静 / 饥饿 / 疼痛），实现音视频融合的情感计算与智能看护。

---

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                         AI 服务端 (Python)                       │
│                                                                 │
│  ┌──────────────────────┐    ┌──────────────────────┐          │
│  │  面部表情分析           │    │     哭声情绪分析        │          │
│  │  baby-emotion-        │    │  baby-cry-emotion-    │          │
│  │  analyzer/            │    │  analyzer/            │          │
│  │                       │    │                       │          │
│  │  jpeg_server_sle.py   │    │  audio_server_sle.py  │          │
│  │  (TCP 8886 收图片)    │    │  (TCP 8866 收音频)    │          │
│  │  baby_emotion.py     │    │  baby_cry_emotion.py  │          │
│  │  MediaPipe + 规则引擎  │    │  librosa + 规则引擎    │          │
│  └──────────┬───────────┘    └──────────┬───────────┘          │
│             │                            │                      │
└─────────────┼────────────────────────────┼──────────────────────┘
              │  WiFi / TCP                │  WiFi / TCP
              ▼                            ▼
     ┌──────────────────────────────────────────────────────┐
     │                  SLE 网关板 (WS63E)                   │
     │         WiFi TCP relay + SLE 链路透传                 │
     └──────────────────┬──────────────────┬─────────────────┘
                        │ SLE              │ SLE
          ┌─────────────┘                  └─────────────┐
          ▼                                              ▼
  ┌──────────────────┐                          ┌──────────────────┐
  │    摄像头采集板    │                          │    音频采集板      │
  │  camera_code/    │                          │  audio_code/      │
  │  OV2640 摄像头    │                          │  INMP441 麦克风   │
  │  1fps JPEG        │                          │  16kHz WAV        │
  │  (OpenHarmony C)  │                          │  (OpenHarmony C)  │
  └──────────────────┘                          └──────────────────┘
```

**硬件 → AI 服务数据流：**
- 摄像头板采集 JPEG → SLE 链路 → 网关透传 → TCP 8886 → [jpeg_server_sle.py](iot_ai_server/baby-emotion-analyzer/jpeg_server_sle.py) 保存到 `imgs/baby/` → `baby_emotion.py` 分析
- 音频板采集 WAV → SLE 链路 → 网关透传 → TCP 8866 → [audio_server_sle.py](iot_ai_server/baby-cry-emotion-analyzer/audio_server_sle.py) 保存到 `cries/` → `baby_cry_emotion.py` 分析

---

## 目录结构

```
baby_emotion/
│
├── audio_code/                  🎤 音频采集板 (OpenHarmony C)
│   ├── audio_demo.c             INMP441 麦克风采集 + SLE 发送
│   ├── wifi_connect.c/h         WiFi 连接
│   └── BUILD.gn                 编译配置
│
├── camera_code/                 📷 摄像头采集板 (OpenHarmony C)
│   ├── camera.c/h               OV2640 摄像头采集 + SLE 发送
│   ├── wifi_conn.c/h            WiFi 连接
│   └── BUILD.gn                 编译配置
│
├── iot_ai_server/               🤖 AI 服务端 (Python)
│   ├── baby-emotion-analyzer/   📷 面部表情分析
│   │   ├── jpeg_server_sle.py   TCP 服务器 (收网关转发的 JPEG, 端口 8886)
│   │   ├── baby_emotion.py      表情分析核心 (MediaPipe + 规则引擎)
│   │   ├── face_landmarker.task MediaPipe 478 关键点模型
│   │   ├── imgs/baby/           输入: 待分析图片 / 硬件传来的 JPEG
│   │   └── output/              输出: 带标注的结果图
│   │
│   └── baby-cry-emotion-analyzer/  🎤 哭声情绪分析
│       ├── audio_server_sle.py  TCP 服务器 (收网关转发的 WAV, 端口 8866)
│       ├── baby_cry_emotion.py  哭声分析核心 (librosa + 规则引擎)
│       └── cries/               输入: 待分析音频 / 硬件传来的 WAV
│
└── README.md                    本文件
```

---

## AI 分析能力

### 面部表情分析 — 5 类情绪

| 情绪 | 典型面部表现 | 技术原理 |
|------|-------------|---------|
| 😐 平静 | 眉毛舒展、眼睛正常、嘴角水平 | MediaPipe 478 关键点 → 6 个几何特征 → 规则引擎 |
| 😊 开心 | 嘴角上扬、眼睛稍眯 | smile_score ↑ + eye_openness ↓ |
| 😴 困倦 | 眼睛闭合、嘴紧闭 | eye_openness ↓↓ + mouth_openness ↓ |
| 😠 烦躁 | 眉毛内聚、嘴抿或张嘴 | brow_inner_gap ↓ + (mouth_aspect ↓ 或 mouth_openness ↑) |
| 😣 身体不适 | 嘴角下拉、皱眉 | smile_score < 0 + brow_inner_gap ↓ |

### 哭声情绪分析 — 5 类需求

| 需求 | 哭声特征 | 技术原理 |
|------|---------|---------|
| 饥饿 | F0≈380Hz，节奏≈0.5s | librosa 14 维声学特征 → 规则引擎 |
| 疼痛 | F0>550Hz，能量突发 | F0_max + RMS 峰值 + rms_std |
| 困倦 | F0<280Hz，间歇性 | F0_mean ↓ + cry_ratio ↓ |
| 身体不适 | RMS 稳定，无明显节奏 | 稳定能量 + F0≈430Hz |
| 平静 | 无哭声或能量极低 | rms_peak < -35dB |

> 两个 AI 模块均无需预训练模型权重，使用启发式规则引擎即可运行。

---

## 使用方式

### AI 服务端（Python）

```bash
# === 面部表情分析 ===
cd iot_ai_server/baby-emotion-analyzer

# 1) 启动 TCP 服务器，接收硬件摄像头传来的 JPEG（保存到 imgs/baby/）
python jpeg_server_sle.py

# 2) 对收到的图片批量分析（也可单独跑）
python baby_emotion.py                    # 默认扫 imgs/baby/
python baby_emotion.py --camera 0         # 或用本地摄像头实时

# === 哭声情绪分析 ===
cd iot_ai_server/baby-cry-emotion-analyzer

# 1) 启动 TCP 服务器，接收硬件音频板传来的 WAV（保存到 cries/）
python audio_server_sle.py

# 2) 对收到的音频批量分析（也可单独跑）
python baby_cry_emotion.py                # 默认扫 cries/
python baby_cry_emotion.py --mic          # 或用本地麦克风实时录音
```

### 硬件端（OpenHarmony WS63E）

编译环境：基于 **OpenHarmony 5.0** 的 WS63E SDK，在 Ubuntu 中通过 `hb build` 编译。

详细编译配置见各子目录的 `README.md`：
- 摄像头板 → [camera_code/README.md](camera_code/README.md)
- 音频板 → [audio_code/README.md](audio_code/README.md)

**AT 指令配置网关 WiFi / 服务端 IP：**
```
AT+WIFI=<SSID>,<密码>          # 配置 WiFi
AT+SRVIP=<AI服务端IP>,8886    # 配置 JPEG 转发目标（与 audio 共用同一 IP）
AT+SRVIP=<AI服务端IP>,8866    # 配置音频转发目标（默认）
```

---

## 环境要求

### AI 服务端
- **Python** 3.9 ～ 3.13
- 面部分析依赖：`mediapipe`, `opencv-python`, `numpy`
- 哭声分析依赖：`librosa`, `soundfile`, `scipy`, `numpy`
- Windows / macOS / Linux 均可

### 硬件端
- OpenHarmony 5.0 + WS63E SDK
- Ubuntu 编译环境（远程 SSH 到 Harmony 编译机）
- WS63E 开发板 × 2（摄像头板 + 音频板，或单网关板）

---

## 子项目文档

| 模块 | 说明 | 快速链接 |
|------|------|---------|
| 摄像头采集板 | OV2640 JPEG 采集 + SLE 发送 | [camera_code/README.md](camera_code/README.md) |
| 音频采集板 | INMP441 WAV 采集 + SLE 发送 | [audio_code/README.md](audio_code/README.md) |
| 面部表情分析 | MediaPipe + 几何规则引擎，5 类情绪 | [baby-emotion-analyzer/README.md](iot_ai_server/baby-emotion-analyzer/README.md) |
| 哭声情绪分析 | librosa + 声学规则引擎，5 类需求 | [baby-cry-emotion-analyzer/README.md](iot_ai_server/baby-cry-emotion-analyzer/README.md) |
