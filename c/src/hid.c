/* hid.c — HID 枚举与读写（纯 Win32，SetupAPI + HidD_*） */
#include "ez24g.h"

#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>

/* TDM64 自带的 hidsdi.h 是精简版，缺下面两个声明，这里补上 */
#ifndef EZ_HID_PREPARSED_DECLARED
#define EZ_HID_PREPARSED_DECLARED
HIDAPI BOOLEAN NTAPI HidD_GetPreparsedData(HANDLE HidDeviceObject,
                                           PHIDP_PREPARSED_DATA *PreparsedData);
HIDAPI BOOLEAN NTAPI HidD_FreePreparsedData(PHIDP_PREPARSED_DATA PreparsedData);
#endif

#ifdef _MSC_VER
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#endif

/* ── 从实例路径提取 VID/PID ─────────────────────────────────────── */
static void parse_ids(const WCHAR *path, UINT16 *vid, UINT16 *pid)
{
    const WCHAR *p = path;
    *vid = 0;
    *pid = 0;
    while (*p) {
        if ((p[0] == 'v' || p[0] == 'V') &&
            (p[1] == 'i' || p[1] == 'I') &&
            (p[2] == 'd' || p[2] == 'D') &&
            (p[3] == '_' || p[3] == '&')) {
            *vid = (UINT16)((p[4] ? p[4] : '0') ? wcstoul(p + 4, NULL, 16) : 0);
        } else if ((p[0] == 'p' || p[0] == 'P') &&
                   (p[1] == 'i' || p[1] == 'I') &&
                   (p[2] == 'd' || p[2] == 'D') &&
                   (p[3] == '_' || p[3] == '&')) {
            *pid = (UINT16)wcstoul(p + 4, NULL, 16);
        }
        p++;
    }
}

/* ── 从实例路径提取 MI_xx（接口号）──────────────────────────────── */
static int parse_mi(const WCHAR *path)
{
    const WCHAR *p = path;
    while (*p) {
        if ((p[0] == 'm' || p[0] == 'M') &&
            (p[1] == 'i' || p[1] == 'I') &&
            p[2] == '_') {
            return (int)wcstoul(p + 3, NULL, 16);
        }
        p++;
    }
    return -1;
}

/* ── 枚举所有 HID 接口 ─────────────────────────────────────────── */
int ez_hid_enumerate(EzHidIface *out, int cap)
{
    GUID guid;          /* 必须可写：HidD_GetHidGuid 会写进去 */
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA did;
    DWORD idx;
    int count = 0;

    if (!out || cap <= 0)
        return 0;

    HidD_GetHidGuid(&guid);
    set = SetupDiGetClassDevsW(&guid, NULL, NULL,
                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return 0;

    did.cbSize = sizeof(did);
    for (idx = 0; SetupDiEnumDeviceInterfaces(set, NULL, &guid, idx, &did);
         idx++) {
        DWORD need = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
        HANDLE h;
        HIDD_ATTRIBUTES attr;
        HIDP_CAPS caps;
        PHIDP_PREPARSED_DATA prep = NULL;
        EzHidIface *e;

        SetupDiGetDeviceInterfaceDetailW(set, &did, NULL, 0, &need, NULL);
        if (!need)
            continue;
        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, need);
        if (!detail)
            continue;
#ifdef _WIN64
        detail->cbSize = 8;
#else
        detail->cbSize = 6;
#endif
        if (!SetupDiGetDeviceInterfaceDetailW(set, &did, detail, need, NULL,
                                              NULL)) {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }

        h = CreateFileW(detail->DevicePath, 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }

        attr.Size = sizeof(attr);
        if (!HidD_GetAttributes(h, &attr) ||
            !HidD_GetPreparsedData(h, &prep)) {
            CloseHandle(h);
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }
        if (HidP_GetCaps(prep, &caps) != HIDP_STATUS_SUCCESS) {
            HidD_FreePreparsedData(prep);
            CloseHandle(h);
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }

        e = &out[count];
        ZeroMemory(e, sizeof(*e));
        wcsncpy(e->path, detail->DevicePath, MAX_PATH - 1);

        /* 产品名：优先 Product String，失败则用实例路径 */
        e->product[0] = 0;
        if (!HidD_GetProductString(h, e->product, sizeof(e->product)))
            e->product[0] = 0;
        if (!e->product[0]) {
            SP_DEVINFO_DATA devinfo;
            devinfo.cbSize = sizeof(devinfo);
            wcsncpy(e->instance, detail->DevicePath, 255);
        } else {
            wcsncpy(e->instance, detail->DevicePath, 255);
        }

        e->vid          = attr.VendorID;
        e->pid          = attr.ProductID;
        e->usage_page   = caps.UsagePage;
        e->usage        = caps.Usage;
        e->input_len    = caps.InputReportByteLength;
        e->output_len   = caps.OutputReportByteLength;
        e->feature_len  = caps.FeatureReportByteLength;
        e->interface_no = parse_mi(detail->DevicePath);

        {
            UINT16 v = 0, p = 0;
            parse_ids(detail->DevicePath, &v, &p);
            if (!e->vid && v)
                e->vid = v;
            if (!e->pid && p)
                e->pid = p;
        }

        HidD_FreePreparsedData(prep);
        CloseHandle(h);
        HeapFree(GetProcessHeap(), 0, detail);

        count++;
        if (count >= cap)
            break;
    }

    SetupDiDestroyDeviceInfoList(set);
    return count;
}

