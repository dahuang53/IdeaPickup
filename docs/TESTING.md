# 当前测试入口

从工程根运行。先安装工程锁定的 uv 0.12.5；主机需要 C/C++ 编译器（USB SDK 检查还使用 clang）、Node、OpenSSL 和可加载的系统 libopus。主机 Python / SDK / 组件由 [工具锁](../tools.lock.json)、Python hash 锁与 `dependencies.lock` 决定；容器 libopus 与基础镜像使用 [容器锁](../server-relay/container-base.lock.json)。不安装全局 Python 包、不连接串口或供应商。

```sh
./scripts/bootstrap.sh
./scripts/setup_relay.sh
./scripts/test_all.sh
```

`bootstrap --host-only` 只适用于无需 SDK 的基础测试；整体入口使用锁定 SDK 的 cJSON / NVS / VFS 方法，因此须运行完整 bootstrap。所有合成音频、秘密、数据库、临时目录及原始日志留在 `private/`。缺失工具、失败用例会退出非零，不用跳过必要用例来获得通过。

| 入口 | 实际范围与边界 |
|---|---|
| [test_all.sh](../scripts/test_all.sh) | 汇总以下现有套件与维护发布拒绝、管理页报告渲染回归；只用合成输入 |
| [run_host_tests.sh](../tests/run_host_tests.sh) | 真实存储 / WAV 收尾、录放状态、语音门、供电、USB PCM 等 portable C 与 FakeSerial 测试，ASan / UBSan；无旧分区迁移测试 |
| [test_relay.sh](../scripts/test_relay.sh) | 实际后端 / HTTP / 账本 / broker / 清理 / 权限 / 供应商本地 TLS 替身；保留原有可选一小时测试边界 |
| [test_service_config.sh](../scripts/test_service_config.sh) | 当前配置 / 配对及 AP handler，实际生产 C/C++ 与 ASGI，ESP / RF / NVS 为显式平台替身 |
| [test_sync_pipeline.sh](../scripts/test_sync_pipeline.sh) | 实际编号 / SD / 无卡方法与 HTTP；配置→配对→WAV / Opus→查询 / 显式重试→两端重启，多条离线／换卡／身份／持久化故障场景；后端、broker 重新打开当前持久数据，设备侧重建配置 / sidecar / NVS 替身，不伪装真实拔电 |
| [test_firmware_install.sh](../scripts/test_firmware_install.sh) | 打包 / 安装边界、SDK / 生产 stdio 与 SD DMA 无内存回归；不使用设备 |

两板普通构建、打包和恢复命令只在[设备安装说明](DEVICE_INSTALL.md)定义。干净源码验证从无 build、sdkconfig、managed_components、venv 的检出开始；依赖缓存可重用，但必须注明 SDK / 工具 / 下载缓存来源及实际下载，不称作全量冷下载。`test_ui.sh` 使用构建得到的锁定 LVGL；设 `MEMO_UI_OUTPUT=private/ci-ui`，避免覆盖历史证据。USB 描述符对象检查命令由 [CI 固件 job](../.github/workflows/ci.yml) 定义，检查编译对象不能证明 CDC / UAC 已枚举。

