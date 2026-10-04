# ESP32-S3 横屏驱动与 LVGL benchmark

目标模组：**ESP32-S3-WROOM-1U-N16R8**，16 MB Flash、8 MB Octal PSRAM。
项目目录沿用要求中的 `fireware`。LCD 为 **480×320 横屏**，默认上电运行从
本地内置的 **LVGL 8.4.0 demo benchmark**；底层驱动不依赖 LVGL。
两路 RS485 分别使用 UART1/UART2，CN2 使用 UART0，控制台使用 USB Serial/JTAG。
本工程不再依赖旧 `ref-code` 目录，可删除该目录；完整依赖见 [DEPENDENCIES.md](DEPENDENCIES.md)。
全部文档入口见 [根目录 readme.md](../readme.md)。

## 硬件与参考依据

- 板级引脚：`../hardware/PCB/SCH_Schematic1_2026-10-04.pdf` 第 2～4 页。
- 当前硬件与接口说明：[双路隔离 RS485 原理图说明](../hardware/原理图说明.md)。
- 屏幕：ZJY350S11CTG21，ST7796S，原生 320×480，四线 SPI，RGB565；
  使用厂家 `USE_HORIZONTAL=2` 对应的 `MADCTL=0x28`，逻辑分辨率 480×320。
- LCD 初始化寄存器：屏幕资料 `03-程序源码.zip` 内 STM32F407 SPI 示例 `HARDWARE/LCD/lcd_init.c`。
- GT911 地址、时序和寄存器：屏幕资料内 `GT911 Datasheet_20130319.pdf`；模块 `02-原理图.zip` 确认 LCD RESET 与 CTP_RST 共用 `RES`。
- ST7796 初始化表、UART 半双工控制与 Modbus 协议实现在 `components/board_drivers`。
  当前屏幕 gamma/VCOM 采用当前厂家示例，所需数据已编入源码。
- LVGL 源码与 benchmark：`components/lvgl`，版本 8.4.0。
  原样保留核心、benchmark 资源和 MIT 许可证，详情见 [PORTING.md](components/lvgl/PORTING.md)。
- 参考工程是旧板：FT6336、TCA9554、AXP2101、GPIO0 按键和 4G 未引入。
  本板通过独立 `board_lvgl` 组件连接 ST7796 / GT911。

| 模块 | 外设 | GPIO / 配置 |
| --- | --- | --- |
| LCD MOSI / SCLK | SPI2 | GPIO1 / GPIO5，默认 20 MHz，SPI mode 0 |
| LCD DC / CS | GPIO | GPIO3 / GPIO4 |
| LCD 背光 | LEDC timer0/channel0 | GPIO6，高电平亮，5 kHz，10 bit |
| LCD + GT911 复位 | 共用 RES | GPIO10，低有效 |
| GT911 SCL / SDA | I2C0 | GPIO7 / GPIO8，100 kHz |
| GT911 INT | GPIO | GPIO9；复位时选地址，运行时输入 |
| 隔离 RS485 第一路，CN1 3/4 | UART1 + U5 ISO3082 | TX17 / RX18 / DE 与 /RE = GPIO21 |
| 隔离 RS485 第二路，CN1 1/2 | UART2 + U7 ISO3082 | TX11 / RX12 / DE 与 /RE = GPIO14 |
| KEY1，保留输入 | GPIO41，低有效 | 双边沿 200 ms 稳定滤波，仅查询状态 |
| KEY2，触摸开关 | GPIO40，低有效 | 30 ms 消抖，稳定按下后长按 2 秒切换触摸 |
| 隔离串口 CN2 | UART0 + CA-IS3722HS | TX43 / RX44，默认 115200，8N1 |
| USB 日志与下载 | USB Serial/JTAG | GPIO19 / GPIO20 |

