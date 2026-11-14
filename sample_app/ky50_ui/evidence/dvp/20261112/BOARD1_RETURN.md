# 第一块 KY50 接屏复测（2026-11-12）

用户换回第一块已交换 VIVO_D10/VIVO_CLK 的板并接回屏幕。核实新 SSH 指纹后登录；内核仍为 `5.10.4-tag--g79534ab146b7 #1 PREEMPT Fri Nov 6 13:51:11 CST 2026`。板端 `dvp-clock-wiring` 已为 `swapped`，启动前没有 `display-source` 文件。

正常停止原开机 LCD 测试程序 `sample_ky50_lcd` 后，将 NFS `ky50_dvp_20261112` 中的程序和资源复制到 `/tmp/ky50-ui`，只加载 I2C1 和触控模块。未加载功放模块、未操作 Speaker、未修改开机服务。本轮传入 `--no-handpose`，业务测试数据使用 `/tmp/ky50-ui/dvp-test-data`，不覆盖既有用户数据库。

运行 ELF SHA256：`b29f49334a99d864665f034bb0157045db95152ae30dc690703a53c4709f4558`，与第二块板对应最终 UI 编译产物一致。触控实际识别为 `/dev/input/event1` / `KY50 ST7102 Touchscreen`。

## 实际触控与画面

启动 RGB 后，用户按实际手指操作进入「菜单 → 高级设置 → 工厂测试 → DVP 显示」，并在指纹头按下、移动、移开手指，回复：**“已完成，确认正常”**。此项是物理屏幕确认，区别于前一轮只保存内存图像。

日志记录 `SOURCE switching RGB -> DVP`，GC0328 ID=0x9d，640×480 YUYV 真实输入。DVP 稳态采集约 20 fps，UI 提交约 29～30 fps；后者不代表 sensor 产生了 30 帧。送屏 480×854，stride=1472。

- `board1-return/dvp-entered.ppm`、`dvp-current.ppm`：应用合成送屏图像；PNG 为同一 PPM 转换。
- `dvp-live-vi.txt`：640×480、YUV、YUYV，LostFrame=0、VbFail=0。
- `dvp-live-vi-dbg.txt` 及后续采样：丢帧、CSI 溢出、宽高异常计数均为 0。
- `dvp-live-vo-disp.txt`：480×854、pitch1472，读写错误为 0、bw fail=0。
- 触控选择 DVP 后 `/mnt/cfg/ky50-ui/display-source` 内容为 `DVP`，该保存发生在取得经过生产缩放处理的首帧之后。

## 往返切换与应用启动恢复

用户继续实际执行 **DVP→返回→RGB→返回→IR→返回→DVP**，回复 **“操作正常”**。日志对应 `DVP -> RGB`、`RGB -> IR`、`IR -> DVP`，切回 DVP 后恢复约 20 fps，配置仍为 DVP。见 `ui-before-restart.log`、`dvp-after-roundtrip-*.txt` 和 `dvp-after-roundtrip.ppm`。

随后通过 `stop.sh` 正常退出，使用同一 ELF 启动，**不传 `--source`**：

```sh
cd /tmp/ky50-ui
export PATH=/sbin:/bin:/usr/sbin:/usr/bin
export LD_LIBRARY_PATH=/system/lib:/usr/lib:/lib
export KY50_DVP_CLOCK_WIRING=swapped
nohup ./ky50_ui --no-handpose --data-dir /tmp/ky50-ui/dvp-test-data \
    > /tmp/ky50-dvp-board1-return/ui-restart.log 2>&1 < /dev/null &
printf '%s\n' "$!" > /run/ky50-ui.pid
```

日志为 `SOURCE switching none -> DVP`，触控设备重新打开成功，采集稳定约 20 fps；`restarted-dvp-home.ppm` 为首页上的 DVP 背景。命令行证据 `restarted-command.txt` 无 source 覆盖参数，运行 ELF 前后 SHA256 一致。配置读取、媒体重新初始化及送屏已通过本轮启动检查。

## 验证范围与保留状态

本次完成用户要求的工厂测试 DVP 实屏入口、真实场景变化、RGB/IR 往返切换、保存选择与应用重启恢复。测试结束保持新 UI 运行，显示源 DVP。本轮无产品 C 源码变更，继续使用此前第二块板验证后的 ELF；补充了证据及文档，并修正 NFS 包沿用旧 PVA 版本的构建说明。

**没有改动固件或开机服务；当前程序在 `/tmp`，整板断电后仍按板上原开机流程启动。应用重启恢复不等于新版固件断电自动启动验收。** 本轮亦未做 30 分钟老化、100 次切换、带手掌算法并行运行或 Speaker 验证。GC2083 停止阶段仍有既有的 `stop streaming timeout`，未阻止本轮切换；部分完整菜单重绘的触控提交耗时超过 100 ms，不能据此声称全 UI 满足 100 ms 目标。

完整内核增量还记录了首次启动第 1 帧的 `disp bw failed`，随后 VO 采样 bw fail=0、未持续新增；应用重启重新复位屏幕/触控阶段有一次 `touch read errors: -121`。触控驱动每轮读取继续重试，连续第三次错误只打印一次，因此不能仅凭不再打印就认定恢复。为此另请求用户重启后点菜单并返回，用户回复 **“确认触控和画面正常”**，实际恢复通过确认，事件留存在 `ui-restart-final.log`。不能将稳定运行期计数为 0 扩大表述为整个启动全过程无告警。
