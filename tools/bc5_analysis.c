// bc5_analysis.c - offline correlation & entropy study of stb-generated BC5 blocks
// on the 4 normal maps of images/bc5. Reproduces the block generation of
// tests/image_tests.c (stb_compress_bc4_block on red, then green, per 4x4 block),
// and also runs the real bcp_compress_bc5 as a ground-truth anchor.
//
// build:  from the repo root:  cc -O2 -I third_party -I src -o bc5_analysis tools/bc5_analysis.c src/bc_packed.c -lm
// usage:  run from the repo root (paths are ./images/bc5/...)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>

#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

#include "bc_packed.h"

static const char* const files[] =
{
    "images/bc5/grey_roof_tiles_02_nor_dx_1k.png",
    "images/bc5/patterned_cobblestone_nor_dx_1k.png",
    "images/bc5/red_brick_nor_dx_1k.png",
    "images/bc5/rough_wood_nor_dx_1k.png"
};

// intra-block row zigzag, same as bc_packed.c
static const uint32_t zz[16] = { 0, 1, 2, 3,  7, 6, 5, 4,  8, 9, 10, 11,  15, 14, 13, 12 };

// stb linear-scale (0 = min ... 7 = max) -> index mapping, derived from stb__CompressAlphaBlock
static const uint8_t s2idx[8] = { 1, 7, 6, 5, 4, 3, 2, 0 };

// D3D BC4_UNORM spec palette (ep0 = block byte 0 = stb max, ep1 = block byte 1 = stb min)
static inline int spec_pal(int e0, int e1, int i)
{
    if (i == 0 || i == 7)
        return e0;
    if (i == 1)
        return e1;
    return (e0 * (7 - i) + e1 * (i - 1)) / 6;
}

static inline int dbucket(int d)
{
    if (d == 0) return 0;
    if (d == 1) return 1;
    if (d == 2) return 2;
    if (d == 3) return 3;
    if (d < 8)  return 4;
    if (d < 16) return 5;
    if (d < 64) return 6;
    return 7;
}
static const char* const dbname[] = { "d=0", "d=1", "d=2", "d=3", "d=4-7", "d=8-15", "d=16-63", "d>=64" };

static inline int mbucket(int m) { return m >> 5; } // 8 buckets of 32

typedef struct
{
    uint8_t ep[2][2];    // [channel][endpoint] as stored: [0]=max, [1]=min (stb order)
    uint8_t idx[2][16];  // [channel][raster pixel] 3-bit index
    uint8_t px[2][16];   // [channel][raster pixel] source 8-bit value
} block_t;

typedef struct
{
    double n, sx, sy, sxx, syy, sxy;
} pearson_t;

static void pear_add(pearson_t* p, double x, double y)
{
    p->n += 1;
    p->sx += x;
    p->sy += y;
    p->sxx += x * x;
    p->syy += y * y;
    p->sxy += x * y;
}

static double pear_get(const pearson_t* p)
{
    if (p->n < 2)
        return 0;
    double cov = p->sxy - p->sx * p->sy / p->n;
    double vx = p->sxx - p->sx * p->sx / p->n;
    double vy = p->syy - p->sy * p->sy / p->n;
    if (vx <= 0 || vy <= 0)
        return 0;
    return cov / sqrt(vx * vy);
}

typedef struct
{
    uint64_t nblk;

    // ---- endpoints
    uint32_t h_epd_left[2][256];
    uint32_t h_epd_top[2][256];
    uint32_t h_epd_2d[2][256];       // raster: curr - (left + top - upperleft) mod 256
    uint32_t h_epd_chain[2][256];    // exact strip-zigzag chain delta (strip first row)
    uint32_t h_epd_extrap[2][256];   // exact compressor 2D extrapolation delta (later rows)
    uint32_t h_d[2][256];            // endpoint range (max - min)
    uint32_t h_mid[2][32];           // midpoint / 8
    uint32_t h_drg[8][8];            // red d-bucket x green d-bucket
    uint32_t h_midrg[17];            // (mid_r - mid_g) / 4 clamped to [-8..8] + 8
    pearson_t ps_dd, ps_mm, ps_raw, ps_mid;

    // ---- single indices, per channel
    uint32_t h_idx[2][8];
    uint64_t s_idxpos[2][16];
    uint32_t h_const[2][8];          // all-16-identical blocks: the repeated value
    uint32_t n_const[2];
    uint32_t h_idx_db[2][8][8];      // idx by endpoint-range bucket
    uint32_t h_idx_dbmid[2][8][8][8];// idx by (range bucket, midpoint bucket)
    uint32_t h_first1[2][8];         // zigzag single, k = 0, as-is
    uint32_t h_sx[2][8][8];          // single ^ previous zigzag single, ctx = previous value
    uint32_t h_sar[2][8][8];         // (single - previous) mod 8, ctx = previous value
    uint32_t h_sar_db[2][64][8];     // (single - previous) mod 8, ctx = previous * 8 + range bucket

    // ---- pairs (zigzag positions 2p, 2p+1), per channel
    uint32_t h_pair[2][64];          // all 8 pairs, as-is
    uint32_t h_first[2][64];         // pair 0, as-is (current model)
    uint32_t h_pctx[2][8][64];       // current model: pair ^ previous, ctx = previous & 7
    uint32_t h_par[2][8][64];        // (pair - previous) mod 64, ctx = previous & 7
    uint32_t h_pctxd[2][8][64];      // pair ^ previous, ctx = endpoint-range bucket

    // ---- within-block index correlation, both channels pooled
    uint32_t h_horiz[8][8];          // (idx[p], idx[p+1]) same row
    uint32_t h_vert[8][8];           // (idx[p], idx[p+4])
    uint32_t h_horiz_db[8][8][8];    // horiz joint by the block's own range bucket
    uint32_t h_vert_db[8][8][8];

    // ---- spatial prediction, per pixel, both channels pooled
    uint32_t h_sp_same[8][8];        // delta mod 8 vs image-adjacent pixel, predictor in same block
    uint32_t h_sp_adj[8][8];         // ... predictor pixel in the left/upper neighbor block
    uint32_t h_rea_same[8][8];       // re-anchored value prediction, same block
    uint32_t h_rea_adj[8][8];        // re-anchored value prediction, neighbor block
    uint32_t h_rea_db[2][8][8];      // re-anchored, ctx = own endpoint-range bucket
    uint32_t h_grid_l[8][8];         // delta vs same grid position of the left block
    uint32_t h_grid_t[8][8];         // delta vs same grid position of the upper block
    uint32_t h_2d_same[8][8];        // 2D in-block predictor (left, else up), ctx = pred
    uint32_t h_2d_db[2][64][8];      // same predictor, ctx = pred * 8 + own range bucket
    uint32_t h_rearg[8][8];          // green idx from red pixel value re-anchored, ctx = pred

    // ---- red x green
    uint32_t h_rg[8][8];             // (red idx, green idx) same pixel
    uint32_t h_rgdiff[15];           // red - green in [-7..7] + 7
    uint32_t h_rgp[64][64];          // (red pair, green pair) same zigzag pair position
    uint32_t h_48xor[8];             // popcount(48-bit red ^ green) buckets
    uint32_t h_rgpred[64][8];        // (green - red) mod 8, ctx = drb * 8 + dgb
    uint32_t h_rgdb[8][8][8][8];     // drb, dgb, red, green

    // ---- source value rank s = round(8 * (v - mn) / d), both channels pooled
    uint32_t h_s[8];
    uint32_t h_sdiff_same[8][8];     // s - s(left pixel) mod 8, same block, ctx = s(left)
    uint32_t h_sdiff_adj[8][8];      // s - s_reanchored(left pixel value) mod 8, ctx = s_pred

    // ---- stb idx-7 behavior check
    uint32_t q7[2], q7min[2], q7max[2];
    uint32_t qmatch8[2], qtot8[2];   // idx == s2idx[rank] among d >= 8 pixels
    uint64_t qerrsum[2], qnpix[2];
    uint32_t h_7band[16];            // idx7 pixels: (v-mn)*16/d in 16 bins, both channels

    // ---- 12-bit rows (row r of the block = 4 raster indices = the r-th 12-bit slice of the 48 bits)
    uint32_t h_row[2][4096];         // row value histogram, per channel
    uint32_t h_rowd[2][4096];        // (row_r - row_{r-1}) mod 4096, r = 1..3, per channel
    uint32_t h_rowx[2][4096];        // row_r ^ row_{r-1}, r = 1..3, per channel
    uint64_t n_rowseq[2];            // adjacent row pairs in the raster row stream, per channel
    uint64_t n_rowseq_eq[2];         // of those, equal rows
} stats_t;

static uint64_t hist_sum(const uint32_t* h, int n)
{
    uint64_t t = 0;
    for (int i = 0; i < n; ++i)
        t += h[i];
    return t;
}

// Shannon entropy of a flat histogram, bits per symbol
static double h_dist(const uint32_t* h, int n)
{
    uint64_t total = hist_sum(h, n);
    if (total == 0)
        return 0;
    double bits = 0;
    for (int i = 0; i < n; ++i)
        if (h[i] > 0)
        {
            double p = (double)h[i] / (double)total;
            bits -= p * log2(p);
        }
    return bits;
}

// H(symbol | context) from a joint histogram [ctx][sym], bits per symbol
static double h_cond(const uint32_t* j, int nctx, int nsym)
{
    uint64_t total = 0;
    for (int c = 0; c < nctx; ++c)
        total += hist_sum(&j[c * nsym], nsym);
    if (total == 0)
        return 0;
    double bits = 0;
    for (int c = 0; c < nctx; ++c)
    {
        uint64_t tc = hist_sum(&j[c * nsym], nsym);
        if (tc == 0)
            continue;
        double cb = 0;
        for (int s = 0; s < nsym; ++s)
            if (j[c * nsym + s] > 0)
            {
                double p = (double)j[c * nsym + s] / (double)tc;
                cb -= p * log2(p);
            }
        bits += cb * (double)tc / (double)total;
    }
    return bits;
}

