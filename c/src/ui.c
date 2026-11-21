/* ui.c — 主面板：分层窗口 + DSH 风格卡片列表
 *
 * 用 WS_EX_LAYERED + UpdateLayeredWindow 实现真正的抗锯齿圆角：
 * 整窗 32bpp DIB 逐像素合成，圆角边缘用 SDF 覆盖率做羽化，
 * 白底上也不会出现台阶（GDI 的 Region 裁剪做不到这一点）。
 */
#include "ez24g.h"

#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_READINGS 32
#define CARD_VP_TOP 88
#define CARD_VP_BOTTOM 345
#define SCROLLBAR_W 6

typedef struct {
    EzConfig   cfg;
    EzReading  items[MAX_READINGS];
    int        item_count;
    double     scale;
    HWND       hwnd;
    HWND       shadow_hwnd;
    HBITMAP    shadow_bmp;
    int        shadow_w, shadow_h;
    HBITMAP    panel_bmp;
    int        panel_w, panel_h;
    HDC        panel_dc;
    void      *panel_bits;
    HICON      tray_icons[MAX_READINGS];
    int        tray_count;
    bool       tray_ready;
    UINT       tray_wm;
    int        scroll;
    int        dragging;
    POINT      drag_start;
    POINT      drag_win;
    bool       dark;
    bool       running;
    UINT_PTR   timer;
    double     last_poll;
} UiState;

static UiState g_ui;

/* ═══ 工具 ═══════════════════════════════════════════════════════ */
static int S(double v)
{
    return (int)(v * g_ui.scale + 0.5);
}

static double sdf_rect(double x, double y, double w, double h, double r)
{
    double cx = w * 0.5, cy = h * 0.5;
    double qx = fabs(x - cx) - (cx - r);
    double qy = fabs(y - cy) - (cy - r);
    double dx = qx > 0.0 ? qx : 0.0;
    double dy = qy > 0.0 ? qy : 0.0;
    double outside = sqrt(dx * dx + dy * dy);
    double inside = (qx > qy ? qx : qy);
    if (outside > 0.0)
        return outside;
    if (inside < 0.0)
        return inside;
    return 0.0;
}

/* 抗锯齿填充圆角矩形 */
static void fill_round_rect_px(DWORD *px, int W, int H, double x0, double y0,
                               double w, double h, double r, COLORREF rgb,
                               double opacity)
{
    int x, y;
    int iy0 = (int)floor(y0) - 1;
    int iy1 = (int)ceil(y0 + h) + 1;
    int ix0 = (int)floor(x0) - 1;
    int ix1 = (int)ceil(x0 + w) + 1;

    if (iy0 < 0) iy0 = 0;
    if (ix0 < 0) ix0 = 0;
    if (iy1 > H) iy1 = H;
    if (ix1 > W) ix1 = W;

    for (y = iy0; y < iy1; y++) {
        for (x = ix0; x < ix1; x++) {
            double d = sdf_rect((double)x + 0.5 - x0, (double)y + 0.5 - y0,
                                w, h, r);
            double cov;
            DWORD oldp, dr, dg, db, da;
            double sr, sg, sb, sa;

            if (d > 1.0)
                continue;
            cov = (d <= 0.0) ? 1.0 : (1.0 - d);
            cov *= opacity;
            if (cov <= 0.0)
                continue;

            oldp = px[y * W + x];
            da = (oldp >> 24) & 0xFF;
            dr = (oldp >> 16) & 0xFF;
            dg = (oldp >> 8) & 0xFF;
            db = oldp & 0xFF;

            sr = (double)GetRValue(rgb);
            sg = (double)GetGValue(rgb);
            sb = (double)GetBValue(rgb);
            sa = (double)da / 255.0;

            sr = sr * cov + (double)dr * (1.0 - cov);
            sg = sg * cov + (double)dg * (1.0 - cov);
            sb = sb * cov + (double)db * (1.0 - cov);
            sa = cov + sa * (1.0 - cov);

            px[y * W + x] = ((DWORD)(sa * 255.0 + 0.5) << 24) |
                            ((DWORD)(sr + 0.5) << 16) |
                            ((DWORD)(sg + 0.5) << 8) |
                            (DWORD)(sb + 0.5);
        }
    }
}

