# 工程依赖与源码来源

本工程的全部项目源码、字体、图片、Kconfig、构建脚本及测试均在 `fireware` 内。
旧参考目录 `ref-code` 仅是历史移植来源，删除它不会影响编译或运行；没有符号链接、
相对包含或构建步骤指向该目录。`hardware` 是设计资料，也不是构建输入。
充电业务与网页已从 `demo` 移植到本地组件，`demo` 同样不是构建输入。

## 项目内依赖

| 内容 | 本地位置 | 说明 |
| --- | --- | --- |
| 应用与任务 | [main](main) | LVGL benchmark、双路 Modbus 示例、按键及串口处理 |
| 本板驱动 | [board_drivers](components/board_drivers) | ST7796、GT911、GPIO、UART0、双 UART RS485 / Modbus |
| 双路充电控制与网络 | [charger](components/charger) | 原生 ESP-IDF 控制、NVS、Wi-Fi、HTTP、OTA，内嵌网页来自用户 demo 的移植 |
| mDNS 1.10.0 | [mdns](components/mdns) | Espressif 官方组件，Apache-2.0；源码、许可、校验表已内置 |
| LVGL 适配 | [board_lvgl](components/board_lvgl) | 显示刷新、触摸输入、时钟与 GUI 任务 |
| LVGL 8.4.0 核心、字体、图片与 benchmark | [lvgl](components/lvgl) | 原样内置，MIT 许可；不依赖组件管理器下载 |
| LVGL 许可与来源 | [LICENCE.txt](components/lvgl/LICENCE.txt)、[PORTING.md](components/lvgl/PORTING.md) | 保留原始许可、版本与提交信息 |
| LVGL 原始文件校验表 | [SOURCE_SHA256.json](components/lvgl/SOURCE_SHA256.json) | 398 个原始文件的 SHA-256；自编适配和 CMake 不在此表内 |
| 本机构建入口 | [build.ps1](build.ps1)、[idf_short_path.py](tools/idf_short_path.py) | 读取本机工具链环境，解决本目录中文路径问题 |
| 验证脚本 | [tests](tests) | 实际 C 协议、坐标、双路通信及按键/触摸策略测试 |

ST7796 初始化寄存器取自屏幕厂家的 SPI 示例，所需寄存器表已完整写入
`components/board_drivers/lcd_st7796.c`；运行时不读取原始示例或硬件压缩包。
GT911、UART/RS485、按键及 LVGL 板级适配均由本工程源码实现。

只包含当前启用的 LVGL core 和 benchmark；未包含其他 demo/examples，构建配置会阻止
误启用这些未内置目录。后续若新增第三方组件或资源，须将实际文件、版本与许可证一并
保存在工程内，并更新本表；不要重新引入对旧参考目录的依赖。

## 本机开发工具

- 固件编译：已安装 ESP-IDF **v6.1**，`C:\esp\v6.1\esp-idf`。
- 芯片工具链：Xtensa GCC **15.2.0**，来自本机 Espressif 安装器环境。
- Python：`C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`。
- 主机测试：已安装 MSVC 14.51 / Windows SDK 10.0.26100；只编译测试 DLL，
  不用于生成 ESP32 固件。测试不需要额外 pip 包。

SDK 和编译器继续使用本机安装，不复制到项目、不升级、不下载替代版本。
拷贝 `fireware` 到其他电脑后需安装相应工具，再调整 `build.ps1 -IdfProfile` 参数。

## 独立性验证

LVGL 校验表保留移植来源的 SHA-256；由于既有 Git 提交将 CRLF 转为 LF，校验脚本只容许
文本换行差异，其他字节仍须匹配。`.gitattributes` 固定 LVGL 文本检出为 LF，mDNS 按上游
原始字节存储；图片等二进制资源始终按原始字节校验。

```powershell
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tests\check_dependencies.py
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tests\check_dependencies.py --prepare-standalone
```

第二条命令在 `tests/build/standalone_<随机编号>` 创建只含固件输入的副本，不拷贝既有
`build` 或 `sdkconfig`，也不修改原始资料目录。在输出目录执行 `./build.ps1 build`
从零构建。副本包含充电控制、网页、mDNS 与 OTA 分区表，无 `demo`、旧参考目录或硬件资料。
可选的通用 Modbus 示例、不同奇偶校验及无 GUI 分支用 `tests/check_optional_build.py` 检查。

## 充电与 mDNS 来源

充电报文和业务、网页画布/样式移植自用户提供的
`____________copy_20260912223219.ino`，源文件 SHA-256：
`ac5d4724782fb9c4e88bc024d60ce33def6b69b3d5c998b2320fecf41d6ab6c6`。
源文件无需保留在构建目录；`components/charger` 已包含本轮所需实现与网页资源。
不引入 Arduino、LovyanGFX、WebServer、Preferences 或 Update 库。

mDNS 取自 [Espressif esp-protocols](https://github.com/espressif/esp-protocols/tree/98d79b16138b419751cce79a015f8b5b0b51bcc1/components/mdns)，
标签 `mdns-v1.10.0`，提交 `98d79b16138b419751cce79a015f8b5b0b51bcc1`。
保留组件源码、构建配置、文档与 Apache-2.0 LICENSE，不包含上游测试/示例；40 个文件
由 `components/mdns/SOURCE_SHA256.json` 校验。其 manifest 仅要求本机 IDF >= 5.0，
`dependencies.lock` 记录当前 IDF 6.1，无需下载额外组件。

测试新增使用本机 Node.js 执行内嵌网页脚本，无 npm 包；MSVC 编译真实充电 service/codec/policy
并用模拟 UART/时钟验证业务。固件始终由本机 ESP-IDF / Xtensa GCC 编译，不改装 SDK。
