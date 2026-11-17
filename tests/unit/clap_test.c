/* Claps in a row (clap.c) on synthetic sound: a room's noise at -60 dBFS, claps as bursts of white noise decaying by
 * 4 dB a millisecond-ish (a hand clap's 5 ms time constant), a voice as a 180 Hz vowel with its harmonics, a door's
 * thump as a 60 Hz burst.  Every case is fed in uneven pieces, as the capture thread's reads are, and must give the
 * same as fed whole. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clap.h"

#define RATE 16000
#define MAXS (RATE * 12)
static int16_t pcm[MAXS];
static unsigned char own[MAXS];
static int len, bad;
static unsigned rnd = 1;

static double noise(void) { rnd = rnd * 1103515245u + 12345u; return ((rnd >> 8) & 0xffff) / 32768.0 - 1.0; }
static double db2a(double db) { return 32768.0 * pow(10, db / 20) * sqrt(3.0); }      /* white noise in [-a, a]: rms a/sqrt 3 */

static void add(int at, double v) { if (at < 0 || at >= MAXS) return; double s = pcm[at] + v; pcm[at] = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s; }

static void room(int ms, double db)                 /* a new case: the room's noise for ms */
{
    memset(pcm, 0, sizeof pcm); memset(own, 0, sizeof own);
    len = ms * RATE / 1000;
    for (int i = 0; i < len; i++) pcm[i] = (int16_t)(noise() * db2a(db));
}

static void clap(int ms, double db)                 /* a clap starting at ms */
{
    int at = ms * RATE / 1000;
    for (int i = 0; i < RATE / 10; i++) add(at + i, noise() * db2a(db) * exp(-i / (0.005 * RATE)));
}

static void vowel(int ms, int dur, double db)       /* a voice: 180 Hz and harmonics falling off, swelling in */
{
    int at = ms * RATE / 1000, n = dur * RATE / 1000;
    for (int i = 0; i < n; i++) {
        double env = fmin(1, i / (0.03 * RATE)) * fmin(1, (n - i) / (0.03 * RATE)), v = 0;
        for (int h = 1; h <= 6; h++) v += sin(2 * M_PI * 180 * h * i / RATE) / (h * h);
        add(at + i, v * env * 32768 * pow(10, db / 20) * 1.2);
    }
}

static void thump(int ms, double db)                /* a door: 60 Hz, decaying */
{
    int at = ms * RATE / 1000;
    for (int i = 0; i < RATE / 5; i++) add(at + i, sin(2 * M_PI * 60 * i / RATE) * 32768 * pow(10, db / 20) * 1.4 * exp(-i / (0.03 * RATE)));
}

static void own_from(int from_ms, int to_ms) { for (int i = from_ms * RATE / 1000; i < to_ms * RATE / 1000 && i < MAXS; i++) own[i] = 1; }

static int run(int pieces, int *rows)               /* the rows told, in order; how many */
{
    struct clap c; int n = 0, i = 0;
    clap_init(&c);
    while (i < len) {
        int k = pieces ? 1 + (int)((noise() + 1) * 400) : 320;     /* 1..800 samples, own flag constant within */
        if (i + k > len) k = len - i;
        int j = 1; while (j < k && own[i + j] == own[i]) j++;
        k = j;
        const char *why = NULL;
        int r = clap_feed(&c, pcm + i, (size_t)k, own[i], &why);
        if (r && n < 8) rows[n++] = r;
        i += k;
    }
    return n;
}

static void check(const char *what, int want)       /* want: the one row told, 0 none */
{
    int a[8], b[8], na = run(0, a), nb = run(1, b);
    int got = na == 1 ? a[0] : na == 0 ? 0 : -na;
    int same = na == nb && !memcmp(a, b, sizeof(int) * (size_t)na);
    int ok = got == want && same;
    printf("%s %s: %s%s\n", ok ? "ok  " : "FAIL", what, got > 0 ? (got == 2 ? "double" : "triple") : got == 0 ? "nothing" : "several rows",
           same ? "" : ", in pieces it differs");
    if (!ok) bad = 1;
}

int main(void)
{
    room(3000, -60); clap(1000, -20); clap(1300, -21);
    check("two claps 300 ms apart", 2);
    room(3000, -60); clap(1000, -20); clap(1250, -20); clap(1500, -22);
    check("three claps 250 ms apart", 3);
    room(3000, -60); clap(1000, -20);
    check("one clap", 0);
    room(4000, -60); for (int i = 0; i < 5; i++) clap(1000 + 250 * i, -20);
    check("five claps (applause)", 0);
    room(3000, -60); clap(1000, -20); clap(1080, -20);
    check("two claps 80 ms apart (too soon)", 0);
    room(4000, -60); clap(1000, -20); clap(1900, -20);
    check("two claps 900 ms apart (too far)", 0);
    room(3000, -60); clap(1000, -20); clap(1300, -34);
    check("the second 14 dB quieter", 0);
    room(4000, -60); vowel(1000, 250, -22); vowel(1400, 250, -22);
    check("two syllables of a voice", 0);
    room(3000, -60); thump(1000, -20); thump(1300, -20);
    check("a door twice", 0);
    room(3000, -60); clap(1000, -20); clap(1300, -20); vowel(1500, 600, -25);
    check("two claps, then talk", 0);
    room(3000, -60); vowel(500, 1200, -30); clap(800, -15); clap(1100, -15);
    check("two claps over talk", 0);
    room(3000, -60); clap(1000, -20); clap(1300, -20); own_from(1200, 1500);
    check("two claps while the Echo plays", 0);
    room(3000, -60); own_from(0, 900); clap(1000, -20); clap(1300, -20);
    check("two claps after the Echo played", 2);
    room(3000, -45); clap(1000, -20); clap(1300, -20);
    check("a loud room (-45 dBFS), claps 25 dB over it", 2);
    room(3000, -45); clap(1000, -35); clap(1300, -35);
    check("claps only 10 dB over the room", 0);
    room(5000, -60); memset(pcm, 0, 16000 * sizeof *pcm); clap(3500, -20); clap(3800, -20);
    check("digital silence first, then the room, then two claps", 2);
    room(11000, -60); clap(1000, -20); clap(1300, -20); clap(5000, -20); clap(5300, -20);
    { int a[8]; int n = run(0, a); int ok = n == 2 && a[0] == 2 && a[1] == 2;
      printf("%s two double claps 4 s apart: %d rows\n", ok ? "ok  " : "FAIL", n); if (!ok) bad = 1; }
    printf(bad ? "clap: FAILED\n" : "clap: all ok\n");
    return bad;
}
