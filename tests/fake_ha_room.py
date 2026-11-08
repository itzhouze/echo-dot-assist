#!/usr/bin/env python3
"""Home Assistant's side of the ESPHome native API against build/hassmic-host (tests/esphome_common.py): room loudness
and speech (roomlevel.h).  The capture, looped: noise at -50 dBFS, and in the samples' lowest bits the front end's
records (afemeta.h, as tests/fake_ha_end.py writes them) with speech in the first 40 % of each 6.4 s loop.  Off by
default; on from the page, two sensors; the loudness and the minute's speech share from the capture; a request beside
them; muted, both unknown, back after; off again, gone."""
import asyncio, os, random, struct, tempfile
from aioesphomeapi import APIClient, SensorInfo, SensorState
from aioesphomeapi import VoiceAssistantEventType as Ev
from esphome_common import check, until, sync, home_assistant, assistant, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17201, 17202, 17203, 17204, 17205
LOOP, SPEECH, DB = 6.4, 0.4, -50          # 6.4 s = 800 records: their phase stays across the loop


def capture(path):
    rnd = random.Random(3)
    a = 32768 * 10 ** (DB / 20) * 3 ** 0.5    # uniform noise of that rms
    out = [round(rnd.uniform(-a, a)) for _ in range(int(LOOP * 16000))]
    speech = int(LOOP * SPEECH * 16000)
    for f in range(0, len(out) - 127, 128):   # sync, version, magic, VAD at 113-114, checksum (docs/re-vad.md, 3.3)
        bits = [0] * 128
        for at, n, v in ((0, 8, 0xa5), (8, 8, 0x63), (16, 7, 0x68), (113, 2, 3 if f + 128 <= speech else 0)):
            for k in range(n): bits[at + k] = v >> (n - 1 - k) & 1
        ones = sum(bits[:120]) & 0xff
        for k in range(8): bits[120 + k] = ones >> (7 - k) & 1
        for k in range(128): out[f + k] = out[f + k] & ~1 | bits[k]
    with open(path, "wb") as f: f.write(struct.pack(f"<{len(out)}h", *out))


async def main(e):
    page, conf, pv = e.page, e.conf, e.pv
    ha = await home_assistant(e)
    c, by = ha.cli, ha.by
    check("room_loudness" not in by and "room_speech" not in by and pv("room_loudness") == 0,
          "room loudness off by default: no sensors in Home Assistant")

    async def relist(old):                    # a feature switched on the page closes the link; HA lists again
        try: await old.disconnect()
        except Exception: pass
        n = APIClient("127.0.0.1", PORT, None); await asyncio.wait_for(n.connect(login=True), 5)
        es, _ = await n.list_entities_services(); sts = []; n.subscribe_states(sts.append); await sync(n)
        return n, {x.object_id: x for x in es}, sts
    page.set(room_loudness="on")
    c, cby, st = await relist(c)
    loud, sp, mute = cby.get("room_loudness"), cby.get("room_speech"), cby.get("mute")
    check(isinstance(loud, SensorInfo) and loud.unit_of_measurement == "dBFS" and int(loud.state_class) == 1
          and loud.accuracy_decimals == 0 and not loud.device_class
          and isinstance(sp, SensorInfo) and sp.unit_of_measurement == "%" and int(sp.state_class) == 1
          and conf().get("room_loudness") == "on",
          f"switched on on the page: Room loudness (dBFS) and Room speech (%), measurements, kept in the settings file: {conf()}")
    last = lambda k: ([x for x in st if isinstance(x, SensorState) and x.key == k] or [None])[-1]
    check(last(loud.key) is not None and last(loud.key).missing_state and last(sp.key).missing_state,
          "unknown until the first 5 s are in")
    await until(lambda: last(loud.key) and not last(loud.key).missing_state and not last(sp.key).missing_state, 8)
    l, s = last(loud.key), last(sp.key)
    check(l and not l.missing_state and abs(l.state - DB) <= 1, f"the loudness after 5 s: {l and l.state} dBFS ({DB} in the capture)")
    check(s and not s.missing_state and abs(s.state - 100 * SPEECH) <= 12,
          f"the speech share from the front end's records: {s and s.state} % ({100 * SPEECH:.0f} % of the capture, a part of a loop in 5 s)")
    n = len(st); await asyncio.sleep(11)
    moved = [x.state for x in st[n:] if isinstance(x, SensorState) and x.key == loud.key]
    check(not moved, f"a steady room: no new loudness state for HA's history: {moved}")

    # it stays out of the way: a request beside it runs as ever, its audio goes out unbroken, and the readings go on
    va = assistant(c); await sync(c)
    e.wake()
    await asyncio.wait_for(va.started.wait(), 5)
    await asyncio.sleep(1)                    # past the lead-in and the ring's replay, which come at once
    t0 = len(va.mic); await asyncio.sleep(2)
    got = (len(va.mic) - t0) / 32000
    check(1.8 <= got <= 2.2, f"a request while it measures: {got:.2f} s of audio in 2 s")
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await sync(c); await asyncio.sleep(0.3)
    check(not last(loud.key).missing_state, "the request has not made it unknown")
    # muted (Home Assistant's switch, as the button would): both unknown within a window, readings back after
    c.switch_command(mute.key, True)
    await until(lambda: last(loud.key).missing_state and last(sp.key).missing_state, 8)
    check(last(loud.key).missing_state and last(sp.key).missing_state, "muted: both unknown")
    c.switch_command(mute.key, False)
    await until(lambda: not last(loud.key).missing_state, 8)
    check(not last(loud.key).missing_state and abs(last(loud.key).state - DB) <= 1, "unmuted: the loudness back within a window")

    page.set(room_loudness="off")
    c, cby, st = await relist(c)
    check("room_loudness" not in cby and "room_speech" not in cby and conf().get("room_loudness") == "off",
          "switched off again: the sensors gone")


if __name__ == "__main__":
    cap = tempfile.mktemp(suffix=".raw"); capture(cap)
    try: run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT), {"HASSMIC_CAP": cap})
    finally: os.unlink(cap)
