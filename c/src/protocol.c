/* protocol.c — 各厂商电量协议
 *
 * 协议来源与依据写在 docs/PROTOCOLS.md。所有读取都是只读查询，
 * 不写入任何设备配置，也不依赖厂商驱动。
 */
#include "ez24g.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define EZ_ATK1_CHECK 0x55
#define EZ_ATK2_PAYLOAD_LEN 7

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void set_source(EzReading *r, const WCHAR *s)
{
    wcsncpy(r->source, s, 95);
    r->source[95] = 0;
}

static void set_detail(EzReading *r, const WCHAR *s)
{
    wcsncpy(r->detail, s, 191);
    r->detail[191] = 0;
}

/* ═══ 标准 HID 电量字段 ══════════════════════════════════════════ */
bool ez_read_standard(const EzHidIface *f, EzReading *out)
{
    HANDLE h;
    BYTE rep[512];
    static const struct { UINT16 page, usage; } cands[] = {
        { 0x0006, 0x0020 },   /* HID 电池强度 */
        { 0x000D, 0x003B },   /* 触控/笔压（部分外设复用）*/
        { 0x0085, 0x0065 },   /* 电池状态 */
    };
    int i;

    if (!f || !out || f->feature_len == 0)
        return false;

    h = ez_hid_open(f->path, false, false);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    for (i = 0; i < 3; i++) {
        int val = 0;
        ZeroMemory(rep, sizeof(rep));
        if (!ez_hid_get_feature(h, rep, f->feature_len))
            continue;
        if (ez_hid_usage_value(h, 0, rep, cands[i].page, cands[i].usage,
                               &val)) {
            out->ok = true;
            out->level = clampi(val, 0, 100);
            out->status = EZ_ST_DISCHARGING;
            set_source(out, L"标准 HID 电量字段");
            out->ts = (double)GetTickCount64() / 1000.0;
            ez_hid_close(h);
            return true;
        }
    }
    ez_hid_close(h);
    return false;
}

/* ═══ ATK（Protocol 1：17 字节，0x55 校验）══════════════════════ */
/* 帧格式：08 04 00×14 + 校验；载荷 16 字节之和 + report id 的低 8 位 = 0x55 */
bool ez_read_atk(const EzHidIface *f, int variant, EzReading *out)
{
    HANDLE hw, hr;
    BYTE req[64];
    BYTE rep[256];
    int total, want, i;

    if (!f || !out)
        return false;

    total = f->output_len ? f->output_len : (variant == 2 ? 64 : 17);
    if (variant == 2) {
        if (total < 64)
            return false;
        want = 64;
    } else {
        if (total < 17)
            return false;
        want = 17;
    }

    hw = ez_hid_open(f->path, true, false);
    if (hw == INVALID_HANDLE_VALUE)
        return false;
    hr = ez_hid_open(f->path, false, true);

    for (i = 0; i < 3; i++) {
        int n;
        ZeroMemory(req, sizeof(req));
        if (variant == 2) {
            /* ATK2：08 + 7D 72 02 00 01 07 01 + 0 填充 */
            static const BYTE pl[EZ_ATK2_PAYLOAD_LEN] =
                { 0x7D, 0x72, 0x02, 0x00, 0x01, 0x07, 0x01 };
            req[0] = 0x08;
            memcpy(req + 1, pl, EZ_ATK2_PAYLOAD_LEN);
        } else {
            /* ATK1：08 04 00×14 + 校验 */
            BYTE cmd[16];
            int sum = 0;
            ZeroMemory(cmd, sizeof(cmd));
            cmd[0] = 0x04;
            for (n = 0; n < 15; n++)
                sum += cmd[n];
            sum += 0x08;
            cmd[15] = (BYTE)((EZ_ATK1_CHECK - (sum & 0xFF)) & 0xFF);
            req[0] = 0x08;
            memcpy(req + 1, cmd, 16);
        }

        ez_hid_drain(hr);
        if (!ez_hid_write(hw, req, (DWORD)total))
            continue;

        {
            ULONGLONG deadline = GetTickCount64() + 800;
            while (GetTickCount64() < deadline) {
                ZeroMemory(rep, sizeof(rep));
                n = ez_hid_read(hr, rep, (DWORD)(want > 64 ? want : 64), 250);
                if (n <= 0)
                    continue;
                if (variant == 2) {
                    if (n > 7 && rep[1] == 0x72 && rep[5] == 0x07) {
                        out->ok = true;
                        out->level = clampi(rep[7], 0, 100);
                        out->status = EZ_ST_DISCHARGING;
                        set_source(out, L"ATK 协议 2（0xFF05/64B）");
                        out->ts = (double)GetTickCount64() / 1000.0;
                        ez_hid_close(hw);
                        ez_hid_close(hr);
                        return true;
                    }
                } else {
                    if (n >= 17 && rep[0] == 0x08 && rep[1] == 0x04) {
                        int sum2 = 0, k;
                        for (k = 0; k < 16; k++)
                            sum2 += rep[k];
                        if (((sum2 + rep[16]) & 0xFF) == EZ_ATK1_CHECK) {
                            bool wired = (n > 7 && rep[7] != 0);
                            out->ok = true;
                            out->level = clampi(rep[6], 0, 100);
                            out->status = wired ? EZ_ST_WIRED
                                                : EZ_ST_DISCHARGING;
                            set_source(out,
                                       L"ATK 协议（0xFF02/17B，0x55 校验）");
                            out->ts = (double)GetTickCount64() / 1000.0;
                            ez_hid_close(hw);
                            ez_hid_close(hr);
                            return true;
                        }
                    }
                }
            }
        }
    }

    ez_hid_close(hw);
    ez_hid_close(hr);
    return false;
}

