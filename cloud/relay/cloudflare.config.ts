import { defineConfig, exports, bindings } from "cf/config";
import * as entrypoint from "./src/index.ts" with { type: "cf-worker" };

export default defineConfig({
  worker: {
    name: "f1challenge-relay",
    compatibilityDate: "2026-10-06",
    entrypoint,
    exports: {
      Room: exports.durableObject({ storage: "sqlite" }),
    },
    env: {
      ROOM: bindings.durableObject({ worker: "f1challenge-relay", exportName: "Room" }),
      GAME: bindings.r2({ name: "f1challenge-game" }),
    },
  },
});
