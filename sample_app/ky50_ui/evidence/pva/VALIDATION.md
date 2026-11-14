# CV184X ky50_ui IR 手掌绘制验证（2026-11-11）

本文件为上一阶段检测绘制基线；当前用户注册/识别、灰度修复和新包指纹见 [../access/VALIDATION.md](../access/VALIDATION.md)。下面的哈希和“尚未实现业务”描述对应旧包，不代表当前运行版本。

## 交付身份

- 应用源码：`SDK/cvi_mpi/sample_app/ky50_ui`，ARM musl 11.3，Cortex-A53 ARM32/NEON。
- 库/头文件：应用 `pva/lib`、`pva/include`，复制自 `SDK/libpva_face/build/cv184x-delivery`。
- NFS 包：`/home/bonoy/NFS_Files/ky50_pva_20261111` → `/mnt/data/opt/ky50_pva_20261111`。
- 板端 ELF：`/tmp/ky50-ui/ky50_ui`，模型 `/tmp/ky50-ui/pva/pva.model`，授权 `/mnt/cfg/ky50-ui/pva/deviceID.key`。
- 模型源为 `quantization_fused_input_20261111/final_models`；包顺序 tiny / large / small，完整文件名与量化元数据见 `pva/models/manifest.json`。

| 文件 | SHA-256 |
| --- | --- |
| ky50_ui（strip 后） | `3d1c3210d5bfd46068d9d5b447b84954cdf028697028351d3e0633517937e3b0` |
| libpva.a | `d174548e2529ed6daa1bd4f236fd54b559ca944914a8a4ac18d0970c2ca3a315` |
| libdeviceCerf.a | `8393b4900b61e96d249b475a6560aa2676704c559db15474bee9e57e61190805` |
| libsodium.a | `9a49e6f22c77eda98dabfb54e5aabeceb4ef4df44d9f7c0e29393f8dc08a6c8e` |
| pva.model | `403074544458ef1b4984891c64d67802ef5847433c1de3881a3852e5871de91e` |

运行时动态依赖为 `libbmrt.so.1.0`、`libbmlib.so.0`、`libatomic.so.1`、musl；业务库及 C++ 标准运行库静态链接。板端完整包哈希校验通过，授权通过 `cv184_probe license` 再次校验。本轮不改变授权请求/校验规则。

## 输入、旋转与副本

IR 固定 pipe0/0x37/MAC1，RGB 固定 pipe1/0x3f/MAC0，双摄均保持启动。算法 group1：1920×1080 NV21 → 960×540（stride960）→ CCW90 紧密 Y 平面 540×960，无裁剪。UV 填128，库只读 Y 后生成 RGB_PLANAR U8，调用原 `pvaGetHandPose`。

显示 group0：1920×1080 → 864×486（stride896）→ CCW90 486×864 → `(2,4)` 裁剪 480×854。`HandPoseRes_t draw_copy=pose` 后使用独立映射结构，原始坐标不变；映射 `x*0.9-2`、`y*0.9-4` 四舍五入。绿色框、腕点红色，其余6点黄色。RGB 背景不做双目配准，直接显示 IR 结果，遵循用户要求；没有新增小窗。

IR 背景使用同一个 VI 帧生成显示和算法输入；RGB 背景另取 IR 帧。VI/VPSS Get/Release 配对，推理前释放媒体帧；一个采集线程串行调用库。原始算法结果和显示结果分别写入 SIGUSR1 JSON。

## 验证证据

- `host-ui-test.log`：LVGL 实际回调、页面往返、时间与键盘回归通过。
- `host-overlay-test.log`：宿主 ASan/UBSan/LSan 通过，覆盖全帧旋转、Y/VU 配对、带 stride 数据、屏幕坐标映射、越界裁剪和原结果字节不变。沙箱内 LSan 初次受 ptrace 限制，最后在普通宿主环境重跑通过。
- `arm-pixel-test.log`：板端 NEON 逐像素旋转与独立坐标参考一致；NV21 转 RGB/alpha 与标量一致（122880像素、256个alpha值）。
- `software-scalar.log`：应用内旋转首版实际手掌命中，累计报告134次命中；曾连续5秒窗口命中33、43、35次；用户明确回复“框和关键点跟随正常，画面和触控正常”。
- `software-neon-ir.log`：最终优化版 IR 背景超过100秒，约16～17次检测/秒，显示提交约29.3～29.7帧/秒，IR错误0；当时场景没有手掌。
- `software-neon-rgb.log`：最终旋转实现的 RGB 背景连续185秒，报告3056次检测调用；算法仍为 IR pipe0，IR错误0。最终打包随后固定LVGL目标文件排序，重部署后的日志另存 final-package.log。
- `board-final.txt`：最终版 VI 双路帧率约31（驱动计数）、LostFrame/VbFail均0，VO bw fail=0，RSS约13MiB。屏幕提交可以重复最新帧，不能当作算法帧率。
- `rgb-palm-ir.png` / `rgb-screen.png`：同次诊断的完整 IR 输入与实际 UI 合成帧；UI截图与算法转储来自不同线程，不保证同一瞬间。
- `palm-ir-close.png` / `palm-rgb-close.png` / `palm-rgb-close.json`：实际近距离手掌帧，未通过现有检测/质量条件，因此没有绘制；不是成功命中示例。检出覆盖另由运行计数和用户实屏确认支持。

## GDC 复现和处理

接手时旧版 UI 已持续 `CVI_VPSS_GetChnFrame 0xc006800e`；基线在 NFS `pva_cv184x_20261111/ui-baseline`。正常重启后双 group GDC 旋转运行约269帧，先收到异常 group1 `960x576`（预期旋转存储576x960），随后内核 `gdc op blocked`，证据在同目录 `ui-gdc-failure`。卡住时还出现缓冲引用残留，单纯重启进程不能恢复。

本次保留 VPSS 硬件缩放，将两路旋转移至应用内 NEON 分块转置；VPSS Rotation=0，不再提交 GDC 任务。没有修改驱动、强制卸载 VO 或调节模型阈值。先经完整像素测试，再运行 IR/RGB 两种背景。优化前后的数学输出一致。

## 边界与复现

板端仍运行最终 RGB 主画面；配置保存值 RGB 未因临时 `--source IR` 测试改变。用户实屏确认发生于应用内旋转首版；之后 NEON 优化经板端逐像素测试，IR/RGB 后端运行通过。没有把最终优化版宣称为另一次独立用户实屏确认。

此次完成 SSH+NFS 部署，未运行 `install-local.sh`，没有改写自启、内核、设备树或固件。未完成新版本30分钟以上稳定性、断电冷启动、断网实屏复验及识别/注册业务验收；人脸模型仍不实现。旧 GDC 驱动内部故障留待单独修复。

停止 `/tmp/ky50-ui/stop.sh` 后可回退 `/mnt/data/opt/ky50_factory/run.sh`。启动本版用 `/mnt/data/opt/ky50_pva_20261111/run.sh`；临时 IR 用 `--source IR`，诊断关闭检测用 `--no-handpose`。SIGUSR1 转储命令和坐标定义见应用 README。
