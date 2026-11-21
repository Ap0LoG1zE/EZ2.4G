# 协议参考

本文件记录 C 版 `protocol.c` 里实现的各厂商电量协议，以及依据来源。
所有查询都是**只读**的，不会写入设备配置。

## 协议来源

协议实现依据以下公开资料交叉核对，未复制任何厂商驱动二进制：

| 来源 | 用途 |
|---|---|
| [Techxey/atk-mouse-battery](https://github.com/Techxey/atk-mouse-battery) `docs/PROTOCOL.md` | ATK 2.4G 协议（U2 PLUS，实测帧） |
| [aliezzzz/mchose-battery](https://github.com/aliezzzz/mchose-battery) `src/protocols/` | MCHOSE 四族协议（A5/A7/gmouse/HUB） |
| [HifumiDaisuki01/MchoseBatteryTray](https://github.com/HifumiDaisuki01/MchoseBatteryTray) `PROTOCOL.md` | MCHOSE 新协议（report 0x4D / 0x11） |
| [DrStopor/ninjutso-battery-tray](https://github.com/DrStopor/ninjutso-battery-tray) `docs/hid-protocol.md` | Ninjutso 命令表（0x11/0x12/0x13） |
| [IgnacioBarb/zowie-battery-tray](https://github.com/IgnacioBarb/zowie-battery-tray) `src/ZowieHid.cs` | ZOWIE 电压查询与百分比插值曲线 |
| 本机实测抓包 | ATK mini A9 pro、MCHOSE V9 Turbo 的帧格式 |

## ATK

### 协议 1（17 字节，`0x55` 校验）

请求：

```
08 04 00 00 00 00 00 00 00 00 00 00 00 00 00 00 49
```

校验：`(sum(rep[0..15]) + rep[16]) & 0xFF == 0x55`

应答：

```
08 04 00 00 00 02 37 00 ... 
```

| 偏移 | 含义 |
|---|---|
| `[1]` | 0x04 = 电量响应 |
| `[6]` | **电量百分比** |
| `[7]` | 非零表示有线模式 |

对应 UsagePage `0xFF02`、输入/输出各 17 字节的接口。

### 协议 2（64 字节）

请求：`08 7D 72 02 00 01 07 01` + 补零到 64 字节。
应答要求 `rep[1] == 0x72 && rep[5] == 0x07`，**电量在 `rep[7]`**。
对应 UsagePage `0xFF05`。

## MCHOSE

### V9 Turbo（C-Media dongle）

请求（64 字节）：`55 65 01` + 补零
应答：`55 65 <level> <status> ...`

### A5 / AX5 族（Feature 65 字节）

Feature 往返，`report[3] = 0x02`，`report[4] = 0x02`，`report[6] = 0x83`。
应答 `[7]` = 充电状态，`[8]` = 电量。
适用 VID `0x2023`，PID 见 `0xF012`~`0xF026`，`MI_02`。

### A7 兼容（Feature 0x11）

请求 `[0x11] + [0x06 ^ 0xFF] + 0xFF × 19`，应答整体 XOR 0xFF 后
`payload[9]` = 电量、`payload[10]` = 充电状态。
适用 VID `0x5253:0x1021`。

### gmouse 族（G3 / A7e / G7）

写 `55 30 a5 0b 2e 01 01 01`，回 `aa 30`，电量在 `offset+8`、充电在 `offset+9`。
适用 usagePage `0xFF01`、VID `0xA8A5` / `0xA8A4` / `0x3837`。

## 杰理 JL_RCSP

帧以 `0xFE` 起，`seq ch 0x00 sub 0x00 0x00 len_lo len_hi`，
随后是 TLV，CRC16-CCITT（多项式 0x1021，初值 0xFFFF）在帧尾。
**TLV `type == 0x01` 的载荷首字节即电量**。

## 罗技 HID++ 2.0

`0x1004` 统一电池：`resp[4]` = 电量，`resp[5]` = 状态
（0/1/2 = 放电/充电/接近满，3 = 满，4 = 充电中）。
`0x1000` 电池状态：`resp[4]` = 电量，`resp[6]` = 状态。
报文 `[0]=reportID, [1]=deviceIndex, [2]=featureIndex, [3]=(func<<4)|0x01`。

## Endgame Gear

Feature `0xA1` 写 `A1 B4` + 补零到 64，读回要求 `rep[1] == 0x01`，
**电量在 `rep[16]`**。适用 VID `0x3367`。

## VAXEE / ZYGEN

Feature `0x0E` 写 `0E A5 0B 01 01` + 补零到 64，
**`rep[5]` 是 5% 步进值**（0–20），乘 5 得百分比。适用 VID `0x3057`。

## ZOWIE（DW 系列，暂未接入）

命令 `08 B4 15`（16 字节），回复 `09 HH LL 00 C8`，
电压 = `(rep[1] << 8 | rep[2]) × 10` mV。
百分比用分段线性插值：

```
[3500→0, 3680→10, 3760→25, 3890→50, 4000→75, 4200→100]
```

适用 VID `0x04A5`，接收器 PID `0x800A`。
需要 `0xFF03`（输出 16B）+ `0xFF04`（输入 16B）两个集合配合，C 版尚未实现双接口协同。

## Nordic nRF

Feature `0x05` 写 `05 15 00 00 01` + 补零到 32，**`rep[9]` 为电量**。
适用 VID `0x1915`。

## 抓包学习的协议

配置里保存为：

```json
{
  "kind": "captured",
  "requests": ["08 04 00 ... 49"],
  "offset": 6,
  "invert": false
}
```

读取时依次发送 `requests` 里的帧，收到输入报告后取 `rep[offset]`；
`invert: true` 时取 `100 - rep[offset]`。
适用于任何能通过抓包定位到「查询帧 + 电量字节」的设备。

## 安全说明

- 只发送**查询**指令，不发送任何写入/配置指令；
- 对未知设备不会盲目尝试非查询报文（部分 opcode 可能改配置或进恢复模式）；
- 抓包学习失败时不会保存半成品协议。