/* 粗体文本（GDI 直接画到分层位图的 DC 上）*/
static void text_at(HDC dc, const WCHAR *s, int x, int y, int size_px,
                    COLORREF color, bool bold, int flags)
{
    HFONT font, old;
    LOGFONTW lf;
    RECT r;
    int h = -size_px;

    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = h;
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, L"Microsoft YaHei UI", 31);
    font = CreateFontIndirectW(&lf);
    old = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);

    r.left = x;
    r.top = y;
    r.right = x + 4000;
    r.bottom = y + size_px * 2;
    DrawTextW(dc, s, -1, &r, flags | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(font);
}

/* ═══ 面板位图 ═══════════════════════════════════════════════════ */
static bool panel_alloc(int w, int h)
{
    BITMAPINFO bi;
    HDC ref;

    if (g_ui.panel_dc) {
        DeleteDC(g_ui.panel_dc);
        g_ui.panel_dc = NULL;
    }
    if (g_ui.panel_bmp) {
        DeleteObject(g_ui.panel_bmp);
        g_ui.panel_bmp = NULL;
    }

    ref = GetDC(NULL);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_ui.panel_bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS,
                                      &g_ui.panel_bits, NULL, 0);
    ReleaseDC(NULL, ref);
    if (!g_ui.panel_bmp || !g_ui.panel_bits)
        return false;

    g_ui.panel_w = w;
    g_ui.panel_h = h;
    g_ui.panel_dc = CreateCompatibleDC(NULL);
    if (!g_ui.panel_dc)
        return false;
    SelectObject(g_ui.panel_dc, g_ui.panel_bmp);
    return true;
}

static void status_text(const EzReading *r, WCHAR *out, int cap)
{
    switch (r->status) {
    case EZ_ST_CHARGING:    wcsncpy(out, L"充电中", cap); break;
    case EZ_ST_DISCHARGING: wcsncpy(out, L"放电中", cap); break;
    case EZ_ST_FULL:        wcsncpy(out, L"已充满", cap); break;
    case EZ_ST_WIRED:       wcsncpy(out, L"有线模式", cap); break;
    case EZ_ST_ASLEEP:      wcsncpy(out, L"设备休眠", cap); break;
    case EZ_ST_UNPLUGGED:   wcsncpy(out, L"未连接", cap); break;
    case EZ_ST_NO_PROTO:    wcsncpy(out, L"协议未知", cap); break;
    default:                wcsncpy(out, L"未知", cap); break;
    }
    out[cap - 1] = 0;
}

static COLORREF bar_color(int level, bool charging)
{
    if (charging)
        return EZ_COL_GREEN;
    if (level >= 60)
        return EZ_COL_GREEN;
    if (level >= 30)
        return EZ_COL_YELLOW;
    return EZ_COL_RED;
}

