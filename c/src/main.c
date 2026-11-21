/* main.c — 程序入口
 *
 * 用法：
 *   EZ2.4G.exe            托盘常驻 + 面板
 *   EZ2.4G.exe --list     列出所有 HID 接口（调试）
 *   EZ2.4G.exe --once     读一次并打印
 *   EZ2.4G.exe --version  版本
 */
#include "ez24g.h"

#include <stdio.h>
#include <string.h>

static void print_reading(const EzReading *r)
{
    const WCHAR *st = L"?";
    switch (r->status) {
    case EZ_ST_CHARGING:    st = L"充电中"; break;
    case EZ_ST_DISCHARGING: st = L"放电中"; break;
    case EZ_ST_FULL:        st = L"已充满"; break;
    case EZ_ST_WIRED:       st = L"有线"; break;
    case EZ_ST_ASLEEP:      st = L"休眠"; break;
    case EZ_ST_UNPLUGGED:   st = L"未连接"; break;
    case EZ_ST_NO_PROTO:    st = L"协议未知"; break;
    default: break;
    }
    wprintf(L"%-28s %3d%%  %-8s %s%s\n", r->name,
            r->ok ? r->level : -1, st, r->source,
            r->stale ? L"（上次值）" : L"");
}

static int do_list(void)
{
    EzHidIface ifaces[256];
    int n = ez_hid_enumerate(ifaces, 256);
    int i;

    wprintf(L"共 %d 个 HID 接口\n", n);
    for (i = 0; i < n; i++) {
        wprintf(L"%04X:%04X UP=%04X U=%04X in=%u out=%u feat=%u | %s\n",
                ifaces[i].vid, ifaces[i].pid, ifaces[i].usage_page,
                ifaces[i].usage, (unsigned)ifaces[i].input_len,
                (unsigned)ifaces[i].output_len,
                (unsigned)ifaces[i].feature_len,
                ifaces[i].product[0] ? ifaces[i].product : L"(无名)");
    }
    return 0;
}

static int do_once(void)
{
    EzConfig cfg;
    EzHidIface ifaces[256];
    int n, i;

    ez_config_load(&cfg);
    n = ez_hid_enumerate(ifaces, 256);

    if (cfg.device_count == 0) {
        wprintf(L"配置里还没有设备。先用托盘面板添加设备，或手动编辑：\n");
        {
            WCHAR p[MAX_PATH];
            ez_config_path(p, MAX_PATH);
            wprintf(L"  %s\n", p);
        }
        return 1;
    }

    for (i = 0; i < cfg.device_count; i++) {
        EzReading r;
        ZeroMemory(&r, sizeof(r));
        ez_read_device(ifaces, n, &cfg.devices[i], &cfg, &r);
        print_reading(&r);
    }
    return 0;
}

int wmain(int argc, WCHAR **argv)
{
    int i;

    /* DPI 感知：动态加载，兼容旧 SDK */
    {
        typedef BOOL(WINAPI * PFN_SetCtx)(HANDLE);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            PFN_SetCtx pfn = (PFN_SetCtx)(void *)GetProcAddress(
                user32, "SetProcessDpiAwarenessContext");
            if (pfn)
                pfn((HANDLE)(INT_PTR)-4);   /* PER_MONITOR_AWARE_V2 */
        }
    }

    for (i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--list") == 0)
            return do_list();
        if (wcscmp(argv[i], L"--once") == 0)
            return do_once();
        if (wcscmp(argv[i], L"--version") == 0) {
            wprintf(L"EZ2.4G 1.1.0 (C)\n");
            return 0;
        }
        if (wcscmp(argv[i], L"--usbpcap") == 0) {
            WCHAR p[MAX_PATH];
            if (ez_usbpcap_find(p, MAX_PATH, true))
                wprintf(L"%s\n", p);
            else
                wprintf(L"未找到 USBPcapCMD.exe\n");
            return 0;
        }
    }

    ez_ui_run();
    return 0;
}
