# 版本与下载

[返回首页](../README.md) · [刷机与配网](DEVICE_INSTALL.md) · [凭据准备](DEPLOYMENT.md#direct-credentials) · [支持与限制](SUPPORT.md#known-limits)

## 当前预发布：2026.11.05-rc1

[下载 2026.11.05-rc1](https://github.com/dahuang53/IdeaPickup/releases/tag/2026.11.05-rc1)。这是设备直连版：在手机配网页保存千问 AI 平台 Key 和思源 Token 后，设备直接转写并投递，不需要部署 Docker 中继、填写服务器地址或配对码。

本版本包含持久密码热点、屏幕主动换密、网页凭据写入／替换／清除、一次处理意图及本地状态查看，以及[固件清理](FIRMWARE_CLEANUP.md)。两板构建与主机回归通过；喵伴最新验收候选已完成实际安装和真实录音→转写→思源投递。斑梨最新清理版本轮只有构建及主机回归，未重刷真机。完整范围见[直连交付记录](DEVICE_DIRECT_CLOUD_DELIVERY.md)，限制见[支持状态](SUPPORT.md#known-limits)。

### 下载哪个文件

| 文件 | 用途 |
|---|---|
| [ideapickup-2026.11.05-rc1-vocat16.tar.gz](https://github.com/dahuang53/IdeaPickup/releases/download/2026.11.05-rc1/ideapickup-2026.11.05-rc1-vocat16.tar.gz) | 仅 16 MB QIO 喵伴：固件、manifest 和许可 |
| [ideapickup-2026.11.05-rc1-spotpear.tar.gz](https://github.com/dahuang53/IdeaPickup/releases/download/2026.11.05-rc1/ideapickup-2026.11.05-rc1-spotpear.tar.gz) | 仅 16 MB DIO 斑梨：固件、manifest 和许可；本轮未重新真机验收 |
| [ideapickup-2026.11.05-rc1-source.tar.gz](https://github.com/dahuang53/IdeaPickup/releases/download/2026.11.05-rc1/ideapickup-2026.11.05-rc1-source.tar.gz) | 本版本源码、安装脚本、锁定依赖和使用文档 |
| [SHA256SUMS](https://github.com/dahuang53/IdeaPickup/releases/download/2026.11.05-rc1/SHA256SUMS) | 本次全部附件的 SHA256 校验值 |

固件包不含安装脚本，请同时下载源码包；不要用 GitHub 自动生成的 Source code ZIP 代替固件附件。32 MB 喵伴及其他型号不能使用这两套固件。

### 校验并取得项目目录

在同一目录下载源码包、对应板型固件包和 `SHA256SUMS`。计算已下载文件的摘要，与校验文件中**同名行的完整 64 位值**比较；不一致则重新下载，不刷写：

```sh
# macOS；Linux 可用 sha256sum
shasum -a 256 ideapickup-2026.11.05-rc1-source.tar.gz
shasum -a 256 ideapickup-2026.11.05-rc1-vocat16.tar.gz
# 斑梨用户将 vocat16 换为 spotpear
mkdir IdeaPickup
tar -xzf ideapickup-2026.11.05-rc1-source.tar.gz -C IdeaPickup
cd IdeaPickup
```

源码归档直接以项目文件为根；目标目录应尚不存在。也可从同一标签取得源码与安装脚本：

```sh
git clone --branch 2026.11.05-rc1 --depth 1 https://github.com/dahuang53/IdeaPickup.git IdeaPickup
cd IdeaPickup
```

随后按[设备安装说明](DEVICE_INSTALL.md)准备工具、解压板包并确认下载模式。已有 IdeaPickup 且依赖匹配时使用 `app-only`；原厂固件或不匹配布局使用安装说明定义的受保护入口，不绕过校验。旧中继配对信息不会自动变成供应商凭据，直连转写需要按[网页配置](DEVICE_INSTALL.md#direct-setup)保存两项 Key；本地录音和可兼容的历史状态保留。

本次不发布新的中继镜像。旧中继用户仍可使用下列历史版及其固定镜像，独立维护入口见[旧中继部署](DEPLOYMENT.md#legacy-relay)。

## 历史旧中继版：2026.10.27-rc1

[下载 2026.10.27-rc1](https://github.com/dahuang53/IdeaPickup/releases/tag/2026.10.27-rc1)。此版本包含手机热点切换修复、录音任务手动提交与取消、本地状态读取、服务器计算音频 SHA，以及无卡与配网页轮询精简。它仍是预发布：[两板低电矩阵未验证、斑梨看门狗告警未解决](SUPPORT.md#known-limits)。新斑梨固件仅完成构建；喵伴新固件已安装并完成启动、状态和保全检查，没有重做真人录音或物理断电测试。

### 下载哪个文件

| 文件 | 用途 |
|---|---|
| [ideapickup-2026.10.27-rc1-vocat16.tar.gz](https://github.com/dahuang53/IdeaPickup/releases/download/2026.10.27-rc1/ideapickup-2026.10.27-rc1-vocat16.tar.gz) | 仅 16 MB QIO 喵伴：固件、manifest 和许可 |
| [ideapickup-2026.10.27-rc1-spotpear.tar.gz](https://github.com/dahuang53/IdeaPickup/releases/download/2026.10.27-rc1/ideapickup-2026.10.27-rc1-spotpear.tar.gz) | 仅 16 MB DIO 斑梨：固件、manifest 和许可 |
| [ideapickup-2026.10.27-rc1-source.tar.gz](https://github.com/dahuang53/IdeaPickup/releases/download/2026.10.27-rc1/ideapickup-2026.10.27-rc1-source.tar.gz) | 本版本源码、安装脚本和部署文件 |
| [SHA256SUMS](https://github.com/dahuang53/IdeaPickup/releases/download/2026.10.27-rc1/SHA256SUMS) | 全部附件的 SHA256 校验值 |
| [ideapickup-relay-0.2.0rc1-corresponding-source.tar.gz](https://github.com/dahuang53/IdeaPickup/releases/download/2026.10.27-rc1/ideapickup-relay-0.2.0rc1-corresponding-source.tar.gz) | 镜像系统组件的完整对应源码；日常部署不需要下载 |

系统组件沿用原对应源码归档；本版本应用源码使用本次 source 归档。固件包不含安装脚本，请同时下载项目源码。

### 校验并取得项目目录

在同一下载目录保存所需附件与 `SHA256SUMS`，逐个计算摘要并与同名行完整比较；不一致则重新下载，不刷写：

```sh
# macOS；Linux 可用 sha256sum
shasum -a 256 ideapickup-2026.10.27-rc1-source.tar.gz
shasum -a 256 ideapickup-2026.10.27-rc1-vocat16.tar.gz
# 斑梨用户将最后的 vocat16 换为 spotpear
mkdir IdeaPickup
tar -xzf ideapickup-2026.10.27-rc1-source.tar.gz -C IdeaPickup
cd IdeaPickup
```

也可使用 `git clone --branch 2026.10.27-rc1 --depth 1 https://github.com/dahuang53/IdeaPickup.git IdeaPickup`。目标目录应尚不存在。后续操作见[刷机和配对](DEVICE_INSTALL.md)。

### 配套 Docker 镜像

支持 `linux/amd64`、`linux/arm64`。使用完整固定引用：

```text
ghcr.io/dahuang53/ideapickup-relay:2026.10.27-rc1@sha256:73069bf8d986ab6e3d150e449337aa130aad172bf1f6361b35094e43df94ae31
```

先按[部署说明](DEPLOYMENT.md)拉取并配置。旧版数据库升级前必须备份，**先升级后端再升级固件**；schema 3 不能直接由旧后端读取，回退须使用匹配的升级前备份。运行时 `code_version` 仍为 `0.2.0rc1`，请以发布标签和摘要区分。

本次固件是已核验的构建候选，manifest 中的构建时 Git HEAD 反映构建时点；实际编译输入摘要和固件文件摘要用于绑定源码与字节。旧有真机证据的适用范围见[支持状态](SUPPORT.md#verification-record)。

## 历史 R12 验收交付（r12-20261016）

历史版本 **[r12-20261016 Release](https://github.com/dahuang53/IdeaPickup/releases/tag/r12-20261016)**。这是预发布，提供已构建的两板固件和配套后端，包含内网 IP 部署与当前配网页。两板本轮必做真机检查已完成；完整低电矩阵未验证、斑梨历史看门狗告警未解决，详见[限制](SUPPORT.md#known-limits)。

### 下载哪个文件

| 文件 | 用途 |
|---|---|
| [ideapickup-r12-20261016-vocat16.tar.gz](https://github.com/dahuang53/IdeaPickup/releases/download/r12-20261016/ideapickup-r12-20261016-vocat16.tar.gz) | 仅 16 MB QIO 喵伴的固件、manifest、许可和支持说明 |
| [ideapickup-r12-20261016-spotpear.tar.gz](https://github.com/dahuang53/IdeaPickup/releases/download/r12-20261016/ideapickup-r12-20261016-spotpear.tar.gz) | 仅斑梨 16 MB DIO 的固件、manifest、许可和支持说明 |
| [ideapickup-r12-20261016-source.tar.gz](https://github.com/dahuang53/IdeaPickup/releases/download/r12-20261016/ideapickup-r12-20261016-source.tar.gz) | 项目源码、受保护安装脚本和 Docker 部署文件；不需要自行编译才能使用固件包 |
| [SHA256SUMS](https://github.com/dahuang53/IdeaPickup/releases/download/r12-20261016/SHA256SUMS) | 上述附件及对应源码的 SHA256 校验值 |
| [ideapickup-relay-0.2.0rc1-corresponding-source.tar.gz](https://github.com/dahuang53/IdeaPickup/releases/download/r12-20261016/ideapickup-relay-0.2.0rc1-corresponding-source.tar.gz) | 镜像内系统依赖的完整对应源码，供许可核查／重建；日常部署不必下载 |

最后一个归档沿用旧文件名，因为系统依赖未变；**R12 应用源码是第三行的 source 归档**。固件包中没有安装脚本，需要同时取得项目源码。不要用 GitHub 自动生成的 Source code ZIP 代替固件包。

### 校验并取得项目目录

在同一个下载目录保存所需 `.tar.gz` 和 `SHA256SUMS`。分别计算已下载文件的摘要，与 `SHA256SUMS` 中**同名一行完整的 64 位值**比较；任意不一致都重新下载，不刷写。

```sh
# macOS；Linux 可把 shasum -a 256 换成 sha256sum
shasum -a 256 ideapickup-r12-20261016-source.tar.gz
shasum -a 256 ideapickup-r12-20261016-vocat16.tar.gz
# 使用斑梨时，第二行替换为 spotpear 文件名
```

在下载目录执行以下命令，目标 `IdeaPickup` 必须是尚不存在的新目录。源码归档直接以项目文件为根，不需要去掉一层目录：

```sh
mkdir IdeaPickup
tar -xzf ideapickup-r12-20261016-source.tar.gz -C IdeaPickup
cd IdeaPickup
```

后文命令均在这个项目根目录执行。也可用 Git 取得同一版本的工具和源码，二选一即可：

```sh
git clone --branch r12-20261016 --depth 1 https://github.com/dahuang53/IdeaPickup.git IdeaPickup
cd IdeaPickup
```

冻结附件内的文档保留发布时原件；后续勘误和上手说明见[仓库 main 文档](https://github.com/dahuang53/IdeaPickup/tree/main/docs)。文档更新不会替换已经发布的固件或镜像。

### R12 配套 Docker 镜像

支持 `linux/amd64`、`linux/arm64`。部署时使用完整固定引用：

```text
ghcr.io/dahuang53/ideapickup-relay:r12-20261016@sha256:970c10f0ce8ee260d731bc7165bfc52a0f13b1d5ad9710a01685f6fd2a4f6d60
```

将它填入部署 env 的 `VOICE_MEMO_IMAGE`，并先执行 `docker pull`，具体步骤见[部署说明](DEPLOYMENT.md)。镜像公开可拉取，不需要用户创建 GHCR 令牌。

后端运行时 `code_version` 仍为 `0.2.0rc1`；**以发布标签和完整镜像摘要区分版本**，不能凭这个字段认定与旧镜像相同。固件精确身份见解压后 `firmware/manifest.json`。第三方许可及对应源码说明见[第三方声明](../THIRD_PARTY_NOTICES.md)。

## 获取 0.2.0rc1 候选

[历史 0.2.0rc1 Release](https://github.com/dahuang53/IdeaPickup/releases/tag/0.2.0rc1) 保留原源码和 Docker 候选，不含本次两板固件及内网 IP 修正。请使用当前版本配套材料，不混用旧后端。

历史固定镜像（仅供识别旧部署）：

```text
ghcr.io/dahuang53/ideapickup-relay:0.2.0rc1@sha256:ff801391c3c2200a3575204afead8579f02fadcc112aa7d68850955a6fc35f6d
```

## 更新与维护

普通更新分别按[后端更新](DEPLOYMENT.md#updates)和[固件更新](DEVICE_INSTALL.md#updates)执行，保留数据卷、配置和录音。发布者的导出、许可检查和公开流程见[维护说明](../CONTRIBUTING.md#发布维护)。