/* ═══ MCHOSE（0x55 0x65 查询）════════════════════════════════════ */
bool ez_read_mchose(const EzHidIface *f, EzReading *out)
{
    HANDLE hw, hr;
    BYTE req[128];
    BYTE rep[256];
    int total;

    if (!f || !out)
        return false;
    total = f->output_len ? f->output_len : 64;
    if (total < 3)
        return false;
    if (total > (int)sizeof(req))
        total = (int)sizeof(req);

    hw = ez_hid_open(f->path, true, false);
    if (hw == INVALID_HANDLE_VALUE)
        return false;
    hr = ez_hid_open(f->path, false, true);

    ZeroMemory(req, sizeof(req));
    req[0] = 0x55;
    req[1] = 0x65;
    req[2] = 0x01;

    ez_hid_drain(hr);
    if (ez_hid_write(hw, req, (DWORD)total)) {
        ULONGLONG deadline = GetTickCount64() + 1200;
        while (GetTickCount64() < deadline) {
            int n;
            ZeroMemory(rep, sizeof(rep));
            n = ez_hid_read(hr, rep, 64, 250);
            if (n < 3)
                continue;
            if (rep[0] == 0x55 && rep[1] == 0x65 && rep[2] <= 100) {
                out->ok = true;
                out->level = rep[2];
                out->status = EZ_ST_DISCHARGING;
                set_source(out, L"MCHOSE 协议（0x55/0x65）");
                out->ts = (double)GetTickCount64() / 1000.0;
                ez_hid_close(hw);
                ez_hid_close(hr);
                return true;
            }
        }
    }

    ez_hid_close(hw);
    ez_hid_close(hr);
    return false;
}

/* ═══ 杰理 JL_RCSP（TLV 0x01 = 电量）════════════════════════════ */
static unsigned short crc16_ccitt(const BYTE *data, int len)
{
    unsigned short crc = 0xFFFF;
    int i, b;
    for (i = 0; i < len; i++) {
        crc ^= (unsigned short)data[i] << 8;
        for (b = 0; b < 8; b++) {
            if (crc & 0x8000)
                crc = (unsigned short)((crc << 1) ^ 0x1021);
            else
                crc = (unsigned short)(crc << 1);
        }
    }
    return crc;
}

