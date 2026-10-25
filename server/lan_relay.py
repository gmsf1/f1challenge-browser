#!/usr/bin/env python3
"""Relay for the virtual LAN (host/winsock.c): players in the same room share 10.77.0.<id> addresses.

  python3 server/lan_relay.py [--port 8765]      then open the page with  ?lan=ws://<this machine>:8765&room=<name>

Binary frames (little endian): kind u8, peer u8, sport u16, dport u16, conn u32, payload. A client sends peer = the
player to deliver to (0 = everyone else in the room); the relay rewrites it to the sender's id. On connect the relay
sends kind 0 with peer = the id assigned (1-16); on disconnect it tells the others with FIN-less silence (the game
notices a dead peer itself).
"""
import argparse
import asyncio
import time

import websockets

rooms = {}   # name -> {id: websocket}
status = {}  # room -> {(host id, port): (reply, time)}: last \\status\\ answer of each game host
STATUS_TTL = 20  # seconds before a probe is passed on to the host again to refresh its answer
TRACE = False


async def handler(ws):
    name = (getattr(ws.request, "path", "/") or "/").strip("/") or "lobby"
    room = rooms.setdefault(name, {})
    pid = next((i for i in range(1, 17) if i not in room), None)
    if pid is None:
        await ws.close(1013, "room full")
        return
    room[pid] = ws
    print(f"[{name}] player {pid} joined ({len(room)} in room)", flush=True)
    await ws.send(bytes([0, pid, 0, 0, 0, 0, 0, 0, 0, 0]))
    try:
        async for msg in ws:
            if not isinstance(msg, (bytes, bytearray)) or len(msg) < 10:
                continue
            dst = msg[1]
            if TRACE:
                print(f"{time.time() % 1000:8.3f} [{name}] kind {msg[0]} {pid}->{dst or 'all'} {msg[2] | msg[3] << 8}->{msg[4] | msg[5] << 8} {len(msg) - 10}B {bytes(msg[10:34])!r}", flush=True)
            sport, dport, body = msg[2] | msg[3] << 8, msg[4] | msg[5] << 8, bytes(msg[10:])
            cache = status.setdefault(name, {})
            if msg[0] == 1 and body.startswith(b"\\gamename\\"):
                if TRACE and cache.get((pid, sport), (None,))[0] != body:
                    print(f"  status reply from {pid}:{sport}: {body!r}", flush=True)
                cache[(pid, sport)] = (body, time.time())
            elif msg[0] == 1 and body == b"\\status\\":
                # A host in a background tab answers seconds late, after the searcher stopped listening: answer from
                # the cache, and only bother the host when its entry has gone stale.
                fresh = True
                for (hid, hport), (reply, t) in list(cache.items()):
                    if hport == dport and hid in room and dst in (0, hid):
                        await ws.send(bytes([1, hid]) + msg[4:6] + msg[2:4] + bytes(4) + reply)
                        fresh &= time.time() - t < STATUS_TTL
                if fresh and any(k[1] == dport and dst in (0, k[0]) for k in cache):
                    continue
            out = bytes([msg[0], pid]) + bytes(msg[2:])
            targets = [w for i, w in room.items() if i != pid] if dst == 0 else ([room[dst]] if dst in room else [])
            if dst == pid:
                targets = [ws]
            for w in targets:
                try:
                    await w.send(out)
                except websockets.ConnectionClosed:
                    pass
    finally:
        room.pop(pid, None)
        for k in [k for k in status.get(name, {}) if k[0] == pid]:
            del status[name][k]
        print(f"[{name}] player {pid} left", flush=True)
        if not room:
            rooms.pop(name, None)


async def main(port):
    async with websockets.serve(handler, "0.0.0.0", port, max_size=1 << 20):
        print(f"LAN relay on ws://0.0.0.0:{port}/<room>")
        await asyncio.Future()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--trace", action="store_true", help="print every frame (kind, from, to, ports, size)")
    a = ap.parse_args()
    TRACE = a.trace
    asyncio.run(main(a.port))
