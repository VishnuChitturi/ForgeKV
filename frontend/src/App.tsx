import { Navigate, Route, Routes } from "react-router-dom";
import { AppLayout } from "./layouts/AppLayout";
import { DashboardPage } from "./pages/DashboardPage";
import { KeysPage } from "./pages/KeysPage";
import { AdminPage } from "./pages/AdminPage";

export function App() {
  return (
    <Routes>
      <Route element={<AppLayout />}>
        {/* Root → Dashboard */}
        <Route index element={<DashboardPage />} />
        <Route path="keys"  element={<KeysPage />} />
        <Route path="admin" element={<AdminPage />} />

        {/* Catch-all: redirect unknown paths to Dashboard */}
        <Route path="*" element={<Navigate to="/" replace />} />
      </Route>
    </Routes>
  );
}
