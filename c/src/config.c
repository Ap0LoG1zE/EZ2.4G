/* config.c — 配置读写
 *
 * 沿用 Python 版的存储位置与格式，保证两版可以互相读取：
 *   %APPDATA%\BatteryTray\config.json
 */
#include "ez24g.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include <wchar.h>

/* ── 极简 JSON 工具（够用即可，不追求完整规范）────────────────── */

/* 在 src 中查找 key（"key":），返回冒号后第一个非空白字符指针 */
static const char *json_find(const char *src, const char *key)
{
    char pat[128];
    const char *p;
    size_t n;

    if (!src || !key)
        return NULL;
    _snprintf(pat, sizeof(pat) - 1, "\"%s\"", key);
    pat[sizeof(pat) - 1] = 0;

    p = strstr(src, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    (void)n;
    return p;
}

static bool json_int(const char *src, const char *key, int *out)
{
    const char *p = json_find(src, key);
    if (!p)
        return false;
    *out = (int)strtol(p, NULL, 10);
    return true;
}

static bool json_hexpair(const char *src, UINT16 *a, UINT16 *b)
{
    unsigned x, y;
    if (!src)
        return false;
    if (sscanf(src, " %x:%x", &x, &y) != 2)
        return false;
    *a = (UINT16)x;
    *b = (UINT16)y;
    return true;
}

/* 读取字符串值到宽字符缓冲 */
static bool json_wstr(const char *src, const char *key, WCHAR *out, int cap)
{
    const char *p = json_find(src, key);
    const char *q;
    int i = 0;

    if (!p || *p != '"' || cap <= 0)
        return false;
    p++;
    q = p;
    while (*q && *q != '"') {
        if (*q == '\\' && q[1])
            q++;
        q++;
    }
    /* 逐字节转宽字符（配置里的路径基本是 ASCII + 少量中文） */
    while (p < q && i < cap - 1) {
        if (*p == '\\' && p + 1 < q) {
            char c = p[1];
            if (c == '"' || c == '\\' || c == '/') {
                out[i++] = (WCHAR)c;
                p += 2;
                continue;
            }
            if (c == 'n') { out[i++] = L'\n'; p += 2; continue; }
            if (c == 'r') { out[i++] = L'\r'; p += 2; continue; }
            if (c == 't') { out[i++] = L'\t'; p += 2; continue; }
            if (c == 'u') {
                /* \uXXXX：只处理 BMP 基本区 */
                unsigned code = 0;
                if (sscanf(p + 2, "%4x", &code) == 1 && code) {
                    out[i++] = (WCHAR)code;
                    p += 6;
                    continue;
                }
            }
        }
        {
            unsigned char uc = (unsigned char)*p;
            if (uc < 0x80) {
                out[i++] = (WCHAR)uc;
                p++;
            } else {
                /* UTF-8 多字节：交给 MultiByteToWideChar 处理剩余部分 */
                int remain = (int)(q - p);
                int wlen = MultiByteToWideChar(CP_UTF8, 0, p, remain, NULL, 0);
                if (wlen > 0 && i + wlen < cap) {
                    MultiByteToWideChar(CP_UTF8, 0, p, remain, out + i, wlen);
                    i += wlen;
                }
                p = q;
            }
        }
    }
    out[i] = 0;
    return i > 0;
}

/* ── 配置路径 ──────────────────────────────────────────────────── */
void ez_config_path(WCHAR *out, int cap)
{
    WCHAR appdata[MAX_PATH];

    if (!out || cap <= 0)
        return;
    out[0] = 0;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL,
                                   SHGFP_TYPE_CURRENT, appdata))) {
        _snwprintf(out, cap, L"%s\\BatteryTray", appdata);
        out[cap - 1] = 0;
        CreateDirectoryW(out, NULL);
        _snwprintf(out, cap, L"%s\\BatteryTray\\config.json", appdata);
        out[cap - 1] = 0;
    }
}

/* ── 读取整个文件到内存（UTF-8）────────────────────────────────── */
static char *read_file(const WCHAR *path, DWORD *out_len)
{
    HANDLE h;
    DWORD size, got = 0;
    char *buf;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > (16u << 20)) {
        CloseHandle(h);
        return NULL;
    }
    buf = (char *)malloc(size + 1);
    if (!buf) {
        CloseHandle(h);
        return NULL;
    }
    if (!ReadFile(h, buf, size, &got, NULL)) {
        free(buf);
        CloseHandle(h);
        return NULL;
    }
    buf[got] = 0;
    CloseHandle(h);
    if (out_len)
        *out_len = got;
    return buf;
}

