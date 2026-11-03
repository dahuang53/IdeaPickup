# 设备直连本地候选

本候选包含源码及 `vocat16`、`spotpear` 两套 16 MB 固件。候选附件与 `SHA256SUMS` 一起保存在本机交付目录。公开的 2026.10.27-rc1 仍对应旧中继版。

在交付目录用 `shasum -a 256 -c SHA256SUMS` 或 `sha256sum -c SHA256SUMS` 核对附件。源码包中的 `PUBLIC_SOURCE_MANIFEST.json` 列出源码文件的哈希。每个固件包内的 `firmware/manifest.json` 记录板型、源码身份、锁定 SDK、构建输入和固件文件哈希。按[设备安装说明](DEVICE_INSTALL.md)使用对应附件。

例如，在交付目录解压源码与喵伴包。斑梨将命令中的 `vocat16` 换成 `spotpear`。

```sh
shasum -a 256 -c SHA256SUMS
tar -xzf ideapickup-direct-source.tar.gz
mkdir -p IdeaPickup/private/downloads/vocat16
tar -xzf ideapickup-direct-vocat16.tar.gz -C IdeaPickup/private/downloads/vocat16
cd IdeaPickup
```

接着按安装说明准备锁定主机工具、确认设备身份和下载模式。示例的清单路径为 `private/downloads/vocat16/firmware/manifest.json`。


| 验收范围 | 已核对的行为 |
|---|---|
| 正式固件 | 喵伴和斑梨均完成受保护的应用更新、写后验证和实际启动。 |
| 真实云服务 | 喵伴分别通过有卡和无卡路径转写并投递至思源，收件结果经核对。 |
| 合成故障 | 斑梨完成成功、拒绝、断线和取消各五轮。有卡断网后继续同一任务并完成一次投递。 |
| 资源 | 两板分别处理 120 秒合成 PCM 和 64 KiB 分片事件。喵伴实测四条配网连接保持，第五条淘汰最旧连接。 |
| 历史与回退 | 原音频和历史状态抽查通过。旧中继未决状态只用于查看，原中继作为独立历史服务保留。 |

两板本地实测覆盖的边界见[存储契约](contracts/firmware-direct-cloud-v1.json)及[测试入口](TESTING.md)。资源对照采用同一设备、同一启动阶段的旧读数与 R2 实测，卡状态及配置有差异。无卡音频跨断网和断电恢复另行研究。斑梨长录音导出的间歇看门狗告警八轮复验未复现，根因待定位，作为已知问题单独跟踪。

候选固件由导出源码树重新构建。真机验收使用同一生产源码的步骤六 R2 构建。源码归档构建采用内容身份，固件文件的哈希以候选清单为准。
