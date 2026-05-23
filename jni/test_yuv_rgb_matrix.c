#include <stdio.h>
#include <math.h>

/* =====================================================
 * Test: YUV→RGB matrix coefficients
 *
 * Problem: ap_d3d11_matrix_from_av maps BT.2020 to
 * D3D11 VP matrix=1 (BT.709), because the legacy
 * D3D11 VideoProcessor API has no BT.2020 option.
 *
 * This test quantifies the color error for typical
 * HDR10 test pixels.
 * ===================================================== */

typedef struct { double kr, kb, kg; } YuvMatrix;

static YuvMatrix BT709  = {0.2126, 0.0722, 1.0 - 0.2126 - 0.0722};
static YuvMatrix BT2020 = {0.2627, 0.0593, 1.0 - 0.2627 - 0.0593};

/* YUV->RGB: R = Y + Vr*Vn, G = Y - Ug*Un - Vg*Vn, B = Y + Ub*Un
   where Yn = (Y - yOff) / yScale, Un = (Cb - cOff) / cScale, Vn = (Cr - cOff) / cScale */
static void yuv_to_rgb(YuvMatrix m, int bit_depth, int full_range,
                       int Y, int Cb, int Cr, double rgb[3])
{
    int scale = 1 << (bit_depth - 8 > 0 ? bit_depth - 8 : 0);
    double yOff, yScale, cOff, cScale;
    int maxSample = (1 << bit_depth) - 1;
    if (full_range) {
        yOff  = 0.0;
        yScale = maxSample;
        cOff  = 1.0 * (1 << (bit_depth - 1));
        cScale = maxSample;
    } else {
        yOff  = 16.0 * scale;
        yScale = 219.0 * scale;
        cOff  = 128.0 * scale;
        cScale = 224.0 * scale;
    }
    double Vr = 2.0 * (1.0 - m.kr);
    double Ub = 2.0 * (1.0 - m.kb);
    double Ug = 2.0 * m.kb * (1.0 - m.kb) / m.kg;
    double Vg = 2.0 * m.kr * (1.0 - m.kr) / m.kg;

    double Yn  = ((double)Y  - yOff) / yScale;
    double Un  = ((double)Cb - cOff) / cScale;
    double Vn  = ((double)Cr - cOff) / cScale;
    if (Yn  < 0.0) Yn  = 0.0; if (Yn  > 1.0) Yn  = 1.0;
    /* Un,Vn allowed outside [0,1] for chroma saturation */

    rgb[0] = Yn + Vr * Vn;
    rgb[1] = Yn - Ug * Un - Vg * Vn;
    rgb[2] = Yn + Ub * Un;
}

/* Test pixels for HDR10 10-bit BT.2020 limited range */
typedef struct {
    const char *name;
    int Y, Cb, Cr;
} TestPixel;

static TestPixel pixels[] = {
    {"black",         64, 512, 512},
    {"mid-gray",     500, 512, 512},
    {"SDR white 100nit", 508, 512, 512},  /* PQ code ~0.5081 → 100 nits */
    {"HDR white 1000nit", 751, 512, 512}, /* PQ code ~0.751 → 1000 nits */
    {"peak 10000nit", 940, 512, 512},     /* PQ code ~1.0 → 10000 nits */
    {"saturated red", 500, 512, 940},     /* Cr = max, Cb = neutral */
    {"saturated blue", 500, 960, 512},    /* Cb = max, Cr = neutral */
    {"saturated yellow", 500, 64, 64},    /* Cb=min, Cr=min */
    {NULL, 0, 0, 0}
};

int main(void)
{
    printf("=== YUV→RGB Matrix Test ===\n\n");
    printf("BT.709:  Kr=%.4f Kb=%.4f Kg=%.4f\n", BT709.kr, BT709.kb, BT709.kg);
    printf("BT.2020: Kr=%.4f Kb=%.4f Kg=%.4f\n\n", BT2020.kr, BT2020.kb, BT2020.kg);

    double max_err = 0.0;
    const char *worst_pixel = NULL;
    int worst_chan = 0;

    for (int i = 0; pixels[i].name; i++) {
        double rgb709[3], rgb2020[3];
        yuv_to_rgb(BT709,  10, 0, pixels[i].Y, pixels[i].Cb, pixels[i].Cr, rgb709);
        yuv_to_rgb(BT2020, 10, 0, pixels[i].Y, pixels[i].Cb, pixels[i].Cr, rgb2020);

        printf("Pixel: %-20s  Y=%4d Cb=%4d Cr=%4d\n",
               pixels[i].name, pixels[i].Y, pixels[i].Cb, pixels[i].Cr);
        printf("  BT.709  RGB: %8.5f %8.5f %8.5f\n",
               rgb709[0], rgb709[1], rgb709[2]);
        printf("  BT.2020 RGB: %8.5f %8.5f %8.5f\n",
               rgb2020[0], rgb2020[1], rgb2020[2]);
        printf("  Δ  RGB: %+8.5f %+8.5f %+8.5f\n",
               rgb709[0]-rgb2020[0], rgb709[1]-rgb2020[1], rgb709[2]-rgb2020[2]);

        for (int c = 0; c < 3; c++) {
            double e = fabs(rgb709[c] - rgb2020[c]);
            if (e > max_err) {
                max_err = e;
                worst_pixel = pixels[i].name;
                worst_chan = c;
            }
        }
        printf("\n");
    }

    const char *chan_name[] = {"R", "G", "B"};
    printf("=== Summary ===\n");
    printf("Max error: %.5f on channel %s for pixel '%s'\n",
           max_err, chan_name[worst_chan], worst_pixel);

    /* Now test: what IF VP used full-range mapping for limited-range PQ content? */
    printf("\n=== Range Error Test ===\n");
    printf("If Nominal_Range is wrong (limited vs full):\n");

    for (int i = 0; pixels[i].name; i++) {
        double rgb_correct[3], rgb_wrong[3];
        /* correct: limited range, BT.2020 */
        yuv_to_rgb(BT2020, 10, 0, pixels[i].Y, pixels[i].Cb, pixels[i].Cr, rgb_correct);
        /* wrong: full range (0-1023) when it should be limited */
        yuv_to_rgb(BT2020, 10, 1, pixels[i].Y, pixels[i].Cb, pixels[i].Cr, rgb_wrong);

        double y_correct  = rgb_correct[0];  /* assume gray pixel for luminance */
        double y_wrong    = rgb_wrong[0];
        printf("Pixel: %-20s  correct Y=%.5f  wrong Y=%.5f  ratio=%.3f\n",
               pixels[i].name, y_correct, y_wrong,
               y_correct > 1e-6 ? y_wrong / y_correct : 0.0);
    }

    return 0;
}
