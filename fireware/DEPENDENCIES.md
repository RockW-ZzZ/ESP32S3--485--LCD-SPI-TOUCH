# 工程依赖与源码来源

本工程的全部项目源码、字体、图片、Kconfig、构建脚本及测试均在 `fireware` 内。
旧参考目录 `ref-code` 仅是历史移植来源，删除它不会影响编译或运行；没有符号链接、
相对包含或构建步骤指向该目录。`hardware` 是设计资料，也不是构建输入。

## 项目内依赖

| 内容 | 本地位置 | 说明 |
| --- | --- | --- |
| 应用与任务 | [main](main) | LVGL benchmark、双路 Modbus 示例、按键及串口处理 |
| 本板驱动 | [board_drivers](components/board_drivers) | ST7796、GT911、GPIO、UART0、双 UART RS485 / Modbus |
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

```powershell
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tests\check_dependencies.py
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' tests\check_dependencies.py --prepare-standalone
```

第二条命令在 `tests/build/standalone_<随机编号>` 创建只含固件输入的副本，不拷贝既有
`build` 或 `sdkconfig`，也不修改、移动、删除原来的参考目录。在输出目录执行
`.\build.ps1 build` 进行从零构建；该副本另外开启双路只读 Modbus 示例，第二路使用
19200 baud、从站 2、起始寄存器 10，以检查独立配置和两任务的编译链接。
这些是验证副本的参数，不会改变主工程默认配置，也没有向实物发送请求。

2026-10-04 验证结果：主工程与上述独立副本均使用本机 ESP-IDF 6.1 构建通过。
独立副本的 711 个编译单元源文件均位于副本或本机 SDK 中；两路示例的配置值已核对。
主工程应用为 569,648 字节，开启双路示例的验证副本为 576,464 字节。
