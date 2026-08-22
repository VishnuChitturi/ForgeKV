# Stage 14 — Frontend Foundation

## Overview

Stage 14 introduces the ForgeKV web frontend: a local developer dashboard for
visualising and interacting with a running ForgeKV server. The goal of this stage
is to establish a clean, maintainable architecture that future stages build on.
No actual data features are implemented yet — all pages display placeholders.

---

## Technology stack

| Concern          | Choice                      | Version  | Reason                                                |
|------------------|-----------------------------|----------|-------------------------------------------------------|
| Build tool       | Vite                        | 5.x      | Fast HMR, native ESM, minimal config                  |
| UI library       | React                       | 18.x     | Widely used, good TypeScript support                  |
| Language         | TypeScript                  | 5.x      | Catches type errors at compile time                   |
| Routing          | React Router DOM            | 6.x      | Industry-standard client-side routing for React       |
| Styling          | CSS Modules                 | —        | Scoped styles, no runtime, no large framework dep     |

No large UI component framework (e.g. Material UI, Ant Design) is introduced.
CSS Modules provide scoped, maintainable styles with zero runtime overhead.

---

## Directory structure

```
frontend/
├── public/
│   └── favicon.svg
├── src/
│   ├── components/           # Reusable, stateless UI primitives
│   │   ├── Empty.tsx / .module.css
│   │   ├── ErrorMessage.tsx / .module.css
│   │   ├── Loading.tsx / .module.css
│   │   └── ServerStatus.tsx / .module.css
│   ├── hooks/                # Custom React hooks
│   │   └── useServerStatus.ts
│   ├── layouts/              # Page shells with navigation
│   │   ├── AppLayout.tsx
│   │   └── AppLayout.module.css
│   ├── pages/                # Top-level route components
│   │   ├── AdminPage.tsx
│   │   ├── DashboardPage.tsx
│   │   ├── KeysPage.tsx
│   │   └── Page.module.css
│   ├── services/             # API client (all fetch calls live here)
│   │   └── api.ts
│   ├── styles/               # Global CSS and design tokens
│   │   └── global.css
│   ├── types/                # TypeScript types for API contracts
│   │   └── api.ts
│   ├── App.tsx               # Route tree
│   ├── main.tsx              # React DOM entry point
│   └── vite-env.d.ts         # Vite client type reference
├── .env.example              # Template for local environment variables
├── .env.local                # Local dev overrides (git-ignored)
├── index.html                # Vite HTML entry point
├── package.json
├── tsconfig.json             # Project references root
├── tsconfig.app.json         # App source compiler options
├── tsconfig.node.json        # Vite config compiler options
└── vite.config.ts
```

---

## Installing dependencies

```bash
cd frontend
npm install
```

Node.js 18+ and npm 8+ are required.

---

## Running the frontend (development)

Start the ForgeKV backend first (defaults to port 8080):

```bash
# From the repo root
./build/forgekv_server
```

Then start the Vite dev server:

```bash
cd frontend
npm run dev
```

Open `http://localhost:5173` in your browser.

The Vite dev server proxies all `/api/*` requests to the ForgeKV backend,
so no CORS configuration is required on the C++ server.

---

## Building for production

```bash
cd frontend
npm run build
```

Output is written to `frontend/dist/`. Serve this directory with any static
file server. Ensure the server hosting the frontend can reach the ForgeKV
backend and that CORS is enabled if they are on different origins.

---

## Previewing the production build locally

```bash
npm run preview
```

Starts a local preview server at `http://localhost:4173`.

---

## TypeScript type check

```bash
npm run typecheck
```

Runs `tsc --noEmit` across both the app and node compiler configurations.
Zero errors expected.

---

## Configuring the API URL

The backend URL is configured via an environment variable:

| Variable            | Default                   | Description                          |
|---------------------|---------------------------|--------------------------------------|
| `VITE_API_BASE_URL` | `http://localhost:8080`   | Full origin of the ForgeKV backend   |

### Local development

Copy `.env.example` to `.env.local` and set `VITE_API_BASE_URL`:

```
VITE_API_BASE_URL=http://localhost:8080
```

`.env.local` is git-ignored so it is never committed.

### How the proxy works

In development, Vite proxies `/api/*` → `VITE_API_BASE_URL`. The frontend
code always calls `/api/health`, `/api/stats`, etc., and Vite rewrites these
to the configured backend origin transparently. This avoids all CORS issues
without touching the C++ server.

---

## How the frontend communicates with ForgeKV

All network calls go through `src/services/api.ts`. There is no scattered
`fetch()` usage in components or hooks.

```
Component / Hook
    │
    ▼
src/services/api.ts        (typed fetch wrappers, error normalisation)
    │
    ▼
Vite dev proxy  (dev only: /api/* → http://localhost:8080)
    │
    ▼
ForgeKV HTTP server        (GET /health, GET /stats, etc.)
```

Each function returns `ApiResult<T>`, a discriminated union:
```ts
type ApiResult<T> =
  | { ok: true;  data: T }
  | { ok: false; status: number; error: string }
```

Callers do not need `try/catch`. Network failures, non-2xx responses, and
JSON parse errors are all normalised to the `ok: false` branch.

---

## Available routes

| Path     | Component        | Description                          |
|----------|------------------|--------------------------------------|
| `/`      | DashboardPage    | Engine metrics overview (placeholder)|
| `/keys`  | KeysPage         | Key browser (placeholder)            |
| `/admin` | AdminPage        | Admin operations (placeholder)       |
| `/*`     | → redirect `/`   | Unknown paths redirect to Dashboard  |

All routes render inside `AppLayout`, which provides the top bar, sidebar
navigation, and `ServerStatusBadge`.

---

## Server connection indicator

`ServerStatusBadge` (in the top bar) calls `GET /health` once on mount via
the `useServerStatus` hook. It shows:

- **Checking…** (animated dot) — request in flight
- **Connected** (green dot) — `{"status":"ok"}` received
- **Offline** (red dot) — request failed or non-200 response

Clicking the badge re-runs the health check. No automatic polling is
implemented at this stage.

---

## Design foundation

Styles are defined in two layers:

1. **`src/styles/global.css`** — CSS custom properties (design tokens), reset,
   base typography, scrollbar styling.
2. **`*.module.css`** per component — scoped, component-local styles that
   reference the tokens.

Design tokens cover colours, spacing radii, font families, and layout
dimensions (sidebar width, top-bar height). Swapping a colour scheme in a
future stage requires changing the tokens in `global.css` only.

No dark mode is implemented yet. The layout is responsive: the sidebar
collapses to icon-only navigation below 640 px viewport width.

---

## Development assumptions

- The backend runs on `localhost:8080` during local development.
- The backend speaks plain HTTP (no TLS). TLS termination is outside scope.
- All key/value data is UTF-8 text. Binary-safe handling of edge-case
  characters is handled in the C++ backend; the frontend displays values
  as strings.
- Stage 14 does not implement the actual Dashboard, Keys, or Admin pages.
  Those are deferred to Stages 15+.

---

## Known limitations

- No dark mode.
- No automatic health polling (re-check is manual, on click).
- No error boundary in the React tree (not needed at this scale yet).
- The production build assumes the frontend is served at the root path `/`.
  If deployed under a sub-path, `vite.config.ts` `base` option must be set.
- Dashboard, Keys, and Admin pages show placeholder empty states only.
- No tests are written for the frontend at this stage.

---

## Backend changes required

**None.** The Vite development proxy eliminates all CORS issues without
modifying the C++ server. The backend is unchanged from Stage 13.
