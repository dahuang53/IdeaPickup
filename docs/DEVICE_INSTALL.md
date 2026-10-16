# 两板安装与普通更新

当前支持 16 MB QIO ESP-VoCat V1.2（`vocat16`）与斑梨 ESP32-S3-Touch-LCD-1.28-BOX（`spotpear`）。32 MB 喵伴不支持。板型必须按设备实际型号/资料选择；ESP32-S3 ROM 的芯片与容量不能识别屏幕、音频和板级引脚。工具检查所选板型与产物匹配，不把 MAC 当作板型证明。

R12两板本轮必做真机检查已完成，低电跳过及斑梨已知告警等边界见[支持表](SUPPORT.md)。不能仅凭命令可执行判断设备已验收；仅安装与实际板型匹配、来自对应Release且校验值明确的附件。

## 构建与候选包

在工程内按 README bootstrap 建立锁定工具，不改全局配置。已有公开包时可直接使用对应 manifest；本地构建：

```sh
./scripts/build_vocat16.sh
.venv/bin/python scripts/package_firmware.py --target vocat16 --output private/candidates/vocat16
# 斑梨用 build_spotpear.sh、--target spotpear 和独立输出目录
```

输出必须为新目录，不覆盖现有包。打包检查完成构建的真实输入与产物，拒绝源码/配置/产物变化及残留旧收据。工程自身有 Git 时应用版本取当时 HEAD；无 Git 源码归档使用 `src-<来源摘要>`，不继承父目录仓库身份。未提交源码仍按真实输入摘要记录，不能仅凭版本字符串把不同候选当成同一固件。包仅包含 bootloader、分区表、otadata、app、模型及 manifest，没有 MAC、串口、私人URL、凭据、录音、sdkconfig、ELF或历史 Flash。来源身份字段、布局和保留边界见[唯一产物契约](contracts/firmware-release-v1.json)。本地私有包不自动授权公开分发。

## 进入下载模式

先确认设备空闲、录放/USB麦克风已结束、无唯一未确认无卡音频/恢复任务、实际可用的恢复方式；本人的板型与原镜像材料继续保留在 private。不得烧eFuse、全片擦除或格式化SD。仅改变stdio描述符修复，没有更换USB控制器或console架构；若后续要改它们，必须先在已安装固件上实际验证软件入口。

16 MB 喵伴在**已安装并支持 `bootloader` 命令的 VoiceMemo 应用**上使用：

```sh
.venv/bin/python scripts/device_registry.py --board vocat16 --port <应用CDC端口> --mode software-rom
```

它保持CDC连接至真实应用确认，然后关闭句柄。忙态/未连接/超时均报错，不写Flash；普通重启不等于进入ROM。USB重新枚举后核对新的ROM端口。现有应用的空闲/活动任务/USB连接检查保持，不能把这一入口说成适用于不支持该命令的任意厂商固件。必须在当前已安装固件上实际核对该入口，不能仅凭版本说明假定可恢复。

斑梨沿用 USB Serial/JTAG 的官方软件复位路径，没有复合CDC的 `bootloader` 支持。它没有应用级忙态门，须在现有 status 的 CARDLESS transport_busy=0、录放停止且恢复/同步空闲时操作；不要在录音或唯一音频等待上传时强制复位。普通更新保留耐久配置/文件/任务，不能把仅在RAM中的未确认尾部说成可跨复位保存。仍使用厂商原固件、且当前暴露USB Serial/JTAG的设备也可先执行只读ROM连接探测：

```sh
mkdir -p private/rom-entry
ESPTOOL_CFGFILE="$PWD/scripts/esptool-protected.cfg" .venv/bin/python -m esptool \
  --chip esp32s3 --port <当前USB-Serial-JTAG端口> --before usb-reset \
  --after no-reset --no-stub read-mac > private/rom-entry/entry.log 2>&1
```