static int build_jl_frame(BYTE *f, int total, BYTE report_id,
                          const BYTE *payload, int plen, int include_zero)
{
    int base = 1;
    int start;
    unsigned short crc;

    if (total < 16 || plen > 32)
        return 0;
    ZeroMemory(f, (size_t)total);
    f[0] = report_id;
    if (include_zero)
        base++;
    start = base;
    f[base + 0] = 0xFE;
    f[base + 1] = 0x00;
    f[base + 2] = 0x10;   /* ch */
    f[base + 3] = 0x00;
    f[base + 4] = 0x02;   /* sub */
    f[base + 5] = 0x00;
    f[base + 6] = 0x00;
    f[base + 7] = (BYTE)(plen & 0xFF);
    f[base + 8] = (BYTE)((plen >> 8) & 0xFF);
    memcpy(f + base + 9, payload, (size_t)plen);
    crc = crc16_ccitt(f + start, base + 9 + plen - start);
    f[base + 9 + plen] = (BYTE)((crc >> 8) & 0xFF);
    f[base + 10 + plen] = (BYTE)(crc & 0xFF);
    return total;
}

bool ez_read_jieli(const EzHidIface *f, EzReading *out)
{
    HANDLE hw, hr;
    BYTE req[128];
    BYTE rep[256];
    int total;
    static const BYTE payload[2] = { 0x01, 0x00 };  /* TLV: type=1 len=0 */

    if (!f || !out)
        return false;
    total = f->output_len ? f->output_len : 64;
    if (total < 17 || total > (int)sizeof(req))
        return false;

    hw = ez_hid_open(f->path, true, false);
    if (hw == INVALID_HANDLE_VALUE)
        return false;
    hr = ez_hid_open(f->path, false, true);

    {
        int rid_idx;
        for (rid_idx = 0; rid_idx < 2; rid_idx++) {
            BYTE rid = rid_idx == 0 ? 0x55 : 0x00;
            int iz;
            for (iz = 0; iz < 2; iz++) {
                if (!build_jl_frame(req, total, rid, payload, 2, iz))
                    continue;
                ez_hid_drain(hr);
                if (!ez_hid_write(hw, req, (DWORD)total))
                    continue;
                {
                    ULONGLONG deadline = GetTickCount64() + 1200;
                    while (GetTickCount64() < deadline) {
                        int n;
                        int off;
                        ZeroMemory(rep, sizeof(rep));
                        n = ez_hid_read(hr, rep, 64, 200);
                        if (n < 12)
                            continue;
                        /* 找 0xFE 起始的帧 */
                        for (off = 0; off + 12 <= n; off++) {
                            int plen, i, p;
                            if (rep[off] != 0xFE)
                                continue;
                            plen = rep[off + 7] | (rep[off + 8] << 8);
                            if (plen <= 0 || off + 9 + plen > n)
                                continue;
                            p = off + 9;
                            for (i = 0; i + 2 <= plen;) {
                                BYTE t = rep[p + i];
                                BYTE ln = rep[p + i + 1];
                                if (t == 0x01 && ln >= 1 &&
                                    i + 2 + ln <= plen) {
                                    out->ok = true;
                                    out->level =
                                        clampi(rep[p + i + 2], 0, 100);
                                    out->status = EZ_ST_DISCHARGING;
                                    set_source(out,
                                               L"杰理 JL_RCSP（TLV 0x01）");
                                    out->ts = (double)GetTickCount64() /
                                              1000.0;
                                    ez_hid_close(hw);
                                    ez_hid_close(hr);
                                    return true;
                                }
                                i += 2 + ln;
                            }
                        }
                    }
                }
            }
        }
    }

    ez_hid_close(hw);
    ez_hid_close(hr);
    return false;
}

/* ═══ 罗技 HID++ 2.0（0x1004 统一电池）══════════════════════════ */
static int hidpp_call(HANDLE hw, HANDLE hr, BYTE rid, BYTE devidx,
                      BYTE featidx, BYTE func, const BYTE *params, int plen,
                      int length, BYTE *resp, int timeout_ms, int *rn)
{
    BYTE body[64];
    ULONGLONG deadline;
    int i;

    if (length > (int)sizeof(body))
        return 0;
    ZeroMemory(body, sizeof(body));
    body[0] = rid;
    body[1] = devidx;
    body[2] = featidx;
    body[3] = (BYTE)(((func & 0x0F) << 4) | 0x01);
    if (params && plen > 0)
        memcpy(body + 4, params, (size_t)plen);

    ez_hid_drain(hr);
    if (!ez_hid_write(hw, body, (DWORD)length))
        return 0;

    deadline = GetTickCount64() + (ULONGLONG)timeout_ms;
    while (GetTickCount64() < deadline) {
        BYTE rep[64];
        int n;
        ZeroMemory(rep, sizeof(rep));
        n = ez_hid_read(hr, rep, (DWORD)length, 100);
        if (n < 5)
            continue;
        if (rep[0] != rid || rep[1] != devidx || rep[2] != featidx)
            continue;
        if (((rep[3] >> 4) & 0x0F) != func)
            continue;
        for (i = 0; i < n && i < (int)sizeof(body); i++)
            resp[i] = rep[i];
        *rn = n;
        return 1;
    }
    return 0;
}

