# 用户注册、识别与 IR 灰度验证（2026-11-11）

后续手掌状态颜色修订已部署为`ky50_quality_20261111`，见文末；以下`ky50_access_20261111`和原应用哈希保留为注册识别基线。

应用：`SDK/cvi_mpi/sample_app/ky50_ui`。本轮 NFS 包 `ky50_access_20261111`；板端 `/mnt/data/opt/ky50_access_20261111`，运行副本 `/tmp/ky50-ui`，日志 `/tmp/ky50-access.log`。旧检测版本 `ky50_pva_20261111` 保留。

| 交付文件 | SHA-256 |
| --- | --- |
| ky50_ui（strip） | `4bd14d6705322cd85af2d273404b1ca10318d2e4f6fb829cc3dea42c9af5a5da` |
| libpva.a | `fc5fe3e3c0f3bd4187a4aff93f1a47ded027ab61358a73499309f550e06d7aa6` |
| libdeviceCerf.a | `8393b4900b61e96d249b475a6560aa2676704c559db15474bee9e57e61190805` |
| pva.model | `403074544458ef1b4984891c64d67802ef5847433c1de3881a3852e5871de91e` |

## 主机与交叉编译

- ARM GNU 11.3 musl 静态业务库和应用构建通过，应用源文件启用 `-Wall -Wextra -Werror`。SQLite 3.53.1 来自同一 SDK 本地源码，静态编入应用。
- `access_test` 使用真实 SQLite 和注入的测试算法后端，ASan/UBSan 通过：UTF-8 边界、空姓名、前导零工号、重复工号、质量不合格不推进、同一进度对象、识别去重、删除失败重试、注册写入中断恢复、保留历史身份、满用户容量、只读存储、模板缺失/未绑定模板保护，以及10000条未导出考勤不被覆盖。沙箱运行关闭 LeakSanitizer；不将此称为 LSan 验证。
- `ui_preview` 通过真实 LVGL 指针回调验证工号→姓名→类型→注册，拼音选“张”、英文大小写/数字/删除、空姓名、进度显示、完成返回、取消、双复选框删除、12行列表滚动时固定底栏仍可点击。截图位于 `build/previews/`，背景为合成图，不是板端照片。
- `hand_overlay_test` ASan/UBSan 通过：Y/VU完整旋转、stride、屏幕映射、裁剪以及算法原始结果不变。
- ARM `pixel_check` 在板端通过：122880像素、256 alpha值，NEON NV21/灰度/混合与独立标量公式一致。

## 实际板端

- 新应用成功授权，依次加载 tiny、large、small 三个加密模型。授权流程、请求地址与公开接口未改变。
- ISP 日志 `pipe0=IR mono=1`、`pipe1=RGB mono=0`。第30帧 VPSS 输出 `chroma_min=128 chroma_max=128`，说明 ISP 源输出已黑白；显示路径另外只用 Y 生成等值 RGB。
- 转储无手掌覆盖的 IR 背景共409920像素，R/G/B最大差0、非灰度像素0。`ir-screen.png` 是实际显示合成帧、`ir-preview.png` 是背景转储，均非面板照片。用户实屏明确确认“黑白正常，注册成功并返回”。
- 原模板从 cfg 复制到 system 持久分区，迁移前后 SHA-256 均为 `2390383f6196a2d88e4c61943a664e47ddfc6a2acc3d264fae1aaebba272c90d`，原副本保留。
- 日志确认 `SOURCE switching IR -> RGB`、`ACCESS enrolled uuid=0 progress=10/10`、`SOURCE switching RGB -> IR`，保存的默认源仍为 IR。首次成功匹配 `score=0.7733`。
- 正常停止/重启最终版后，板端只读检查器报告 integrity=ok、users=1、registered_palms=1、pending_operations=0、uuid_mismatches=0、matched_events=2。SDK UUID0对应5个有效模板标志全为0。
- 用户最终实屏反馈“重启后识别正常，删除后不再识别”。删除后板端 users=0、registered_palms=0、pending_operations=0、pending_deletions=0，两个历史匹配事件的用户外键置NULL并保留身份快照。
- 用户随后明确批准复制完整数据库，说明当前为伪数据。通过在线备份生成一致副本后，主机`/tmp/ky50-access-check.sqlite3`检查通过：integrity_check=ok、foreign_key_check无错误，用户/凭证/操作均0，2条匹配与2条未匹配事件、创建/注册/删除审计仍保留；摘要为`database-check.json`。完整副本不放入交付包。
- 无手掌时采集/检测约17～18fps，有手掌并提特征时约12～16fps，屏幕提交约29～30fps；屏幕提交可重复最近一帧，不能等同于推理帧率。

