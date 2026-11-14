# 本机用户、人脸、掌静脉与考勤数据

## 文件与关联

运行目录默认 `/mnt/system/ky50-data`，可用 `--data-dir DIR` 替换本机用户数据目录；PVA 的数据库路径仍由 `--pva-config FILE` 内的 `database_path` 单独决定，测试时须同时配置用户资料目录与两种 SDK 模板路径。

| 文件 | 用途 |
| --- | --- |
| `users.sqlite3` | 本机用户、注册状态、待恢复操作、识别考勤、审计记录 |
| `pva/pvis_cv184x_fused_20261111.bin` | 原 SDK 维护的掌静脉模板，公开 API 不变 |
| `pva/pvis_face_1000_3tpl_20261113.bin` | 人脸 INT8 模板，默认每人 3×1024 字节，与掌静脉分开 |
| `avatars/` | 头像路径预留；人脸注册提取模板，不保存照片 |
| `/mnt/cfg/ky50-ui/pva/deviceID.key` | 原设备授权文件，规则/地址/读取逻辑不变 |

用户数据位于 `/mnt/system/ky50-data`；`run.sh` 不再从旧目录导入模板。schema v1/v2 的旧用户、凭证、待办、考勤和审计记录会直接删除，并清空两种 SDK 模板；清空失败会保留重试标记并暂停业务。新 schema v3 数据在正常重启时保留，设备参数不随用户数据清空。

业务编号与模板编号分开管理：

- `users.id`：SQLite 自增的永久人员记录编号；删除人员后不复用，供凭证外键和考勤关联使用。
- `users.local_id`：界面与命令使用的资料编号，与 SDK 无关。本机资料容量继续为 100 人，删除资料后该编号可复用。
- `credentials.template_uuid`：SDK 返回的整数模板 ID。`kind=palm` 与 `kind=face` 的命名空间独立，各为 0～99；未绑定为 SQL NULL（应用结构体中为 -1）。唯一约束仅作用于 `(kind, template_uuid)`。
- `employee_id`：1～32 位纯数字工号，TEXT 唯一约束，保留前导零。

例如甲的 `palm_uuid=0`、乙的 `face_uuid=0` 完全合法；甲后来注册人脸时可以获得 `face_uuid=1`。识别和删除必须同时使用模态与模板编号，不能按 `local_id` 或裸 SDK UUID 查人员。

姓名可为空，最多 32 个 Unicode 字符/128 个 UTF-8 字节。本机完整保存；传给 SDK 的 `userName[33]` 按 UTF-8 边界截短，不能拆断汉字。

## SQLite schema v3

不提供旧数据迁移。清空旧库时先在新 SQLite 中提交 `templates_reset_pending=1`，随后删除两种 SDK 库的旧模板并核验空库；完成后才清除标记。断电或 SDK 不可用时，下次启动继续清空，不允许带着旧模板注册/认证。第一次处理旧库后，后续 v3 重启不会再次清空。

SQL 定义以 `access_store.c:store_open()` 为准。所有注册方式都有独立状态行，避免将“档案存在”和“已采集生物特征”混为一谈。

| 表 | 关键字段和含义 |
| --- | --- |
| `users` | `id,local_id,employee_id,name,is_admin,enabled,state`；部门、职位、公司、电话、邮箱、备注、头像路径、门禁策略标识、考勤组、有效期、创建/修改时间、最近识别时间/分数/次数、来源、外部编号、同步版本 |
| `credentials` | `user_id,kind` 联合主键；类型 face/palm/fingerprint/card/password；状态 0未注册/1注册中/2已注册/3模板缺失；模板 UUID、模型版本、左右手、注册/更新时间、错误、凭证引用、人脸方向、扩展 JSON |
| `operations` | local_id、enroll/delete、modality(palm/face)、template_uuid、创建时间；跨 SQLite/PVA 数据库操作的持久化恢复记录 |
| `attendance_events` | 用户主键及 local_id/模态模板 UUID/工号/姓名快照、方式、matched/unmatched、分数、UTC 时间、时间可信标志、单调时钟、设备标识、方向、是否已导出 |
| `audit_events` | 用户主键及身份快照、创建/注册/取消/删除/模板缺失动作、详情、UTC 时间及可信标志；保留最近 2000 条 |
| `device_meta` | schema 等设备元数据键值 |

目前界面编辑工号、姓名，并维护掌静脉和人脸注册状态。组织信息、其他凭证、有效期策略、权限配置、记录查询/导出、考勤排班是后续扩展字段；本版不宣称已经实现这些业务。密码不存明文字段，未来应存独立的加盐密码验证数据。

识别记录最多 10000 条：满额时只回收已明确标记 `exported=1` 的旧记录；不覆盖未导出考勤。无可回收记录或写入失败时，界面提示“识别成功，记录保存失败”，算法识别和记录保存结果分开。当前没有导出界面，达到容量后需接入导出维护。用户删除后历史记录保留身份快照，外键置 NULL，避免资料编号或模板编号复用后把旧考勤归到新人。