GPIO2 对应 H1 第 12 针预留 MISO；当前屏幕 11 针接口未接 SDO，因此 SPI 设置 `miso_io_num=-1`。
GPIO35～37 不作外设 GPIO 使用，保留给 N16R8 模组内部 Octal PSRAM。
最新原理图新增的 HNB09A03 有源蜂鸣器已接 GPIO13，并设 R30 10 kΩ 基极下拉。
当前固件未定义或启用蜂鸣器，后续可用普通 GPIO 实现高电平鸣叫、低电平停止，详见 [硬件说明第 4.4 节](../hardware/原理图说明.md)。
KEY1/KEY2 不包含 BOOT(GPIO0) 与硬件复位键。

## 本机编译

使用已有安装，不下载或切换 ESP-IDF：

- ESP-IDF：`C:\esp\v6.1\esp-idf`，版本 `v6.1`。
- Xtensa GCC：`C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204`，GCC 15.2.0。
- Python：`C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`。
- 安装器环境文件：`C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`。

在 `fireware` 内运行：

```powershell
.\build.ps1 build
.\build.ps1 menuconfig
.\build.ps1 size
# COM5 仅为示例，替换为本机实际 USB Serial/JTAG 端口。
.\build.ps1 -IdfArgs @('-p', 'COM5', 'flash', 'monitor')
```

本机 ESP-IDF 6.1 的编译参数文件生成过程会破坏中文路径。`build.ps1` 与
`tools/idf_short_path.py` 保留本项目的 NTFS 英文短路径，指向同一份源码和 `build`
目录；不复制源码、不映射盘符、不修改 ESP-IDF 安装或全局 Git 配置。
因此在当前中文目录下使用此脚本，而不是直接运行 `idf.py`。
若拷贝到禁用 8.3 短文件名的磁盘，请使用英文项目路径。
安装位置改变时可用 `-IdfProfile` 指向对应版本的本机安装器环境文件。

默认 CPU 240 MHz、性能优化编译，Flash DIO 40 MHz，PSRAM Octal 80 MHz，启动时初始化与内存测试。
使用标准单应用分区（1 MB factory 分区），其余 Flash 未分配；
配置 16 MB Flash 不代表自动使用全部空间。未加入 OTA 和业务数据分区。
需要完整烧录时使用 `flash`，它会按生成的分区表烧录 bootloader、分区表和 app。

## 启动示例

`main/app_main.c` 依次初始化 LCD、触摸、按键、CN2 串口和两路 485：

- LCD 清黑后背光 80%，独立 GUI 任务自动运行一次 LVGL benchmark，全速模式。
  完成后屏幕显示可触摸滚动的结果表，USB 日志输出 `Weighted FPS`、`Opa. speed`
  及各场景 FPS。复位可重新测试；没有预填或模拟 FPS 数值。
- GT911 作为 LVGL pointer 输入。benchmark 运行时由 LVGL 独占触摸帧读取，
  主循环不再读取同一就绪寄存器，避免漏报或错误释放。
- 如需纯驱动测试，在 `Board basic drivers` 关闭 `Run LVGL 8 benchmark at boot`，
  再开启 RGB 色条；此时触摸坐标与释放事件恢复输出到 USB 日志。
- 上电默认开启触摸；GPIO40 长按 2 秒切换开启/关闭，每次持续按住只切换一次，
  必须松开后再次长按才能再次切换。状态不保存到 Flash，重启恢复开启。
- GPIO41 保留备用；按下和松开均需稳定 200 ms，只读取状态并记录变化，无业务动作、无长按事件。
- CN2 串口收到数据时以十六进制输出到 USB；回显默认关闭，可在 menuconfig 开启。
- 两路 485 上电均初始化，默认各为 9600、8N1；可分别配置波特率、校验、停止位。
  两路均提供 Modbus RTU 主站 API，独立锁和 RX 队列；一路等待响应或超时不占用另一路。
- 默认不发出猜测设备地址的轮询或写操作。在 `Board basic drivers` 开启
  `Enable two concurrent READ-ONLY Modbus demo tasks` 后，创建两个独立任务同时轮询两路。
  每路独立配置从站地址及起始寄存器，默认各为地址 1、功能码 03、起始寄存器 0、
  读取 2 个寄存器，响应超时 2 秒，每次事务完成后间隔 2 秒。一个任务失败不停止另一个。
