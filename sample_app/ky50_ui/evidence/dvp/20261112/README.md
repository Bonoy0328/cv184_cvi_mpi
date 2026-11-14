# KY50 飞线后 DVP 调试：双板对照

最新状态：2026-11-12，第二块板已通过真实 GC0328 采集与生产显示处理链路。随后用户换回第一块板并接屏，实际点击工厂测试 DVP，确认画面随手指变化正常；详见 [第一块板接屏验证](BOARD1_RETURN.md) 和 [第二块板结果](BOARD2.md)。Speaker 调试暂停。

以下保留第一块板当时的失败实验记录，不能用其“未取帧”结论覆盖后续第二块板的软件修正结果。第一块板尚未用最终修正版本复测。

## 板卡与程序

- 用户确认 VIVO_D10 / VIVO_CLK 在芯片端交叉飞线，全部外设接回。
- 板端配置 `/mnt/cfg/ky50-ui/dvp-clock-wiring=swapped`；输入 PCLK 在 VIVO_CLK mux 1，输出 MCLK 在 VIVO_D10 mux 5。
- I2C2/0x21，8 位寄存器，ID 0x9d；640×480 YUYV。
- 原板内核 `#1 PREEMPT Fri Nov 6 13:51:11 CST 2026`，详见 board-version。
- 产品源码仅修正 combo attr 的 active_w/h=640/480，并添加独立探测工具。未烧录内核或固件。
- 临时 UI `/tmp/ky50-ui/ky50_ui` SHA256：`db474bc3c24939c263b897d12a11a90dbf8be58d21d58fda0f84014de37152b5`。DVP 首帧超时后正常回退 RGB；未保存失败的 DVP 选择。

## 已核对的关键实验

| 对照 | 日志（board1/ 前缀） | 结果 |
|---|---|---|
| 仅 VI 内部图；只加引脚；再加接收器复位 | stage0/1/2 | 每组 5 秒 212 帧 |
| 加 BT.601 属性；再加 sensor 时钟 | stage3/4 | 每组 5 秒 423 帧 |
| 再等待 300 ms；再唤醒 sensor；再读 ID | stage6/7/8 | 每组 5 秒 423 帧 |
| 完整 sensor 寄存器表（在 VI 前输出同步） | stage5/9 | 0 帧 |
| 表前 275/277 项 | limit275/277 | 每组 3 秒 254 内部图帧 |
| 表前 278/279 项 | limit278/279 | 0 帧；第 278 项为 F1=07 |
| 延后 F1/F2，内部 NV21 图 | lateallpat | 5043 内部图帧，0 超时 |
| 延后 F1/F2，内部 YUYV 图 | lateyuvpat | 初期约 84.6 fps；手动切外部后不再有帧 |
| 延后 F1/F2，真实 GC0328 | lateall、real-final | 0 帧 |
| 延后 F1/F2，增加 ISP 流程 | fullisplate | 15 秒 0 帧 |
| 延后 F1/F2，MAC1/2 | latemac1/2 | 各 8 秒 0 帧 |

上述 stage/limit 每组前先运行 RGB UI 作为一致基线。第 278 项的定位说明 sensor 输出开启时机影响内部图初始化，不能据此认定 sensor 损坏或真实输入根因已找到。NV21 内部图运行期间逐项开启 F1=1、2、4、7 都继续出帧；问题与启动时机有关，不能简单说“开启 PCLK 就一定坏”。

首次 GPIO 轮询约 1 秒：VS=20 Hz，HREF≈9.58 kHz；VS 高电平占约 75.7%，HREF 约 51.2%。PCLK/MCLK 都有跳变，但采样速率不足以求其真实频率。

## 防止误读证据

- `FIRST_FRAME` 可能来自内部 patgen！没有任何此次真实 GC0328 帧可作为验收图。
- `lateyuvpat.log` 的 4346 帧全部出现在切外部输入之前。由于诊断器按全程累计帧数返回，最终 rc=0 不能代表外部输入通过。日志前段约 85 fps 也不是 GC0328 帧率。
- `good-regs.txt` 是早期误命名的失败状态；真正成功的快照为 `after-rgb-regs.txt` / `expanded-good.txt`。`expanded-bad.txt` 对应 sensor 提前输出的失败内部图。
- `cleanpat-after-rgb.log` 中临时把 MAC 200M 改为 400M 后，内部图帧率约 42→85 且无超时；已恢复。两个频率都能工作，不能声称 200M 解决了真实输入。
- 真实输入计数显示 SOF 增长、有效像素/行/完成帧为 0，GetChnFrame 超时 `0xc00e800e`。GPIO 有跳变不能证明 TTL 接收时序正确。
- 后续尝试了极性、映射、时钟选择与 CSI shadow 更新等有限视频寄存器实验；均未产生真实帧。退出后恢复试验设置，不加入生产源码。

## 文件与复现

- `board1/`：板端原始日志、寄存器读回、进程停止记录与 ELF 校验。
- `diagnostics/`：本轮临时 C 源码与生成脚本快照。脚本中的 `/tmp` 路径保留当时现场语义，不作为产品构建入口。
- `bin/`：用于第二块板对照的固定 ELF 和 SHA256SUMS，避免换板时换了程序。
- 产品探测器：在 ky50_ui 目录 `make dvp_probe`；先停所有媒体应用，再运行 `KY50_DVP_CLOCK_WIRING=swapped ./dvp_probe 10`。仅限已确认交换时钟的板。
- 第二块板使用同一模块/排线/电源，先核实 SSH 身份、固件版本和飞线配置，再用 `dvp_probe_lateall`（真实输入）及 `dvp_probe_lateyuvpat`（内部图）做相同时间对照。不要并发运行媒体程序。
- 所有诊断器均有运行时限，SIGTERM 正常清理。临时原始输出 `/tmp/ky50-gc0328-first.raw` 可能是旧的内部图，换测试前应删除；其存在不代表新测试成功。

两板差异只能用于缩小范围，不能单独证明飞线质量好坏。还需真实图像与物理屏幕验收。现板日志已备份，媒体进程已停止，可断电更换主板。