UTC 时间以系统时钟为准，早于 2024 年标记 `time_valid=0`；界面按 UTC+8 显示。该标志是最低日期合理性检查，不等同于已确认 NTP 同步。单调时间用于去重，不能跨重启当作日期。

## 事务、线程与恢复

SQLite 使用 foreign_keys、WAL journal、synchronous=FULL、secure_delete，文件权限0600、数据目录0700。业务状态、SQLite和ISP操作由 `algorithm_lock` 串行保护，检测线程处理新帧/命令，结果接收线程处理后台完成事件；特征/认证线程只做SDK计算。LVGL主线程只提交有界命令队列、读取加锁快照。

WAL启动时核验实际模式，256页自动checkpoint，空闲场景做PASSIVE checkpoint，正常关闭尝试TRUNCATE；长读事务会延迟回收。数据库必须保留在本机UBIFS，不放NFS；`-wal`可能包含最新已提交记录，不能直接单独复制主库。使用下文SQLite在线备份工具。切换WAL不改变schema和已有用户数据；仍在考勤写入尝试完成后发布成功事件。验证记录见 [结果交接与写库优化](evidence/image-resample/RESULT_LATENCY.md)。

注册顺序：根据模态调用 `pvaDataBasePickFreeId()` 或 `pvaDataBasePickFreeIdFace()`，检查返回值在 0～99 且该库尚不存在，然后先提交 `operations=enroll`（含独立模板 UUID）与 pending 凭证绑定，再调用 SDK 注册。查询本身不占 SDK 槽位，应用在业务锁内串行执行，每轮只分配一次，之后一直使用该 ID。持久保留同一个 `ResgisRes_t`，按 `progress/total` 更新进度。SDK 报成功后读回该模态所分配 UUID 的完整模板，才提交对应凭证的已注册状态并清除操作记录。掌静脉固定 5×1024 字节，调用 `pvaRegister(...,true,...)`；人脸按 `pvaDataBaseGetFeatSizeFace()` 分配（默认 3072 字节），调用 `pvaRegisterFace`，默认 1 帧采集、3 条模板。两种注册进度分开，一次只启动一种。只在同一模态内限制一个模板 ID 关联一个人员；跨模态允许同号。质量判断、设备授权和公开接口不变。

退出注册页、取消或 120 秒超时会清空 SDK 采集缓冲，删除未完成模板，撤销 pending；用户档案仍保留，便于重新选择注册方式。注册成功后返回类型页，恢复进入前的预览源，不改保存的默认预览设置。

删除顺序：先提交 deleting 状态和 delete 操作，分别按凭证记录的 `palm_uuid` 和 `face_uuid` 调用 SDK 删除模板，核验各自目标不存在，再删 SQLite 档案。任一失败均保留待办；取消某种注册只删除本次模态，不影响另一个已注册凭证。失败保留待恢复状态和各自模板绑定，不误报成功或释放槽位。重启后完成待删除操作；未完成注册若模板齐全则完成关联，否则清理部分模板。SDK 读写不可用时保留操作并禁用识别，避免带着不一致状态工作。完成旧库清空以后，新出现的无绑定模板只统计为孤立模板，不自动认领；SDK 空闲 ID 查询会避开它们，但不阻止创建本机人员资料。缺失模板解除该模态的绑定并标记需重新注册，以免其 ID 被新用户复用时串人。

库层补充错误传递：`pvaDataBaseGetFeat` 返回真实读取结果；注册写入失败以及写入/删除的 fsync 失败向调用者返回。未调整授权、模型算法或接口布局。

## 图像与识别

- 默认检测阈值 `model_probability_threshold=0.76`。
- IR pipe0 开启 ISP Mono，RGB pipe1 不开启。IR 预览只用 Y 生成 R=G=B，避免任何色度输入导致偏色。
- 注册页临时全屏 RGB 预览，掌静脉框/点使用 IR 结果，人脸框/点使用 RGB 结果；不增加小窗，不做双目配准。
- 掌静脉算法使用 IR 整帧半尺寸并逆时针90°：540×960、无裁剪。先复制 `HandPoseRes_t` 给绘制，再用原始结果和同一张 IR 图调用 `pvaGetFeat`。库内从 Y 生成 RGB_PLANAR，符合 fused-input 模型入口。
- 检出帧持续执行`pvaGetFeat`，直接检查其结构体status：非`STATUS_OK`不采集注册帧、不识别、不记识别事件；只更新红色框/点。OK时框/点统一绿色，按原业务节流继续。首次检测坐标副本只更新status，不采用提特征内部修改的ROI坐标。未检出时框、点清除。状态宏名和红外检测提示条已移除。
- 首页启用识别；其他业务页停用识别。连续两次质量合格且匹配同一 UUID 后显示姓名/工号，写入考勤。首次进入连续2帧检测true确认；成功后只停止`pvaGetAuthRes`，`pvaGetFeat`和实时框/点颜色继续更新。人脸和掌静脉分别按主应用配置的连续false帧数解除成功锁，默认均为5帧；少于阈值的漏检不重新做进入滤波。下一轮再次两帧确认；事件间隔仍至少3秒。未匹配也记录一次，避免每帧刷记录。当前不会自动操作继电器。

