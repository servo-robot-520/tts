# tts

## 简介

通用 TTS 节点（节点全名 `/audio_tts`）：文字 → WAV，再以 `audio_io_interface/AudioClip`
交给 `audio_io` 播放。节点不打开声卡，也不实现 ASR。

- `libtts`：组合式 `tts::TtsNode`、后端、缓存与音频工具。其他功能包可以链接本库，
  把自己的 `rclcpp::Node` 传进来，共享同一个执行器。
- `tts_node`：只做 ROS 组合与 common_cpp 日志初始化的可执行文件。

后端：

| 后端 | 实现 | 适用场景 |
|---|---|---|
| `piper` | 调用 piper CLI 子进程，stdin 送文本、输出 WAV | 开发机；无 paroli-server 时的兜底 |
| `paroli` | HTTP `POST <url>/api/v1/synthesise`，请求裸 PCM 后写 WAV | RK3566 等板端，由 paroli-server 使用 RKNN/NPU 加速 |

两个后端都使用 Piper 模型；`paroli` 需要 encoder/decoder 分离的 streaming ONNX 模型，
`piper` 使用普通 `.onnx` voice。接口/消息定义见 `tts_interface` 包。

## 接口

| 接口 | 类型 | 说明 |
|---|---|---|
| `~/text` | `tts_interface/msg/TtsText` | 合成并按 `play` 播放；`play=false` 只预热缓存 |
| `~/synthesize` | `tts_interface/srv/Synthesize` | 只合成，返回缓存 WAV 的 `file://` URI |
| `~/say` | `tts_interface/action/Say` | 长文本按句合成，feedback 给进度，可取消 |
| `~/event` | `tts_interface/msg/TtsEvent` | 合成/缓存/播放状态、失败、取消 |

播放策略固定为 `priority = SPEECH`、`group = tts`（可由 `TtsText.group` 覆盖）：
- `~/text`：`REPLACE_SAME_GROUP`，新一句替换旧一句；
- `~/say`：`QUEUE`，逐句按顺序播放。

`~/synthesize` 忽略 `TtsText.play`。`voice_id` 为空表示使用后端默认音色，数字字符串表示
多说话人模型的 speaker id；其他字符串会被拒绝。`rate` 是语速倍率（`length_scale = 1/rate`），
`pitch` 目前不生效（日志会提示）。`volume=0` 使用 `default.volume`。

节点订阅 `<audio_io.node>/event`，把 `AudioEvent.EVENT_STARTED/FINISHED/PREEMPTED/FAILED`
转换成 `TtsEvent.PLAY_STARTED/PLAY_FINISHED/CANCELED/FAILED`；因此播放类事件反映
`audio_io` 的真实结果，而不是提交成功。

## 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `backend` | `piper` | `piper` / `paroli` |
| `piper.executable` | `piper` | 可执行文件或 PATH 中的名字；支持 `~` |
| `piper.model` | 空（必填） | `.onnx` voice；支持 `~` |
| `piper.config` | 空 | 空 = `<model>.json` |
| `piper.espeak_data` | 空 | espeak-ng-data 目录 |
| `piper.speaker` | `-1` | 默认 speaker id |
| `paroli.url` | `http://127.0.0.1:8848` | paroli-server 根地址 |
| `paroli.sample_rate` | `22050` | 必须与 paroli 模型原生采样率一致 |
| `paroli.speaker_id` | `-1` | 默认 speaker id |
| `backend.timeout_ms` | `30000` | 单次合成超时；超时会杀子进程/中止 HTTP |
| `audio_io.node` | `/audio_io` | 播放节点全名 |
| `frame_id` | `audio` | AudioClip/TtsEvent 的 frame_id |
| `default.group` | `tts` | 默认音频焦点组 |
| `default.volume` | `1.0` | `volume=0` 时的线性增益 |
| `max_sentence_chars` | `120` | `~/say` 单句最大码点数 |
| `cache.dir` | `~/.cache/robot_audio/tts` | 缓存目录 |
| `cache.max_bytes` | `268435456` | LRU 上限；0 = 不限制 |
| `log.*` | common_cpp 默认 | `log_level` / `log_path` / `async` |

缓存键是 `SHA-256(backend 指纹 + text + voice_id + language + rate + pitch)`，命中会刷新
mtime；超过 `cache.max_bytes` 时按 mtime 从旧到新删除。

## 本机运行（piper）

```bash
source /opt/ros/humble/setup.bash
colcon build --base-paths src --packages-select tts
source install/setup.bash

# 下载 piper 和默认中文音色到 ~/.local/share/tts
install/tts/share/tts/tools/install_piper.sh
# 可选：换成英文音色
# install/tts/share/tts/tools/install_piper.sh en_US-lessac-medium

# audio_io 可以用 null 后端在无声卡环境验证整条链路
ros2 run audio_io audio_io_node --ros-args \
  -p backend:=null -p capture.enabled:=false

ros2 launch tts tts.launch.py

# 只合成
ros2 service call /audio_tts/synthesize tts_interface/srv/Synthesize \
  "{request: {text: '你好，我是小鸭', language: zh-CN}}"

# 合成并播放（bool 没有“未设置”，必须显式 play: true）
ros2 topic pub --once -w 0 /audio_tts/text tts_interface/msg/TtsText \
  "{text: '你好，我是小鸭', play: true}"

# 长文本
ros2 action send_goal /audio_tts/say tts_interface/action/Say \
  "{text: {text: '第一句。第二句。', play: true}}" --feedback

ros2 topic echo /audio_tts/event
ros2 topic echo /audio_io/state
```

## 板端运行（paroli + RKNN）

1. 按 [paroli](https://github.com/marty1885/paroli) 的说明在板端编译 `paroli-server`，
   RK3566 可参考 `paroli-on-orangepi` 分支的 NPU 适配；
2. 准备 streaming 模型：`encoder.onnx` / `decoder.onnx` / `model.json`；
3. 启动：

```bash
./paroli-server --encoder encoder.onnx --decoder decoder.onnx \
  -c model.json --ip 127.0.0.1 --port 8848
```

4. 覆盖节点参数：

```bash
ros2 run tts tts_node --ros-args \
  -p backend:=paroli \
  -p paroli.url:=http://127.0.0.1:8848 \
  -p paroli.sample_rate:=22050
```

`paroli` 返回裸 16-bit little-endian PCM；`paroli.sample_rate` 必须与模型一致，
否则播放速度会不对。paroli-server 不可达时，请求会以 `success=false` / `~/event FAILED`
返回，节点本身保持运行。

## 构建与测试

```bash
source /opt/ros/humble/setup.bash
CC=/usr/bin/gcc-13 CXX=/usr/bin/g++-13 \
  colcon build --base-paths src --packages-select tts
colcon test --base-paths src --packages-select tts
colcon test-result --test-result-base build/tts/test_results --verbose
```

依赖：`rclcpp`、`rclcpp_action`、`common_cpp`、`audio_io_interface`、`tts_interface`、
系统 `libcurl` 与 `libssl`（OpenSSL 用于缓存键 SHA-256）。

## 已知限制

- 不发布 `AudioStream`，所有播放都走 `AudioClip` + 文件缓存；首句延迟等于一次完整合成。
- 不支持 `pitch`；需要变调时应在后端外部做重采样/变调。
- 没有 eSpeak/云后端，也没有自动 fallback；`piper`/`paroli` 由参数显式选择。
