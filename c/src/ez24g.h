/* ez24g.h — EZ2.4G 外设电量托盘工具（C 版）
 *
 * Windows 10/11 的 2.4G 无线外设电量托盘工具。
 * 纯 Win32 API，零第三方依赖，单个 exe。
 */
#ifndef EZ24G_H
#define EZ24G_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#define EZ_APP_NAME      L"2.4G"
#define EZ_APP_TITLE     L"2.4G 外设电量"
#define EZ_APP_ID        L"2.4GDevice.BatteryPanel"
#define EZ_MUTEX_NAME    L"Global\\EZ24G_SingleInstance"
#define EZ_WM_SHOW       (WM_APP + 1)
#define EZ_WM_TRAY       (WM_APP + 2)
#define EZ_WM_WAKE       (WM_APP + 3)
#define EZ_WM_RENDER     (WM_APP + 4)

/* ── DSH 设计令牌（对齐 DeepSeek Harness 深色桌面 UI）──────────────── */
#define EZ_COL_PANEL       RGB(0x1B, 0x1B, 0x1C)
#define EZ_COL_CARD        RGB(0x21, 0x21, 0x23)
#define EZ_COL_CARD_LINE   RGB(0x30, 0x31, 0x36)
#define EZ_COL_TEXT        RGB(0xF5, 0xF5, 0xF7)
#define EZ_COL_TEXT_DIM    RGB(0xA0, 0xA3, 0xA8)
#define EZ_COL_TEXT_WEAK   RGB(0x78, 0x7B, 0x80)
#define EZ_COL_TRACK       RGB(0x2E, 0x2F, 0x33)
#define EZ_COL_BORDER      RGB(0x3C, 0x3C, 0x3C)   /* 2px，实测 Harness 值 */
#define EZ_COL_GREEN       RGB(0x22, 0xC5, 0x5E)
#define EZ_COL_YELLOW      RGB(0xF5, 0x9E, 0x0B)
#define EZ_COL_RED         RGB(0xEF, 0x44, 0x44)

/* 几何常量（逻辑像素，使用前乘 DPI 缩放 S） */
#define EZ_WIN_W        560
#define EZ_WIN_H        432
#define EZ_BORDER_W     2
#define EZ_RADIUS       16          /* 视觉圆角半径 */
#define EZ_CARD_INSET   24
#define EZ_CARD_H       122
#define EZ_CARD_GAP     12
#define EZ_CARD_RADIUS  12
#define EZ_SHADOW_PAD   28

/* ── 读电量结果 ─────────────────────────────────────────────────── */
typedef enum {
    EZ_ST_UNKNOWN = 0,
    EZ_ST_CHARGING,
    EZ_ST_DISCHARGING,
    EZ_ST_FULL,
    EZ_ST_WIRED,
    EZ_ST_ASLEEP,
    EZ_ST_UNPLUGGED,
    EZ_ST_NO_PROTO,
} EzStatus;

typedef struct {
    bool     ok;
    bool     present;
    bool     stale;         /* 用上次成功电量 */
    int      level;         /* 0..100，ok 时有效 */
    EzStatus status;
    WCHAR    name[128];
    WCHAR    source[96];
    WCHAR    detail[192];
    UINT16   vid;
    UINT16   pid;
    UINT16   usage_page;
    double   ts;
} EzReading;

/* ── 设备（配置里保存的）─────────────────────────────────────────── */
typedef struct {
    UINT16 vid;
    UINT16 pid;
    WCHAR  name[128];
} EzDevice;

typedef struct {
    char   request[8][96];  /* 十六进制帧，最多 8 条 */
    int    req_count;
    int    offset;
    bool   invert;
    bool   is_standard;
} EzProtocol;

#define EZ_MAX_DEVICES   32
#define EZ_MAX_PROTOCOLS 64
#define EZ_MAX_ALIAS     32
#define EZ_MAX_IGNORED   32

/* ── 配置 ───────────────────────────────────────────────────────── */
typedef struct {
    int      interval;                 /* 刷新间隔秒 */
    WCHAR    usbpcap[MAX_PATH];        /* USBPcapCMD.exe */
    bool     auto_start;

    EzDevice devices[EZ_MAX_DEVICES];
    int      device_count;

    struct {
        UINT16 vid, pid;
        EzProtocol proto;
    } protocols[EZ_MAX_PROTOCOLS];
    int protocol_count;

    struct {
        UINT16 src_vid, src_pid;
        UINT16 dst_vid, dst_pid;
    } aliases[EZ_MAX_ALIAS];
    int alias_count;

    struct { UINT16 vid, pid; } ignored[EZ_MAX_IGNORED];
    int ignored_count;

    /* 上次成功读数 */
    struct {
        UINT16 vid, pid;
        int    level;
        double ts;
        WCHAR  source[96];
        WCHAR  name[128];
    } last[EZ_MAX_DEVICES];
    int last_count;
} EzConfig;