// mutual information of a joint histogram, bits
static double mi_2d(const uint32_t* j, int na, int nb)
{
    uint64_t total = 0;
    for (int i = 0; i < na * nb; ++i)
        total += j[i];
    if (total == 0)
        return 0;
    double mi = 0;
    for (int a = 0; a < na; ++a)
    {
        uint64_t ca = 0;
        for (int k = 0; k < nb; ++k)
            ca += j[a * nb + k];
        for (int b = 0; b < nb; ++b)
        {
            uint64_t cb = 0;
            for (int k = 0; k < na; ++k)
                cb += j[k * nb + b];
            if (j[a * nb + b] == 0 || ca == 0 || cb == 0)
                continue;
            double p = (double)j[a * nb + b] / (double)total;
            double pab = (double)ca / (double)total * (double)cb / (double)total;
            mi += p * log2(p / pab);
        }
    }
    return mi;
}

// correlation coefficient from a joint histogram of integer values
static double joint_corr(const uint32_t* j, int na, int nb)
{
    uint64_t total = 0, sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    for (int a = 0; a < na; ++a)
        for (int b = 0; b < nb; ++b)
        {
            uint64_t n = j[a * nb + b];
            if (n == 0)
                continue;
            total += n;
            sa += n * a;
            sb += n * b;
            saa += n * (uint64_t)a * a;
            sbb += n * (uint64_t)b * b;
            sab += n * (uint64_t)a * b;
        }
    if (total < 2)
        return 0;
    double d = (double)total;
    double cov = (double)sab / d - (double)sa / d * (double)sb / d;
    double va = (double)saa / d - (double)sa / d * (double)sa / d;
    double vb = (double)sbb / d - (double)sb / d * (double)sb / d;
    if (va <= 0 || vb <= 0)
        return 0;
    return cov / sqrt(va * vb);
}

static void merge_pearson(pearson_t* g, const pearson_t* s)
{
    g->n += s->n; g->sx += s->sx; g->sy += s->sy;
    g->sxx += s->sxx; g->syy += s->syy; g->sxy += s->sxy;
}

static void merge_stats(stats_t* g, const stats_t* s)
{
    g->nblk += s->nblk;
    #define M2(F) for (int a = 0; a < 2; ++a) for (int b = 0; b < 256; ++b) g->F[a][b] += s->F[a][b];
    M2(h_epd_left) M2(h_epd_top) M2(h_epd_2d) M2(h_epd_chain) M2(h_epd_extrap) M2(h_d)
    #undef M2
    #define M3(F, M, N) for (int a = 0; a < M; ++a) for (int b = 0; b < N; ++b) g->F[a][b] += s->F[a][b];
    M3(h_mid, 2, 32) M3(h_idx, 2, 8) M3(h_const, 2, 8) M3(h_first1, 2, 8)
    M3(h_pair, 2, 64) M3(h_first, 2, 64)
    #undef M3
    #define M4(F, M, N, K) for (int a = 0; a < M; ++a) for (int b = 0; b < N; ++b) for (int c = 0; c < K; ++c) g->F[a][b][c] += s->F[a][b][c];
    M4(h_idx_db, 2, 8, 8) M4(h_sx, 2, 8, 8) M4(h_sar, 2, 8, 8)
    M4(h_sar_db, 2, 64, 8) M4(h_pctx, 2, 8, 64) M4(h_par, 2, 8, 64) M4(h_pctxd, 2, 8, 64) M4(h_rea_db, 2, 8, 8)
    M4(h_horiz_db, 8, 8, 8) M4(h_vert_db, 8, 8, 8)
    #undef M4
    #define M5(F, M, N, K, L) for (int a = 0; a < M; ++a) for (int b = 0; b < N; ++b) for (int c = 0; c < K; ++c) for (int e = 0; e < L; ++e) g->F[a][b][c][e] += s->F[a][b][c][e];
    M5(h_idx_dbmid, 2, 8, 8, 8) M5(h_rgdb, 8, 8, 8, 8)
    #undef M5
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 16; ++b)
            g->s_idxpos[a][b] += s->s_idxpos[a][b];
    for (int a = 0; a < 2; ++a)
    {
        g->n_const[a] += s->n_const[a];
        g->q7[a] += s->q7[a];
        g->q7min[a] += s->q7min[a];
        g->q7max[a] += s->q7max[a];
        g->qmatch8[a] += s->qmatch8[a];
        g->qtot8[a] += s->qtot8[a];
        g->qerrsum[a] += s->qerrsum[a];
        g->qnpix[a] += s->qnpix[a];
    }
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 64; ++b)
            for (int c = 0; c < 8; ++c)
                g->h_2d_db[a][b][c] += s->h_2d_db[a][b][c];
    for (int a = 0; a < 16; ++a)
        g->h_7band[a] += s->h_7band[a];
    for (int a = 0; a < 2; ++a)
    {
        for (int b = 0; b < 4096; ++b)
        {
            g->h_row[a][b] += s->h_row[a][b];
            g->h_rowd[a][b] += s->h_rowd[a][b];
            g->h_rowx[a][b] += s->h_rowx[a][b];
        }
        g->n_rowseq[a] += s->n_rowseq[a];
        g->n_rowseq_eq[a] += s->n_rowseq_eq[a];
    }
    for (int a = 0; a < 8; ++a)
        for (int b = 0; b < 8; ++b)
        {
            g->h_2d_same[a][b] += s->h_2d_same[a][b];
            g->h_rearg[a][b] += s->h_rearg[a][b];
            g->h_drg[a][b] += s->h_drg[a][b];
            g->h_sp_same[a][b] += s->h_sp_same[a][b];
            g->h_sp_adj[a][b] += s->h_sp_adj[a][b];
            g->h_rea_same[a][b] += s->h_rea_same[a][b];
            g->h_rea_adj[a][b] += s->h_rea_adj[a][b];
            g->h_grid_l[a][b] += s->h_grid_l[a][b];
            g->h_grid_t[a][b] += s->h_grid_t[a][b];
            g->h_rg[a][b] += s->h_rg[a][b];
            g->h_sdiff_same[a][b] += s->h_sdiff_same[a][b];
            g->h_sdiff_adj[a][b] += s->h_sdiff_adj[a][b];
            g->h_horiz[a][b] += s->h_horiz[a][b];
            g->h_vert[a][b] += s->h_vert[a][b];
        }
    for (int a = 0; a < 8; ++a)
        g->h_s[a] += s->h_s[a];
    for (int a = 0; a < 15; ++a)
        g->h_rgdiff[a] += s->h_rgdiff[a];
    for (int a = 0; a < 64; ++a)
        for (int b = 0; b < 64; ++b)
            g->h_rgp[a][b] += s->h_rgp[a][b];
    for (int a = 0; a < 8; ++a)
        g->h_48xor[a] += s->h_48xor[a];
    for (int a = 0; a < 64; ++a)
        for (int b = 0; b < 8; ++b)
            g->h_rgpred[a][b] += s->h_rgpred[a][b];
    for (int a = 0; a < 17; ++a)
        g->h_midrg[a] += s->h_midrg[a];
    merge_pearson(&g->ps_dd, &s->ps_dd);
    merge_pearson(&g->ps_mm, &s->ps_mm);
    merge_pearson(&g->ps_raw, &s->ps_raw);
    merge_pearson(&g->ps_mid, &s->ps_mid);
}

static void print_8x8(const char* name, const uint32_t* h)
{
    printf("  %s (rows = first value, cols = second):\n", name);
    printf("        ");
    for (int b = 0; b < 8; ++b)
        printf("   %d", b);
    printf("\n");
    for (int a = 0; a < 8; ++a)
    {
        printf("   %d |", a);
        for (int b = 0; b < 8; ++b)
            printf(" %5u", h[a * 8 + b]);
        printf("\n");
    }
}

typedef struct
{
    uint32_t count;
    uint32_t value;
} rowcnt_t;

static int rowcnt_cmp(const void* a, const void* b)
{
    const rowcnt_t* ra = (const rowcnt_t*)a;
    const rowcnt_t* rb = (const rowcnt_t*)b;
    if (ra->count != rb->count)
        return ra->count < rb->count ? 1 : -1;
    return ra->value < rb->value ? -1 : 1;
}

// Static top-k row table: fills *mass (fraction of rows captured) and *cost (modeled Shannon
// bits/row: flag + in-table reference entropy + 12-bit escape + the 12-bit-per-entry header).
static void row_table_cost(const uint32_t* h, int k, double* mass, double* cost)
{
    static rowcnt_t tab[4096];
    *mass = 0;
    *cost = 12.0;
    uint64_t total = hist_sum(h, 4096);
    if (total == 0)
        return;
    for (int i = 0; i < 4096; ++i)
    {
        tab[i].count = h[i];
        tab[i].value = (uint32_t)i;
    }
    qsort(tab, 4096, sizeof(rowcnt_t), rowcnt_cmp);
    double m = 0, ent = 0;
    for (int i = 0; i < k; ++i)
    {
        if (tab[i].count == 0)
            break;
        double p = (double)tab[i].count / (double)total;
        m += p;
        ent -= p * log2(p);
    }
    if (m <= 0.0)
        return;
    double miss = 1.0 - m;
    *mass = m;
    *cost = -m * log2(m) - miss * log2(miss) + ent + miss * 12.0 + (double)k * 12.0 / (double)total;
}

// model rows for the per-image / aggregate tables
typedef struct
{
    const char* name;
    double red, green, avg;
} model_row;

static void model_table(const char* indent, const model_row* rows, int n)
{
    printf("%s%-30s %8s %8s %8s   (bits/index)\n", indent, "model", "red", "green", "avg");
    for (int i = 0; i < n; ++i)
        printf("%s%-30s %8.4f %8.4f %8.4f\n", indent, rows[i].name, rows[i].red, rows[i].green, rows[i].avg);
}

