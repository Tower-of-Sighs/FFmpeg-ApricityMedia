#include <stdio.h>
#include <math.h>

/* ==========================================================
 * Full pipeline test: PQ YUV → VP → shader → SDR
 *
 * Simulates the D3D11 VideoProcessor path:
 *   10-bit PQ-limited YUV → BT.709 matrix RGB → RGBA16F
 *   → GL shader: pqToLinear → /ref_white → ACES → gamma
 *
 * Hypothesis: VP output is too small for tone mapper.
 * ========================================================== */

static double pq_eotf(double n) {
    const double m1 = 0.1593017578125;
    const double m2 = 78.84375;
    const double c1 = 0.8359375;
    const double c2 = 18.8515625;
    const double c3 = 18.6875;
    double x = pow(fmax(n, 0.0), 1.0/m2);
    double num = fmax(x - c1, 0.0);
    double den = fmax(c2 - c3 * x, 1e-6);
    return 10000.0 * pow(num / den, 1.0/m1);
}

static double tone_map_aces(double x) {
    const double A = 2.51, B = 0.03, C = 2.43, D = 0.59, E = 0.14;
    return fmax(0.0, fmin(1.0, (x*(A*x+B))/(x*(C*x+D)+E)));
}

/* Simulate D3D11 VP: 10-bit YUV limited → RGB full range, BT.709 matrix */
static void vp_yuv_to_rgb(int Y10, int Cb10, int Cr10, double rgb[3]) {
    /* limited range Y: 64→0, 940→1; CbCr: 64→0, 960→1 */
    /* But VP uses same range for CbCr as Y (64-940),
       which is slightly wrong for CbCr (should be 64-960). */
    double Yn  = (Y10  - 64.0) / (940.0 - 64.0);
    double Cbn = (Cb10 - 64.0) / (940.0 - 64.0);  /* VP's incorrect C range */
    double Crn = (Cr10 - 64.0) / (940.0 - 64.0);
    if (Yn  < 0.0) Yn  = 0.0; if (Yn  > 1.0) Yn  = 1.0;
    /* BT.709 coefficients (what VP actually uses) */
    double Vr = 2.0*(1.0-0.2126); /* 1.5748 */
    double Ub = 2.0*(1.0-0.0722); /* 1.8556 */
    double Ug = 2.0*0.0722*(1.0-0.0722)/0.7152; /* 0.1873 */
    double Vg = 2.0*0.2126*(1.0-0.2126)/0.7152; /* 0.4681 */
    rgb[0] = Yn + Vr * (Crn - 0.5 + 0.5); /* Crn is relative to 0, but Cr=512 should be neutral */
    rgb[1] = Yn - Ug * (Cbn - 0.5 + 0.5) - Vg * (Crn - 0.5 + 0.5);
    rgb[2] = Yn + Ub * (Cbn - 0.5 + 0.5);
}

int main(void) {
    typedef struct { const char *label; double Y10; } ScenePoint;
    ScenePoint scenes[] = {
        {"black (0 nit)",    64},
        {"100 nit SDR white", 508},
        {"500 nit highlight", 679},
        {"1000 nit",         751},
        {"2000 nit",         803},
        {"10000 nit peak",   940},
        {NULL, 0}
    };

    double ref_whites[] = {20.0, 50.0, 75.0, 100.0, 200.0, -1.0};

    printf("=== PQ Pipeline Test (VP BT.709 matrix, limited range) ===\n\n");

    for (int ri = 0; ref_whites[ri] > 0; ri++) {
        printf("--- ref_white_nits = %.0f ---\n", ref_whites[ri]);
        printf("%-20s | Y10 | VP-RGB | nits | /refW | ACES-out | sRGB\n", "");
        for (int s = 0; scenes[s].label; s++) {
            double Y10  = scenes[s].Y10;
            double Cb10 = 512, Cr10 = 512; /* neutral */
            double rgb[3];
            vp_yuv_to_rgb((int)Y10, (int)Cb10, (int)Cr10, rgb);
            double vp_out  = rgb[0]; /* all three equal for neutral */

            double nits = pq_eotf(vp_out);
            double normalized = nits / ref_whites[ri];
            double aces = tone_map_aces(normalized);
            double srgb  = pow(fmax(0.0, fmin(1.0, aces)), 1.0/2.2);

            printf("%-20s | %4.0f | %.4f | %8.1f | %6.2f | %8.4f | %.4f\n",
                   scenes[s].label, Y10, vp_out, nits, normalized, aces, srgb);
        }
        printf("\n");
    }

    /* Test: what if VP output were 10x too small? */
    printf("=== VP Output Scaling Hypothesis ===\n\n");
    for (double scale = 0.01; scale <= 1.0; scale *= 10.0) {
        double vp_val = 0.5068 * scale; /* SDR white 100nit PQ value */
        double nits = pq_eotf(vp_val);
        double normalized = nits / 75.0;
        double aces = tone_map_aces(normalized);
        double srgb  = pow(fmax(0.0, fmin(1.0, aces)), 1.0/2.2);
        printf("VP output = %.4f (scale=%.2f) → nits=%.2f → /75=%.4f → ACES=%.4f → sRGB=%.4f\n",
               vp_val, scale, nits, normalized, aces, srgb);
    }

    /* Test: what IF VP does NOT expand limited→full range? */
    printf("\n=== Missing Range Expansion ===\n\n");
    printf("If VP outputs 10-bit code values directly [0,1023] into RGBA16F:\n");
    for (int s = 0; scenes[s].label; s++) {
        double Y10 = scenes[s].Y10;
        /* VP output = raw code / 1023 (like full-range pass-through) */
        double vp_val = Y10 / 1023.0;
        double nits = pq_eotf(vp_val);
        double srgb;
        if (nits < 1e-3) {
            srgb = 0.0;
        } else {
            double aces = tone_map_aces(nits / 75.0);
            srgb = pow(fmax(0.0, fmin(1.0, aces)), 1.0/2.2);
        }
        printf("%-20s Y10=%4.0f → VP-out=%.4f → nits=%.2f → sRGB=%.4f\n",
               scenes[s].label, Y10, vp_val, nits, srgb);
    }

    return 0;
}