bool ez_read_logitech(const EzHidIface *f, EzReading *out)
{
    HANDLE hw, hr;
    BYTE resp[64];
    int n = 0;
    int outlen;

    if (!f || !out)
        return false;
    if (f->vid != 0x046D || f->output_len < 7)
        return false;

    outlen = (f->output_len >= 20) ? 20 : 7;
    {
        BYTE rid = (f->output_len >= 20) ? 0x11 : 0x10;
        int devidx;
        static const struct { UINT16 fid; int kind; } feats[] = {
            { 0x1004, 0 },   /* unified battery */
            { 0x1000, 1 },   /* battery status */
            { 0x1001, 2 },   /* battery voltage */
        };

        hw = ez_hid_open(f->path, true, false);
        if (hw == INVALID_HANDLE_VALUE)
            return false;
        hr = ez_hid_open(f->path, false, true);

        for (devidx = 0; devidx < 7; devidx++) {
            BYTE di = (devidx == 0) ? 0xFF : (BYTE)devidx;
            int fi;
            for (fi = 0; fi < 3; fi++) {
                BYTE params[2];
                BYTE featidx;

                params[0] = (BYTE)(feats[fi].fid >> 8);
                params[1] = (BYTE)(feats[fi].fid & 0xFF);
                if (!hidpp_call(hw, hr, rid, di, 0x00, 0x00, params, 2,
                                outlen, resp, 300, &n))
                    continue;
                if (n > 3 && (resp[3] & 0x80))
                    continue;
                featidx = resp[4];
                if (!featidx)
                    continue;
                if (!hidpp_call(hw, hr, rid, di, featidx, 0x00, NULL, 0,
                                outlen, resp, 300, &n))
                    continue;
                if (n < 6)
                    continue;
                if (feats[fi].kind == 0) {
                    int level = resp[4];
                    int st = resp[5];
                    out->ok = true;
                    out->level = clampi(level, 0, 100);
                    out->status = (st == 1 || st == 2 || st == 4)
                                      ? EZ_ST_CHARGING
                                      : (st == 3 ? EZ_ST_FULL
                                                 : EZ_ST_DISCHARGING);
                    set_source(out, L"罗技 HID++（0x1004 统一电池）");
                    out->ts = (double)GetTickCount64() / 1000.0;
                    ez_hid_close(hw);
                    ez_hid_close(hr);
                    return true;
                }
                if (feats[fi].kind == 1 && n >= 7) {
                    int level = resp[4];
                    int st = resp[6];
                    out->ok = true;
                    out->level = clampi(level, 0, 100);
                    out->status = (st == 1 || st == 2 || st == 4)
                                      ? EZ_ST_CHARGING
                                      : (st == 3 ? EZ_ST_FULL
                                                 : EZ_ST_DISCHARGING);
                    set_source(out, L"罗技 HID++（0x1000 电池状态）");
                    out->ts = (double)GetTickCount64() / 1000.0;
                    ez_hid_close(hw);
                    ez_hid_close(hr);
                    return true;
                }
            }
        }
        ez_hid_close(hw);
        ez_hid_close(hr);
    }
    return false;
}

