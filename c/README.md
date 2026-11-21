# EZ2.4G（C 版）

Windows 10/11 的 2.4G 无线外设电量托盘工具，**纯 Win32 + GDI**，零第三方依赖，
单文件约 **99 KB**（Python 版打包后约 10.3 MB）。

## 与其他实现的关系

- `EZ2.4G.python.zip` / `my_devices.py`：原始 Python 版，功能最全；
- 本目录：C 版重写，主打**体积小、启动快、内存低**，并采用分层窗口实现真正的抗锯齿圆角。

两版共用同一份配置文件 `%APPDATA%\BatteryTray\config.json`，可以混用。

## 构建

需要 MinGW-w64（gcc）+ windres。示例用的是 Dev-C++ 自带的 TDM64：

```bat
cd c
build.bat
```

产物：`build\EZ2.4G.exe`

> **注意**：TDM64 的 `windres` 在路径含空格时会失败（预处理阶段调 gcc 不带引号）。
> 遇到 `preprocessing failed` 就把工具链复制到无空格路径（如 `D:\mingw64`），
> 或直接改 `build.bat` 里的 `GCC` / `WINDRES` 变量。

## 命令行

```bat
EZ2.4G.exe              :: 托盘常驻 + 面板
EZ2.4G.exe --once       :: 读一次并打印
EZ2.4G.exe --list       :: 列出所有 HID 接口
EZ2.4G.exe --usbpcap    :: 打印定位到的 USBPcapCMD.exe
EZ2.4G.exe --version    :: 版本
```

## 功能

- 托盘常驻，左键开关面板，右键菜单（刷新 / 显示隐藏 / 退出）
- 每台设备一张卡片：设备名、状态、电量数字、进度条
- 环形电量图标，按设备数量动态增减托盘图标
- 拖拽移动面板，滚轮滚动卡片列表
- 深色圆角面板，2px 描边，分层阴影，**抗锯齿圆角**
- 只读查询，不修改设备设置

## 支持协议

| 厂商 / 方案 | 识别方式 | 电量位置 |
|---|---|---|
| 标准 HID | UsagePage 0x06/0x0D/0x85 的 Feature 值 | 标准字段 |
| ATK 协议 1 | UsagePage 0xFF02，17 字节 | `rep[6]`，0x55 校验 |
| ATK 协议 2 | UsagePage 0xFF05，64 字节 | `rep[7]` |
| MCHOSE | Vendor 接口，`55 65 01` 查询 | 回 `55 65 <level>` |
| 杰理 JL_RCSP | 64 字节 Vendor 接口，TLV | TLV type 0x01 |
| 罗技 HID++ 2.0 | VID 0x046D | 特性 0x1004 / 0x1000 |
| Endgame Gear | VID 0x3367，Feature 0xA1 | `rep[16]` |
| VAXEE / ZYGEN | VID 0x3057，Feature 0x0E | `rep[5] × 5` |
| Nordic nRF | VID 0x1915，Feature 0x05 | `rep[9]` |
| 抓包学习的协议 | 配置里的 `requests` + `offset` | 任意偏移，支持反向 |

详见 [PROTOCOLS.md](PROTOCOLS.md)。

## 配置

`%APPDATA%\BatteryTray\config.json`，与 Python 版格式一致：

```json
{
  "interval": 60,
  "usbpcap": "D:\\Program Files\\USBPcap\\USBPcapCMD.exe",
  "devices": [{ "vid": 14139, "pid": 4728, "name": "ATK mini A9 pro" }],
  "protocols": {
    "373B:1278": {
      "kind": "captured",
      "requests": ["08 04 00 ... 49"],
      "offset": 6
    }
  },
  "aliases": { "373B:128E": "373B:1278" },
  "ignored": ["373B:128E"],
  "last": { "373B:1278": { "level": 55, "name": "ATK mini A9 pro" } }
}
```

## USBPcap 路径

C 版**不依赖绝对路径**。定位顺序：

1. 配置里保存的路径
2. 程序自身目录（便携版把 exe 放一起）
3. 卸载注册表（HKLM / WOW6432Node / HKCU）
4. 各盘符的 `Program Files\USBPcap` 与 `Program Files (x86)\USBPcap`
5. `PATH`

命中后自动写回配置，旧路径失效可自愈。

## 已知限制

- 仅 Windows 10/11 x64
- 未知设备仍需用 Python 版的抓包助手学习协议（C 版暂未内置抓包 UI）
- 加密或私有握手的协议无法读取

## 许可证

MIT
