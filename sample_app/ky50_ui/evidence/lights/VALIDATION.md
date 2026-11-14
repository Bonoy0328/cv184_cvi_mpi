# KY50 工厂白光 / 红外灯 GPIO 开关

日期：2026-11-13，北京时间。工厂灯控源码、ARM 应用/模块构建、主机 LVGL 测试、NFS 部署和板端 GPIO 检查已完成。用户确认白光灯正常亮灭；红外灯关闭后变暗但仍亮，物理关断尚未验收。用户最新要求暂不处理残亮问题，应用启动默认改为红外常亮、白光关闭；默认状态更新验证另见本文末节。

## 引脚依据

KY50 V1.0 原理图 `Doc/KY50-V1.0(1).pdf` 第4页给出主控映射，第7页给出灯驱动：白光经 NCE2312 低边开关，红外接 PAM2804 EN。按原理图实现高电平开启、低电平关闭。

| 灯 | 原理图网络 | SoC pad / ball | GPIO | mux 寄存器 | GPIO 功能值 |
| --- | --- | --- | --- | --- | --- |
| 白光 | `LED_PWM` | JTAG_CPU_TMS / N6 | GPIOA19 | `0x03001064[2:0]` | 3 |
| 红外 | `ILED-PWM`（需求中称 ILED_PWM） | SD0_CMD / L3 | GPIOA8 | `0x03001020[2:0]` | 3 |

GPIO 功能值由 `SDK/linux_5.10/drivers/pinctrl/cvitek/cv184x_pinlist_swconfig.h` 核对，寄存器偏移由 `SDK/u-boot-2021.10/board/cvitek/cv184x/cv184x_reg_fmux_gpio.h` 及 `SDK/ramdisk/tools/cvi_pinmux/cv184x/cvi_pinmux.c` 核对。GPIO 控制器按 label `3020000.gpio` 查找，不依赖可变的全局 GPIO 编号。没有启用 PWM，也没有使用 LCD 背光 GPIOA7。

## 实现与生命周期

- 入口仍为「菜单 → 高级设置 → 工厂测试」，使用现有两个补光灯占位位置。每个按钮显示“开启/关闭白光灯（红外灯）”及当前状态，两路独立。
- `ui.c` 仅发布动作；`main.c` 调用 `factory.c`，驱动写成功后才更新状态。设备打开/写入失败提示错误，不显示虚假的开启成功；可再次点击重试。
- `kernel/ky50_lights.c` 提供独占 `/dev/ky50-lights`。两字节命令 `w0/w1` 控白光、`i0/i1` 控红外；只改目标 GPIO。可读回三个字符：白光命令状态、红外命令状态、换行；这是驱动命令状态，不是光学测量。
- 模块加载及设备打开时均置两路为低；应用初始化成功后，在采集线程启动前主动打开红外灯，因此每次启动最终为红外高、白光低。只在启动时应用一次，不循环覆盖工厂按钮的手动选择；切页、预览源切换保留灯状态，不写配置。退出应用关闭 fd 时，内核将两路拉低；GPIO 拉低不表示已解决红外硬件残亮。
- 两路 GPIO 申请失败时模块返回错误；加载失败不阻止其他工厂测试。卸载时先拉低，再恢复加载前 pinmux 并释放 GPIO。使用期间不允许第二个进程打开设备或卸载模块。
- `kernel/Makefile`、应用 `Makefile package`、`run.sh` 已纳入新模块；未修改固件设备树、开机服务、用户数据库、算法参数或相机管线。

## 本机验证

在 `SDK/cvi_mpi/sample_app/ky50_ui` 执行：

```sh
python3 tools/make_fonts.py
make -j8 package
make -j8 TARGET=host
mkdir -p build/previews-lights
./ui_preview build/previews-lights
sh -n run.sh
(cd build/deploy && sha256sum -c SHA256SUMS)
```