/* ═══ Endgame Gear（Feature 0xA1）════════════════════════════════ */
bool ez_read_endgame(const EzHidIface *f, EzReading *out)
{
    HANDLE h;
    BYTE req[64];
    BYTE rep[64];
    int i;

    if (!f || !out)
        return false;
    if (f->vid != 0x3367 || f->feature_len < 64)
        return false;

    h = ez_hid_open(f->path, true, false);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    ZeroMemory(req, sizeof(req));
    req[0] = 0xA1;
    req[1] = 0xB4;

    for (i = 0; i < 3; i++) {
        ZeroMemory(rep, sizeof(rep));
        rep[0] = 0xA1;
        if (!ez_hid_set_feature(h, req, 64))
            continue;
        Sleep(300);
        if (!ez_hid_get_feature(h, rep, 64))
            continue;
        if (rep[1] == 0x01 && rep[16] <= 100) {
            out->ok = true;
            out->level = rep[16];
            out->status = EZ_ST_DISCHARGING;
            set_source(out, L"Endgame 协议（Feature 0xA1）");
            out->ts = (double)GetTickCount64() / 1000.0;
            ez_hid_close(h);
            return true;
        }
    }
    ez_hid_close(h);
    return false;
}

/* ═══ VAXEE / ZYGEN（Feature 0x0E，5% 步进）═════════════════════ */
bool ez_read_vaxee(const EzHidIface *f, EzReading *out)
{
    HANDLE h;
    BYTE req[64];
    BYTE rep[64];
    int i;

    if (!f || !out)
        return false;
    if (f->vid != 0x3057 || f->feature_len < 64)
        return false;

    h = ez_hid_open(f->path, true, false);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    ZeroMemory(req, sizeof(req));
    req[0] = 0x0E;
    req[1] = 0xA5;
    req[2] = 0x0B;
    req[3] = 0x01;
    req[4] = 0x01;

    for (i = 0; i < 2; i++) {
        ZeroMemory(rep, sizeof(rep));
        rep[0] = 0x0E;
        if (!ez_hid_set_feature(h, req, 64))
            continue;
        Sleep(150);
        if (!ez_hid_get_feature(h, rep, 64))
            continue;
        if (rep[5] <= 20) {
            out->ok = true;
            out->level = clampi(rep[5] * 5, 0, 100);
            out->status = EZ_ST_DISCHARGING;
            set_source(out, L"VAXEE 协议（Feature 0x0E）");
            out->ts = (double)GetTickCount64() / 1000.0;
            ez_hid_close(h);
            return true;
        }
    }
    ez_hid_close(h);
    return false;
}

/* ═══ Nordic nRF 接收器（Feature 0x05）═══════════════════════════ */
bool ez_read_nordic(const EzHidIface *f, EzReading *out)
{
    HANDLE h;
    BYTE req[32];
    BYTE rep[32];
    int i;

    if (!f || !out)
        return false;
    if (f->vid != 0x1915 || f->feature_len < 32)
        return false;

    h = ez_hid_open(f->path, true, false);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    ZeroMemory(req, sizeof(req));
    req[0] = 0x05;
    req[1] = 0x15;
    req[4] = 0x01;

    for (i = 0; i < 2; i++) {
        ZeroMemory(rep, sizeof(rep));
        rep[0] = 0x05;
        if (!ez_hid_set_feature(h, req, 32))
            continue;
        Sleep(100);
        if (!ez_hid_get_feature(h, rep, 32))
            continue;
        if (rep[9] <= 100) {
            out->ok = true;
            out->level = rep[9];
            out->status = EZ_ST_DISCHARGING;
            set_source(out, L"Nordic 协议（Feature 0x05）");
            out->ts = (double)GetTickCount64() / 1000.0;
            ez_hid_close(h);
            return true;
        }
    }
    ez_hid_close(h);
    return false;
}

/* ═══ 抓包学到的协议 ═════════════════════════════════════════════ */
static int parse_hex_frame(const char *s, BYTE *out, int cap)
{
    int n = 0;
    const char *p = s;
    while (*p && n < cap) {
        unsigned v;
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (sscanf(p, "%2x", &v) != 1)
            break;
        out[n++] = (BYTE)v;
        p += 2;
        while (*p && *p != ' ')
            p++;
    }
    return n;
}