管理页及设备配网页的真实隔离浏览器入口仍由 [README](../README.md) 和[后端说明](../server-relay/README.md#本地开发与测试)路由；本次 CI 不增加浏览器依赖。桌面/手机 viewport 不是实际手机/AP 或触摸/实听。

## CI 与源码候选

[ci.yml](../.github/workflows/ci.yml) 是命令事实源：适用主机回归、`vocat16` / `spotpear` 普通构建及 UI / 描述符 / 打包、两架构原生 Linux runner 的容器子集。不构建 32 MB 或只读资格固件，不读取生产 Secret，不部署、访问串口或上传产物。Action 按 DONOR 的已固定完整 commit 取用，uv 版本仍锁定；工程依赖由既有入口安装。没有公开仓库时执行等价本地命令，如实区分 Mac、Desktop Linux VM 原生 arm64、模拟 amd64和真实托管 CI。

```sh
./scripts/test_container_ci.sh --platform linux/arm64
./scripts/test_container_ci.sh --platform linux/amd64
```

该子集构建非 root 运行镜像、实际加载 libopus、渲染基础/代理 Compose，并在隔离无网络验证镜像跑 broker 套件。测试秘密权限使用已有显式 ownership fixture；它不替代 实际 Unix peer、生产权限、HTTPS、真实供应商和完整部署生命周期验收。验证镜像只额外复制其测试支持模块；默认运行镜像不包含测试代码。

[release-candidate.yml](../.github/workflows/release-candidate.yml) 仅手动调用同一 CI 检查。无需旧 `PUBLIC_RELEASE_QUALIFICATION`、旧 OSS 全表或全部 A/C PASS；它不导出历史仓库、不创建 Release/tag、不推送镜像、不宣称固件稳定。干净公开导出和具体发布授权仍属 公开候选准备／首次发布，设备与供应商尚未执行的项目仍在支持表记录。

## 隔离容器协议验收

在工程根用已经构建的固定镜像身份执行，不连接设备或真实供应商：

```sh
.venv/bin/python scripts/test_container_acceptance.py --platform linux/arm64 --image 'sha256:<实际当前镜像ID>' --previous-image 'sha256:<真实兼容旧代码镜像ID>'
.venv/bin/python scripts/test_container_acceptance.py --platform linux/amd64 --image 'sha256:<实际当前镜像ID>'
```

[驱动](../scripts/test_container_acceptance.py)使用当前生产 Compose、管理 CLI、HTTP、codec、SQLite、broker 和更新操作。只有隔离命名空间的 relay 挂载只读[合成夹具](../tests/container_fixture.py)：供应商回调、暂停/丢响应、明确的偏移时钟及删除故障可替换；默认业务镜像不含这些入口。管理员、配置和设备从空卷通过当前公开 HTTP 建立，没有自动旧身份或配置导入。密码/Key都是固定合成值，原始请求和结果在 `private/container-acceptance/`。

驱动生成仅回环可访问的 HTTPS 和观测端口，测试 CA 只由客户端显式指定；Mac 验收将合成 TLS 文件安装为 Linux 命名卷内 root 0600，替代生产 TLS 的宿主专用目录挂载。这不代表已在 Mac 验证 Linux 宿主的 root 文件安装。普通部署和 TLS 输入仍以[部署说明](DEPLOYMENT.md)为准。空间故障仅在有界 tmpfs 工作目录分配 40 MiB，不占满宿主；到期测试使用持久偏移时钟，不冒称等待了 24 小时。两个架构顺序运行，测试专用网段需空闲；仅删除本次生成的资源，没有全局 prune 或个人服务迁移。

`--previous-image`必须来自真正不同的兼容后端源码；改标签或同版本重启不能替代升级证据。真实供应商测试另需当前明确目标、用户配置及调用/写入授权，此驱动永不使用真实凭据。实际已验证范围及未执行项见[支持状态](SUPPORT.md)。

## 源码与镜像材料

同步状态回归还覆盖：详情只读取已保存状态、不发 HTTP 或改写任务意图；同编号同长度音频变化拒绝复用结果；无卡 1024 条索引与 4 个详情槽反复淘汰、活动记录保留、孤立任务恢复，以及未结束任务后台查询和终态停止查询。`test_sync_metadata_lock.py` 用真实元数据编解码与 FatFs 共享冲突替身验证并发读写；`test_service_runtime.py` 验证串口维护模式沿用保存事务、保留配对和 Wi-Fi、忙态拒绝与重复保存不写 Flash。主机替身不能代替设备内存、SD 读取耗时、触摸或真实传输测试。现有 HTTP SDK 操作超时也不等于请求全程的硬总期限。

`test_network_worker.py` 执行生产网络调度循环，核对无卡时仍推进网络任务、活动流期间排除 SD 同步，以及活动／空闲／配网的等待间隔。`test_cardless_runtime.py` 另行执行原传输状态机的真实协议与故障回归；共享执行者不改变录音编号、确认顺序或不确定结果的恢复规则。

`tests/test_public_export.py` 验证源归档隐私、链接／变更拒绝及无Git构建身份，随整体主机入口执行。构建收据的source_commit可能是本仓库Git提交或source-sha256；定义在产物契约／source_identity，不读取外层Git。

镜像检查用 `scripts/audit_container_image.py`，库存用 `scripts/container_sbom.py`；必须指定当前私有归档、实际平台／manifest／config及校验值。Syft固定锁只支持Darwin arm64维护环境，不是运行依赖。系统版权文字、公开测试向量和二进制可能触发启发式命中，必须按具体来源解释，不整体删除结果或据此声称没有风险。材料检查不调用真实供应商或公开推送。

R12 手动配网页回归：`scripts/test_service_config.sh` 执行既有配置／配对／SSID持久化和请求防护，以及生产 AP 生命周期、异步连接、保存失败、保持AP＋STA、退出取消并等待连接结束、重新进入及旧自动弹页代码清理检查。SDK模拟不代替真实DHCP、手机切换应用、普通更新、USB切换和退出后不可达验证。DNS通配解析器已移除，不再以自动弹页作为成功标准。

`scripts/test_ui.sh` 验证360、240和vocat16三种生产布局；包括真实网络错误进入录音控制页时的缺字、宽度和状态区域边界，不能只检查配网页的短提示。配网页保留完整手动URL，热点和地址同色、相邻且行距一致，状态与操作分组。主机渲染不代替真实屏幕／触摸。

`tests/test_service_portal_browser.cjs` 配合原生处理器fixture验证手机／桌面页面：提交与连接／保存／配对状态区分，保持热点时进度更新，页面刷新和临时失联恢复，失败重试、重复提交防护和明确退出。fixture的SDK／NVS替身及模拟配对收据不证明真实手机或服务器配对成功；真机须使用精确候选再验。

`node tests/test_portal_async.cjs` 直接执行生产配网页脚本，使用可控时钟、请求和DOM替身验证保存期间的新编辑、重复提交、旧读取跨保存返回、Wi-Fi目标及持久化终态、扫描互斥、断连恢复和关闭后的迟到响应。此脚本没有浏览器或硬件依赖；由浏览器回归入口先行调用，不替代原生处理器浏览器和真机验收。

当前配网页采用本人选定的 Gemini A 布局与 B 配色（保留选中内描边）。浏览器回归还检查草稿与生效模式分离、抽屉状态回读、真实 SSID 与手动选项隔离，以及轮询/中文组合输入期间的控件身份与光标保持。设计原型仿真器不编入固件；无外部脚本、字体或浏览器持久化凭据。

网络选择控件使用真实扫描结果的信号／锁图标；回归覆盖开放／加密／未知信息、长SSID、手动输入、键盘方向键／Escape、选择不隐式提交及扫描忙碌互斥。320px与390px检查两行右侧按钮边界一致、模式短文案不换行；手机实际样式另外验收。

R12 HTTP容量回归使用锁定SDK的实际accept／LRU函数体，描述符分配和会话队列为明确主机替身。它复现默认7客户端＋3内部socket在并发网络工作下的ENFILE，验证4客户端限制可先触发LRU并留下3个网络socket；不冒称真实ESP并发手机压力通过。源码导出检查仅审过具体示例／无网络测试网址，同一文件的新地址或凭据仍拒绝。

R12 SD探卡回归由`tests/run_host_tests.sh`调用`test_sd_probe.py`，五组直接提取生产函数，并明确使用硬件、RTOS与散列引擎替身：检查无需求无限等待、可见页5秒／真实待办15秒、睡眠与活动互斥、录音入口重新分类、pending恢复标记防覆盖；检查空队列停止扫描、新录音事件和扫描中事件保留、单项缓存／WAV读失败不误当空；检查SD播放暂停继续时同编号同长度异音频拒绝；检查UI退出配置／USB至猫脸不会发布中间存储需求。流式文件遍历及AUDIO所有权执行生产路径，替身散列不验证加密算法质量。

`test_service_runtime.py`执行真实MEDIA/AUDIO/COMMIT互斥与SD读入口，覆盖拒绝、释放及睡眠边界；`test_sync_pipeline.py`另核目录缺失与成功空扫描差异。真机使用`status`中的`SD_PROBE`数值（checks/wakes/view/work/scans）区分实际SD查询、任务通知唤醒和目录扫描；SD启动初始化与读取音频的业务I/O不计入后台探查次数。纯猫脸、USB及配置无需求窗口观察计数，物理插卡、页面可见刷新、录音中模式固定和有卡恢复按原介质批次验证。不把主机替身、串口计数或USB状态切换当作物理拔卡、电流／续航或PCM实听通过。

按需探卡的页面回归由 `scripts/test_ui.sh` 在三种现有布局上执行生产 `ui.c` 和锁定LVGL：首次tap打开操作页后异步有卡→无卡、readiness变化及插回有卡均保持操作页与存储需求；真实录音状态变化仍回猫脸，60秒超时结束需求。原实现会在第一项复现关闭页面，修复不改变探卡周期。主机模拟快照不能替代真实插拔和首次触摸复验。

操作页计时回归另核15秒及55秒仍保持、一次实际LVGL翻页触摸后重新计时、再次55秒仍保持与满60秒关闭并取消存储需求；原两分钟表情待机回归仍执行。60秒真机时长已由本人在合并集中组确认约30秒保持、约60秒返回，并经限定独立结果审查；不称独立秒表测量。主机计时与本人实际观察分别记录。

自动同步状态回归还覆盖固定无卡索引／详情淘汰、本地详情不发 HTTP、同一任务恢复、挂载内音频身份缓存失效、SD 会话状态，以及请求期限。`test_http_deadline.py` 用 socket pair 验证生产中断任务打断嵌套等待及暂时中断失败后的重试，TCP/TLS 与 RTOS 通知／时间调度为明确替身；不将它称为真机 TLS 故障测试。物理换卡、触摸、真实转写与投递证据须分别核验。
