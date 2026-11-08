/* Room loudness and speech (feature "room_loudness", off by default): how loud the room is and how much of the time
 * somebody talks in it, as two sensors for Home Assistant's presence and activity automations.  Read from the mic
 * stream the wake word hears (micAsr: after Amazon's echo canceller and beamformer, before our gain), so neither the
 * mic level setting nor the pipeline's AGC moves it.
 *
 * Loudness: the mean square of the last 5 s in dBFS, rounded to a whole dB, as TECHO5's "Ambient noise" (5 s, dBFS).
 * Not a calibrated sound level: micAsr is ~30 dB under what STT wants (micgain.h).  What a quiet room reads on each
 * model is not measured yet.
 * Speech: the share of the last minute's front end frames (8 ms each) whose DNN VAD says >= 2, p >= 0.5 (afemeta.h:
 * 98-100 % of a normal voice's loud frames, 0-23 % of sounds without words, 0 for the Echo's own replies).  Pauses
 * between words count as none, so a talker stays under 100; how far is not measured yet.  Only where the stream carries
 * the records; elsewhere unknown.
 *
 * Samples the Echo itself plays into (own: our replies, music, sounds, a call's far end, and their tail) are left out;
 * a window that is mostly that gives no reading and the last one stays.  A minute without a reading (music) makes both
 * unknown: a held value would claim a room that nobody measured.  Muted mics: unknown at the window's end.
 *
 * Runs on the capture thread, which feeds the wake word: nothing here may block, and rl_feed takes no lock and makes no
 * system call.  The caller reads the mute state and sends the readings elsewhere (main.c room_feed).
 *
 * Counted in samples, not wall time: the unit test (tests/unit/roomlevel_test.c) feeds it exactly. */
#ifndef ROOMLEVEL_H
#define ROOMLEVEL_H
#include <stddef.h>
#include <stdint.h>
#include "afemeta.h"

#define RL_RATE 16000
#define RL_WINDOW (5 * RL_RATE)             /* samples per reading */
#define RL_MINUTE 12                        /* windows: speech over the last minute; a minute without a reading */
#define RL_FLOOR_DB (-96)                   /* digital silence (a mean square under one LSB) */
#define RL_SPEECH_MIN (RL_RATE / AFEMETA_FRAME)   /* records: under 1 s of them in the minute is no share */

struct roomlevel {
    /* the window running */
    uint32_t seen, clean, muted;            /* samples: all, counted, muted */
    uint64_t sum;                           /* squares of the counted ones: < 2^47 in a window */
    int frames, speech;                     /* records counted, with speech */
    /* the records: found at a phase, then every 128 samples (a lost read moves it: looked for again) */
    int16_t buf[2 * AFEMETA_FRAME]; int nbuf, synced, meta;     /* meta: any record seen */
    unsigned char rec_own;                  /* the record collecting now overlaps own playback or mute */
    /* the last minute */
    int fr[RL_MINUTE], sp[RL_MINUTE], slot, stale;
    float db, speech_pct;                   /* the readings; NAN: unknown */
};

void rl_reset(struct roomlevel *r);
/* n samples of micAsr; own: the Echo plays into them; muted: the mics are off.  1 when a window closed: db and
 * speech_pct are this window's readings (the same values again when it gave none) */
int  rl_feed(struct roomlevel *r, const int16_t *s, size_t n, int own, int muted);
/* Whether a reading goes to Home Assistant, against the one sent last: unknown <-> known at once, a move of step or
 * more, any move after RL_MINUTE windows (since: windows since that send).  zero: a move from or to 0 counts at once
 * (speech starting in a silent room). */
int  rl_send(float sent, float now, int since, float step, int zero);
#endif