## 验收边界

用户已确认实际注册、黑白显示、正常重启后的识别、界面删除及删除后不再识别。当前通过正常进程重启验证持久化，未做强制断电、冷启动、满容量板端长期试验。未安装开机服务、更新内核/设备树或固件；仅程序使用 SSH+NFS，用户数据写入持久化分区。

人脸、指纹、卡、密码保留入口，不实现注册。识别写考勤并显示身份，不自动开锁；权限策略、考勤排班、记录查询/导出尚未实现。具体 schema、数据恢复和容量行为见 `../../USER_DATABASE.md`。

板端只读统计：

```sh
/mnt/data/opt/ky50_access_20261111/access_inspect /mnt/system/ky50-data/users.sqlite3
```

检查器默认只输出数量、一致性和完整性，不导出姓名、工号或模板。显式使用`--backup NEW_FILE`时通过SQLite backup API生成0600权限的一致副本，不覆盖已有文件。不要直接复制运行中的SQLite文件作为一致性备份。

## 手掌质量状态与颜色修订

- 当前NFS目录：主机`/home/bonoy/NFS_Files/ky50_quality_20261111`，板端`/mnt/data/opt/ky50_quality_20261111`；日志`/tmp/ky50-quality.log`。
- 新应用SHA-256：`f94f2eec7be6edcce440bdd0d17f0b031f879e9b7cca8e075ca7eb7cc46f721b`。libpva/libdeviceCerf、模型、授权及配置内容与上表一致。
- 每个`pvaGetHandPose`检出帧均调用`pvaGetFeat`刷新结构体status；原先业务节流、识别去重和离开首页不再阻止显示质量检查。非`STATUS_OK`立即返回，不执行该帧注册/识别；SDK异常退出也不会继续业务。
- 框与全部7个点统一颜色：OK=`#36ff70`，非OK=`#ff4848`。未检出时不提特征、不绘制。左上角显示例如`STATUS_TOO_DARK (8)`，正常/未检出/预览超时隐藏，不截获触控；纯色屏幕诊断页保持纯色。
- 提特征会改写内部旋转ROI坐标，因此绘制继续用首次检测的完整副本，只同步提特征后的status。屏幕映射不修改算法结果，图像和绘制状态在同一把锁下发布。SIGUSR1转储也保留该坐标/状态组合。
- ASan/UBSan：覆盖全部23个已定义状态的统一颜色、原坐标不变、非OK不调用注册/识别、注册中间坏帧不增长、节流/去重/业务停用时仍刷新质量、未检出不提特征。主机LVGL指针回归和状态提示显隐/跨页测试通过，日志见`evidence/quality/host-*.log`，主机图`host-status.png`为合成背景。
- 板端启动和哈希一致；原IR默认配置保留，ISP色度仍为128。实际出现STATUS_OK绿色、STATUS_FAILED/TOO_BRIGHT/TOO_TOP等红色、未检出隐藏的切换；无手掌的实际IR背景409920像素中非灰度像素0，未残留框/点。
- 用户实屏确认“颜色、状态提示和进度控制都正常”，对应未检出消失、坏帧红色且显示状态、好帧绿色，以及红色时注册进度暂停、绿色时继续采集。实际转换日志与汇总见`evidence/quality/board-final.txt`、`board-summary.json`。不将主机合成图或状态日志代替此次实屏确认。

## 参数设置修订（2026-11-11）

- NFS 包 `ky50_parameters_20261111`；运行 SHA256 `d06fb1001aa15dc04c9b7d724c206d6269dd40ea4b10a4be86900d3c476971ab`。
- 默认 ROI 亮度 60-160，两个 offset tolerance 为 0.01；SDK 通用/CV184X 配置、静态库交付配置、应用配置和 NFS 包已同步。
- 高级设置 → 参数设置提供检测/识别阈值（0.00-1.00，0.01 步长）及动态等级（0-4）。分别调用 SDK 的 `pvaConfigSetHandThreshold`、`pvaConfigSetAuthThreshold`、`pvaConfigSetDynamicDetectionLevel`。
- 主机验证真实 LVGL 点击、保存/取消、上下限、重新进入、重启恢复；服务测试覆盖越界拒绝、注册中拒绝、参数写入/接口失败回退，旧质量门控/用户/注册流程回归通过。ASan/UBSan 开启，detect_leaks=0。
- 用户实际保存了检测阈值 **0.70**、识别阈值 **0.50**、动态等级 **1**，确认“全部正常”。正常退出重启后，`ACCESS parameters loaded source=database hand=70 auth=50 dynamic=1`；SDK 运行 JSON 与之一致，四项新默认值仍正确。保留用户设置。
- 证据目录 `evidence/parameters/`：`host-access.log`、`host-ui.log`、`board-before-restart.log`、`board-restart.log`、`summary.json`。`host-parameters.png` 是主机参数页渲染；`board-home-quality.png` 是用户返回首页后采集的实际 IR/质量状态画面。参数页实物显示和手指触控由用户确认。
- 库、模型和授权逻辑未改动；现有静态库已包含三个设置接口。当前验证是应用正常重启，未做断电测试；仍采用 SSH+NFS，不修改启动服务或烧录固件。

