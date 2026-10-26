# F1 Challenge 99-02 in the browser

Browser port of F1 Challenge 99-02 (ISI/EA, Windows), using the static-recompilation approach of
[cricket07-browser](https://github.com/Dhaiwat10/cricket07-browser) (MIT, base of this tree): x86 exe →
C (Ghidra + `translator/x86toc.py`) → WebAssembly, with `host/` standing in for Windows
(D3D→WebGPU, DirectSound→Web Audio, DirectInput→gamepad/keyboard, files→OPFS).
No game files are included; bring your own copy.

## Status
Boots to the front end, loads Test Day and Practice sessions (tested on Albert Park) and renders the track, sky, pit lane and cars in Chrome
(WebGPU). Mouse and keyboard work in the menus and on track, and the driving keys and aids you set in the game are kept. Audio works through a small Miles
reimplementation (`host/miles.c`: WAV effects, MP3 music via minimp3). The game's own LAN multiplayer works between two browsers (see Multiplayer below).
Not done: movies (Bink is faked: 1-frame intros, endless menu backdrops) and internet play beyond a relay you run yourself. The game needs about 800 MB of guest heap.

You need a *readable* exe (the retail one is SafeDisc-encrypted; this project does not include or help remove copy protection) in
`work/exe_candidate/f1c.exe`, then run `ghidra/export.sh`, `x86toc.py` and `build_web.sh` (see below). The game data is the disc plus the `main.dmp` RAR
(extract it over the install; keep the corrected `Config.ini`: `CIRCUITS\`, `AUDIO\VEHICLE\`, `REPLAYS\`,
`VIDEOGUID=D7B71E3E-4212-11CF-077A-9A2106C2CB35`). Menus: click ✓ on Test Day, pick a circuit, ✓ again, then the drive flag.

Debug: `http://localhost:8007/?autoplay&log` (host log), `&trace` (each game function once), `&d3d=1`, `&fps`, and the `?rc_*` switches below.

## Multiplayer
`host/winsock.c` implements the game's Winsock calls on a virtual LAN. Players in one room get addresses `10.77.0.<id>`, and UDP broadcast, UDP and TCP
travel as frames over a WebSocket relay, so the game's own lobby (host, refresh the LAN list, join) runs unchanged.

1. Start the relay: `uv run --with websockets python server/lan_relay.py --port 8765` (add `--trace` to log every frame).
2. Open the page in two different browsers (tabs of one browser share one profile) with the same room:
   `http://localhost:8007/?autoplay&lan=ws://localhost:8765&room=f1`. The launcher's Play Multiplayer screen builds this URL for you.
3. In the game's Multiplayer menu, one player hosts. The other refreshes the LAN list and joins.

`&playername=Alice` renames the browser's profile, which is the name the lobby shows. The relay answers the game's `\status\` probes from the host's last
reply, because a host in a background tab responds too slowly for the searching game. Discovery and joining are tested between Edge and Chrome; racing
together over the relay has not been tested yet.

## Hosted demo
The page and the wasm build run as a static site on Cloudflare Workers (`cloud/site`, served at `https://f1challenge.<your-subdomain>.workers.dev`). Visitors pick their own game folder, which
is copied into their browser's private storage and never uploaded. The Multiplayer screen defaults to the hosted relay, a Worker with one Durable Object per room.

Deploy your own (needs `cf auth login`): run `build_web.sh`, then `cloud/prepare.sh`, then `cf deploy` in `cloud/site` (the page) and in `cloud/relay` (the relay).
The page needs cross-origin isolation for wasm threads, which `cloud/prepare.sh` sets up with a `_headers` file. The page finds the relay by name: `f1challenge.<sub>.workers.dev` talks to `f1challenge-relay.<sub>.workers.dev`. On a custom domain, set `PUBLIC_RELAY` in `web/loader.js`.

## Milestones
1. Get the exe → inspect imports (`d3d8/d3d9/ddraw`, `dsound`, `dinput`) to learn which graphics API F1C uses.
   If it's not D3D8, `host/d3d8.c` / `render_*.c` / `*shader.c` need the matching API surface.
2. Ghidra export → `work/functions.json` (`GHIDRA=... ghidra/export.sh game.exe work/functions.json`).
3. Fill in `host/profile.f1c.h` (exe name, install path, registry), then `PROFILE=host/profile.f1c.h ./build.sh game.exe work/functions.json` (native).
4. Boot to menu natively, then `PROFILE=host/profile.f1c.h ./build_web.sh` and `python3 web/serve.py`.
5. Optional: lockstep netplay from cricket07-browser (`server/`). F1C's AI and physics determinism is unverified, so the LAN bridge above is the supported route.

## Toolchain
Python venv in `.venv` (capstone, pefile). Still needed: Ghidra 12, wgpu-native in `third_party/wgpu`
(native build only), emcc (installed), SDL2 (installed).

## Debugging the renderer

All of these are environment variables natively and in Node; in the browser pass them as `?rc_<name>=<value>` (e.g. `?rc_no_cull=1`).
`RC_LOG_DRAWS=<frame>` prints every draw's state (and that frame's clears) in `[draw]`/`[clear]` lines; `RC_DUMP_TEX=<dir>` (with `RC_DUMP_LEVEL`) dumps
that frame's textures raw (`tools/texsheet.py` turns them into contact sheets); `RC_CLEAR_COLOR=ffff00ff` clears the colour buffer every frame so
undrawn pixels show; `RC_ONLY_DRAWS=a-b` / `RC_SKIP_DRAWS=a-b` bisect a frame's draws; `RC_NO_CULL=1` disables face culling; `RC_INPUT` scripts input
(also in the browser: `?rc_input=500:at=612:458,560:click,...`).
Headless runs write into the game folder (saves included), so run them on a copy.

## Credits
- **[Dhaiwat Pandya (Dhaiwat10)](https://github.com/Dhaiwat10) - [cricket07-browser](https://github.com/Dhaiwat10/cricket07-browser)** (MIT): the whole approach and the base of this tree:
  the x86-to-C translator, the runtime, the Windows/Direct3D/DirectInput/DirectSound host layer, the WebGPU renderer, the loader and the netplay server.
  This project adapts it to F1 Challenge 99-02.
- **Manvindar Singh**: the F1 Challenge port (profile, `host/f1c.c`, setjmp/longjmp unwinding, Bink/Winsock/cursor support,
  input fixes, culling and texture fixes, Miles audio, the LAN bridge and relay, the launcher).
- Built with [Ghidra](https://ghidra-sre.org/) (NSA, Apache-2.0), [Capstone](https://www.capstone-engine.org/) and [Unicorn](https://www.unicorn-engine.org/) (difftest),
  [Emscripten](https://emscripten.org/) with the emdawnwebgpu port (Dawn), wgpu-native for the native build, and [minimp3](https://github.com/lieff/minimp3) (CC0) for the music.
- *F1 Challenge '99-'02* is (c) Electronic Arts / Image Space Incorporated (ISI). Everyone who made the game: this is a preservation/compatibility project.

## Legal
No game files, disc images, cracked or decrypted executables are included or distributed. You must own the game and supply your own files.
Not affiliated with or endorsed by Electronic Arts, ISI or Microsoft. All trademarks belong to their owners.

## Defaults compiled into the game
The game builds its settings (driving aids, key bindings) from defaults in its code and doesn't always apply the player file in a session,
so `host/f1c.patches` changes those defaults at translation time (`translator/x86toc.py --patch-imm/--patch-reg`; run `tools/translate_f1c.sh`, then `build_web.sh`).
The Difficulty presets (function `0x4adf10`) overwrite the driving aids whenever the profile loads or a level is chosen, so the Beginner and Novice presets are patched too: Braking Point Assistance, Spin Recovery and Pit Lane Assistance start Off.
