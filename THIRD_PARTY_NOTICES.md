# 来源与第三方许可

项目原创代码／文档使用根目录 Apache-2.0。下列材料各自保留其版权与许可，不因根许可证改变。项目名称不表示厂商背书。

| 材料 | 来源与本地修改 | 保留条款 |
|---|---|---|
| `main/board.c`、`board_lcd_init.h` | [Espressif BSP](https://github.com/espressif/esp-bsp/tree/b1bc71e7980663110055fbeba7eab59d5e689c2b/bsp/esp_vocat)；板级资源由本项目管理，补16MB SD DMA可达性检查 | 文件原版权／Apache-2.0；原文见 `licenses/Apache-2.0.txt` |
| `main/board_spotpear.c` | [小智斑梨板定义](https://github.com/78/xiaozhi-esp32/tree/184a688/main/boards)的引脚／显示配置及厂商公开引脚资料；本项目独立管理外设 | 小智 MIT 版权及原文保留在 `licenses/xiaozhi-MIT.txt`；引脚事实不等于复制完整聊天应用 |
| `components/esp-wifi-connect/` | [78/esp-wifi-connect](https://github.com/78/esp-wifi-connect/tree/cc103899ea8199b0fbf18c9b6e65152d9a92091e)，3.3.1；修改见组件 `VOICE_MEMO_PATCHES.md` | MIT，Terrence，保留组件 LICENSE |
| `components/usb_device_uac/` | [Espressif 1.3.1](https://components.espressif.com/components/espressif/usb_device_uac/versions/1.3.1)；本地USB门控／描述符适配 | Apache-2.0，保留 license.txt 和源文件版权 |
| `main/assets/cat/*.aaf` | [ESP-Brookesia](https://github.com/espressif/esp-brookesia/tree/b22c488f50bafe53342c8e171081bd736396ef58/core/brookesia_core/systems/speaker/assets/animations/emotion)六个原动画；无卡快眨眼从slow派生，说明／重建见目录README | Apache-2.0，保留该目录LICENSE与派生说明；导出检查已确认文件摘要 |
| `main/memo_font*.c` | [Noto CJK Serif2.003 / Sans](https://github.com/notofonts/noto-cjk/tree/Serif2.003/Sans)，使用NotoSansCJKsc-Regular，经过子集化／转换 | SIL OFL-1.1，Adobe版权；原文／原字体摘要／转换工具锁在 `assets/fonts/` |
| `main/assets/usb_no_host.pcm` | 既有“还没连上电脑喵”提示音；精确摘要与来源在同目录txt | 所有者已确认既有分发授权，LicenseRef-Owner-Confirmed-Redistribution；不改为Apache或CC0，不重复要求证明 |

构建时获取的依赖不随源码归档拷贝：ESP-IDF固定提交及组件/Python版本见根工具锁、组件锁与 `server-relay/requirements.lock.txt`。保留下载包原许可证；固件／模型分发应核对实际包内条款，而不是把所有依赖统称Apache。

ESP-SR 2.3.1使用[ESPRESSIF MIT License](https://components.espressif.com/components/espressif/esp-sr/versions/2.3.1/license)，授权限定用于Espressif产品。当前板型均为ESP32-S3；源码/Docker候选不附带模型或固件，后续附件须独立核对模型、预编译库及资产条件。

Docker基于锁定的Python/Debian镜像，安装固定libopus及Python包；系统和传递依赖的许可证各自适用。Python 3.11.16原文及pip vendored distlib启动器条款分别保留在 `licenses/Python-3.11-LICENSE.txt`、`licenses/distlib-LICENSE.txt`；setuptools启动器保留安装包MIT原文。镜像保留 `/usr/share/doc/*/copyright`、Python安装包的license/NOTICE及系统库存，供接收者查阅。

两架构实际 Debian 二进制包与对应 source 包／source 版本、原始 `.dsc` 和源码文件摘要／精确获取地址只记录在 [`licenses/container-source-index.json`](licenses/container-source-index.json)，镜像内为 `/usr/share/voice-memo/licenses/container-source-index.json`。二进制重建后缀与 source 版本分开记录，不以二进制包名或版本猜测来源。该索引覆盖全部安装的 Debian 包，包括 GPL／LGPL 组件。

本项目重新分发镜像时，在同一 GitHub Release 与镜像获取说明一起提供索引所列对应源码的完整归档 `ideapickup-relay-0.2.0rc1-corresponding-source.tar.gz` 及 SHA256；接收者可直接下载，不需另行申请或仅依赖外部 Debian 服务。归档保留未经修改的 Debian `.dsc`、全部原源码包／Debian 补丁及构建脚本，已逐项核对 `.dsc` 的 Source、Version、Checksums-Sha256。上游 [Debian Snapshot](https://snapshot.debian.org/) 精确地址同时保留为来源路由。后续镜像改变系统包时重新取得匹配 source 并同步提供；只发布库存或通用网页链接不替代对应源码。原创 Apache 许可不覆盖这些组件。

源码、Docker和以后固件附件分别审查。许可库存不是漏洞扫描结果，扫描无命中也不能代替实际材料和公开内容审查。当前硬件结果见支持表，发布流程见公开发布说明。

材料库存工具Syft固定1.54.0，仅作为维护工具使用，Apache-2.0，来源及摘要见 `scripts/container-tools.lock.json`。工具在项目private内安装，不进入业务镜像；SPDX库存和每层检查是材料检查，不能自动授予公开权限。
