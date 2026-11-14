# KY50 双目 GC2083 取图与屏幕显示

2026-11-10 已通过 SSH + NFS 在 CV1842HP / KY50 上验证。
用户确认：“两路画面正常，会随镜头变化，无红线或抖动”。
屏幕上方为 IR，下方为 RGB，两个 1920×1080 图像分别等比例缩小为 480×270。
同日进一步修正了第二路 ISP Run 遗漏，用户再次确认：红外补光灯开启时 RGB 正常。

本目录是独立 NFS 调试程序。没有修改板型配置、烧录镜像或安装开机服务。
屏幕使用当天已经实物验证的完整 ST7102 初始化、长包 CRC 用户态修正和 HS 4/16/8。

## 硬件依据与实际配置

依据 `Doc/KY50-V1.0(1).pdf` 第 2、5、7 页、
`Doc/sensor_doc/GC2083 CSP Datasheet Release Preliminary V0.2.pdf` 第 19、24、30 页，
以及 `Doc/CV184X_驱动调试方法与注意事项.md`。

| 项目 | IR | RGB |
| --- | --- | --- |
| 程序 VI pipe | 0 | 1；单路测试时为 0 |
| 实测 I2C 总线/7 位地址 | I2C3 / `0x37` | I2C3 / `0x3f` |
| ID 寄存器 | `0x03f0=0x20`, `0x03f1=0x83` | 同左 |
| RESET | E4 / CAM_PD1 / GPIOA4，低有效 | E5 / CAM_RST0 / GPIOA2，低有效 |
| PWDN | C12 / USB_VBUS_EN / GPIOB5，测试时输出低 | C15 / GPIO_RTX / GPIOB23，测试时输出低 |
| 模组参考时钟 | REFCLK0 → SoC CAM_MCLK1 / D3 | REFCLK1 → SoC CAM_MCLK0 / C4 |
| 实际配置 MCLK | 24 MHz | 24 MHz |
| CSI 接收 MAC | 1 | 0 |
| CSI clock / data0 / data1 | RX5 / RX4 / RX3 | RX2 / RX1 / RX0 |
| 完整 lane 数组 | `5,4,3,-1,-1,-1` | `2,1,0,-1,-1,-1` |
| P/N 交换 | 全 0 | 全 0 |
| sensor 模式 | 1920×1080，30 fps，RAW10，线性 | 同左 |
| ISP 输出 | NV21，1920×1080，无压缩 | 同左 |

两路共用 I2C3：SCL 为 D4 / GPIOA5，SDA 为 F4 / GPIOA6，复用功能均为 0。
程序按 GPIO 控制器标签申请 GPIO，保存和恢复 pinmux，不依赖 Linux 动态全局 GPIO 编号。
图像来自镜头；程序直接链接 GC2083 驱动，没有 ini 查找失败后退回内部测试图的路径。

## 本次修正与定位

1. `build/media/SensorSupportList/gcore_gc2083/gc2083_cmos.c` 的备用地址原为 `0x7e`，
   这是规格书的 8 位写地址。底层 I2C 使用 7 位地址，已修正为 `0x3f`。
   先单独释放 IR、RGB 的复位，再同时释放，均实测确认地址与 ID，不靠总线盲扫推断。
2. 当前 `.config` 没有启用 GC2083。本目录 Makefile 直接编译该 sensor 的两份源文件和
   `sensor_i2c.c`，不修改全局配置，也不替换公共 `libsns_full.a`。
3. 初次 IR 试验将 RX3/4/5 绑定 MAC0，在已经配置双路 PHY 的状态下没有首帧。
   按双路硬件结构改为 IR→MAC1、RGB→MAC0 后，IR 单路和双路均出帧。
   VI pipe 编号和 CSI MAC 编号不是同一个概念，见上表。
4. 当前 SDK 的 lane 数组长度为 6，所有未使用项明确填 `-1`。
   本程序直接设置每路回调属性，避免通用配置路径只复制前 5 项而使末项残留为 0。
