# 发布与后续主线

首次只发布审阅后的源码与Docker候选 `0.2.0rc1`，不附带稳定固件、模型、原Flash、ELF、私人验收日志或开发历史。固件范围只见[支持状态](SUPPORT.md)；根许可证与第三方条款各自适用。

## 获取 0.2.0rc1 候选

`0.2.0rc1` 提供版本化源码与双架构 Docker 后端候选；固件仍处于待真机验收阶段。

源码见 [IdeaPickup 仓库](https://github.com/dahuang53/IdeaPickup)；[0.2.0rc1 Release](https://github.com/dahuang53/IdeaPickup/releases/tag/0.2.0rc1) 提供源码归档和 SHA256SUMS。此版本不提供稳定固件附件，硬件实测范围以支持表为准。

Docker 镜像包含 `linux/arm64`、`linux/amd64`，部署时使用固定索引身份：

```text
ghcr.io/dahuang53/ideapickup-relay:0.2.0rc1@sha256:ff801391c3c2200a3575204afead8579f02fadcc112aa7d68850955a6fc35f6d
```

将此值填入部署 env 的 `VOICE_MEMO_IMAGE`；无需改变数据卷或其他持久标识。安装和普通更新入口见[部署说明](DEPLOYMENT.md)。同一 Release 提供镜像内 Debian 包的[完整对应源码归档](https://github.com/dahuang53/IdeaPickup/releases/download/0.2.0rc1/ideapickup-relay-0.2.0rc1-corresponding-source.tar.gz)与校验值；精确包和来源索引见[第三方声明](../THIRD_PARTY_NOTICES.md)。

## R12 验收交付（r12-20261016）

R12使用既有仓库中的独立 `r12-20261016` 标签，交付两板固件、源码、SHA256SUMS及对应源码，保留历史 `0.2.0rc1` 原件。实际可获取文件以对应Release为准。此标签是验收修订标识；后端运行时 `code_version` 仍为 `0.2.0rc1`，部署必须使用下面配套索引摘要，不凭该字段或旧标签判断代码相同。

R12配套镜像的固定索引身份为：

```text
ghcr.io/dahuang53/ideapickup-relay:r12-20261016@sha256:970c10f0ce8ee260d731bc7165bfc52a0f13b1d5ad9710a01685f6fd2a4f6d60
```

索引仅含已审的 `linux/amd64` 与 `linux/arm64` 运行manifest，不含构建证明或私人历史。该交付的附件名称为 `ideapickup-r12-20261016-vocat16.tar.gz`、`ideapickup-r12-20261016-spotpear.tar.gz`、`ideapickup-r12-20261016-source.tar.gz`、`SHA256SUMS`，以及已核匹配、字节不变的 `ideapickup-relay-0.2.0rc1-corresponding-source.tar.gz`。最后一个文件沿用原名，内容对应相同系统依赖；应用源码以本次R12源码归档为准。

固件包附原第三方许可、实际manifest、支持限制和校验值，按[安装说明](DEVICE_INSTALL.md)选择板型。低电矩阵未实测、斑梨旧看门狗告警未解决等限制见[支持状态](SUPPORT.md)，不以附件发布掩盖延期项。发布流程要求精确材料审查、对象授权和实际下载核验，不改写rc1历史。

## 准备源码候选

```sh
.venv/bin/python scripts/export_public_source.py --report private/public-release/scan.json --output private/public-release/source-candidate
```

导出器复制明确允许的源文件、锁、公共文档和必要测试；不复制 `.git`、private、缓存、构建输出、设计过程和内部施工记录。报告只含路径／分类／摘要，不打印命中值；拒绝不明确的二进制、链接和源码变化。它生成新的私有目录，不推送、不签发公开授权，不要求填写日常资格JSON。

清单记录实际文件／模式与树摘要。导出后按README在该目录运行构建、测试和容器启动；无Git归档使用归档树身份，不继承外层私人仓库。导出工具的扫描是启发式检查，发布前仍审查具体内容、许可与材料。

## 首次公开

在具体候选审查通过且所有者批准目标与内容后，使用干净目录建立公共仓库和首次提交，发布固定版本与镜像digest。不得推送当前私人开发分支／历史。仓库创建、私密安全报告入口、实际CI及公开拉取均在首次发布时核对；源码候选准备不替代公开授权。

镜像保留每个实际系统／Python组件的版权材料；检查所有历史层、配置和软件库存，不能只检查最终文件系统。首发仅取经审阅的两个运行 manifest，新建双架构索引仅引用这两个载荷。BuildKit 原构建索引及其 attestation 不在首发材料中；不能直接推送含未审内容的原索引。运行 manifest、config 与本地构建索引分别记录并核对，不混用摘要。

同一 Release 提供[第三方声明](../THIRD_PARTY_NOTICES.md)约定的完整对应源码归档和 SHA256，并在镜像下载说明中给出直接入口；索引定义精确包／source 版本及所有 `.dsc`／原档摘要。未取得匹配对应源码或未能同时提供时不发布镜像。根源码许可不改第三方条款；ESP-SR模型属于Espressif限定材料，不由源码/Docker候选附带。

首次发布后，公共仓库的默认分支是唯一业务代码主线。本地按该主线建立工作分支，提交、测试、审查后同步；不长期维护私人分叉。后端修改通常只需相关测试、重建镜像和一次兼容更新；固件修改按板型构建／受保护安装／必要真机验证。协议或格式不兼容时先写清版本策略，不用旧数据库覆盖新事实。

公开目标、名称和具体待发布材料由本次发布确认记录给出；确认前不创建公开仓库、提交、标签或镜像推送。
