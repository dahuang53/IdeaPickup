# 版本与下载

[返回首页](../README.md) · [部署后端](DEPLOYMENT.md) · [刷机](DEVICE_INSTALL.md) · [支持与限制](SUPPORT.md#known-limits)

## R12 验收交付（r12-20261016）

新用户使用 **[r12-20261016 Release](https://github.com/dahuang53/IdeaPickup/releases/tag/r12-20261016)**。这是预发布，提供已构建的两板固件和配套后端，包含内网 IP 部署与当前配网页。两板本轮必做真机检查已完成；完整低电矩阵未验证、斑梨历史看门狗告警未解决，详见[限制](SUPPORT.md#known-limits)。

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

### 配套 Docker 镜像

支持 `linux/amd64`、`linux/arm64`。部署时使用完整固定引用：

```text
ghcr.io/dahuang53/ideapickup-relay:r12-20261016@sha256:970c10f0ce8ee260d731bc7165bfc52a0f13b1d5ad9710a01685f6fd2a4f6d60
```

将它填入部署 env 的 `VOICE_MEMO_IMAGE`，并先执行 `docker pull`，具体步骤见[部署说明](DEPLOYMENT.md)。镜像公开可拉取，不需要用户创建 GHCR 令牌。

后端运行时 `code_version` 仍为 `0.2.0rc1`；**以发布标签和完整镜像摘要区分版本**，不能凭这个字段认定与旧镜像相同。固件精确身份见解压后 `firmware/manifest.json`。第三方许可及对应源码说明见[第三方声明](../THIRD_PARTY_NOTICES.md)。

## 获取 0.2.0rc1 候选

[历史 0.2.0rc1 Release](https://github.com/dahuang53/IdeaPickup/releases/tag/0.2.0rc1) 保留原源码和 Docker 候选，不含本次两板固件及内网 IP 修正。新用户请使用上面的 R12 配套材料，不混用旧后端。

历史固定镜像（仅供识别旧部署）：

```text
ghcr.io/dahuang53/ideapickup-relay:0.2.0rc1@sha256:ff801391c3c2200a3575204afead8579f02fadcc112aa7d68850955a6fc35f6d
```

## 更新与维护

普通更新分别按[后端更新](DEPLOYMENT.md#updates)和[固件更新](DEVICE_INSTALL.md#updates)执行，保留数据卷、配置和录音。发布者的导出、许可检查和公开流程见[维护说明](../CONTRIBUTING.md#发布维护)。
