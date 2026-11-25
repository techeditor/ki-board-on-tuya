import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import { readFileSync } from "fs";
import { resolve } from "path";

// Tauri expects a fixed port and ignores Vite's HMR websocket in production.
export default defineConfig({
  plugins: [react()],
  clearScreen: false,
  define: {
    // Single source of truth for the displayed version (companion/VERSION).
    __APP_VERSION__: JSON.stringify(readFileSync(resolve(__dirname, "VERSION"), "utf8").trim()),
  },
  server: {
    port: 1420,
    strictPort: true,
  },
  build: {
    rollupOptions: {
      input: {
        // Production Tauri entry.
        main: resolve(__dirname, "index.html"),
      },
    },
  },
});
