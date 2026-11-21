/* log.c — 简易日志，写 %TEMP%\EZ2.4G.log */
#include "ez24g.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static WCHAR g_log_path[MAX_PATH];
static bool  g_log_init = false;
static CRITICAL_SECTION g_log_cs;
static bool  g_log_cs_init = false;

static void log_init(void)
{
    WCHAR tmp[MAX_PATH];
    DWORD n;

    if (g_log_init)
        return;
    if (!g_log_cs_init) {
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_init = true;
    }
    n = GetTempPathW(MAX_PATH, tmp);
    if (n > 0 && n < MAX_PATH - 32)
        _snwprintf(g_log_path, MAX_PATH, L"%sEZ2.4G.log", tmp);
    else
        wcsncpy(g_log_path, L"EZ2.4G.log", MAX_PATH - 1);
    g_log_path[MAX_PATH - 1] = 0;
    g_log_init = true;
}

void ez_log(const WCHAR *fmt, ...)
{
    WCHAR line[2048];
    va_list ap;
    FILE *fp;
    SYSTEMTIME st;

    log_init();
    va_start(ap, fmt);
    _vsnwprintf(line, 2047, fmt, ap);
    va_end(ap);
    line[2047] = 0;

    EnterCriticalSection(&g_log_cs);
    fp = _wfopen(g_log_path, L"a, ccs=UTF-8");
    if (fp) {
        GetLocalTime(&st);
        fwprintf(fp, L"[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute,
                 st.wSecond, line);
        fclose(fp);
    }
    LeaveCriticalSection(&g_log_cs);
}
