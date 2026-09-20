import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

// https://vitejs.dev/config/
export default defineConfig(async () => ({
  plugins: [react()],
  // Vite options tailored for Tauri to prevent too much magic
  // inline to 8kb limit is.
  build: {
    target: ['es2021', 'chrome100', 'safari13'],
    minify: !process.env.TAURI_DEBUG ? 'es' : false,
    sourcemap: !!process.env.TAURI_DEBUG,
  },
  server: {
    strictPort: true,
  },
}));
