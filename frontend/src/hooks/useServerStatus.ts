// =============================================================================
// useServerStatus
//
// Checks whether the ForgeKV backend is reachable by calling GET /health.
// Runs once on mount. Does not poll continuously.
// =============================================================================

import { useEffect, useState } from "react";
import { getHealth } from "../services/api";

export type ServerStatus = "loading" | "connected" | "offline";

export interface UseServerStatusResult {
  status: ServerStatus;
  /** Re-run the health check manually (e.g. after a user-triggered refresh). */
  refresh: () => void;
}

export function useServerStatus(): UseServerStatusResult {
  const [status, setStatus] = useState<ServerStatus>("loading");

  async function check() {
    setStatus("loading");
    const result = await getHealth();
    setStatus(result.ok ? "connected" : "offline");
  }

  useEffect(() => {
    check();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return { status, refresh: check };
}
