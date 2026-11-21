/* round.c — DSH 风格圆角窗口、2px 边框、分层阴影
 *
 * 对齐 DeepSeek Harness 实测参数：
 *   边框 2px，RGB(60,60,60)；圆角视觉半径 16px；
 *   阴影只压暗 3~4 个灰阶，最大 alpha ≈ 40，σ = 6×S。
 *
 * 关键坑（来自实测）：
 *   1. CreateRoundRectRgn 的参数是椭圆**直径** = 2 × 视觉半径；
 *   2. SetWindowRgn 是二值裁剪，圆角边界会留白点 —— 边框必须
 *      向面板内部延伸 2~3px 垫底，并对边缘做 ~1px 羽化；
 *   3. GDI 没有抗锯齿，所以边框和阴影都走 32bpp DIB 手工 SDF 计算。
 */
#include "ez24g.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_SHADOW_SLOTS 8

/* ── 圆角裁剪区域：直径 = 2 × 半径 ─────────────────────────────── */
HRGN ez_round_region(int w, int h, double s, int radius)
{
    int d = (int)(radius * 2.0 * s + 0.5);
    if (d < 2)
        d = 2;
    return CreateRoundRectRgn(0, 0, w + 1, h + 1, d, d);
}

/* ── 有符号距离：圆角矩形的 SDF ────────────────────────────────── */
static double sdf_round_rect(double x, double y, double w, double h, double r)
{
    double cx = w * 0.5;
    double cy = h * 0.5;
    double qx = fabs(x - cx) - (cx - r);
    double qy = fabs(y - cy) - (cy - r);
    double dx = qx > 0.0 ? qx : 0.0;
    double dy = qy > 0.0 ? qy : 0.0;
    double outside = sqrt(dx * dx + dy * dy);
    double inside = (qx > qy ? qx : qy);

    if (outside > 0.0)
        return outside;          /* 形状外，正值 */
    if (inside < 0.0)
        return inside;           /* 形状内，负值 */
    return 0.0;
}

/* ── 创建 32bpp DIB 段 ────────────────────────────────────────── */
static HBITMAP make_dib32(HDC ref, int w, int h, void **bits)
{
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   /* 自上而下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(ref, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

/* ── 分层阴影位图 ─────────────────────────────────────────────── */
static void *g_shadow_bits[MAX_SHADOW_SLOTS];
static int   g_shadow_w[MAX_SHADOW_SLOTS];
static int   g_shadow_h[MAX_SHADOW_SLOTS];

static int shadow_slot(HBITMAP bmp, void *bits, int w, int h)
{
    int i;
    for (i = 0; i < MAX_SHADOW_SLOTS; i++) {
        if (!g_shadow_bits[i]) {
            g_shadow_bits[i] = bits;
            g_shadow_w[i] = w;
            g_shadow_h[i] = h;
            (void)bmp;
            return i;
        }
    }
    return 0;
}

HBITMAP ez_shadow_bitmap(int w, int h, double s, int radius, int *out_w,
                         int *out_h)
{
    int pad = (int)(EZ_SHADOW_PAD * s + 0.5);
    int W = w + pad * 2;
    int H = h + pad * 2;
    void *bits = NULL;
    HBITMAP bmp;
    HDC ref = GetDC(NULL);
    double sigma = 6.0 * s;
    double r = radius * s;
    int x, y;

    if (out_w)
        *out_w = W;
    if (out_h)
        *out_h = H;

    bmp = make_dib32(ref, W, H, &bits);
    ReleaseDC(NULL, ref);
    if (!bmp || !bits)
        return NULL;

    shadow_slot(bmp, bits, W, H);
    ZeroMemory(bits, (size_t)W * H * 4);
    {
        DWORD *px = (DWORD *)bits;
        double two_sigma2 = 2.0 * sigma * sigma;
        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                double d = sdf_round_rect((double)x - pad, (double)y - pad,
                                          (double)w, (double)h, r);
                double alpha;
                if (d <= 0.0) {
                    /* 面板覆盖区：完全不画阴影 */
                    px[y * W + x] = 0;
                    continue;
                }
                alpha = 40.0 * exp(-(d * d) / two_sigma2);
                if (alpha > 40.0)
                    alpha = 40.0;
                if (alpha < 1.0) {
                    px[y * W + x] = 0;
                    continue;
                }
                px[y * W + x] = ((DWORD)(alpha + 0.5) << 24);   /* 黑色，带 alpha */
            }
        }
    }
    return bmp;
}

/* ── 边框位图：向内部延伸垫底 + 边缘羽化 ──────────────────────── */
void ez_border_bitmap(HBITMAP target, HDC ref_dc, int w, int h, double s,
                      int radius)
{
    void *bits = NULL;
    BITMAP bm;
    double r = radius * s;
    double bw = EZ_BORDER_W * s;
    int W, H, x, y, i;

    if (!target || !ref_dc)
        return;
    if (!GetObject(target, sizeof(bm), &bm))
        return;
    if (bm.bmBitsPixel != 32)
        return;

    /* DIB section 的 bmBits 常为 NULL，用创建时登记的指针 */
    for (i = 0; i < MAX_SHADOW_SLOTS; i++) {
        if (g_shadow_bits[i] && g_shadow_w[i] == bm.bmWidth &&
            g_shadow_h[i] == bm.bmHeight) {
            bits = g_shadow_bits[i];
            break;
        }
    }
    if (!bits)
        bits = bm.bmBits;
    if (!bits)
        return;

    W = bm.bmWidth;
    H = bm.bmHeight;
    {
        DWORD *px = (DWORD *)bits;
        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                double d = sdf_round_rect((double)x, (double)y, (double)w,
                                          (double)h, r);
                double ad = fabs(d);
                DWORD old_px = px[y * W + x];
                DWORD base_a = (old_px >> 24) & 0xFF;
                DWORD rgb = old_px & 0x00FFFFFF;
                DWORD new_a = base_a;

                if (d <= 1.0 && d >= -(bw + 2.0)) {
                    double cov;
                    if (d > 0.0) {
                        cov = 1.0 - d;         /* 外沿 1px 羽化 */
                        if (cov < 0.0)
                            cov = 0.0;
                    } else if (ad <= bw) {
                        cov = 1.0;
                    } else {
                        double t = (ad - bw) / 2.0;  /* 向内垫底淡出 */
                        cov = 1.0 - t;
                        if (cov < 0.0)
                            cov = 0.0;
                    }
                    if (cov > 0.0) {
                        DWORD a = (DWORD)(cov * 255.0 + 0.5);
                        if (a > new_a) {
                            new_a = a;
                            rgb = ((DWORD)GetRValue(EZ_COL_BORDER) << 16) |
                                  ((DWORD)GetGValue(EZ_COL_BORDER) << 8) |
                                  (DWORD)GetBValue(EZ_COL_BORDER);
                        }
                    }
                }
                px[y * W + x] = (new_a << 24) | rgb;
            }
        }
    }
}

void ez_round_init(void)
{
    /* 预留：如需启用 DWM 圆角偏好，可在这里设置 */
}
