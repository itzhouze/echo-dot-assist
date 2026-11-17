/* Claps in a row (clap.h) */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "clap.h"

void clap_init(struct clap *c)
{
    memset(c, 0, sizeof *c);
    c->prev_above = CLAP_NO_ONSET_DB;   /* the first block is no onset: nothing came before it */
}

/* The row so far is nothing: dropped, and the reason noted when there was a row to drop */
static void row_drop(struct clap *c, const char *why, const char **dbg)
{
    if (c->n && dbg) *dbg = why;
    c->n = 0; c->loud = 0;
}

/* A clap ended: it joins the row, or (not like the first) starts one of its own */
static void clap_done(struct clap *c, const char **dbg)
{
    c->in = 0;
    if (c->n) {
        if (c->at - c->last_at < CLAP_GAP_MIN) { row_drop(c, "clap: too soon after the last", dbg); return; }
        if (fabsf(c->peak - c->first_peak) > CLAP_PEAK_DB || abs(c->len - c->first_len) > CLAP_LEN_BLOCKS)
            row_drop(c, "clap: not like the first, a row of its own", dbg);
    }
    if (!c->n) { c->first_peak = c->peak; c->first_len = c->len; }
    c->n++; c->last_at = c->at; c->quiet_from = c->block;
}

/* The row is over once no further clap can start in it and the room stayed quiet after its last: its length when it
 * is a double or triple clap, else 0 */
static int row_close(struct clap *c, const char **dbg)
{
    if (!c->n || c->in || c->block <= c->last_at + CLAP_GAP_MAX || c->block < c->quiet_from + CLAP_QUIET_BLOCKS) return 0;
    int n = c->n, loud = c->loud;
    c->n = 0; c->loud = 0;
    if (loud) { if (dbg) *dbg = "clap: a row, but something loud came with it"; return 0; }
    if (n > CLAP_ROW_MAX) { if (dbg) *dbg = "clap: too many in a row (applause)"; return 0; }
    return n >= 2 ? n : 0;
}

/* A block's level in dBFS and how bright it is: the rms of its first difference over its rms (a clap is broadband) */
static void measure(const int16_t *s, float *level, float *bright)
{
    double sum = 0, diff = 0;
    for (int i = 0; i < CLAP_BLOCK; i++) {
        double v = s[i]; sum += v * v;
        if (i) { double d = v - s[i - 1]; diff += d * d; }
    }
    *level = (float)(10 * log10(sum / CLAP_BLOCK / (32768.0 * 32768.0) + 1e-12));
    *bright = sum > 0 ? (float)(sqrt(diff / (CLAP_BLOCK - 1)) / sqrt(sum / CLAP_BLOCK)) : 0;
}

/* The room's floor: the quietest block of the last windows, down at once, never under what a microphone's own noise is
 * (a stream's digital silence would put every sound 40 dB "over" it) */
static void floor_update(struct clap *c, float level)
{
    if (!c->win_n || level < c->win_min[c->win]) c->win_min[c->win] = level;     /* a window's first block starts it over */
    if (++c->win_n == CLAP_FLOOR_BLOCKS) { c->win = (c->win + 1) % CLAP_FLOOR_WINDOWS; c->win_n = 0; }
    float f = c->win_min[0];
    for (int i = 1; i < CLAP_FLOOR_WINDOWS; i++) if (c->win_min[i] < f) f = c->win_min[i];
    c->floor = f < CLAP_FLOOR_MIN_DB ? CLAP_FLOOR_MIN_DB : f;
}

/* One block while a clap is going on: it ends where it fell far enough under its peak, and is no clap if it does not */
static void clap_continue(struct clap *c, float level, const char **dbg)
{
    c->len++;
    if (level > c->peak) c->peak = level;
    if (level <= c->peak - CLAP_FALL_DB) clap_done(c, dbg);
    else if (c->len >= CLAP_FALL_BLOCKS) { c->in = 0; row_drop(c, "clap: a loud sound that went on", dbg); }
}

/* One block; returns the row's length when one ended with it */
static int block(struct clap *c, const int16_t *s, int own, const char **dbg)
{
    int row = row_close(c, dbg);
    c->block++;
    if (own) {
        if (c->in || c->n) { c->in = 0; row_drop(c, "clap: the Echo played something", dbg); }
        c->prev_above = CLAP_NO_ONSET_DB;
        return row;
    }

    float level, bright;
    measure(s, &level, &bright);
    if (!c->have_floor) {
        for (int i = 0; i < CLAP_FLOOR_WINDOWS; i++) c->win_min[i] = level;
        c->floor = level < CLAP_FLOOR_MIN_DB ? CLAP_FLOOR_MIN_DB : level;
        c->have_floor = 1;
    }
    float above = level - c->floor;

    if (c->in) clap_continue(c, level, dbg);
    else if (above >= CLAP_ONSET_DB && c->prev_above < CLAP_FROM_DB) {
        if (bright >= CLAP_BRIGHT) { c->in = 1; c->at = c->block; c->peak = level; c->len = 1; }
        else row_drop(c, "clap: a loud dull sound", dbg);
    } else if (c->n && above >= CLAP_QUIET_DB && level > c->first_peak - CLAP_FALL_DB)
        c->loud = 1;                    /* a sound rising out of the room, not a clap's decay: talk, music */

    floor_update(c, level);
    c->prev_above = above;
    return row;
}

int clap_feed(struct clap *c, const int16_t *s, size_t count, int own, const char **dbg)
{
    int row = 0;
    while (count) {
        size_t k = CLAP_BLOCK - (size_t)c->nbuf;
        if (k > count) k = count;
        memcpy(c->buf + c->nbuf, s, k * sizeof *s);
        c->nbuf += (int)k; s += k; count -= k;
        if (c->nbuf < CLAP_BLOCK) continue;
        int n = block(c, c->buf, own, dbg);
        if (n) row = n;
        c->nbuf = 0;
    }
    return row;
}