/* ── HID 接口 ───────────────────────────────────────────────────── */
typedef struct {
    WCHAR   path[MAX_PATH];
    WCHAR   product[128];
    WCHAR   instance[256];
    UINT16  vid;
    UINT16  pid;
    UINT16  usage_page;
    UINT16  usage;
    UINT16  input_len;
    UINT16  output_len;
    UINT16  feature_len;
    int     interface_no;
} EzHidIface;

/* ═══ hid.c ═══════════════════════════════════════════════════════ */
int  ez_hid_enumerate(EzHidIface *out, int cap);
HANDLE ez_hid_open(const WCHAR *path, bool write, bool read_overlapped);
void ez_hid_close(HANDLE h);
bool ez_hid_write(HANDLE h, const BYTE *data, DWORD len);
int  ez_hid_read(HANDLE h, BYTE *buf, DWORD len, int timeout_ms);
bool ez_hid_get_feature(HANDLE h, BYTE *buf, DWORD len);
bool ez_hid_set_feature(HANDLE h, const BYTE *data, DWORD len);
void ez_hid_drain(HANDLE rh);
bool ez_hid_usage_value(HANDLE h, int kind, const BYTE *report,
                        UINT16 want_page, UINT16 want_usage, int *out);

/* ═══ config.c ════════════════════════════════════════════════════ */
void        ez_config_path(WCHAR *out, int cap);
void        ez_config_load(EzConfig *cfg);
void        ez_config_save(const EzConfig *cfg);
EzProtocol *ez_config_protocol(EzConfig *cfg, UINT16 vid, UINT16 pid);
bool        ez_config_has_protocol(EzConfig *cfg, UINT16 vid, UINT16 pid);
void        ez_config_set_protocol(EzConfig *cfg, UINT16 vid, UINT16 pid,
                                   const EzProtocol *p);
void        ez_config_forget_last(EzConfig *cfg, UINT16 vid, UINT16 pid);
const WCHAR *ez_config_alias_for(EzConfig *cfg, UINT16 vid, UINT16 pid,
                                 UINT16 *out_vid, UINT16 *out_pid);

/* ═══ protocol.c ══════════════════════════════════════════════════ */
bool ez_read_device(const EzHidIface *ifaces, int n,
                    const EzDevice *dev, EzConfig *cfg, EzReading *out);
bool ez_read_standard(const EzHidIface *f, EzReading *out);
bool ez_read_atk(const EzHidIface *f, int variant, EzReading *out);
bool ez_read_mchose(const EzHidIface *f, EzReading *out);
bool ez_read_jieli(const EzHidIface *f, EzReading *out);
bool ez_read_logitech(const EzHidIface *f, EzReading *out);
bool ez_read_endgame(const EzHidIface *f, EzReading *out);
bool ez_read_vaxee(const EzHidIface *f, EzReading *out);
bool ez_read_nordic(const EzHidIface *f, EzReading *out);
bool ez_read_captured(const EzHidIface *f, const EzProtocol *p, EzReading *out);

/* ═══ usbpcap.c ═══════════════════════════════════════════════════ */
bool ez_usbpcap_find(WCHAR *out, int cap, bool refresh);

/* ═══ ui.c ════════════════════════════════════════════════════════ */
void   ez_ui_run(void);
double ez_ui_scale(void);
void   ez_ui_set_scale(double s);

/* 圆角窗口与分层阴影（对齐 Harness） */
void   ez_round_init(void);
HRGN   ez_round_region(int w, int h, double s, int radius);
HBITMAP ez_shadow_bitmap(int w, int h, double s, int radius, int *out_w,
                         int *out_h);
void   ez_border_bitmap(HBITMAP target, HDC ref_dc, int w, int h, double s,
                        int radius);

/* 托盘环形电量图标 */
HICON  ez_make_ring_icon(int level, bool charging, bool light, int px);

/* 文本与通用绘制 */
void   ez_draw_text_ui(HDC dc, const WCHAR *text, RECT *r, int size_px,
                       COLORREF color, int flags);

/* 日志（调试用，写 %TEMP%） */
void ez_log(const WCHAR *fmt, ...);

#endif /* EZ24G_H */
