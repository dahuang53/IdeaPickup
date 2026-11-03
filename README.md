# 拾想 · IdeaPickup

**随手录下想法，离线留在卡上；需要时转成文字，送进自己的思源收集箱。**

IdeaPickup 是运行在 ESP32-S3 上的录音工具。插入 microSD 卡即可离线录放。启用转写后，设备直接连接千问和思源云端收集箱，无需部署中继。

- 有卡录音保存在 SD 卡，可浏览和回放。启用转写后，新录音自动建立一次任务，也可在详情手动提交未处理录音。
- 无卡录音需要持续联网和可用凭据，通过千问实时识别。设备保留任务状态。音频无法回放，也无法跨断网或重启恢复。
- 两板支持触摸和本地语音操作，16 MB 喵伴还支持 USB 麦克风。

1. 按[设备安装说明](docs/DEVICE_INSTALL.md)选择正确板型并刷写固件。[直连版](docs/DEVICE_DIRECT_CLOUD_DELIVERY.md)是本地验收候选，公开的 [2026.10.27-rc1](docs/PUBLIC_RELEASE.md) 仍是旧中继版。
2. 只需录放时，插卡并保持「仅本地使用」。需要转写时，先[准备千问与思源凭据](docs/DEPLOYMENT.md#direct-credentials)。
3. [连接 Wi-Fi 并通过 USB 保存凭据](docs/DEVICE_INSTALL.md#direct-setup)，然后在配网页启用转写服务。
4. 录一条适合送往云服务的短语音，查看设备详情的处理状态，并在思源云端收集箱核对文字。

## 支持的硬件

| 设备 | 固件目标 | 存储规格 | USB 麦克风 |
|---|---|---|---|
| ESP-VoCat V1.2 喵伴 | `vocat16` | **16 MB QIO** | 支持 |
| ESP32-S3-Touch-LCD-1.28-BOX 斑梨 | `spotpear` | **16 MB DIO** | 不支持 |

**32 MB 喵伴和其他板型暂不支持。** 同为 ESP32-S3 不表示固件通用；请先核对实物，再下载对应包。首次安装使用项目的受保护刷写脚本，写入前备份原 Flash；普通更新只写应用，详见[安装与更新](docs/DEVICE_INSTALL.md)。

## 开始使用与遇到问题

| 要找的内容 | 入口 |
|---|---|
| 区分本地候选和旧公开附件 | [版本与下载](docs/PUBLIC_RELEASE.md) |
| 刷机、配网和凭据 | [设备安装与配置](docs/DEVICE_INSTALL.md) |
| 录放、状态和故障排查 | [使用帮助](docs/SUPPORT.md) |
| 旧中继部署和维护 | [历史部署入口](docs/DEPLOYMENT.md#legacy-relay) |

## 开发与许可

直接使用发布附件不需要编译固件。开发环境、锁定依赖、构建与测试命令见[开发说明](CONTRIBUTING.md)、[测试入口](docs/TESTING.md)和[后端说明](server-relay/README.md)。

项目原创代码采用 [Apache-2.0](LICENSE)。第三方代码、动画、字体、模型和提示音各自保留原许可，见[第三方声明](THIRD_PARTY_NOTICES.md)。不要在公开 Issue 中上传 API Key、设备令牌、原始日志、录音或数据库。
