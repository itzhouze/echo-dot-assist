/* Room loudness and speech (roomlevel.c) on made-up micAsr: noise at a known level, the front end's records in the
 * lowest bits (written as tests/unit/afemeta_test.c does, version 3.3), fed in odd pieces as cap_read hands them over.
 * The level, the speech share, a phase lost and found again, the Echo's own playback (left out, held, then unknown),
 * muted mics, the records' own playback flag, digital silence, and when a reading goes to Home Assistant. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "roomlevel.h"

static int bad;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) bad = 1; }

static void put(int16_t *s, int at, int n, unsigned v) { while (n--) { s[at] = (int16_t)((s[at] & ~1) | ((v >> n) & 1)); at++; } }

static void record(int16_t *s, int vad, int playback)
{
    for (int i = 0; i < 128; i++) s[i] &= ~1;
    put(s, 0, 8, 0xa5); put(s, 8, 8, 0x63); put(s, 16, 7, 0x68);
    put(s, 67, 1, (unsigned)playback); put(s, 113, 2, (unsigned)vad);
    unsigned ones = 0; for (int i = 0; i < 120; i++) ones += s[i] & 1;
    put(s, 120, 8, ones & 0xff);
}

/* the stream: uniform noise at db dBFS rms; with records, VAD 3 in the first `speech` of every 10 frames, else 0 */
static long long pos;                       /* samples made so far: records start where pos % 128 == phase */
static int phase, recs, speech = 5, playback;
static double level_db = -40;
static int16_t one(void)
{
    double a = 32768 * pow(10, level_db / 20) * sqrt(3);
    double v = level_db <= -200 ? 0 : ((double)rand() / RAND_MAX * 2 - 1) * a;
    return (int16_t)lround(v > 32767 ? 32767 : v < -32768 ? -32768 : v);   /* rounded: a cast cuts the power of small noise */
}
static int16_t big[RL_WINDOW * RL_MINUTE + 4096];
static size_t fill(size_t n)                 /* n samples into big, records at phase, from pos */
{
    for (size_t i = 0; i < n; i++) big[i] = one();
    if (recs)
        for (size_t i = 0; i + 128 <= n; i++)
            if ((pos + (long long)i) % 128 == phase) { long long f = (pos + (long long)i) / 128;
                record(big + i, (int)(f % 10) < speech ? 3 : 0, playback); i += 127; }
    pos += (long long)n;
    return n;
}

/* feeds n samples in pieces of 333 (no multiple of 128); own, muted for all of them.  Returns windows closed */
static int feed(struct roomlevel *r, size_t n, int own, int muted)
{
    int w = 0;
    while (n) {
        size_t k = n < sizeof big / sizeof *big ? n : sizeof big / sizeof *big;
        fill(k);
        for (size_t at = 0; at < k; at += 333) w += rl_feed(r, big + at, k - at < 333 ? k - at : 333, own, muted);
        n -= k;
    }
    return w;
}

