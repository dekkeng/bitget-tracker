/**
 * Refresh the Bitget session and push the renewed cookie to the tracker.
 *
 * Designed to run both locally and on GitHub Actions (stateless). The tracker
 * is the single source of truth for the cookie:
 *
 *   1. If no local cookie file exists, pull the current cookie from the tracker
 *      (token-protected /api/poller/cookie/export) and seed data/cookies.txt.
 *   2. Run a headless silent refresh (tryRefreshSession). If the session is
 *      still valid, Bitget hands back fresh cookies with extended expiry — no
 *      phone approval needed.
 *   3. Push the refreshed cookie back to the tracker so the live dashboard and
 *      the next run both pick it up.
 *
 * Exits 0 on success, 1 on failure. A non-zero exit makes the GitHub Action
 * fail, which emails you so you know to run a full `npm run login` by hand.
 */
const fs = require('fs');
const path = require('path');

try {
    require('dotenv').config({ path: path.resolve(__dirname, '.env') });
} catch { /* optional */ }

const { tryRefreshSession, COOKIE_PATH, readLocalCookies } = require('./login');
const {
    cookiesToString,
    stringToCookies,
    pullCookieFromTracker,
    pushCookieToTracker,
    writeLocalCookies,
} = require('./cookie-bridge');

async function main() {
    const trackerUrl = (process.env.TRACKER_URL || '').replace(/\/$/, '');
    const token = process.env.COOKIE_SYNC_TOKEN || '';

    if (!trackerUrl) {
        console.error('[Refresh] TRACKER_URL not set');
        process.exit(1);
    }

    // 1. Seed the local cookie file from the tracker if we have no local state
    //    (always the case on a fresh GitHub Actions runner).
    if (!fs.existsSync(COOKIE_PATH)) {
        if (!token) {
            console.error('[Refresh] no local cookie and COOKIE_SYNC_TOKEN not set — cannot pull from tracker');
            process.exit(1);
        }
        console.log('[Refresh] no local cookie — pulling current cookie from tracker...');
        try {
            const cookieStr = await pullCookieFromTracker(trackerUrl, token);
            writeLocalCookies(stringToCookies(cookieStr));
            console.log(`[Refresh] seeded local cookies (${cookieStr.length} chars)`);
        } catch (err) {
            console.error('[Refresh] could not pull cookie from tracker:', err.message);
            process.exit(1);
        }
    }

    // 2. Silent headless refresh.
    const result = await tryRefreshSession();
    if (!result.success) {
        console.error('[Refresh] silent refresh failed — a full `npm run login` is required');
        process.exit(1);
    }

    // 3. Push the renewed cookie back to the tracker.
    try {
        const cookies = readLocalCookies();
        const cookieStr = cookiesToString(cookies);
        const len = await pushCookieToTracker(trackerUrl, cookieStr);
        console.log(`[Refresh] pushed renewed cookie to tracker (${len} chars)`);
        if (result.newExpiry) console.log(`[Refresh] new expiry: ${result.newExpiry}`);
        console.log('[Refresh] ✅ done');
        process.exit(0);
    } catch (err) {
        console.error('[Refresh] could not push cookie to tracker:', err.message);
        process.exit(1);
    }
}

main().catch((err) => {
    console.error('[Refresh] unexpected error:', err);
    process.exit(1);
});
