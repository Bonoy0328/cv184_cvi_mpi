# KY50 ST7102 屏幕 NFS 调试

当前按用户要求仅通过 SSH + NFS 调试，不自动打包到固件，也不安装开机服务。
已撤回 rootfs 安装钩子、sample_app 自动构建入口和 S99z_ky50_lcd 源文件，并清理 staging 中对应文件。
以前生成的 rootfs.spinand / upgrade.zip 未重新打包，仍属于旧集成产物，不用于本轮调试。

## 当前依据与实物结果

- 最终原理图：`Doc/KY50-V1.0(1).pdf`，以第 2、4、5、8 页为准。
- 模组规格：`Doc/M050-0287-1B0规格书(1).pdf`，ST7102、480×854、MIPI 2 lane，带触控。
- 厂家初始化：`Doc/lcd_dirver_example/W_ST7102_CSOT_496_60hz_0920(3.5T)_tp 3ms_G2.2_241025.TXT`。
- 2026-11-10 用户纠正此前结果：旧标准 DCS 程序实际只有背光，未显示中央 A。
  `board-test.log` 是旧日志，不能作为实物显示成功证据。
- 加入厂家完整表后，用户看到白屏和靠右的竖直虚线；补上 RGB888 COLMOD 后仍异常。
- 早期仅图形层测试：硬件彩条闪烁，纯红测试只有竖直虚线；修正初始化 CRC 后仍白屏。
- 完整 RGB 视频帧首次显示黑底中央 A，但左侧有红色虚线，A 和虚线的位置快速抖动。
  用户照片：`Doc/lcd_img/ef3aa009-331d-4979-9fbf-d4bb6e04acb4.png`。
- 2026-11-10 逐组固定参数实测：HS 8/45/12 仍异常，HS 4/16/8 用户确认
  “A 稳定、没有红线”。完整视频帧和 4/16/8 已设为默认；重新编译、部署后使用普通
  `./run.sh 0` 启动，用户再次确认“A 稳定，无红线、无抖动”。
  板端 HS 寄存器 `0x0a098014=0x08100401`，NFS 程序和主机编译产物 SHA-256 一致：
  `80f97b86b5006afb9140ead6270cb6ca843ba2722d363e42d4b6a01a11befcc9`。
  默认启动日志见 `board-default-stable-20261110.log`。本次确认覆盖该板的字符显示，尚未做长时间稳定性测试。

## 接线

| 功能 | J10 引脚及主控 | 配置 |
| --- | --- | --- |
| LCD reset | 12 → RST_LCD → F2 / PAD_AUD_AOUTL | GPIOC25，低有效 |
| 触控 reset | 6 → TP_RST → A2 / PAD_MIPI_TXM4 | GPIOC18，低有效 |
| 触控 INT | 7 → TP_INT → B3 / PAD_MIPI_TXP4 | GPIOC19 |
| 触控 SCL | 8 → TP_I2C1_SCL → B1 / PAD_MIPI_TXP3 | I2C1，功能 6 |
| 触控 SDA | 9 → TP_I2C1_SDA → B2 / PAD_MIPI_TXM3 | I2C1，功能 6 |
| LCD 背光 | LCD-PWM → L1 / SD0_CLK → U12.EN | GPIOA7，高有效 |
| DSI D0 | 14/15 → TXM0/TXP0 | 物理 lane 0 |
| DSI D1 | 17/18 → TXM1/TXP1 | 物理 lane 1 |
| DSI CLK | 20/21 → TXM2/TXP2 | 物理 lane 2 |

lane 数组为 `{MIPI_TX_LANE_0, MIPI_TX_LANE_1, MIPI_TX_LANE_CLK, -1, -1}`，原理图无 P/N 对调。
LCD 与 TP 复位一起处理；不占用触控 SDA/SCL/INT。`BL_EN` 属于门铃，不是 LCD 背光。
GPIO 使用字符设备控制器标签，不依赖动态全局 GPIO 编号。

触控尚未实现：当前运行设备树 `i2c@04010000/status=disabled`，不存在 `/dev/i2c-1`，
SDA/SCL 仍是 GPIO 功能。内核构建配置未启用 CONFIG_INPUT_TOUCHSCREEN，板上只有 keyscan 输入设备。
厂家提供的 TXT 是 LCD 初始化表，不包含触控 I2C 地址、坐标协议或触控固件。

