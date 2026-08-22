import { defineConfig, loadEnv } from "vite";
import react from "@vitejs/plugin-react";

// https://vitejs.dev/config/
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), "");
  const apiBaseUrl = env.VITE_API_BASE_URL ?? "http://localhost:8080";

  return {
    plugins: [react()],
    server: {
      proxy: {
        // In development, proxy all /api/* requests to the ForgeKV backend.
        // This avoids CORS issues without modifying the C++ server.
        "/api": {
          target: apiBaseUrl,
          changeOrigin: true,
          rewrite: (path) => path.replace(/^\/api/, ""),
        },
      },
    },
  };
});
