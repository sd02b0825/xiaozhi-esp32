# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

小智 AI 聊天机器人项目 - 基于 ESP32 的智能语音交互设备，支持大模型语音对话、设备控制、表情显示等功能。项目使用 ESP-IDF 框架开发，支持多种硬件平台和通信协议。

## 开发环境

### 必需工具
- ESP-IDF v5.4 或更高版本
- Cursor 或 VSCode（安装 ESP-IDF 插件）
- Python 3.8+

### 推荐平台
- Linux 系统（编译速度快，驱动问题少）
- Windows 可用但可能遇到驱动问题

### 代码风格
- 使用 Google C++ 代码风格
- 提交代码前请确保格式规范

## 构建和烧录

### 基本构建命令
```bash
# 配置构建目标（首次设置）
idf.py set-target esp32s3  # 或 esp32, esp32c3, esp32p4 等

# 构建项目
idf.py build

# 烧录到设备
idf.py -p COM3 flash monitor  # Windows 使用 COM 端口
idf.py -p /dev/ttyUSB0 flash monitor  # Linux 使用设备路径
```

### 开发板配置
通过 `main/Kconfig.projbuild` 选择开发板类型：
```bash
idf.py menuconfig
# 选择 Component config → Xiaozhi Assistant → Board Type
```

### 分区表选择
- v2 版本：支持动态资源加载，有独立的 assets 分区
- v1 版本：传统分区表，可通过 `git checkout v1` 切换

## 架构设计

### 核心组件

1. **应用层 (Application)**
   - `main/application.cc` - 主应用逻辑，状态机管理
   - `main/device_state_machine.cc` - 设备状态管理

2. **硬件抽象层 (Board)**
   - `main/boards/common/` - 通用硬件抽象
   - `main/boards/[board_type]/` - 特定开发板实现
   - 支持多种显示、音频、网络模块

3. **音频服务**
   - `main/audio/` - 音频编解码、处理、流式传输
   - 支持 OPUS 编码、多种音频芯片
   - 集成 ESP-SR 离线唤醒

4. **显示系统**
   - `main/display/` - OLED/LCD 显示控制
   - 集成 LVGL 图形库
   - 支持自定义主题、表情包

5. **通信协议**
   - `main/protocols/` - WebSocket 和 MQTT 协议实现
   - 支持实时语音流传输

6. **MCP 服务器**
   - `main/mcp_server.cc` - 设备端 MCP 协议实现
   - 支持设备控制和云端能力扩展

### 关键特性

- **多硬件支持**：70+ 种开源硬件，自动适配引脚配置
- **多语言支持**：支持中、英、日等 30+ 种语言
- **动态资源**：v2 版本支持网络加载唤醒词、主题等资源
- **音频处理**：支持回声消除、噪声抑制
- **低功耗模式**：可调节电源管理级别

## 重要配置

### SDK 配置
- 默认使用 SPIRAM 优化版本
- LVGL 启用压缩字体以节省空间
- 关闭不必要功能以减小固件大小

### 资源管理
- 使用 assets 分区存储动态内容
- 支持自定义 assets.bin 替换默认资源
- 通过 `scripts/build_default_assets.py` 生成资源包

### 网络配置
- 支持 WiFi 热点配置、声波配置、Blufi 配置
- 支持 ML307 Cat.1 4G 模块
- 自动网络检测和重连

## 开发指南

### 添加新开发板
1. 在 `main/boards/` 下创建新目录
2. 实现 Board 接口类
3. 添加配置文件和引脚定义
4. 在 `Kconfig.projbuild` 中添加选项

### 自定义音频处理
1. 在 `main/audio/codecs/` 添加新的编解码器
2. 在 `main/audio/processors/` 添加音频处理模块
3. 通过 Kconfig 启用新功能

### 扩展 MCP 协议
1. 在 `mcp_server.cc` 添加新的 MCP 工具
2. 实现设备控制或云端能力
3. 更新 MCP 协议文档

## 常见问题

### 编译错误
- 检查目标芯片配置是否匹配开发板
- 确保 ESP-IDF 版本正确
- 查看日志中的具体错误信息

### 烧录失败
- 检查串口权限（Linux 可能需要 `sudo`）
- 确认 Boot 模式（正常烧录需进入 Boot 模式）
- 尝试手动复位设备

### 设备无法连接
- 检查 WiFi 配置是否正确
- 确认服务器地址是否可达
- 使用串口查看调试日志

## 版本说明

- 当前版本：v2.2.4
- v2 与 v1 分区表不兼容，需手动烧录升级
- v1 分支维护至 2026 年 2 月