- 单个外设初始化失败会记录错误，其余模块仍继续初始化；PSRAM 启动检测由 IDF 自身处理。

应用日志与 bootloader 控制台走 USB。ESP32-S3 ROM 在应用运行之前仍可能从
默认 UART0/GPIO43 输出启动字节；未修改 eFuse。CN2 外部设备应能忽略上电杂字节。
三个 UART 均用于业务接口，不能再将应用控制台切换到 UART；串口驱动有编译期检查。

## LVGL 移植设置

- 本地组件 `components/lvgl` 使用原始 Kconfig；配置来源是 `sdkconfig`，
  初始值在 `sdkconfig.defaults`。不需要联网下载组件，也不需要另写 `lv_conf.h`。
- RGB565，`LV_COLOR_16_SWAP=n`：字节序仅在 LCD 驱动中转换一次。
- 单个 PSRAM 绘图缓冲默认 480×40 像素（38,400 字节）；可调 10～80 行。
  LCD 使用内部 DMA 缓冲分块发送，等待 SPI 完成后才调用 `lv_disp_flush_ready()`。
  这是同步 SPI 移植，benchmark 包含该传输开销。
- `esp_timer` 每 5 ms 更新 tick；GUI 任务栈 8 KB、优先级 4，双核时运行在核 1。
  `lv_timer_handler()` 循环至少让出 1 tick，触摸读取周期 20 ms。
- 开启压缩字体、复杂绘制及 benchmark 所需控件；LVGL malloc/free 使用 IDF 堆，
  由已启用的 PSRAM malloc 策略分配大块内存。
- 默认 SPI 保持 20 MHz。可在 menuconfig 调到 40 MHz，但实际稳定性需上板检查。
  比较 FPS 时应记录 CPU/SPI 频率、缓冲大小和优化等级，测试期间避免其他串口负载及触摸操作。
- LVGL 所有对象操作应在 GUI 任务中执行。当前没有对外提供跨任务 LVGL 锁或对象接口。
  同时从其他任务调用 LCD 绘图接口会覆盖 benchmark 画面，应在关闭 benchmark 后做色条测试。
- 为减少工程体积，仅内置核心和 benchmark；其他 demo/examples 未包含，若在 LVGL 菜单
  勾选会明确报配置错误。以后如需扩展，应引入匹配版本的完整资源和许可，再更新依赖表。

## 驱动接口

所有 `*_init()` 在启动时由一个任务顺序调用；运行期 API 从任务调用，不可在 ISR 调用。
驱动按设备整个运行周期持有资源，不提供运行中卸载 API。LCD/触摸初始化可重复调用；
串口和每一路 485 重复初始化返回 `ESP_ERR_INVALID_STATE`。现有 `app_main()` 已初始化
这些设备；下列初始化片段用于编写独立应用时参考，不要在已运行的应用中重复初始化。

### LCD

```c
#include "lcd_st7796.h"
ESP_ERROR_CHECK(lcd_init());
ESP_ERROR_CHECK(lcd_fill(0x0000));
ESP_ERROR_CHECK(lcd_fill_rect(20, 30, 100, 60, 0xF800));
ESP_ERROR_CHECK(lcd_set_backlight(80));
// pixels 至少包含 width * height 个 uint16_t RGB565 像素。
ESP_ERROR_CHECK(lcd_draw_rgb565(x, y, width, height, pixels, pixel_count));
```

横屏左上为 `(0,0)`，有效范围 `x=0..479`、`y=0..319`，矩形参数为宽高。
RGB565 采用主机 `uint16_t` 值，红色 `0xF800`；驱动转换为高字节先发送。
同步 SPI DMA 传输，内部 7,680 字节缓冲；函数返回后调用方可立即复用像素数据，
不要求调用方缓冲在 DMA 内存中。矩形越界返回参数错误，不隐式裁剪。
并发绘图与背光调用由互斥锁串行化，不提供全屏帧缓存或撕裂同步。

### GT911