/* ── 打开设备 ──────────────────────────────────────────────────── */
HANDLE ez_hid_open(const WCHAR *path, bool write, bool read_overlapped)
{
    DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
    DWORD flags  = read_overlapped ? FILE_FLAG_OVERLAPPED : 0;
    HANDLE h;

    if (access)
        h = CreateFileW(path, access,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, flags, NULL);
    else
        h = INVALID_HANDLE_VALUE;

    if (h == INVALID_HANDLE_VALUE && write) {
        /* 回退成只读打开 */
        h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, flags, NULL);
    }
    return h;
}

void ez_hid_close(HANDLE h)
{
    if (h && h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

/* ── 同步写 ────────────────────────────────────────────────────── */
bool ez_hid_write(HANDLE h, const BYTE *data, DWORD len)
{
    DWORD written = 0;
    if (h == INVALID_HANDLE_VALUE || !h)
        return false;
    return WriteFile(h, data, len, &written, NULL) && written == len;
}

/* ── 带超时读（overlapped）─────────────────────────────────────── */
int ez_hid_read(HANDLE h, BYTE *buf, DWORD len, int timeout_ms)
{
    OVERLAPPED ov;
    HANDLE ev;
    DWORD got = 0;
    int result = -1;

    if (h == INVALID_HANDLE_VALUE || !h)
        return -1;

    ZeroMemory(&ov, sizeof(ov));
    ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ev)
        return -1;
    ov.hEvent = ev;

    if (!ReadFile(h, buf, len, &got, &ov)) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            CloseHandle(ev);
            return -1;
        }
        if (WaitForSingleObject(ev, (DWORD)timeout_ms) == WAIT_TIMEOUT) {
            CancelIoEx(h, &ov);
            CloseHandle(ev);
            return -1;
        }
        if (!GetOverlappedResult(h, &ov, &got, FALSE)) {
            CloseHandle(ev);
            return -1;
        }
    }
    CloseHandle(ev);
    result = (int)got;
    return result;
}

bool ez_hid_get_feature(HANDLE h, BYTE *buf, DWORD len)
{
    if (h == INVALID_HANDLE_VALUE || !h)
        return false;
    return HidD_GetFeature(h, buf, len) != FALSE;
}

bool ez_hid_set_feature(HANDLE h, const BYTE *data, DWORD len)
{
    if (h == INVALID_HANDLE_VALUE || !h)
        return false;
    return HidD_SetFeature(h, (PVOID)data, len) != FALSE;
}

/* ── 清空待读队列 ──────────────────────────────────────────────── */
void ez_hid_drain(HANDLE rh)
{
    BYTE tmp[256];
    int guard = 0;
    if (rh == INVALID_HANDLE_VALUE || !rh)
        return;
    while (guard++ < 64 && ez_hid_read(rh, tmp, sizeof(tmp), 15) > 0)
        ;
}

/* ── 读取标准 HID 电量字段（Usage Page 0x06/0x0D/0x85）────────── */
bool ez_hid_usage_value(HANDLE h, int kind, const BYTE *report,
                        UINT16 want_page, UINT16 want_usage, int *out)
{
    PHIDP_PREPARSED_DATA prep = NULL;
    HIDP_VALUE_CAPS *caps = NULL;
    USHORT cap_len = 0;
    bool ok = false;
    HIDP_REPORT_TYPE rt = (kind == 0) ? HidP_Feature : HidP_Input;

    if (!h || h == INVALID_HANDLE_VALUE || !out)
        return false;
    if (!HidD_GetPreparsedData(h, &prep))
        return false;

    /* 第一次调用拿所需字节数 */
    if (HidP_GetValueCaps(rt, NULL, &cap_len, prep) !=
            HIDP_STATUS_BUFFER_TOO_SMALL ||
        cap_len == 0) {
        HidD_FreePreparsedData(prep);
        return false;
    }
    caps = (HIDP_VALUE_CAPS *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                        cap_len + sizeof(HIDP_VALUE_CAPS));
    if (!caps) {
        HidD_FreePreparsedData(prep);
        return false;
    }

    if (HidP_GetValueCaps(rt, caps, &cap_len, prep) == HIDP_STATUS_SUCCESS) {
        int total = (cap_len + sizeof(HIDP_VALUE_CAPS) - 1) /
                    sizeof(HIDP_VALUE_CAPS);
        int j;
        HidP_GetValueCaps(rt, caps, &cap_len, prep);
        for (j = 0; j < total; j++) {
            HIDP_VALUE_CAPS *c = &caps[j];
            USAGE usage = c->IsRange ? c->Range.UsageMin
                                     : c->NotRange.Usage;
            ULONG val = 0;
            if (c->UsagePage != want_page || usage != want_usage)
                continue;
            if (HidP_GetUsageValue(rt, c->UsagePage, 0, usage, &val, prep,
                                   (PCHAR)report, 0) ==
                HIDP_STATUS_SUCCESS) {
                *out = (int)val;
                ok = true;
                break;
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, caps);
    HidD_FreePreparsedData(prep);
    return ok;
}
