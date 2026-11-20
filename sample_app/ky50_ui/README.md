# KY50 触控 UI / RGB 实时预览

独立应用 `ky50_ui`，C + LVGL **9.6.0**，适用于当前已联调的 CV1842HP KY50 板、480×854 ST7102 模组、RGB GC2083。SDK 通用应用仍为 `ipcamera/solutions/ipcamera/src/app_ipcam_main.c`；本应用不接管其业务。第二阶段新增工厂测试和可选的板端本地安装／开机恢复，见下文。

**后续开发入口：[ky50_ui 开发指南](DEVELOPMENT.md)**。2026-11-13人脸集成阶段已完成；当前双模态状态、验证边界、线程与数据约定、独立离场配置和新功能接入流程以该指南为准。下文带日期的旧包和性能记录保留其历史范围。

## 产品功能开关与固件

应用顶层 Makefile 中的三个编译宏均默认为 `0`。将 `KY50_ENABLE_FINGERPRINT`、`KY50_ENABLE_FACE` 或 `KY50_ENABLE_PALM` 设为 `1` 可分别启用指纹、人脸或掌静脉。可直接修改默认值，也可在构建时覆盖。例如，在本目录执行 `make -j4 KY50_ENABLE_FACE=1 package` 可生成仅启用人脸的应用包。更换开关后可直接增量编译。算法开启时仍需本机授权和匹配的模型。

全关版保留用户管理、预览、切换镜头及工厂测试。注册方式列表按编译选择显示，人脸双摄校准入口随人脸开关显示。全关版不加载生物识别模型，也不需要设备算法授权。

`cv1842hp_wevb_0014a_spinand` 的 SDK `pack_rootfs` 会编译应用并将运行文件装入 `/system/ky50-ui`。启动脚本 `/etc/init.d/S99zz_ky50_ui` 在媒体驱动加载后调用 `run.sh`，启动日志的前 64 KiB 保存在 `/tmp/ky50-ui-boot.log`。在已构建该板型的 SDK 根目录执行以下命令，可以重新生成固件。若要临时启用某项功能，请在 `pack_rootfs` 前导出对应变量，例如 `export KY50_ENABLE_FACE=1`。

```bash
source build/envsetup_soc.sh
cvi_setup_env && pack_rootfs && pack_upgrade
```

## 已实现的界面

- RGB 画面逆时针 90°旋转，等比例缩放、居中裁剪铺满屏幕。
- 顶部半透明状态栏：每 500 ms 读取 `eth0/carrier`，表示网线物理链路；Wi-Fi 灰显“未启用”。不把 carrier 当作互联网连通性。
- 底部半透明操作区：北京时间（UTC+8）每秒更新，格式为 `2026年11月10日 21:33:34`，时分秒使用单个冒号分隔。系统时间早于 2024 年时显示“北京时间待校准”；不修改系统时区，沿用系统 NTP 校时。
- 数字键盘：0–9、删除、清空、返回，最多 32 位，仅本页交互。
- 门铃：按下反馈和“门铃功能建设中”提示，暂不驱动物理门铃。
- 菜单：**4 行 × 2 列**，图形 + 中文。用户管理接入本机用户列表、添加、人脸/掌静脉注册和批量删除；高级设置进入工厂测试及参数设置；其余六个入口保留占位页，均可返回。
- 本机用户通过独立的掌静脉/人脸 UUID 绑定各自的 PVA 模板；首页识别显示姓名/工号并保存考勤，注册页临时显示 RGB；掌静脉使用 IR，人脸使用 RGB 摄像头的 NV12。IR ISP 和预览固定灰度，检测阈值默认 0.76。数据库、输入法、恢复规则和容量说明见 [USER_DATABASE.md](USER_DATABASE.md)。2026-11-11 历史板测包为 `ky50_session20_20261111`，日志 `/tmp/ky50-session20.log`；下文旧 PVA 包保留为上一阶段记录。
- 菜单、输入页保留实时视频，加深遮罩。触控直接来自 Linux evdev，不需要鼠标。

## 三路 RTSP 工厂测试

进入“菜单 → 高级设置 → 工厂测试”，点击“开启RTSP”，等待按钮变为“关闭RTSP”。默认关闭，不保存开关状态；返回其他页面后继续推流。当前板 IP 为 `192.168.31.83`，远端电脑可同时打开：

| 图像 | 地址 | H.264 输出 |
| --- | --- | --- |
| RGB | `rtsp://192.168.31.83:8554/rgb` | 960×540，目标15 fps，2 Mbps |
| 红外 | `rtsp://192.168.31.83:8554/ir` | 960×540，目标15 fps，2 Mbps |
| DVP | `rtsp://192.168.31.83:8554/dvp` | 640×480，目标15 fps，768 Kbps |

用 VLC 打开网络串流，或分别运行 `ffplay -rtsp_transport tcp rtsp://192.168.31.83:8554/rgb`（另外两个路径替换为 `ir` / `dvp`）。推荐 TCP；流为相机原方向、无屏幕菜单和识别框、无音频。服务绑定板端8554端口，当前工厂测试不配置账号密码，请在受信任测试网络使用。

