# 设备安装、更新与配对

[返回首页](../README.md) · [固件下载](PUBLIC_RELEASE.md) · [部署后台](DEPLOYMENT.md) · [使用与排错](SUPPORT.md)

本页默认安装已发布的 R12 固件，**无需自行编译**。先核对硬件，再准备安装工具；刷写不会替你部署后端。只需离线录放可跳过后端和配对。

## 1. 确认硬件与准备材料

| 实物 | 下载的固件包 | 安装脚本 |
|---|---|---|
| ESP-VoCat V1.2 喵伴，16 MB QIO | `ideapickup-r12-20261016-vocat16.tar.gz` | `scripts/flash_vocat16.sh` |
| ESP32-S3-Touch-LCD-1.28-BOX 斑梨，16 MB DIO | `ideapickup-r12-20261016-spotpear.tar.gz` | `scripts/flash_spotpear.sh` |

32 MB 喵伴和其他型号不能使用这些固件。同为 ESP32-S3、能读到 MAC、Flash 容量相同，都不能证明屏幕、音频和引脚兼容。

准备可传数据的 USB 线、macOS／Linux 电脑，以及按[下载说明](PUBLIC_RELEASE.md#校验并取得项目目录)取得的项目源码与对应固件包。先核对 SHA256。已有 SD 卡里的文件应先自行备份；不需要为了安装格式化卡。停止设备录音、播放、USB 麦克风和同步，已有配置的设备先保存“仅本地使用”，避免更新后自动上传未处理录音。

安装前确认实际可用的下载入口。可对照 [Espressif 喵伴说明](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp-vocat/user_guide_v1.2.html)和[斑梨 BOX 官方说明](https://spotpear.com/index.php/wiki/ESP32-S3-N16R8-AI-DeepSeek-XiaoZhi-XiaGe-Qwen-DouBao-1.28-inch-Round-LCD-BOX-TouchScreen.html)核对实物和按键；外壳无法按到 BOOT／RST 时，不把图上的按键当作可用恢复方式。

## 2. 安装项目内的工具

以下都在项目根目录执行。需要 Git、curl、Bash；原生 Windows 和 WSL2 的刷机路径未经本项目验收。项目锁定 uv **0.12.5**，已有该版本可跳过下载；否则使用 [uv 官方安装器](https://docs.astral.sh/uv/getting-started/installation/)安装到本工程，不修改 shell 全局配置：

```sh
mkdir -p private .tools/uv
curl --proto '=https' --tlsv1.2 -fLsS https://astral.sh/uv/0.12.5/install.sh -o private/install-uv.sh
UV_UNMANAGED_INSTALL="$PWD/.tools/uv" sh private/install-uv.sh
export PATH="$PWD/.tools/uv:$PATH"
uv --version
./scripts/bootstrap.sh --host-only
```

最后一步建立锁定的 Python 和 esptool 环境，不构建固件，也不安装 ESP-IDF SDK。下载失败应先解决网络或依赖错误，不用其他版本工具强行刷写。重新打开终端时，先回项目根并重新执行 `export PATH=...`。

## 3. 解压固件包并识别端口

把校验过的固件压缩包放入项目的 `private/downloads/`。以下以喵伴为例，斑梨将两个 `vocat16` 替换为 `spotpear`；解压目录应是新目录：

```sh
mkdir -p private/downloads/vocat16
tar -xzf private/downloads/ideapickup-r12-20261016-vocat16.tar.gz -C private/downloads/vocat16
.venv/bin/python -m serial.tools.list_ports -v
```

实际 manifest 在 **`private/downloads/vocat16/firmware/manifest.json`**，不是解压目录根部。项目脚本来自源码目录，固件文件来自解压的附件，不要混用另一板型或旧包。

根据端口列表确认当前连接的是哪台设备。macOS 通常形如 `/dev/cu.usbmodem...`，Linux 通常为 `/dev/ttyACM...`；不要照抄示例编号。若列不出端口，先查数据线和供电；Linux 的权限错误按串口设备所属组处理，不用 `sudo` 绕过安装脚本。关闭正在占用该端口的串口监视器。

<a id="download-mode"></a>
## 4. 进入下载模式

### 首次安装：当前还是其他固件

喵伴的厂商固件不一定支持本项目软件命令。若 BOOT 实际可操作，按官方说明在上电时按住 BOOT 进入下载模式，随后释放 BOOT。有电池时单拔 USB 不一定断电，先用已确认的 POWER 键关机再操作。不要猜测不可见按键的位置。

斑梨若当前暴露 USB Serial/JTAG，可按下方的只读软件 ROM 探测进入下载模式；不可达时，按 BOX 官方说明核对实物的 POWER／BOOT 键再进入。官方图中 1 为电源、2 为 BOOT：关机后按住 BOOT 再开机，进入下载状态后释放。仅适用于图示按键一致的实物。

**没有实际可用的下载入口时先停止，不写 Flash。** 不运行 `erase-flash`、不烧 eFuse，不使用 `--force` 绕过检查。

### 已安装 IdeaPickup：优先使用验证过的软件入口

先填实际应用端口（此处示例值必须替换）：

```sh
MEMO_PORT='/dev/cu.usbmodem实际编号'
mkdir -p private/rom-entry
.venv/bin/python scripts/device.py --port "$MEMO_PORT" --command status --log private/rom-entry/before.log
```

确认录放、同步、恢复任务已结束；无卡唯一未确认音频不能靠强制复位保全。喵伴使用支持 `bootloader` 命令的已安装应用：

```sh
.venv/bin/python scripts/device_registry.py --board vocat16 --port "$MEMO_PORT" --mode software-rom
```

工具等待真实应用确认后才关闭连接。忙态、超时或未连接均应停止；普通重启不等于已进入下载模式。它不适用于任意厂商固件。

斑梨使用 USB Serial/JTAG 路径；此入口没有应用忙态门，必须先确认设备空闲：

```sh
ESPTOOL_CFGFILE="$PWD/scripts/esptool-protected.cfg" .venv/bin/python -m esptool \
  --chip esp32s3 --port "$MEMO_PORT" --before usb-reset \
  --after no-reset --no-stub read-mac > private/rom-entry/entry.log 2>&1
```

首次安装且已有 USB Serial/JTAG 时，也可先在终端设置 `MEMO_PORT`、创建该日志目录，再执行这个探测；不要求厂商应用能响应本项目 `status` 命令。

USB 重新枚举后，再运行端口列表命令核对 **ROM 端口**；它可能与原应用端口不同。重新设置下面的变量，不继续沿用旧端口猜测。

## 5. 首次刷写与启动检查

喵伴：

```sh
MEMO_ROM_PORT='/dev/cu.usbmodem实际ROM编号'
./scripts/flash_vocat16.sh "$MEMO_ROM_PORT" private/downloads/vocat16/firmware/manifest.json first-install
```

斑梨使用自己的包和脚本：

```sh
MEMO_ROM_PORT='/dev/cu.usbmodem实际ROM编号'
./scripts/flash_spotpear.sh "$MEMO_ROM_PORT" private/downloads/spotpear/firmware/manifest.json first-install
```

**只执行符合自己板型的一条。** 首次安装会先完整备份原 Flash 到 `private/install-runs/`，随后写入 bootloader、分区表、otadata、app、模型；这是替换原系统，备份不是自动迁移旧系统数据。不要删除备份或中途拔线。脚本核对芯片、16 MB 容量、安全状态、文件和保护区；出错时按实际结果处理，不追加手工擦除命令。

结束后先确认屏幕出现本项目猫脸／首页。重新枚举端口，填入应用端口并读取状态：

```sh
MEMO_PORT='/dev/cu.usbmodem实际应用编号'
.venv/bin/python scripts/device.py --port "$MEMO_PORT" --command status --log private/startup.log
```

核对板型和版本，再做一次短录音／回放（有卡时）。`FLASH_VERIFIED` 只证明写入和读回一致；`RESET_REQUESTED_BOOT_NOT_CONFIRMED` 不代表已启动。黑屏或串口不响应时看[安装故障](SUPPORT.md#install-troubleshooting)，不要把未知结果当作成功。

<a id="pairing"></a>
## 6. 手机配网与设备配对

只用离线有卡录放：保持“仅本地使用”，直接看[日常使用](SUPPORT.md#daily-use)。要转写，先完成[后端部署和配置](DEPLOYMENT.md)，从后台准备好**服务地址和新配对码**。初始化码、管理员密码、千问 Key 和思源 Token 都不填在设备上。

1. 长按设备首页猫脸约 **3 秒**进入联网设置。iPhone／Android 在 Wi-Fi 设置中连接屏幕所示热点；当前热点无需密码。如提示没有互联网，选择继续使用。
2. 在 Safari／其他浏览器地址栏手动打开 **`http://192.168.4.1`**。当前版本不使用系统自动弹出的配网页；不需要每次忘记热点。该地址只有设备热点开启、手机连到它时可用。
3. 展开 Wi-Fi 设置，选择家庭／办公的 **2.4 GHz** 网络，填写密码，点“连接并保存”。等网页显示连接成功与保存完成。已有正确的 Wi-Fi 不用重新填写；列表中的信号图标表示强度，锁表示需要密码。
4. 选择“转写服务”，填[部署时确定的服务地址](DEPLOYMENT.md#addresses)及后台生成的新配对码，点“启用此转写服务”。等网页更新为“已配对”；“设置已保存”本身不等于配对成功。局域网可直接填内网 IP（默认 8765）；自定义端口／路径不可遗漏。服务器填可信 HTTPS 地址。
5. 确认状态后点网页“完成并关闭配置热点”，或在设备上点“退出配网”。热点消失、原配网页无法访问是正常结果。随后录一条短语音，在详情／后台任务与思源云端收集箱核对结果。

选择模式后需要点击启用按钮才保存；只点模式选项并未改变已生效配置。“仅本地使用”会停止联网同步，但保留 Wi-Fi、服务地址和已有配对；恢复原服务且地址未变时，配对码可留空。更换服务器需新配对码。

切到其他 App 复制地址后，可以返回原浏览器页面；已提交操作继续执行，未提交输入不算已保存。设备热点不提供手机上网，最好提前准备好地址和码；码过期回后台生成新的即可。页面打不开、持续等待或配对失败时看[故障排查](SUPPORT.md#troubleshooting)。

<a id="updates"></a>
## 普通固件更新

先保存“仅本地使用”，停止录放／同步，下载并校验同板型新包，解压到新的目录，再按上面的下载模式步骤进入 ROM。使用新包路径、将模式改为 `app-only`：

```sh
./scripts/flash_vocat16.sh "$MEMO_ROM_PORT" private/downloads/vocat16/firmware/manifest.json app-only
# 斑梨改用 flash_spotpear.sh 与自己的 spotpear manifest；不要两条都执行
```

脚本先核对 bootloader／分区表／模型依赖并备份本次拟写扇区，只写 app，保留配置、编号、任务和 SD 录音。依赖不同会停止，不能为了通过而改用 `first-install`；先看新版本的兼容说明。更新后确认启动、原有录音和状态，准备好联网后再自行恢复转写服务。

<details>
<summary>受保护安装的失败结果与恢复边界</summary>

候选在本次运行内冻结，写入和保护区逐项读回核验。失败时只尝试恢复本次已备份的拟写范围并校验，保留真实结果在运行目录 `result.json`。`RECOVERY_UNCONFIRMED` 表示恢复未确认；`RETURN_FAILED_BOOT_NOT_CONFIRMED` 表示返回步骤异常，不能据此断言仍在 ROM 或已经启动。保留备份和日志，停止继续刷写并按支持页报告脱敏结果。

返回应用沿用板型专用 ROM helper：斑梨锁定 esptool `hard_reset()`，喵伴使用经验证的 watchdog 路径，不写 eFuse。应用失效或 CDC 不可用时，只能使用实际核实过的 ROM 恢复入口；应用软件命令不能自救。原始 Flash、串口身份和日志只留在 private，不上传公开 Issue。产物定义见[固件契约](contracts/firmware-release-v1.json)。

</details>

## 自行构建与维护者测试

只在需要开发时运行完整 bootstrap；依赖见[测试入口](TESTING.md)。例如：

```sh
./scripts/bootstrap.sh
./scripts/build_vocat16.sh
.venv/bin/python scripts/package_firmware.py --target vocat16 --output private/candidates/vocat16
```

本地打包的 manifest 位于 `private/candidates/vocat16/manifest.json`，与发布附件多一层 `firmware/` 的布局不同。斑梨对应 `build_spotpear.sh` 和 `--target spotpear`。输出必须是新目录，打包器会校验实际构建输入和产物，拒绝变化或旧收据。

`./scripts/test_firmware_install.sh` 使用 FakeSerial 与合成 Flash，不打开真实设备。构建／打包通过不等于刷写、触摸、实听或恢复路径通过；支持范围见[验证记录](SUPPORT.md#verification-record)。
