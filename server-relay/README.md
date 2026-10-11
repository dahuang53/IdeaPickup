# 拾想 · IdeaPickup 转写中继

当前为0.2.0rc1自管后端候选，支持千问转写、思源云端收集箱、WAV／Opus、管理配置和设备独立凭据。已验证范围只见[支持状态](../docs/SUPPORT.md)；首次使用面向新安装，不导入旧项目数据。

## 本地开发与测试

从项目根目录执行：

```sh
./scripts/setup_relay.sh
./scripts/test_relay.sh
node tests/test_admin_probe.cjs
```

后端依赖与 Python 由 `pyproject.toml`、`uv.lock`、`requirements.lock.txt` 和 `.python-version` 锁定；工具及虚拟环境均在工程内。主机测试使用合成音频、测试凭据和提供商替身，原始输出留在 `private/`。可选的一小时负载不会默认执行；实际供应商调用另行明确执行。

管理页浏览器回归使用现有 Chrome、Playwright **1.62.1** 和工程内 Python；不会安装全局依赖。源码保留 `docs/design/admin-approved/index.html` 作为只读视觉对照基准，它是已确认的演示原型，不能作为业务管理页或协议实现；正式品牌文字与原型旧名称的差异允许，视觉参数仍保持。先从已安装的运行时定位 Playwright 模块目录和 Chrome 可执行文件，再在两个终端中分别运行（每次使用新的 `private/` 子目录）：

```sh
# 终端 1：只绑定本机，生成测试证书/初始化码，供应商全部替身
.venv-relay/bin/python tests/relay_admin_fixture.py --output private/admin-browser-run
```

```sh
# 终端 2：替换为本机已安装路径；脚本校验 Playwright 版本
PLAYWRIGHT_MODULE=/absolute/path/to/node_modules/playwright \
CHROME_EXECUTABLE=/absolute/path/to/chrome \
node tests/test_admin_browser.cjs private/admin-browser-run
```

脚本从该目录读取本地 HTTPS 入口，不能指定生产地址；自签名证书例外只在隔离浏览器内。实际 HTTP、合成静音、测试凭据、请求收据、结果和截图均留在同一私有目录，测试后用 Ctrl-C 停止终端 1。覆盖真实初始化/登录、配置错误与丢响应恢复、配对/撤销、任务重试/单条对账、分页和桌面/手机对照。结果分开记录，已验证范围见[支持状态](../docs/SUPPORT.md)。这是静态页面集成回归，不代替容器、真机或真实供应商测试。

## 运行入口

正式业务为非 root Linux 单 worker，使用 `python -m relay serve`。配置 broker 是无网络的 root-only 本地 helper，按其 RPC 契约限定 Unix peer 和秘密访问；不在生产提供测试 ownership 或 TLS 绕过开关。Docker / 自动卷与broker入口、HTTPS代理和普通版本更新见[部署说明](../docs/DEPLOYMENT.md)。空卷先初始化管理页，配置后readiness才表示可接收录音；已验证容器/TLS/供应商范围见支持表。

服务地址来自部署的 `VOICE_MEMO_PUBLIC_URL`，管理页的地址仅供查看。管理员在 HTTPS 管理页填写千问 API Key 和思源 Token，一次“保存并生效”；字段留空保留已有值，清除使用明确选项，已保存秘密不回填。当前千问接口 / 模型和思源云端收集箱协议固定，页面以只读字段展示。

初始化码由部署主机的 `python -m relay bootstrap` 生成；应将输出重定向到本工程 `private/` 的仅本人可读文件，再由所有者在管理页设置口令。`reset` 撤销管理会话并签发新初始化码，不清录音任务或供应商配置。没有 API Key 时服务仍能进入初始化 / 配置页面；业务 readiness 会明确显示未配置。

## 协议与数据行为

HTTP 字段、认证、配对重试、ACK 和配置 / 重试语义统一见 [HTTP 契约](../docs/contracts/device-api-v2.json)。内部秘密存储与 RPC 边界见 [broker 契约](../docs/contracts/config-broker-v1.json)，字段和允许方法路由到相应源定义。

设备在首次配对请求前持久保存随机 enrollment ID 和候选凭据，同一请求丢响应可自动重试；管理员仍只生成普通一次性配对码。设备不填写千问 Key 或管理员口令，接口不限制硬件型号。

正在接收 / 处理的任务保持原配置。失败任务停止处理后释放旧秘密快照，修改 Key 不因历史失败而永久锁住；显式重试是使用当前配置的新一次尝试，页面会说明。真实投递结果不确定时只处理该条结果，不自动重投或要求全局历史证明。

不提供云端音频下载 / 回放。成功或拒绝后清理音频；失败 / 中断的闲置暂存最长 24 小时；查询不续期，重启先处理到期数据。投递完成后清除暂存正文。磁盘、提交或清理失败不能报成功；真机、容器权限和实际供应商结果与主机测试分开记录。
