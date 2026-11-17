import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import { resolve } from "path";

// Tauri expects a fixed port and ignores Vite's HMR websocket in production.
export default defineConfig({
  plugins: [react()],
  clearScreen: false,
  server: {
    port: 1420,
    strictPort: true,
  },
  build: {
    rollupOptions: {
      input: {
        // Production Tauri entry.
        main: resolve(__dirname, "index.html"),
        // Standalone redesign preview (browser-reviewable without Tauri).
        redesign: resolve(__dirname, "redesign.html"),
      },
    },
  },
});