```c
#include "touch_gt911.h"
ESP_ERROR_CHECK(touch_init());
ESP_ERROR_CHECK(touch_set_enabled(false)); // 屏蔽触摸，不影响 LCD 和 benchmark。
ESP_ERROR_CHECK(touch_set_enabled(true));  // 恢复；仍按住的手指先松开再触摸。
bool enabled = touch_is_enabled();
touch_frame_t frame;
esp_err_t err = touch_read(&frame);
if (err == ESP_OK && frame.updated) {
    // count=0 表示释放；count=1..5，points[] 包含 id/x/y/size。
}
```

`updated=false` 表示没有新帧，不能当成松手。读取所有点后才清 `0x814E` 就绪位；
I2C 读取失败返回错误，不伪造触点。先按芯片报告的分辨率归一到原生 320×480，
再应用横屏变换 `(x,y) → (y,319-x)`，输出范围与 LCD 的 480×320 一致。
原生左上角映射为横屏左下角，原生右上角映射为横屏左上角。
保留厂家传感器配置，不下发未经确认的 GT911 配置表；menuconfig 的交换/镜像
用于校准原生传感器方向，在固定横屏旋转之前应用，默认均关闭。
读取接口只能有一个消费者；LVGL 活动时应用不要另外调用 `touch_read()`。
`touch_set_enabled()` 和 `touch_is_enabled()` 可由其他任务调用，与 I²C 读取共用驱动锁。
关闭后仍读取并确认 GT911 数据，输出释放状态；重新开启前丢弃挂起帧，屏蔽尚未松开的
触点，避免重放旧触摸。切换模式时 `frame.input_reset=true`，自定义 UI 应取消旧手势。
LVGL 适配在 GUI 任务中调用 `lv_indev_reset()`，避免把关触摸动作解释为一次点击。
轮询寄存器，INT 不用于中断唤醒，避免依赖厂商配置中的中断极性。
首次显示/触摸初始化统一完成共用 GPIO10 复位与地址选择，优先 7 位地址 `0x5D`，
探测失败再尝试 `0x14`。之后 LCD 仅做 ST7796 软件复位，避免重置已启动的触摸。

### GPIO 按键

```c
#include "buttons.h"
ESP_ERROR_CHECK(buttons_init());
button_event_t event;
if (buttons_get_event(&event, pdMS_TO_TICKS(100))) {
    // event.id: BUTTON_KEY2（GPIO40）；GPIO41 不产生事件。
    // event.type: BUTTON_PRESSED / BUTTON_RELEASED / BUTTON_LONG_PRESS
}
bool pressed = buttons_is_pressed(BUTTON_KEY1);
```

驱动每 5 ms 扫描。GPIO40 的按下/释放稳定 30 ms 后生效，从有效按下起计时 2000 ms
产生一次 LONG_PRESS；保持按住不会重复触发，松手仍有 RELEASED。应用消费这个长按
事件切换触摸。事件队列最多 16 项，单消费者；满时丢最旧事件并输出告警。
GPIO41 按下/释放均稳定 200 ms 后更新 `buttons_is_pressed(BUTTON_KEY1)`，不加入事件队列。

### 隔离串口

```c
#include "serial_port.h"
ESP_ERROR_CHECK(serial_port_init(115200, UART_PARITY_DISABLE, UART_STOP_BITS_1));
uint8_t rx[128];
int n = serial_port_read(rx, sizeof(rx), pdMS_TO_TICKS(100));
const uint8_t tx[] = {0x01, 0x02, 0x03};
ESP_ERROR_CHECK(serial_port_write(tx, sizeof(tx)));
```

支持 1200～115200 baud、无/奇/偶校验、1/2 停止位，8 数据位。RX 环形缓冲 2048 字节；
`read()` 为单消费者，返回实际长度、0 超时或 -1 错误。`write()` 对多任务串行化，
阻塞到发送完成，单次上限 4096 字节。应用需及时读取，持续灌入而不读取会丢数据。
CN2 隔离侧 VDDB/GNDB 需要按原理图接外部电源/参考地，不能误当 RS232 或 RS485 电平。

### 双路 Modbus RTU 主站 / 隔离 RS485

