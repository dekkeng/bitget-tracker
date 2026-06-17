# Automated cookie refresh (GitHub Actions)

Keeps the Bitget session cookie alive automatically. The GitHub Actions workflow
runs twice a day, verifies the session is still authenticated, and pushes the
renewed cookie to your Render tracker. If the session has expired, the Action
fails and GitHub emails you — the signal to re-paste a fresh cookie manually.

---

## What it actually does

```
GitHub Actions (cron: 00:00 and 12:00 UTC)
  │
  │  1. Pull current cookie
  │     GET /api/poller/cookie/export  (X-Sync-Token header required)
  │
  │  2. Open headless Chromium, load cookies, navigate to bitget.com
  │
  │  3. Call a real authenticated Bitget endpoint to verify the session
  │     POST /v1/trace/mt5/trace/getFollowPortfolios (credentials: include)
  │     ✅ code 00000 → session alive → save + push renewed cookie
  │     ❌ "Log in expired" / code 00004 → FAIL (Action fails → email)
  │
  │  4. Push verified-fresh cookie
  │     POST /api/poller/cookie
  ▼
Render tracker — dashboard keeps working
```

**Important:** the Action verifies auth with a real API call, not just by
checking that the cookie string exists. A dead cookie looks the same as a
live one until you actually ask Bitget about it. Only a verified-working
cookie is pushed back.

**Whether this keeps the cookie alive indefinitely** depends on Bitget's
server behaviour — specifically, whether navigating to the site while logged
in slides the expiry window forward. If it does, the Action is fully
automatic. If Bitget uses a fixed-TTL JWT that doesn't extend on activity,
the Action will fail once every ~5 days (around expiry) and email you to
re-paste manually. Either way you get proactive notification rather than
silently stale data.

---

## One-time setup (5 minutes)

### 1. Generate a sync token

The token protects the cookie-export endpoint. Anyone with it can pull your
live session cookie, so keep it private.

```bash
openssl rand -hex 32
```

Copy the output (a 64-character hex string).

**Alternative if openssl isn't available:**
```bash
python3 -c "import secrets; print(secrets.token_hex(32))"
```

### 2. Add the token to Render

In the Render dashboard for your service:
**Environment → Add environment variable**

| Key | Value |
|-----|-------|
| `COOKIE_SYNC_TOKEN` | the token from step 1 |

Without this the export endpoint returns 404 and stays disabled.
Redeploy the service after adding it.

### 3. Add secrets to GitHub

In your GitHub repo:
**Settings → Secrets and variables → Actions → New repository secret**

Add **both** of these:

| Name | Value |
|------|-------|
| `TRACKER_URL` | `https://YOUR-SERVICE-NAME.onrender.com` |
| `COOKIE_SYNC_TOKEN` | the same token from step 1 |

These are **Repository secrets** (not environment secrets). The values are
never exposed in logs even on a public repo.

### 4. Paste a valid cookie first

The Action can only *renew* a living session — it cannot log in from scratch.
Before the first Action run, paste a fresh cookie manually:

1. Log in to [bitget.com](https://bitget.com) in Chrome
2. Get the cookie — **either method works:**
   - **Console:** open DevTools → Console → run `copy(document.cookie)`
   - **Network tab:** DevTools → Network → reload → click any bitget.com request
     → Headers → Request Headers → find `cookie:` → copy the value
3. Open your dashboard → **Polling Setup** → paste → Save

### 5. Test the workflow

Trigger it manually to confirm everything is wired up before the first cron run:

**GitHub → Actions tab → "Refresh Bitget cookie" → Run workflow**

Check the logs under the **"Refresh and push cookie"** step:
- ✅ `[Login] session verified authenticated` → working correctly
- ❌ `[Login] session NOT authenticated` → the cookie you pasted was already
  dead — paste a fresh one and re-run

---

## When the Action fails (what to do)

If the session has genuinely expired, the Action fails and GitHub sends you
an email titled something like *"[dOHSI8/bitget-tracker] Run failed"*.

**Fix (takes ~2 minutes):**

1. Re-paste a fresh cookie from Chrome DevTools (Step 4 above)
2. Optionally re-run the workflow manually to confirm it's working again

**You do NOT need to use the local CLI login tool** unless you want to.
Pasting the cookie via the dashboard is the simplest path.

---

## Local CLI login (advanced / fallback)

If you prefer automating from your own machine instead of GitHub Actions, or
if the Action keeps failing (e.g. Bitget blocks GitHub's datacenter IPs):

```bash
cd headless
cp .env.example .env        # first time only
# edit .env — fill in:
#   TRACKER_URL=https://YOUR-SERVICE-NAME.onrender.com
#   COOKIE_SYNC_TOKEN=<your token>
#   BITGET_PHONE=<your phone number>      # optional — enables auto-login
#   BITGET_PASSWORD=<your password>       # optional

npm install

# Option A: silent refresh (headless, no phone needed — works if session alive)
npm run refresh

# Option B: full auto-login (opens a browser, types credentials, waits for
#            you to approve in the Bitget app, then pushes the cookie)
npm run login
npm run push-cookie
```

`npm run refresh` does the same thing as the GitHub Action.
`npm run login` is for when the session is truly dead and you need full auth.

---

## Cron schedule

The workflow runs at **00:00 and 12:00 UTC** (07:00 and 19:00 Bangkok time).
Cookie TTL is ~5 days, so twice-daily checks are more than sufficient.

You can also trigger it anytime:
**GitHub → Actions → "Refresh Bitget cookie" → Run workflow**

---

## Security notes

- `COOKIE_SYNC_TOKEN` is passed only via the `X-Sync-Token` **header** —
  never as a URL query param, which would appear in server logs.
- `headless/data/cookies.txt` (the local session file) is gitignored — never
  committed.
- If `COOKIE_SYNC_TOKEN` leaks: change it in both Render and GitHub Secrets,
  and paste a fresh cookie. The old token immediately stops working.
- The export endpoint (`GET /api/poller/cookie/export`) is disabled by default
  (returns 404) unless `COOKIE_SYNC_TOKEN` is set on the server.
