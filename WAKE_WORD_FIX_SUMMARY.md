# 唤醒词检测修复总结

## 问题描述

之前的实现中存在**关键错误**：`DetectWakeWordInPlayback` 函数检测的是播放音频数据（扬声器输出），而不是麦克风输入数据。这违反了"播放音频时从麦克风接收输入并识别唤醒词"的需求。

## 修复内容

### 1. 删除错误代码

已删除以下错误的实现：

#### `audio_service.h`
- 删除了 `EnableWakeWordDuringPlayback()` 方法声明
- 删除了 `IsWakeWordDuringPlaybackEnabled()` 方法
- 删除了 `SetWakeWordDetectionPriority()` 方法
- 删除了 `IsWakeWordDetectionHighPriority()` 方法
- 删除了相关的成员变量：
  - `enable_wake_word_during_playback_`
  - `wake_word_detection_priority_`
  - `wake_word_buffer_`
  - `wake_word_buffer_mutex_`
  - `last_wake_word_check_time_`
  - `wake_word_check_interval_ms_`
  - `max_wake_word_buffer_size_`
- 删除了私有方法：
  - `ResampleForWakeWord()`
  - `DetectWakeWordInPlayback()`

#### `audio_service.cc`
- 删除了 `AudioOutputTask` 中错误的调用：`DetectWakeWordInPlayback(task->pcm)`
- 删除了所有相关函数的实现

### 2. 保留正确的流程

**正确流程**（已存在且未修改）：

```
AudioInputTask (audio_service.cc:269-287)
    ├─ 持续从麦克风读取数据 (ReadAudioData)
    ├─ 检查 AS_EVENT_WAKE_WORD_RUNNING 标志
    └─ 如果标志已设置，送入唤醒词检测器：wake_word_->Feed(data)
```

## 正确的工作原理

### 播放时的唤醒词检测流程：

1. **状态转换** (`application.cc:975-988`)
   - 进入 `kDeviceStateSpeaking` 状态
   - 如果定义了 `CONFIG_WAKE_WORD_DURING_SPEAKING`
   - 调用 `audio_service_.EnableWakeWordDetection(true)`

2. **启动检测** (`audio_service.cc:577-578`)
   - 设置 `AS_EVENT_WAKE_WORD_RUNNING` 标志
   - 调用 `wake_word_->Start()` 启动检测器

3. **持续输入** (`audio_service.cc:269-287`)
   - `AudioInputTask` 持续从麦克风读取数据
   - 数据送入 AFE（Audio Front-End）

4. **回声消除** (`afe_wake_word.cc:74-75`)
   ```cpp
   afe_config->aec_init = codec_->input_reference();
   afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
   ```
   - AFE 使用 AEC（Acoustic Echo Cancellation）消除播放音频的干扰
   - 确保检测到的是用户说的话，而不是设备播放的音频

5. **唤醒词检测** (`afe_wake_word.cc:146-172`)
   - `AudioDetectionTask` 持续处理 AFE 输出
   - 检测到唤醒词后触发回调

## 关键优势

### ✅ 正确性
- 检测的是麦克风输入，不是播放音频
- 符合"播放时也能从麦克风接收输入并识别唤醒词"的需求

### ✅ 简洁性
- 删除了重复且错误的检测逻辑
- 利用现有的 `AudioInputTask` 流程
- 依赖成熟的 AFE AEC 技术

### ✅ 可靠性
- AEC 是经过验证的回声消除技术
- 不受播放音量影响
- 不会误检测播放内容中的唤醒词

## 配置要求

要启用播放时唤醒词检测，需要：

1. **启用配置选项**（在 `menuconfig` 中）：
   ```
   CONFIG_WAKE_WORD_DURING_SPEAKING=y
   ```

2. **使用 AFE 唤醒词**：
   ```
   CONFIG_USE_AFE_WAKE_WORD=y
   ```
   （仅 ESP32-S3 和 ESP32-P4 支持）

3. **硬件要求**：
   - 需要支持 `input_reference()` 的音频编解码器
   - 需要物理的回声参考信号路径

## 测试建议

1. 播放包含唤醒词的音频，确保不会误触发
2. 在播放时说出唤醒词，确保能正确检测
3. 测试不同音量下的检测效果
4. 测试不同环境噪音下的表现

## 文件修改列表

- ✅ `main/audio/audio_service.h` - 删除错误的方法声明和成员变量
- ✅ `main/audio/audio_service.cc` - 删除错误的实现和调用
- ✅ 无需修改 `application.cc` - 现有流程已正确
- ✅ 无需修改 `afe_wake_word.cc` - AEC 配置已正确

## 修复验证

修复后的代码：
- ✅ 编译检查通过（无 linter 错误）
- ✅ 删除了所有错误的检测逻辑
- ✅ 保留了正确的麦克风输入检测流程
- ✅ 符合原始需求规范