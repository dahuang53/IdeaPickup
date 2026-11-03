# 设备直连：第一步接口与实施决定

> 本文记录该步骤通过审计时的北京百炼候选。第六步按用户决定切换为千问 AI 平台，当前接口及凭据配置见[第六步记录](DEVICE_DIRECT_CLOUD_STEP6.md)和[现行契约](contracts/firmware-direct-cloud-v1.json)。

日期：2026-11-02。适用基线：`6a89d6aea8b82207e705f8112fab35a5248a6d5e`（产品代码与 `cf05bb5` 相同）。本记录落实[七步计划的第一步](DEVICE_DIRECT_CLOUD_PLAN.md#步骤一-确定接口与改造边界)，是后续实现约束，不表示新协议已实现或真机已通过。

## 1 当前实现与复用依据

| 对象 | 本轮只读确认 | 决定 |
|---|---|---|
| 有卡音频 | [memo_store.h](../main/memo_store.h)／[memo_store.c](../main/memo_store.c)：16 kHz、单声道、16-bit little-endian PCM，44 字节 WAV 头；文件独占创建和恢复已存在 | 继续用原 WAV，不转码、不更名、不增加音频摘要 |
| 无卡音频 | [memo_cardless.cpp](../main/memo_cardless.cpp)：320 样本帧，Opus 编码；1 MiB 编码环、64 KiB PCM 缓冲、32,768 B 编码栈和 32,000 B 网络块 | 采集入口复用，直连先选 PCM，删除不再需要的无卡 Opus 编码任务和 ACK 保留环；USB／其他功能仍需的 codec 不删 |
| 调度与详情 | [network_sync.cpp](../main/network_sync.cpp)：单网络所有者，16,384 B 网络栈、10,240 B 详情栈；详情独立读取本地状态 | 复用互斥、任务通知和本地详情，替换协议执行；不为每个历史任务建立线程 |
| 状态与历史 | [memo_sync_cache.h](../main/memo_sync_cache.h)：状态 0—14；[memo_sync_cache.c](../main/memo_sync_cache.c)：164 B 双槽有卡记录；[memo_cardless_record.h](../main/memo_cardless_record.h)：228 B 无卡记录 | 原格式只读兼容；直连用新来源标记和最小检查点，不重写全部历史 |
| 无卡索引 | `memo_cardless.cpp`：最多 1024 个 16 B 索引，4 个完整详情槽；[partitions.csv](../partitions.csv)：`memo_meta` 为 256 KiB | 两种来源合计仍最多 1024 个索引和 4 个详情槽；物理分区先满则提前明确拒绝，不删除历史 |
| 配置与后台 | [memo_network_config_nvs.c](../main/memo_network_config_nvs.c)：`memo_cfg1`；[providers.py](../server-relay/relay/providers.py)／[app.py](../server-relay/app.py)：旧模型及云接口、180 秒分段和 12 秒安静边界搜索 | 旧配置不作为直连凭据；参考分段和投递语义，不搬服务器整块内存分配、broker 或账本 |
| 工具链 | [main/idf_component.yml](../main/idf_component.yml)锁定 ESP-IDF 5.5.1、音频组件及 LVGL；[sdkconfig.defaults](../sdkconfig.defaults)启用 CA bundle、PSRAM | 不升级 SDK、不改分区、不烧 eFuse；两板分别构建与验证 |

以上是源码事实。历史设备内存测量只作对比参考，不当作直连资源实测；本步骤不打开串口、不读实际凭据或录音。

## 2 接口选择

### 2.1 固定目标与模型

本轮只支持阿里云北京地域和思源云端收集箱，不新增任意供应商 URL、地区菜单或聊天模型。千问使用用户北京地域 API Key 和业务空间 ID；`{WorkspaceId}` 是待配置的非秘密值，不是本机现值。工作空间 ID 在保存时限1—63个ASCII字节，首尾须为字母／数字，中间允许短横线；存储缓冲64 B含结尾NUL。它拼入固定域名，不接收完整主机名；限制依据[DNS标签规则](https://www.rfc-editor.org/rfc/rfc1035.html#section-2.3.4)。

| 用途 | 冻结选择 | 权限与结果 |
|---|---|---|
| 有卡文件识别 | `POST https://{WorkspaceId}.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/chat/completions`；`qwen3-asr-flash-2026-02-10`；`stream:false` | `Authorization: Bearer <key>`；完整响应取 `choices[0].message.content`，要求字段为字符串、UTF-8 合法且 `finish_reason=stop`；空内容按下述项目规则处理 |
| 无卡实时识别 | `wss://{WorkspaceId}.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime?model=qwen3-asr-flash-realtime-2026-02-10` | 相同 Bearer 鉴权；仅收识别文本，不请求音频回复 |
| 思源投递 | `POST https://ld246.com/apis/siyuan/inbox/addCloudShorthand` | `Authorization: token <token>`；JSON `title`、`content`；HTTP 2xx 且合法 JSON `code` 为数值 0 才记成功 |

选择来自[千问模型清单](https://help.aliyun.com/zh/model-studio/asr-model/)、[文件识别接口](https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference)及[实时接入说明](https://help.aliyun.com/zh/model-studio/real-time-speech-recognition-user-guide)。空间专属域名是文档当前推荐形式；旧共享域名不作为自动回退。快照不可用、权限不足或地区不匹配时清楚报错，不静默换模型或地域。

### 2.2 有卡请求、分段与检查点

文件请求使用一个 `user` 消息，内容为 `input_audio`，`input_audio.data` 为 `data:audio/wav;base64,...`；不传聊天提示词，不打开 SSE。合法错误包含 HTTP 状态和供应商短错误码；不在用户界面或日志保留鉴权、请求正文或音频。

官方限制为单段不超过 5 分钟、10 MB；Base64 膨胀也要计入预算。项目选择最长 180 秒：`44 + 180×16000×2 = 5,760,044 B` WAV，Base64 为 `7,680,060 B`。请求 JSON 固定开销另限 2048 B，低于 10,000,000 B。只按块读取／编码并计算 Content-Length，不能分配整个请求。

沿用旧后端的边界方法：每段最长 180 秒，在最后 12 秒按 20 ms PCM 小块比较绝对幅度和，取最低能量边界；不足一段直接到结尾。以样本偏移保存**实际边界**，各段连续、不重叠、不留缝；只读这段边界窗口，不扫描全部文件。每段生成临时 WAV 头后流式发送，无需落地一份分段音频。

有卡检查点放在 WAV 同目录：`direct_r1_<10位编号>.job.0/.1`，两个不超过 512 B 的显式版本／CRC／代次槽；文本为同名前缀 `.text`。持久字段统一限定为：编号／文件长度、用户状态、ASR或DELIVERY阶段、有效意图／暂停原因、提交代次、Qwen与思源各自配置代次、下一样本偏移／已确认文本长度、当前段ASR已用尝试数／思源已用尝试数、取消前是否可能投递、短错误。文本总量限 256 KiB，RAM 只保留小块。

每段确认后先追加文本并同步，再推进检查点；重启按已确认文本长度截掉未提交尾部，最后一段最多重新请求。不是每包写盘；状态不变不写。文本读不到、长度不足或元数据写失败时停止任务，不默默重新转写全部内容。成功后仅留最终状态并删除临时文本；取消后删除本次中间文本，原 WAV 保留。失败保留已确认文本以便手动重试；每条录音最多一份，下一次提交复用或按明确规则重置，不能按重试次数增加副本。

结构完整、`finish_reason=stop` 且 content 为合法空字符串或仅空白时，**按项目无语音策略**确认空段并推进样本偏移；这不是额外的供应商静音证明。content 缺失／类型错误、非法UTF-8、非stop、响应截断／超限均失败，不能当静音跳过。全部段均空时最终为 `REJECTED/未识别到语音`，不投递、不自动重试，有卡仍可手动提交。其他非空文本依正常流程处理。不移植与新模型无关的过度质量启发式；真实分段质量在步骤六验证。

### 2.3 无卡协议与失效边界

使用 PCM16、16 kHz 单声道，100 ms 一块。收到 `session.created` 后发送 `session.update`，设置 `input_audio_format:pcm`、`sample_rate:16000`、`turn_detection:{type:server_vad,threshold:0.2,silence_duration_ms:800}`；收到 `session.updated` 才开始采集。本轮不使用需要客户端断句的 Manual 模式。

`input_audio_buffer.append.audio` 使用小块 Base64；官方明确没有逐块 ACK，因此写入 socket 不能解释为云端已持久保存。正常停止严格依次执行：停止采集并冻结末尾样本 → 排空本会话PCM队列、发送不足100 ms的尾块（按实际偶数字节数，不补假音频）→ 确认所有发送调用完整写出 → 发送 `session.finish` → 收齐对应最终条目及 `session.finished` 后关闭。停止后的30秒预算含排空，排空本身最多5秒；尾块写不全或超时即失败，不能投递部分正文。取消／断线不按正常停止排空投递。

以 `item_id`／`previous_item_id` 维护至多 16 个未结条目的次序；同条临时结果替换，最终 `transcript` 每条只入一次正文。缺条、乱序超过容量或单条识别失败均不能报完整成功。协议依据：[客户端事件](https://help.aliyun.com/zh/model-studio/qwen-asr-realtime-client-events)、[服务端事件](https://help.aliyun.com/zh/model-studio/qwen-asr-realtime-server-events)。

无卡只在本次联网会话内发送和收尾；不建立新会话“续传”已丢音频。意外断线、队列满或停止等待超时即明确失败，不投递看似完整的部分正文。结束／失败／取消后释放音频和文本；NVS 只存有界状态，无音频、全文或逐包进度。启动按§3的优先级处理：可能已投递先记不确定，其余未结束无卡录音记中断；不恢复云会话。**这不是断网／断电恢复能力。**

### 2.4 思源结果、不确定与频率

请求沿用本项目标题／正文格式（编号、导入时间与转写正文），不新增设备全文阅读；标题不超过 128 UTF-8 字节。正文按块 JSON 转义，避免复制一整份转义后的文本。空内容、超限或未完成转写不投递。

[官方收集箱说明](https://github.com/siyuan-note/siyuan/blob/d3af9d5782082b379d45a69eedf4ebcfe8ba6889/app/guide/20210808180117-czj9bvb/20200812220555-lj3enxa/20230805222417-2lj3dvk/20230808120348-orm8sjf/20230808120348-hynr7og.sy)给出 token 请求格式及每天最多 128 次插入；未提供本接口的任务幂等键、收件查询或远程撤回契约，也未在该说明给出正文大小。项目自行采用 256 KiB 正文上限，不能把它称为官方保证；供应商拒绝必须如实返回。有卡可保留正文供稍后处理，无卡失败后仅保留状态。

不为配额建立新设备账本；本设备串行投递，相邻请求至少间隔 10 秒，遇明确限流走下节有界退避。其他客户端可能共用配额，不宣称设备能保证全账号不超额。

投递前持久写 `DELIVERING`。明确拒绝可以失败；已写出任何 HTTP 请求数据后失去有效响应、响应截断、HTTP 5xx，或重启发现 `DELIVERING`，均记 `UNCERTAIN`。不自动重发，也不提供普通提交按钮；提示去思源确认。只有确定一个 HTTP 字节都没发出时可按连接失败重试。没有供应商契约支持的“查真实任务”或“取消云任务”入口不伪造实现。

## 3 状态、重试与用户动作

复用 `memo_job_state_t`，状态语义由来源区分。直连不使用旧 `RECEIVED/CHECKING` 表达云任务收据；不修改旧格式中原状态编号。少量检查点字段表达阶段，避免新建通用任务框架。

| 逻辑状态 | 复用值／补充字段 | 允许动作与后续 |
|---|---|---|
| 本地保存 | `LOCAL`，无自动意图 | 有卡可提交；仅本地时不运行 |
| 等待联网／排队 | `QUEUED/WAITING`，有效处理意图 | 有卡恢复同次提交；可取消，不扫描其他 WAV 补交 |
| 转写中 | `UPLOADING/TRANSCRIBING`，阶段为 ASR | 有卡完成一段再检查点；无卡仅当前流；可请求取消 |
| 待投递 | `WAITING`，阶段为 DELIVERY | 有卡重用已确认文本，不再次转写；可取消 |
| 投递中 | `DELIVERING` | 可请求停止，但可能已送达；以实际响应或不确定收尾 |
| 已送达 | `SENT` | 只显示结果，不再提交，不查询 |
| 失败 | `FAILED/REJECTED`＋短错误／阶段 | 有卡明确可重试才显示提交；无卡只显示状态 |
| 取消中／已取消 | `CANCELLING/CANCELLED` | 所有者结束网络操作后确认；有卡取消后可重新提交，无卡不重试 |
| 投递结果不确定 | `UNCERTAIN` | 保留状态，有卡保留正文；不自动重发，不显示普通提交 |
| 因模式／配置暂停 | `WAITING`＋暂停原因 | 不发请求；仅同配置恢复或用户明确重新绑定才继续 |

### 身份与幂等的最小边界

- 录音身份沿用设备持久编号及当前文件名／长度；详情不读音频正文。接受同编号同长度的外部替换可能显示旧状态的既有取舍。
- 一次提交以该录音的递增提交代次区分；双击只接受第一次。请求 ID 仅用于诊断，不冒充供应商幂等键。
- 自动录音只在启用联网转写且保存成功后产生一次意图；录制时为仅本地则不产生。升级、配对／配置和浏览详情均不补交历史。
- 有卡只重试未确认段，成功段不重复。旧中继“服务器已接收则只查同任务”的规则仅对旧来源保留，不能虚构为新文件接口的能力。
- 无卡历史不提供提交／重试；处理中可取消，终态仅状态。只存本项目必要状态，不把云端全文存入 Flash。

### 重试与时间策略

| 情况 | 决定 |
|---|---|
| 无网络但有卡意图有效 | 进入等待联网，任务休眠直到网络事件；可长期保留**用户可取消的状态**，不是无限 HTTP 等待或忙轮询 |
| 有卡 ASR 对时／DNS／TLS／连接失败、408／429／5xx／响应丢失 | 每个未确认段最多首次加 2 次自动尝试，间隔 5、30 秒；**每次请求发出前持久增加已用尝试数，提交成功才允许请求**。请求中断电也计一次；额度耗尽停止，等手动提交。未确认段可能重复计费，不能承诺零重复 |
| 凭据／模型／参数错误或响应非法／超限 | 不自动循环；保存失败原因，修正后有卡可手动提交 |
| 正式下次 Wi-Fi 事件 | 不能重置同一次提交已耗尽的重试预算；预算仅由显式再次提交或下一段成功推进后重置 |
| 无卡启动失败 | 不开始录音，显示原因；用户下一次录音是新操作，不后台连试 |
| 无卡流中断／背压 | 最长 2 秒未能推进且队列接近满即停；不重建会话、不自动再录 |
| 思源对时／DNS／TLS／连接失败且确定零HTTP字节发出，或明确429拒绝 | 两类失败共用同一个持久思源尝试数，总计最多首次加2次，按10、60秒退避；请求前持久消耗额度，不按错误类型或重启重置。有卡耗尽后失败保留正文等手动；无卡在本次收尾上限内才重试 |
| 思源其他明确 4xx／业务拒绝 | 不自动重试，失败时有卡保留正文；没有成功副作用保证的响应按不确定处理 |
| 思源请求可能已发出后连接异常／5xx／非合法成功响应 | `UNCERTAIN`，不自动重发 |

取消只控制本地调度和关闭连接，不保证撤销已发出的识别费用或笔记。有卡取消转写时清除本次中间文本并保留 WAV；下次提交从头。若取消与思源成功交错，成功优先；失去响应为不确定，不能覆盖成已取消。

退避遇合法 `Retry-After` 时取比基础等待更长的值；超过300秒则停止自动重试并显示限流，不能提前300秒就请求。无卡所需等待若超过剩余120秒收尾预算则直接失败。为跨重启保住退避，不保存逐任务计时器：若启动时当前段已有尝试但无结果，先联网对时，并至少重新等该段相应5／30秒；已明确限流而重启，保守再等300秒，无法对时不发请求。单条检查点保存一个限流标志即可，耗尽次数不因重启重置。

### 启动转换优先级

启动没有可继承的在途socket；先完成以下持久转换，失败则锁住新云请求，不猜测为全新任务。

| 持久状态／来源 | 启动决定 |
|---|---|
| 旧中继来源（任何未终态） | 保持历史状态／待核对，不进入直连调度 |
| 直连 `DELIVERING`，或 `CANCELLING` 且取消前可能已投递 | 最高优先转 `UNCERTAIN`，不再提交；包括无卡，不能覆盖成一般“录音中断” |
| 有卡 `CANCELLING` 且无可能投递 | 完成取消，删除唯一临时文本，保留WAV；清理中重启继续清理，不恢复ASR／投递 |
| 无卡 `CANCELLING` 且无可能投递 | 完成取消并释放活动标记，不恢复流 |
| 无卡采集／ASR／待投递等其他未终态 | `FAILED/录音中断`，无正文可恢复，不投递；终态写入成功后再清活动键 |
| 有卡ASR／等待阶段 | 校验小检查点与文本长度，按已持久尝试数继续；同配置、意图有效且未暂停才运行 |
| 有卡DELIVERY待投递阶段 | 使用已确认文本，按思源剩余额度继续；不得重新转写 |
| 终态 | 只读展示／完成幂等临时清理，不请求云端 |

取消前是否可能投递的小字段在持久进入 `DELIVERING` 时同时置位；取消不能先覆盖掉这个事实。有卡保持原双槽持久方式；无卡同一个不超过128 B的记录先持久状态再更新／清理活动键，重复启动按真实已写状态处理。

### 模式与配置变更

1. 切到仅本地：立即阻止新云请求；当前请求由所有者有界收尾。有卡保存检查点并标暂停，无卡正在采集则停止且明确“已中断”，不隐式变为有卡。已经发出的投递仍按真实结果／不确定记录。
2. 恢复联网转写：只恢复本直连来源、配置代次相同的已有有效意图；不处理仅本地录下的历史。若有卡在 ASR 请求中暂停而没有确认，下次仅重发该段并提示可能重复识别。
3. 修改空间、密钥或思源 token：录放／云请求进行中返回忙碌；空闲才能保存。固定使用Qwen／思源两个32-bit配置代次，各自仅在对应值实际变化时增加，达到最大值拒绝更新，不回绕；重复保存不写Flash。旧任务任一配置代次不符即暂停；不保存历代凭据或比较凭据哈希。
4. 用户在有卡详情明确选择“提交到当前账号”才重新绑定：Qwen 配置变化重新转写；只有思源凭据变化可用原确认文本，但页面必须说明目标已变。已送达／不确定／旧服务未决记录都不能用该按钮重发。不建立凭据历史表。
5. 密钥空输入保持不变；清除必须明确操作；任一凭据缺失禁止新联网任务。保存不测试账号、不调用千问或投递思源。

## 4 资源与硬性超时预算

以下数值为本轮工程上限，步骤二以后需据此实现并测量；不宣称已经达到。分配失败立即明确退出，不能自动放宽上限。KiB 为 1024 B。

| 项目 | 冻结值 | 原因／用尽行为 |
|---|---|---|
| 有卡音频块／Base64 块 | 3072 B／4096 B，另 2048 B 固定 JSON／头缓冲 | 3 字节编码边界；大文件不进 RAM |
| 网络 RX／TX 分片 | 各 4096 B，TLS 记录缓冲沿用锁定 SDK 配置 | 分片不是整响应；接收不足则累计到明确上限 |
| HTTP 响应／WS 完整事件 | 各至多 64 KiB；解析临时 arena 至多 192 KiB，放 PSRAM | 保证 JSON 节点／字符串也有上限，超限拒绝，不截断后成功 |
| 有卡单段文本／总临时正文 | 16 KiB／256 KiB（后者在 SD） | 超限停止；不因长录音把全部正文载入 RAM |
| 无卡 PCM 队列／发送块 | 65,536 B／3200 B（100 ms），Base64 块 4268 B＋最多 340 B JSON | 约 2.048 秒排队容量；背压 2 秒或满立即停止；删除旧 1 MiB ACK 环 |
| 无卡最终文本／临时预览 | 256 KiB／16 KiB，PSRAM；未结 item 至多 16 个 | 满则停止并明确失败，不静默丢文本；不入 NVS |
| 新协议增量 PSRAM 峰值 | 相对不含旧 ACK 环／编码任务的录放基线不超过 768 KiB | 包含上述队列／文本／事件／解析 arena／I/O及必要 TLS 堆；超出则修实现，不能只看总剩余 PSRAM |
| 应用／WS 任务栈 | 网络 16 KiB、详情 10 KiB、deadline 2560 B 沿用；唯一 WS worker 8192 B | 无卡删除原 32 KiB Opus 栈；不按任务数量开线程；每栈真机余量至少 2048 B，deadline 至少 1024 B |
| 内部堆／连续块下限 | 最低空闲内部堆 48 KiB；最大连续内部块至少 24 KiB | 两板和 AP 并发均检查；是准入预算，历史测量不算直连 PASS |
| 活动连接 | 最多 1 个出站供应商 TLS 会话；AP 浏览器沿用 **4** 连接 | 锁定lwIP为10 sockets，HTTPD另占3内部socket；4＋3＋1＝8，余2供DNS／其他现有网络活动。保持LRU与既有容量回归，不提高上限；WS关闭后再投递；AP HTTPD 8192 B 栈保持 |
| 无卡历史／详情 | 总计 1024×16 B 索引、4 详情槽；单条直连持久记录至多 128 B | 与旧 228 B 历史共用原分区；分区或索引满即拒绝新无卡任务，终态不自动删 |
| 配置／密钥 | Qwen key 256 B、思源 token 256 B、workspace有效值≤63 B（缓冲64 B含NUL）；NVS单blob不超过1024 B | 长度按UTF-8字节校验，不截断；仅一组当前值，无凭据历史；物理条目预算见§5 |
| 有卡 checkpoint | 每条双槽各至多 512 B；最多一个网络活动任务 | 发现 pending 用原流式目录遍历，不新增全卡常驻任务索引 |
| SNTP／DNS-TLS 建连 | SNTP 共 15 秒；每次连接共 15 秒 | 证书时间和域名校验均开启；失败不能跳过验证 |
| 有卡文件请求 | 全请求硬期限 240 秒；单次阻塞 I/O 最多 10 秒 | 包括发送及读取，不用每次读到一点就无限续期；取消标记2秒内禁止排新块，在途SDK调用最长10秒后由所有者收尾，不跨线程释放句柄 |
| 无卡 WS 开始／结束 | 开始含配置确认15秒；停止后最多30秒，含最多5秒排空尾音 | 失败不报完整，关闭由网络所有者执行；SDK自动重连关闭；写不全即失败 |
| 无卡心跳 | 活动会话 ping 10 秒、无 pong 20 秒；已结束不保活 | 只在确有录音／收尾会话时保持连接 |
| 思源请求／无卡总收尾 | 单次硬期限 30 秒；无卡停止后总计最多 120 秒 | 包括 final、投递及可重试退避，超过即失败／不确定并释放文本 |
| 有卡临时数据淘汰 | 完成／取消即删除文本；失败保留每录音唯一文本，上限 256 KiB | 不按尝试建副本；正文与 checkpoint 不一致时停止，不创建“修复缓存” |
| 空闲任务／页面 | 无 pending 等待通知；网页隐藏不查询，可见处理中 2 秒、稳定 10 秒 | 使用既有策略；没有旧服务查询、每历史条目定时器或无变化写入 |

有卡请求失败后退避和离线休眠不占连接；每次尝试**开始前**消耗持久额度，结果和阶段改变时更新，无变化不写。配置代次及意图也是小字段，不能由此引入通用调度／持久数据库。

## 5 配置输入、保存与升级回退

### 凭据渠道决定

保持现有开放热点、手动 Safari 页面和既定视觉；**长期供应商密钥不通过该明文 HTTP 提交**。采用电脑 USB CDC 进行一次性隐藏输入配置：复用串口控制通道，步骤五增加一个有界的专用输入命令和本地助手。助手使用隐藏输入，值不出现在命令行参数、shell history 或磁盘文件中；设备不回显输入，只返回保存结果和“已配置”标志。USB 连接的电脑被视为可信，不能称 USB 加密或抗本机恶意软件。

现有 `app.c:console` 只有 96 B 行缓冲，不能直接塞两把密钥；专用命令进入最大 1024 B 的单次有界接收，15 秒无输入清空退出，溢出整次拒绝。只在空闲及设备主动配置状态接收，不将普通串口诊断变成秘密打印入口。网页展示配置是否齐全、模式、Wi-Fi 和真实任务状态，提供清除／引导；没有向开放 HTTP 发送密钥的隐藏备用接口。相比现流程，直连用户需在电脑上完成一次凭据写入及以后换 key；这是明确的技术限制，不改成要求每次配网输入热点密码。

凭据写入默认 NVS 的独立 `memo_cloud1` 命名空间，沿用现有**单blob＋版本／CRC＋commit＋关闭旧handle后精确回读**。不在NVS之上再做SD式双槽；只在配置变化时写，不遍历全分区。写入／提交／回读失败后，必须用新handle重载确认当前真实值，确认前禁止新云请求；不能继续使用未经确认的RAM旧值。NVS 未加密的物理读取风险仍存在；本轮不烧安全熔丝，不声称密钥具备硬件防提取能力。生命周期结束清除临时输入和持有密钥的可变缓冲；不得写日志、源码、浏览器存储或公共交付包。

### NVS物理容量与准入

依据锁定[IDF v5.5.1 NVS API](https://github.com/espressif/esp-idf/blob/v5.5.1/components/nvs_flash/include/nvs.h)、[页结构说明](https://github.com/espressif/esp-idf/blob/v5.5.1/docs/en/api-reference/storage/nvs_flash.rst)及[writeMultiPageBlob实现](https://github.com/espressif/esp-idf/blob/v5.5.1/components/nvs_flash/src/nvs_storage.cpp)：每页4096 B、126个32 B条目；单chunk blob为 `2＋ceil(长度/32)` 条目。1024 B可能跨页成为两chunk（两个数据头＋一个索引），最坏35条目，不能只按34；128／228 B小记录在不足尾部先换页，仍按6／10条目。另需namespace、更新及GC空间。默认NVS是4页（16 KiB），已有Wi-Fi最多10组、旧配置和编号共用，不能以“有16 KiB”当成可用空间。

| 分区／操作 | 保守预算及准入 |
|---|---|
| 默认NVS首次创建云配置 | 1024 B blob最多35条目；为当前＋替换各35、namespace1、GC整页126，要求保存前 `free_entries≥197`。这是上限预留，不分配197条目；不足拒绝配置，不擦旧命名空间 |
| 默认NVS更新云配置 | 当前blob已占用，要求剩余 `free_entries≥162`（35替换＋1保守namespace余量＋126 GC）；还须SDK实际set／commit／新handle回读成功。重复相同配置不消耗条目 |
| 默认NVS分配新录音ID | 现有16 B编号blob占3条目；保留namespace1及126 GC，要求分配前 `free_entries≥130`，未成功保留编号不开始无卡捕获／云请求；有卡沿用编号失败仍可本地独占保存但无自动意图的规则 |
| `memo_meta`新直连无卡任务 | 64页，总8064条目，预留GC一页后7938；旧228 B每条最多10条目，新128 B每条6，故混合历史不保证都存满1024。创建前要求 `free_entries≥158`（126 GC＋32个工作条目），并有索引空位。32含新namespace1、新记录6、≤16 B活动blob3，以及至少两次记录更新12与一次活动标记更新3，另留7条余量 |
| 无卡活动期间 | 单个metadata写所有者，不创建第二任务、不写旧历史；上述32工作条目专供本次创建／状态更新／清活动键，禁止其他对象挤占，开始识别／投递前先持久所需状态。容量不足或持久化失败锁住新云请求；旧状态留存，若投递可能发出而终态未写成功，持久DELIVERING使下次启动变不确定 |

`nvs_get_stats`只作准入预估，不能代替实际写成功；坏页／碎片／读写错误仍明确失败。记录和活动键不每包写入；更新复用原key、由NVS GC回收旧值，不靠删除用户历史挪空间。步骤四／五要用实际NVS容量夹具覆盖“开始前不足、开始后终态写失败、混合历史满”并在步骤六核对两板真实条目数；此处仅预算，未声称现机有这些空位。

### 升级和旧任务

- `memo_cfg1`、`records1`、旧 `sync_r1_*.job.*`、WAV、编号高水位均保留；不改变分区或强制格式化。新 `memo_cloud1` 缺失即仅本地、未配置，不读取旧 token 代替供应商 key。
- 新有卡仅写 `direct_r1_*.job.*`；新无卡使用原 `memo_meta` 内独立 `direct1` 命名空间和独立活动键；编号仍统一由 `memo_record_ids` 保留。旧记录只读且不自动恢复／查询，不将旧 active 标记解释为新协议。
- 已送达历史继续显示已送达。旧无卡未决记录显示“旧服务待核对”，无重试／新提交；旧有卡待核对同样不直接转交新接口。**bound=false不证明未上传**：旧代码发HTTP前先写WAITING，丢响应也可能未绑定。仅允许“合法WAV且确实没有侧车”或“有效旧格式且bound=false、状态仅LOCAL／QUEUED”的白名单由用户手动提交；WAITING／UPLOADING／RECEIVED／TRANSCRIBING／DELIVERING／CANCELLING／UNCERTAIN等均排除，坏槽／I/O错误不当作缺侧车。没有新授权意图不自动提交。
- 新侧车优先于该录音的旧状态；禁止同时创建一条旧 pending 意图。新直连结果不覆盖旧槽，旧解析器仅承担读取展示；不在固件中保留旧协议备用引擎。
- 未知版本／坏数据只报错，保留原件；不能当作全新录音自动提交。有卡恢复只读文件头／长度和已确认文本长度，无完整音频 SHA。
- 升级包先保全原应用、默认 NVS、`memo_meta` 和 SD 状态。回退使用**配套应用与数据副本**，不能直接原样回滚默认NVS：先只读保全回退前数据，取升级前floor、回退前floor、已保全新增有卡／无卡记录编号的最大已分配低31位值，用现有 `memo_record_ids` 16 B编码写入离线回退副本并解码回读；不能倒退／重用新direct1中旧固件不可见的编号。任一高水位无法可靠取得则停止回退，不猜零。副本中旧配置设仅本地，再恢复匹配应用／数据；原未改动备份另存保留。新增录音先保全，不覆盖新数据来制造可回退。
- 本步骤不执行迁移或恢复操作，不停止现有服务器、不撤销共用凭据、不改后端接口；历史待办在旧服务上的结果继续以旧服务为事实源。

## 6 参考版本与裁剪决定

| 来源 | 版本／许可依据 | 复用范围与排除 |
|---|---|---|
| ESP-IDF HTTP／TLS | [v5.5.1 HTTP API](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/protocols/esp_http_client.html)、[v5.5.1 示例](https://github.com/espressif/esp-idf/tree/v5.5.1/examples/protocols/esp_http_client)、[Apache-2.0](https://github.com/espressif/esp-idf/blob/v5.5.1/LICENSE) | 采用 open/write/fetch/read 的流式 HTTPS 与现有 deadline 取消；不关 CA、域名或时间验证，不升级 IDF |
| 官方 WS client | [websocket-v1.5.0 manifest](https://github.com/espressif/esp-protocols/blob/websocket-v1.5.0/components/esp_websocket_client/idf_component.yml)、[头文件](https://github.com/espressif/esp-protocols/blob/websocket-v1.5.0/components/esp_websocket_client/include/esp_websocket_client.h)、[Apache-2.0](https://github.com/espressif/esp-protocols/blob/websocket-v1.5.0/components/esp_websocket_client/LICENSE) | 依赖要求 IDF ≥5.0，适配本项目 5.5.1；步骤二精确锁 `==1.5.0` 并更新 lock。复用握手、分片与生命周期，设置 CA、固定栈、超时、关闭自动重连；不自己造 WS 帧协议 |
| 乐鑫通义示例 | [esp_llm_tongyi 0.5.0 示例](https://components.espressif.com/components/espressif/esp_llm_tongyi/versions/0.5.0/examples/esp_llm_tongyi_agent_app?language=zh) | 仅参考先联网／对时、会话与采集停止顺序。示例要求 IDF 5.5≥5.5.2（另支持 5.4≥5.4.3），与锁定 5.5.1 不直接兼容；不导入组件／示例源码、密钥默认值、对话/TTS/视觉栈，故不引入其代码许可依赖 |
| 小智 | [固定提交 a78ca19d](https://github.com/78/xiaozhi-esp32/tree/a78ca19d5bc9a12ee0853b9f509530276d36aa03)、[WS 实现](https://github.com/78/xiaozhi-esp32/blob/a78ca19d5bc9a12ee0853b9f509530276d36aa03/main/protocols/websocket_protocol.cc)、[MIT](https://github.com/78/xiaozhi-esp32/blob/a78ca19d5bc9a12ee0853b9f509530276d36aa03/LICENSE) | 参考回调不阻塞、连接异常收尾、采集所有权；不搬 hello/listen/stt、自有服务鉴权、音频二进制头或整套应用。若后续确实摘取代码，保留 MIT 声明并登记具体片段 |
| 当前配网与音频组件 | 项目 vendored `esp-wifi-connect` 3.3.1（MIT、IDF≥5.3）、音频及 LVGL 见锁文件 | 保留已修 AP＋STA、异步连接、手动 URL 与既定设计；只有无卡专属 Opus 任务移除，codec 库按其他消费者决定 |

API 文档为检索日的在线事实，选型与预算是本项目决定；真实请求和两板 SDK 兼容仍按后续步骤验证，不把文档阅读写成运行通过。

## 7 清理处置清单

此表是实施去向及证据索引，不表示已删除。每个替换／删除项都在对应步骤落实；完成标准只在主计划勾选。

| 编号／现有入口及调用者 | 类别与理由 | 负责步骤／验证入口 |
|---|---|---|
| C01 `network_sync.cpp:pairing_request/recover_pairing/refresh_service_status` → 网络工作循环；`memo_service_pairing.cpp:memo_pairing_run` → 前者 | 删除：中继配对、实例及设备 token 协议不用于直连 | 二、五；配置与 worker 回归、链接符号及无旧请求观察 |
| C02 `network_sync.cpp:remote_status/upload_file/sd_status/sd_upload/sd_retry/sd_cancel` → `sync_one` | 替换：文件识别／投递代替中继 CRUD，保留网络互斥 | 二、三；合成供应商、SD 流程及终态无请求 |
| C03 `memo_sd_sync.cpp:memo_sd_sync_attempt/receipt/control_token` → `network_sync.cpp:sync_one` | 替换：旧 receipt、远端重试／取消 token 不再适用 | 三；提交／取消交错、请求丢失、投递不确定 |
| C04 `memo_sd_sync.cpp:memo_sd_sync_pending` → `discover_sd_work` | 替换发现范围：只发现新直连意图，仍流式遍历 | 三；旧 WAV／旧 pending 不自动提交，重启恢复直连 pending |
| C05 `memo_sync_cache.c`／`.h` → SD 同步及详情 | 兼容保留旧 164 B 格式读取；新格式复用小记录双槽方法，不新增全音频校验 | 三；旧状态样本／坏槽／无变化写入／本地详情 |
| C06 `memo_cardless.cpp:TransferState`、ACK／GET／恢复／历史轮询 → `memo_cardless_network_step` | 删除旧协议，替换为实时 ASR 状态；不能为未来恢复继续运行 | 二、四；真实生产状态机合成 WS、启动中断及空闲观察 |
| C07 `memo_cardless.cpp:encode_task`、`AUDIO_BUFFER_BYTES/CHUNK_CAPACITY`、`memo_cardless_buffer.c/.h` → 无卡编码／ACK 所有者 | 删除无卡 Opus 编码、1 MiB 环及旧包 ACK；采集改接有界 PCM 队列 | 四；音频时序、满队列及资源对比；核对全调用树后删除包缓冲模块 |
| C08 `memo_cardless_record.c/.h`、`records1` → 历史列表／详情 | 兼容保留解码及旧状态，不恢复旧活动流；新 `direct1` 最小状态 | 四；混合历史／分区满／活动标记／1024索引4槽 |
| C09 `memo_cardless_state.c/.h` → 无卡传输及 UI 状态 | 替换协议专属迁移；复用终态、取消和错误分类中仍适用部分 | 四；每种失败／取消／终态资源释放 |
| C10 `memo_network_config.c/.h`／`memo_network_config_nvs.c` → 网络、配网及配对 | 替换直连配置；仅旧状态展示必需的类型／解码兼容，旧数据由快照保全 | 五；配置空值／更新／清除／重复保存及回退副本检查 |
| C11 `memo_service_portal.cpp:memo_portal_apply/memo_portal_status` 及内嵌 JS → 配网 handler | 替换中继 URL／配对字段、请求和状态；保留 UI 设计、Wi-Fi 及异步处理；密钥仅 USB | 五；浏览器 fixture／交错测试／无秘密 HTTP 接口 |
| C12 `app.c:console` → 本地维护／自动化 | 复用控制分派，增加有限秘密输入；清理旧协议专用诊断／故障注入命令 | 四、五；有界输入／无回显、原录放和USB命令回归 |
| C13 `memo_http_transport.cpp:MemoHttpTransport/deadline_worker` → 现有 HTTP 调用 | 复用硬期限和取消；WS 用官方客户端的所有者收尾，SDK事件回调不阻塞 | 二；deadline回归、断连／取消及任务余栈 |
| C14 `memo_record_ids*.c` → 有卡和无卡创建 | 复用统一编号和错误处理，无新编号器／导入机制 | 三、四；编号跨模式／重启和保全 |
| C15 `memo_store.c`、`app.c`存储／录放／探卡、`voice.c`、USB模块、`ui.c` | 复用；仅任务入口和状态提示必要调整，保留模式锁定及按需探卡 | 三至六；host／UI／USB及两板差异验收 |
| C16 `main/CMakeLists.txt`、`main/idf_component.yml`、`dependencies.lock` → 两板 build | 删除已无消费者的旧模块注册；新增唯一官方 WS 依赖；不盲删 mbedTLS／音频codec | 二、四、六；两板 lock／map／符号、产物大小及完整构建 |
| C17 `network_sync.cpp`旧 URL SHA／`memo_sync_target_t`绑定及音频摘要比较 → 旧协议状态校验 | 兼容保留旧字段解析；删除失效运行校验，直连只用小型配置代次，无完整／部分音频指纹 | 三、五；详情不读音频、无重复校验、旧数据不改 |
| C18 `tests/test_cardless_runtime.py`、`test_sync_pipeline.py`、`test_network_worker.py` → 现有测试入口 | 替换固件旧协议用例接线，保留行为断言；后端协议测试继续独立归属 | 二至六；`scripts/test_all.sh`及合成云故障矩阵，不以删断言通过 |
| C19 `scripts/test_service_config.sh`、`tests/test_service_portal.py`、`test_portal_async.cjs`、`test_service_portal_browser.cjs` → 配置／浏览器入口 | 替换失效中继字段预期，保持草稿、状态刷新、切后台和手动页断言 | 五、六；现有服务配置与浏览器回归 |
| C20 `README.md`、安装／部署／存储／支持／测试说明、`docs/contracts`及发布清单 → 用户／CI／打包 | 更新直连路径、凭据输入、限制和版本归属；移除固件失效引用 | 五至七；本地链接／契约／打包／新用户路径审查 |
| C21 `server-relay/`、生产部署、私有备份／录音、原固件 | 保留：独立后端及回退历史，不是本轮固件垃圾 | 全程；差异范围、数据保全及交付隐私核验 |
| C22 `network_sync_state.c/.h:memo_sync_remote_action/memo_sync_upload_response_pending/memo_sync_local_state` → 当前仅 `tests/test_network_sync_state.c`消费者 | 删除：失效中继响应语义及未被生产调用的展示辅助函数，连同专属测试接线移除，生产模块其他函数不整体删除 | 二；调用搜索与该C测试更新，旧协议函数无链接符号 |
| C23 同模块 `memo_sync_queue_*`／`memo_sync_device_attempt_done` → 网络工作循环及C测试 | 复用队列，替换“WAITING即本次发送结束”判断；按直连阶段及有效意图区分待投递／暂停，不能丢工作 | 三、四；`tests/test_network_sync_state.c`、worker及ASR→DELIVERY完整循环 |
| C24 `memo_network_config.c/.h:memo_config_activity_*` → `network_sync.cpp`配置提交／录放互斥，`app.c`媒体与探卡入口 | 复用现有活动门闩及短事务边界，不随C10清除；不跨网络持锁 | 二至五；`tests/test_service_runtime.py`、探卡及配置互斥回归 |
| C25 `network_sync.cpp/.h:network_sync_stream_request` → `memo_cardless.cpp`旧传输 | 删除旧中继无卡HTTP／分块包装及其旧SHA字段路径，不影响C13通用期限能力 | 二、四；cardless／worker回归、链接符号及无旧请求观察 |

最终候选应看不到已删除项的运行路径／构建注册；保留项按上述目的使用。清理必须随实现发生，不能只靠链接器裁剪或关闭开关。当前不执行任何删除。

## 8 本步骤验证与后续入口

本轮只做源码、官方接口、容量计算与状态场景走查；业务测试、实际云请求、设备和升级验证留在相应实施步骤。以下是已冻结的场景决定，不是未来实现的测试 PASS。

| 场景 | 已确定结果 |
|---|---|
| 开机／打开旧长录音详情 | 本地状态；无音频 SHA、无云请求、无重新提交 |
| 新有卡录音保存时仅本地／离线 | 仅本地无意图；联网模式离线保留有效意图 |
| 有卡两次点击提交 | 一次提交代次、一个执行所有者 |
| 第一段成功后第二段请求丢响应 | 前段文本不重做；后段有界重试，可能重复计费 |
| 文本同步后检查点断电 | 截去未确认尾部，最多重做未确认段；文件保留 |
| 思源成功后本地状态还未落盘即断电 | 重启见 DELIVERING → UNCERTAIN，不重发 |
| 取消与识别／投递交错 | ASR可停止后再提交；投递成功优先，否则按失败／不确定，不虚称撤回 |
| 替换凭据／切仅本地／再启用 | 在空闲更改，暂停或显式重绑定；不补交其他旧录音 |
| 无卡断线／断电／队列满 | 有限失败收尾，无补传／会话恢复承诺 |
| 录音中插卡／拔卡 | 沿用录制开始时模式；有卡写失败停止，不中途切无卡 |
| 历史或内存容量满 | 清楚拒绝新工作，保留原历史，不无界分配 |
| 隐藏网页／成功终态／无 pending | 页面停止查询／云连接关闭／任务休眠 |
| 开放热点收到云密钥字段 | 拒绝；USB专用通道不回显，保存不进行付费调用 |
| 旧服务器未决任务 | 旧来源展示／待核对，不直接重提交到千问 |

第一步完成标准与本记录的对应：接口→§2；状态与旧任务→§3、§5；模式／凭据变更→§3、§5；资源与超时→§4；凭据／回退→§5；排除项与原行为→§1、§2.3、§8；清理处置→§6、§7。独立审计报告应逐项给出结论和修复复审记录。

没有待用户决定的实施开关：本轮选北京、固定快照、USB输入及明确状态规则。用户以后要求其他地区、手机输入密钥或无卡恢复时，先单独评估，不能自动扩展当前范围。本步骤审计通过并提交后停止；下一入口是主计划步骤二，须由用户后续指令启动。
