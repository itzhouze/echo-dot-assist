/* Claps in a row (clap.h) */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "clap.h"

void clap_init(struct clap *c)
{
    memset(c, 0, sizeof *c);
    c->prev_above = 99;                 /* the first block is no onset: nothing came before it */
}

static void row_drop(struct clap *c, const char *why, const char **dbg)
{
    if (c->n && dbg) *dbg = why;
    c->n = 0; c->loud = 0;
}

/* a clap ended: into the row, or a row of its own */
static void clap_done(struct clap *c, const char **dbg)
{
    c->in = 0;
    if (c->n) {
        long gap = c->at - c->last_at;
        if (gap < CLAP_GAP_MIN) { row_drop(c, "clap: too soon after the last", dbg); return; }
        if (fabsf(c->peak - c->first_peak) > CLAP_PEAK_DB || abs(c->len - c->first_len) > CLAP_LEN_BLOCKS)
            row_drop(c, "clap: not like the first, a row of its own", dbg);       /* this one starts the next row */
    }
    if (!c->n) { c->first_peak = c->peak; c->first_len = c->len; c->loud = 0; }
    c->n++; c->last_at = c->at; c->quiet_from = c->block;
}

static int block(struct clap *c, const int16_t *s, int own, const char **dbg)
{
    int r = 0;
    /* the row is over once no further clap can start in it and the room was quiet long enough after its last */
    if (c->n && !c->in && c->block > c->last_at + CLAP_GAP_MAX && c->block >= c->quiet_from + CLAP_QUIET_BLOCKS) {
        int n = c->n;
        if (c->loud) { if (dbg) *dbg = "clap: a row, but something loud came with it"; }
        else if (n >= 2 && n <= CLAP_ROW_MAX) r = n;
        else if (n > CLAP_ROW_MAX && dbg) *dbg = "clap: too many in a row (applause)";
        c->n = 0; c->loud = 0;
    }
    c->block++;
    if (own) {
        if (c->in || c->n) { c->in = 0; row_drop(c, "clap: the Echo played something", dbg); }
        c->prev_above = 99;
        return r;
    }
    double sum = 0, diff = 0;
    for (int i = 0; i < CLAP_BLOCK; i++) {
        double v = s[i]; sum += v * v;
        if (i) { double d = v - s[i - 1]; diff += d * d; }
    }
    float level = (float)(10 * log10(sum / CLAP_BLOCK / (32768.0 * 32768.0) + 1e-12));
    if (!c->have_floor) {
        for (int i = 0; i < CLAP_FLOOR_WINDOWS; i++) c->win_min[i] = level;
        c->floor = level; c->have_floor = 1;
    }
    float above = level - c->floor;

    if (c->in) {
        c->len++;
        if (level > c->peak) c->peak = level;
        if (level <= c->peak - CLAP_FALL_DB) clap_done(c, dbg);
        else if (c->len >= CLAP_FALL_BLOCKS) { c->in = 0; c->n = 0; c->loud = 0; if (dbg) *dbg = "clap: a loud sound that went on"; }
    } else if (above >= CLAP_ONSET_DB && c->prev_above < CLAP_FROM_DB) {
        double bright = sum > 0 ? sqrt(diff / (CLAP_BLOCK - 1)) / sqrt(sum / CLAP_BLOCK) : 0;
        if (bright >= CLAP_BRIGHT) { c->in = 1; c->at = c->block; c->peak = level; c->len = 1; }
        else row_drop(c, "clap: a loud dull sound", dbg);
    } else if (c->n && above >= CLAP_QUIET_DB && level > c->first_peak - CLAP_FALL_DB)
        c->loud = 1;                    /* a sound rising out of the room, not a clap's decay: talk, music */

    /* the floor: the quietest block of the last windows, down at once */
    if (!c->win_n || level < c->win_min[c->win]) c->win_min[c->win] = level;     /* a window's first block starts it over */
    if (++c->win_n == CLAP_FLOOR_BLOCKS) { c->win = (c->win + 1) % CLAP_FLOOR_WINDOWS; c->win_n = 0; }
    c->floor = c->win_min[0];
    for (int i = 1; i < CLAP_FLOOR_WINDOWS; i++) if (c->win_min[i] < c->floor) c->floor = c->win_min[i];
    c->prev_above = above;
    return r;
}

int clap_feed(struct clap *c, const int16_t *s, size_t count, int own, const char **dbg)
{
    int r = 0;
    while (count) {
        size_t k = CLAP_BLOCK - c->nbuf;
        if (k > count) k = count;
        memcpy(c->buf + c->nbuf, s, k * sizeof *s);
        c->nbuf += (int)k; s += k; count -= k;
        if (c->nbuf == CLAP_BLOCK) { int n = block(c, c->buf, own, dbg); if (n) r = n; c->nbuf = 0; }
    }
    return r;
}