/* ── 定位数组内某个对象的起始位置 ─────────────────────────────── */
static const char *array_next_object(const char *p)
{
    int depth = 0;
    bool in_str = false;

    if (!p)
        return NULL;
    while (*p) {
        char c = *p;
        if (in_str) {
            if (c == '\\' && p[1])
                p += 2;
            else {
                if (c == '"')
                    in_str = false;
                p++;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
            p++;
            continue;
        }
        if (c == '{') {
            if (depth == 0)
                return p;
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0)
                return p + 1;
        }
        p++;
    }
    return NULL;
}

/* ── 载入配置 ──────────────────────────────────────────────────── */
void ez_config_load(EzConfig *cfg)
{
    WCHAR path[MAX_PATH];
    char *txt;
    const char *p;
    int i;

    if (!cfg)
        return;
    ZeroMemory(cfg, sizeof(*cfg));
    cfg->interval = 60;

    ez_config_path(path, MAX_PATH);
    txt = read_file(path, NULL);
    if (!txt) {
        /* 全新安装：用默认值 */
        ez_usbpcap_find(cfg->usbpcap, MAX_PATH, false);
        return;
    }

    json_int(txt, "interval", &cfg->interval);
    if (cfg->interval < 5)
        cfg->interval = 60;

    json_wstr(txt, "usbpcap", cfg->usbpcap, MAX_PATH);

    /* devices 数组 */
    p = json_find(txt, "devices");
    if (p && *p == '[') {
        p++;
        while (cfg->device_count < EZ_MAX_DEVICES) {
            const char *obj = array_next_object(p);
            const char *end;
            if (!obj || *obj != '{')
                break;
            end = obj;
            {
                int depth = 0;
                bool in_str = false;
                while (*end) {
                    char c = *end;
                    if (in_str) {
                        if (c == '\\' && end[1]) { end += 2; continue; }
                        if (c == '"') in_str = false;
                        end++;
                        continue;
                    }
                    if (c == '"') { in_str = true; end++; continue; }
                    if (c == '{') depth++;
                    else if (c == '}') {
                        depth--;
                        if (depth == 0) { end++; break; }
                    }
                    end++;
                }
            }
            {
                char slice[2048];
                int len = (int)(end - obj);
                if (len > 0 && len < (int)sizeof(slice)) {
                    int vi = 0, pi = 0;
                    memcpy(slice, obj, len);
                    slice[len] = 0;
                    json_int(slice, "vid", &vi);
                    json_int(slice, "pid", &pi);
                    if (vi > 0 && pi > 0) {
                        EzDevice *d = &cfg->devices[cfg->device_count];
                        d->vid = (UINT16)vi;
                        d->pid = (UINT16)pi;
                        json_wstr(slice, "name", d->name, 128);
                        cfg->device_count++;
                    }
                }
            }
            p = end;
            while (*p == ' ' || *p == ',' || *p == '\r' || *p == '\n')
                p++;
            if (*p == ']')
                break;
        }
    }

    /* protocols 对象 */
    p = json_find(txt, "protocols");
    if (p && *p == '{') {
        const char *cur = p + 1;
        while (cfg->protocol_count < EZ_MAX_PROTOCOLS) {
            UINT16 vid, pid;
            const char *key_start = cur;
            const char *key_end;
            const char *obj;
            const char *end;
            int depth = 0;
            bool in_str = false;

            while (*cur == ' ' || *cur == ',' || *cur == '\r' || *cur == '\n')
                cur++;
            if (*cur == '}' || *cur != '"')
                break;
            key_start = cur + 1;
            key_end = key_start;
            while (*key_end && *key_end != '"')
                key_end++;
            if (!json_hexpair(key_start, &vid, &pid))
                break;
            cur = key_end + 1;
            while (*cur == ' ' || *cur == ':')
                cur++;
            if (*cur != '{')
                break;
            obj = cur;
            end = cur;
            in_str = false;
            while (*end) {
                char c = *end;
                if (in_str) {
                    if (c == '\\' && end[1]) { end += 2; continue; }
                    if (c == '"') in_str = false;
                    end++;
                    continue;
                }
                if (c == '"') { in_str = true; end++; continue; }
                if (c == '{') depth++;
                else if (c == '}') {
                    depth--;
                    if (depth == 0) { end++; break; }
                }
                end++;
            }
            {
                char slice[4096];
                int len = (int)(end - obj);
                if (len > 0 && len < (int)sizeof(slice)) {
                    EzProtocol *pp;
                    memcpy(slice, obj, len);
                    slice[len] = 0;
                    pp = &cfg->protocols[cfg->protocol_count].proto;
                    ZeroMemory(pp, sizeof(*pp));
                    cfg->protocols[cfg->protocol_count].vid = vid;
                    cfg->protocols[cfg->protocol_count].pid = pid;
                    pp->offset = 2;
                    {
                        const char *k = json_find(slice, "kind");
                        if (k && *k == '"' &&
                            strncmp(k + 1, "standard", 8) == 0)
                            pp->is_standard = true;
                    }
                    json_int(slice, "offset", &pp->offset);
                    {
                        const char *inv = json_find(slice, "invert");
                        if (inv && strncmp(inv, "true", 4) == 0)
                            pp->invert = true;
                    }
                    {
                        const char *rq = json_find(slice, "requests");
                        if (rq && *rq == '[') {
                            const char *r = rq + 1;
                            while (pp->req_count < 8) {
                                char item[96];
                                int n = 0;
                                while (*r == ' ' || *r == ',' ||
                                       *r == '\r' || *r == '\n')
                                    r++;
                                if (*r != '"')
                                    break;
                                r++;
                                while (*r && *r != '"' && n < 95)
                                    item[n++] = *r++;
                                item[n] = 0;
                                if (*r == '"')
                                    r++;
                                if (n > 0) {
                                    strncpy(pp->request[pp->req_count], item,
                                            95);
                                    pp->request[pp->req_count][95] = 0;
                                    pp->req_count++;
                                }
                            }
                            /* 字符串里的反斜杠转义在帧数据里没有，保持原样 */
                        }
                    }
                    cfg->protocol_count++;
                }
            }
            cur = end;
        }
    }

    /* aliases */
    p = json_find(txt, "aliases");
    if (p && *p == '{') {
        const char *cur = p + 1;
        while (cfg->alias_count < EZ_MAX_ALIAS) {
            UINT16 sv, sp, dv, dp;
            char key[32];
            int n = 0;
            const char *val;
            while (*cur == ' ' || *cur == ',' || *cur == '\r' || *cur == '\n')
                cur++;
            if (*cur != '"')
                break;
            cur++;
            while (*cur && *cur != '"' && n < 31)
                key[n++] = *cur++;
            key[n] = 0;
            if (*cur == '"')
                cur++;
            while (*cur == ' ' || *cur == ':')
                cur++;
            if (*cur != '"')
                break;
            cur++;
            val = cur;
            {
                char vbuf[32];
                int m = 0;
                while (*cur && *cur != '"' && m < 31)
                    vbuf[m++] = *cur++;
                vbuf[m] = 0;
                if (*cur == '"')
                    cur++;
                if (json_hexpair(key, &sv, &sp) &&
                    json_hexpair(vbuf, &dv, &dp)) {
                    cfg->aliases[cfg->alias_count].src_vid = sv;
                    cfg->aliases[cfg->alias_count].src_pid = sp;
                    cfg->aliases[cfg->alias_count].dst_vid = dv;
                    cfg->aliases[cfg->alias_count].dst_pid = dp;
                    cfg->alias_count++;
                }
            }
            (void)val;
        }
    }

    /* ignored */
    p = json_find(txt, "ignored");
    if (p && *p == '[') {
        const char *cur = p + 1;
        while (cfg->ignored_count < EZ_MAX_IGNORED) {
            char item[32];
            int n = 0;
            UINT16 vi, pi;
            while (*cur == ' ' || *cur == ',' || *cur == '\r' || *cur == '\n')
                cur++;
            if (*cur != '"')
                break;
            cur++;
            while (*cur && *cur != '"' && n < 31)
                item[n++] = *cur++;
            item[n] = 0;
            if (*cur == '"')
                cur++;
            if (json_hexpair(item, &vi, &pi)) {
                cfg->ignored[cfg->ignored_count].vid = vi;
                cfg->ignored[cfg->ignored_count].pid = pi;
                cfg->ignored_count++;
            }
        }
    }

    /* last */
    p = json_find(txt, "last");
    if (p && *p == '{') {
        const char *cur = p + 1;
        while (cfg->last_count < EZ_MAX_DEVICES) {
            char key[32];
            int n = 0;
            UINT16 vi, pi;
            const char *obj, *end;
            int depth = 0;
            bool in_str = false;

            while (*cur == ' ' || *cur == ',' || *cur == '\r' || *cur == '\n')
                cur++;
            if (*cur != '"')
                break;
            cur++;
            while (*cur && *cur != '"' && n < 31)
                key[n++] = *cur++;
            key[n] = 0;
            if (*cur == '"')
                cur++;
            while (*cur == ' ' || *cur == ':')
                cur++;
            if (*cur != '{')
                break;
            obj = cur;
            end = cur;
            while (*end) {
                char c = *end;
                if (in_str) {
                    if (c == '\\' && end[1]) { end += 2; continue; }
                    if (c == '"') in_str = false;
                    end++;
                    continue;
                }
                if (c == '"') { in_str = true; end++; continue; }
                if (c == '{') depth++;
                else if (c == '}') {
                    depth--;
                    if (depth == 0) { end++; break; }
                }
                end++;
            }
            if (json_hexpair(key, &vi, &pi)) {
                char slice[2048];
                int len = (int)(end - obj);
                if (len > 0 && len < (int)sizeof(slice)) {
                    int lvl = 0;
                    memcpy(slice, obj, len);
                    slice[len] = 0;
                    if (json_int(slice, "level", &lvl)) {
                        cfg->last[cfg->last_count].vid = vi;
                        cfg->last[cfg->last_count].pid = pi;
                        cfg->last[cfg->last_count].level = lvl;
                        json_wstr(slice, "name",
                                  cfg->last[cfg->last_count].name, 128);
                        json_wstr(slice, "source",
                                  cfg->last[cfg->last_count].source, 96);
                        cfg->last_count++;
                    }
                }
            }
            cur = end;
        }
    }

    free(txt);
    if (!cfg->usbpcap[0])
        ez_usbpcap_find(cfg->usbpcap, MAX_PATH, false);
    for (i = 0; i < cfg->protocol_count; i++) {
        if (cfg->protocols[i].proto.offset <= 0)
            cfg->protocols[i].proto.offset = 2;
    }
}