int main(void)
{
    struct roomlevel r; char what[160];
    srand(7);

    rl_reset(&r);
    check(isnan(r.db) && isnan(r.speech_pct), "unknown before the first window");
    check(feed(&r, RL_WINDOW - 1, 0, 0) == 0 && isnan(r.db), "no reading before 5 s");
    check(feed(&r, 1, 0, 0) == 1 && r.db == -40, "-40 dBFS of noise: -40 after 5 s");
    snprintf(what, sizeof what, "no records (the PC, Android's HALs): speech unknown (%g)", r.speech_pct);
    check(isnan(r.speech_pct), what);
    level_db = -63.3; feed(&r, RL_WINDOW, 0, 0);
    snprintf(what, sizeof what, "-63.3 dBFS: -63, rounded to a whole dB (%g)", r.db);
    check(r.db == -63, what);
    level_db = -300; feed(&r, RL_WINDOW, 0, 0);
    check(r.db == RL_FLOOR_DB, "digital silence: the floor, not minus infinity");

    /* records: half the frames speech, fed in pieces that cut them anywhere */
    rl_reset(&r); level_db = -40; recs = 1; phase = 77; speech = 5;
    feed(&r, RL_WINDOW, 0, 0);
    snprintf(what, sizeof what, "records found at their phase, half the frames speech: %g %%", r.speech_pct);
    check(fabsf(r.speech_pct - 50) <= 1 && r.db == -40, what);
    speech = 0; feed(&r, 6 * RL_WINDOW, 0, 0);
    snprintf(what, sizeof what, "half a minute of silence after it: the minute's share halves, %g %%", r.speech_pct);
    check(fabsf(r.speech_pct - 50.0f * 1 / 7) <= 1, what);
    feed(&r, 6 * RL_WINDOW, 0, 0);
    check(r.speech_pct == 0, "a whole minute without speech: 0 %");

    /* samples lost: the records' phase moves, they are found again within a window */
    speech = 10; phase = 13;
    feed(&r, RL_WINDOW, 0, 0); feed(&r, RL_WINDOW, 0, 0);
    snprintf(what, sizeof what, "phase moved (a read lost samples): found again, 2 windows of 12 all speech = %g %%", r.speech_pct);
    check(fabsf(r.speech_pct - 100.0f * 2 / 12) <= 1, what);

    /* the Echo's own playback: left out, the last reading held, a minute of it unknown */
    rl_reset(&r); speech = 5; level_db = -50;
    feed(&r, RL_WINDOW, 0, 0);
    float sp = r.speech_pct;
    level_db = -10; speech = 10;
    feed(&r, RL_WINDOW, 1, 0);
    snprintf(what, sizeof what, "a window of its own reply: no reading, -50 dBFS and %g %% held (%g, %g)", sp, r.db, r.speech_pct);
    check(r.db == -50 && r.speech_pct == sp, what);
    feed(&r, (RL_MINUTE - 2) * RL_WINDOW, 1, 0);
    check(r.db == -50, "11 windows of music: still held");
    feed(&r, RL_WINDOW, 1, 0);
    check(isnan(r.db) && isnan(r.speech_pct), "a minute of music: both unknown");
    level_db = -45; speech = 0;
    feed(&r, RL_WINDOW, 0, 0);
    check(r.db == -45 && r.speech_pct == 0, "quiet again: readings at once");

    /* part of a window: the rest counts */
    rl_reset(&r); level_db = -10;
    feed(&r, RL_WINDOW / 4, 1, 0);
    level_db = -40; feed(&r, RL_WINDOW * 3 / 4, 0, 0);
    snprintf(what, sizeof what, "a quarter of the window its own sound at -10: the rest's -40 (%g)", r.db);
    check(r.db == -40, what);
    level_db = -10; feed(&r, RL_WINDOW * 3 / 4, 1, 0);
    level_db = -40; feed(&r, RL_WINDOW / 4, 0, 0);
    check(r.db == -40, "three quarters its own: no reading, the last one held");

    /* the records' own playback flag: the front end played it, whatever the core thinks */
    rl_reset(&r); speech = 10; playback = 1;
    feed(&r, RL_WINDOW, 0, 0);
    check(isnan(r.speech_pct), "records flagged playback: none counted, speech unknown");
    playback = 0; feed(&r, RL_WINDOW, 0, 0);
    check(r.speech_pct == 100, "unflagged: counted");

    /* muted: unknown at once, the minute forgotten */
    feed(&r, RL_WINDOW, 0, 1);
    check(isnan(r.db) && isnan(r.speech_pct), "muted: both unknown at once");
    speech = 0; feed(&r, RL_WINDOW, 0, 0);
    check(r.db == -40 && r.speech_pct == 0, "unmuted: the minute before the mute forgotten");

    /* what goes to Home Assistant */
    check(rl_send(NAN, -40, 0, 2, 0) && rl_send(-40, NAN, 0, 2, 0) && !rl_send(NAN, NAN, 99, 2, 0), "unknown <-> known: sent at once");
    check(!rl_send(-40, -41, 0, 2, 0) && rl_send(-40, -42, 0, 2, 0), "loudness: 1 dB kept back, 2 sent");
    check(rl_send(-40, -41, RL_MINUTE, 2, 0) && !rl_send(-40, -40, RL_MINUTE, 2, 0), "any move after a minute; none without one");
    check(rl_send(0, 2, 0, 5, 1) && rl_send(3, 0, 0, 5, 1) && !rl_send(3, 6, 0, 5, 1) && rl_send(3, 8, 0, 5, 1),
          "speech: from or to 0 at once, else 5 points");
    return bad;
}