/* ── 重绘整个分层窗口 ─────────────────────────────────────────── */
static void render(void)
{
    int w = S(EZ_WIN_W);
    int h = S(EZ_WIN_H);
    DWORD *px;
    int i;
    HDC screen;
    POINT dst = { 0, 0 };
    BLENDFUNCTION bf;

    if (!g_ui.hwnd)
        return;
    if (!g_ui.panel_bmp || g_ui.panel_w != w || g_ui.panel_h != h) {
        if (!panel_alloc(w, h))
            return;
    }
    px = (DWORD *)g_ui.panel_bits;
    ZeroMemory(px, (size_t)w * h * 4);

    /* 面板底：圆角 + 2px 边框 */
    fill_round_rect_px(px, w, h, 0.0, 0.0, (double)w, (double)h,
                       (double)S(EZ_RADIUS), EZ_COL_PANEL, 1.0);
    {
        /* 边框：外圈圆角矩形挖去内圈 */
        int x, y;
        double r = (double)S(EZ_RADIUS);
        double bw = (double)S(EZ_BORDER_W);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                double d = sdf_rect((double)x + 0.5, (double)y + 0.5,
                                    (double)w, (double)h, r);
                double ad = fabs(d);
                if (ad <= bw + 1.0 && d < 1.0) {
                    double cov;
                    DWORD oldp, da, dr, dg, db;
                    double sr, sg, sb, sa;
                    if (d > 0.0)
                        cov = 1.0 - d;
                    else if (ad <= bw)
                        cov = 1.0;
                    else {
                        cov = 1.0 - (ad - bw);
                        if (cov < 0.0)
                            cov = 0.0;
                    }
                    if (cov <= 0.0)
                        continue;
                    oldp = px[y * w + x];
                    da = (oldp >> 24) & 0xFF;
                    dr = (oldp >> 16) & 0xFF;
                    dg = (oldp >> 8) & 0xFF;
                    db = oldp & 0xFF;
                    sr = (double)GetRValue(EZ_COL_BORDER);
                    sg = (double)GetGValue(EZ_COL_BORDER);
                    sb = (double)GetBValue(EZ_COL_BORDER);
                    sa = (double)da / 255.0;
                    sr = sr * cov + (double)dr * (1.0 - cov);
                    sg = sg * cov + (double)dg * (1.0 - cov);
                    sb = sb * cov + (double)db * (1.0 - cov);
                    sa = cov + sa * (1.0 - cov);
                    px[y * w + x] = ((DWORD)(sa * 255.0 + 0.5) << 24) |
                                    ((DWORD)(sr + 0.5) << 16) |
                                    ((DWORD)(sg + 0.5) << 8) |
                                    (DWORD)(sb + 0.5);
                }
            }
        }
    }

    /* 标题 */
    text_at(g_ui.panel_dc, EZ_APP_TITLE, S(24), S(22), S(19), EZ_COL_TEXT,
            true, DT_LEFT);

    /* 卡片区 */
    if (g_ui.item_count == 0) {
        RECT r;
        r.left = S(24);
        r.top = S(150);
        r.right = w - S(24);
        r.bottom = S(230);
        /* 空态卡片 */
        fill_round_rect_px(px, w, h, (double)S(24), (double)S(110),
                           (double)(w - S(48)), (double)S(110),
                           (double)S(EZ_CARD_RADIUS), EZ_COL_CARD, 1.0);
        {
            WCHAR msg[] = L"请插入设备，或拔插一下要添加的设备";
            SIZE sz;
            HFONT font, old;
            LOGFONTW lf;
            ZeroMemory(&lf, sizeof(lf));
            lf.lfHeight = -S(14);
            lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfQuality = CLEARTYPE_QUALITY;
            wcsncpy(lf.lfFaceName, L"Microsoft YaHei UI", 31);
            font = CreateFontIndirectW(&lf);
            old = (HFONT)SelectObject(g_ui.panel_dc, font);
            GetTextExtentPoint32W(g_ui.panel_dc, msg, (int)wcslen(msg), &sz);
            SelectObject(g_ui.panel_dc, old);
            DeleteObject(font);
            text_at(g_ui.panel_dc, msg,
                    (w - (int)sz.cx) / 2,
                    S(110) + (S(110) - (int)sz.cy) / 2,
                    S(14), EZ_COL_TEXT_DIM, false, DT_LEFT);
        }
        (void)r;
    } else {
        int card_h = S(EZ_CARD_H);
        int gap = S(EZ_CARD_GAP);
        int y = CARD_VP_TOP * 0 + S(78) - g_ui.scroll;

        for (i = 0; i < g_ui.item_count; i++) {
            const EzReading *r = &g_ui.items[i];
            int cw = w - S(EZ_CARD_INSET) * 2;
            int cy = y + i * (card_h + gap);
            int cl, cr;

            /* 视口裁剪 */
            if (cy + card_h < S(CARD_VP_TOP) || cy > S(CARD_VP_BOTTOM))
                continue;

            fill_round_rect_px(px, w, h, (double)S(EZ_CARD_INSET),
                               (double)cy, (double)cw, (double)card_h,
                               (double)S(EZ_CARD_RADIUS), EZ_COL_CARD, 1.0);

            /* 卡片描边 */
            {
                int bx, by;
                double r = (double)S(EZ_CARD_RADIUS);
                for (by = cy; by < cy + card_h; by++) {
                    if (by < 0 || by >= h)
                        continue;
                    for (bx = S(EZ_CARD_INSET);
                         bx < S(EZ_CARD_INSET) + cw; bx++) {
                        double d = sdf_rect((double)bx + 0.5 - S(EZ_CARD_INSET),
                                            (double)by + 0.5 - cy,
                                            (double)cw, (double)card_h, r);
                        double ad = fabs(d);
                        if (d < 0.0 && ad <= 1.0) {
                            double cov = 1.0 - ad;
                            DWORD oldp = px[by * w + bx];
                            DWORD da = (oldp >> 24) & 0xFF;
                            double sr, sg, sb, sa;
                            sr = (double)GetRValue(EZ_COL_CARD_LINE);
                            sg = (double)GetGValue(EZ_COL_CARD_LINE);
                            sb = (double)GetBValue(EZ_COL_CARD_LINE);
                            sa = (double)da / 255.0;
                            sr = sr * cov + (double)((oldp >> 16) & 0xFF) *
                                 (1.0 - cov);
                            sg = sg * cov + (double)((oldp >> 8) & 0xFF) *
                                 (1.0 - cov);
                            sb = sb * cov + (double)(oldp & 0xFF) *
                                 (1.0 - cov);
                            sa = cov + sa * (1.0 - cov);
                            px[by * w + bx] =
                                ((DWORD)(sa * 255.0 + 0.5) << 24) |
                                ((DWORD)(sr + 0.5) << 16) |
                                ((DWORD)(sg + 0.5) << 8) |
                                (DWORD)(sb + 0.5);
                        }
                    }
                }
            }

            /* 设备名 */
            text_at(g_ui.panel_dc, r->name, S(EZ_CARD_INSET) + S(20),
                    cy + S(16), S(15), EZ_COL_TEXT, true, DT_LEFT);

            /* 来源/状态副标题 */
            {
                WCHAR sub[256];
                WCHAR st[32];
                status_text(r, st, 32);
                if (r->stale)
                    _snwprintf(sub, 255, L"%s · 上次更新", st);
                else if (r->source[0])
                    _snwprintf(sub, 255, L"%s · %s", st, r->source);
                else
                    _snwprintf(sub, 255, L"%s", st);
                sub[255] = 0;
                text_at(g_ui.panel_dc, sub, S(EZ_CARD_INSET) + S(20),
                        cy + S(40), S(11), EZ_COL_TEXT_DIM, false, DT_LEFT);
            }

            /* 电量数字 或 充电中 */
            if (r->status == EZ_ST_CHARGING && r->ok) {
                text_at(g_ui.panel_dc, L"充电中",
                        S(EZ_CARD_INSET) + cw - S(90), cy + S(26), S(15),
                        EZ_COL_TEXT, true, DT_LEFT);
            } else if (r->ok && r->level >= 0) {
                WCHAR num[16];
                _snwprintf(num, 15, L"%d", r->level);
                num[15] = 0;
                text_at(g_ui.panel_dc, num,
                        S(EZ_CARD_INSET) + cw - S(88), cy + S(18), S(30),
                        EZ_COL_TEXT, true, DT_LEFT);
                text_at(g_ui.panel_dc, L"%",
                        S(EZ_CARD_INSET) + cw - S(40), cy + S(34), S(13),
                        EZ_COL_TEXT_DIM, false, DT_LEFT);
            } else {
                text_at(g_ui.panel_dc, L"--",
                        S(EZ_CARD_INSET) + cw - S(60), cy + S(24), S(22),
                        EZ_COL_TEXT_WEAK, true, DT_LEFT);
            }

            /* 进度条 */
            {
                int bar_y = cy + card_h - S(26);
                int bar_x = S(EZ_CARD_INSET) + S(20);
                int bar_w = cw - S(40);
                int bar_h = S(5);
                int lvl = (r->status == EZ_ST_CHARGING && r->ok)
                              ? 100
                              : (r->ok ? r->level : 0);

                fill_round_rect_px(px, w, h, (double)bar_x, (double)bar_y,
                                   (double)bar_w, (double)bar_h,
                                   (double)bar_h * 0.5, EZ_COL_TRACK, 1.0);
                if (lvl > 0) {
                    int fw = bar_w * lvl / 100;
                    if (fw < bar_h)
                        fw = bar_h;
                    fill_round_rect_px(px, w, h, (double)bar_x,
                                       (double)bar_y, (double)fw,
                                       (double)bar_h, (double)bar_h * 0.5,
                                       bar_color(r->level, r->status ==
                                                 EZ_ST_CHARGING), 1.0);
                }
            }

            cl = S(EZ_CARD_INSET);
            cr = w - S(EZ_CARD_INSET);
            (void)cl;
            (void)cr;
        }

        /* 滚动条（设备超过 2 个时） */
        if (g_ui.item_count > 2) {
            int total = g_ui.item_count * (card_h + gap) - gap;
            int view = S(CARD_VP_BOTTOM) - S(CARD_VP_TOP);
            int bar_x = w - S(SCROLLBAR_W) - S(8);
            int bar_h = view * view / (total > view ? total : view);
            int bar_y = S(CARD_VP_TOP);
            if (bar_h < S(24))
                bar_h = S(24);
            if (total > view)
                bar_y += (view - bar_h) * g_ui.scroll / (total - view);
            fill_round_rect_px(px, w, h, (double)bar_x,
                               (double)S(CARD_VP_TOP),
                               (double)S(SCROLLBAR_W), (double)view,
                               (double)S(SCROLLBAR_W) * 0.5,
                               RGB(0x2A, 0x2B, 0x2E), 1.0);
            fill_round_rect_px(px, w, h, (double)bar_x, (double)bar_y,
                               (double)S(SCROLLBAR_W), (double)bar_h,
                               (double)S(SCROLLBAR_W) * 0.5,
                               RGB(0x5A, 0x5D, 0x63), 1.0);
        }
    }

    /* 提交到分层窗口：psize 必须与位图尺寸一致，
       传 NULL 会沿用窗口尺寸（曾因尺寸不一致导致整窗透明）*/
    screen = GetDC(NULL);
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    {
        SIZE psz;
        psz.cx = g_ui.panel_w;
        psz.cy = g_ui.panel_h;
        if (!UpdateLayeredWindow(g_ui.hwnd, screen, NULL, &psz,
                                 g_ui.panel_dc, &dst, 0, &bf, ULW_ALPHA)) {
            ez_log(L"UpdateLayeredWindow(panel) failed err=%lu",
                   GetLastError());
        }
    }
    ReleaseDC(NULL, screen);
}

