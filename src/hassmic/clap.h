/* Claps in a row as an event for Home Assistant (the "Clap" event entity, setting clap_detection; main.c feeds it).
 *
 * The rules are TECHO5's double clap detector (echod/internal/feature/clap/detect.go, MIT), on 10 ms blocks:
 *   - the room's floor: the quietest block of the last CLAP_FLOOR_WINDOWS x CLAP_FLOOR_BLOCKS (2 s).  TECHO5 falls
 *     by 0.3 dB a block and rises by 0.002 dB, which left a floor taken from a stream's first digital silence (or a
 *     quiet night) at -90 dBFS for half an hour, every sound 30 dB "over" it and no block "under 6 dB" for a clap to
 *     start from; the minimum follows a louder room within 2 s, and claps and words, with pauses between, never lift it.
 *     It never goes under CLAP_FLOOR_MIN_DB, a microphone's own noise: digital silence puts no sound "over" it
 *   - a clap starts where a block is >= CLAP_ONSET_DB over the floor after one under CLAP_FROM_DB, and is "bright":
 *     the rms of its first difference >= CLAP_BRIGHT x its rms (a clap is broadband; a voice, a door's thump and the
 *     bass of music are not), and ends where it fell CLAP_FALL_DB under its peak within CLAP_FALL_BLOCKS (a voice,
 *     music, a dropped pot ring on: no clap, and the row is over)
 *   - the claps of a row start CLAP_GAP_MIN..CLAP_GAP_MAX blocks apart, peaks within CLAP_PEAK_DB of the first's,
 *     lengths within CLAP_LEN_BLOCKS: one person clapping, not a door, then a clap
 *   - after the last clap the room stays under CLAP_QUIET_DB over the floor for CLAP_QUIET_BLOCKS: claps inside
 *     applause, music or a conversation do not count
 * A row of 2 or 3 is told (clap_feed's answer) once no further clap can join it, CLAP_GAP_MAX after the last's start;
 * one clap or four and more are nothing.
 *
 * TECHO5 reads a raw microphone; hassmic hears micAsr, after Amazon's echo canceller, beamformer and gain.  A clap is
 * short and loud, which the canceller leaves alone (it removes what the speaker plays) and the beamformer passes on
 * from any side; whether the gain stage flattens the peak is what the thresholds have to be checked against on an
 * Echo (capture dump: kill -TTIN, PLAN.md).  The caller leaves out what the Echo plays itself (own): every block of
 * it breaks a row and leaves the floor alone. */
#ifndef CLAP_H
#define CLAP_H
#include <stddef.h>
#include <stdint.h>

#define CLAP_BLOCK 160                  /* samples: 10 ms at 16 kHz */
#define CLAP_ONSET_DB 18.0f
#define CLAP_FROM_DB 6.0f
#define CLAP_BRIGHT 0.5f
#define CLAP_FALL_DB 15.0f
#define CLAP_FALL_BLOCKS 12
#define CLAP_GAP_MIN 15
#define CLAP_GAP_MAX 70
#define CLAP_PEAK_DB 6.0f
#define CLAP_LEN_BLOCKS 4
#define CLAP_QUIET_DB 12.0f
#define CLAP_QUIET_BLOCKS 40
#define CLAP_ROW_MAX 3                  /* longer rows are applause, a rhythm: nothing */
#define CLAP_FLOOR_BLOCKS 50
#define CLAP_FLOOR_WINDOWS 4
#define CLAP_FLOOR_MIN_DB -75.0f        /* dBFS: under this no microphone's noise, and no clap to be told from it */
#define CLAP_NO_ONSET_DB 99.0f          /* "the last block was loud": the next one starts no clap (first block, after own sound) */

struct clap {
    int16_t buf[CLAP_BLOCK]; int nbuf;
    int64_t block;                      /* blocks seen: 64 bits, a 32-bit long wraps after 248 days of 10 ms blocks */
    float floor; int have_floor;
    float win_min[CLAP_FLOOR_WINDOWS]; int win, win_n;  /* the floor's windows: their quietest blocks, the one filling */
    float prev_above;                   /* the last block, dB over the floor */
    int in;                             /* a clap is going on: its start, peak, blocks */
    int64_t at; float peak; int len;
    int n;                              /* the row: claps so far, the first's peak and length, the last's start */
    float first_peak; int first_len; int64_t last_at;
    int64_t quiet_from;                 /* the last clap ended here: the room has to stay quiet from here */
    int loud;                           /* something not quiet came after it */
};

void clap_init(struct clap *c);
/* count samples of micAsr; own: they are the Echo's own sound (or muted, or busy): no claps there.  Returns the row's
 * length (2 or 3) when one ended in these samples, else 0.  dbg (may be NULL): why a candidate was dropped, for the log */
int clap_feed(struct clap *c, const int16_t *s, size_t count, int own, const char **dbg);
#endif
