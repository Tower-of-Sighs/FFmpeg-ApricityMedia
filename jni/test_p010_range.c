#include <stdio.h>
#include <math.h>

static double pq_eotf(double n) {
    const double m1=0.1593017578125, m2=78.84375;
    const double c1=0.8359375, c2=18.8515625, c3=18.6875;
    double x=pow(fmax(n,0),1.0/m2);
    return 10000.0*pow(fmax(x-c1,0)/fmax(c2-c3*x,1e-6), 1.0/m1);
}

static double aces(double x) {
    const double A=2.51,B=0.03,C=2.43,D=0.59,E=0.14;
    return fmax(0, fmin(1, (x*(A*x+B))/(x*(C*x+D)+E)));
}

double srgb_out(double r, double ref_w) {
    double n = pq_eotf(fmax(0,fmin(1,r)));
    if (n < 1e-3) return 0;
    return pow(fmax(0,fmin(1,aces(n/ref_w))), 1.0/2.2);
}

int main(void) {
    printf("=== P010 VP Normalization Hypothesis ===\n\n");

    double ref_w = 75.0;

    /* D3D11 VP with P010 surface:
       Nominal_Range=1 (16-235, ×4 for 10-bit = 64-940)
       The VP SHOULD map: 10-bit code 508 → Yn = (508-64)/(940-64) = 0.5068
       But what if VP treats 10-bit values stored in 16-bit words differently? */

    printf("Hypothesis A: VP correctly uses 10-bit range (64-940)\n");
    double yn_a = (508.0-64.0)/(940.0-64.0);
    printf("  PQ 508 → Yn = %.4f → sRGB = %.4f\n", yn_a, srgb_out(yn_a, ref_w));

    printf("\nHypothesis B: VP uses 16-bit range (4096-60160), ×64 for 10-bit\n");
    printf("  (P010: 10-bit values in 16-bit words, ×64 scaling)\n");
    double yn_b1 = (508.0 - 4096.0) / (60160.0 - 4096.0);  /* 16*256=4096, 235*256=60160 */
    double yn_b2 = (508.0*64.0 - 4096.0) / (60160.0 - 4096.0);
    printf("  PQ 508 raw    → Yn = %.4f → sRGB = %.4f\n", yn_b1, srgb_out(fmax(0,yn_b1), ref_w));
    printf("  PQ 508 ×64   → Yn = %.4f → sRGB = %.4f\n", yn_b2, srgb_out(fmax(0,yn_b2), ref_w));

    printf("\nHypothesis C: VP treats 10-bit as 8-bit (no ×4 scaling)\n");
    printf("  Nominal_Range=1 means 16-235 for 8-bit, applied to 10-bit codes directly\n");
    double yn_c = (508.0-16.0)/(235.0-16.0);
    printf("  PQ 508 → applied 16-235 (8-bit) → Yn = %.4f → sRGB = %.4f\n", yn_c, srgb_out(fmax(0,yn_c), ref_w));

    printf("\nHypothesis D: VP treats P010 values as raw uint16 (0-65535)\n");
    double yn_d = 508.0 / 65535.0;
    printf("  PQ 508 → /65535 → Yn = %.6f → nits = %.2f → sRGB = %.4f\n",
           yn_d, pq_eotf(yn_d), srgb_out(yn_d, ref_w));

    printf("\nHypothesis D2: VP treats P010 values ×64 as uint16 (32512/65535)\n");
    yn_d = (508.0*64.0) / 65535.0;
    printf("  PQ 508×64 → /65535 → Yn = %.4f → nits = %.1f → sRGB = %.4f\n",
           yn_d, pq_eotf(yn_d), srgb_out(yn_d, ref_w));

    printf("\nHypothesis E: VP uses full range 8-bit (0-255) on 10-bit codes\n");
    double yn_e = 508.0 / 255.0; /* > 1.0 */
    printf("  PQ 508 / 255 → Yn = %.4f (clipped to 1.0) → sRGB = %.4f\n",
           yn_e, srgb_out(1.0, ref_w));

    printf("\n--- Most likely culprit ---\n");
    printf("If VP interprets P010 10-bit codes [0,1023] as 8-bit [0,255]\n");
    printf("and applies Nominal_Range=1 (16-235):\n");
    for (int y10 = 64; y10 <= 940; y10 += 200) {
        /* 10-bit code mapped directly into 8-bit range formula */
        double yn = ((double)y10 - 16.0) / (235.0 - 16.0);
        if (yn < 0) yn = 0; if (yn > 1) yn = 1;
        double n = pq_eotf(yn);
        double s = (n<1e-3)?0:pow(fmax(0,fmin(1,aces(n/ref_w))),1.0/2.2);
        printf("  Y10=%4d → VP-out=%.4f → nits=%.1f → sRGB=%.3f\n", y10, yn, n, s);
    }

    printf("\nIf VP interprets P010 as FULL range 8-bit (0-255):\n");
    for (int y10 = 64; y10 <= 940; y10 += 200) {
        double yn = (double)y10 / 255.0;
        if (yn < 0) yn = 0; if (yn > 1) yn = 1;
        double n = pq_eotf(yn);
        double s = (n<1e-3)?0:pow(fmax(0,fmin(1,aces(n/ref_w))),1.0/2.2);
        printf("  Y10=%4d → VP-out=%.4f → nits=%.1f → sRGB=%.3f\n", y10, yn, n, s);
    }

    return 0;
}