5. 原调试程序仅调用 `CVI_ISP_Run(0)`，误以为共享事件循环即自动启用全部管线。
   当前 Linux `libisp.a` 需要每路分别调用 Run，设置每路的运行标志；事件处理会跳过
   未启用的管线。现在按 `sample_app/sensor/sample_sensor.c` 为每路启动 Run，并在退出时
   逐路 Exit / join。共享事件循环只由其中一个线程执行，另一调用返回成功属于正常行为。

## 红外补光触发 RGB 黑屏的修正

初版两路均约 30 fps，但开独立供电的红外灯时 RGB 变黑，关灯恢复。系统重启不能消除。
黑图来自 ISP 输出 NV21：RGB Y 均值约 1.32/255，99.95% 的像素 Y≤16；CSI 错误为零。
加入可选曝光日志后发现，RGB `CVI_ISP_QueryExposureInfo(1)` 返回成功，但曝光、增益、
亮度全部为零，RGB sensor 曝光固定 70 行、增益 1 倍。说明出帧不等于该路 AE 正常运行。

补全每路 Run 后，红外灯开启时 RGB AE 恢复有效结果，实测曝光约 33096 us、模拟增益
约 4.35 倍，NV21 Y 均值约 150/255；用户确认下半屏 RGB 正常。IR 曝光约 4800 us、
增益 1 倍，两路各自更新。该对照支持本次黑屏由不完整的双路 ISP 启动触发，不能仅根据
“开红外灯变黑”推断硬件接触不良或 RGB 滤光片问题。

诊断开关只读取曝光结果及抽样亮度，不设置 AE 参数：

```sh
KY50_SENSOR_DIAG=1 ./run.sh
```

每 150 帧输出 `DIAG IR/RGB`，包含 Ymean、AE 返回值、曝光、增益及统计亮度。
`evidence/display-exposure-before-isp-fix.log` 保存修正前零值 AE，
`evidence/display-isp-fix.log`、`regs-isp-fix-lamp-on.log` 和
`stable-isp-fix-lamp-on.log` 保存修正后的开灯验证。

## 编译

在主机 SDK 根目录执行，保留现有板型配置：

```sh
cd /home/bonoy/CV184X_WS/SDK
source build/envsetup_soc.sh
export CHIP=cv1842hp BOARD=wevb_0014a_spinand
cvi_setup_env
make -C cvi_mpi/sample_app/ky50_sensor
```

使用 ARM musl 工具链，`-Wall -Wextra -Werror` 编译通过。链接当前工作区的
`libmipi_tx.a`，其中已有按实际 DSI lane 数计算传输时间的修正。
`display.c` 复用 `../ky50_lcd/sample_ky50_lcd.c` 中已验证的屏幕实现，不修改原字符测试。

已验证采集/显示二进制 SHA-256：

```text
3e752c37764f357cee17fd463476d41fb5b2e98359f999da012edef28a313b59
```

主机部署目录为 `/home/bonoy/NFS_Files/ky50_sensor`，板端映射为 `/mnt/data/opt/ky50_sensor`。
部署新二进制前先正常停止旧实例，再写 `.new` 文件并重命名；不要覆盖正在执行的 NFS 文件。

## 启动与停止

板端前台启动，Ctrl-C 正常退出：

```sh
cd /mnt/data/opt/ky50_sensor
./run.sh
```

默认等价于 `./ky50_sensor_capture both 0 --display`：双目持续显示。
`./run.sh ir 300` 或 `./run.sh rgb 300` 可单路显示 300 帧。
已有实例时脚本会拒绝重复启动。旧 Flash 的字符服务会先正常停止；若另外有 NFS 字符
测试正在运行，脚本会明确报出其 PID，应正常停止后再启动 sensor 程序。

后台启动（先确认旧实例已停止，日志名按本次实验区分）：

```sh
nohup ./run.sh >display-default.log 2>&1 </dev/null &
cat capture.pid
tail -20 display-default.log
./stop.sh
```

`stop.sh` 先核对 `/proc/PID/exe`，只向本目录的采集程序发送 SIGTERM。
采集程序退出时先释放显示，再停止 VI/ISP、时钟和 GPIO。超时不强杀、不卸载 VO 模块。
启动脚本已验证重复启动拒绝、正常停止和默认命令重启；重启后两路继续出帧。

