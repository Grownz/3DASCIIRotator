/* shatter.c - the "shatter" effect: every drawn character becomes a particle that
 * falls under gravity and piles up on the floor the object was resting on.
 * Pure C, no platform calls: it only reads and writes the character grid. */
#include <stdlib.h>
#include <string.h>
#include "shatter.h"

#define GRAVITY       80.0   /* shatter particle accel, cells/s^2   */
#define RESTITUTION    0.35  /* shatter bounce factor               */
#define PILE_MAX      10     /* max debris height above the floor   */

/* --------------------------------------------------------------- particles
 * When the object is shattered (ENTER / --shatter) every visible character
 * becomes an independent particle that falls under gravity onto the invisible
 * floor the object was resting on. The debris spreads sideways and settles
 * into a shallow layer no higher than PILE_MAX rows.
 */

typedef struct {
    float x, y;    /* screen cell coordinates (x = right, y = down) */
    float vx, vy;  /* cells per second                              */
    char  c;
    int   rest;
} Particle;

static Particle *g_parts     = NULL;
static int       g_part_cap  = 0;
static int       g_part_n    = 0;
static int       g_p_cols    = 0;
static int       g_floor_row = 0;
static int      *g_col_h     = NULL;   /* resting particles per column */
static int       g_col_cap   = 0;

static unsigned int g_rng = 2463534242u;
void shatter_seed(unsigned int s) { g_rng = s ? s : 1u; }
static double rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (double)g_rng / 4294967296.0;
}

static int particle_room(int need) {
    if (need <= g_part_cap) return 1;
    {
        Particle *p = (Particle *)realloc(g_parts, (size_t)need * sizeof(Particle));
        if (!p) return 0;
        g_parts = p; g_part_cap = need;
    }
    return 1;
}

static int col_h_room(int cols) {
    if (cols <= g_col_cap) return 1;
    {
        int *p = (int *)realloc(g_col_h, (size_t)cols * sizeof(int));
        if (!p) return 0;
        g_col_h = p; g_col_cap = cols;
    }
    return 1;
}

/* Capture the current character frame as falling particles. */
void shatter_spawn(const char *grid, int cols, int rows) {
    int r, c, maxr = 0;
    if (!particle_room(cols * rows) || !col_h_room(cols)) return;
    g_part_n = 0; g_p_cols = cols;
    for (r = 0; r < rows; ++r) {
        for (c = 0; c < cols; ++c) {
            char ch = grid[(size_t)r * cols + c];
            if (ch == ' ' || ch == '\0') continue;
            {
                Particle *p = &g_parts[g_part_n++];
                p->x  = (float)c;
                p->y  = (float)r;
                p->vx = (float)((rnd() - 0.5) * 9.0);
                p->vy = (float)(-rnd() * 5.0);   /* small upward pop */
                p->c  = ch;
                p->rest = 0;
            }
            if (r > maxr) maxr = r;
        }
    }
    g_floor_row = maxr;
    for (c = 0; c < cols; ++c) g_col_h[c] = 0;
}

void shatter_step(double dt) {
    int i;
    if (!g_parts) return;
    for (i = 0; i < g_part_n; ++i) {
        Particle *p = &g_parts[i];
        int col, surface;
        if (p->rest) continue;

        p->vy += (float)(GRAVITY * dt);
        p->x  += (float)(p->vx * dt);
        p->y  += (float)(p->vy * dt);

        if (p->x < 0.0f)            { p->x = 0.0f;                    p->vx = -p->vx * 0.5f; }
        if (p->x > g_p_cols - 1.0f) { p->x = (float)(g_p_cols - 1);    p->vx = -p->vx * 0.5f; }

        col = (int)(p->x + 0.5f);
        if (col < 0) col = 0;
        if (col >= g_p_cols) col = g_p_cols - 1;
        surface = g_floor_row - g_col_h[col];

        if (p->y >= surface) {
            if (p->vy > 8.0f) {                  /* fast enough to bounce */
                p->y  = (float)surface;
                p->vy = -p->vy * (float)RESTITUTION;
                p->vx *= 0.6f;
            } else {                             /* settle into the debris layer */
                int target = col, d;
                for (d = 1; d <= 24; ++d) {      /* spill towards a shorter column */
                    int l = col - d, r = col + d;
                    if (l >= 0   && g_col_h[l] < PILE_MAX) { target = l; break; }
                    if (r < g_p_cols && g_col_h[r] < PILE_MAX) { target = r; break; }
                }
                if (g_col_h[target] < PILE_MAX) {
                    {
                        int ry = g_floor_row - g_col_h[target];
                        if (ry < 0) ry = 0;
                        p->y = (float)ry;
                    }
                    g_col_h[target]++;
                } else {                         /* layer full: let it overlap */
                    int ry = g_floor_row - (int)(rnd() * PILE_MAX);
                    if (ry < 0) ry = 0;
                    p->y = (float)ry;
                }
                p->x  = (float)target;
                p->vy = 0.0f;
                p->vx = 0.0f;
                p->rest = 1;
            }
        }
    }
}

void shatter_draw(char *grid, int cols, int rows) {
    int i;
    memset(grid, ' ', (size_t)cols * rows);
    if (!g_parts) return;
    for (i = 0; i < g_part_n; ++i) {
        Particle *p = &g_parts[i];
        int c = (int)(p->x + 0.5f);
        int r = (int)(p->y + 0.5f);
        if (c < 0 || c >= cols || r < 0 || r >= rows) continue;
        grid[(size_t)r * cols + c] = p->c;
    }
}


void shatter_free(void) {
    free(g_parts); g_parts = NULL; g_part_cap = 0; g_part_n = 0;
    free(g_col_h); g_col_h = NULL; g_col_cap = 0;
}
