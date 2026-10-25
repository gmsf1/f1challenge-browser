/* Relay for the virtual LAN (host/winsock.c); the same protocol as server/lan_relay.py, one Durable Object per room.
 *
 * Binary frames (little endian): kind u8, peer u8, sport u16, dport u16, conn u32, payload. A client sends peer = the
 * player to deliver to (0 = everyone else in the room); the relay rewrites it to the sender's id. On connect the relay
 * sends kind 0 with peer = the id assigned (1-16). */
import { DurableObject } from "cloudflare:workers";

const MAX_PLAYERS = 16;
const MAX_FRAME = 16 * 1024;
const STATUS_TTL_MS = 20_000;
const STATUS_PROBE = "\\status\\";
const STATUS_REPLY = "\\gamename\\";

const startsWith = (b: Uint8Array, s: string, at = 10) => b.length >= at + s.length && [...s].every((c, i) => b[at + i] === c.charCodeAt(0));

export class Room extends DurableObject {
  peers = new Map<number, WebSocket>();
  // a game host's last \status\ answer, keyed "<host id>:<port>": a host in a background tab replies seconds late,
  // after the searching game has stopped listening, so the relay answers probes from here
  status = new Map<string, { reply: Uint8Array; t: number }>();

  async fetch(req: Request): Promise<Response> {
    let pid = 0;
    for (let i = 1; i <= MAX_PLAYERS && !pid; i++) if (!this.peers.has(i)) pid = i;
    if (!pid) return new Response("room full", { status: 503 });
    const { 0: client, 1: server } = new WebSocketPair();
    server.accept();
    server.binaryType = "arraybuffer";
    this.peers.set(pid, server);
    server.send(new Uint8Array([0, pid, 0, 0, 0, 0, 0, 0, 0, 0]));
    server.addEventListener("message", (ev) => {
      const d = ev.data as unknown;
      if (d instanceof ArrayBuffer) this.onFrame(pid, server, new Uint8Array(d));
      else if (ArrayBuffer.isView(d)) this.onFrame(pid, server, new Uint8Array(d.buffer, d.byteOffset, d.byteLength));
      else server.send(`bad message type: ${typeof d} ${Object.prototype.toString.call(d)}`);
    });
    const leave = () => {
      if (this.peers.get(pid) !== server) return;
      this.peers.delete(pid);
      for (const k of this.status.keys()) if (k.startsWith(pid + ":")) this.status.delete(k);
    };
    server.addEventListener("close", leave);
    server.addEventListener("error", leave);
    return new Response(null, { status: 101, webSocket: client });
  }

  onFrame(pid: number, ws: WebSocket, msg: Uint8Array) {
    if (msg.length < 10 || msg.length > MAX_FRAME) return;
    const dst = msg[1];
    const sport = msg[2] | (msg[3] << 8), dport = msg[4] | (msg[5] << 8);
    if (msg[0] === 1 && startsWith(msg, STATUS_REPLY)) {
      this.status.set(`${pid}:${sport}`, { reply: msg.subarray(10).slice(), t: Date.now() });
    } else if (msg[0] === 1 && msg.length === 10 + STATUS_PROBE.length && startsWith(msg, STATUS_PROBE)) {
      let fresh = true, answered = false;
      for (const [k, { reply, t }] of this.status) {
        const [hid, hport] = k.split(":").map(Number);
        if (hport !== dport || !this.peers.has(hid) || (dst !== 0 && dst !== hid)) continue;
        const out = new Uint8Array(10 + reply.length);
        out.set([1, hid, msg[4], msg[5], msg[2], msg[3], 0, 0, 0, 0]);
        out.set(reply, 10);
        ws.send(out);
        answered = true;
        fresh &&= Date.now() - t < STATUS_TTL_MS;
      }
      if (answered && fresh) return;   // only bother the host when its entry has gone stale
    }
    const out = msg.slice();
    out[1] = pid;
    const targets = dst === 0 ? [...this.peers].filter(([i]) => i !== pid).map(([, w]) => w) : dst === pid ? [ws] : this.peers.has(dst) ? [this.peers.get(dst)!] : [];
    for (const w of targets) {
      try { w.send(out); } catch { /* peer is closing */ }
    }
  }
}

interface Env {
  ROOM: DurableObjectNamespace<Room>;
  GAME: R2Bucket;          // the owner's own copy of the game (cloud/upload_game.py), never public
  GAME_TOKEN?: string;     // secret: Authorization: Bearer <token> unlocks /game/*
}

const originOk = (o: string) => /^https:\/\/f1challenge\.[a-z0-9-]+\.workers\.dev$/.test(o) || o === "http://localhost:8007";

async function tokenOk(env: Env, req: Request): Promise<boolean> {
  const given = (req.headers.get("Authorization") || "").replace(/^Bearer /, "");
  if (!env.GAME_TOKEN || !given) return false;
  const sha = async (t: string) => new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(t)));
  const [a, b] = await Promise.all([sha(given), sha(env.GAME_TOKEN)]);
  const subtle = crypto.subtle as any;
  return subtle.timingSafeEqual ? subtle.timingSafeEqual(a, b) : a.every((x: number, i: number) => x === b[i]);
}

async function game(req: Request, env: Env): Promise<Response> {
  const origin = req.headers.get("Origin") || "";
  const cors: Record<string, string> = originOk(origin) ? { "Access-Control-Allow-Origin": origin, "Access-Control-Allow-Headers": "Authorization", Vary: "Origin" } : {};
  if (req.method === "OPTIONS") return new Response(null, { status: 204, headers: cors });
  if (!env.GAME_TOKEN) return new Response("not enabled", { status: 503, headers: cors });
  if (!(await tokenOk(env, req))) return new Response("unauthorized", { status: 401, headers: cors });
  const key = decodeURIComponent(new URL(req.url).pathname.slice("/game/".length));
  const obj = key ? await env.GAME.get(key) : null;
  if (!obj) return new Response("not found", { status: 404, headers: cors });
  return new Response(obj.body, { headers: { ...cors, "Content-Length": String(obj.size), "Cache-Control": "private, no-store", "Content-Type": key.endsWith(".json") ? "application/json" : "application/octet-stream" } });
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    if (new URL(req.url).pathname.startsWith("/game/")) return game(req, env);
    if (req.headers.get("Upgrade") !== "websocket") return new Response("F1 Challenge LAN relay. Connect with a WebSocket to /<room>.\n");
    const room = decodeURIComponent(new URL(req.url).pathname.slice(1)).slice(0, 48) || "lobby";
    return env.ROOM.get(env.ROOM.idFromName(room)).fetch(req);
  },
};