```c
#include "modbus_rtu.h"
ESP_ERROR_CHECK(rs485_init(RS485_PORT_1, 9600, UART_PARITY_DISABLE, UART_STOP_BITS_1));
ESP_ERROR_CHECK(rs485_init(RS485_PORT_2, 19200, UART_PARITY_EVEN, UART_STOP_BITS_1));
uint16_t regs[2];
uint8_t exception;
esp_err_t err = modbus_read_registers(RS485_PORT_1, 1, 0x03, 0, 2, regs, 2000, &exception);
// 在另一个任务中，以自己的 regs/exception 调用 RS485_PORT_2，即可并行通信。
// 0x04 输入寄存器同样使用 modbus_read_registers。
// 下列操作会写外部设备，按其寄存器手册明确选择地址和值后再调用：
// err = modbus_write_single(RS485_PORT_2, 1, 0x0010, 123, 2000, &exception);
// uint16_t values[] = {123, 456};
// err = modbus_write_multiple(RS485_PORT_2, 1, 0x0010, 2, values, 2000, &exception);
```

- 功能码 `03/04` 读取 1～125 个寄存器，`06` 写单寄存器，`10` 写 1～123 个寄存器。
- 从站地址 1～247，不支持地址 0 广播。寄存器采用零基址，不直接填 `40001`。
- API 第一个参数明确选择 `RS485_PORT_1` / `RS485_PORT_2`；旧版无通道参数的调用需更新。
  两路支持相同的 `03/04/06/10` 功能码，各自维护 UART、波特率、帧间隔、事件队列和事务锁。
- 同一路的请求与应答整体由锁保护，多个调用者排队；不同路的两个任务可同时通信。
  一路超时、CRC 错误或初始化失败不会清除另一路接收队列。完整两任务示例见 `main/app_main.c`。
- UART1 的 RTS 自动控制 GPIO21，UART2 的 RTS 自动控制 GPIO14：高发送、低接收，
  等待最后一个停止位完成后接收。两路初始化均先保持方向低电平。
- 发送前清理残留数据并等待总线空闲；超过 19200 baud 按 1.75 ms 基准设置帧间隔，
  低速按 3.5 字符计算，UART 空闲中断阈值向上取整到整字符。接收以空闲事件结束，
  FIFO 满事件仅作为分片；检查 CRC、地址、功能码、字节数、长度、写回显与异常响应。
- UART 奇偶/帧错误及 RX 溢出使事务失败。响应超时覆盖完整响应帧；另有锁等待和
  总线空闲等待，所以函数总耗时可能大于 `timeout_ms`。不自动重发写请求。
- `ESP_ERR_MODBUS_EXCEPTION` 时读取 `exception`；CRC、超时、帧错误分别返回
  `ESP_ERR_INVALID_CRC`、`ESP_ERR_TIMEOUT`、`ESP_ERR_INVALID_RESPONSE` 等。失败不覆盖读寄存器输出。
- 每条物理总线是基本单主站实现，不含从站寄存器服务。未做严格的 1.5 字符帧内间隔检测，
  也不能识别“前一个已超时的相同请求”的迟到应答；应用应按设备最大响应时间设超时，
  避免立即重试。不用于宣称 Modbus 时序一致性认证。

串口格式必须与各自从站匹配。默认是常见传感器的 8N1；若按标准无校验格式使用，
选择 8N2，或按设备配置使用 8E1。`BOARD_RS4851_*` / `BOARD_RS4852_*` 是独立配置项；
旧单路配置通过 `main/sdkconfig.rename` 迁移为第一路配置。
原理图两路终端为 R15/R22，偏置为 R12/R18 与 R19/R25。两路同时发送的场景是两条独立
总线；若 CN1 仅用于兼容同一设备的两种引脚定义，应只向实际连接的通道发请求。
完整 RS485 收发需 VIN 为两路总线侧 A5V 供电，USB 单供电不足以验证隔离侧通信。

## 验证与上板检查

主机协议测试直接编译 `modbus_codec.c` 为测试 DLL，再从 Python 执行，不复制实现：

