/* usbpcap.c — USBPcapCMD.exe 定位
 *
 * 不依赖配置里的绝对路径：保存的路径失效（换盘/换机/重装）后，
 * 自动按「程序目录 → 卸载注册表 → 各盘标准目录 → PATH」重新定位。
 */
#include "ez24g.h"

#include <stdio.h>
#include <string.h>

#define EZ_USBPCAP_NAME L"USBPcapCMD.exe"

static void push(WCHAR list[][MAX_PATH], int *count, int cap, const WCHAR *path)
{
    int i;
    if (!path || !path[0] || *count >= cap)
        return;
    for (i = 0; i < *count; i++)
        if (_wcsicmp(list[i], path) == 0)
            return;
    wcsncpy(list[*count], path, MAX_PATH - 1);
    list[*count][MAX_PATH - 1] = 0;
    (*count)++;
}

/* ── 注册表：从卸载信息里找回安装目录 ─────────────────────────── */
static void from_registry(WCHAR list[][MAX_PATH], int *count, int cap)
{
    static const struct {
        HKEY  hive;
        const WCHAR *path;
    } roots[] = {
        { HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
    };
    int r;

    for (r = 0; r < (int)(sizeof(roots) / sizeof(roots[0])); r++) {
        HKEY key;
        DWORD idx;

        if (RegOpenKeyExW(roots[r].hive, roots[r].path, 0, KEY_READ, &key) !=
            ERROR_SUCCESS)
            continue;

        for (idx = 0; idx < 512 && *count < cap; idx++) {
            WCHAR sub[256];
            DWORD sub_len = 256;
            HKEY sk;

            if (RegEnumKeyExW(key, idx, sub, &sub_len, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS)
                break;
            if (RegOpenKeyExW(key, sub, 0, KEY_READ, &sk) != ERROR_SUCCESS)
                continue;

            {
                WCHAR display[512];
                DWORD dl = sizeof(display);
                DWORD type = 0;
                bool is_usbpcap = false;

                if (RegQueryValueExW(sk, L"DisplayName", NULL, &type,
                                     (LPBYTE)display, &dl) == ERROR_SUCCESS) {
                    WCHAR lower[512];
                    int i;
                    for (i = 0; display[i] && i < 511; i++)
                        lower[i] = (WCHAR)towlower(display[i]);
                    lower[511] = 0;
                    if (wcsstr(lower, L"usbpcap"))
                        is_usbpcap = true;
                }
                if (is_usbpcap) {
                    static const WCHAR *fields[] = {
                        L"InstallLocation", L"DisplayIcon", L"UninstallString"
                    };
                    int f;
                    for (f = 0; f < 3; f++) {
                        WCHAR raw[MAX_PATH * 2];
                        DWORD rl = sizeof(raw);
                        DWORD t2 = 0;
                        if (RegQueryValueExW(sk, fields[f], NULL, &t2,
                                             (LPBYTE)raw, &rl) !=
                            ERROR_SUCCESS)
                            continue;
                        /* 去引号 */
                        {
                            WCHAR clean[MAX_PATH * 2];
                            WCHAR *p = raw;
                            WCHAR *dir;
                            WCHAR *q;
                            while (*p == L' ' || *p == L'"')
                                p++;
                            wcsncpy(clean, p, MAX_PATH * 2 - 1);
                            clean[MAX_PATH * 2 - 1] = 0;
                            q = wcschr(clean, L'"');
                            if (q)
                                *q = 0;
                            /* 判断是目录还是文件 */
                            if (GetFileAttributesW(clean) !=
                                INVALID_FILE_ATTRIBUTES) {
                                DWORD attr = GetFileAttributesW(clean);
                                if (attr & FILE_ATTRIBUTE_DIRECTORY)
                                    dir = clean;
                                else {
                                    static WCHAR buf2[MAX_PATH * 2];
                                    wcsncpy(buf2, clean, MAX_PATH * 2 - 1);
                                    buf2[MAX_PATH * 2 - 1] = 0;
                                    q = wcsrchr(buf2, L'\\');
                                    if (q)
                                        *q = 0;
                                    dir = buf2;
                                }
                                {
                                    WCHAR full[MAX_PATH];
                                    _snwprintf(full, MAX_PATH, L"%s\\%s", dir,
                                               EZ_USBPCAP_NAME);
                                    full[MAX_PATH - 1] = 0;
                                    push(list, count, cap, full);
                                }
                            }
                        }
                    }
                }
            }
            RegCloseKey(sk);
        }
        RegCloseKey(key);
    }
}

/* ── 各盘标准目录 ─────────────────────────────────────────────── */
static void from_drives(WCHAR list[][MAX_PATH], int *count, int cap)
{
    WCHAR vol[8];
    WCHAR drive;
    static const WCHAR *envs[] = {
        L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"
    };
    int i;

    for (i = 0; i < 3 && *count < cap; i++) {
        WCHAR base[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(envs[i], base, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            WCHAR full[MAX_PATH];
            _snwprintf(full, MAX_PATH, L"%s\\USBPcap\\%s", base,
                       EZ_USBPCAP_NAME);
            full[MAX_PATH - 1] = 0;
            push(list, count, cap, full);
        }
    }

    for (drive = L'C'; drive <= L'Z' && *count < cap; drive++) {
        WCHAR root[8];
        _snwprintf(root, 8, L"%c:\\", drive);
        root[7] = 0;
        if (GetDriveTypeW(root) < DRIVE_REMOVABLE)
            continue;
        (void)vol;
        {
            WCHAR full[MAX_PATH];
            _snwprintf(full, MAX_PATH, L"%c:\\Program Files\\USBPcap\\%s",
                       drive, EZ_USBPCAP_NAME);
            full[MAX_PATH - 1] = 0;
            push(list, count, cap, full);

            _snwprintf(full, MAX_PATH,
                       L"%c:\\Program Files (x86)\\USBPcap\\%s", drive,
                       EZ_USBPCAP_NAME);
            full[MAX_PATH - 1] = 0;
            push(list, count, cap, full);
        }
    }
}

/* ── PATH ─────────────────────────────────────────────────────── */
static void from_path(WCHAR list[][MAX_PATH], int *count, int cap)
{
    WCHAR path[8192];
    DWORD n = GetEnvironmentVariableW(L"PATH", path, 8192);
    WCHAR *p;
    if (n == 0 || n >= 8192)
        return;
    p = path;
    while (*p && *count < cap) {
        WCHAR *semi = wcschr(p, L';');
        WCHAR dir[MAX_PATH];
        int len = semi ? (int)(semi - p) : (int)wcslen(p);
        if (len > 0 && len < MAX_PATH) {
            wcsncpy(dir, p, len);
            dir[len] = 0;
            {
                WCHAR full[MAX_PATH];
                _snwprintf(full, MAX_PATH, L"%s\\%s", dir, EZ_USBPCAP_NAME);
                full[MAX_PATH - 1] = 0;
                push(list, count, cap, full);
            }
        }
        if (!semi)
            break;
        p = semi + 1;
    }
}

/* ── 对外接口 ─────────────────────────────────────────────────── */
bool ez_usbpcap_find(WCHAR *out, int cap, bool refresh)
{
    WCHAR list[64][MAX_PATH];
    int count = 0;
    int i;
    (void)refresh;

    if (!out || cap <= 0)
        return false;

    /* 1. 配置里保存的路径（可能已失效，仍先试）*/
    {
        EzConfig cfg;
        ez_config_load(&cfg);
        push(list, &count, 64, cfg.usbpcap);
    }

    /* 2. 程序自身目录（便携版把 exe 放一起）*/
    {
        WCHAR self[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            WCHAR *slash = wcsrchr(self, L'\\');
            if (slash) {
                WCHAR full[MAX_PATH];
                *slash = 0;
                _snwprintf(full, MAX_PATH, L"%s\\%s", self, EZ_USBPCAP_NAME);
                full[MAX_PATH - 1] = 0;
                push(list, &count, 64, full);
            }
        }
    }

    from_registry(list, &count, 64);
    from_drives(list, &count, 64);
    from_path(list, &count, 64);

    for (i = 0; i < count; i++) {
        DWORD attr = GetFileAttributesW(list[i]);
        if (attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            wcsncpy(out, list[i], cap - 1);
            out[cap - 1] = 0;
            return true;
        }
    }
    out[0] = 0;
    return false;
}
