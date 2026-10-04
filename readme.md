# ESP32-S3 双路隔离 RS485 / SPI LCD / 触摸屏

本文是项目文档总入口。目标模组为 **ESP32-S3-WROOM-1U-N16R8**（16 MB Flash、
8 MB Octal PSRAM），固件使用本机 **ESP-IDF 6.1**，项目目录沿用 `fireware`。

## 当前功能与资源

| 功能 | 外设和 GPIO | 当前行为 |
| --- | --- | --- |
| LCD | ST7796S，SPI2；MOSI1 / SCLK5 / DC3 / CS4 / BL6 / RESET10 | 480×320 横屏，RGB565，上电运行 LVGL 8.4 benchmark |
| 触摸 | GT911；SCL7 / SDA8 / INT9，与 LCD 共用 RESET10 | 上电启用，坐标与横屏一致 |
| RS485 第一路 | UART1；TX17 / RX18 / DE21；CN1 3/4 针 | 独立充电器 1 控制，运行设定每 15 秒写入 |
| RS485 第二路 | UART2；TX11 / RX12 / DE14；CN1 1/2 针 | 独立充电器 2 控制，与第一路并行 |
| CN2 隔离逻辑串口 | UART0；TX43 / RX44 | 默认 115200、8N1 |
| KEY2 | GPIO40，低有效 | 长按 2 秒切换触摸，一次按住只切换一次 |
| KEY1 | GPIO41，低有效 | 保留备用，只读状态，按下/松开各 200 ms 稳定滤波 |
| 下载与日志 | USB Serial/JTAG；GPIO19 / GPIO20 | 不占用三个业务 UART |
| 有源蜂鸣器（新增硬件） | HNB09A03 + SS8050；GPIO13；R30 10 kΩ 基极下拉 | 高电平鸣叫、低电平停止；上电关闭，网页开关 |

两路 485 默认分别运行 demo 充电器协议，独立控制两台充电器。启动先请求关机，运行中
每 15 秒写入电压/电流；首次启动预装设定、开关机和保护停机立即执行。网页提供选路、
CC/CP、预设、爬坡、保护、统计、波形、配色、配网和 OTA。通用 Modbus `03/04/06/10`
驱动保留；关闭充电应用后可启用双路只读示例。详见 [充电控制说明](fireware/CHARGER.md)。

蜂鸣器在最新原理图中已改用 GPIO13，GPIO35～37 继续保留给内部 PSRAM。
器件参数、驱动方式及 R28 装配建议见 [硬件说明第 4.4 节](hardware/原理图说明.md)。

## 文档索引

| 文档或资料 | 维护内容 |
| --- | --- |
| [硬件原理图说明](hardware/原理图说明.md) | 电源与隔离边界、器件/接口、引脚、UART 分配、样机检查 |
| [原理图 PDF](hardware/PCB/SCH_Schematic1_2026-10-04.pdf) | 当前四页电路连接的原始依据 |
| [立创 EDA 工程](hardware/PCB/ProDoc_Board1_2026-10-04.epro2) | 原理图、器件属性与 BOM 装配标记的编辑源 |
| [LCD 引脚及尺寸图](hardware/LCD/3.5寸LCD尺寸和引脚.png) | 屏幕外形、接口位置 |
| [LCD 模组与芯片资料](hardware/LCD/中景园ZJY350S11CTG21电容触摸技术资料-ST7796) | ST7796、GT911、模块原理图和厂家示例；详细文件见硬件说明索引 |
| [固件使用与 API](fireware/README.md) | 编译/烧录、默认行为、驱动调用、配置与验证步骤 |
| [双路充电控制与网页](fireware/CHARGER.md) | 移植范围、网络入口、15 秒写入时序、厂家协议、API、OTA 与验收 |
| [固件依赖清单](fireware/DEPENDENCIES.md) | 项目内依赖、本机工具、独立构建及删除旧参考目录后的验证 |
| [LVGL 移植说明](fireware/components/lvgl/PORTING.md) | LVGL 版本、包含范围、板级适配、字体/图片与配置 |
| [mDNS 来源与校验](fireware/components/mdns/SOURCE_SHA256.json) | 官方 mDNS 1.10.0 固定提交与文件 SHA-256 |
| [mDNS 许可](fireware/components/mdns/LICENSE) | 已内置 mDNS 的 Apache-2.0 许可 |
| [LVGL 原始许可](fireware/components/lvgl/LICENCE.txt) | 第三方源码的 MIT 许可 |
| [LVGL 文件校验表](fireware/components/lvgl/SOURCE_SHA256.json) | 398 个原始文件的 SHA-256 |
| [固件测试](fireware/tests) | 协议、触摸坐标、双路通信、按键/防误触策略及独立性校验脚本 |

## 构建和使用

在 PowerShell 中：

```powershell
Set-Location fireware
.\build.ps1 build
.\build.ps1 menuconfig
# COM5 为示例，替换为实际 USB Serial/JTAG 端口。
.\build.ps1 -IdfArgs @('-p', 'COM5', 'flash', 'monitor')
```

使用已有 SDK 和工具链，不升级；LVGL、mDNS 和网页资源已内置，构建无需联网下载它们。全部编译所需项目文件已放入 `fireware`，
`ref-code` 可由用户删除；本文及构建均不将它作为必需资料目录。硬件资料保留在 `hardware`。
`demo`、`tmp` 中的现有内容不是正式固件的构建输入。

LVGL benchmark 完成后在屏幕和 USB 日志显示结果。GPIO40 只控制触摸输入，关闭触摸不
停止 LCD 刷新或 benchmark。重新打开时，仍按住屏幕的手指需先松开；状态不保存，重启恢复开启。
GPIO41 没有业务动作。实体充电 UI 与 EC11 暂不移植，后续改用 LVGL 触摸 UI。

默认热点 `ChargerCtrl` / `12345678`；访问 `http://192.168.4.1/` 自动跳转到 8899 端口，
默认 PIN `1234`。STA 配网后可通过局域网 IP 或 `charger.local` 访问。
本次新增双 OTA 应用槽，首次从旧固件升级需通过 USB 完整烧录分区表和应用。

## 文档维护规则

1. 电路、位号或接口改变时，同步更新硬件说明、原理图导出文件及该说明中的 SHA-256。
2. GPIO/UART/按键行为改变时，同时更新固件 `board_pins.h`、驱动/配置、固件 README 与本页资源表。
3. 新增第三方源码或资源时，文件与许可放入 `fireware`，更新依赖清单和相应版本/校验记录。
4. 新增正式文档时，在本页索引登记，使用相对链接；不要链接计划删除的旧参考目录。
5. 验证记录区分编译、主机模拟测试与实板测试，不用编译通过代替实机通信或 FPS 结果。

2026-10-05：本轮验证记录见 [充电控制说明](fireware/CHARGER.md#验证与实板验收)。
测试包含原有 19 组驱动测试、新增 14 组充电控制测试和内嵌网页脚本测试。
未检测到串口设备，尚无本轮实板烧录、充电、双路总线或 FPS 测量结果。