/* ── 保存配置 ──────────────────────────────────────────────────── */
void ez_config_save(const EzConfig *cfg)
{
    WCHAR path[MAX_PATH];
    WCHAR tmp[MAX_PATH];
    FILE *fp;
    int i, j;

    if (!cfg)
        return;
    ez_config_path(path, MAX_PATH);
    if (!path[0])
        return;
    _snwprintf(tmp, MAX_PATH, L"%s.tmp", path);
    tmp[MAX_PATH - 1] = 0;

    fp = _wfopen(tmp, L"w, ccs=UTF-8");
    if (!fp)
        return;

    fwprintf(fp, L"{\n");
    fwprintf(fp, L"  \"interval\": %d,\n", cfg->interval);
    fwprintf(fp, L"  \"usbpcap\": \"%s\",\n", cfg->usbpcap);

    /* devices */
    fwprintf(fp, L"  \"devices\": [");
    for (i = 0; i < cfg->device_count; i++) {
        fwprintf(fp, L"%s\n    {\"vid\": %u, \"pid\": %u, \"name\": \"%s\"}",
                 i ? L"," : L"", (unsigned)cfg->devices[i].vid,
                 (unsigned)cfg->devices[i].pid, cfg->devices[i].name);
    }
    fwprintf(fp, L"%s],\n", cfg->device_count ? L"\n  " : L"");

    /* protocols */
    fwprintf(fp, L"  \"protocols\": {");
    for (i = 0; i < cfg->protocol_count; i++) {
        const EzProtocol *p = &cfg->protocols[i].proto;
        fwprintf(fp, L"%s\n    \"%04X:%04X\": {\n", i ? L"," : L"",
                 (unsigned)cfg->protocols[i].vid,
                 (unsigned)cfg->protocols[i].pid);
        if (p->is_standard)
            fwprintf(fp, L"      \"kind\": \"standard\"\n");
        else {
            fwprintf(fp, L"      \"kind\": \"captured\",\n");
            fwprintf(fp, L"      \"invert\": %s,\n",
                     p->invert ? L"true" : L"false");
            fwprintf(fp, L"      \"offset\": %d,\n", p->offset);
            fwprintf(fp, L"      \"requests\": [");
            for (j = 0; j < p->req_count; j++) {
                char req[96];
                int k;
                for (k = 0; k < 95 && p->request[j][k]; k++)
                    req[k] = p->request[j][k];
                req[k] = 0;
                fwprintf(fp, L"%s\n        \"%S\"", j ? L"," : L"", req);
            }
            fwprintf(fp, L"\n      ]\n");
        }
        fwprintf(fp, L"    }");
    }
    fwprintf(fp, L"%s},\n", cfg->protocol_count ? L"\n  " : L"");

    /* aliases */
    fwprintf(fp, L"  \"aliases\": {");
    for (i = 0; i < cfg->alias_count; i++) {
        fwprintf(fp, L"%s\n    \"%04X:%04X\": \"%04X:%04X\"",
                 i ? L"," : L"", (unsigned)cfg->aliases[i].src_vid,
                 (unsigned)cfg->aliases[i].src_pid,
                 (unsigned)cfg->aliases[i].dst_vid,
                 (unsigned)cfg->aliases[i].dst_pid);
    }
    fwprintf(fp, L"%s},\n", cfg->alias_count ? L"\n  " : L"");

    /* ignored */
    fwprintf(fp, L"  \"ignored\": [");
    for (i = 0; i < cfg->ignored_count; i++) {
        fwprintf(fp, L"%s\"%04X:%04X\"", i ? L", " : L"",
                 (unsigned)cfg->ignored[i].vid,
                 (unsigned)cfg->ignored[i].pid);
    }
    fwprintf(fp, L"],\n");

    /* last */
    fwprintf(fp, L"  \"last\": {");
    for (i = 0; i < cfg->last_count; i++) {
        fwprintf(fp,
                 L"%s\n    \"%04X:%04X\": {\"level\": %d, \"name\": \"%s\", "
                 L"\"source\": \"%s\"}",
                 i ? L"," : L"", (unsigned)cfg->last[i].vid,
                 (unsigned)cfg->last[i].pid, cfg->last[i].level,
                 cfg->last[i].name, cfg->last[i].source);
    }
    fwprintf(fp, L"%s}\n", cfg->last_count ? L"\n  " : L"");
    fwprintf(fp, L"}\n");
    fclose(fp);

    DeleteFileW(path);
    MoveFileW(tmp, path);
}

