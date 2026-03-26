# AGENTS.md - xiaozhi-esp32 开发指南

## 项目概述

这是一个基于 ESP-IDF 的 AI 语音聊天机器人固件项目，支持多种 ESP32 芯片（ESP32、ESP32-S3、ESP32-C3、ESP32-P4 等）和 70+ 种开源硬件开发板。

## 构建系统

### 环境要求

- ESP-IDF SDK v5.4 或更高版本
- Python 3.x
- CMake 3.16+

### 构建命令

```bash
# 设置 ESP-IDF 环境（Linux/macOS）
source $IDF_PATH/export.sh

# 设置 ESP-IDF 环境（Windows）
%IDF_PATH%\export.bat

# 配置项目（选择开发板和功能）
idf.py menuconfig

# 编译项目
idf.py build

# 烧录固件
idf.py -p /dev/ttyUSB0 flash

# 监控串口输出
idf.py -p /dev/ttyUSB0 monitor

# 一键编译+烧录+监控
idf.py flash monitor

# 清理构建
idf.py fullclean

# 生成合并的二进制文件
idf.py merge-bin
```

### 构建特定开发板

使用 `scripts/release.py` 脚本可以构建特定开发板：

```bash
# 列出所有支持的开发板
python scripts/release.py --list-boards

# 构建特定开发板
python scripts/release.py <board-name> --name <variant-name>

# 示例：构建 bread-compact-wifi
python scripts/release.py bread-compact-wifi --name wifi
```

### 配置选项

项目通过 Kconfig 进行配置，主要配置文件：
- `sdkconfig.defaults` - 默认配置
- `sdkconfig.defaults.<chip>` - 特定芯片配置（如 `sdkconfig.defaults.esp32s3`）

关键配置项：
- `CONFIG_BOARD_TYPE_*` - 开发板类型
- `CONFIG_LANGUAGE_*` - 界面语言
- `CONFIG_USE_AUDIO_PROCESSOR` - 音频处理开关
- `CONFIG_USE_DEVICE_AEC` / `CONFIG_USE_SERVER_AEC` - 回声消除模式

## 测试

这是一个嵌入式硬件项目，没有传统的单元测试框架。测试主要通过：

1. **硬件测试**：在实际设备上运行并验证功能
2. **串口监控**：使用 `idf.py monitor` 查看日志输出
3. **CI 构建验证**：GitHub Actions 自动构建所有开发板变体

```bash
# 查看串口日志
idf.py monitor

# 使用过滤器查看特定标签的日志
idf.py monitor | grep "Application"
```

## 代码风格

### 标准

项目使用 **Google C++ 代码风格**，通过 `.clang-format` 文件定义。

### 格式化

```bash
# 格式化单个文件
clang-format -i main/application.cc

# 格式化整个目录
find main -name "*.cc" -o -name "*.c" -o -name "*.h" | xargs clang-format -i
```

### 关键风格规则

- **缩进**：4 个空格，不使用 Tab
- **行宽限制**：100 字符
- **大括号**：K&R 风格（左大括号不换行）
- **指针对齐**：左对齐（`int* ptr`）
- **命名约定**：
  - 类名：`PascalCase`（如 `Application`、`AudioCodec`）
  - 函数名：`PascalCase`（如 `GetInstance`、`Initialize`）
  - 成员变量：`snake_case_` 带下划线后缀（如 `event_group_`、`button_handle_`）
  - 常量：`kPascalCase`（如 `kAecOnDeviceSide`）
  - 枚举值：`kPascalCase`（如 `BUTTON_PRESS_DOWN`）
  - 宏：`UPPER_SNAKE_CASE`（如 `CONFIG_BOARD_TYPE`）
  - 文件名：`snake_case.cc`（如 `audio_codec.cc`）

### 导入/包含顺序

按照 `.clang-format` 中 `IncludeCategories` 定义的优先级：

1. ESP-IDF 头文件（`<esp_*.h>`）
2. 驱动头文件（`<driver/*.h>`）
3. 其他系统头文件（`<*.h>`）
4. C++ 标准库（`<*>`）
5. 项目本地头文件（`"*.h"`）

示例：
```cpp
#include "application.h"      // 项目头文件
#include "board.h"

#include <cstring>            // C++ 标准库
#include <esp_log.h>          // ESP-IDF
#include <cJSON.h>            // 第三方库
#include <driver/gpio.h>      // 驱动
```

### 错误处理

- 使用 ESP-IDF 的错误检查宏：`ESP_ERROR_CHECK()`、`ESP_LOGx()`
- 定义 `TAG` 宏用于日志标签：`#define TAG "ModuleName"`
- 日志级别：`ESP_LOGE`（错误）、`ESP_LOGW`（警告）、`ESP_LOGI`（信息）、`ESP_LOGD`（调试）、`ESP_LOGV`（详细）

```cpp
#define TAG "MyModule"

void my_function() {
    esp_err_t ret = some_esp_function();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Function failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Operation successful");
}
```

## 项目结构

```
xiaozhi-esp32/
├── main/                    # 主要源代码
│   ├── application.cc/h     # 应用主逻辑
│   ├── main.cc              # 入口点
│   ├── audio/               # 音频处理
│   │   ├── codecs/          # 音频编解码器
│   │   ├── processors/      # 音频处理器
│   │   └── wake_words/      # 唤醒词检测
│   ├── boards/              # 开发板支持
│   │   ├── common/          # 通用板级代码
│   │   └── <board-name>/    # 特定开发板实现
│   ├── display/             # 显示驱动
│   │   └── lvgl_display/    # LVGL 显示实现
│   ├── led/                 # LED 控制
│   ├── protocols/           # 通信协议（WebSocket、MQTT）
│   └── CMakeLists.txt       # 组件构建配置
├── docs/                    # 文档
├── scripts/                 # 构建和工具脚本
├── partitions/              # 分区表定义
├── sdkconfig.defaults*      # SDK 默认配置
├── CMakeLists.txt           # 项目根构建文件
└── .clang-format            # 代码格式化配置
```

## 开发工作流

### 添加新开发板

1. 在 `main/boards/` 下创建新目录
2. 实现继承自 `WifiBoard` 或 `Ml307Board` 的板级类
3. 在 `main/CMakeLists.txt` 添加板型配置
4. 在 `Kconfig.projbuild` 添加配置选项
5. 添加对应的 `sdkconfig.defaults.<board>` 文件

### 添加新功能

1. 遵循现有模块的目录结构
2. 使用项目的命名约定
3. 添加适当的日志标签和错误处理
4. 更新相关的 CMakeLists.txt

### 代码提交

- 提交前运行 `clang-format` 格式化代码
- 确保 `idf.py build` 编译成功
- 在目标硬件上测试功能
- CI 会自动验证所有开发板的构建

## 常见问题

### 编译错误：找不到头文件

确保 `CMakeLists.txt` 中正确添加了 `INCLUDE_DIRS` 和 `PRIV_REQUIRES`。

### 内存不足

检查 `sdkconfig` 中的堆栈大小配置，ESP32 内存有限，需谨慎分配。

### 多板卡支持

使用条件编译 `#ifdef CONFIG_BOARD_TYPE_*` 来处理不同开发板的差异。

## 相关资源

- [ESP-IDF 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/)
- [项目文档](docs/)
- [开发者文档](https://ccnphfhqs21z.feishu.cn/wiki/F5krwD16viZoF0kKkvDcrZNYnhb)
