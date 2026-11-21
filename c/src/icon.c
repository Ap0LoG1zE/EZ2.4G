/* icon.c — 环形电量图标（GDI 手工绘制，带抗锯齿）
 *
 * 32×32 基准画布，圆心 (16,16)，半径 12，线宽 4。
 * 电量 100% 时画整圆（起终点重合的 Arc 什么都不画）。
 */
#include "ez24g.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    int w, h;
    DWORD *px;
    HBITMAP bmp;
    HDC dc;
    void *bits;
} Canvas;

static bool canvas_new(Canvas *c, int w, int h)
{
    BITMAPINFO bi;
    HDC ref;

    ZeroMemory(c, sizeof(*c));
    c->w = w;
    c->h = h;
    ref = GetDC(NULL);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    c->bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &c->bits, NULL, 0);
    ReleaseDC(NULL, ref);
    if (!c->bmp || !c->bits)
        return false;
    c->px = (DWORD *)c->bits;
    ZeroMemory(c->bits, (size_t)w * h * 4);
    c->dc = CreateCompatibleDC(NULL);
    if (c->dc)
        SelectObject(c->dc, c->bmp);
    return true;
}

static void canvas_free(Canvas *c)
{
    if (c->dc) {
        DeleteDC(c->dc);
        c->dc = NULL;
    }
    if (c->bmp) {
        DeleteObject(c->bmp);
        c->bmp = NULL;
    }
    c->px = NULL;
}

/* 带 alpha 混合写一个像素 */
static void blend(Canvas *c, int x, int y, COLORREF rgb, double a)
{
    DWORD *p;
    double sr, sg, sb, sa;
    double dr, dg, db;

    if (x < 0 || y < 0 || x >= c->w || y >= c->h)
        return;
    if (a <= 0.0)
        return;
    if (a > 1.0)
        a = 1.0;

    p = &c->px[y * c->w + x];
    dr = (double)GetRValue(rgb);
    dg = (double)GetGValue(rgb);
    db = (double)GetBValue(rgb);
    sr = (double)((*p >> 16) & 0xFF);
    sg = (double)((*p >> 8) & 0xFF);
    sb = (double)(*p & 0xFF);
    sa = (double)((*p >> 24) & 0xFF) / 255.0;

    /* 目标（源色）叠到已有像素上 */
    dr = dr * a + sr * (1.0 - a);
    dg = dg * a + sg * (1.0 - a);
    db = db * a + sb * (1.0 - a);
    {
        double na = a + sa * (1.0 - a);
        if (na > 1.0)
            na = 1.0;
        *p = ((DWORD)(na * 255.0 + 0.5) << 24) |
             ((DWORD)(dr + 0.5) << 16) |
             ((DWORD)(dg + 0.5) << 8) |
             (DWORD)(db + 0.5);
    }
}

/* 画一段圆弧（带抗锯齿）：角度从正上方开始顺时针 */
static void draw_arc(Canvas *c, double cx, double cy, double radius,
                     double width, double start_deg, double sweep_deg,
                     COLORREF rgb, double opacity)
{
    int x, y;
    double outer = radius + width * 0.5;
    double inner = radius - width * 0.5;
    double a0 = start_deg;
    double a1 = start_deg + sweep_deg;

    for (y = 0; y < c->h; y++) {
        for (x = 0; x < c->w; x++) {
            double dx = (double)x + 0.5 - cx;
            double dy = (double)y + 0.5 - cy;
            double dist = sqrt(dx * dx + dy * dy);
            double ang, cov_radial, cov_ang;
            double deg;

            if (dist > outer + 1.5 || dist < inner - 1.5)
                continue;

            /* 角度：正上方 = 0，顺时针增长 */
            ang = atan2(dx, -dy);
            deg = ang * 180.0 / M_PI;
            if (deg < 0.0)
                deg += 360.0;

            /* 径向覆盖率 */
            if (dist <= inner - 0.5 || dist >= outer + 0.5)
                cov_radial = 0.0;
            else if (dist >= inner + 0.5 && dist <= outer - 0.5)
                cov_radial = 1.0;
            else if (dist < inner + 0.5)
                cov_radial = (dist - (inner - 0.5)) / 1.0;
            else
                cov_radial = ((outer + 0.5) - dist) / 1.0;
            if (cov_radial < 0.0)
                cov_radial = 0.0;
            if (cov_radial > 1.0)
                cov_radial = 1.0;

            /* 角度覆盖率（首尾各 1 度羽化） */
            {
                double d0 = deg - a0;
                double d1 = a1 - deg;
                /* 处理 0..360 环绕 */
                if (sweep_deg >= 360.0) {
                    cov_ang = 1.0;
                } else {
                    while (d0 < 0.0) d0 += 360.0;
                    while (d0 >= 360.0) d0 -= 360.0;
                    while (d1 < 0.0) d1 += 360.0;
                    while (d1 >= 360.0) d1 -= 360.0;
                    if (d0 > 180.0 && d1 > 180.0) {
                        cov_ang = 0.0;
                    } else {
                        double e0 = d0 < 1.0 ? d0 : 1.0;
                        double e1 = d1 < 1.0 ? d1 : 1.0;
                        cov_ang = e0 < e1 ? e0 : e1;
                    }
                }
            }

            if (cov_radial > 0.0 && cov_ang > 0.0)
                blend(c, x, y, rgb, cov_radial * cov_ang * opacity);
        }
    }
}

static COLORREF level_color(int level, bool charging)
{
    if (charging)
        return EZ_COL_GREEN;
    if (level >= 60)
        return EZ_COL_GREEN;
    if (level >= 30)
        return EZ_COL_YELLOW;
    return EZ_COL_RED;
}

/* ── 环形电量图标 ─────────────────────────────────────────────── */
HICON ez_make_ring_icon(int level, bool charging, bool light, int px)
{
    Canvas c;
    double scale, cx, cy, radius, width;
    COLORREF fg;
    COLORREF track;
    ICONINFO ii;
    HBITMAP mask;
    HICON icon;

    if (px < 8)
        px = 8;
    if (px > 256)
        px = 256;
    if (!canvas_new(&c, px, px))
        return NULL;

    scale = (double)px / 32.0;
    cx = cy = (double)px * 0.5;
    radius = 12.0 * scale;
    width = 4.0 * scale;

    track = light ? RGB(0xB8, 0xBB, 0xC0) : RGB(0x5F, 0x62, 0x69);
    fg = level_color(level, charging);

    /* 轨道整圈 */
    draw_arc(&c, cx, cy, radius, width, 0.0, 360.0, track, 0.55);

    /* 电量弧：100% 画整圆 */
    if (level >= 100) {
        draw_arc(&c, cx, cy, radius, width, 0.0, 360.0, fg, 1.0);
    } else {
        double sweep = 360.0 * (double)(level < 0 ? 0 : level) / 100.0;
        if (sweep > 0.5)
            draw_arc(&c, cx, cy, radius, width, 0.0, sweep, fg, 1.0);
    }

    /* 生成掩码：全透明图标也能被托盘正确混合 */
    mask = CreateBitmap(px, px, 1, 1, NULL);
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = c.bmp;
    ii.hbmMask = mask;
    icon = CreateIconIndirect(&ii);

    if (mask)
        DeleteObject(mask);
    canvas_free(&c);
    return icon;
}