- ARM 应用及 `ky50_lights.ko` 构建通过，构建日志无 `warning:` / `error:`。模块 vermagic 为 `5.10.4-tag--g79534ab146b7 preempt mod_unload ARMv7 thumb2 p2v8`，已与目标板实际内核核对并成功加载。
- 主机真实 LVGL 指针测试通过：两个按钮分别发出正确动作；收到后端结果前不提前变更状态；两路状态独立；预览返回工厂页保留状态；不可用提示及重试动作正确。已有页面/参数/用户流程回归通过。
- 字库重新生成，6854 字符；与修改前 SHA-256 相同，新增文案所需字形已存在。
- 已检查 480×854 开/关状态渲染，按钮文案完整，无越界。预览图为主机渲染，不代表实屏或实际灯状态。
- 启动脚本语法及完整部署包 SHA-256 校验通过。日志、界面图和产物指纹保存在本目录。

首版“应用启动两路关闭”的历史产物如下，最新默认状态版本见末节。

| 产物 | SHA-256 |
| --- | --- |
| `build/deploy/ky50_ui` | `a53ef116522a61f81c16caaef40319f9399501c8f44453795423c45cee507f88` |
| `build/deploy/ky50_lights.ko` | `b4a22e5b866861f94d067ea0d7d7a8835de7c0d9f6676780a455a52c4417f806` |

## 上板运行与 GPIO 检查

首次连接 `192.168.31.83` 因主机密钥变化停止。用户随后回复“确认是当前机器”，重新扫描的 ECDSA 指纹与用户确认的 `SHA256:OEBGiTQ9s8w+A0PsbqWJ42/jT1CqpL12P+rNlVQUgFg` 一致。使用任务专用 known-hosts 且保持严格校验，没有覆盖全局旧密钥。

核对板上当时运行的是 `/usr/bin/sample_ky50_lcd`，没有 `ky50_ui` 进程；GPIOA19/A8 无 GPIO 占用。挂载既有 NFS 导出到 `/mnt/data/opt`，独立发布目录为主机 `/home/bonoy/NFS_Files/ky50_lights_20261113`、板端 `/mnt/data/opt/ky50_lights_20261113`。发布包全量校验通过，旧发布目录保留。

先加载 `ky50_lights.ko`，执行 [board-driver-check.sh](board-driver-check.sh)，结果见 [board-driver-check.log](board-driver-check.log)：

- 两路 mux 均为 3，GPIO 方向均为输出。
- `w1/i1/w0/i0` 以及反顺序开启通过；GPIO 输出寄存器与 EXT 输入回读中的 A19/A8 位均匹配目标状态。
- 第二进程打开返回 busy；非法 `w2` 被拒绝且不改变灯状态；关闭设备 fd 后两路输出和输入回读均为低。
- 检查期间同一 GPIO bank 其他输出位及方向未被改变。寄存器逻辑低不等于已测到 PAM2804 EN 脚的模拟电压。

随后使用 `/etc/init.d/S99z_ky50_lcd stop` 正常停止 LCD 测试，启动本包 `run.sh`，没有刷固件或修改开机服务。初次启动 PID 为311，实际 `/proc/311/exe` 和 `/tmp/ky50-ui/ky50_lights.ko` 的 SHA-256 与上表一致。双 GC2083、PVA 双模态及触控启动，采集约16 fps；仍存在此前记录的 ISP `WDMA buffer full` 告警，本轮未将其作为灯控问题修改。

正式数据仍使用 `/mnt/system/ky50-data`，用户保存的 `palm_leave_frames=3`、`face_leave_frames=3` 保留。运行日志 `/tmp/ky50-lights.log`，准备及启动证据见 `board-prepare.log`、`board-startup.log`。

## 红外灯关闭后仍亮：现场反馈与电路线索

用户实际点击反馈：“白光灯正常亮灭，红外灯关闭时明显变暗，但是没有完全灭，开启时红外灯变亮”。应用日志已记录对应 `FACTORY_LIGHT WHITE/IR=ON/OFF`，因此白光物理开关已确认，红外物理关断不能标为通过。

随后用户将两路关闭并回复“两路已关闭，红外仍微亮”。北京时间17:33:18开始同步读取，证据见 [ir-off-confirmed.log](ir-off-confirmed.log)：运行进程仍为311，最后红外命令为 `IR=OFF`，GPIOA8/A19 在 debugfs 中均为 `out lo`。连续三次样本中，两路 mux 均为3、DDR方向位均为1、DR输出位和EXT输入回读位均为0；`DR=0x00008094`、`DDR=0x40288194`。本次已将用户观察到的残亮与 GPIO 低电平对应；软件关断命令和 GPIO 逻辑状态正常，但尚不能用数字回读推定 U9 EN 脚已低于0.3 V。