屏幕预览始终开启，不提供关闭预览按钮。三路相机同时采集；本机切换 RGB/IR/DVP 显示不会切换远端端点的相机身份。开启/关闭需要重建采集链路，本机画面会短暂等待。三路都产生编码帧后才显示开启成功，任一路持续无帧会停止RTSP并恢复普通预览。DVP仍要求已确认的 `swapped` 时钟接线，不改变硬件接线检查。

远端批量验证（依赖本机Python 3、ffmpeg，不保存现场图像）：

```sh
python3 tools/check_rtsp.py 192.168.31.83 --seconds 30
```

实现与板测边界见 [RTSP验证记录](evidence/rtsp/VALIDATION.md) 和 [开发指南](DEVELOPMENT.md)。`--rtsp` 参数可用于诊断时启动即开启；正常启动不需要此参数。

## 构建

以下命令在 `SDK/cvi_mpi/sample_app/ky50_ui` 源码目录执行；NFS 打包目录用于板端运行：

```sh
make -j8              # ARM musl 应用，libpva/libdeviceCerf 静态链接
make kernel           # 触控、继电器、功放兼容模块，不改写内核镜像/Flash
make test             # LVGL 页面回归 + 旋转/坐标/副本保护 ASan/UBSan 测试
make pixel_check      # ARM NEON/标量结果一致性测试，在板端执行
make package          # 打包到 build/deploy/
```

内核构建目录为 `SDK/linux_5.10/build/cv1842hp_wevb_0014a_spinand`；模块必须与板上内核配置和版本匹配。本次板端内核为 `5.10.4-tag--g79534ab146b7`。应用工具链固定为 SDK 自带 ARM GNU 11.3 musl 工具链。`pixels.c` 使用已核对支持 NEON 的 Cortex-A53 ARM32 指令。字体已编入程序，正常构建无需网络、npm、Pillow 或外部字体文件。

`make test` 输出 `build/previews/*.ppm`，这些是合成背景上的 UI 预览。`evidence/home-board.png` 是板上真实 RGB 帧与 UI 的合成结果，不是实屏照片。

## NFS 部署与运行

当前 NFS：主机 `/home/bonoy/NFS_Files` → 板端 `/mnt/data/opt`。将 `build/deploy/` 中的文件复制到 NFS 的 `ky50_ui/`。板端执行：

```sh
# 先释放原显示程序；如当前有对应程序在运行，再执行相应 stop。
/mnt/data/opt/ky50_sensor/stop.sh
/etc/init.d/S99z_ky50_lcd stop

# 前台运行（Ctrl-C 正常退出）
/mnt/data/opt/ky50_ui/run.sh

# 或持续联调，日志写板端，不依赖 NFS：
nohup /mnt/data/opt/ky50_ui/run.sh > /tmp/ky50-ui.log 2>&1 &

# 正常停止
/mnt/data/opt/ky50_ui/stop.sh
```

`run.sh` 检测已有媒体进程；发现冲突会拒绝启动。它先复制 ELF、四个模块、加密模型、配置、音频和停止脚本到 `/tmp/ky50-ui/`，之后从本地执行，PID 为 `/run/ky50-ui.pid`。因此拔网线不会影响 ELF/字体读取，日志也留在本地。`/tmp` 为 RAM 文件系统，重启后需重新部署；本期没有开机服务。停止只发 SIGTERM，不强杀、不卸载 VO 驱动。

恢复原双摄显示：

```sh
/tmp/ky50-ui/stop.sh
/mnt/data/opt/ky50_sensor/run.sh both 0
```

若需卸载本次触控模块，先停止应用，再按 `rmmod ky50_touch`、`rmmod ky50_i2c1` 顺序执行。不要在 UI 运行时卸载。I2C1 桥接模块卸载时恢复原 SDA/SCL pinmux。退出显示后 TP 被原显示清理流程复位，尚未卸载的轮询模块可能报告一次 I2C 读取失败；下一次屏幕初始化会恢复读取。

## 真实触控

原理图及现有显示初始化确认：I2C1，SDA GPIOC20 / SCL GPIOC21（mux 6），TP reset GPIOC18，TP interrupt GPIOC19；LCD reset GPIOC25，背光 GPIOA7。显示和触控复位统一由原 ST7102 初始化管理，触控驱动不重复操作复位或供电。

本机设备树的 I2C1 节点当前为 disabled。为先做 NFS 联调而不刷机：

1. `ky50_i2c1.ko` 用原设备树节点的 MMIO/IRQ/clock/reset 信息创建 I2C1，并保存/切换 C20/C21 pinmux；已有 I2C1 时拒绝重复创建。
2. LCD 完成厂家初始化、释放 TP reset 后，应用加载 `ky50_touch.ko`。
3. 驱动读取 `0x55` 的 **16 位寄存器地址**信息块，核对 480×854 和触点数。实板读回 FW `01`、revision `01.42.01.03`、5 个触点。
4. 每 10 ms 轮询完整触点帧，产生 MT slot、ABS_X/Y、BTN_TOUCH、SYN_REPORT，设备名 `KY50 ST7102 Touchscreen`（本板 `/dev/input/event1`，应用按名称发现）。无新坐标时保持静止按住状态；3 次连续 I2C 错误时释放触点，避免卡住。