/* ── 协议查询 ──────────────────────────────────────────────────── */
EzProtocol *ez_config_protocol(EzConfig *cfg, UINT16 vid, UINT16 pid)
{
    int i;
    if (!cfg)
        return NULL;
    for (i = 0; i < cfg->protocol_count; i++)
        if (cfg->protocols[i].vid == vid && cfg->protocols[i].pid == pid)
            return &cfg->protocols[i].proto;
    return NULL;
}

bool ez_config_has_protocol(EzConfig *cfg, UINT16 vid, UINT16 pid)
{
    return ez_config_protocol(cfg, vid, pid) != NULL;
}

void ez_config_set_protocol(EzConfig *cfg, UINT16 vid, UINT16 pid,
                            const EzProtocol *p)
{
    EzProtocol *dst;
    if (!cfg || !p)
        return;
    dst = ez_config_protocol(cfg, vid, pid);
    if (!dst) {
        if (cfg->protocol_count >= EZ_MAX_PROTOCOLS)
            return;
        cfg->protocols[cfg->protocol_count].vid = vid;
        cfg->protocols[cfg->protocol_count].pid = pid;
        dst = &cfg->protocols[cfg->protocol_count].proto;
        cfg->protocol_count++;
    }
    *dst = *p;
}

void ez_config_forget_last(EzConfig *cfg, UINT16 vid, UINT16 pid)
{
    int i;
    if (!cfg)
        return;
    for (i = 0; i < cfg->last_count; i++) {
        if (cfg->last[i].vid == vid && cfg->last[i].pid == pid) {
            int j;
            for (j = i; j < cfg->last_count - 1; j++)
                cfg->last[j] = cfg->last[j + 1];
            cfg->last_count--;
            return;
        }
    }
}

const WCHAR *ez_config_alias_for(EzConfig *cfg, UINT16 vid, UINT16 pid,
                                 UINT16 *out_vid, UINT16 *out_pid)
{
    int i;
    if (!cfg)
        return NULL;
    for (i = 0; i < cfg->alias_count; i++) {
        if (cfg->aliases[i].dst_vid == vid && cfg->aliases[i].dst_pid == pid) {
            if (out_vid)
                *out_vid = cfg->aliases[i].src_vid;
            if (out_pid)
                *out_pid = cfg->aliases[i].src_pid;
            return L"alias";
        }
    }
    return NULL;
}