人脸使用 RGB pipe1 的完整紧密 NV12，Pose/Feat 必须同帧。两种 Pose 每轮独立检测、可同时绘制；原始手掌检测为 true 时不做人脸 Feat/认证，手掌质量失败也不回退人脸。只有原始手掌 false 才允许人脸认证；绘制防抖与业务判断分离。人脸注册质量/活体失败不增加进度；人脸考勤 `method=face`，掌静脉为 `palm`。见 [README.md](README.md) 的 2026-11-13 集成说明。

## 输入法和头像

姓名页提供26键 QWERTY、大小写、数字/常用符号、删除、返回、确认。中文按单音节无声调拼音选择汉字，5个候选一页，左右键翻页，`v` 代替 ü，空格选择当前首候选。不是整句联想输入法。字典覆盖408个音节、6756个 GB2312 字符；字体还含界面字符，共6854字形，20/24px。字典为本地独立 TSV，不依赖网络。

列表采用默认人形图标；未来 face 凭证已注册且 `avatar_path` 指向 `avatars/` 内可解码 PNG 时显示真实头像，限制128KiB、512×512以内。当前已实现人脸注册，但不保存人脸照片，继续显示默认图标。

## 构建与调试

```sh
# SDK/libpva_face
scripts/build_cv184x.sh
# SDK/cvi_mpi/sample_app/ky50_ui
make pva-sync
make -j8 package
make access-test overlay-test
make TARGET=host && ./ui_preview build/previews
# ARM pixel_check 在板端执行
```

以下为 2026-11-11 掌静脉版本的历史板测记录，不覆盖新的人脸集成。NFS 包 `/home/bonoy/NFS_Files/ky50_session20_20261111`，板端 `/mnt/data/opt/ky50_session20_20261111`。正常 stop 后运行 `nohup .../run.sh >/tmp/ky50-session20.log 2>&1 &`。本轮只通过 SSH+NFS 运行程序，持久化业务数据；未安装自启/修改固件。实际验证及文件指纹见 `evidence/access/VALIDATION.md`。

`access_inspect DB`默认只读汇总。需要备份时显式执行`access_inspect DB --backup NEW_FILE`，使用SQLite在线备份接口生成0600权限的一致副本，目标必须不存在；这只备份本机SQLite，PVA模板的协调备份另需先正常停止应用。用户已授权本次伪数据完整副本复制到主机检查，副本未打入交付包。

### 算法参数持久化

掌静脉的三个可调项以 `device_meta.key='pva.parameters.v1'` 保存，值形如 `76 52 2`（检测阈值 0.76、识别阈值 0.52、动态等级 2）。单条 UPSERT 是原子事务，沿用 `synchronous=FULL`，不修改用户/凭证/考勤记录。没有此键时使用本次发布配置的默认值。保存后立即应用；应用或保存失败时恢复原参数，界面提示失败。注册进行中拒绝更改参数。

本轮参数设置包为 `ky50_parameters_20261111`，日志 `/tmp/ky50-parameters.log`，仍按 SSH+NFS 运行。

### 识别成功曝光目标

`device_meta` 增加独立键 `pva.ir_ae_target.v1`，保存上次确认识别成功时 IR ISP 的 AE Compensation（1-255）。与算法阈值记录分离，更新不修改用户/凭证/考勤或原阈值。记录仅在新确认识别成功且值变化时写入；失败仍在内存保留并记录错误，下次成功重试。新一轮检测和重启后都只在检出手掌时恢复，不在无手掌阶段写 ISP。注册成功、未识别、质量不合格均不覆盖该成功记录。


### 人脸参数与主应用离场配置

人脸七个独立设置保存到 `device_meta.key='pva.face.parameters.v1'`，字段顺序为检测阈值、质量阈值、活体阈值、质量开关、活体开关、认证阈值、过滤阈值；浮点阈值使用百分位整数，开关0/1。例如默认 `50 30 50 1 1 24 24`。检测两个命名接口是同义接口，界面合并为一个字段。人脸保存与掌静脉保存独立，通过SDK setter立即应用；失败回滚，重启再恢复持久值。

离场计数不属于SDK配置。两个独立值以 `palm_leave_frames`、`face_leave_frames` 写入业务目录的 `app.conf`（默认 `/mnt/system/ky50-data/app.conf`），默认均5，范围1～100。文件有 `version=1`，不写SQLite或 `pvaConf.json`。保存成功即更新内存并清零两种漏检计数，成功认证锁仍等待新的连续漏检达到阈值才解除。UI分别显示两种帧数，并用独立“保存离场设置”按钮提交。所有参数保存均在现有算法线程中处理，不需要重启。