当前已通过实际手指四角、中心、长按后抬起、菜单和数字键盘测试，坐标不随摄像头旋转。本版使用轮询，**尚未改为 C19 中断模式**，不假设中断极性。未来固件集成应通过正式 DTS 启用 I2C1/触控节点，并移除临时桥接模块；本次不更改已启动的设备树或烧录固件。

## 视频、显示与生命周期

`camera_port.c` 从原 capture.c 提取已验证的 GC2083 初始化，拆分 SYS/VB 与可切换的 VI/ISP 生命周期。RGB/IR 模式均初始化双摄：pipe0 固定 IR（I2C3 `0x37`，MAC1，lanes 5/4/3），pipe1 固定 RGB（I2C3 `0x3f`，MAC0，lanes 2/1/0），各 30 fps；选择的预览源不会改变算法来源。一个应用独占 SYS/VB/VI/ISP/VO/MIPI，保留 `/tmp/ky50-sensor.lock` 与原显示锁，Get/Release 配对。

显示链路：1920×1080 NV21 → VPSS group0 864×486 → 应用内 CCW90 → 486×864 → 从 `(2,4)` 取 480×854 → NEON BT.601 RGB888 → 检测绘制 → ARGB UI 合成 → VO。保留原屏幕比例和裁剪位置。IR 算法另用 group1 960×540，整帧逆时针旋转到 540×960，不裁剪，仅提取 Y。

本板硬件 GDC 在旧版运行中已卡住；重启后双 group 旋转也复现了异常尺寸和 `gdc op blocked`。当前版本用经过逐像素测试的 NEON 分块转置处理 Y/VU，VPSS 设置 `ROTATION_0`，不再提交 GDC 任务。保留 VPSS 硬件缩放，不更改 BSP 驱动。实际 group0 的 stride 为 896，有效宽 864；group1 stride/有效宽为 960，均按 stride 读取。公共 VI 池 10 块，每个 VPSS group 独立 4 块，VO 独立 3 块。

显示复用完整厂家 ST7102 初始化、DSI payload CRC workaround、HS **4/16/8**，请求 HSA/HBP/HFP=2/40/40、VSA/VBP/VFP=2/9/193，使用实际读回的 HFP=54、pixel clock=36563 kHz。旧资料中的简化 DCS、6/32/1 或旧 porch 不是本版本参数。基线显示日志中 `top=IR bottom=RGB` 属于原双摄提示，本应用实际显示所选单路摄像头。

LVGL 与 evdev 在主线程调度；阻塞 VI/VPSS 等待在采集线程，线程间以带时间戳的最新 RGB 帧交接。UI 的持久 ARGB 缓冲只由 UI 线程写。合成写入当前独占的 VB，提交给 VO 后立即释放用户引用，不再改写该块，等 VB 池重新归还后才复用。

GFBG 选择记录：实板 `/dev/fb0` 存在，ARGB8888、stride 1920，但当前图形层关闭。本 SDK 的 `gfbg_wait_regconfig_work()` 在 40 ms 超时返回 0 时仍报告成功（`osdrv/interdrv/gfbg/platform/linux/gfbg_main.c`），不能据该接口证明翻页已经完成。因此本期选择计划中的 **持久 ARGB + 原 VO 三缓冲合成** 后端，不依赖 GFBG 内置双缓冲。没有宣称已完成 GFBG 硬件透明翻页验收。

## 诊断与验收

```sh
# 可选前台参数
/mnt/data/opt/ky50_ui/run.sh --touch-test --seconds 60

# SIGUSR1：下次提交时保存实际合成帧 /tmp/ky50-ui/screen.ppm
kill -USR1 "$(cat /run/ky50-ui.pid)"

# SIGUSR2：自动 100 轮首页→菜单→八类占位页→菜单→首页
# 自动切换期间不做手指操作，避免干扰。此测试不替代实际手指验收。
kill -USR2 "$(cat /run/ky50-ui.pid)"

# 板端像素一致性测试
/mnt/data/opt/ky50_ui/pixel_check

# 30 分钟监控（先复制到本地，拔网线也继续记录）
cp /mnt/data/opt/ky50_ui/soak.sh /tmp/ky50-ui/soak.sh
nohup sh /tmp/ky50-ui/soak.sh "$(cat /run/ky50-ui.pid)" 1800 > /tmp/ky50-ui-soak.log 2>&1 &
```

`STATS` 每 5 秒记录实际时间间隔计算的采集/提交 fps、帧数、页面和 carrier。`TOUCH_SUBMIT latency_ms` 是内核 evdev 时间戳至提交 VO 的耗时，不含触控扫描与屏幕扫描时间。截图写盘、自动高频切页属于诊断负载，应与静态首页预览性能分开记录。

拔网线应先确认程序已在 `/tmp` 执行，并观察实屏；本次主机未发现可用的串口设备，网络断开期间只能通过实屏观察和板端本地日志验证，插回后再收集 SSH 日志。实测结果及剩余项目见 `evidence/VALIDATION.md`。

## 第二阶段：工厂测试与断电恢复

