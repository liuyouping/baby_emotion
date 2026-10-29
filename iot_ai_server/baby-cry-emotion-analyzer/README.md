# 婴幼儿哭声情绪分析

基于 **librosa 声学特征提取** + **规则引擎** 的婴幼儿哭声情绪识别工具，对 5 类婴幼儿情绪/需求进行启发式分析：**饥饿 / 疼痛 / 困倦 / 身体不适 / 平静**。

✅ 无需预训练模型权重，即开即用
✅ 支持批量目录 / 单段音频 / 麦克风实时录音
✅ 输出终端 ASCII 波形 + 情绪概率条

---

##  目录结构

```
baby-cry-emotion-analyzer/
├── baby_cry_emotion.py       核心脚本
├── requirements.txt          Python 依赖清单
├── run.bat                   Windows 一键启动 (带菜单)
├── README.md                 本文件
├── cries/                    ← 把待分析的哭声音频放到这里
│   └── *.wav / *.mp3 / *.flac
└── recording.wav             (麦克风录音时自动生成)
```

##  快速开始

### Windows（推荐）
双击 `run.bat`，首次运行会自动安装依赖。

### 命令行
```bash
# 安装依赖（仅需一次）
pip install -r requirements.txt

# 1) 批量分析 (默认扫描 cries/)
python baby_cry_emotion.py

# 2) 单段音频
python baby_cry_emotion.py cries\your_cry.wav

# 3) 指定目录
python baby_cry_emotion.py --batch cries

# 4) 麦克风实时录音 5 秒后分析
python baby_cry_emotion.py --mic
```

##  环境要求

- **Python** 3.9 ～ 3.13
- **依赖**：librosa, soundfile, scipy, numpy（pip 自动安装）
- **平台**：Windows / macOS / Linux
- **可选**：麦克风（仅 --mic 模式需要）；ffmpeg（处理 mp3/flac 需要）

##  关于音频

- 支持格式：`.wav` / `.mp3` / `.flac` / `.ogg` / `.m4a` / `.aac`
- 把你的婴幼儿哭声音频丢到 `cries/` 目录即可，**不需要**改代码
- WAV 格式最稳（8kHz 采样率也能处理）

##  工作原理

```
音频 (.wav )
  │
  ▼
librosa 声学特征提取 (14 维):
  ├── F0_mean / F0_std / F0_max      基频 (音调高低/抖动)
  ├── RMS_db / RMS_peak_db / RMS_std 能量 (音量/峰值/起伏)
  ├── ZCR / 频谱质心 / MFCC           音色特征
  ├── cry_ratio / rhythm_period      哭声节奏特征
  └── duration / cry_duration        时长特征
  │
  ▼
规则引擎 (sigmoid + gaussian + softmax, 已用真实+合成音频校准):
  平静 ← F0≈0 或 rms_peak<-35dB
  疼痛 ← F0_max>550 + RMS峰值高 + 能量突发 (rms_std大)
  饥饿 ← F0_mean≈380Hz + 节奏≈0.5s
  困倦 ← F0<280Hz + cry_ratio低 (间歇性)
  不适 ← RMS稳定 + 无明显节奏 + F0≈430Hz
  │
  ▼
终端 ASCII 波形图 + 5 类情绪概率条
```

##  关于置信度

- 真实婴幼儿哭声置信度通常在 **25%~45%**，这是正常现象——不同情绪的哭声音频特征天然有重叠
- 规则引擎是可解释的，每个阈值都基于婴幼儿哭声的声学研究文献值
- 如果有标注好的数据集，可以把 `score_emotions()` 规则引擎替换为训练好的分类器

##  可扩展方向

1. **可视化**：可以把输出改成生成波形图 + 频谱图 PNG（加 matplotlib 即可）
2. **实时监控**：`--mic` 改成循环模式即可做哭声实时检测
3. **训练分类器**：有标注数据时，用提取的 14 维特征训练 XGBoost/MLP，替换规则引擎
