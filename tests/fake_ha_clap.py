#!/usr/bin/env python3
"""Home Assistant's side of the ESPHome native API against build/hassmic-host (tests/esphome_common.py): claps.
The capture is a room at -60 dBFS with two claps, then three, looped (clap.c).
  - "Clap" (setting clap_detection): off by default, no entity; on, its event types, double_clap and triple_clap as
    the loop plays them; none while a request runs or while muted"""
import asyncio, math, os, random, signal, struct, tempfile
from aioesphomeapi import APIClient, EventInfo, VoiceAssistantEventType as Ev
from aioesphomeapi.model import Event
from esphome_common import assistant, check, until, sync, home_assistant, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17201, 17202, 17203, 17204, 17205
RATE = 16000


def capture(path):
    """1.5 s of room, two claps 300 ms apart, 2 s of room, three 250 ms apart, 2 s of room: 6.5 s, looped"""
    rnd = random.Random(1); n = int(6.5 * RATE)
    a = lambda db: 32768 * 10 ** (db / 20) * math.sqrt(3)
    s = [rnd.uniform(-1, 1) * a(-60) for _ in range(n)]
    for t in (1.5, 1.8, 4.0, 4.25, 4.5):
        at = int(t * RATE)
        for i in range(RATE // 10): s[at + i] += rnd.uniform(-1, 1) * a(-20) * math.exp(-i / (0.005 * RATE))
    with open(path, "wb") as f: f.write(struct.pack(f"<{n}h", *(max(-32768, min(32767, int(v))) for v in s)))


async def main(e):
    ha = await home_assistant(e)
    c, by = ha.cli, ha.by
    async def relist(old):
        try: await old.disconnect()
        except Exception: pass
        n = APIClient("127.0.0.1", PORT, None); await asyncio.wait_for(n.connect(login=True), 5)
        es, _ = await n.list_entities_services(); sts = []; n.subscribe_states(sts.append)
        va = assistant(n); await sync(n)
        return n, {x.object_id: x for x in es}, sts, va

    # Claps: off by default
    check("clap" not in by and e.pv("clap_detection") == 0, "clap detection off by default: no entity in Home Assistant")
    await asyncio.sleep(7)
    check("clap:" not in e.log(), "off, the claps in the room are not looked for")
    e.page.set(clap_detection="on")
    c, by, st, va = await relist(c)
    ev = by.get("clap")
    check(isinstance(ev, EventInfo) and list(ev.event_types) == ["double_clap", "triple_clap"] and e.conf().get("clap_detection") == "on",
          f"clap detection switched on on the page: the event entity, kept in the settings file: {ev}")
    claps = lambda: [x.event_type for x in st if isinstance(x, Event) and ev and x.key == ev.key]
    await until(lambda: "double_clap" in claps() and "triple_clap" in claps(), 16)
    got = claps()
    check("double_clap" in got and "triple_clap" in got and all(a != b for a, b in zip(got, got[1:])),
          f"two claps and three as the loop plays them, by turns ({got})")

    # none while a request runs (the wake word's pipeline listens: the room is the request's)
    va.started.clear(); e.wake(); await asyncio.wait_for(va.started.wait(), 5)
    n0 = len(claps()); await asyncio.sleep(7)
    check(len(claps()) == n0, f"no claps while the Echo listens ({claps()[n0:]})")
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "stt-no-text-recognized", "message": "No text recognized"})
    await until(lambda: len(claps()) > n0, 16)
    check(len(claps()) > n0, "idle again: claps are told again")

    # muted: the Echo does not listen for claps at all
    c.switch_command(by["mute"].key, True); await asyncio.sleep(0.5)
    n0 = len(claps()); await asyncio.sleep(8)
    c.switch_command(by["mute"].key, False); await until(lambda: "soft mute: 0" in e.log())
    log = e.log(); muted_log = log[log.rindex("soft mute: 1"):log.rindex("soft mute: 0")]
    check(len(claps()) == n0 and "in a row" not in muted_log, f"muted: no claps looked for, none told ({claps()[n0:]})")
    await until(lambda: len(claps()) > n0, 16)
    check(len(claps()) > n0, "unmuted: claps are told again")


cap = tempfile.mktemp(suffix=".raw"); capture(cap)
try:
    run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT), {"HASSMIC_CAP": cap}, log=True)
finally:
    os.unlink(cap)
