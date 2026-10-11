# Docker 部署与普通更新

Linux Docker Compose 后端由非 root relay、无网络的 root 配置 broker 和自动卷初始化组成。用户只填写部署地址/镜像与必要TLS信息；管理员、千问Key、思源Token从HTTPS管理页配置。配置/接口定义见[当前契约](contracts/device-api-v2.json)、[broker契约](contracts/config-broker-v1.json)，验证边界见[支持状态](SUPPORT.md)。无需旧设备/旧服务迁移、共享令牌、人工资格记录或上传包中的root测试。

## 运行边界与首次启动

使用 Linux rootful Docker、Compose V2及Buildx；首版目标 `linux/arm64`、`linux/amd64`。工具不安装Docker/QEMU，不修改全局配置、防火墙、DNS或个人服务器。rootless/UID映射尚未通过本项目秘密权限边界，不能通过放宽权限启用。Docker Desktop Linux VM的原生/模拟与Linux宿主实测分开，已执行范围见支持表。

固定候选镜像和完整对应源码下载见[发布入口](PUBLIC_RELEASE.md#获取-020rc1-候选)。取得已发布镜像时可跳过构建；在工程根执行：

```sh
mkdir -p private/deploy
chmod 700 private/deploy
cp deploy/.env.example private/deploy/relay.env
chmod 600 private/deploy/relay.env
docker build --target runtime --iidfile private/deploy/image.id -f server-relay/Dockerfile .
```

必须填写两项：

- `VOICE_MEMO_IMAGE`：实际image.id中的 `sha256:...`，或已发布 `registry/name:version@sha256:...`。只用tag可能悄悄变版本，脚本要求固定身份。
- `VOICE_MEMO_PUBLIC_URL`：实际公开HTTPS根地址或子路径，例如 `https://relay.example.org`、`https://relay.example.org/memo`、有效IP证书的 `https://192.0.2.10:9443/memo`。证书/端口/主机/路径需一致，不加结尾斜线。HTTP不能作为设备服务入口。

其余保留默认；`VOICE_MEMO_PROJECT`应保持稳定，决定四个持久卷。默认专用网络 `172.30.86.0/24`如与已有网络冲突，按示例同时修改网关/relay/proxy地址；同机多套部署也须避免网段和端口冲突。这是普通Docker网络配置，没有设备/服务数量限制。env仅部署元数据，不接受API Key、管理员口令、设备令牌、旧兼容字段或shell展开，不要source它。

```sh
./scripts/relay_deploy.sh validate
./scripts/relay_deploy.sh start
./scripts/relay_deploy.sh status
```

入口只依赖宿主Python标准库与Docker CLI，不运行env文件。validate检查镜像/Compose/平台，不冒称实时权限已测。start自动初始化空卷、启动broker/relay；管理页可从空卷设置，供应商未配置时liveness正常、readiness准确返回not_configured，二者不能混为投递可用。异常权限的非空卷会拒绝启动，不递归修复/清空；按私有诊断修实际权限即可，无资格清单。stop/recreate/restart/更新均使用同一数据卷；禁止 `down -v`、volume prune或复制旧数据库覆盖。

| 组件/卷 | 边界 |
|---|---|
| relay | UID/GID10001，根文件系统只读，全部capability移除；无秘密卷挂载 |
| broker | UID0、无网络、根文件系统只读，仅访问秘密/socket卷；Linux peer UID校验限定relay |
| volume-init | 一次性固定四路径、无网络，仅CHOWN；空卷初始化，已有正确卷保持 |
| data / work | relay所有0700；当前SQLite、streams与工作区，临时音频/正文遵循HTTP契约清理规则 |
| secrets | root所有0700，秘密文件0600；不在镜像层或宿主env |
| broker-socket | root:10001所有0750/socket0660；管理control socket在relay私有data/control内 |

镜像基础/代理digest分别由 `server-relay/container-base.lock.json`、`deploy/proxy-image.lock.json`锁定；Python hash lock和固定libopus版本沿用后端锁。`.dockerignore`为允许列表，私有数据、历史、固件、主机环境不会进入构建上下文。production默认target与用户10001保持；verification target仅隔离测试用，不能部署为业务镜像。

## HTTPS 代理与路径

默认relay只发布宿主 `127.0.0.1:8765`；用已有HTTPS代理时参考 `deploy/nginx-existing.conf.example`。根路径用 `location /`，子路径如 `/memo/` 使用对应location；`proxy_pass`无URI后缀，保留原始路径前缀，不能截掉后再转发。服务入口、页面资源、登录cookie路径与设备API使用同一前缀；编码/中文路径按后端已有规范处理。

代理保留Host，覆盖X-Forwarded-Proto/For，关闭缓存、请求/响应缓冲及自动上游重试，body上限沿用2147483646。信任地址仅专用Docker网关与proxy地址，不能设 `*` 来绕过HTTPS来源校验。其他拓扑先核对实际代理来源；错误证书不得关闭校验。

可选内置Nginx：在env启用 `VOICE_MEMO_PROXY=1`，设置 `VOICE_MEMO_TLS_DIR` 为private内专用目录，仅含 `certificate.pem` / `private-key.pem`，实际root所有0600。它们只读挂载，不进入镜像。提供可信CA签发、域名或IP SAN匹配的证书；Linux宿主使用受控root安装权限，测试自签名例外仅隔离测试。

默认监听 `127.0.0.1:8443`；实际需要公开时显式设置HTTPS_BIND/PORT并自行安排访问策略。public_url含非443公开端口。Nginx master root仅读TLS key并建立worker，worker降权；无Docker socket，临时缓存路径有界tmpfs，关闭音频磁盘缓冲。模板覆盖根/子路径，本候选已实测隔离TLS，部署者仍需使用自己的可信证书，不能把代理头替身当作加密链路通过。

## 管理员初始化

首次部署取得一次性码，输出仅放private：

```sh
umask 077
docker compose --env-file private/deploy/relay.env -f deploy/compose.yaml exec -T relay relay-admin bootstrap > private/deploy/bootstrap-code.txt
```

在 `<public_url>/admin/` 输入码、设置管理员口令，登录后填必要API Key/Token，保存一次生效。码15分钟过期/一次性；使用后删除私有临时码文件，不放URL、聊天或公开日志。忘记管理员口令时同一受控部署机将bootstrap改为reset，撤销会话重新初始化，不清任务/录音。

## 普通版本更新与失败恢复

先按发布说明选择兼容版本的固定镜像，然后：

```sh
./scripts/relay_deploy.sh upgrade 'registry/name:version@sha256:<实际digest>'
```

脚本自动排空旧服务，等待接收会话/任务/请求为零，再关闭worker、checkpoint并停止唯一写入服务。超时拒绝更新、恢复旧服务接收；不强杀正在进行的工作。候选自动只读检查当前数据库格式，保留同一实例/配置/卷启动；不存在人工schema证明或带PASS的资格JSON，也不导入旧作者数据。

检查仅在writer停止后，从只读挂载的当前DB及已提交WAL建立权限0700/0600的独立临时副本，调用镜像已有current reader判断；不复制旧数据库回真实数据卷，不忽略WAL。临时检查空间为只在该容器存在的私有tmpfs，结束或容器删除即销毁；SQLite只在副本重建WAL索引，真实DB/WAL保持。版本事实仍来自镜像当前代码/manifest，没有人工证明。


选定镜像元数据在启动候选前耐久保存，运行镜像变化不能靠直接start覆盖；正常容器重启无永久drain字段/额外确认。候选startup/配置/清理或身份检查失败，先排空仍在运行的候选，再检查旧代码能否读**当前**数据库；可以才恢复旧代码，数据库不回滚。容器在崩溃重启且无活PID时停止其重启循环后再检查。失联但有活worker时，不当作已安全收尾；保留实际状态和数据，恢复Docker/控制入口后继续处理。

旧代码读不了当前格式时服务停止并明确报错；不要恢复陈旧数据库或手改schema。未来不兼容更新需明确版本策略，本轮只交付兼容普通更新。若已close而Docker stop失败，恢复Docker后start可重新创建确认关闭/安全的服务。进程中断或失去Docker也不伪报更新成功；以当前受保护env和实际容器身份为准。脚本锁只避免同时改同一部署，不是新运维平台。

部署命令报错时，把当前容器诊断留在private后检查，不要公开原始日志或一次性码：

```sh
docker compose --env-file private/deploy/relay.env -f deploy/compose.yaml logs --no-color --tail 100 > private/deploy/runtime.log 2>&1
```

## 验证入口与未验证项

`./scripts/test_relay.sh` 包含deployment操作/错误/真实当前schema读法的主机替身测试；`test_deployment`可以单独运行。Docker构建与生产Compose的隔离生命周期测试见测试说明，原始日志/测试Key均在private。主机、容器、真实供应商和真机结果分别记录，见支持表。
