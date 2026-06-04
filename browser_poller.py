import asyncio
import json
import logging
import os
from datetime import datetime, timezone, timedelta
from pathlib import Path
from typing import Callable

logger = logging.getLogger(__name__)

BKK = timezone(timedelta(hours=7))
PORTFOLIO_ID = os.environ.get("PORTFOLIO_ID", "1443199880395776000")
BITGET_BASE = "https://www.bitget.com"
POLL_INTERVAL = int(os.environ.get("POLL_INTERVAL_SEC", "120"))
COOKIES_FILE = Path(os.environ.get("COOKIES_PATH", "cookies.json"))

# Minimal Chrome flags — reduce memory as much as possible
CHROMIUM_ARGS = [
    "--no-sandbox", "--disable-setuid-sandbox", "--disable-dev-shm-usage",
    "--disable-gpu", "--single-process", "--no-zygote",
    "--disable-extensions", "--disable-background-networking",
    "--disable-default-apps", "--disable-sync", "--no-first-run",
    "--mute-audio", "--disable-hang-monitor",
    "--disable-features=TranslateUI,site-per-process",
    "--js-flags=--max-old-space-size=64",
    "--enable-low-end-device-mode",
]

_status = {
    "running": False,
    "browser_alive": False,
    "last_poll": None,
    "last_scrape": None,
    "last_error": None,
    "polls": 0,
    "scrapes": 0,
}


def get_status() -> dict:
    cookie_str = _load_cookie_string()
    return {
        **_status,
        "has_cookie": bool(cookie_str),
        "cookie_preview": (cookie_str[:40] + "...") if len(cookie_str) > 40 else cookie_str,
        "poll_interval_sec": POLL_INTERVAL,
    }


def _load_cookie_string() -> str:
    if COOKIES_FILE.exists():
        try:
            data = json.loads(COOKIES_FILE.read_text())
            val = data.get("cookie", "")
            if val:
                return val
        except (json.JSONDecodeError, OSError):
            pass
    return os.environ.get("BITGET_COOKIE", "")


def _parse_cookie_string(cookie_str: str) -> list[dict]:
    cookies = []
    for pair in cookie_str.split("; "):
        if "=" not in pair:
            continue
        name, _, value = pair.partition("=")
        name = name.strip()
        if not name:
            continue
        cookies.append({"name": name, "value": value, "domain": ".bitget.com", "path": "/"})
    return cookies


async def start_poller(push_fn: Callable):
    _status["running"] = True
    await asyncio.sleep(3)

    try:
        from playwright.async_api import async_playwright  # noqa: F401
    except ImportError:
        logger.error("Playwright not installed — poller disabled")
        _status["last_error"] = "Playwright not installed"
        return

    while True:
        cookie_str = _load_cookie_string()
        if not cookie_str:
            _status["last_error"] = "No cookie set"
            _status["browser_alive"] = False
            await asyncio.sleep(10)
            continue

        _status["last_error"] = None
        try:
            await _poll_once(push_fn, cookie_str)
        except Exception as e:
            logger.error("Poll cycle crashed: %s", e)
            _status["last_error"] = f"Poll error: {e}"

        _status["browser_alive"] = False
        logger.info("Next poll in %ds...", POLL_INTERVAL)
        await asyncio.sleep(POLL_INTERVAL)


async def _poll_once(push_fn: Callable, cookie_str: str):
    from playwright.async_api import async_playwright

    async with async_playwright() as p:
        browser = await p.chromium.launch(headless=True, args=CHROMIUM_ARGS)
        try:
            context = await browser.new_context(
                viewport={"width": 800, "height": 600},
                user_agent="Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
            )

            # Load user's session cookies (Bitget auth + any existing cf_clearance)
            cookies = _parse_cookie_string(cookie_str)
            if cookies:
                await context.add_cookies(cookies)

            # ── Step 1: brief page visit to pass Cloudflare JS challenge ──
            # This gets a fresh cf_clearance tied to this server's IP.
            # We block heavy resources to minimise memory, but allow scripts
            # so the Cloudflare challenge can run.
            page = await context.new_page()

            async def _block(route):
                if route.request.resource_type in {"document", "script", "xhr", "fetch"}:
                    await route.continue_()
                else:
                    await route.abort()

            await page.route("**/*", _block)
            _status["browser_alive"] = True

            try:
                await page.goto(f"{BITGET_BASE}/about",
                                wait_until="domcontentloaded", timeout=30_000)
                logger.info("Navigation OK — Cloudflare passed")
            except Exception as e:
                logger.info("Navigation ended early (%s) — continuing", e)

            await page.close()  # free renderer memory immediately

            # ── Step 2: use context.request for all API calls ──
            # context.request shares the browser context (including the fresh
            # cf_clearance set in step 1) but doesn't need a renderer process.
            req = context.request
            await _active_poll(req, push_fn)
            await _fetch_balance(req, push_fn)

            logger.info("Poll cycle complete")
        finally:
            await browser.close()