入口：菜单 → 高级设置 → 工厂测试。屏幕纯色顺序是红、绿、蓝、白、黑、灰；每次触摸抬起切下一色，第六次返回工厂页。全屏不叠加状态栏。

- RGB / IR：切换 GC2083，并打开实时预览页，左上角返回工厂页。确认新 sensor 首帧经过缩放后，才保存选择。失败回滚原摄像头。RGB 与 IR 均 1920×1080、30 fps、逆时针旋转及等比裁剪。
- DVP：GC0328 0x21/8-bit 寄存器，640×480 YUYV，片内 ISP，VI bypass → VPSS → 软件逆时针旋转/居中裁剪，实测约 20 fps。默认开启芯片反相（Page0 `0x43=0x01`），输出白底黑色；独占预览和三路 RTSP 共用该初始化，摄像头重新初始化时自动恢复反相。两块已交换 VIVO_D10/VIVO_CLK 的板均取得真实图像，第一块板用户已确认工厂页点击 DVP 后画面随手指变化正常。必须使用已核实的 `KY50_DVP_CLOCK_WIRING=swapped`；软件关键修正为 VS 低有效、VI 就绪后开启 sensor 输出、CIF 显式 YUYV 输出和 VPSS 640×480 输入尺寸。基础验收见 `evidence/dvp/20261112/`，反相实测与固化见 [开发指南第13节](DEVELOPMENT.md#13-dvp-默认反相2026-11-15)；应用运行验证不等同固件冷启动验收。
- 继电器：GPIOA21 LOCK 高电平吸合，只触发一次；独立内核 delayed_work 在 5 秒后关闭，重复点击不延长，不因离开页面丢失关闭动作，进程退出立即关闭。
- Speaker：TinyALSA PCM 1/0，16 kHz 双声道输出，播放一次“识别成功，请开门”。语音资产源为单声道 PCM，运行时复制到左右声道；播放与 UI 独立线程。AMP 由 `ky50_amp.ko` 修正已刷内核忽略 DT active-low 的行为，GPIOA15 低=播放、高=关断。
- 白光灯 / 红外灯：两个独立开关，白光 `LED_PWM` = GPIOA19（JTAG_CPU_TMS），红外 `ILED-PWM` = GPIOA8（SD0_CMD）；均使用 GPIO mux 3，高电平开、低电平关，不启用 PWM。应用每次启动默认红外常亮、白光关闭；之后可通过工厂按钮独立切换，切页保持状态，不保存灯状态。按钮显示开关动作和当前状态，操作失败显示错误；退出或进程释放设备时两路 GPIO 拉低。红外关闭后仍微亮的硬件问题按用户要求暂不处理。`ky50_lights.ko` 随包加载，设备为 `/dev/ky50-lights`。验证边界见 [灯控验证记录](evidence/lights/VALIDATION.md)。
- **门铃、韦根、MIC、IC、ID 仅按钮**，统一提示未实现，不控制相关硬件。门铃范围已按用户后续指示缩减。

设置文件：`/mnt/cfg/ky50-ui/display-source`，内容为 `RGB\n`、`IR\n` 或 `DVP\n`。采用临时文件、fsync、rename、目录 fsync；缺失或损坏回退 RGB。不会逐帧写 Flash。临时探测 `--source IR` 不自动修改保存值；正常 UI 成功选择才保存。

```sh
# 将 build/deploy 全部文件放在 /mnt/data/opt/ky50_factory 后执行：
/mnt/data/opt/ky50_factory/install-local.sh
# 安装不会打断当前应用，手动切换到已安装版本：
/tmp/ky50-ui/stop.sh
nohup /mnt/system/ky50-ui/boot.sh >/tmp/ky50-ui-boot.log 2>&1 &
```

安装位置 `/mnt/system/ky50-ui`（UBIFS）；在 `/mnt/data/auto.sh` 中加入带边界标记的启动段，并保留原脚本和旧应用备份。`boot.sh` 等待 rcS 退出，再停止已刷固件中的 `S99z_ky50_lcd`，避免两个 VO 所有者。`run.sh` 把程序、模块和音频全部复制到 `/tmp/ky50-ui` 后运行，运行不依赖网络。

`ky50_amp` 是匹配现有内核的兼容模块：模块加载期间原 codec_dac 的左功放 GPIO 控制槽暂时禁用，GPIO request 仍属于 codec，兼容模块独占控制电平；不允许同时解绑或替换 codec_dac。`kernel/codec-dac-polarity.patch` 提供后续正式内核修复，尚未刷入；应用此补丁会改变内部结构布局，**必须移除兼容模块并调整 factory.c 的功放打开路径，不能混用**。

本阶段独立证据记录见 `evidence/FACTORY_VALIDATION.md`；首版 `evidence/VALIDATION.md` 的 30 分钟结果只对应首版构建。

## CV184X 红外手掌实时绘制（2026-11-11）

业务静态库和头文件在 `pva/lib/`、`pva/include/`，模型在 `pva/models/pva.model`。来源为 `SDK/libpva_face/build/cv184x-delivery`；`make pva-sync` 可同步重新构建的交付。应用用 C++ 交叉链接器链接 `libpva.a`、`libdeviceCerf.a` 和 `libsodium.a`，板端还需配套 `libbmrt.so.1.0`、`libbmlib.so.0`、`libatomic.so.1` 和 musl。C++ 标准运行库静态链接。

`pva/pvaConf.json` 使用本地 `/tmp/ky50-ui/pva/pva.model`；授权文件为 `/mnt/cfg/ky50-ui/pva/deviceID.key`。三个最新 fused-input 模型按 tiny、large、small 顺序加密打包；模型在内存解密后加载，不写出明文文件。授权沿用库的现有校验，UI 不自动申请授权，不改请求地址/规则。`device_authorize` 随包交付，首次授权按库说明执行 `device_authorize 1`，也支持指定自定义保存目录。

- 算法只接收 IR pipe0：1920×1080 → 960×540（宽高均减半）→ CCW90 → 连续 Y 平面 540×960。保持 YUV 接口，UV 填 128，库只读 Y 并复制到 RGB 三平面，模型内部完成归一化。
- IR 背景时由同一 VI 帧产生预览与算法输入；实时显示持续推进，叠加最新算法结果（500ms没有新完成结果则隐藏）。RGB 背景时另外从 IR pipe0 取帧检测。不会将 RGB 送给 `pvaGetHandPose`。当前指纹三路模式在DVP预览下仍检测掌静脉/人脸，但不叠加其它镜头的框。
- `HandPoseRes_t draw_copy = pose` 复制整个结果，再生成独立 `HandOverlay`。原始框和关键点仍为算法的 540×960 坐标，后续 `pvaGetFeat` 应继续使用原图和原结果。
- 映射：`x_screen = round(x_algo * 486 / 540 - 2)`，`y_screen = round(y_algo * 864 / 960 - 4)`。比例/偏移取自当前预览 viewport。仅绘制时裁剪越界线条和点。
- 首次进入须连续2帧检测true后才提特征；2026-11-13 起原始true即绘制检测框，确认前为红色；确认后每个true帧都调用`pvaGetFeat`，以`HandPoseRes_t.status`决定框和7个点颜色：OK绿色，其他红色。false帧不继续提特征/业务；显示按下文性能优化新增的3帧/150ms短暂漏检防抖。状态宏名/数值及“红外检测”提示条已移除。非OK不注册/识别；绘制仍使用独立坐标副本，只复制提特征后的status。
- 采集线程独占 VI/VPSS 与相机切换；算法线程独占 PVA/SQLite/曝光反馈，单槽最新帧交接，不积压。主线程独占 LVGL/触控，并对最新预览副本绘制框和点。跨线程输入拥有独立内存，源切换通过 epoch 丢弃旧输入和旧叠层；媒体退出先等待算法线程，避免访问已停用 ISP。

历史状态颜色包为`ky50_quality_20261111`。`ky50_session20_20261111`起已移除两条检测状态文字，RGB背景中的红绿框与关键点直接使用同帧质量结果。识别成功只锁住`pvaGetAuthRes`，`pvaGetFeat`和曝光反馈继续执行。

本次独立 NFS 包：主机 `/home/bonoy/NFS_Files/ky50_pva_20261111`，板端 `/mnt/data/opt/ky50_pva_20261111`。常用命令：

```sh
/tmp/ky50-ui/stop.sh
nohup /mnt/data/opt/ky50_pva_20261111/run.sh >/tmp/ky50-pva.log 2>&1 &
# 临时查看 IR，不改 /mnt/cfg/ky50-ui/display-source 的保存值：
# 先 stop，再执行：
/mnt/data/opt/ky50_pva_20261111/run.sh --source IR
# 仅关闭检测以诊断显示：--no-handpose
# 可选替换配置：--pva-config /absolute/path/to/pvaConf.json
kill -USR1 "$(cat /run/ky50-ui.pid)"
```

SIGUSR1 保存 `screen.ppm`（实际 UI 合成帧），另由下一次检测保存同一帧的 `palm-ir.pgm`（完整 540×960）、`palm-overlay.ppm`（480×854）和 `palm-result.json`（原坐标、viewport、屏幕坐标），全部位于 `/tmp/ky50-ui/`。UI 截图与算法转储分线程触发，不保证相同时间点；算法转储的三个文件互相对应。`PALM_STATS` 每5秒记录IR输入来源、调用数、命中数和检测耗时。`PERF`分开记录取帧、预览、IR处理、帧间隔、检测、提特征/业务及UI耗时，`PALM_QUEUE`记录替换未处理帧的次数。预览采集、屏幕提交和检测更新帧率分别统计。

回退：正常 stop 后运行 `/mnt/data/opt/ky50_factory/run.sh`。本次未执行 `install-local.sh`，未修改 Flash、自启脚本或固件。验证结果见 [evidence/pva/VALIDATION.md](evidence/pva/VALIDATION.md)。

### 参数设置（2026-11-11）

入口：高级设置 → 参数设置。手掌检测阈值和识别相似度阈值范围均为 0.00-1.00，点击加减以 0.01 调整；动态注册等级为 0-4，0 表示不检查动态，数值越大越严格。点击保存后立即生效，返回会放弃尚未保存的修改。

依次调用 `pvaConfigSetHandThreshold`、`pvaConfigSetAuthThreshold`、`pvaConfigSetDynamicDetectionLevel`；全部调用在算法工作线程执行，不与推理并发。三个值作为一组存入本机 `users.sqlite3` 的 `device_meta`（键 `pva.parameters.v1`，阈值按百分位整数保存），启动时恢复。SQLite 持久化失败会恢复旧运行参数并提示失败；注册进行中不允许修改。授权、模型、SDK 公共接口保持原样。

出厂默认 `model_probability_threshold=0.76`、`auth_threshold=0.52`、`dynamic_detection_level=2`。默认配置的 ROI 亮度范围改为 `60-160`，`horizontal_offset_tolerance`、`vertical_offset_tolerance` 均改为 `0.01`。每次启动加载发布包的默认配置，再覆盖上次通过界面保存的三个可调参数；不再依赖 `/tmp` 配置文件保留设置。

### IR 手掌曝光反馈（2026-11-11）

仅在 `pvaGetHandPose=true` 的 IR 帧上检查 `pvaGetFeat` 返回结构体的 status。连续两帧 `STATUS_TOO_BRIGHT` 时降低 IR pipe0 的 AE 目标亮度 `stAuto.u8Compensation`；连续两帧 `STATUS_TOO_DARK` 时提高。初始步长 4，方向反转时减半，最小步长 1；目标限定为 1-255，每次调整后等待 500ms，让 ISP 曝光和缓冲帧更新，再继续注册/识别。其他质量状态不调整，未检出手掌时不读写 ISP，原有非 OK 禁止注册/识别的规则保留。

只使用 `CVI_ISP_GetExposureAttr/SetExposureAttr` 调整 pipe0 的自动曝光目标，保留其他 AE 字段。RGB pipe1、补光灯、PVA 亮度阈值均不参与调节。依据 `Doc/ISPDevelopmentReference_zh.pdf` 的 AE 属性说明，Compensation 是目标亮度，实际快门/增益由 ISP 自动曝光处理。

首页确认识别成功后读取实际 AE 目标（同时记录曝光时间、增益和平均亮度日志），将目标保存至本机 SQLite `device_meta` 的 `pva.ir_ae_target.v1`。仅在值变化时持久化。连续20个有效检测帧返回false后视为下一轮；下一轮连续2帧true确认进入后恢复成功目标。应用重启也读取该记录，但启动和无手掌阶段不下发曝光调整。识别成功保存的是 AE 目标，不锁定快门/增益，以便 ISP 继续适应环境光。

日志前缀 `IR_AE`，其中 luma 是 ISP 全局统计，与 PVA ROI 灰度均值不同。曝光到达范围边界仍不合格时保持边界并绘制红框，不通过修改阈值或光源绕过检查。NFS 本轮包 `ky50_ir_ae_20261111`，日志 `/tmp/ky50-ir-ae.log`。

### 进入滤波与成功锁（2026-11-11）

掌静脉进入确认仍为 `PALM_ENTER_FRAMES=2`。人脸和掌静脉离场阈值分别配置，默认均为连续5次未检测到；少于阈值的漏检不解除成功锁。达到各自阈值后解除该模态会话，掌静脉下一轮再从两帧确认开始。取帧/检测错误打断连续计数，不能当作false累加。屏幕隐藏仍独立按下文显示防抖处理。

识别成功后仍逐帧调用`pvaGetHandPose`和`pvaGetFeat`，更新实时坐标、红绿颜色及曝光反馈；只暂停`pvaGetAuthRes`。成功锁独立于页面、参数保存和时间去重，直到确认离开才解除。原注册流程、质量门控、识别结果和注册进度保留。

日志 `PALM_SESSION` 记录进入/成功锁/离开；每5秒 `PALM_PIPELINE` 输出 features/auth 累计调用数及成功锁状态。长时间举手成功后应看到 features 持续增加，auth 保持不变；确认离开再进入后 auth 才继续增加。

### 手掌出现时的性能优化（2026-11-11）

当前性能版本位于 `/mnt/data/opt/ky50_perf_stable_20261111`，日志 `/tmp/ky50-perf-stable.log`。灰度扩展与图像旋转/合成使用NEON常量通道索引，避免旧实现逐像素计算和每8像素转置的栈读写。算法线程使用普通调度nice=3，预览和UI保持正常优先级，不使用实时调度。

`palm_queue.c`只保留一个待处理红外帧；消费者交换获得独占缓冲，摄像头不能覆盖正在推理的图像。相机跳过的帧不算检测false；掌静脉两帧进入、各模态配置的离场帧数都只按实际检测结果计数。成功后继续提特征/ISP反馈，确认离场前不再识别。绘制仅使用算法结果的副本；异步框会比当前预览晚一个算法处理周期，500ms没有新完成结果时自动隐藏。SIGUSR1仍保留同帧输入和背景的配对转储。

性能基线、优化结果和验证命令见 [evidence/performance/VALIDATION.md](evidence/performance/VALIDATION.md)。回退时正常执行stop，再运行 `/mnt/data/opt/ky50_session20_20261111/run.sh`；数据库、模型、授权和已保存参数沿用原位置。

绘制防闪烁策略（用户已确认）：原始检测false仍立即停止当帧提特征、注册/识别及曝光反馈，只在显示端短暂保留上一份结果副本；连续3次false或从首次false起150ms即隐藏。下一次true会在提特征前结束漏检计时，避免倾斜手掌的较慢旋转/二次推理使框提前消失。看门狗按结果完成时间计500ms，修复原先按输入采集时间计250ms导致的新结果过早失效；输入等待超过250ms仍丢弃，不进行过期输入推理。相机/接口故障及切换立即清空显示，显示防抖不改变各模态独立的业务离场计数。`PALM_QUALITY`表示当次算法结果，`PALM_DRAW`记录实际显示/隐藏变化。

最终性能与防抖版已由用户实机确认正常；实际预览约30fps。记录见 `evidence/performance/VALIDATION.md`，程序SHA256 `1163dfaf316a4de987d17fffd4fece3a87dba0424556377403ea3aaa9e14c23d`。


## RGB 人脸与 IR 掌静脉集成（2026-11-13）

接口依据 `SDK/libpva_face/SDK_INTEGRATION_zh.md`。`init_pva` 一次加载手掌三模型及人脸四模型，退出统一 `deinit_pva`。默认配置开启人脸质量与活体检测；初始化失败会停用识别。

- RGB pipe1：1920×1080 → 960×540 → CCW90 → 540×960 紧密 NV12。逐行处理实际 stride，旋转色度对后将 NV21 的 VU 交换成 UV，保留真实颜色。IR pipe0 保持原有完整半尺寸 Y 输入。
- 采集线程串行复用现有半尺寸 VPSS group，不新增 VB pool；最新帧队列保存各自独立的 IR 和 RGB 算法缓冲。算法持有缓冲直到 Pose/Feat/业务调用全部结束，采集不会覆盖。RGB/IR 预览都采集这两路；当前指纹三路模式在DVP预览下仍采集检测RGB/IR。两路是同一采集轮次取得的帧，不保证硬件同步。
- 同一个算法线程每轮调用 `pvaGetHandPose(IR)` 与 `pvaGetFacePose(RGB_NV12, judgeMode=0)`。原始 hand=true 时只走掌静脉特征/识别；掌静脉质量失败或尚未完成两帧进入确认也不会改做人脸。只有原始 hand=false 且 face=true 才允许人脸特征/识别。IR 取帧/API 错误不视作“无手掌”。
- 两个框和各自关键点独立绘制，可同时显示；复制 Pose 几何后才提特征。人脸仅检测时蓝色，实际执行人脸 Feat 后 OK 为绿色、失败为红色；框的颜色不表示身份已匹配。各自保留 3 次/150ms 漏检绘制防抖和 500ms 完成结果超时，防抖不参与业务判断。映射沿用当前预览的缩放/裁剪，无双目视差校正。
- 用户管理已开放人脸注册，使用同一注册预览页、独立进度与模板库；默认单张采集，保存 3 条双 ROI 模板。注册人脸时需移开手掌。首页人脸识别也经过质量/活体、用户启用状态和连续两次同 UUID 匹配检查，再写 `method=face` 的考勤；成功后等待 20 次原始无脸检测解除保持。掌静脉注册、曝光反馈和成功保持逻辑继续沿用。
- 新人脸库为 `/mnt/system/ky50-data/pva/pvis_face_1000_3tpl_20261113.bin`。不自动导入旧人脸模板布局。两库和 SQLite 的持久化/删除恢复规则见 [USER_DATABASE.md](USER_DATABASE.md)。

`make pva-sync` 同步统一头文件、静态库、两份模型及人脸清单；随后必须 `make -j8 package` 重新链接。`run.sh` 将两份模型暂存本机 `/tmp/ky50-ui/pva`，持久数据仍留在独立目录。`--no-handpose` 作为原有诊断选项会一起关闭两种算法。

构建与回归：`make -j8 TARGET=host all`、`make test`、`make -j8 package`。本沙箱中 LeakSanitizer 无法在 ptrace 下运行，测试使用 `ASAN_OPTIONS=detect_leaks=0`，保留 AddressSanitizer/UndefinedBehaviorSanitizer。新增覆盖真实 UV 旋转、双框绘制、双缓冲并发交接、优先级、活体失败、两种注册/删除恢复、人脸注册触控流程；初版结果存于 `build/face-integration/`。后续独立 UUID 修订的验证结果见 `build/independent-ids/`。

本次仅本地构建与模拟后端回归，尚未部署到板端；真实相机识别、双框对齐、预览帧率、退出重入及触控需要实机确认。前文历史板测结论不代表本次七模型集成已完成板测。包新增约 7 MiB 人脸模型，安装到持久分区前应核对空间；本次未执行安装、自启或刷机。


### 独立模板 UUID（2026-11-13 修订）

`local_id` 是应用资料编号；`palm_uuid`、`face_uuid` 分别存放两种 SDK 模板编号。开始掌静脉注册时调用 `pvaDataBasePickFreeId()`，开始人脸注册时调用 `pvaDataBasePickFreeIdFace()`，各自取得 0～99 的空闲 ID，填入 `User_For_C_t.uuid`。每轮只查询一次，整个采集过程固定该 ID；创建人员资料不分配模板编号。查询不占 SDK 槽位，应用会先持久保存本轮的模态和编号，SDK 成功写入后才完成注册状态。

两个模态的同号可以属于不同人，同一人的两个模板编号也可以不同。识别通过 `(kind, template_uuid)` 回查人员；删除和取消使用该人员实际绑定的各自模板 ID。某个模态满库只阻止该模态的新注册。用户界面仍使用自己的资料编号和工号。

按本次要求不迁移旧注册数据：遇到 SQLite schema v1/v2 时，删除旧人员、凭证、待办、考勤与审计数据，并清空两种 SDK 模板。清空标记持久保存，任一步失败则暂停业务、下次启动继续，完成后进入 schema v3。v3 的正常重启保留新数据；设备参数不随人员数据清空。`run.sh` 不再从旧目录导入模板。

独立 UUID 的 ARM 构建和主机模拟回归已通过，覆盖同号不同人、异号同一人、分别分配/满库、识别与考勤映射、删除中断恢复、取消与槽位复用、旧库一次性清空。尚未上板验证。


## 人脸参数与独立离场设置（2026-11-13）

高级设置 → 参数设置共五页：掌静脉三项、人脸检测/质量、人脸活体/认证、人脸过滤、两种模态离场帧数。点击“上一页/下一页”切换。人脸三页共用一份草稿，“保存人脸”保存全部人脸项；“保存离场设置”只保存两个离场值。未保存的编辑在退出参数页面后丢弃。

| 页面项 | SDK 接口或主应用字段 | 可调范围 |
| --- | --- | --- |
| 人脸检测阈值 | `pvaConfigSetModelProbabilityThresholdFace`，与 `pvaConfigSetFaceThreshold` 同义，只设一项 | 0.01～1.00 |
| 人脸质量检测 | `pvaConfigSetQualityEnableFace` | 关闭/开启 |
| 人脸质量阈值 | `pvaConfigSetQualityThresholdFace` | 0.00～1.00 |
| 人脸活体检测 | `pvaConfigSetLivenessEnableFace` | 关闭/开启 |
| 人脸活体阈值 | `pvaConfigSetLivenessThresholdFace` | 0.00～1.00 |
| 人脸认证阈值 | `pvaConfigSetAuthThresholdFace` | -1.00～1.00 |
| 人脸过滤阈值 | `pvaConfigSetFilterThresholdFace` | -1.00～1.00 |
| 掌静脉离场帧数 | 主应用 `palm_leave_frames` | 1～100，默认5 |
| 人脸离场帧数 | 主应用 `face_leave_frames` | 1～100，默认5 |

阈值步长0.01，帧数步长1。所有项点击保存后由算法线程在帧间处理并立即生效，无需重启。人脸参数通过SDK setter更新，持久值单独写入 `device_meta['pva.face.parameters.v1']`，应用/存储失败回滚，回滚失败则禁用人脸业务并提示。注册期间不允许改参数。质量和活体开关保持当前值，首次使用读取发布配置（均开启）。

离场参数属于主应用，单独写入 `/mnt/system/ky50-data/app.conf`，绝不放入 `pvaConf.json` 或SDK参数记录。文件不存在时生成默认值，保存采用临时文件、fsync及原子重命名；失败不更新运行值。修改离场阈值会从零重新计连续漏检，不直接解除已成功的认证锁。启动读取同一文件保留设置：

```ini
version=1
palm_leave_frames=5
face_leave_frames=5
```

无效或不可读的主应用配置会显示错误并停止识别，避免静默改变已保存的业务策略。原有显示防抖（3次/150ms、500ms结果超时）保持不变；离场帧数统计算法检测结果，不按摄像头采集帧数计数。

指纹集成：DVP 640×480 YUYV 取 Y 并逆时针 90°，独立存在检测后才提特征和匹配。三路认证优先级为指纹、掌静脉、人脸；支持指纹注册/删除和独立 UUID。见 [开发指南 §14](DEVELOPMENT.md) 与 [存在检测验证](evidence/fingerprint/VALIDATION.md)。


### 指纹三路运行性能（2026-11-15）

量产以RGB为预览，DVP用于调试和指纹注册。三路优先级为指纹 > 掌静脉 > 人脸，空指纹不提特征。当前DVP算法输入默认VPSS原尺寸转换＋GDC270°，完整480×640 GRAY8；允许少量硬件灰度差异，失败自动回退软件。RGB/IR预览且RTSP关闭时，非阻塞取DVP，最多100ms缓存复用，重复sample_id不计确认或离场。SDK存在检测用等价NEON统计。

RGB空场景三路实测：采集19.996→21.878fps，检测整轮98.125→94.990ms；相对原双路24.256fps仍有开销。不是成功识别时延或准确率保证。默认`run.sh --source RGB`；诊断回退可设`KY50_FINGER_ROTATION=software`，`compare`只用于120帧像素差异统计。参数、用户库和阈值0.87保留。详细实现及待验收项见[开发指南§14](DEVELOPMENT.md#14-三路指纹集成2026-11-15)与[验证报告](evidence/fingerprint/VALIDATION.md)。
