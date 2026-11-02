# 设备直连第二步：协议基础与验证

本步以 `2f25b49260d26598886944493e827152f840abc7` 为基线，遵循[第一步决定](DEVICE_DIRECT_CLOUD_STEP1.md)，对应[主计划第二步](DEVICE_DIRECT_CLOUD_PLAN.md#步骤二-实现设备直连基础能力)。本步提供可调用、可验证的协议模块；有卡／无卡录音调度与配置分别在第三至五步接入。

## 实现入口与所有权

| 入口 | 本步行为 |
|---|---|
| [memo_cloud.h](../main/memo_cloud.h)／[memo_cloud.cpp](../main/memo_cloud.cpp) | `file_asr`、`deliver`、`Realtime`，每次只执行一个供应商请求／会话，无内部自动重试 |
| [memo_cloud_json.c](../main/memo_cloud_json.c)／[符号别名](../main/memo_cloud_json_names.h) | 直接编译固定SDK的原cJSON源码，使用私有符号空间；解析仅从单个192 KiB PSRAM arena分配，不影响共享SDK解析器 |
| [memo_cloud_esp.h](../main/memo_cloud_esp.h)／[memo_cloud_esp.cpp](../main/memo_cloud_esp.cpp) | 真实ESP HTTP／WebSocket适配，固定CA bundle／域名验证，关闭HTTP重定向及WS自动重连；无测试域名、私有CA或凭据默认值 |
| [memo_http_transport.cpp](../main/memo_http_transport.cpp) | 复用已有socket deadline所有者；直连增加请求全程期限、单次I/O窗口、TLS校验原因及“可能写出HTTP字节”标记；原调用默认窗口不变 |
| [network_sync.cpp](../main/network_sync.cpp) | 新适配复用现有递归HTTP门闩和对时入口，额外一个原子占用标记防止两个直连会话重入；没有新录音调度线程 |
| [main/idf_component.yml](../main/idf_component.yml)／[dependencies.lock](../dependencies.lock) | 唯一新增组件 `esp_websocket_client==1.5.0`；SDK仍5.5.1，已有依赖版本保持，包括dl_fft 0.7.0 |

调用者持有输入来源、输出容器及取消标志，生命周期须覆盖调用／会话，并在请求期间保持来源不变。API只返回结构化结果，不在日志输出正文／音频／密钥；临时鉴权缓冲退出时清零，支持完整256字节凭据。错误结果仅保留通过长度及字符检查的供应商短码，不保留错误正文或回显的完整凭据。

固定SDK的`transport_ws`在握手写失败时会输出含鉴权的请求，`HTTP_CLIENT`的调试日志也包含请求头。适配器在持有网络执行权期间，仅将这两个SDK日志tag设为NONE，结束后恢复各自原级别；不关闭项目自身错误反馈，不修改SDK或全局机器配置。构建必须支持动态日志级别。

协议调用前获得执行权，结束时由调用所有者关闭句柄并归还执行权。SDK事件回调只复制有界片段或设置错误，不执行JSON解析、文件I/O、关闭句柄或等待队列。WS回调合并SDK分片及续帧到64 KiB事件缓冲，再以零等待写入有界消息缓冲；满即明确失败。消息缓冲与片段存储分配在PSRAM，控制结构固定。

三份构建defaults启用官方WS独立TX锁，使PONG／心跳与应用音频共用SDK互斥，不另造帧锁。固定1.5.0把名为`TX_LOCK_TIMEOUT_MS`的控制帧锁等待直接按ticks使用，故设为200；两板100 Hz下实际为2秒，ESP编译断言防止配置／时钟变化后静默扩大等待。SDK会对锁及各分片重复使用timeout，因此适配器还用既有socket deadline所有者的独立SEND期限覆盖整次SDK调用；音频2秒、配置／结束使用其剩余期限，返回后先判取消／超时再判发送结果。

两板构建编译新组件，但录音主路径尚未调用这些入口，未引用代码可能被链接器裁剪。本步不声称设备已直连；第三、四步接入时才改变录音处理流程。

## 协议、边界及结果

- 文件请求：固定北京地域快照、180秒段最大5,760,044 B；3072 B读取／4096 B Base64编码，正确处理短读取、末尾填充、部分写入和Content-Length。不会分配整份音频或7.68 MB请求。
- 文件响应：最多64 KiB，跨读取块重组后检查UTF-8；先做严格数字／字符串控制字符、深度及分配预算预检，再用固定SDK的cJSON解析。私有parser hooks固定指向有界arena分配函数，用线程局部指针选择当前arena，不切换共享SDK hooks；单个192 KiB PSRAM块耗尽即失败，不能回退内部堆。树节点和字符串均在此块内，生命周期结束整块释放。拒绝尾随垃圾、相关字段重复、内嵌NUL、缺失／错类型、非stop、超限及截断。合法空白转写文本返回Empty，不冒充有正文成功。
- 思源请求：正文最多256 KiB，标题最多128 B；固定目标，按块转义引号、反斜线及控制字符。先用有界读取计算长度及校验UTF-8，再从同一稳定来源流式发送，不复制整个转义正文。
- 思源结果：HTTP 2xx且数值code=0才成功。明确401／403、429及业务拒绝分别报告；请求可能写出后超时、断线、坏响应或5xx均为Uncertain，无自动重发。已收完并交给解析器的合法成功响应优先于同时发生的取消，不额外等待网络。连接前确定未写HTTP字节的失败保留传输／TLS分类。
- Retry-After：支持秒数和UTC HTTP日期；保留大于300的值，由后续任务按第一步规则停止自动重试；协议层不自行sleep或另建额度。
- 实时识别：created→update→updated后才允许PCM；每块最多3200 B且为偶数字节，允许短尾块。只收最终条目，临时草稿不会追加；按条目链顺序释放结果，最多16个未结条目，待排序文本合计最多16 KiB，整次输出最多256 KiB。最近16个已释放ID作固定重复过滤；更旧未知重复会失败，不会追加第二遍。
- 实时结束：调用者先停止采集、排空PCM队列并发送尾块，再调用finish；协议层发送session.finish并有界等待最终条目与finished。缺失条目、未知／异常链、输入／输出超限、坏证书、断线、取消和超时均退出。Sink在finish返回Ok之前是临时结果，调用者收到失败必须丢弃，不得投递部分正文。
- 实时资源：通过官方ext_transport接入已有受保护socket；读、写、SEND调用和强制中断分别保留期限，既有deadline worker取最早有效期限，普通I/O完成只解除本方向期限。取消／失败／正常结束先由该所有者执行shutdown以中断慢滴握手／帧读取，再走官方stop/destroy流程释放会话、事件与排序缓冲，不跨任务关闭或释放TLS对象。SDK错误与断开事件仅在TCP_TRANSPORT类型下读取SDK已初始化的TLS字段，同时保留inner transport真实校验原因；后续通用断开不覆盖具体分类。输出Sink归调用者管理，后续录音步骤负责最终文本释放与持久状态；主机检查不证明真实RF／RTOS调度时延。

后续任务在调用前持久消耗尝试额度、处理10秒投递间隔和断电／取消状态。本步没有设备持久化或自动重试，没有隐含付费调用入口。

## 清理与暂留对象

对应第一步C01—C25，当前完成及暂留边界如下。暂留的对象仍服务于尚未迁移的现有录音路径，最终直连交付前必须按归属步骤替换清理。

| 范围 | 本步处置 | 后续归属／验证 |
|---|---|---|
| C22：`memo_sync_remote_action`、`memo_sync_upload_response_pending`、`memo_sync_local_state` | 删除当前仅有测试消费者的旧辅助函数、声明和专属断言；队列与仍被生产调用的判断保留 | 新协议HTTP结果由真实协议测试覆盖；原队列回归继续执行 |
| C13、C16 | 共用期限能力扩展、新WS依赖精确锁定，新模块加入两板构建 | 两板编译及socket deadline回归；最终链接清理在六步再核 |
| C01、C10、C11 | 中继配对及配置仍被现有网页／录音使用，尚未切换 | 五步移除完整配置调用链；二步不留第二套备用开关 |
| C02—C05、C17、C23 | 旧有卡上传、查询／收据、状态格式和WAITING完成判断仍是现有任务的生产路径 | 三步替换调度、直连检查点及本地详情；旧格式只留必要读取 |
| C06—C09、C25 | 旧无卡ACK、Opus环、恢复／轮询、stream_request包装仍被现有无卡流程调用 | 四步切PCM并删除旧协议运行资源；不得漏掉stream包装及共享模块内部旧逻辑 |
| C12、C18—C20 | 旧诊断／测试／用户指引随消费者暂留，新增测试已进入标准入口 | 三至五步改相应接线，七步用户说明和交付收口 |
| C14、C15、C24 | 编号、录放／USB／探卡、活动门闩复用 | 原回归继续；不随旧配置删除共享互斥 |
| C21 | 后端、现有部署、录音及恢复材料保留 | 本步不操作这些对象 |

## 可重复验证

使用现有项目环境，在根目录运行：

```sh
./scripts/test_cloud.sh
./scripts/test_sync_pipeline.sh
./scripts/test_all.sh
./scripts/build_vocat16.sh
./scripts/build_spotpear.sh
```

标准test_all已纳入新cloud入口，正常全套执行时无需再重复单独跑。

干净host环境须先按README运行完整`bootstrap.sh`，无需预先编译固件。SDK测试优先读取已有WS组件源码；尚未解析组件时仅下载官方固定1.5.0标签的单份公开源码并校验SHA-256，缓存于项目`.tools`。缺失、超限或摘要不符均失败，不跳过测试。

| 验证 | 实际使用的代码／边界 | 成功依据 |
|---|---|---|
| [test_cloud_protocol.py](../tests/test_cloud_protocol.py) | 编译生产协议C++及锁定cJSON，经显式IO替身连接本地真实HTTPS／WebSocket服务器；本地地址映射和测试CA只在测试文件 | 三类真实网络往返、最大段、部分读写、UTF-8／JSON故障、证书拒绝、思源不确定、WS分片／排序／重复／尾音、20轮成功／失败／取消，资源归零 |
| [cloud_native.cpp](../tests/cloud_native.cpp) | 同一协议函数的ASan／UBSan可执行程序和FFI薄封装；测试钩子计数协议块及arena分配，不进入固件 | URI／Retry-After边界、成功／失败／取消循环，arena耗尽和分配失败后归零；共享SDK树跨私有解析仍有效。峰值计数包含响应／事件／排序块，不能再当成仅cJSON节点数；不是ESP堆测量 |
| [test_cloud_esp.py](../tests/test_cloud_esp.py) | 编译实际ESP适配源码和协议核心的ESP分支，SDK／RTOS为明确替身，ASan／UBSan开启 | CA／无重定向／无自动重连／8 KiB栈配置、SDK片段及续帧、队列满、真实错误事件顺序与未定义字段填充、对端关闭前最后事件、完整响应计数、精确PSRAM能力分配及192 KiB分配失败不回退、日志级别恢复、单所有者和20轮释放 |
| [test_cloud_ws_sdk.py](../tests/test_cloud_ws_sdk.py) | 提取固定IDF握手及WS1.5.0接收／发送函数体，配生产transport和socket pair；TLS／时钟／RTOS及发送帧I/O为明确替身 | 慢滴握手／大帧在期限或取消后被shutdown中断；实际SDK握手失败日志正负对照；真实PONG分支取得／归还共用TX锁、占锁2秒失败；真实SDK send锁等待＋多分片累计仍受一个SEND期限约束，取消／超时后资源归零；ASan／UBSan通过 |
| [test_http_deadline.py](../tests/test_http_deadline.py) | 生产transport装饰器＋socket pair，底层TLS／时钟／RTOS为替身 | 原窗口与新全程期限、期限不能被新写窗口刷新、读写两个方向交错及完成／强制中断竞争仍由实际socket shutdown退出；没有测试RF |
| 原同步／全套回归 | 现有公开测试入口 | 现有录音／配置／同步／后端等行为不因第二步提前删除消费者而损坏 |
| 两板标准构建 | 锁定ESP-IDF 5.5.1、WS1.5.0、原有其他依赖 | 两板模块编译和应用分区检查通过；编译不是刷机／实听／真实供应商验证 |

本步故障测试分别记录请求前、发送中及响应后；SDK实机握手、内存连续块、真实任务持久化和音频质量留在对应后续步骤，不用主机替身冒充通过。完整运行记录在本机私有证据中保留，独立审计的结论和候选绑定另列报告。