async def _active_poll(req, push_fn: Callable):
    logger.info("Polling APIs...")

    try:
        r = await req.post(
            f"{BITGET_BASE}/v1/trace/mt5/data/tracePosition",
            data=json.dumps({"portfolioId": PORTFOLIO_ID}),
            headers={"Content-Type": "application/json"},
            timeout=20_000,
        )
        data = await _safe_json(r)
        logger.info("Positions: HTTP %s code=%s", r.status, data.get("code") if data else "err")
        if r.ok and data:
            push_fn("positions", data)
    except Exception as e:
        logger.warning("Poll positions error: %s", e)

    try:
        r = await req.post(
            f"{BITGET_BASE}/v1/trace/mt5/trace/positionHistory",
            data=json.dumps({"portfolioId": PORTFOLIO_ID, "pageNo": 1, "pageSize": 50}),
            headers={"Content-Type": "application/json"},
            timeout=20_000,
        )
        data = await _safe_json(r)
        logger.info("History: HTTP %s code=%s", r.status, data.get("code") if data else "err")
        if r.ok and data:
            push_fn("history", data)
    except Exception as e:
        logger.warning("Poll history error: %s", e)

    for ep in [
        "/v1/trace/mt5/trace/balanceHistory",
        "/v1/trace/mt5/data/balanceHistory",
        "/v1/trace/mt5/trace/fundFlow",
    ]:
        try:
            r = await req.post(
                f"{BITGET_BASE}{ep}",
                data=json.dumps({"portfolioId": PORTFOLIO_ID, "pageNo": 1, "pageSize": 100}),
                headers={"Content-Type": "application/json"},
                timeout=20_000,
            )
            if not r.ok:
                continue
            j = await _safe_json(r) or {}
            rows = j.get("data") or []
            if isinstance(rows, dict):
                rows = rows.get("rows") or rows.get("list") or []
            if isinstance(rows, list) and rows:
                logger.info("Polled balance_history from %s", ep)
                push_fn("balance_history", rows)
                break
        except Exception:
            pass

    _status["last_poll"] = datetime.now(BKK).strftime("%Y-%m-%d %H:%M:%S")
    _status["polls"] += 1


async def _fetch_balance(req, push_fn: Callable):
    get_eps = [
        f"/v1/trace/mt5/trace/traceDetail?portfolioId={PORTFOLIO_ID}",
        f"/v1/trace/mt5/data/copyDetail?portfolioId={PORTFOLIO_ID}",
        f"/v1/trace/mt5/data/accountInfo?portfolioId={PORTFOLIO_ID}",
        f"/v1/trace/mt5/account/balance?portfolioId={PORTFOLIO_ID}",
        f"/v1/trace/mt5/data/followerDetail?portfolioId={PORTFOLIO_ID}",
        f"/v1/trace/mt5/trace/followerDetail?portfolioId={PORTFOLIO_ID}",
    ]
    for ep in get_eps:
        try:
            r = await req.get(f"{BITGET_BASE}{ep}", timeout=20_000)
            if not r.ok:
                logger.info("Balance GET %s → %s", ep.split("?")[0].split("/")[-1], r.status)
                continue
            result = await _safe_json(r) or {}
            d = result.get("data", result) if isinstance(result, dict) else result
            if isinstance(d, dict) and any("balance" in k.lower() or "equity" in k.lower() for k in d):
                logger.info("Found balance via GET %s", ep.split("?")[0].split("/")[-1])
                push_fn("copy_details", d)
                _status["last_scrape"] = datetime.now(BKK).strftime("%Y-%m-%d %H:%M:%S")
                _status["scrapes"] += 1
                return
        except Exception as e:
            logger.warning("Balance GET %s: %s", ep.split("?")[0].split("/")[-1], e)

    for ep in [
        "/v1/trace/mt5/trace/traceDetail", "/v1/trace/mt5/data/copyDetail",
        "/v1/trace/mt5/data/followerDetail", "/v1/trace/mt5/data/accountInfo",
        "/v1/trace/mt5/account/balance",
    ]:
        try:
            r = await req.post(
                f"{BITGET_BASE}{ep}",
                data=json.dumps({"portfolioId": PORTFOLIO_ID}),
                headers={"Content-Type": "application/json"},
                timeout=20_000,
            )
            if not r.ok:
                continue
            result = await _safe_json(r) or {}
            d = result.get("data", result) if isinstance(result, dict) else result
            if isinstance(d, dict) and any("balance" in k.lower() or "equity" in k.lower() for k in d):
                logger.info("Found balance via POST %s", ep.split("/")[-1])
                push_fn("copy_details", d)
                _status["last_scrape"] = datetime.now(BKK).strftime("%Y-%m-%d %H:%M:%S")
                _status["scrapes"] += 1
                return
        except Exception as e:
            logger.warning("Balance POST %s: %s", ep.split("/")[-1], e)

    logger.warning("No balance endpoint found")


async def _safe_json(r) -> dict | None:
    try:
        return await r.json()
    except Exception:
        return None
