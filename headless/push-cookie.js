/**
 * Push the locally-saved cookie (data/cookies.txt) to the tracker.
 * Run this after a manual `npm run login` to upload the fresh session.
 */
const path = require('path');

try {
    require('dotenv').config({ path: path.resolve(__dirname, '.env') });
} catch { /* optional */ }

const { readLocalCookies } = require('./login');
const { cookiesToString, pushCookieToTracker } = require('./cookie-bridge');

async function main() {
    const trackerUrl = (process.env.TRACKER_URL || '').replace(/\/$/, '');
    if (!trackerUrl) {
        console.error('[Push] TRACKER_URL not set');
        process.exit(1);
    }
    let cookieStr;
    try {
        cookieStr = cookiesToString(readLocalCookies());
    } catch (err) {
        console.error('[Push] could not read local cookies — run `npm run login` first:', err.message);
        process.exit(1);
    }
    try {
        const len = await pushCookieToTracker(trackerUrl, cookieStr);
        console.log(`[Push] ✅ pushed cookie to tracker (${len} chars)`);
        process.exit(0);
    } catch (err) {
        console.error('[Push] failed:', err.message);
        process.exit(1);
    }
}

main();
