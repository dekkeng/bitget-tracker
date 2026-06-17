# Automated cookie refresh (Option B + GitHub Actions)

Keeps the Bitget session cookie alive so you stop manually pasting it every ~5
days. The Render tracker stays the single source of truth — nothing scrapes on
your laptop, and Render memory is unaffected.

## How it works

```
GitHub Actions (cron, twice a day)
  │
  │ 1. GET /api/poller/cookie/export   (current cookie, token-protected)
  ▼
headless Chromium → bitget.com  →  server re-issues fresh cookie
  │
  │ 2. POST /api/poller/cookie         (renewed cookie)
  ▼
Render tracker  →  dashboard keeps working, no manual paste
```

- **Silent refresh** needs no phone approval — it just re-loads the session.
- If the cookie has truly expired, the silent refresh fails, the Action fails,
  and GitHub **emails you**. Then you run a full login by hand (below).

## One-time setup

### 1. Pick a sync token
```bash
openssl rand -hex 32
```

### 2. Render — add an env var
In the Render dashboard for your service, add:

| Key | Value |
|-----|-------|
| `COOKIE_SYNC_TOKEN` | the token from step 1 |

This enables `GET /api/poller/cookie/export`. Without it, the export endpoint
stays disabled (returns 404).

### 3. GitHub — add repo secrets
Repo → Settings → Secrets and variables → Actions → New repository secret:

| Name | Value |
|------|-------|
| `TRACKER_URL` | `https://YOUR-SERVICE-NAME.onrender.com` |
| `COOKIE_SYNC_TOKEN` | the same token from step 1 |

### 4. Make sure the tracker already has a valid cookie
Paste a fresh cookie once via the dashboard (Polling Setup). The Action renews
from there on out.

That's it. The workflow (`.github/workflows/refresh-cookie.yml`) runs at 00:00
and 12:00 UTC. Trigger it manually any time from the **Actions** tab →
*Refresh Bitget cookie* → *Run workflow*.

## When the Action fails (full re-login)

If the silent refresh can't renew (session fully expired, or Bitget flags the
datacenter IP and wants app approval), do a full login from your laptop:

```bash
cd headless
cp .env.example .env        # first time only
# fill in TRACKER_URL, COOKIE_SYNC_TOKEN, BITGET_PHONE, BITGET_PASSWORD
npm install
npm run login               # opens browser, approve in Bitget app
npm run push-cookie         # uploads the fresh cookie to the tracker
```

Or do both in one step (`npm run refresh` pulls from the tracker, refreshes,
and pushes back) — but that only works while the session is still valid.

## Local-only (skip GitHub Actions)

You don't have to use GitHub Actions. Run `npm run refresh` on your laptop
whenever you like (e.g. a daily `cron`/Shortcuts job). Same effect, but only
while your machine is on.

## Notes

- `data/cookies.txt` (the saved session) is gitignored — never commit it.
- The export endpoint returns a live session secret. Keep `COOKIE_SYNC_TOKEN`
  private and rotate it if it leaks (change it in both Render and GitHub).
- Bitget may occasionally block GitHub's datacenter IPs. If silent refresh
  keeps failing there, run `npm run refresh` locally instead — the dashboard
  result is identical.
