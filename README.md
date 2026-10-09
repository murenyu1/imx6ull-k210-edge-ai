# IMX6ULL + K210 端侧人脸检测终端

K210 运行 KPU 人脸检测，通过 UART 或 USB CDC 发送检测结果，IMX6ULL 上的 Qt 应用展示人脸数量、最高置信度、链路状态及原始帧。Qt 基座来自 [EMIandE3/smart_terminal](https://github.com/EMIandE3/smart_terminal)，保留其基础界面和源码版权注释。

当前实现的是人脸检测与结果展示，没有身份识别或考勤功能。界面沿用“摄像头人脸识别”的名称，进入后选择接口，再点击开始接收。

公开版本已对 MQTT 预设地址脱敏，主机默认值与占位提示统一为 `127.0.0.1`。使用 MQTT 页面前，请填写自己的 broker 地址；本地工程配置保持原样。

## 架构与目录

```text
K210 摄像头 → KPU 人脸检测 → #FACE / #HB 文本帧
                                  │ UART / USB CDC
                                  ▼
IMX6ULL Linux → K210Reader → Qt 检测状态与终端界面
```

| 路径 | 用途 |
| --- | --- |
| `src/k210/k210_vision.py` | CanMV K210 / MicroPython 检测及帧发送脚本 |
| `src/smart_terminal/project/` | Qt 工程、基础界面、硬件逻辑及必要界面资源 |
| `scripts/board/run.sh` | 板端启动模板，处理出厂桌面停止与退出恢复 |

源码包只保留基础项目必要文件。SDK、第三方库、K210 固件和模型、演示媒体需另行准备。源码中的旧文档注释保留原样，相关资料未随本仓库发布；构建与部署所需说明集中在本 README。

## 通信协议

UART 为 **115200、8N1、无流控**；K210 单向发送 ASCII 文本，以 `\n` 结束一帧，当前协议无校验字段。

| 帧 | 字段与发送时机 |
| --- | --- |
| `#FACE,<count>,<max_score>\n` | 每轮检测发送；数量为非负整数，最高置信度为 `0.00`～`1.00`、两位小数 |
| `#HB,<uptime_s>\n` | 每 2000 ms 发送；脚本运行秒数为非负整数 |

例如 `#FACE,1,0.92`、`#FACE,0,0.00`、`#HB,12`，发送时均附换行。实际帧率取决于设备和运行环境，本仓库不承诺固定性能。

- **JP6**：K210 UART1 的 IO7 为发送脚，IMX6ULL 接收节点默认 `/dev/ttymxc2`。接线需按开发板引脚定义确认，并共地。
- **USB_HOST**：K210 Type-C 接 IMX6ULL USB Host，USB CDC 通常枚举为 `/dev/ttyACM0`；实际节点以板端为准。
- Qt 支持 `K210_UART_DEV`、`K210_USB_DEV` 环境变量覆盖两个节点。
- K210 默认 `SEND_CHANNEL = "both"`，同时输出 UART 与 stdout/REPL。USB 通道混有脚本状态信息，接收侧会将非协议行计为非法帧。

## 环境与外部依赖

工程使用 qmake、C++11，`project.pro` 按 `QT_ARCH` 选择 ARM 或 x86。Qt 需要 Core、Gui、Widgets、Network、Multimedia、MultimediaWidgets、Charts、Sql 模块；附属 SQL 页面还需对应 SQL 驱动。QtMqtt 与 Qt 主库的版本、架构及 ABI 必须匹配。

### ARM / IMX6ULL

项目所用工具链为 NXP Yocto Qt SDK `fsl-imx-x11` 4.1.15-2.1.0 / Qt 5.12.9。先安装适合开发板的 SDK；每个新 shell 都要加载工具链。

从上游仓库或自行构建的匹配依赖中准备以下路径，均相对 `src/smart_terminal/project/`：

| 路径 | 必需内容 |
| --- | --- |
| `lib/open_lib/opencv_3.4.1/arm_opencv/include/` | OpenCV 3.4.1 ARM 头文件 |
| `lib/open_lib/opencv_3.4.1/arm_opencv/lib/` | `project.pro` 列出的 11 个 `libopencv_*.so.3.4.1` 库 |
| `lib/arm_qtmqtt/src/mqtt/` | QtMqtt 公开头文件及其相对包含的头文件 |
| `lib/arm_qtmqtt/lib/libQt5Mqtt.so.5.12.9` | 匹配 SDK 的 ARM QtMqtt 库 |

只取所需目录，不必复制整个 `open_lib`。上游 QtMqtt 头文件可能含原作者绝对包含路径；取回后在自己的依赖目录修正为同目录相对包含，例如：

```bash
sed -E -i 's|#include </home/[^>"]*/([^/"]+)>|#include "\1"|g' \
    src/smart_terminal/project/lib/arm_qtmqtt/src/mqtt/*.h
```

从仓库根目录构建：

```bash
source /opt/fsl-imx-x11/4.1.15-2.1.0/environment-setup-cortexa7hf-neon-poky-linux-gnueabi
mkdir -p src/smart_terminal/build-arm
cd src/smart_terminal/build-arm
qmake ../project/project.pro
make -j"$(nproc)"
file project
```

确认 `file` 输出为 ARM ELF 后才部署。没有准备外部依赖的干净克隆无法直接完成构建；其他 SDK 安装路径应使用其对应环境脚本。

### x86 桌面验证（可选）

需要 Qt 5 开发环境及以上模块、`pkg-config` 可发现的 `opencv4` 和 `gtk+-3.0`。Ubuntu 常用依赖为 `build-essential pkg-config qtbase5-dev qtmultimedia5-dev libqt5charts5-dev libqt5sql5-sqlite libopencv-dev libgtk-3-dev`。

另需准备匹配桌面 Qt 的 QtMqtt：公开头文件放在 `lib/x86_qtmqtt/src/mqtt/`，共享库和链接名 `libQt5Mqtt.so` 放在 `lib/x86_qtmqtt/lib/`。若头文件仍有绝对包含路径，按 ARM 示例修正对应目录。上游 Qt 5.12.9 预编译库不能保证适配系统 Qt 5.15；应使用匹配版本的 QtMqtt 自行构建，使用 Qt 私有头构建时还需相应私有开发包。

```bash
mkdir -p src/smart_terminal/build-x86
cd src/smart_terminal/build-x86
qmake ../project/project.pro
make -j"$(nproc)"
LD_LIBRARY_PATH=../project/lib/x86_qtmqtt/lib:"$LD_LIBRARY_PATH" ./project
```

## 最小部署

### K210

从 [01Studio 官方资料下载](https://wiki.01studio.cc/docs/canmv_k210/download/) 获取适合 CanMV K210 开发板的固件、工具及原人脸检测模型。脚本使用 `maix.KPU` 接口，不能假定所有 K210 固件都兼容。模型与加载示例见 [官方人脸检测例程](https://wiki.01studio.cc/docs/canmv_k210/machine_vision/face_detection/)。

准备 `yolo_face_detect.kmodel`：有 MicroSD 时放到卡根目录，脚本读取 `/sd/yolo_face_detect.kmodel`；无卡时用固件下载工具将模型写入 Flash `0x300000`。脚本预设长度为 **278440 字节**，须与实际模型一致，并确认烧录区域不覆盖已有固件。只写模型时不做整片擦除。

通过 CanMV IDE 运行 `src/k210/k210_vision.py`；需要开机运行时按所用固件的 IDE 保存方式部署为 `main.py`。先确认 K210 摄像头检测和帧输出，再连接 IMX6ULL。

### IMX6ULL

将以下内容传到板端：

- ARM 构建产物 → `/home/root/project/project`。
- `scripts/board/run.sh` → `/home/root/project/run.sh`。
- 匹配产物的运行库 → `/home/root/project/lib/`；OpenCV 的运行链接名需满足 `libopencv_*.so.3.4`，QtMqtt 需满足 `libQt5Mqtt.so.5`，不能只复制版本文件而遗漏所需链接名。以实际 ELF 依赖为准，同时保留板端 Qt 库与平台插件。
- 若使用 OpenCV 默认图片页面，`opencv_src/01.jpg` → `/home/root/project/opencv_src/01.jpg`。

在板端执行：

```sh
cd /home/root/project
chmod +x project run.sh
./run.sh
```

模板设置应用目录的库搜索路径和 `QT_QPA_PLATFORM=linuxfb`，停止 `/opt/QDesktop` 后启动应用；默认在退出时恢复桌面。恢复策略可设为 `QDESKTOP_RESTORE_MODE=always`（默认）、`if-running` 或 `never`。选择 `never` 仍会停桌面，需自行恢复。

## 验证与限制

2026-10-09 用户反馈：已部署本项目两个脚本，正常退出后桌面恢复，USB_HOST 连接 K210 使用效果良好。这是用户陈述，未附本轮原始日志、截图、设备版本回读或性能数据，不能据此宣称全部硬件功能通过验收。

- `run.sh` 仍有已知停止失败问题：出厂桌面经停止和强杀后若仍存活，启动段仍会运行应用，可能争抢 framebuffer 与输入设备；本发布保留该基础代码。脚本没有并发单实例保护，请一次运行一个实例。
- stdout 写后计时可以摘除过慢端点；若首次写入永久阻塞，计时防护无法执行，可改 `SEND_CHANNEL = "uart"` 使用排针通道。
- 基座的轮播图、OpenCV、板级设备、音视频、MQTT、摄像头监控及 SQL 界面保留。音乐和视频需在可执行文件同目录准备 `myMusic/`、`myVideo/`；其他页面需对应设备、服务或驱动。
- 只随库提供 OpenCV 默认图片，其他演示图片、视频、标定样本及 SVM 模型需从上游另取。未补齐素材时不要运行相应演示；尤其 x86 手写数字按钮直接加载 `opencv_src/sample/SVM_HOG.xml`，缺失可能抛异常。板端运行时素材路径仍取决于工作目录。
- Python 本机语法检查不证明 MicroPython API、模型加载或硬件正常；Qt 串口回放也不证明 UART 电气、USB 枚举、触摸或板端显示。

## 来源与版权

Qt 基座与其资源来自 [EMIandE3/smart_terminal](https://github.com/EMIandE3/smart_terminal)；K210 检测接口和模型配置参考 01Studio 官方例程。现有源码中的作者及版权声明保留。本仓库未新增统一开源许可证，第三方组件仍适用各自的许可条件。