不能仅因同为ESP32-S3就假定这条路径可用；若当前固件占用了USB控制器、软件路径不可达，只有在已核实外部BOOT/RST可操作时才能使用厂商物理下载方式。不可按到外壳内按键、无可用软件入口时停止设备写入，先解决实际入口。无需刷只读资格固件、登记旧app/服务器/卡或证明历史编号。

## 首次安装 / 兼容应用更新

在确认的ROM端口执行对应板型包装器：

```sh
./scripts/flash_vocat16.sh <ROM端口> private/candidates/vocat16/manifest.json first-install
# 普通更新只写应用：
./scripts/flash_vocat16.sh <ROM端口> private/candidates/vocat16/manifest.json app-only
# 斑梨同样使用 flash_spotpear.sh 和对应 spotpear manifest
```

首次安装在写前把原Flash完整读到私有运行目录，只写契约内的五类产物。普通更新先核对当前bootloader/分区表/模型与候选依赖，备份本次应用拟擦写扇区，只写app；没有隐式重置配置、编号、任务、录音或元数据。依赖不同即停止，不偷偷把应用更新扩大为重新安装。以后不兼容格式或布局需另行明确版本处理，本轮不建设迁移框架。

每次检查ROM芯片、容量、安全状态；在无stub模式下读/写/核验，禁止force跳过安全检查。备份、ROM身份和原始输出只在自动建立的 `private/install-runs/` 中。候选文件在本次运行内冻结，所有写入结果和保护区做实际读回核验。出错后只恢复本次已备份拟写范围并校验，停留ROM；恢复未知时明确 `RECOVERY_UNCONFIRMED`，不得报告原机已恢复或继续启动坏候选。结果见该次 `result.json`，不要把原始日志粘贴进公开Issue。

成功写回与复位请求分别记录。S3软件强制下载位须清除，GPIO0须释放；使用工程内窄ROM return helper：斑梨在命令行句柄交接后重新确认官方 USB Serial/JTAG ROM 入口和同一实物，直接调用锁定 esptool 的 `hard_reset()`；喵伴保留经验证的 watchdog 返回路径，不写Flash/eFuse。返回异常只记 `RETURN_FAILED_BOOT_NOT_CONFIRMED`，不能由该标签断言实际仍在ROM。`FLASH_VERIFIED` 与 `RESET_REQUESTED_BOOT_NOT_CONFIRMED` 均不是设备启动/真机验收PASS。复位后用原 `scripts/device.py --port <重新枚举应用端口> --command status --log private/...` 查看实际项目、板型和版本，再核对精确候选、录放与数据保持。应用失效或CDC不可用时不能依赖该应用的软件命令自救，应使用已核实的ROM入口及本次原写入区备份；没有可达ROM入口时暂停。

## 主机测试

`./scripts/test_firmware_install.sh` 执行实际installer/bundle/软件ROM请求及stdio/锁定SDK VFS测试，使用FakeSerial与合成16MB Flash；不会打开真实串口。成功构建和打包只证明产物/封装检查，不证明Flash、USB CDC/UAC、实听或恢复路径已实测。

## 手机配置 Wi-Fi 与服务

长按猫脸约3秒进入联网设置，在手机选择屏幕所示热点，然后用浏览器打开 `http://192.168.4.1`。热点为开放连接，不输入密码；采用手动浏览器页面，不再触发系统网络登录弹窗。若手机提示此网络没有互联网，请选择继续使用。热点不替手机提供互联网，服务地址及配对码可提前从管理页复制。

网页分别显示 Wi-Fi 和服务状态。保存 Wi-Fi 后等待“已连接并保存”；已有保存网络会尝试自动连接，不必重新填写。保存服务设置后等待“服务已配对”；仅收到保存响应不代表配对完成。连接失败、保存未确认和配对失败分别在页面反馈。页面切出或重开不取消已提交操作，重新连接热点并打开原地址可查询当前结果；未提交的输入不要当作已保存。

配置期间热点保持开启，完成后在网页点“完成并关闭配置热点”或设备点“退出配网”。普通更新保留已有配置和录音；历史热点密码键保持原样但不再使用。当前手机、两板及更新验收状态按支持表和 R12 证据记录。