bool ez_read_captured(const EzHidIface *f, const EzProtocol *p, EzReading *out)
{
    HANDLE hw, hr;
    BYTE req[64];
    BYTE rep[256];
    int total, i;

    if (!f || !p || !out || p->req_count <= 0)
        return false;
    total = f->output_len ? f->output_len : 64;
    if (total < 8)
        return false;
    if (total > (int)sizeof(req))
        total = (int)sizeof(req);

    hw = ez_hid_open(f->path, true, false);
    if (hw == INVALID_HANDLE_VALUE)
        return false;
    hr = ez_hid_open(f->path, false, true);

    for (i = 0; i < p->req_count; i++) {
        int rlen, off;
        ZeroMemory(req, sizeof(req));
        rlen = parse_hex_frame(p->request[i], req, total);
        if (rlen <= 0)
            continue;
        off = (p->offset > 0 && p->offset < total) ? p->offset : 2;

        ez_hid_drain(hr);
        if (!ez_hid_write(hw, req, (DWORD)total))
            continue;
        {
            ULONGLONG deadline = GetTickCount64() + 1200;
            while (GetTickCount64() < deadline) {
                int n;
                ZeroMemory(rep, sizeof(rep));
                n = ez_hid_read(hr, rep, 64, 250);
                if (n <= off)
                    continue;
                {
                    int lvl = p->invert ? (100 - rep[off]) : rep[off];
                    if (lvl < 0 || lvl > 100)
                        continue;
                    out->ok = true;
                    out->level = lvl;
                    out->status = EZ_ST_DISCHARGING;
                    set_source(out, L"抓包协议");
                    {
                        WCHAR det[192];
                        _snwprintf(det, 191, L"偏移 %d = 0x%02X%s", off,
                                   rep[off],
                                   p->invert ? L"（反向）" : L"");
                        det[191] = 0;
                        set_detail(out, det);
                    }
                    out->ts = (double)GetTickCount64() / 1000.0;
                    ez_hid_close(hw);
                    ez_hid_close(hr);
                    return true;
                }
            }
        }
    }

    ez_hid_close(hw);
    ez_hid_close(hr);
    return false;
}

