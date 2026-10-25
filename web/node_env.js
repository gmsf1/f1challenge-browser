Error.stackTraceLimit = 400;
// --pre-js for the Node test build: pass RC_* environment variables to the game's getenv().
Module.preRun = Module.preRun || [];
Module.preRun.push(() => {
  ENV.RC_HEADLESS = '1';   // no page to render to
  for (const [k, v] of Object.entries(process.env)) if (k.startsWith('RC_')) ENV[k] = v;
});

// Netplay harness: RC_NP_URL=ws://.../netplay/<CODE>?ticket=... joins a room (web/netplay.js) and holds
// main() back until the host starts the session; RC_NP_HOST=<n> makes this peer the one that starts it
// once n players are in. RC_NP_DELAY (frames, or auto: the default) is the input delay the host asks for.
if (process.env.RC_NP_URL) {
  const np = require(require('path').join(__dirname, '../../web/netplay.js'));
  const want = Number(process.env.RC_NP_HOST || 0);
  let started = false;
  Module.preRun.push(() => {
    addRunDependency('netplay');
    const client = np.connect({
      url: process.env.RC_NP_URL,
      name: process.env.RC_NP_NAME || 'node',
      version: process.env.RC_NP_VERSION || 'test',
      onLobby(l) {
        console.error(`[netplay] lobby: you are player ${l.you}, ${l.players.length} in the room`);
        // RC_NP_DELAY=auto (default) waits until every player has reported a round trip to the room
        const d = process.env.RC_NP_DELAY || 'auto';
        if (want && l.host && !started && l.players.length >= want && (d !== 'auto' || l.players.every((p) => p.rtt > 0)))
          client.start(d === 'auto' ? np.autoDelay(l.players) : Number(d));
      },
      onStart(s) {
        started = true;
        console.error(`[netplay] session start: ${s.players} players, delay ${s.delay}, you are ${s.you}`);
        Object.assign(ENV, np.gameEnv(s));
        removeRunDependency('netplay');
      },
      onLeft(slot) { console.error(`[netplay] player ${slot} left`); },
      onReconnect(state) { console.error(`[netplay] room connection: ${state}`); },
      onError(e) { console.error(`[netplay] ${e}`); process.exit(2); },
    });
    const prev = Module.onRuntimeInitialized;
    Module.onRuntimeInitialized = () => {
      prev?.();
      client.attach(Module);
      // test: RC_NP_DROP_AT=<seconds> drops this peer's room connection once, mid-game
      if (process.env.RC_NP_DROP_AT) setTimeout(() => { console.error('[netplay] test: dropping the room connection'); client.testDrop(); }, Number(process.env.RC_NP_DROP_AT) * 1000);
    };
  });
}

// Virtual LAN harness (host/winsock.c, server/lan_relay.py): RC_LAN_URL=ws://localhost:8765/<room> joins a room before the
// game starts and bridges its sockets to it, so a headless instance can host or join a game next to a browser tab.
if (process.env.RC_LAN_URL) {
  const queue = [];
  const deliver = (f) => {
    if (!Module._lan_rx) { queue.push(f); return; }
    const p = Module._lan_alloc(f.length) >>> 0;
    new Uint8Array(wasmMemory.buffer, p, f.length).set(f);
    Module._lan_rx(p, f.length);
    Module._lan_free(p);
  };
  Module.preRun.push(() => {
    addRunDependency('lan');
    const ws = new WebSocket(process.env.RC_LAN_URL);
    ws.binaryType = 'arraybuffer';
    let id = 0;
    Module.lanSend = (b) => { if (ws.readyState === 1) ws.send(b); };
    ws.onerror = () => { console.error('[lan] could not reach the relay'); process.exit(2); };
    ws.onmessage = (e) => {
      const f = new Uint8Array(e.data);
      if (!id && f[0] === 0) {
        id = f[1];
        ENV.RC_LAN = '1'; ENV.RC_LAN_ID = String(id);
        console.error(`[lan] you are player ${id} (10.77.0.${id})`);
        removeRunDependency('lan');
        const t = setInterval(() => { if (Module._lan_rx) { clearInterval(t); queue.splice(0).forEach(deliver); } }, 20);
        return;
      }
      deliver(f);
    };
  });
}