## 编译和部署

在主机执行（加载现有板型配置，不重置 .config）：

```sh
cd /home/bonoy/CV184X_WS/SDK
source build/envsetup_soc.sh
export CHIP=cv1842hp BOARD=wevb_0014a_spinand
cvi_setup_env
make -C cvi_mpi/modules/mipi_tx PWD="$PWD/cvi_mpi/modules/mipi_tx"
make -C cvi_mpi/sample_app/ky50_lcd
install -m 0755 cvi_mpi/sample_app/ky50_lcd/sample_ky50_lcd /home/bonoy/NFS_Files/ky50_lcd/sample_ky50_lcd.new
mv /home/bonoy/NFS_Files/ky50_lcd/sample_ky50_lcd.new /home/bonoy/NFS_Files/ky50_lcd/sample_ky50_lcd
install -m 0755 cvi_mpi/sample_app/ky50_lcd/run.sh /home/bonoy/NFS_Files/ky50_lcd/run.sh
```

板端：

```sh
cd /mnt/data/opt/ky50_lcd
./run.sh 0
```

0 表示持续运行，60 表示 60 秒；Ctrl-C 关闭背光、视频通道、VO/MIPI，并释放私有视频缓冲和 GPIO。
`run.sh` 会先停止旧固件中可能存在的 S99z_ky50_lcd 服务。
脚本默认启用 `KY50_DIAG_PAYLOAD_CRC=1`：由持有 MIPI 设备的本进程通过 ESC 寄存器发送
正确 CRC 的长包，绕过已烧录 VO 模块的 CRC 问题。该临时路径在 `dsi_raw.h`，只供 NFS 调试。
板上当前 rootfs 为只读 SquashFS，已经烧录的旧服务仍存在，重启会再次启动；
主机源码回退不会自动修改板上 Flash。

后台运行（先停止已有 NFS 测试）：

```sh
cd /mnt/data/opt/ky50_lcd
nohup ./run.sh 0 >/tmp/ky50-lcd-test.log 2>&1 </dev/null &
echo $! >/tmp/ky50-lcd-test.pid
cat /tmp/ky50-lcd-test.log
# 停止本次测试
kill "$(cat /tmp/ky50-lcd-test.pid)"
```

## 显示配置与诊断

默认 `--vendor-test 2,40,40,2,9,193,60` 使用厂家给出的 porch，60 Hz、RGB888 burst，输入像素时钟 35675 kHz。
修正 middleware 按实际 lane 数计算传输时间后，SDK 将 HFP 扩展为 54、像素时钟调整为 36563 kHz，
以容纳 burst 包开销。程序回读实际 MIPI 时序用于 VO，避免两处配置不同。
`st7102_init.h` 按原文件顺序保留 23 条命令、20 条长命令、重复 A7 及 120/20 ms 延时。
原表中 count=0 的 00 是占位值，不发送为参数。额外在 Sleep Out 前发送 `3A 77` 与 RGB888 主机匹配。
HS prepare/zero/trail 默认采用本板实物确认的 **4/16/8**。
SDK 通用默认 6/32/1 和对照 8/45/12 均有位置抖动及红线，不能沿用。

字符 A 为 80×112，左上角 (200,371)，在 480×854 画面居中。
默认提交 RGB888 视频帧并使能视频层/通道，图形层关闭；stride=1472，私有缓冲 1257088 字节。
板端实际 DMA 内存抓取确认：非黑像素仅在 A 的区域内，右侧与行填充区全为 0；
SHA-256 为 `b38b7103f4360bc7415b3a29f269e72d8d3c0f2381caeb6ec9eeaf3d30885a56`。
内存正确与实物稳定是两项独立验证，不能相互替代。

运行过程中可发送诊断信号：

```sh
# 每次依序切换 A → 硬件彩条 → 红 → 绿 → 蓝 → 黑 → A
kill -USR1 "$(cat /tmp/ky50-lcd-test.pid)"
# 暂停 HS 视频，读取 DCS 状态，再恢复视频
kill -USR2 "$(cat /tmp/ky50-lcd-test.pid)"
```