// fills the model rows from a stats struct (per channel where applicable)
static int build_rows(const stats_t* S, model_row* rows)
{
    double Hidx[2], Hsxor[2], Hsar[2], Hpairraw[2], Hcur[2], Hpar[2], Hpctxd[2], Hidb[2], Hidbmid[2], Hrealdb[2];
    double Hrow[2], Hrowd[2], Hrowx[2];
    for (int c = 0; c < 2; ++c)
    {
        Hidx[c] = h_dist(&S->h_idx[c][0], 8);
        Hsxor[c] = (h_dist(&S->h_first1[c][0], 8) + 15.0 * h_cond(&S->h_sx[c][0][0], 8, 8)) / 16.0;
        Hsar[c] = (h_dist(&S->h_first1[c][0], 8) + 15.0 * h_cond(&S->h_sar[c][0][0], 8, 8)) / 16.0;
        Hpairraw[c] = h_dist(&S->h_pair[c][0], 64) / 2.0;
        Hcur[c] = (h_dist(&S->h_first[c][0], 64) + 7.0 * h_cond(&S->h_pctx[c][0][0], 8, 64)) / 16.0;
        Hpar[c] = (h_dist(&S->h_first[c][0], 64) + 7.0 * h_cond(&S->h_par[c][0][0], 8, 64)) / 16.0;
        Hpctxd[c] = (h_dist(&S->h_first[c][0], 64) + 7.0 * h_cond(&S->h_pctxd[c][0][0], 8, 64)) / 16.0;
        Hidb[c] = h_cond(&S->h_idx_db[c][0][0], 8, 8);
        Hidbmid[c] = h_cond(&S->h_idx_dbmid[c][0][0][0], 64, 8);
        Hrealdb[c] = h_cond(&S->h_rea_db[c][0][0], 8, 8);
        Hrow[c] = h_dist(&S->h_row[c][0], 4096);
        Hrowd[c] = h_dist(&S->h_rowd[c][0], 4096);
        Hrowx[c] = h_dist(&S->h_rowx[c][0], 4096);
    }
    uint64_t ns = hist_sum(&S->h_sp_same[0][0], 64) + hist_sum(&S->h_sp_adj[0][0], 64);
    double Hsp = ns ? (h_cond(&S->h_sp_same[0][0], 8, 8) * (double)hist_sum(&S->h_sp_same[0][0], 64) +
                       h_cond(&S->h_sp_adj[0][0], 8, 8) * (double)hist_sum(&S->h_sp_adj[0][0], 64)) / (double)ns : 0;
    uint64_t nr = hist_sum(&S->h_rea_same[0][0], 64) + hist_sum(&S->h_rea_adj[0][0], 64);
    double Hrea = nr ? (h_cond(&S->h_rea_same[0][0], 8, 8) * (double)hist_sum(&S->h_rea_same[0][0], 64) +
                        h_cond(&S->h_rea_adj[0][0], 8, 8) * (double)hist_sum(&S->h_rea_adj[0][0], 64)) / (double)nr : 0;
    uint64_t ng = hist_sum(&S->h_grid_l[0][0], 64) + hist_sum(&S->h_grid_t[0][0], 64);
    double Hgrid = ng ? (h_cond(&S->h_grid_l[0][0], 8, 8) * (double)hist_sum(&S->h_grid_l[0][0], 64) +
                         h_cond(&S->h_grid_t[0][0], 8, 8) * (double)hist_sum(&S->h_grid_t[0][0], 64)) / (double)ng : 0;

    double Hgr = h_cond(&S->h_rg[0][0], 8, 8);
    double Hrgjoint = h_dist(&S->h_rg[0][0], 64) / 2.0;
    double Hrgpred = h_cond(&S->h_rgpred[0][0], 64, 8);

    int n = 0;
    #define ROW(NAME, R, G) rows[n].name = NAME; rows[n].red = R; rows[n].green = G; rows[n].avg = (R + G) / 2.0; ++n;
    ROW("raw 3-bit index", Hidx[0], Hidx[1]);
    ROW("single^chain (v1 style)", Hsxor[0], Hsxor[1]);
    ROW("single-delta chain", Hsar[0], Hsar[1]);
    ROW("raw 6-bit pair / 2", Hpairraw[0], Hpairraw[1]);
    ROW("CURRENT: pair^prev (8 ctx)", Hcur[0], Hcur[1]);
    ROW("pair arith delta (8 ctx)", Hpar[0], Hpar[1]);
    ROW("pair^prev, ctx = range bucket", Hpctxd[0], Hpctxd[1]);
    ROW("idx | range bucket", Hidb[0], Hidb[1]);
    ROW("idx | (range, mid) bucket", Hidbmid[0], Hidbmid[1]);
    rows[n].name = "2D-neighbor idx (left-preferred)"; rows[n].red = Hsp; rows[n].green = Hsp; rows[n].avg = Hsp; ++n;
    rows[n].name = "re-anchored value predictor";       rows[n].red = Hrea; rows[n].green = Hrea; rows[n].avg = Hrea; ++n;
    ROW("re-anchored, ctx = range bucket", Hrealdb[0], Hrealdb[1]);
    rows[n].name = "grid-identical neighbor block";    rows[n].red = Hgrid; rows[n].green = Hgrid; rows[n].avg = Hgrid; ++n;
    ROW("green idx | red idx (same px)", Hgr, Hgr);
    ROW("green-red mod8, ctx = ranges", Hrgpred, Hrgpred);
    ROW("joint H(red,green) / 2", Hrgjoint, Hrgjoint);
    {
        uint64_t n2d = hist_sum(&S->h_2d_same[0][0], 64);
        double H2d = h_cond(&S->h_2d_same[0][0], 8, 8);
        double Hrawp = (Hidx[0] + Hidx[1]) / 2.0;
        double v2d = (15.0 * H2d + Hrawp) / 16.0;
        rows[n].name = "2D in-block, ctx = pred"; rows[n].red = v2d; rows[n].green = v2d; rows[n].avg = v2d; ++n;
        double v2dd[2];
        for (int c = 0; c < 2; ++c)
            v2dd[c] = (15.0 * h_cond(&S->h_2d_db[c][0][0], 64, 8) + Hidx[c]) / 16.0;
        ROW("2D in-block, ctx = pred+range", v2dd[0], v2dd[1]);
        double Hrearg = h_cond(&S->h_rearg[0][0], 8, 8);
        ROW("green | red-value re-anchored", Hrearg, Hrearg);
        {
            double vsar[2];
            for (int c = 0; c < 2; ++c)
                vsar[c] = (h_dist(&S->h_first1[c][0], 8) + 15.0 * h_cond(&S->h_sar_db[c][0][0], 64, 8)) / 16.0;
            ROW("single-delta chain, ctx = prev+range", vsar[0], vsar[1]);
        }
        (void)n2d;
    }
    double vrowraw[2], vrowd[2], vrowtab[2];
    for (int c = 0; c < 2; ++c)
    {
        vrowraw[c] = Hrow[c] / 4.0;
        double hd = Hrowd[c] < Hrowx[c] ? Hrowd[c] : Hrowx[c];
        vrowd[c] = (12.0 + 3.0 * hd) / 16.0;
        double m, cost;
        row_table_cost(&S->h_row[c][0], 64, &m, &cost);
        vrowtab[c] = cost / 4.0;
    }
    ROW("12-bit row raw / 4", vrowraw[0], vrowraw[1]);
    ROW("row delta vs row above / 4", vrowd[0], vrowd[1]);
    ROW("row top-64 table / 4", vrowtab[0], vrowtab[1]);
    #undef ROW
    return n;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    bcp_context* ctx = bcp_init(NULL);

    static stats_t global;
    static uint32_t file_hrow[4][2][4096]; // per-file row histograms, for the cross-file repetition study
    memset(&global, 0, sizeof(global));

    int nf = (int)(sizeof(files) / sizeof(files[0]));

    for (int f = 0; f < nf; ++f)
    {
        uint32_t w, h, nc;
        uint8_t* rgba = stbi_load(files[f], (int*)&w, (int*)&h, (int*)&nc, 4);
        if (rgba == NULL)
        {
            fprintf(stderr, "unable to load %s\n", files[f]);
            continue;
        }

        uint32_t wb = w / 4, hb = h / 4;
        block_t* blocks = malloc(wb * hb * sizeof(block_t));
        memset(blocks, 0, wb * hb * sizeof(block_t));

        // build the BC5 image exactly like test_bc5_images
        for (uint32_t by = 0; by < hb; ++by)
            for (uint32_t bx = 0; bx < wb; ++bx)
            {
                block_t* B = &blocks[by * wb + bx];
                for (uint32_t c = 0; c < 2; ++c)
                {
                    uint8_t img16[16];
                    for (uint32_t j = 0; j < 4; ++j)
                        for (uint32_t i = 0; i < 4; ++i)
                        {
                            uint8_t v = rgba[((by * 4 + j) * w + (bx * 4 + i)) * 4 + c];
                            img16[j * 4 + i] = v;
                            B->px[c][j * 4 + i] = v;
                        }
                    static uint8_t bc4[16]; // static: avoids an arm64 ASan stack-slot-reuse false positive
                    stb_compress_bc4_block(bc4, img16);
                    B->ep[c][0] = bc4[0];
                    B->ep[c][1] = bc4[1];
                    for (uint32_t p = 0; p < 16; ++p)
                    {
                        uint32_t bit = p * 3;
                        uint32_t byte = 2 + (bit >> 3);
                        uint32_t off = bit & 7;
                        uint32_t v = bc4[byte] >> off;
                        if (off > 5)
                            v |= bc4[byte + 1] << (8 - off);
                        B->idx[c][p] = (uint8_t)(v & 7);
                    }
                }
            }

        static stats_t S;
        memset(&S, 0, sizeof(S));
        S.nblk = wb * hb;
        // ---- per-block statistics
        for (uint32_t by = 0; by < hb; ++by)
            for (uint32_t bx = 0; bx < wb; ++bx)
            {
                block_t* B = &blocks[by * wb + bx];
                const block_t* left = bx > 0 ? &blocks[by * wb + bx - 1] : NULL;
                const block_t* top = by > 0 ? &blocks[(by - 1) * wb + bx] : NULL;
                const block_t* ul = (bx > 0 && by > 0) ? &blocks[(by - 1) * wb + bx - 1] : NULL;

                for (uint32_t c = 0; c < 2; ++c)
                {
                    int mx = B->ep[c][0], mn = B->ep[c][1];
                    int d = mx - mn;
                    int db = dbucket(d);
                    S.h_d[c][d]++;
                    S.h_mid[c][(mx + mn) >> 4]++;

                    // endpoint deltas vs raster neighbors
                    for (int j = 0; j < 2; ++j)
                    {
                        if (left)
                            S.h_epd_left[c][(uint8_t)(B->ep[c][j] - left->ep[c][j])]++;
                        if (top)
                            S.h_epd_top[c][(uint8_t)(B->ep[c][j] - top->ep[c][j])]++;
                        if (ul)
                            S.h_epd_2d[c][(uint8_t)(B->ep[c][j] - (left->ep[c][j] + top->ep[c][j] - ul->ep[c][j]))]++;
                    }

                    // red-green endpoint relations
                    if (c == 0)
                    {
                        int dgr = dbucket(B->ep[1][0] - B->ep[1][1]);
                        S.h_drg[db][dgr]++;
                        int mdiff = ((mx + mn) / 2) - ((B->ep[1][0] + B->ep[1][1]) / 2);
                        mdiff /= 4;
                        if (mdiff > 8) mdiff = 8;
                        if (mdiff < -8) mdiff = -8;
                        S.h_midrg[mdiff + 8]++;
                        pear_add(&S.ps_dd, d, B->ep[1][0] - B->ep[1][1]);
                        pear_add(&S.ps_mm, (mx + mn) / 2.0, (B->ep[1][0] + B->ep[1][1]) / 2.0);
                        pear_add(&S.ps_mid, (mx + mn) / 2.0 - 128.0, (B->ep[1][0] + B->ep[1][1]) / 2.0 - 128.0);
                    }

                    // indices: histograms, per-position sums, constant blocks, stb quirk, source rank
                    int all_same = 1;
                    for (uint32_t p = 0; p < 16; ++p)
                    {
                        uint8_t i = B->idx[c][p];
                        S.h_idx[c][i]++;
                        S.s_idxpos[c][p] += i;
                        S.h_idx_db[c][db][i]++;
                        S.h_idx_dbmid[c][db][mbucket((mx + mn) / 2)][i]++;
                        if (i != B->idx[c][0])
                            all_same = 0;
                        S.qnpix[c]++;
                        S.qerrsum[c] += (uint64_t)abs(spec_pal(mx, mn, i) - B->px[c][p]);
                        if (i == 7 && d > 0)
                        {
                            int off = B->px[c][p] - mn;
                            S.q7[c]++;
                            if (off * 8 < d)
                                S.q7min[c]++;
                            else if ((mx - B->px[c][p]) * 8 < d)
                                S.q7max[c]++;
                            int t = off * 16 / d;
                            if (t > 15) t = 15;
                            S.h_7band[t]++;
                        }
                        // source rank
                        int s = 0;
                        if (d > 0)
                        {
                            s = (8 * (B->px[c][p] - mn) + d / 2) / d;
                            if (s > 7) s = 7;
                        }
                        S.h_s[s]++;
                        if (d >= 8)
                        {
                            S.qtot8[c]++;
                            if (i == s2idx[s])
                                S.qmatch8[c]++;
                        }
                        // rank deltas vs left pixel
                        uint32_t r = p >> 2, col = p & 3;
                        if (col > 0)
                        {
                            int sn = 0;
                            if (d > 0)
                            {
                                sn = (8 * (B->px[c][p - 1] - mn) + d / 2) / d;
                                if (sn > 7) sn = 7;
                            }
                            S.h_sdiff_same[sn][(s - sn) & 7]++;
                        }
                        else if (left)
                        {
                            int mnl = left->ep[c][1];
                            int vnl = left->px[c][3 * 4 + r];
                            int sp = 0;
                            if (d > 0)
                            {
                                sp = (8 * (vnl - mn) + d / 2) / d;
                                if (sp > 7) sp = 7;
                                if (sp < 0) sp = 0;
                            }
                            (void)mnl;
                            S.h_sdiff_adj[sp][(s - sp) & 7]++;
                        }
                    }
                    if (all_same)
                    {
                        S.n_const[c]++;
                        S.h_const[c][B->idx[c][0]]++;
                    }

                    // within-block neighbor joints
                    for (uint32_t r = 0; r < 4; ++r)
                        for (uint32_t col = 0; col < 4; ++col)
                        {
                            uint32_t p = r * 4 + col;
                            if (col < 3)
                            {
                                S.h_horiz[B->idx[c][p]][B->idx[c][p + 1]]++;
                                S.h_horiz_db[db][B->idx[c][p]][B->idx[c][p + 1]]++;
                            }
                            if (r < 3)
                            {
                                S.h_vert[B->idx[c][p]][B->idx[c][p + 4]]++;
                                S.h_vert_db[db][B->idx[c][p]][B->idx[c][p + 4]]++;
                            }
                        }

                    // 12-bit rows: row r = the r-th 12-bit slice of the 48-bit block (4 raster indices)
                    uint32_t rws[4];
                    for (uint32_t r = 0; r < 4; ++r)
                    {
                        uint32_t row = 0;
                        for (uint32_t i = 0; i < 4; ++i)
                            row |= (uint32_t)B->idx[c][r * 4 + i] << (3 * i);
                        rws[r] = row;
                        S.h_row[c][row]++;
                    }
                    for (uint32_t r = 1; r < 4; ++r)
                    {
                        S.h_rowd[c][(rws[r] - rws[r - 1]) & 4095]++;
                        S.h_rowx[c][rws[r] ^ rws[r - 1]]++;
                    }
                    // row stream adjacency in raster order: row r-1 of this block, or row 3 of the block above
                    for (uint32_t r = 0; r < 4; ++r)
                    {
                        uint32_t prevs = 0;
                        if (r > 0)
                            prevs = rws[r - 1];
                        else if (top)
                            for (uint32_t i = 0; i < 4; ++i)
                                prevs |= (uint32_t)top->idx[c][3 * 4 + i] << (3 * i);
                        else
                            continue;
                        S.n_rowseq[c]++;
                        if (prevs == rws[r])
                            S.n_rowseq_eq[c]++;
                    }

                    // zigzag single chain
                    uint8_t prev = 0;
                    for (uint32_t k = 0; k < 16; ++k)
                    {
                        uint8_t i = B->idx[c][zz[k]];
                        if (k == 0)
                            S.h_first1[c][i]++;
                        else
                        {
                            S.h_sx[c][prev][i ^ prev]++;
                            S.h_sar[c][prev][(i - prev) & 7]++;
                            S.h_sar_db[c][prev * 8 + db][(i - prev) & 7]++;
                        }
                        prev = i;
                    }

                    // pairs
                    uint32_t prevp = 0;
                    uint64_t val48 = 0;
                    for (uint32_t p = 0; p < 8; ++p)
                    {
                        uint32_t a = B->idx[c][zz[2 * p]];
                        uint32_t b = B->idx[c][zz[2 * p + 1]];
                        uint32_t pair = (a << 3) | b;
                        val48 |= (uint64_t)pair << (p * 6);
                        S.h_pair[c][pair]++;
                        if (p == 0)
                            S.h_first[c][pair]++;
                        else
                        {
                            S.h_pctx[c][prevp & 7][pair ^ prevp]++;
                            S.h_par[c][prevp & 7][(pair - prevp) & 63]++;
                            S.h_pctxd[c][db][pair ^ prevp]++;
                        }
                        prevp = pair;
                    }
                    if (c == 1)
                    {
                        uint64_t valr = 0;
                        for (uint32_t p = 0; p < 8; ++p)
                        {
                            uint32_t a = B->idx[0][zz[2 * p]];
                            uint32_t b = B->idx[0][zz[2 * p + 1]];
                            valr |= (uint64_t)((a << 3) | b) << (p * 6);
                        }
                        int pc = 0;
                        uint64_t x = valr ^ val48;
                        while (x)
                        {
                            pc += x & 1;
                            x >>= 1;
                        }
                        int bkt = pc == 0 ? 0 : pc <= 2 ? 1 : pc <= 5 ? 2 : pc <= 11 ? 3 : pc <= 23 ? 4 : pc <= 35 ? 5 : pc <= 47 ? 6 : 7;
                        S.h_48xor[bkt]++;
                    }

                    // spatial prediction, per pixel (left-preferred)
                    for (uint32_t r = 0; r < 4; ++r)
                        for (uint32_t col = 0; col < 4; ++col)
                        {
                            uint32_t p = r * 4 + col;
                            uint8_t i = B->idx[c][p];
                            if (col > 0)
                            {
                                uint8_t pn = B->idx[c][p - 1];
                                S.h_sp_same[pn][(i - pn) & 7]++;
                            }
                            else if (left)
                            {
                                uint8_t pn = left->idx[c][3 * 4 + r];
                                S.h_sp_adj[pn][(i - pn) & 7]++;
                            }
                            if (col == 0 && r > 0)
                            {
                                uint8_t pn = B->idx[c][p - 4];
                                S.h_sp_same[pn][(i - pn) & 7]++;
                            }
                            else if (col == 0 && r == 0 && top)
                            {
                                uint8_t pn = top->idx[c][col];
                                S.h_sp_adj[pn][(i - pn) & 7]++;
                            }
                            if (left)
                            {
                                uint8_t pn = left->idx[c][p];
                                S.h_grid_l[pn][(i - pn) & 7]++;
                            }
                            if (top)
                            {
                                uint8_t pn = top->idx[c][p];
                                S.h_grid_t[pn][(i - pn) & 7]++;
                            }
                        }

                    // 2D in-block predictor: left, else up (15/16 pixels), ctx = pred
                    for (uint32_t r = 0; r < 4; ++r)
                        for (uint32_t col = 0; col < 4; ++col)
                        {
                            uint32_t p = r * 4 + col;
                            uint8_t i = B->idx[c][p];
                            uint8_t pred = 0;
                            if (col > 0)
                                pred = B->idx[c][p - 1];
                            else if (r > 0)
                                pred = B->idx[c][p - 4];
                            else
                                continue;
                            S.h_2d_same[pred][(i - pred) & 7]++;
                            S.h_2d_db[c][pred * 8 + db][(i - pred) & 7]++;
                        }
                    if (c == 1)
                        for (uint32_t r = 0; r < 4; ++r)
                            for (uint32_t col = 0; col < 4; ++col)
                            {
                                uint32_t p = r * 4 + col;
                                uint8_t g = B->idx[1][p];
                                int vr = spec_pal(B->ep[0][0], B->ep[0][1], B->idx[0][p]);
                                int mxg = B->ep[1][0], mng = B->ep[1][1], dg = mxg - mng;
                                int sp = 1;
                                if (dg > 0)
                                {
                                    sp = (8 * (vr - mng) + dg / 2) / dg;
                                    if (sp > 7) sp = 7;
                                    if (sp < 0) sp = 0;
                                }
                                uint8_t pred = s2idx[sp];
                                S.h_rearg[pred][(g - pred) & 7]++;
                            }

                    // re-anchored value prediction, same sample selection
                    for (uint32_t r = 0; r < 4; ++r)
                        for (uint32_t col = 0; col < 4; ++col)
                        {
                            uint32_t p = r * 4 + col;
                            uint8_t i = B->idx[c][p];
                            const block_t* nb = NULL;
                            uint32_t np = 0;
                            int sameblk = 0;
                            if (col > 0)
                            {
                                nb = B;
                                np = p - 1;
                                sameblk = 1;
                            }
                            else if (left)
                            {
                                nb = left;
                                np = 3 * 4 + r;
                            }
                            else if (r > 0)
                            {
                                nb = B;
                                np = p - 4;
                                sameblk = 1;
                            }
                            else if (top)
                            {
                                nb = top;
                                np = col;
                            }
                            if (!nb)
                                continue;
                            int vn = spec_pal(nb->ep[c][0], nb->ep[c][1], nb->idx[c][np]);
                            int sp = 1;
                            if (d > 0)
                            {
                                sp = (8 * (vn - mn) + d / 2) / d;
                                if (sp > 7) sp = 7;
                                if (sp < 0) sp = 0;
                            }
                            uint8_t pred = s2idx[sp];
                            if (sameblk)
                                S.h_rea_same[pred][(i - pred) & 7]++;
                            else
                                S.h_rea_adj[pred][(i - pred) & 7]++;
                            S.h_rea_db[c][db][(i - pred) & 7]++;
                        }
                }

                // red x green per pixel and per pair
                for (uint32_t p = 0; p < 16; ++p)
                {
                    uint8_t r = B->idx[0][p];
                    uint8_t g = B->idx[1][p];
                    int drb = dbucket(B->ep[0][0] - B->ep[0][1]);
                    int dgb = dbucket(B->ep[1][0] - B->ep[1][1]);
                    S.h_rg[r][g]++;
                    S.h_rgdiff[(int)r - (int)g + 7]++;
                    S.h_rgdb[drb][dgb][r][g]++;
                    S.h_rgpred[drb * 8 + dgb][(g - r) & 7]++;
                    pear_add(&S.ps_raw, B->px[0][p], B->px[1][p]);
                }
                for (uint32_t p = 0; p < 8; ++p)
                {
                    uint32_t pr = ((uint32_t)B->idx[0][zz[2 * p]] << 3) | B->idx[0][zz[2 * p + 1]];
                    uint32_t pg = ((uint32_t)B->idx[1][zz[2 * p]] << 3) | B->idx[1][zz[2 * p + 1]];
                    S.h_rgp[pr][pg]++;
                }
            }

        // ---- exact compressor endpoint predictors (64 strips, zigzag; x passed = zigzag index)
        {
            uint32_t num_strips = 64;
            uint32_t strip_h = hb / num_strips;
            uint8_t seed[2][2];
            memset(seed, 0, sizeof(seed));
            for (uint32_t s = 0; s < num_strips; ++s)
            {
                uint32_t y0 = s * strip_h;
                if (y0 >= hb)
                    break;
                uint32_t x0 = (y0 & 1) ? 0 : wb - 1; // first block of the strip in chain order
                for (uint32_t c = 0; c < 2; ++c)
                    for (uint32_t j = 0; j < 2; ++j)
                        seed[c][j] += blocks[y0 * wb + x0].ep[c][j];
            }
            for (uint32_t c = 0; c < 2; ++c)
                for (uint32_t j = 0; j < 2; ++j)
                    seed[c][j] /= num_strips;

            for (uint32_t s = 0; s < num_strips; ++s)
            {
                uint32_t y0 = s * strip_h;
                if (y0 >= hb)
                    break;
                const block_t* chain_prev = NULL;
                for (uint32_t y = y0; y < y0 + strip_h && y < hb; ++y)
                {
                    for (uint32_t xi = 0; xi < wb; ++xi)
                    {
                        uint32_t x = (y & 1) ? xi : wb - 1 - xi;
                        block_t* B = &blocks[y * wb + x];
                        for (uint32_t c = 0; c < 2; ++c)
                            for (uint32_t j = 0; j < 2; ++j)
                            {
                                if (y == y0)
                                {
                                    const uint8_t* ref = (xi == 0) ? &seed[c][j] : &chain_prev->ep[c][j];
                                    S.h_epd_chain[c][(uint8_t)(B->ep[c][j] - *ref)]++;
                                }
                                else
                                {
                                    // exact copy of bc5_predict_endpoints (x there is the zigzag index)
                                    uint32_t zigzag_x = (y & 1) ? xi : wb - xi - 1;
                                    const block_t* topb = &blocks[(y - 1) * wb + zigzag_x];
                                    uint32_t previous_phys = (xi > 0) ? ((y & 1) ? xi - 1 : wb - xi)
                                                                       : (((y - 1) & 1) ? wb - 1 : 0);
                                    uint32_t previous_row = (xi > 0) ? y : y - 1;
                                    const block_t* above = (previous_row == y0) ? NULL : &blocks[(previous_row - 1) * wb + previous_phys];
                                    uint8_t prev_e = chain_prev ? chain_prev->ep[c][j] : seed[c][j];
                                    uint8_t above_e = above ? above->ep[c][j] : seed[c][j];
                                    uint8_t pred = (uint8_t)(prev_e + topb->ep[c][j] - above_e);
                                    S.h_epd_extrap[c][(uint8_t)(B->ep[c][j] - pred)]++;
                                }
                            }
                        chain_prev = B;
                    }
                }
            }
        }

        // ---- per-image output
        printf("== %s  (%" PRIu32 "x%" PRIu32 ", %" PRIu64 " blocks)\n", files[f], w, h, S.nblk);

        // ground truth: the real compressor
        {
            uint8_t* packed = malloc(wb * hb * 16);
            for (uint32_t bi = 0; bi < wb * hb; ++bi)
            {
                block_t* B = &blocks[bi];
                for (uint32_t c = 0; c < 2; ++c)
                {
                    packed[bi * 16 + c * 8] = B->ep[c][0];
                    packed[bi * 16 + c * 8 + 1] = B->ep[c][1];
                    uint64_t v = 0;
                    for (uint32_t p = 0; p < 16; ++p)
                        v |= (uint64_t)B->idx[c][p] << (p * 3);
                    for (int b = 0; b < 6; ++b)
                        packed[bi * 16 + c * 8 + 2 + b] = (uint8_t)(v >> (b * 8));
                }
            }
            size_t cmax = packed_bc5_maxsize(w, h);
            uint8_t* cbuf = malloc(cmax);
            uint32_t nstrips = 0;
            size_t clen = bcp_compress_bc5(ctx, packed, w, h, cbuf, cmax, &nstrips);
            if (clen > 0)
            {
                bcp_stats_bc5 st;
                bcp_get_stats_bc5(ctx, &st);
                uint64_t n16 = 16 * S.nblk;
                printf("  bcp_compress_bc5: stream %zu bytes, ratio %.4f:1 ; endpoint %.3f bits/sym, pair %.3f bits/idx, dict %" PRIu32 " bytes\n",
                       clen, 16.0 * (double)S.nblk / (double)clen,
                       8.0 * (double)st.endpoint_bytes / (double)(4 * S.nblk),
                       8.0 * (double)st.pair_bytes / (double)n16, st.dict_bytes);
            }
            else
            {
                printf("  bcp_compress_bc5 FAILED\n");
            }
            free(cbuf);
            free(packed);
        }

        model_row rows[24];
        int n = build_rows(&S, rows);
        model_table("  ", rows, n);
        {
            double Hs = h_dist(&S.h_s[0], 8);
            double Hsd1 = h_cond(&S.h_sdiff_same[0][0], 8, 8);
            double Hsd2 = h_cond(&S.h_sdiff_adj[0][0], 8, 8);
            printf("  %-30s %8.4f   (source rank s, bits/index)\n", "rank s | left same-block", Hsd1);
            printf("  %-30s %8.4f   (source rank s, bits/index)\n", "rank s | left re-anchored", Hsd2);
            printf("  %-30s %8.4f   (source rank s, bits/index)\n", "raw source rank s", Hs);
            double e = (h_dist(&S.h_epd_chain[0][0], 256) + h_dist(&S.h_epd_chain[1][0], 256) +
                        h_dist(&S.h_epd_extrap[0][0], 256) + h_dist(&S.h_epd_extrap[1][0], 256)) / 4.0;
            printf("  endpoint deltas (exact compressor predictor): %.4f bits/symbol (4 symbols/block)\n", e);
            for (int c = 0; c < 2; ++c)
            {
                const char* cn = c == 0 ? "red" : "grn";
                uint64_t tot = hist_sum(&S.h_row[c][0], 4096);
                uint32_t distinct = 0, topv = 0, topc = 0;
                for (int v = 0; v < 4096; ++v)
                {
                    if (S.h_row[c][v] > 0)
                        distinct++;
                    if (S.h_row[c][v] > topc)
                    {
                        topc = S.h_row[c][v];
                        topv = (uint32_t)v;
                    }
                }
                printf("  %s 12-bit rows: H=%.4f  distinct=%u/4096  repeat=%.1f%%  top1=0x%03x (%.2f%%)  seq-eq=%.2f%%\n",
                       cn, h_dist(&S.h_row[c][0], 4096), distinct,
                       100.0 * (1.0 - (double)distinct / (double)tot), topv, 100.0 * (double)topc / (double)tot,
                       S.n_rowseq[c] ? 100.0 * (double)S.n_rowseq_eq[c] / (double)S.n_rowseq[c] : 0.0);
            }
        }

        // ---- integrity checks (catch silent histogram corruption)
        {
            int bad = 0;
            for (int c = 0; c < 2; ++c)
            {
                if (hist_sum(&S.h_idx[c][0], 8) != 16 * S.nblk) { printf("  CHECK FAIL h_idx[%d]\n", c); bad = 1; }
                if (hist_sum(&S.h_pair[c][0], 64) != 8 * S.nblk) { printf("  CHECK FAIL h_pair[%d]\n", c); bad = 1; }
                if (hist_sum(&S.h_first[c][0], 64) != S.nblk) { printf("  CHECK FAIL h_first[%d]\n", c); bad = 1; }
                if (hist_sum(&S.h_idx_db[c][0][0], 64) != 16 * S.nblk) { printf("  CHECK FAIL h_idx_db[%d]\n", c); bad = 1; }
                if (hist_sum(&S.h_d[c][0], 256) != S.nblk) { printf("  CHECK FAIL h_d[%d]\n", c); bad = 1; }
                if (S.qnpix[c] != 16 * S.nblk) { printf("  CHECK FAIL qnpix[%d]\n", c); bad = 1; }
                if (hist_sum(&S.h_row[c][0], 4096) != 4 * S.nblk) { printf("  CHECK FAIL h_row[%d]\n", c); bad = 1; }
                if (hist_sum(&S.h_rowd[c][0], 4096) != 3 * S.nblk) { printf("  CHECK FAIL h_rowd[%d]\n", c); bad = 1; }
            }
            if (hist_sum(&S.h_s[0], 8) != 32 * S.nblk) { printf("  CHECK FAIL h_s\n"); bad = 1; }
            if (hist_sum(&S.h_rg[0][0], 64) != 16 * S.nblk) { printf("  CHECK FAIL h_rg\n"); bad = 1; }
            if (hist_sum(&S.h_horiz[0][0], 64) != 24 * S.nblk) { printf("  CHECK FAIL h_horiz\n"); bad = 1; }
            if (hist_sum(&S.h_vert[0][0], 64) != 24 * S.nblk) { printf("  CHECK FAIL h_vert\n"); bad = 1; }
            if (!bad)
                printf("  integrity checks: OK\n");
        }

        memcpy(&file_hrow[f], S.h_row, sizeof(file_hrow[f]));
        merge_stats(&global, &S);
        stbi_image_free(rgba);
        free(blocks);
    }

    if (ctx)
        bcp_terminate(ctx);

    // ================= aggregate detailed output =================
    printf("\n\n================ AGGREGATE (%" PRIu64 " blocks, all 4 images) ================\n", global.nblk);

    // ---- stb idx-7 behavior
    printf("\n-- stb BC4 idx-7 behavior (D3D spec: idx 7 = ep0 = max endpoint)\n");
    for (int c = 0; c < 2; ++c)
    {
        const char* cn = c == 0 ? "red" : "grn";
        double avgerr = global.qnpix[c] ? (double)global.qerrsum[c] / (double)global.qnpix[c] : 0;
        printf("  %s: idx7 pixels %" PRIu32 " (%.3f%%), of which near-min %" PRIu32 " (%.1f%%) / near-max %" PRIu32 " (%.1f%%); "
               "idx == s2idx[rank] on d>=8: %.1f%%; mean |decode-src| = %.3f\n",
               cn, global.q7[c], 100.0 * (double)global.q7[c] / (double)global.qnpix[c],
               global.q7min[c], global.q7[c] ? 100.0 * (double)global.q7min[c] / (double)global.q7[c] : 0,
               global.q7max[c], global.q7[c] ? 100.0 * (double)global.q7max[c] / (double)global.q7[c] : 0,
               global.qtot8[c] ? 100.0 * (double)global.qmatch8[c] / (double)global.qtot8[c] : 0, avgerr);
    }
    {
        uint64_t t7 = 0;
        for (int i = 0; i < 16; ++i)
            t7 += global.h_7band[i];
        printf("  position of idx7 pixels within the block range, t = (v-mn)*16/d (16 bins, both channels):\n   ");
        for (int i = 0; i < 16; ++i)
            printf("%4.1f%%", 100.0 * (double)global.h_7band[i] / (double)t7);
        printf("\n   (bin k covers the range fraction [k/16, (k+1)/16) above the block minimum)\n");
    }

    // ---- endpoints
    printf("\n-- endpoints\n");
    for (int c = 0; c < 2; ++c)
    {
        const char* cn = c == 0 ? "red" : "grn";
        uint64_t tot = hist_sum(&global.h_d[c][0], 256);
        printf("  %s range d:  d=0:%5.1f%%  1-3:%5.1f%%  4-7:%5.1f%%  8-15:%5.1f%%  16-63:%5.1f%%  >=64:%5.1f%%   H(d)=%.3f\n",
               cn,
               100.0 * (double)global.h_d[c][0] / (double)tot,
               100.0 * (double)(global.h_d[c][1] + global.h_d[c][2] + global.h_d[c][3]) / (double)tot,
               100.0 * (double)hist_sum(&global.h_d[c][4], 4) / (double)tot,
               100.0 * (double)hist_sum(&global.h_d[c][8], 8) / (double)tot,
               100.0 * (double)hist_sum(&global.h_d[c][16], 48) / (double)tot,
               100.0 * (double)hist_sum(&global.h_d[c][64], 192) / (double)tot,
               h_dist(&global.h_d[c][0], 256));
        uint64_t tm = hist_sum(&global.h_mid[c][0], 32);
        printf("  %s midpoint:  <64:%5.1f%%  64-127:%5.1f%%  128-191:%5.1f%%  >191:%5.1f%%\n", cn,
               100.0 * (double)hist_sum(&global.h_mid[c][0], 8) / (double)tm,
               100.0 * (double)hist_sum(&global.h_mid[c][8], 8) / (double)tm,
               100.0 * (double)hist_sum(&global.h_mid[c][16], 8) / (double)tm,
               100.0 * (double)hist_sum(&global.h_mid[c][24], 8) / (double)tm);
    }
    printf("  red-green:  corr(d_r,d_g)=%.4f   corr(mid_r,mid_g)=%.4f   corr(mid_r-128,mid_g-128)=%.4f\n",
           pear_get(&global.ps_dd), pear_get(&global.ps_mm), pear_get(&global.ps_mid));
    printf("  raw pixel values: corr(R,G)=%.4f  (%.0f pixel samples)\n", pear_get(&global.ps_raw), global.ps_raw.n);
    printf("  delta vs left block (both endpoints pooled):  red %.3f | grn %.3f bits/symbol\n",
           h_dist(&global.h_epd_left[0][0], 256), h_dist(&global.h_epd_left[1][0], 256));
    printf("  delta vs top block:                            red %.3f | grn %.3f bits/symbol\n",
           h_dist(&global.h_epd_top[0][0], 256), h_dist(&global.h_epd_top[1][0], 256));
    printf("  delta vs 2D extrap (raster):                 red %.3f | grn %.3f bits/symbol\n",
           h_dist(&global.h_epd_2d[0][0], 256), h_dist(&global.h_epd_2d[1][0], 256));
    printf("  exact compressor chain (strip row 0):        red %.3f | grn %.3f bits/symbol\n",
           h_dist(&global.h_epd_chain[0][0], 256), h_dist(&global.h_epd_chain[1][0], 256));
    printf("  exact compressor 2D extrap (later rows):     red %.3f | grn %.3f bits/symbol\n",
           h_dist(&global.h_epd_extrap[0][0], 256), h_dist(&global.h_epd_extrap[1][0], 256));

    printf("\n  d-bucket joint (red row x green col), %% of blocks:\n     ");
    for (int b = 0; b < 8; ++b)
        printf("%8s", dbname[b]);
    printf("\n");
    uint64_t totd = hist_sum(&global.h_drg[0][0], 64);
    for (int a = 0; a < 8; ++a)
    {
        printf("  %s:", dbname[a]);
        for (int b = 0; b < 8; ++b)
            printf(" %5.1f%%", 100.0 * (double)global.h_drg[a][b] / (double)totd);
        printf("\n");
    }

    printf("\n  midpoint difference (mid_r - mid_g)/4 histogram, -8..8:\n   ");
    for (int i = 0; i < 17; ++i)
        printf("%6d", i - 8);
    printf("\n   ");
    for (int i = 0; i < 17; ++i)
        printf("%6u", global.h_midrg[i]);
    printf("\n");

    // ---- single index
    printf("\n-- single 3-bit index\n");
    for (int c = 0; c < 2; ++c)
    {
        const char* cn = c == 0 ? "red" : "grn";
        uint64_t tot = hist_sum(&global.h_idx[c][0], 8);
        printf("  %s histogram:  ", cn);
        for (int i = 0; i < 8; ++i)
            printf("%" PRIu32 "(%.1f%%) ", global.h_idx[c][i], 100.0 * (double)global.h_idx[c][i] / (double)tot);
        printf("H = %.4f bits\n", h_dist(&global.h_idx[c][0], 8));
        uint64_t ctot = 0;
        for (int i = 0; i < 8; ++i)
            ctot += global.h_const[c][i];
        printf("  %s constant blocks: %.2f%% of all; value dist: ", cn, 100.0 * (double)ctot / (double)global.nblk);
        for (int i = 0; i < 8; ++i)
            if (global.h_const[c][i] > 0)
                printf("%u:%.1f%% ", i, 100.0 * (double)global.h_const[c][i] / (double)ctot);
        printf("\n");
    }
    printf("  mean index per raster position (0 = top-left, row major):\n   red:");
    for (int p = 0; p < 16; ++p)
        printf(" %5.3f", (double)global.s_idxpos[0][p] / (double)(16 * global.nblk));
    printf("\n   grn:");
    for (int p = 0; p < 16; ++p)
        printf(" %5.3f", (double)global.s_idxpos[1][p] / (double)(16 * global.nblk));
    printf("\n");

    printf("  H(idx | range bucket) per bucket (%% mass, H bits):\n   range:   ");
    for (int b = 0; b < 8; ++b)
        printf("%12s", dbname[b]);
    printf("\n   red  :   ");
    uint64_t ti0 = hist_sum(&global.h_idx_db[0][0][0], 64);
    for (int b = 0; b < 8; ++b)
        printf(" %5.1f%% %5.3f", 100.0 * (double)hist_sum(&global.h_idx_db[0][b][0], 8) / (double)ti0, h_cond(&global.h_idx_db[0][b][0], 1, 8));
    printf("\n   grn  :   ");
    uint64_t ti1 = hist_sum(&global.h_idx_db[1][0][0], 64);
    for (int b = 0; b < 8; ++b)
        printf(" %5.1f%% %5.3f", 100.0 * (double)hist_sum(&global.h_idx_db[1][b][0], 8) / (double)ti1, h_cond(&global.h_idx_db[1][b][0], 1, 8));
    printf("\n   pooled:  red %.4f   grn %.4f   (unconditional: red %.4f grn %.4f)\n",
           h_cond(&global.h_idx_db[0][0][0], 8, 8), h_cond(&global.h_idx_db[1][0][0], 8, 8),
           h_dist(&global.h_idx[0][0], 8), h_dist(&global.h_idx[1][0], 8));
    printf("   H(idx | range, mid):  red %.4f   grn %.4f\n",
           h_cond(&global.h_idx_dbmid[0][0][0][0], 64, 8), h_cond(&global.h_idx_dbmid[1][0][0][0], 64, 8));

    for (int c = 0; c < 2; ++c)
    {
        const char* cn = c == 0 ? "red" : "grn";
        printf("  %s idx histogram per range bucket (%% within bucket):\n   range:  ", cn);
        for (int i = 0; i < 8; ++i)
            printf("%3d ", i);
        printf("\n");
        for (int b = 0; b < 8; ++b)
        {
            uint64_t tb = hist_sum(&global.h_idx_db[c][b][0], 8);
            printf("   %-8s", dbname[b]);
            for (int i = 0; i < 8; ++i)
                printf(" %4.1f%%", tb ? 100.0 * (double)global.h_idx_db[c][b][i] / (double)tb : 0);
            printf("\n");
        }
    }

    // ---- within-block joints
    printf("\n-- within-block index correlation (same channel, both channels pooled)\n");
    {
        uint64_t th = hist_sum(&global.h_horiz[0][0], 64);
        uint64_t tv = hist_sum(&global.h_vert[0][0], 64);
        uint64_t eqh = 0, eqv = 0;
        for (int i = 0; i < 8; ++i)
        {
            eqh += global.h_horiz[i][i];
            eqv += global.h_vert[i][i];
        }
        printf("  horizontal neighbor: I=%.4f bits  corr=%.4f  equal=%.1f%%  H(2nd|1st)=%.4f\n",
               mi_2d(&global.h_horiz[0][0], 8, 8), joint_corr(&global.h_horiz[0][0], 8, 8),
               100.0 * (double)eqh / (double)th, h_cond(&global.h_horiz[0][0], 8, 8));
        printf("  vertical   neighbor: I=%.4f bits  corr=%.4f  equal=%.1f%%  H(2nd|1st)=%.4f\n",
               mi_2d(&global.h_vert[0][0], 8, 8), joint_corr(&global.h_vert[0][0], 8, 8),
               100.0 * (double)eqv / (double)tv, h_cond(&global.h_vert[0][0], 8, 8));
        print_8x8("horizontal joint", &global.h_horiz[0][0]);
        print_8x8("vertical joint", &global.h_vert[0][0]);
        printf("  I by own range bucket (horiz / vert):\n   range:   ");
        for (int b = 0; b < 8; ++b)
            printf("%10s", dbname[b]);
        printf("\n   ");
        for (int b = 0; b < 8; ++b)
            printf(" %6.3f/%5.3f", mi_2d(&global.h_horiz_db[b][0][0], 8, 8), mi_2d(&global.h_vert_db[b][0][0], 8, 8));
        printf("\n");
    }

    // ---- spatial
    printf("\n-- spatial prediction (delta mod 8 vs predictor; ctx = predictor value)\n");
    {
        uint64_t ns = hist_sum(&global.h_sp_same[0][0], 64);
        uint64_t na = hist_sum(&global.h_sp_adj[0][0], 64);
        double Hs_s = h_cond(&global.h_sp_same[0][0], 8, 8), Hs_a = h_cond(&global.h_sp_adj[0][0], 8, 8);
        double Hs = ns + na ? (Hs_s * (double)ns + Hs_a * (double)na) / (double)(ns + na) : 0;
        uint64_t eq = 0;
        for (int v = 0; v < 8; ++v)
            eq += global.h_sp_same[v][0] + global.h_sp_adj[v][0];
        printf("  image-adjacent: same-block samples %" PRIu64 ", neighbor-block %" PRIu64 "\n", ns, na);
        printf("  delta entropy: same-block %.4f, neighbor-block %.4f, pooled %.4f bits; delta==0: %.1f%%\n",
               Hs_s, Hs_a, Hs, 100.0 * (double)eq / (double)(ns + na));
        printf("  pooled delta distribution:  ");
        for (int v = 0; v < 8; ++v)
        {
            uint64_t t = 0;
            for (int a = 0; a < 8; ++a)
                t += global.h_sp_same[a][v] + global.h_sp_adj[a][v];
            printf("%+d:%5.1f%% ", v - 4, 100.0 * (double)t / (double)(ns + na));
        }
        printf("\n");
        uint64_t nr = hist_sum(&global.h_rea_same[0][0], 64) + hist_sum(&global.h_rea_adj[0][0], 64);
        double Hr_s = h_cond(&global.h_rea_same[0][0], 8, 8), Hr_a = h_cond(&global.h_rea_adj[0][0], 8, 8);
        uint64_t nr_s = hist_sum(&global.h_rea_same[0][0], 64);
        double Hr = nr ? (Hr_s * (double)nr_s + Hr_a * (double)(nr - nr_s)) / (double)nr : 0;
        uint64_t eqr = 0;
        for (int v = 0; v < 8; ++v)
            eqr += global.h_rea_same[v][0] + global.h_rea_adj[v][0];
        printf("  re-anchored value: pooled %.4f bits (same %.4f / neighbor %.4f); delta==0: %.1f%%\n", Hr, Hr_s, Hr_a, 100.0 * (double)eqr / (double)nr);
        uint64_t ng = hist_sum(&global.h_grid_l[0][0], 64) + hist_sum(&global.h_grid_t[0][0], 64);
        double Hg_l = h_cond(&global.h_grid_l[0][0], 8, 8), Hg_t = h_cond(&global.h_grid_t[0][0], 8, 8);
        uint64_t ngl = hist_sum(&global.h_grid_l[0][0], 64);
        double Hg = ng ? (Hg_l * (double)ngl + Hg_t * (double)(ng - ngl)) / (double)ng : 0;
        uint64_t eqg = 0;
        for (int v = 0; v < 8; ++v)
            eqg += global.h_grid_l[v][0] + global.h_grid_t[v][0];
        printf("  grid-identical neighbor block: pooled %.4f bits (left %.4f / top %.4f); delta==0: %.1f%%\n", Hg, Hg_l, Hg_t, 100.0 * (double)eqg / (double)ng);
        printf("  grid-identical delta distribution:  ");
        for (int v = 0; v < 8; ++v)
        {
            uint64_t t = 0;
            for (int a = 0; a < 8; ++a)
                t += global.h_grid_l[a][v] + global.h_grid_t[a][v];
            printf("%+d:%5.1f%% ", v - 4, 100.0 * (double)t / (double)ng);
        }
        printf("\n");
    }

    // ---- red x green
    printf("\n-- red x green indices, same pixel\n");
    {
        uint64_t tot = hist_sum(&global.h_rg[0][0], 64);
        print_8x8("joint (red row, green col)", &global.h_rg[0][0]);
        double Hmi = mi_2d(&global.h_rg[0][0], 8, 8);
        double Hr = h_dist(&global.h_idx[0][0], 8), Hg = h_dist(&global.h_idx[1][0], 8);
        double Heq = 0;
        for (int i = 0; i < 8; ++i)
            Heq += global.h_rg[i][i];
        printf("  H(red)=%.4f H(green)=%.4f  I(red;green)=%.4f bits  corr=%.4f  equal: %.1f%%\n",
               Hr, Hg, Hmi, joint_corr(&global.h_rg[0][0], 8, 8), 100.0 * (double)Heq / (double)tot);
        printf("  H(green | red) = %.4f bits (vs H(green) = %.4f)\n", h_cond(&global.h_rg[0][0], 8, 8), Hg);
        printf("  red - green difference histogram:\n   ");
        for (int i = 0; i < 15; ++i)
            printf("%+d:%5.1f%% ", i - 7, 100.0 * (double)global.h_rgdiff[i] / (double)tot);
        printf("\n");
        printf("  H(green | red, range buckets) pooled over 64 ctx: %.4f bits; per (drb,dgb) with >1%% mass:\n",
               h_cond(&global.h_rgpred[0][0], 64, 8));
        uint64_t totp = 0;
        for (int a = 0; a < 64; ++a)
            for (int b = 0; b < 8; ++b)
                totp += global.h_rgpred[a][b];
        for (int a = 0; a < 64; ++a)
        {
            uint64_t ta = hist_sum(&global.h_rgpred[a][0], 8);
            if (ta * 100 < totp)
                continue;
            printf("    drb=%d dgb=%d: H(green-red mod8 | ctx) = %.4f bits (%.2f%% of samples)\n",
                   a >> 3, a & 7, h_cond(&global.h_rgpred[a][0], 1, 8), 100.0 * (double)ta / (double)totp);
        }
        // per (drb,dgb) conditional H(green | red)
        printf("  H(green | red) by (drb, dgb) [drb row, dgb col]:\n        ");
        for (int b = 0; b < 8; ++b)
            printf("%8s", dbname[b]);
        printf("\n");
        for (int a = 0; a < 8; ++a)
        {
            printf("  %s:", dbname[a]);
            for (int b = 0; b < 8; ++b)
                printf(" %5.3f", h_cond(&global.h_rgdb[a][b][0][0], 8, 8));
            printf("\n");
        }
        double mip = mi_2d(&global.h_rgp[0][0], 64, 64);
        double Hpr = h_dist(&global.h_pair[0][0], 64), Hpg = h_dist(&global.h_pair[1][0], 64);
        printf("  pair level: H(red pair)=%.4f H(green pair)=%.4f  I(red pair;green pair)=%.4f bits/pair\n", Hpr, Hpg, mip);
        printf("  48-bit red^green popcount buckets {0,1-2,3-5,6-11,12-23,24-35,36-47,48}:\n   ");
        uint64_t t48 = 0;
        for (int i = 0; i < 8; ++i)
            t48 += global.h_48xor[i];
        const char* b48[] = { "0", "1-2", "3-5", "6-11", "12-23", "24-35", "36-47", "48" };
        for (int i = 0; i < 8; ++i)
            printf("%s:%5.1f%% ", b48[i], 100.0 * (double)global.h_48xor[i] / (double)t48);
        printf("\n");
    }

    // ---- source rank
    {
        printf("\n-- source value rank s = round(8*(v-mn)/d), both channels pooled\n");
        uint64_t tot = hist_sum(&global.h_s[0], 8);
        printf("  histogram:  ");
        for (int i = 0; i < 8; ++i)
            printf("%" PRIu32 ":%5.1f%% ", global.h_s[i], 100.0 * (double)global.h_s[i] / (double)tot);
        printf("H(s)=%.4f\n", h_dist(&global.h_s[0], 8));
        printf("  H(s | s left same-block) = %.4f,  H(s | s left re-anchored) = %.4f bits\n",
               h_cond(&global.h_sdiff_same[0][0], 8, 8), h_cond(&global.h_sdiff_adj[0][0], 8, 8));
    }

    // ---- aggregate model table
    printf("\n-- aggregate model comparison (bits/index, both channels)\n");
    {
        model_row rows[24];
        int n = build_rows(&global, rows);
        model_table("  ", rows, n);
    }

    // ---- 12-bit row top table (does a most-used-rows table beat the raw 12 bits?)
    {
        static uint32_t hp[4096];
        static uint32_t pf[4][4096];
        for (int v = 0; v < 4096; ++v)
        {
            hp[v] = global.h_row[0][v] + global.h_row[1][v];
            for (int f = 0; f < 4; ++f)
                pf[f][v] = file_hrow[f][0][v] + file_hrow[f][1][v];
        }
        uint64_t tt = hist_sum(hp, 4096);

        printf("\n-- 12-bit row top table (row = 4 raster indices; the 48-bit block = 4 concatenated rows)\n");
        for (int c = 0; c < 3; ++c)
        {
            const uint32_t* h = c == 2 ? hp : &global.h_row[c][0];
            const char* cn = c == 0 ? "red" : c == 1 ? "grn" : "pool";
            uint64_t tot = hist_sum(h, 4096);
            uint32_t distinct = 0, topv = 0, topc = 0;
            for (int v = 0; v < 4096; ++v)
            {
                if (h[v] > 0)
                    distinct++;
                if (h[v] > topc)
                {
                    topc = h[v];
                    topv = (uint32_t)v;
                }
            }
            uint64_t nseq = c == 0 ? global.n_rowseq[0] : c == 1 ? global.n_rowseq[1] : global.n_rowseq[0] + global.n_rowseq[1];
            uint64_t neq = c == 0 ? global.n_rowseq_eq[0] : c == 1 ? global.n_rowseq_eq[1] : global.n_rowseq_eq[0] + global.n_rowseq_eq[1];
            printf("  %s: H(row)=%.4f  distinct=%u/4096  rows repeating an earlier row: %.1f%%  row == prev stream row: %.2f%%  top1=0x%03x (%.2f%%)\n",
                   cn, h_dist(h, 4096), distinct, 100.0 * (1.0 - (double)distinct / (double)tot),
                   nseq ? 100.0 * (double)neq / (double)nseq : 0.0, topv, 100.0 * (double)topc / (double)tot);
        }

        // top-8 most common rows, pooled, with per-file mass
        {
            static rowcnt_t tab[4096];
            for (int v = 0; v < 4096; ++v)
            {
                tab[v].count = hp[v];
                tab[v].value = (uint32_t)v;
            }
            qsort(tab, 4096, sizeof(rowcnt_t), rowcnt_cmp);
            printf("  top-8 pooled rows  [i0 i1 i2 i3]  total%%   per-file %% (grey roof, cobble, brick, wood):\n");
            for (int i = 0; i < 8; ++i)
            {
                int v = tab[i].value;
                int present = 0;
                for (int f = 0; f < 4; ++f)
                    if (pf[f][v] > 0)
                        present++;
                printf("    0x%03x [%d %d %d %d]  %5.2f%%   %5.2f %5.2f %5.2f %5.2f   (%d/4 files)\n",
                       v, v & 7, (v >> 3) & 7, (v >> 6) & 7, (v >> 9) & 7,
                       100.0 * (double)tab[i].count / (double)tt,
                       tt / 4 ? 100.0 * (double)pf[0][v] / (double)(tt / 4) : 0.0,
                       tt / 4 ? 100.0 * (double)pf[1][v] / (double)(tt / 4) : 0.0,
                       tt / 4 ? 100.0 * (double)pf[2][v] / (double)(tt / 4) : 0.0,
                       tt / 4 ? 100.0 * (double)pf[3][v] / (double)(tt / 4) : 0.0,
                       present);
            }
        }

        // cumulative mass of the top-k rows
        {
            static rowcnt_t tab[4096];
            static const int ks[13] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };
            for (int c = 0; c < 3; ++c)
            {
                const uint32_t* h = c == 2 ? hp : &global.h_row[c][0];
                uint64_t tot = hist_sum(h, 4096);
                for (int v = 0; v < 4096; ++v)
                {
                    tab[v].count = h[v];
                    tab[v].value = (uint32_t)v;
                }
                qsort(tab, 4096, sizeof(rowcnt_t), rowcnt_cmp);
                const char* cn = c == 0 ? "red" : c == 1 ? "grn" : "pool";
                printf("  top-k mass %s:", cn);
                for (int i = 0; i < 13; ++i)
                {
                    uint64_t m = 0;
                    for (int v = 0; v < ks[i]; ++v)
                        m += tab[v].count;
                    printf(" k=%-4d %5.2f%%", ks[i], 100.0 * (double)m / (double)tot);
                }
                printf("\n");
            }
        }

        // cross-file repetition of the same 12-bit row
        {
            uint64_t per_file = tt / 4;
            uint64_t m_all = 0, m_p01 = 0, m_p10 = 0, n_all = 0, n_p01 = 0, n_p10 = 0;
            for (int v = 0; v < 4096; ++v)
            {
                if (!(pf[0][v] && pf[1][v] && pf[2][v] && pf[3][v]))
                    continue;
                m_all += hp[v];
                n_all++;
                if (pf[0][v] * 1000 >= per_file && pf[1][v] * 1000 >= per_file && pf[2][v] * 1000 >= per_file && pf[3][v] * 1000 >= per_file)
                {
                    m_p01 += hp[v];
                    n_p01++;
                }
                if (pf[0][v] * 100 >= per_file && pf[1][v] * 100 >= per_file && pf[2][v] * 100 >= per_file && pf[3][v] * 100 >= per_file)
                {
                    m_p10 += hp[v];
                    n_p10++;
                }
            }
            printf("  cross-file: rows present in all 4 files: %llu (%.3f%% of all rows); in all 4 at >=0.1%% each: %llu (%.3f%%); >=1%% each: %llu (%.3f%%)\n",
                   (unsigned long long)n_all, 100.0 * (double)m_all / (double)tt,
                   (unsigned long long)n_p01, 100.0 * (double)m_p01 / (double)tt,
                   (unsigned long long)n_p10, 100.0 * (double)m_p10 / (double)tt);
        }

        // top-table cost model
        {
            static const int ks[11] = { 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048 };
            printf("  top-table cost per row (bits; raw = 12.00)  |  fixed: 1 flag + log2(k) ref + 12 escape  |  modeled: Shannon flag + ref entropy + escape + header\n");
            printf("   %6s  %8s  |  %8s %8s %8s  |  %8s %8s %8s\n", "k", "mass pool", "fix red", "fix grn", "fix pool", "mod red", "mod grn", "mod pool");
            for (int i = 0; i < 11; ++i)
            {
                int k = ks[i];
                int L = 0;
                while ((1 << L) < k)
                    L++;
                double fixed[3], mod[3], massp = 0;
                for (int c = 0; c < 3; ++c)
                {
                    const uint32_t* h = c == 2 ? hp : &global.h_row[c][0];
                    double m, cost;
                    row_table_cost(h, k, &m, &cost);
                    fixed[c] = m * (1.0 + (double)L) + (1.0 - m) * 13.0;
                    mod[c] = cost;
                    if (c == 2)
                        massp = m;
                }
                printf("   %6d  %6.2f%%  |  %8.3f %8.3f %8.3f  |  %8.3f %8.3f %8.3f\n",
                       k, 100.0 * massp, fixed[0], fixed[1], fixed[2], mod[0], mod[1], mod[2]);
            }
        }

        // row-delta vs row-above model (row 0 raw 12 bits; rows 1..3 as 12-bit deltas of the row above)
        {
            static uint32_t hd[4096];
            static uint32_t hx[4096];
            for (int v = 0; v < 4096; ++v)
            {
                hd[v] = global.h_rowd[0][v] + global.h_rowd[1][v];
                hx[v] = global.h_rowx[0][v] + global.h_rowx[1][v];
            }
            for (int c = 0; c < 3; ++c)
            {
                const uint32_t* a = c == 2 ? hd : &global.h_rowd[c][0];
                const uint32_t* b = c == 2 ? hx : &global.h_rowx[c][0];
                const char* cn = c == 0 ? "red" : c == 1 ? "grn" : "pool";
                double Hamod = h_dist(a, 4096);
                double Haxor = h_dist(b, 4096);
                double hbest = Hamod < Haxor ? Hamod : Haxor;
                printf("  row delta vs row above %s: H(mod)=%.4f  H(xor)=%.4f  ->  per row %.4f bits, per index %.4f\n",
                       cn, Hamod, Haxor, (12.0 + 3.0 * hbest) / 4.0, (12.0 + 3.0 * hbest) / 16.0);
            }
        }
    }

    printf("\ndone.\n");
    return 0;
}