```powershell
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tests\test_modbus_codec.py
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tests\test_touch_mapping.py
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tests\test_runtime.py
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tests\check_dependencies.py
```

测试用本机已安装 Visual Studio 18 MSVC 14.51 和 Windows SDK 10.0.26100；
这是主机测试编译器，固件仍完全使用本机 ESP-IDF Xtensa GCC。
路径不同可传 `--msvc-bin`、`--sdk-include`。覆盖固定 CRC/报文向量、03/04/06/10、
异常帧、坏 CRC、错误地址/功能码/回显、短帧、125/123 寄存器边界、地址溢出、
缓冲区边界和 500 组固定随机种子的有效报文。

横屏坐标测试直接编译驱动使用的 `touch_mapping.h`，覆盖四角、153,600 个原生像素
的一一映射、不同传感器分辨率、校准选项、边界与无效输入。

运行期测试编译实际 `rs485.c`、`modbus_rtu.c`、`serial_port.c`，仅将 IDF I/O 替换为主机
锁和模拟 UART 队列。覆盖 UART 分配、失败重试、不同波特率、双路同地址并发、单路事务
串行化、一路超时不阻塞另一路、两路写入和异常响应，以及按键阈值和触摸屏蔽策略。
这些测试验证软件资源与状态隔离，不证明实物 UART 时序或电气性能。

当前验证结果（2026-10-04）：本机 ESP-IDF 6.1 完整构建通过，7 组协议测试、4 组坐标
测试及 8 组运行期测试通过。应用镜像 569,648 字节，1 MB factory 分区剩余约 46%。
398 个 LVGL 原始文件有本地校验表；依赖校验不再需要旧参考目录。
只复制固件输入、没有旧 `build`/`sdkconfig` 的独立副本也已从零构建通过；其中启用了
双路只读示例，并将第二路设为 19200 baud、从站 2、起始寄存器 10。711 个编译单元的
源文件均来自该副本或本机 SDK，没有借用原项目或旧参考目录的源码。
可按 [依赖说明](DEPENDENCIES.md) 中的步骤复现；该副本的测试参数不会写回主工程。
色条、串口回显、Modbus 示例、奇/偶校验、触摸坐标变换以及关闭 GUI 的可选分支已用同一套
Xtensa GCC 单独编译检查，未改变默认固件配置；复现命令为：

```powershell
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tests\check_optional_build.py
```

产物：`build/esp32s3_basic_drivers.bin`、`build/bootloader/bootloader.bin`、
`build/partition_table/partition-table.bin`。整板首次烧录使用 `build.ps1 ... flash`，
不要把仅应用镜像直接烧到地址 0。

编译和主机测试不能替代实板检查；本机未检测到串口设备，尚未烧录，暂无实机 FPS。
连接开发板后执行上面的 `flash monitor` 命令，固件会自动开始 benchmark。建议按顺序确认：

1. USB 启动日志中的 PSRAM 为 8388608 bytes；显示/触摸初始化成功。
2. 屏幕以 480×320 横屏运行 benchmark，完成后屏幕及 USB 输出结果；日志无 flush 错误。
3. 用触摸滚动结果表验证方向与释放；需要完整五点/四角测试时关闭 benchmark，
   开启色条模式查看原始驱动日志，同时核对红绿蓝、横屏方向与背光。
4. GPIO40 短按不切换，长按 2 秒切换一次，保持按住不重复；触摸关闭时 benchmark
   继续运行。关闭前和开启时保持手指按屏，确认无旧点击，松手后恢复。重启默认开启。
   GPIO41 小于 200 ms 的跳变被滤除，稳定按下/松开可读且不触发业务动作。
5. CN2 隔离侧正确供电，通过串口适配器测试收发；回显需显式开启。
6. 两路分别连接已知 Modbus 从站，核对每路配置后开启双任务只读示例；断开一路时另一路
   仍应持续收到正确应答。再按设备手册测试允许写的寄存器，检查超时、错误波特率、
   异常码，以及 GPIO21/GPIO14 在各自收发结束后的电平。