## IR ISP 曝光反馈（2026-11-11）

- 包 `ky50_ir_ae_20261111`，程序 SHA256 `46c868498eac775568ca54f4d058a729a4406f9a3841c90acac03f48baac7502`，日志 `/tmp/ky50-ir-ae.log`。
- 只在 IR 检出手掌的帧上，以 post-`pvaGetFeat` status 驱动 pipe0 的 `stAuto.u8Compensation`。连续两帧过亮/过暗才按方向调节，每次等待 500ms，方向反转减小步长。ISP get/set/readback 均检查返回值；无手掌时不调用曝光读写。RGB、灯、阈值不改变。
- 只有首页实际确认匹配成功才保存目标到 `device_meta.pva.ir_ae_target.v1`，同值不重复写盘。下一轮及重启后首次检出再恢复。读盘发生于启动，ISP 下发必须等待真实检测。曝光收敛等待期暂停本帧注册/识别，保持原非 OK 禁止业务和绘制副本约束。
- 主机 ASan/UBSan 策略测试覆盖过亮下调、过暗上调、未检出/其他状态不调整、两帧确认、限速和等待、反转衰减、边界、API 失败、成功记忆和重启；服务回归验证实际匹配才保存；既有参数、用户、质量门控和 LVGL 触控测试通过。detect_leaks=0。
- 实机过亮 target **56→52→48→44→40→36→32** 后变为 OK 并识别成功，成功时曝光时间 **4474us**、增益均 1024（1x）。后续复用 32 多次匹配。正常重启先读到 32，连续约 50 秒无手掌没有曝光下发；首次检出后日志 `reason=restore_success target=56->32`，等待后状态 OK 并识别成功。
- 用户第一次确认“是的，目前全部正常”，重启后确认“仍可正常识别”。当前保留成功目标 32 及用户原阈值设置 0.70/0.50/动态等级1。亮度阈值仍 60/160，offset仍0.01。
- `evidence/exposure/` 包含主机策略/服务/UI 日志、板端重启前后日志和 summary。日志 `luma` 是 ISP 全局统计，与 PVA ROI 灰度均值不是同一测量。此次实机日志没有自然出现过暗调节，过暗方向通过自动测试；没有做断电测试，部署仍为 SSH+NFS。

## 连续20帧离开与成功识别锁（2026-11-11）

最终行为以用户补充为准：识别成功后继续 `pvaGetFeat`，只停止 `pvaGetAuthRes`。当前进入确认2帧true，离开确认20帧false。成功后提特征/质量颜色/ISP反馈继续，直到20帧离开才允许下一轮识别；短暂1-19帧漏检不重做进入滤波，false帧无框。取帧/检测错误打断计数，不当作离开。页面、参数、时间不解除成功锁。

- NFS：`ky50_session20_20261111`，日志 `/tmp/ky50-session20.log`。
- 程序 SHA256：`012c3612d91702b577e3e7c7d256fdaca0d5527f3e9ca79ce76d361f82f0c74d`，包和板端执行文件一致。
- ASan/UBSan 服务测试通过2帧进入、19帧保持/20帧解除、连续计数被异常打断、成功后持续提特征但auth不调用、页面/参数切换及既有注册/数据库回归。UI代码未因20帧调整改变，真实LVGL事件与像素测试通过，状态叠层已移除；颜色和曝光策略回归通过。detect_leaks=0。
- 实机 `PALM_PIPELINE` 例：成功时 features=10/auth=2，保持手掌后 features=49、106、141，auth 始终为2。日志审计8次成功与8次20帧离开，成功锁内auth计数变化为0；锁内仍记录21次红色/29次绿色状态转换。新一轮重新出现 `entered true_frames=2` 后才继续auth。
- 用户先确认 STATUS/红外检测文字已消失，将离开判定改为20帧后确认“这次正常了”。当前保留20帧版运行。证据目录 `evidence/session20/` 含主机测试、板端事件日志、实际屏幕、summary.json。
- PVA公共接口、模型、授权、灯光、算法阈值和用户数据保持原有逻辑。原坐标副本绘制与IR输入路径继续使用。仍为SSH+NFS部署，未进行固件集成。
