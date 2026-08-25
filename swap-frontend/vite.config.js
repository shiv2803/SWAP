import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';

// This config is an ES module ("type": "module"), where __dirname is not
// defined -- Vite only shims it while bundling the config, which is what the
// __dirname warning was about. Derive it from import.meta.url instead.
const rootDir = path.dirname(fileURLToPath(import.meta.url));

// https://vite.dev/config/
export default defineConfig({
  plugins: [react(), tailwindcss()],
  // Vite otherwise always claims 5173, which collides when more than one
  // dev server runs against this folder. Honouring PORT lets a supervisor
  // assign a free port; the 5173 default is unchanged when PORT is unset.
  server: {
    port: Number(process.env.PORT) || 5173,
    host: true,
    // The dashboard is reloaded constantly while chasing a link problem, and a
    // cached bundle pointing at an old backend IP looks exactly like a backend
    // failure. Never let this build be cached.
    headers: { 'Cache-Control': 'no-store, no-cache, must-revalidate' },
  },
  preview: {
    port: Number(process.env.PREVIEW_PORT) || 4173,
    host: true,
    headers: { 'Cache-Control': 'no-store, no-cache, must-revalidate' },
  },
  resolve: {
    alias: {
      '@': path.resolve(rootDir, './src'),
    },
  },
});