旧 `--otp-test` 仅保留用于对照，不能用作已验证驱动；`--probe` 会复位和配置屏，不是纯只读操作。
DCS 回读使用哨兵值识别 SDK BTA 超时未写入数据的情况，最多重试三次。
这不能识别硬件返回的旧 RX 数据；低时钟实验曾所有寄存器均回读 A0，不能视为有效结果。

正常启动不需要临时诊断变量。`KY50_DIAG=1` 在启动视频两秒后暂停并读一次状态；
`KY50_DIAG_POST_INIT_READ=1` 在初始化结束、进入视频前额外回读；默认均不启用。
`KY50_DIAG_HS=8,45,12` 可对照 HS 时序，`KY50_DIAG_GFBG=1` 可复现旧的仅图形层路径。
旧环境变量 `KY50_DIAG_VIDEO_FRAME` 和 `KY50_DIAG_NO_POST_READ` 已由默认行为替代。
试验性 P/N 交换、取消时钟 90 度相移均未改善，当前采用原理图映射、无 P/N 交换和 SDK 默认相移。
切勿同时运行另一个打开 MIPI 设备的诊断程序，原驱动按进程管理设备所有权。

## 已发现的 SDK 问题

1. 原 `dsi_mac.c` 对长包的 CRC 包含了 4 字节包头。已修正为只计算 payload；
   初始化表恰有 20 条长命令，原初始化阶段读到错误计数 0x14。
   NFS 用户态正确 CRC 路径已在板上验证：初始化错误计数降为 0x00，B0/B7/BF 回读与厂家表相符。
   视频启动后 0x05 回读为 0xFF，稳定和异常 HS 组均读到该值。
   该诊断值无法区分本次实际显示质量，需结合 ST7102 具体寄存器定义继续解释。
   内核源码修正已单独编译，但修正版模块未在板上加载；初始化修复不等于显示修复。
2. 尝试热卸载原 VO 模块时，vo_core_remove 在 vo_destroy_instance 后访问已释放的 g_vo_ctx，
   触发空指针 Oops，模块停留 Unloading。不要在原固件上再次 rmmod cv184x_vo。
   修正版 VO 模块当时尚未加载。异常记录在 `board-nfs-20261110.log`。
3. `modules/mipi_tx/platform/linux/platform_mipi_tx.c` 的传输时间原本固定除以 4 lane，
   已改为实际 lane 数。主机对 1/2/4 lane 验算了行内传输时间，4 lane 的计算行为保持不变。
   更新后的库已静态链接进 NFS 程序，板端实际时序已回读确认。

## 待确认的硬件状态

当前 GPIO 输出寄存器将 C25/C18 均置高，但 debugfs 采样 C25 为低、C18 为高。
C25 复用音频焊盘，其独立 GPIO 输入使能寄存器 `0x0300a12c` 读取为 0，
因此不能仅凭该低回读断言 LCD 仍在复位。2026-11-10 用户实测确认 J10.12（RST_LCD）、
J10.6（TP_RST）均为 1.8 V：当前两个复位脚均已释放。这个静态电压不能验证上电复位脉冲时序。
VO 的 480×854 时序、图形层使能已确认；带宽错误状态曾为 0，断电重启后的图形层测试又读到 1。

为排除视频 DMA 没有有效底图，加入完整视频帧路径（`vo_frame.h`）：
申请约 1.2 MiB 私有视频缓冲，提交完整 RGB888 中央 A，关闭图形层。
板端已记录视频通道使能、帧提交成功、stride=1472、带宽错误为 0。
此后曾发生 ping/SSH 失联；用户断电重启后恢复，没有串口异常证据，不能认定为 VO 崩溃。
复测期间 SSH 稳定；VO 欠载累计只在启动首帧增加 1 次，之后未增加。
显示中断在 5.01 秒内增加 300 次，实际帧率约 60 Hz。

触控复位已处理，触控坐标功能尚未实现。串口可用 `/dev/ttyUSB0`、115200 波特率，
但当前主机执行 `sudo -n minicom` 提示需要 sudo 密码，尚未取得串口日志。

当前仍仅使用 NFS 调试；触控坐标和固件开机显示不属于本轮已验证结果。
