# 字体来源与重建

界面使用 `memo_font`，18 px、4 bpp，包含 ASCII 以及 `main/ui.c` 中全部非 ASCII 字符。生成的 `main/memo_font.c` 已纳入源码，正常固件构建无需 Node.js 或联网。

- 来源：Noto Sans CJK SC Regular，Noto CJK 仓库 `Serif2.003` 标签中 Sans 字体。
- 固定下载地址：<https://raw.githubusercontent.com/notofonts/noto-cjk/Serif2.003/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf>
- 原字体 SHA256：`2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`。
- 授权：SIL Open Font License 1.1，见同目录 `OFL.txt`；来源 <https://raw.githubusercontent.com/notofonts/noto-cjk/Serif2.003/Sans/LICENSE>。
- 原字体版权通知：© 2014-2021 Adobe (http://www.adobe.com/). 原字体及转换生成的 `main/memo_font*.c` 均保留该版权通知并遵守 SIL OFL-1.1；项目根许可证不替代字体许可。
- 生成器：`lv_font_conv 1.5.3`，含其依赖的 `package-lock.json`。只安装工程内开发工具，不更改全局配置。

从项目根目录执行 `sh scripts/generate_font.sh`。脚本下载源字体到已忽略的 `.tools/ui-font/`，核对哈希，以 `npm ci` 安装锁定工具，然后重建 `main/memo_font.c`。新增中文标签后执行一次此命令并提交生成结果。
