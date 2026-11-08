/* Room loudness and speech: see roomlevel.h. */
#include "roomlevel.h"
#include <math.h>
#include <string.h>

void rl_reset(struct roomlevel *r)
{
    memset(r, 0, sizeof *r);
    r->db = r->speech_pct = NAN;
}

static void count(struct roomlevel *r, const struct afemeta *m, int own)
{
    r->meta = 1;
    if (own || m->playback || m->muted) return;     /* the record's own flags too: the front end knows what it played */
    r->frames++;
    r->speech += m->vad >= 2;
}

/* The records in k samples.  Synced: one every 128 samples, checked whole (sync, version, magic, checksum: a wrong one
 * means samples were lost and the phase moved).  Else 129 phases are tried on 256 samples, then the next 128: cheap
 * where no record ever comes (the PC, Android's HALs), as afemeta_frame gives up on the first byte. */
static void records(struct roomlevel *r, const int16_t *s, size_t k, int own)
{
    struct afemeta m;
    while (k) {
        if (r->synced) {
            size_t t = k < (size_t)(AFEMETA_FRAME - r->nbuf) ? k : (size_t)(AFEMETA_FRAME - r->nbuf);
            memcpy(r->buf + r->nbuf, s, t * sizeof *s); r->nbuf += (int)t; r->rec_own |= own; s += t; k -= t;
            if (r->nbuf < AFEMETA_FRAME) continue;
            if (afemeta_frame(r->buf, &m)) count(r, &m, r->rec_own);
            else r->synced = 0;
            r->nbuf = 0; r->rec_own = 0;
        } else {
            size_t t = k < (size_t)(2 * AFEMETA_FRAME - r->nbuf) ? k : (size_t)(2 * AFEMETA_FRAME - r->nbuf);
            memcpy(r->buf + r->nbuf, s, t * sizeof *s); r->nbuf += (int)t; s += t; k -= t;
            if (r->nbuf < 2 * AFEMETA_FRAME) continue;
            int at = -1;
            for (int o = 0; o <= AFEMETA_FRAME && at < 0; o++) if (afemeta_frame(r->buf + o, &m)) at = o;
            if (at >= 0) {                          /* what follows the record starts the next one */
                count(r, &m, own);
                r->nbuf = AFEMETA_FRAME - at;
                memmove(r->buf, r->buf + at + AFEMETA_FRAME, (size_t)r->nbuf * sizeof *r->buf);
                r->synced = 1; r->rec_own = (unsigned char)own;
            } else {
                memmove(r->buf, r->buf + AFEMETA_FRAME, AFEMETA_FRAME * sizeof *r->buf);
                r->nbuf = AFEMETA_FRAME;
            }
        }
    }
}

static void close_window(struct roomlevel *r)
{
    if (r->muted * 2 >= r->seen) {                  /* the mics were off: nothing heard, nothing to hold */
        r->db = r->speech_pct = NAN; r->stale = 0;
        memset(r->fr, 0, sizeof r->fr); memset(r->sp, 0, sizeof r->sp);
    } else {
        if (r->clean * 2 >= r->seen) {
            double ms = (double)r->sum / r->clean, d = ms > 0 ? 10 * log10(ms / (32768.0 * 32768.0)) : RL_FLOOR_DB;
            r->db = (float)round(d < RL_FLOOR_DB ? RL_FLOOR_DB : d); r->stale = 0;
        } else if (++r->stale >= RL_MINUTE) r->db = NAN;
        r->fr[r->slot] = r->frames; r->sp[r->slot] = r->speech; r->slot = (r->slot + 1) % RL_MINUTE;
        int f = 0, sp = 0;
        for (int i = 0; i < RL_MINUTE; i++) { f += r->fr[i]; sp += r->sp[i]; }
        r->speech_pct = r->meta && f >= RL_SPEECH_MIN ? (float)round(100.0 * sp / f) : NAN;
    }
    r->seen = r->clean = r->muted = 0; r->sum = 0; r->frames = r->speech = 0;
}

int rl_feed(struct roomlevel *r, const int16_t *s, size_t n, int own, int muted)
{
    int closed = 0;
    while (n) {
        size_t k = RL_WINDOW - r->seen; if (k > n) k = n;
        if (muted) r->muted += (uint32_t)k;
        else if (!own) { for (size_t i = 0; i < k; i++) r->sum += (uint64_t)((int32_t)s[i] * s[i]); r->clean += (uint32_t)k; }
        r->seen += (uint32_t)k;
        records(r, s, k, own || muted);
        s += k; n -= k;
        if (r->seen >= RL_WINDOW) { close_window(r); closed = 1; }
    }
    return closed;
}

int rl_send(float sent, float now, int since, float step, int zero)
{
    if (isnan(sent) || isnan(now)) return isnan(sent) != isnan(now);
    float d = fabsf(now - sent);
    if (d >= step) return 1;
    if (zero && (sent == 0) != (now == 0)) return 1;
    return d > 0 && since >= RL_MINUTE;
}