/* ═══ 数据刷新 ═══════════════════════════════════════════════════ */
static void refresh_data(void)
{
    EzHidIface ifaces[256];
    int n, i;
    int idx = 0;

    n = ez_hid_enumerate(ifaces, 256);
    g_ui.item_count = 0;

    for (i = 0; i < g_ui.cfg.device_count && idx < MAX_READINGS; i++) {
        EzReading r;
        ZeroMemory(&r, sizeof(r));
        ez_read_device(ifaces, n, &g_ui.cfg.devices[i], &g_ui.cfg, &r);
        if (r.name[0] || r.present) {
            g_ui.items[idx] = r;
            if (!g_ui.items[idx].name[0]) {
                wcsncpy(g_ui.items[idx].name, L"未知设备", 127);
                g_ui.items[idx].name[127] = 0;
            }
            idx++;
        }
    }
    g_ui.item_count = idx;
    ez_config_save(&g_ui.cfg);
    g_ui.last_poll = (double)GetTickCount64() / 1000.0;
}

/* ═══ 托盘 ═══════════════════════════════════════════════════════ */
static void tray_sync(void)
{
    NOTIFYICONDATAW nid;
    int i;

    if (!g_ui.hwnd)
        return;

    /* 删除多余图标 */
    for (i = 0; i < g_ui.tray_count; i++) {
        if (i >= g_ui.item_count) {
            ZeroMemory(&nid, sizeof(nid));
            nid.cbSize = sizeof(nid);
            nid.hWnd = g_ui.hwnd;
            nid.uID = (UINT)(1000 + i);
            Shell_NotifyIconW(NIM_DELETE, &nid);
            if (g_ui.tray_icons[i]) {
                DestroyIcon(g_ui.tray_icons[i]);
                g_ui.tray_icons[i] = NULL;
            }
        }
    }

    for (i = 0; i < g_ui.item_count && i < MAX_READINGS; i++) {
        const EzReading *r = &g_ui.items[i];
        HICON old_icon;
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_ui.hwnd;
        nid.uID = (UINT)(1000 + i);
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = EZ_WM_TRAY;

        old_icon = g_ui.tray_icons[i];
        g_ui.tray_icons[i] = ez_make_ring_icon(r->ok ? r->level : 0,
                                               r->status == EZ_ST_CHARGING,
                                               !g_ui.dark, 32);
        nid.hIcon = g_ui.tray_icons[i] ? g_ui.tray_icons[i]
                                       : LoadIconW(NULL, IDI_APPLICATION);
        wcsncpy(nid.szTip, r->name[0] ? r->name : EZ_APP_TITLE, 127);
        nid.szTip[127] = 0;
        if (!g_ui.tray_ready || i >= g_ui.tray_count)
            Shell_NotifyIconW(NIM_ADD, &nid);
        else
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        if (old_icon)
            DestroyIcon(old_icon);
    }

    if (g_ui.item_count > g_ui.tray_count)
        g_ui.tray_count = g_ui.item_count;
    g_ui.tray_ready = true;
}