原理图第7页的 [红外驱动局部图](ir-driver-schematic.png) 显示以下独立供电路径：

```text
VCC5V0_SYS → R52（20Ω）→ D6（1N4148，正向）→ LED+
          → 红外 LED 负载 → LED− → R53（0.6Ω）→ GND
```

这条路径绕过 U9/PAM2804 与 L6，不受 `ILED-PWM → EN` 控制。用户随后确认 R52/D6 均正常焊接，暂时无法测量 EN 电压。因此旁路已有实物装配依据；只要 LED 负载在这条支路上可导通，即使 PAM2804 已关断也可能继续供电，与“关后变暗、开后变亮”相符。目前尚未断开支路做对照，也未测量 EN 电压，不能写成拆件验证过的根因。

[Diodes 官方 PAM2804 数据手册](https://www.diodes.com/datasheet/download/PAM2804.pdf) 第2页规定 EN 低于0.3 V关断、高于1.5 V开启；第1页说明关断时芯片内部断开输入输出。这不能切断板外 R52/D6 旁路。

两路关闭后红外仍微亮，已排除由白光开启造成的现象；该次同步读取时应用运行、两路 GPIO 均为低。R52/D6 装配已由用户确认，EN 电压未测。此前曾建议整板断电后暂时抬起 R52 一端做旁路隔离对照，但用户随后要求暂不理会这个问题，故没有执行硬件修改或继续该诊断。旁路线索保留，不记为经过隔离验证的最终根因。

本轮后续只调整应用启动默认状态，驱动仍使用普通 GPIO。红外完全关断、预览切换返回及应用退出后的物理关灯验收不因默认值变更而自动完成。

## 最终更新：启动默认红外常亮、白光关闭

按用户最新要求，`main.c` 在灯控初始化成功后、启动采集线程前，打开红外灯并用实际后端状态刷新 UI。该逻辑只执行一次；驱动打开设备先将两路置低，因此红外初始化切换明确为0→1，白光保持0。之后工厂按钮仍可独立切换，下次启动恢复默认，不保存灯状态。失败时沿用现有错误处理，不把不可用显示为开启。

本次仅改动上述应用启动逻辑和相关文档，`factory.c`、内核模块、UI 页面及业务/相机代码不变。执行 `make -j8 package` 成功，构建无 `warning:` / `error:`；包校验通过。此前的主机 UI 回归结果继续作为未改动 UI 的已有证据，本次直接验证真实应用启动行为，没有将主机预览状态注入当作启动默认值验证。

| 项目 | 本轮验证值 |
| --- | --- |
| 主机 NFS 发布目录 | `/home/bonoy/NFS_Files/ky50_lights_default_20261113` |
| 板端发布目录 | `/mnt/data/opt/ky50_lights_default_20261113` |
| 运行 ELF / 首次启动 PID | `/tmp/ky50-ui/ky50_ui` / 577 |
| 运行日志 | `/tmp/ky50-lights-default.log` |
| 应用 SHA-256 | `4a57450bdec33b7e8949815dddb29f7ccad6a2cd8f0be3a17e0e1b5f11159876` |
| 灯控模块 SHA-256 | `b4a22e5b866861f94d067ea0d7d7a8835de7c0d9f6676780a455a52c4417f806`，与首版相同 |

正常停止旧应用后，debugfs 记录两路均为低；随后启动新包，运行 ELF 哈希与包一致。北京时间17:45:35开始核查：GPIOA8 红外 `out hi`、GPIOA19 白光 `out lo`，两路 mux均为3，方向均为输出；DR/EXT目标位均为红外1、白光0（`DR=0x00008194`、`DDR=0x40288194`）。启动日志先出现 `FACTORY_LIGHT IR=ON`，随后是应用、PVA和摄像头启动信息；触控设备正常打开，RGB预览约16 fps，离场参数3/3保留。

证据：[部署及正常停止](default-deploy.log)、[启动与 GPIO 检查](default-startup-check.log)、[构建日志](default-package.log)。当前为 NFS 应用运行验证，未刷写固件、未修改开机服务，红外硬件残亮问题按要求保留且暂不处理。