纯采集与 ID 诊断（必须先停止显示实例）：

```sh
./ky50_sensor_capture ir 120
./ky50_sensor_capture rgb 120
./ky50_sensor_capture both 300
./ky50_sensor_probe ir 0
./ky50_sensor_probe rgb 0
./ky50_sensor_probe both 0
```

probe 会控制 sensor 复位、时钟和 pinmux，因此不是无副作用的只读程序；它只读取两个
明确的 ID 寄存器，不写 sensor 初始化表。它与采集程序共用独占锁，已验证采集运行时会
拒绝执行，避免改动正在工作的时钟。PWDN 参数使用本板已验证的 `0`。

## 数据、显示与验收证据

- 两路单独各取得 120 帧，双路测试每路取得 300 帧，均正常退出。
- 首次持续显示每路取得 4984 帧后正常停止，约 2 分 47 秒；末尾平均取帧率分别约
  29.79 / 29.85 fps，平均值包含启动和同步保存文件造成的停顿。
- 稳态 `/proc/soph/vi` 报告每路约 30 fps，`LostFrame=0`、`VbFail=0`。
  `/proc/mipi-rx` 两路 ECC/CRC/Header/WordCount/FIFO 错误为 0。
  `/proc/soph/vo_disp` 报告 `bw fail(0)`、480×854、stride=1472。
- 用户通过分别遮挡/晃动镜头确认两幅画面随真实场景变化，无红线、无抖动。
- 默认 `./run.sh` 重启后再次取得超过 3570 帧/路，保持约 30 fps；
  最终证据在 `stable-default.log` 和 `display-default.log`。
- 第一轮显示启动及同步保存图像期间有 3 条 VO `waitq full, drop frame`；后续稳定运行
  快照未见新增；默认重启后也在同样阶段出现 3 条，随后未见新增。
  不能把“VI 无丢帧”写成整个运行期间“显示从未丢帧”。

程序在第 1、90 帧保存 `ir/rgb-001/090-1920x1080.nv21`。每行去掉 stride padding，
文件为紧凑 NV21，大小严格为 3,110,400 字节；日志记录原始 stride、plane 长度、PTS 和均值。
同一目录重复启动会覆盖这些固定名称，需先归档上一轮证据。

主机查看示例：

```sh
ffmpeg -f rawvideo -pixel_format nv21 -video_size 1920x1080 \
  -i rgb-090-1920x1080.nv21 -frames:v 1 rgb.png
```

显示链路为 `GC2083 → CSI → VI/ISP → NV21 → CPU 缩小/转 RGB888 → VO → ST7102`。
每幅图保留完整视场，缩小 4 倍；IR 在屏幕 `(0,79)`，RGB 在 `(0,506)`，每幅 480×270。
IR 预览仅使用亮度作为灰度；RGB 预览使用 NV21 的 VU 色度。
输出使用三个私有 VB 块，送入 VO 后释放应用引用，不改写正在由 VO 使用的缓冲。
屏幕使用输入 porch `2/40/40,2/9/193`、60 Hz；SDK 调整后 HFP=54、pixel clock=36563 kHz，
VO 使用读回时序。HS prepare/zero/trail 为 `4/16/8`。

`evidence/` 保存编译日志、芯片 ID、失败定位、单路/双路/显示日志、状态快照和图像。
当前板端运行日志为 `display-isp-fix-restart.log`，旧 `display-default.log` 仅为初版历史证据。
验证只覆盖本板本次运行和正常启停；未烧录固件、未验证修正版冷启动、未做长期稳定性
测试或 ISP 画质调校。用户使用独立电源的红外补光灯验证了修正前后差异，程序不控制该灯。
IR 预览仅取亮度转灰度，画质优化应作为后续独立工作，不把本次取图/显示验收当成画质验收。
两颗 sensor 当前独立输出，没有配置 FSYNC 硬件同步，不宣称双目帧精确同步。

恢复字符屏测试：先 `./stop.sh`，再执行
`cd /mnt/data/opt/ky50_lcd && ./run.sh 0`。不要热卸载旧固件的 `cv184x_vo`。
