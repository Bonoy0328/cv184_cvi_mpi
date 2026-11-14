# 第二块 KY50：GC0328 真实图像已打通（2026-11-12）

## 实物与对照条件

用户确认第二块板也已将芯片端 VIVO_D10 / VIVO_CLK 交叉飞线；仍使用原外设，只接双 GC2083 与 GC0328，没有屏幕/触控。用户核实了第二块板 SSH 身份。内核与第一块一致：`5.10.4-tag--g79534ab146b7 #1 PREEMPT Fri Nov 6 13:51:11 CST 2026`。

同一组固定校验值的旧诊断程序在第二块板上结果与第一块相同：原真实输入、仅延迟输出开启的真实输入各 10 秒均为 0 帧；内部 YUV 图 10 秒 845 帧。见 `board2/cold-*.log`。因此换板本身没有解决问题，不能认定第一块飞线质量有问题。

GPIO 输入只读采样结合 sensor F1 输出控制进一步核对：关 PCLK 后 B22 停止跳变，关 HREF 后 B16 停止跳变，关 VS 后 B15 停止跳变，B11 的 MCLK 不受上述输出控制影响。见 `signal-map-f1-*.txt`。这支持逻辑连接正确，不能代替示波器验证波形、边沿和建立保持时间。轮询高频信号的跳变计数不是时钟频率。

## 最终修正

1. `gc0328.c`：BT.601 `active_w/active_h=640/480`，避免 VI crop 变为零。此前第一块板阶段已修正。
2. GC0328 page0 **`0x46=0x02`**：VS 低有效、HREF 高有效，保留下降沿推出数据；旧值 `0x03` 在本接收配置下只有 SOF、没有完整帧。该极性组合与稳定启动顺序一起验证通过，不能把先前运行中随意改寄存器的失败当成反证。
3. 初始化表里的 F1/F2 强制保持 0；VI 通道启用后才写 **F2=1、F1=7**。退出时先停 VI，再关闭 sensor 输出，保证接收器能等到帧边界。
4. CIF BT.601 默认实际输出 Cb Y Cr Y，而 VI 描述符标成 YUYV。通过 `/dev/cv184x-mipi-rx` 的 SDK **`CVI_MIPI_SET_BT_FMT_OUT` / `BT_FMT_OUT_YCBYCR`** 显式设置 Y Cb Y Cr。读回 `0x0a0a0010=0x621`，原默认为 `0x601`。不靠改变解码器掩盖描述符不一致。
5. `scale.c`：DVP 的 VPSS 输入组必须设置 **640×480**，RGB/IR 保持 **1920×1080**。该 BSP 的 `vpss_send_frame()` 对组尺寸与输入帧进行相等检查，`u32MaxW/H` 不能理解成任意上限。旧配置报 `0xc0068003` 和 `Size(640 * 480) mismatch`。输出仍为 864×648 NV21，逆时针旋转后居中裁剪为 480×854。
6. DVP 实测约 20 fps，VI `snrFps` 同步设置为 20。未修改内核、固件、开机服务或 Speaker 配置。

## 验证结果与限制

| 检查 | 证据 | 结果 |
|---|---|---|
| VS=02、延后输出、原 CIF 字节序 | `board2/yuvpol2.log` | 120 秒 2400 帧、0 次超时；原始字节实际为 UYVY |
| 修正 CIF 字节序后的真实输入 | `board2/corrected-watch.log` | 约 396 秒、7915 帧、0 次超时；描述符与 YUYV 数据一致 |
| 真实图像内容 | [原始尺寸图](board2/corrected-empty.png) | 可见指纹采集窗/棱镜结构；没有启用内部 patgen，尚无按手指的用户确认 |
| 原尺寸配置的失败 | `board2/pipeline-check.log` | 进入 VPSS 返回 `0xc0068003`，定位输入尺寸不匹配 |
| 最终生产链路两轮 DVP→RGB→IR→DVP | `board2/pipeline-fixed.log` | 共 8 次启动、480 帧；四次 DVP 各 60 帧且 0 超时；RGB 每次启动阶段 2 次短等待、IR 每次 1 次，随后均取得 60 帧；最终 rc=0 |
| 实际送屏尺寸的内存图像 | [480×854 图](board2/ky50-dvp-scaled-1-3.png) | 生产 VPSS、旋转、裁剪已完成；不是 LCD 物理显示验收 |
| 内核日志增量 | `board2/pipeline-fixed-kernel-delta.log` | VPSS 尺寸错误消失；DVP 停止超时消失；GC2083 原停止流程仍每路打印 `stop streaming timeout`，两轮均可再启动，不能声明日志完全无错误 |

GC2083 的停止告警对应 `camera_port.c` 中 standby 早于 DisableDev 的既有流程，本轮未调整该独立流程。RGB↔IR 正常 UI 切换只选择预览 pipe，诊断器则刻意销毁后重建，覆盖资源重用。此两轮检查不是 30 分钟老化或 100 次切换验收。

第二块板当前没接屏幕和触控，尚未确认实际按钮、DVP 实屏方向/清晰度、按手指变化、断电恢复；第一块板也未用最终代码复测。没有证据要求继续改飞线。

## 产物与复现

固定历史 A/B 程序仍在 `bin/`，不替换。最终产物在 `board2/validated/`，对应源码快照在其 `source/`。全部第二块板证据校验值见 [BOARD2_SHA256SUMS](BOARD2_SHA256SUMS)。临时 RAW 灰度实验经未初始化 ISP 输出全黑，不能作为显示成功证据；部分 `after-gc2083-real-vi*.txt`、`rawgray-vsinvert-vi-dbg.txt` 是停止后快照，也不能作为流运行证据。

| 文件 | SHA256 |
|---|---|
| ky50_ui（strip 后） | `b29f49334a99d864665f034bb0157045db95152ae30dc690703a53c4709f4558` |
| dvp_probe | `9bae5cea3dcbf312df24be8033e15c9a375b79b47b40d5f93d3b21e22219b406` |
| dvp_pipeline_check | `ff7ed1bd10b4f3b006a4026a9ff0fb14500724580fcbdcc3e774edc08c647ee2` |

在应用目录构建：

```sh
make -j8 ky50_ui dvp_probe dvp_pipeline_check
make package
```

停止全部媒体应用后，仅在已确认时钟飞线的板运行：

```sh
KY50_DVP_CLOCK_WIRING=swapped ./dvp_probe 120
KY50_DVP_CLOCK_WIRING=swapped ./dvp_pipeline_check 2
```

两个程序不可并发，各自持有 `/tmp/ky50-sensor.lock`。`dvp_probe` 的 rc=0 只表示曾收到帧，应结合逐秒 misses、帧数及真实图像；第一帧可能尚未完成 AE。`dvp_pipeline_check` 复用生产摄像头/VPSS函数，不初始化屏幕或音频，DVP 第 40 帧输出 `/tmp/ky50-dvp-scaled-轮次-阶段.ppm`。

NFS 调试包已准备在主机 `/home/bonoy/NFS_Files/ky50_dvp_20261112`。本轮第二块板诊断通过显式环境变量选择 swapped，尚未写入该板的持久化接线配置，亦未安装开机服务。接屏测试须先正常结束媒体进程、保存日志，再整板断电接线；启动前再核对接线配置，UI 使用同一最终 ELF。Speaker 调试继续暂停。
