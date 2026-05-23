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

/* Simulate D3D11 VP: YUV limited→RGB full, BT.709 matrix (what VP actually uses) */
void vp_convert(int Y10, int Cb10, int Cr10, double rgb[3]) {
    double y_black=64, y_white=940, y_range=y_white-y_black;
    /* VP Nominal_Range=1 uses same 64-940 for C too (not 64-960). That's wrong for C
       but for neutral chroma it produces a small offset, not darkness. */
    double Yn  = (Y10  - y_black) / y_range;
    double Cbn = (Cb10 - y_black) / y_range;
    double Crn = (Cr10 - y_black) / y_range;
    if (Yn<0)Yn=0; if(Yn>1)Yn=1;
    /* signed chroma relative to neutral=0.5 in full-range space */
    double Cbs = Cbn - 0.5;
    double Crs = Crn - 0.5;
    /* BT.709 matrix coeffs */
    double Vr=1.5748, Ub=1.8556, Ug=0.1873, Vg=0.4681;
    rgb[0]=Yn + Vr*Crs;
    rgb[1]=Yn - Ug*Cbs - Vg*Crs;
    rgb[2]=Yn + Ub*Cbs;
}

int main(void) {
    printf("=== PQ HDR10 → VP → shader tone map → sRGB ===\n\n");

    typedef struct { const char *n; int Y,Cb,Cr; } Px;
    Px px[] = {
        {"black            ",  64,512,512},
        {"100 nit (SDR w)  ", 508,512,512},
        {"500 nit          ", 679,512,512},
        {"1000 nit         ", 751,512,512},
        {"10000 nit peak   ", 940,512,512},
        {"saturated red    ", 500,512,940},
        {"saturated blue   ", 500,960,512},
        {NULL,0,0,0}
    };

    double ref_w = 75.0;

    for (int i=0; px[i].n; i++) {
        double rgb[3];
        vp_convert(px[i].Y, px[i].Cb, px[i].Cr, rgb);
        double nits[3];
        for (int c=0;c<3;c++) nits[c] = pq_eotf(fmax(0,fmin(1,rgb[c])));
        double srgb[3];
        for (int c=0;c<3;c++) {
            double x=fmax(0,nits[c]/ref_w);
            srgb[c]=pow(fmax(0,fmin(1,aces(x))),1.0/2.2);
        }
        printf("%s | VP=(%.4f %.4f %.4f) | nits=(%.0f %.0f %.0f) | sRGB=(%.3f %.3f %.3f)\n",
               px[i].n, rgb[0],rgb[1],rgb[2], nits[0],nits[1],nits[2], srgb[0],srgb[1],srgb[2]);
    }

    /* What if VP does NOT do limited→full expansion?
       I.e., outputs 10-bit codes directly as float [0,1023] → /1023 */
    printf("\n=== What if VP skips range expansion? ===\n\n");
    for (int i=0; px[i].n; i++) {
        double r = px[i].Y / 1023.0;
        double n = pq_eotf(r);
        double s = (n<1e-3)?0:pow(fmax(0,fmin(1,aces(n/ref_w))),1.0/2.2);
        printf("%s | Y/1023=%.4f → nits=%.1f → sRGB=%.3f\n", px[i].n, r, n, s);
    }

    /* What if VP treats 10-bit P010 as 8-bit values?
       P010: 10-bit value in lower 10 bits of 16-bit LE word.
       If VP reads as 8-bit NV12, it reads byte-by-byte. */
    printf("\n=== What if VP misreads 10-bit as 8-bit? ===\n\n");
    printf("P010 10-bit value 508 = 0x01FC, LE bytes: [0xFC, 0x01]\n");
    printf("VP reads as 8-bit NV12: first byte = 0xFC = 252\n");
    double r8 = 252.0/255.0;
    double n8 = pq_eotf(r8);
    double s8 = (n8<1e-3)?0:pow(fmax(0,fmin(1,aces(n8/ref_w))),1.0/2.2);
    printf("NV12(252) → VP-out=%.4f → nits=%.1f → sRGB=%.3f\n", r8, n8, s8);

    printf("\nBut P010 Y=500 (0x01F4) → LE bytes [0xF4,0x01]\n");
    printf("NV12 first byte = 0xF4 = 244\n");
    r8 = 244.0/255.0;
    n8 = pq_eotf(r8);
    s8 = (n8<1e-3)?0:pow(fmax(0,fmin(1,aces(n8/ref_w))),1.0/2.2);
    printf("NV12(244) → VP-out=%.4f → nits=%.1f → sRGB=%.3f\n", r8, n8, s8);

    return 0;
}
