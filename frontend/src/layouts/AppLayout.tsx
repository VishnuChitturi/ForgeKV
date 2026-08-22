import { NavLink, Outlet } from "react-router-dom";
import { useServerStatus } from "../hooks/useServerStatus";
import { ServerStatusBadge } from "../components/ServerStatus";
import styles from "./AppLayout.module.css";

interface NavItem {
  to: string;
  label: string;
  icon: string;
}

const NAV_ITEMS: NavItem[] = [
  { to: "/",      label: "Dashboard", icon: "⊞" },
  { to: "/keys",  label: "Keys",      icon: "⊟" },
  { to: "/admin", label: "Admin",     icon: "⊛" },
];

export function AppLayout() {
  const { status, refresh } = useServerStatus();

  return (
    <div className={styles.shell}>
      {/* ------------------------------------------------------------------ */}
      {/* Skip-to-main link — keyboard accessibility                         */}
      {/* ------------------------------------------------------------------ */}
      <a href="#main-content" className={styles.skipLink}>
        Skip to main content
      </a>

      {/* ------------------------------------------------------------------ */}
      {/* Top bar                                                             */}
      {/* ------------------------------------------------------------------ */}
      <header className={styles.topbar}>
        <div className={styles.brand}>
          <span className={styles.brandIcon} aria-hidden="true">⬡</span>
          <span className={styles.brandName}>ForgeKV</span>
          <span className={styles.brandVersion}>v0.13</span>
        </div>
        <div className={styles.topbarRight}>
          <ServerStatusBadge status={status} onRefresh={refresh} />
        </div>
      </header>

      {/* ------------------------------------------------------------------ */}
      {/* Body: sidebar + main                                                */}
      {/* ------------------------------------------------------------------ */}
      <div className={styles.body}>
        <nav className={styles.sidebar} aria-label="Main navigation">
          <ul className={styles.navList} role="list">
            {NAV_ITEMS.map(({ to, label, icon }) => (
              <li key={to}>
                <NavLink
                  to={to}
                  end={to === "/"}
                  className={({ isActive }) =>
                    `${styles.navItem} ${isActive ? styles.navItemActive : ""}`
                  }
                  aria-current={undefined}
                >
                  {({ isActive }) => (
                    <>
                      {/* aria-current="page" on the inner content div so
                          the NavLink itself carries the attribute naturally */}
                      <span className={styles.navIcon} aria-hidden="true">
                        {icon}
                      </span>
                      <span className={styles.navLabel}>{label}</span>
                      {isActive && (
                        <span className={styles.srOnly}>(current page)</span>
                      )}
                    </>
                  )}
                </NavLink>
              </li>
            ))}
          </ul>

          <footer className={styles.sidebarFooter}>
            <span className={styles.footerText}>ForgeKV&nbsp;·&nbsp;Stage&nbsp;19</span>
          </footer>
        </nav>

        <main className={styles.main} id="main-content">
          <Outlet />
        </main>
      </div>
    </div>
  );
}
