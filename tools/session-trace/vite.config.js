import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { workxSessionApi } from './server/api.js';

// 单命令启动：`npm run dev` 同时提供 React 前端 + /api 后端（读取 ~/.workx/projects）
export default defineConfig({
  plugins: [react(), workxSessionApi()],
  server: {
    port: 5173,
    open: true,
  },
});
