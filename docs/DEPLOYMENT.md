# 部署中继与配置后台

[返回首页](../README.md) · [下载版本](PUBLIC_RELEASE.md) · [设备配对](DEVICE_INSTALL.md#pairing) · [排查问题](SUPPORT.md#troubleshooting)

只需要离线有卡录放，可以跳过本页。联网转写需要一套自己的中继、千问 API Key 和思源云端收集箱 Token。本页先完成部署，再配置后台，最后生成设备配对码。

## 1. 准备环境与项目文件

- Linux Docker Engine（rootful）或运行 Linux 容器的 macOS Docker Desktop，支持 `linux/amd64`、`linux/arm64`；安装入口见 [Docker 官方说明](https://docs.docker.com/get-started/get-docker/)。需要 Compose V2 和 Buildx。
- 宿主终端需要 Bash、Python 3.10 或更高版本；下列 `python3` 必须指向这个版本。不需要固件 SDK 或 Python 后端虚拟环境来运行现成镜像。
- 从[下载说明](PUBLIC_RELEASE.md#校验并取得项目目录)取得当前版本项目目录，并在该目录执行后续命令。

```sh
python3 --version
docker version
docker compose version
docker buildx version
```

`docker version` 应同时显示客户端和服务端。macOS 先启动 Docker Desktop。当前脚本依赖 POSIX 环境；原生 Windows、WSL2 的 USB／网络路径和 rootless Docker 不在已验证部署范围内，不能按本页直接保证可用。实际平台证据见[支持状态](SUPPORT.md#verification-record)。

<a id="addresses"></a>
## 2. 先确定地址，不需要先进入后台

**服务地址由你部署的位置决定，后台地址就是服务地址后面加 `/admin/`。** 设备配网页、管理后台、转写服务是三个不同入口：

| 入口 | 本地 Docker 示例 | 服务器示例 | 谁使用 |
|---|---|---|---|
| 服务地址 | `http://192.168.1.20:8765` | `https://relay.example.org/memo` | 填入部署配置和设备“服务器地址” |
| 管理后台 | `http://192.168.1.20:8765/admin/` | `https://relay.example.org/memo/admin/` | 管理员初始化、配置密钥、生成配对码、查看任务 |
| 设备配网页 | `http://192.168.4.1` | 同左 | 手机连接设备热点后，填写 Wi-Fi 和服务地址 |

表中 `192.168.1.20` 是示例电脑地址，`relay.example.org` 是示例域名，必须换成自己的值。设备服务地址**不要带 `/admin/`**，也不要填设备热点地址。

本地 IP 的查找方法：在电脑当前 Wi-Fi／以太网连接的详情中查看 IPv4 地址，或在路由器“已连接设备”里找到这台电脑。Linux 也可用 `ip -4 addr show`；macOS 可用 `networksetup -listallhardwareports` 找到 Wi-Fi 对应的设备名，再运行 `ipconfig getifaddr 设备名`。选择与设备连到同一路由器的接口，不能使用回环、Docker 网桥或 VPN 地址。

记下这张地址表中的两个自有地址。后端不会自动替你注册域名、发现电脑地址或生成初始访问链接；后台“配置”页显示的服务器地址来自你填写的 `VOICE_MEMO_PUBLIC_URL`。

<a id="local-docker"></a>
## 3A. 本地电脑 Docker：内网 IP 即可

适合设备与电脑处于同一家庭／办公网络。不需要域名、证书或路由器公网端口转发；电脑必须保持开机、Docker 运行且设备可访问。

### 拉取镜像并填写配置

先拉取[本次发布](PUBLIC_RELEASE.md#配套-docker-镜像)的固定镜像：

```sh
docker pull ghcr.io/dahuang53/ideapickup-relay:2026.10.27-rc1@sha256:73069bf8d986ab6e3d150e449337aa130aad172bf1f6361b35094e43df94ae31
mkdir -p private/deploy
chmod 700 private/deploy
cp deploy/.env.example private/deploy/relay.env
chmod 600 private/deploy/relay.env
```

`cp` 只用于首次建立配置；已有部署不要覆盖原文件。用文本编辑器修改 `private/deploy/relay.env` 的两行，其余保持默认：

```dotenv
VOICE_MEMO_IMAGE=ghcr.io/dahuang53/ideapickup-relay:2026.10.27-rc1@sha256:73069bf8d986ab6e3d150e449337aa130aad172bf1f6361b35094e43df94ae31
VOICE_MEMO_PUBLIC_URL=192.168.1.20
```

替换第二行示例 IP。裸内网 IP 自动采用 HTTP、端口 8765；自定义端口可写 `192.168.1.20:9876/memo`。若填写完整 HTTP URL，省略端口表示 **80**，并不是 8765。只接受 RFC1918 IPv4：`10.*`、`172.16.*`—`172.31.*`、`192.168.*`，不接受公网 HTTP 域名或公网 IP。

env 只写 `KEY=value`，不加引号、不使用 `$变量`，不要 `source`。千问 Key、思源 Token 和管理员密码不写进这个文件。`VOICE_MEMO_PROJECT` 保持稳定，它决定持久卷身份。

### 启动并打开后台

```sh
./scripts/relay_deploy.sh validate
./scripts/relay_deploy.sh start
./scripts/relay_deploy.sh status
```

工具只检查本地已有镜像，所以不能省略前面的 `docker pull`。启动后用电脑浏览器打开地址表中的管理后台；同一路由器上的手机也应能访问。随后继续[管理员初始化](#admin-setup)。

首次启动显示 `not_configured` 是尚未填写供应商凭据，不等于容器启动失败。若后台打不开，先按[后台无法访问](SUPPORT.md#admin-unreachable)检查地址、端口和网络隔离。

局域网 HTTP 不加密链路，只用于自己信任的网络；登录和配对仍需认证。同网监听者可能读取传输内容，需要加密时使用下方 HTTPS 方案。电脑休眠、IP 改变或 Docker 停止会让设备暂时无法同步，可在路由器为电脑固定地址。千问和思源仍通过各自公网 HTTPS 接口访问。

<a id="server-https"></a>
## 3B. 服务器 Docker：可信 HTTPS 域名

部署者需要服务器管理权限、已指向该服务器的域名，以及覆盖该域名的可信证书。若服务器已有 HTTPS 网站，新增一个独立路径即可；不要覆盖原站点配置。

1. 在域名 DNS 管理处将自有域名解析到服务器。开放实际使用的 HTTPS 端口（通常 443）；后端 8765 无需直接暴露公网。
2. 按上节拉取镜像、创建 env，将 `VOICE_MEMO_PUBLIC_URL` 改为完整地址，例如 `https://relay.example.org/memo`，执行 `validate`、`start`。域名模式下 relay 只监听宿主 `127.0.0.1:8765`。
3. 在已有可信 HTTPS 的 Nginx `server` 块里加入 [nginx-existing.conf.example](../deploy/nginx-existing.conf.example) 的完整 `location /memo/` 配置。根地址部署则将 location 改为 `/`。证书必须覆盖你实际使用的域名并提供完整证书链；配置与检查方法见 [Nginx HTTPS 官方文档](https://nginx.org/en/docs/http/configuring_https_servers.html)。
4. 在服务器检查并重载 Nginx（下面命令适用于已安装 Nginx 的宿主；其他代理按其自身工具检查）。用浏览器打开 `https://relay.example.org/memo/admin/`，确认没有证书告警，再进行后台初始化。

```sh
sudo nginx -t
# 仅在检查成功后执行
sudo nginx -s reload
```

代理模板的 `proxy_pass http://127.0.0.1:8765;` **不能追加斜杠或去掉 `/memo` 前缀**。它保留 Host，设置 HTTPS 转发头，关闭缓存、音频缓冲及上游自动重试；请使用完整模板，不只复制 proxy_pass 一行。部署 URL、location 和设备填写地址的前缀必须相同。域名填写默认 HTTPS；非 443 端口要显式写入服务地址。

没有既有 HTTPS 入口时，先按服务器平台的域名／证书指引建立入口。本项目不代办 DNS、证书签发或自动续期，也不允许用关闭证书校验代替配置；本地电脑方案无需这些步骤。

<details>
<summary>可选：项目自带 Nginx 代理与高级网络配置</summary>

在 env 启用 `VOICE_MEMO_PROXY=1`，设置 `VOICE_MEMO_TLS_DIR` 为本项目 private 内专用目录，只放 `certificate.pem` 与 `private-key.pem`，文件须实际 root 所有、0600。证书应由可信 CA 签发，SAN 覆盖访问域名；密钥只读挂载，不进入镜像。

默认 HTTPS 绑定 `127.0.0.1:8443`；需要对外服务时显式设置 `VOICE_MEMO_HTTPS_BIND` 和 `VOICE_MEMO_HTTPS_PORT`，并使 public URL 与真实端口一致。Linux 宿主需由管理员准备上述权限；不要以放宽秘密文件权限解决启动错误。

默认 Docker 网络为 `172.30.86.0/24`；若与已有网络冲突，按 [env 示例](../deploy/.env.example)一起修改 subnet、gateway、relay、proxy 四项。同机多实例也需分别配置项目名、网段和端口。HTTPS 来源只信任专用网关／proxy 地址，不可用任意来源通配符绕过检查。

</details>

<a id="admin-setup"></a>
## 4. 初始化管理员

服务已启动后，在**部署这套 Docker 的机器、同一个项目目录**执行：

```sh
umask 077
docker compose --env-file private/deploy/relay.env -f deploy/compose.yaml exec -T relay relay-admin bootstrap > private/deploy/bootstrap-code.txt
```

用本机文本编辑器打开 `private/deploy/bootstrap-code.txt`，将码填入后台的“一次性初始化码”，设置 **12—128 个字符**的管理员口令并提交。码只可用一次、15 分钟有效；过期重新执行该命令。成功后删除这份临时码文件，保存好管理员口令。

| 内容 | 来源 | 填在哪里 |
|---|---|---|
| 初始化码 | 上面的部署命令 | 后台首次初始化页面 |
| 管理员口令 | 初始化时由你设置 | 后台登录页面 |
| 千问 API Key／思源 Token | 各自服务账号 | 后台“配置”页 |
| 设备配对码 | 后台“设备”页生成 | 设备热点的配网页 |

不要把码或口令放进 URL、公开日志或 Issue。忘记管理员口令时，在同一命令中将 `bootstrap` 换成 `reset`，把新初始化码写入同一个私有文件，再到后台重新设置；这会撤销管理会话，保留任务与供应商配置。重新部署空卷不是重置密码的方法。

<a id="provider-setup"></a>
## 5. 配置千问和思源

登录后台，打开“配置”：

1. **千问 API Key**：在阿里云百炼控制台创建与当前北京地域 DashScope 接口对应的 API Key，并确认账号有对应语音识别模型的调用权限／额度。当前接口和模型在页面以只读字段显示；不同地域的 Key 不能混用，也不要填写阿里云 AccessKey 或其他产品订阅的专用 Key。获取入口可从 [DashScope 官方 Python SDK](https://github.com/dashscope/dashscope-sdk-python) 的 API Key 说明进入。
2. **思源云端收集箱 Token**：登录与思源相同的链滴账号，在[链滴设置](https://ld246.com/settings/account)的“账号”中取得 API Token。账号须具有云端收集箱使用权限；订阅要求以[思源官方说明](https://github.com/siyuan-note/siyuan)及客户端用户指南“收集箱”为准。直达设置页若提示未授权或无法访问，先登录链滴，再从“设置 → 账号”进入。这里不是思源本地内核“设置 → 关于”里的 API Token，也不是同步加密密钥。
3. 点击“保存并生效”。密钥保存后不会回填；以后编辑时留空会保留旧值，只有明确的清除选项会移除。设备端不用填写这两项。
4. 可先点击“检查千问连接”“检查思源连接”。它们检查 TLS 连通性，不代表 Key、账号权限或投递已经通过。需要确认完整链路时，再主动执行“端到端测试”：会调用千问并向思源写入一条测试记录，可能产生费用。

后端保存凭据使用 root 所有、0600 的秘密文件，经无网络 broker 提供受限访问；不把秘密写入镜像或部署 env。不要在设备、Issue 或公开截图中暴露它们。

<a id="pair-code"></a>
## 6. 生成配对码并连接设备

先记下“配置”页的服务器地址，再到“设备”页点“生成配对码”，复制新码。码是一次性的，**15 分钟有效**，一台设备用一个新码；过期重新生成，不改管理员密码。

随后按[手机配网与配对](DEVICE_INSTALL.md#pairing)连接设备热点。服务地址填刚才的地址，配对码填新码；等待“已配对”再关闭热点。多台设备可使用同一套后端，各自获得独立凭据。换家庭 Wi-Fi 可单独保存 Wi-Fi；已配对且服务地址未变时，配对码留空即可恢复“转写服务”。

首次启用前确认卡内待同步录音可以上传，必要时先保持“仅本地使用”。完成后录一条不含敏感信息的短语音，在后台“任务”页查看进度，在思源的**云端收集箱**确认结果；不是自动写入某个笔记本。失败时看[状态与重试](SUPPORT.md#data-and-status)，不要为确认状态反复新建录音或重试投递。

<a id="updates"></a>
## 普通更新、停止与数据保留

保留 `private/deploy/relay.env`、同一 `VOICE_MEMO_PROJECT` 和原有数据卷。镜像更新不会自动更新固件；先阅读对应 Release 的兼容说明。取得新版本**真实固定摘要**后，先 `docker pull`，再执行（下例为命令形式，占位内容必须替换）：

```text
./scripts/relay_deploy.sh upgrade 'registry/name:version@sha256:实际的64位摘要'
```

任务取消版本将运行数据库从 schema 2 升到 3，在原任务表增加取消标记，保留录音、任务、配对和凭据。先升级后端，再升级固件。升级前让设备停止提交任务，确认后台没有在途工作，按下文停止服务并备份项目持久卷和部署配置；随后启动原有旧实例并确认健康，再执行上述 `upgrade`，因为升级工具要求旧实例正在运行。备份至升级完成期间保持设备和用户不创建或处理任务。旧后端拒绝读取 schema 3，回退必须使用匹配的升级前卷备份；恢复该备份会丢失备份后新增或变更的数据，不能只换回旧镜像，也不能自动用旧数据库覆盖新状态。设备取消状态也需要新版固件读取，固件回退应使用对应的 Flash／状态备份，详见 [存储说明](FIRMWARE_STORAGE.md)。


管理器先排空在途工作、停止唯一写入服务并检查当前数据库，再启动候选；超时或不兼容会停止更新。不要用直接 `start` 覆盖运行镜像，也不要用旧数据库覆盖新状态。详细恢复边界见下文。

需要临时停止时，先让设备停止录音／上传，确认后台无在途任务，然后使用：

```sh
docker compose --env-file private/deploy/relay.env -f deploy/compose.yaml stop
# 恢复同一实例
./scripts/relay_deploy.sh start
```

启用了可选内置代理时，停止命令还需加 `-f deploy/compose.proxy.yaml`。不要执行 `down -v` 或 `docker volume prune`；它们会删除配置、配对或任务数据。不要在活跃写入时直接复制 SQLite 当作可靠备份；项目暂不提供一键备份／还原工具。主机级备份应在确认无在途工作并停止整套服务后，保全该项目的持久卷和部署配置，限制副本访问权限；重启前保持原卷身份与权限。

<a id="deploy-diagnostics"></a>
## 诊断与高级维护

保存诊断到私有文件，分享前自行脱敏：

```sh
./scripts/relay_deploy.sh status
docker compose --env-file private/deploy/relay.env -f deploy/compose.yaml ps
docker compose --env-file private/deploy/relay.env -f deploy/compose.yaml logs --no-color --tail 100 > private/deploy/runtime.log 2>&1
```

常见症状见[故障排查](SUPPORT.md#troubleshooting)。自行构建镜像及更新内部边界见下方；日常使用发布镜像无需构建。

<details>
<summary>自行构建、数据卷权限与失败恢复</summary>

从对应版本项目根执行：

```sh
docker build --target runtime --iidfile private/deploy/image.id -f server-relay/Dockerfile .
```

把 `image.id` 内实际 `sha256:...` 填入 env。仅使用 runtime；verification target 只供隔离测试。基础镜像、系统库及 Python 依赖由项目锁文件固定，构建上下文排除 private、历史和固件产物。

| 组件／卷 | 权限与用途 |
|---|---|
| relay | UID/GID 10001，只读根文件系统，无秘密卷挂载 |
| broker | root、无网络，仅访问 secrets 和 socket 卷 |
| volume-init | 首次自动初始化空卷；已有异常权限的卷拒绝启动，不递归清空或放宽权限 |
| data / work | relay 所有 0700，保存当前数据库与有期限的暂存数据 |
| secrets | root 所有 0700，秘密文件 0600 |
| broker-socket | root:10001 所有 0750，socket 0660 |

普通更新等接收会话／任务／请求为零后 checkpoint，检查当前数据库及已提交 WAL 的只读副本。候选失败时，仅当旧代码还能读取当前数据，才恢复旧代码；数据库不回滚。Docker 失联、有活跃 worker 或恢复未知时，不会假报成功，先恢复控制通道再处理。

协议、权限和验证入口见 [HTTP 契约](contracts/device-api-v2.json)、[broker 契约](contracts/config-broker-v1.json)、[测试说明](TESTING.md)。主机替身、容器、真实供应商和真机结果分别列在[支持记录](SUPPORT.md#verification-record)。

</details>