/* ═══ 阴影窗口 ═══════════════════════════════════════════════════ */
static LRESULT CALLBACK shadow_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        (void)dc;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void shadow_update(int px, int py)
{
    int w = S(EZ_WIN_W);
    int h = S(EZ_WIN_H);
    int sw, sh, pad;

    if (!g_ui.shadow_hwnd)
        return;

    if (!g_ui.shadow_bmp) {
        g_ui.shadow_bmp = ez_shadow_bitmap(w, h, g_ui.scale, EZ_RADIUS, &sw,
                                           &sh);
        if (!g_ui.shadow_bmp)
            return;
        g_ui.shadow_w = sw;
        g_ui.shadow_h = sh;
    }

    pad = S(EZ_SHADOW_PAD);
    SetWindowPos(g_ui.shadow_hwnd, HWND_TOP, px - pad, py - pad,
                 g_ui.shadow_w, g_ui.shadow_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    {
        HDC screen = GetDC(NULL);
        HDC mem = CreateCompatibleDC(screen);
        HDC mem2 = CreateCompatibleDC(screen);
        HBITMAP old = (HBITMAP)SelectObject(mem, g_ui.shadow_bmp);
        BLENDFUNCTION bf;
        POINT dst = { 0, 0 };
        SIZE size = { g_ui.shadow_w, g_ui.shadow_h };
        HGDIOBJ old2;

        /* 先清空成完全透明，再贴阴影 */
        {
            BITMAPINFO bi;
            void *bits = NULL;
            ZeroMemory(&bi, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = g_ui.shadow_w;
            bi.bmiHeader.biHeight = -g_ui.shadow_h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            old2 = SelectObject(mem2,
                                CreateDIBSection(screen, &bi, DIB_RGB_COLORS,
                                                 &bits, NULL, 0));
            if (bits)
                ZeroMemory(bits, (size_t)g_ui.shadow_w * g_ui.shadow_h * 4);
        }

        /* 把阴影位图整块拷到 mem2 */
        BitBlt(mem2, 0, 0, g_ui.shadow_w, g_ui.shadow_h, mem, 0, 0,
               SRCCOPY);

        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        if (!UpdateLayeredWindow(g_ui.shadow_hwnd, screen, NULL, &size, mem2,
                                 &dst, 0, &bf, ULW_ALPHA)) {
            ez_log(L"UpdateLayeredWindow(shadow) failed err=%lu",
                   GetLastError());
        }

        if (old2) {
            HBITMAP tmp = (HBITMAP)SelectObject(mem2, old2);
            if (tmp)
                DeleteObject(tmp);
        }
        SelectObject(mem, old);
        DeleteDC(mem2);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
    }
}

/* ═══ 主窗口过程 ═════════════════════════════════════════════════ */
static void layout_shadow(void)
{
    RECT rc;
    if (!g_ui.hwnd)
        return;
    GetWindowRect(g_ui.hwnd, &rc);
    shadow_update(rc.left, rc.top);
}

static void show_panel(void)
{
    if (!g_ui.hwnd)
        return;
    refresh_data();
    render();
    ShowWindow(g_ui.hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_ui.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    layout_shadow();
    tray_sync();
}

static void hide_panel(void)
{
    if (g_ui.hwnd)
        ShowWindow(g_ui.hwnd, SW_HIDE);
    if (g_ui.shadow_hwnd)
        ShowWindow(g_ui.shadow_hwnd, SW_HIDE);
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        render();
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case EZ_WM_SHOW:
        show_panel();
        return 0;

    case EZ_WM_RENDER:
        render();
        return 0;

    case WM_TIMER:
        if (wp == 1) {
            double now = (double)GetTickCount64() / 1000.0;
            if (now - g_ui.last_poll >= (double)g_ui.cfg.interval) {
                refresh_data();
                render();
                tray_sync();
            }
        }
        return 0;

    case EZ_WM_TRAY:
        {
            UINT ev = (UINT)LOWORD(lp);
            if (ev == WM_LBUTTONUP) {
                if (IsWindowVisible(hwnd))
                    hide_panel();
                else
                    show_panel();
            } else if (ev == WM_RBUTTONUP) {
                POINT pt;
                HMENU menu;
                GetCursorPos(&pt);
                menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"刷新");
                AppendMenuW(menu, MF_STRING, 2, IsWindowVisible(hwnd)
                                                  ? L"隐藏面板"
                                                  : L"打开面板");
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(menu, MF_STRING, 3, L"退出");
                SetForegroundWindow(hwnd);
                {
                    int cmd = (int)TrackPopupMenu(
                        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                        hwnd, NULL);
                    if (cmd == 1) {
                        refresh_data();
                        render();
                        tray_sync();
                    } else if (cmd == 2) {
                        if (IsWindowVisible(hwnd))
                            hide_panel();
                        else
                            show_panel();
                    } else if (cmd == 3) {
                        DestroyWindow(hwnd);
                    }
                }
                DestroyMenu(menu);
            } else if (ev == WM_MOUSEMOVE || ev == WM_LBUTTONDOWN) {
                /* 托盘图标上滚动/按键：不处理 */
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
        {
            POINT pt;
            RECT rc;
            GetCursorPos(&pt);
            GetWindowRect(hwnd, &rc);
            g_ui.dragging = 1;
            g_ui.drag_start = pt;
            g_ui.drag_win.x = rc.left;
            g_ui.drag_win.y = rc.top;
            SetCapture(hwnd);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (g_ui.dragging) {
            POINT pt;
            GetCursorPos(&pt);
            {
                int nx = g_ui.drag_win.x + (pt.x - g_ui.drag_start.x);
                int ny = g_ui.drag_win.y + (pt.y - g_ui.drag_start.y);
                SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE);
                layout_shadow();
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_ui.dragging) {
            g_ui.dragging = 0;
            ReleaseCapture();
        }
        return 0;

    case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int card_h = S(EZ_CARD_H) + S(EZ_CARD_GAP);
            int total = g_ui.item_count * card_h - S(EZ_CARD_GAP);
            int view = S(CARD_VP_BOTTOM) - S(CARD_VP_TOP);
            int max_scroll = total > view ? total - view : 0;
            g_ui.scroll -= (delta / WHEEL_DELTA) * S(40);
            if (g_ui.scroll < 0)
                g_ui.scroll = 0;
            if (g_ui.scroll > max_scroll)
                g_ui.scroll = max_scroll;
            render();
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            hide_panel();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_ui.running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ═══ 对外接口 ═══════════════════════════════════════════════════ */
double ez_ui_scale(void)
{
    return g_ui.scale;
}

void ez_ui_set_scale(double s)
{
    g_ui.scale = s > 0.1 ? s : 1.0;
}

static void register_classes(HINSTANCE inst)
{
    WNDCLASSEXW wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"EZ24G_Panel";
    RegisterClassExW(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = shadow_proc;
    wc.hInstance = inst;
    wc.hCursor = NULL;
    wc.lpszClassName = L"EZ24G_Shadow";
    RegisterClassExW(&wc);
}

void ez_ui_run(void)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    MSG msg;
    DWORD dpi = 96;
    HANDLE mutex;

    ez_log(L"=== ez_ui_run start ===");

    mutex = CreateMutexW(NULL, TRUE, EZ_MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(L"EZ24G_Panel", NULL);
        if (prev)
            PostMessageW(prev, EZ_WM_SHOW, 0, 0);
        CloseHandle(mutex);
        return;
    }

    g_ui.scale = 1.0;
    /* GetDpiForSystem 在旧 SDK 里没有，用动态加载 */
    {
        typedef UINT(WINAPI * PFN_GetDpiForSystem)(void);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        PFN_GetDpiForSystem pfn = NULL;
        if (user32)
            pfn = (PFN_GetDpiForSystem)(void *)GetProcAddress(
                user32, "GetDpiForSystem");
        if (pfn)
            dpi = pfn();
        else {
            HDC screen = GetDC(NULL);
            dpi = (DWORD)GetDeviceCaps(screen, LOGPIXELSX);
            ReleaseDC(NULL, screen);
        }
    }
    if (dpi >= 96)
        g_ui.scale = (double)dpi / 96.0;
    g_ui.dark = true;
    g_ui.running = true;
    ez_log(L"scale=%.2f", g_ui.scale);

    ez_config_load(&g_ui.cfg);
    ez_log(L"config loaded: devices=%d protocols=%d", g_ui.cfg.device_count,
           g_ui.cfg.protocol_count);
    register_classes(inst);
    ez_round_init();

    /* 阴影窗口（点击穿透） */
    g_ui.shadow_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        L"EZ24G_Shadow", L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, inst,
        NULL);
    ez_log(L"shadow hwnd=%p", (void *)g_ui.shadow_hwnd);

    g_ui.hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"EZ24G_Panel", EZ_APP_TITLE, WS_POPUP, 0, 0, S(EZ_WIN_W),
        S(EZ_WIN_H), NULL, NULL, inst, NULL);
    ez_log(L"panel hwnd=%p", (void *)g_ui.hwnd);

    if (!g_ui.hwnd) {
        if (mutex)
            CloseHandle(mutex);
        return;
    }

    /* 屏幕居中偏右下 */
    {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        int w = S(EZ_WIN_W);
        int h = S(EZ_WIN_H);
        ez_log(L"screen=%dx%d panel=%dx%d", sw, sh, w, h);
        SetWindowPos(g_ui.hwnd, HWND_TOPMOST, (sw - w) / 2, (sh - h) / 2, w, h,
                     SWP_NOACTIVATE);
    }

    ez_log(L"refresh_data...");
    refresh_data();
    ez_log(L"items=%d", g_ui.item_count);
    ez_log(L"render...");
    render();
    ez_log(L"layout_shadow...");
    layout_shadow();
    ez_log(L"showwindow...");
    ShowWindow(g_ui.hwnd, SW_SHOWNOACTIVATE);
    ez_log(L"tray_sync...");
    tray_sync();
    ez_log(L"entering message loop");

    g_ui.timer = SetTimer(g_ui.hwnd, 1, 1000, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_ui.shadow_hwnd)
        DestroyWindow(g_ui.shadow_hwnd);
    if (g_ui.panel_bmp)
        DeleteObject(g_ui.panel_bmp);
    if (g_ui.panel_dc)
        DeleteDC(g_ui.panel_dc);
    if (g_ui.shadow_bmp)
        DeleteObject(g_ui.shadow_bmp);
    {
        int i;
        for (i = 0; i < MAX_READINGS; i++)
            if (g_ui.tray_icons[i])
                DestroyIcon(g_ui.tray_icons[i]);
    }
    if (mutex)
        CloseHandle(mutex);
}