/* ═══ 主入口：读一台设备 ═════════════════════════════════════════ */
bool ez_read_device(const EzHidIface *ifaces, int n, const EzDevice *dev,
                    EzConfig *cfg, EzReading *out)
{
    int i;
    UINT16 vid = dev->vid, pid = dev->pid;
    UINT16 alias_vid = 0, alias_pid = 0;
    bool alias_used = false;
    int matched = 0;
    int exact[64], alias[64];
    int exact_n = 0, alias_n = 0;

    if (!ifaces || n <= 0 || !dev || !out)
        return false;

    ZeroMemory(out, sizeof(*out));
    out->vid = vid;
    out->pid = pid;
    wcsncpy(out->name, dev->name, 127);
    out->name[127] = 0;

    /* 有线别名：插线充电时优先读有线接口 */
    if (ez_config_alias_for(cfg, vid, pid, &alias_vid, &alias_pid)) {
        for (i = 0; i < n; i++) {
            if (ifaces[i].vid == alias_vid && ifaces[i].pid == alias_pid) {
                alias[alias_n++] = i;
                alias_used = true;
            }
        }
    }
    for (i = 0; i < n && exact_n < 64; i++)
        if (ifaces[i].vid == vid && ifaces[i].pid == pid)
            exact[exact_n++] = i;

    if (alias_n == 0 && exact_n == 0) {
        out->present = false;
        out->status = ez_config_has_protocol(cfg, vid, pid) ? EZ_ST_UNPLUGGED
                                                            : EZ_ST_NO_PROTO;
        set_detail(out, L"未连接");
        return false;
    }

    matched = alias_n > 0 ? alias_n : exact_n;
    {
        int *list = alias_n > 0 ? alias : exact;
        EzProtocol *proto = ez_config_protocol(cfg, vid, pid);
        bool got = false;

        /* 1. 标准 HID 电量字段 */
        for (i = 0; i < matched && !got; i++) {
            EzReading r;
            ZeroMemory(&r, sizeof(r));
            if (ez_read_standard(&ifaces[list[i]], &r) && r.ok) {
                *out = r;
                got = true;
                if (!proto) {
                    EzProtocol sp;
                    ZeroMemory(&sp, sizeof(sp));
                    sp.is_standard = true;
                    ez_config_set_protocol(cfg, vid, pid, &sp);
                    ez_config_save(cfg);
                }
                break;
            }
        }

        /* 2. 抓包保存的协议 */
        if (!got && proto && !proto->is_standard && proto->req_count > 0) {
            for (i = 0; i < matched && !got; i++) {
                EzReading r;
                ZeroMemory(&r, sizeof(r));
                if (ez_read_captured(&ifaces[list[i]], proto, &r) && r.ok) {
                    *out = r;
                    got = true;
                }
            }
        }

        /* 3. 未保存协议时，按已知厂商协议试一遍 */
        if (!got && !proto) {
            struct { int (*fn)(const EzHidIface *, EzReading *); } known[8];
            int kn = 0;
            (void)known;
            for (i = 0; i < matched && !got; i++) {
                const EzHidIface *f = &ifaces[list[i]];
                EzReading r;

                ZeroMemory(&r, sizeof(r));
                if (f->output_len >= 17 && f->usage_page == 0xFF02 &&
                    ez_read_atk(f, 1, &r) && r.ok) {
                    *out = r;
                    got = true;
                    break;
                }
                ZeroMemory(&r, sizeof(r));
                if (f->output_len >= 64 && f->usage_page == 0xFF05 &&
                    ez_read_atk(f, 2, &r) && r.ok) {
                    *out = r;
                    got = true;
                    break;
                }
                ZeroMemory(&r, sizeof(r));
                if (f->output_len >= 64 && ez_read_mchose(f, &r) && r.ok) {
                    *out = r;
                    got = true;
                    break;
                }                ZeroMemory(&r, sizeof(r));
                if (ez_read_logitech(f, &r) && r.ok) {
                    *out = r;
                    got = true;
                    break;
                }
                ZeroMemory(&r, sizeof(r));
                if (ez_read_endgame(f, &r) && r.ok) {
                    *out = r;
                    got = true;
                    break;
                }
                ZeroMemory(&r, sizeof(r));
                if (ez_read_vaxee(f, &r) && r.ok) {
                    *out = r;
                    got = true;
                    break;
                }
                ZeroMemory(&r, sizeof(r));
                if (ez_read_nordic(f, &r) && r.ok) {
                    *out = r;
                    got = true;
                    break;
                }
                ZeroMemory(&r, sizeof(r));
                if (f->output_len >= 32 && ez_read_jieli(f, &r) && r.ok) {
                    *out = r;
                    got = true;
                    break;
                }
            }
            (void)kn;
        }

        if (got) {
            wcsncpy(out->name, dev->name, 127);
            out->name[127] = 0;
            out->present = true;
            out->vid = vid;
            out->pid = pid;
            if (alias_used) {
                WCHAR tmp[96];
                _snwprintf(tmp, 95, L"有线别名 · %s", out->source);
                tmp[95] = 0;
                wcsncpy(out->source, tmp, 95);
                out->source[95] = 0;
            }
            {
                int j;
                for (j = 0; j < cfg->last_count; j++) {
                    if (cfg->last[j].vid == vid && cfg->last[j].pid == pid) {
                        cfg->last[j].level = out->level;
                        cfg->last[j].ts = out->ts;
                        wcsncpy(cfg->last[j].source, out->source, 95);
                        cfg->last[j].source[95] = 0;
                        break;
                    }
                }
                if (j >= cfg->last_count && cfg->last_count < EZ_MAX_DEVICES) {
                    cfg->last[cfg->last_count].vid = vid;
                    cfg->last[cfg->last_count].pid = pid;
                    cfg->last[cfg->last_count].level = out->level;
                    cfg->last[cfg->last_count].ts = out->ts;
                    wcsncpy(cfg->last[cfg->last_count].source, out->source,
                            95);
                    cfg->last[cfg->last_count].source[95] = 0;
                    cfg->last_count++;
                }
            }
            return true;
        }

        /* 读不到：有协议算休眠，没协议算协议未知 */
        out->present = true;
        if (proto) {
            int j;
            out->status = EZ_ST_ASLEEP;
            for (j = 0; j < cfg->last_count; j++) {
                if (cfg->last[j].vid == vid && cfg->last[j].pid == pid) {
                    out->level = cfg->last[j].level;
                    out->ok = out->level >= 0;
                    out->stale = true;
                    wcsncpy(out->source, cfg->last[j].source, 95);
                    out->source[95] = 0;
                    break;
                }
            }
            set_detail(out, L"设备休眠");
        } else {
            out->status = EZ_ST_NO_PROTO;
            set_detail(out, L"协议未知，无法读取电量");
        }
    }
    return false;
}
