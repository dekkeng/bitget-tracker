import asyncio
import json
import logging
import os
from datetime import datetime, timezone, timedelta
from pathlib import Path
from typing import Callable

logger = logging.getLogger(__name__)

BKK = timezone(timedelta(hours=7))
BITGET_BASE = "https://www.bitget.com"
POLL_INTERVAL = int(os.environ.get("POLL_INTERVAL_SEC", "120"))
COOKIES_FILE = Path(os.environ.get("COOKIES_PATH", "cookies.json"))

# Parse TRADERS env var: "DKTrading:1443199880395776000,XauKingScalp:1433276980578508800"
_TRADERS_ENV = os.environ.get("TRADERS", "")
if _TRADERS_ENV:
    _TRADERS: dict[str, str] = {}
    for _item in _TRADERS_ENV.split(","):
        _item = _item.strip()
        if ":" in _item:
            _n, _p = _item.split(":", 1)
            _TRADERS[_n.strip()] = _p.strip()
else:
    _TRADERS = {
        os.environ.get("TRADER_NAME", "DKTrading"): os.environ.get("PORTFOLIO_ID", "1443199880395776000")
    }

# Reverse lookup: portfolioId → trader name
_pid_to_name: dict[str, str] = {pid: name for name, pid in _TRADERS.items()}

# Legacy single-ID alias (used in positions probe which is still global)
PORTFOLIO_ID = next(iter(_TRADERS.values()), "")

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
    "pushes": 0,
    "last_pos_response": None,
    "last_hist_response": None,
}


def get_status() -> dict:
    cookie_str = _load_cookie_string()
    return {
        **_status,
        "has_cookie": bool(cookie_str),
        "cookie_preview": (cookie_str[:40] + "...") if len(cookie_str) > 40 else cookie_str,
        "poll_interval_sec": POLL_INTERVAL,
        "traders": list(_TRADERS.keys()),
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

    def _counted_push(kind: str, data, trader: str = None):
        _status["pushes"] += 1
        logger.info("push_fn called: kind=%s trader=%s pushes=%d", kind, trader, _status["pushes"])
        push_fn(kind, data, trader)

    while True:
        cookie_str = _load_cookie_string()
        if not cookie_str:
            _status["last_error"] = "No cookie set"
            _status["browser_alive"] = False
            await asyncio.sleep(10)
            continue

        _status["last_error"] = None
        try:
            await _poll_once(_counted_push, cookie_str)
        except Exception as e:
            logger.error("Poll cycle crashed: %s", e)
            _status["last_error"] = f"Poll error: {e}"

        _status["browser_alive"] = False
        logger.info("Next poll in %ds… (pushes so far: %d)", POLL_INTERVAL, _status["pushes"])
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

            cookies = _parse_cookie_string(cookie_str)
            if cookies:
                await context.add_cookies(cookies)

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
                logger.info("CF challenge passed")
            except Exception as e:
                logger.info("About nav: %s", e)

            await _active_poll(page, push_fn)
            await _fetch_balance(page, push_fn)

            logger.info("Poll cycle complete — closing browser")
        finally:
            await browser.close()


async def _active_poll(page, push_fn: Callable):
    logger.info("Polling APIs via page.evaluate...")

    # ── Positions — global probe (not trader-specific, still 403 in most cases) ──
    pos_probes = []
    for label, body in [
        ("empty",          {}),
        ("portfolioId",    {"portfolioId": PORTFOLIO_ID}),
        ("followId",       {"followPortfolioId": PORTFOLIO_ID}),
        ("userId",         {"userId": PORTFOLIO_ID}),
    ]:
        try:
            result = await page.evaluate("""async ([body]) => {
                try {
                    const r = await fetch('/v1/trace/mt5/trace/getFollowOpenPosition', {
                        method: 'POST', credentials: 'include',
                        headers: {'Content-Type': 'application/json'},
                        body: JSON.stringify(body),
                    });
                    const text = await r.text();
                    if (text.trimStart().startsWith('<')) return {status: r.status, error: 'html_redirect'};
                    const j = JSON.parse(text);
                    return {status: r.status, code: j?.code, msg: j?.msg,
                            data_keys: j?.data != null ? Object.keys(Object(j.data)).slice(0, 8) : null};
                } catch(e) { return {status: 0, error: String(e)}; }
            }""", [body])
            code = result.get("code") if isinstance(result, dict) else None
            entry = {"body": label, "status": result.get("status"), "code": code, "error": result.get("error"), "data_keys": result.get("data_keys")}
            pos_probes.append(entry)
            if isinstance(result, dict) and result.get("status") == 200 and code in ("00000", "200", "0"):
                logger.info("Positions found with body=%s code=%s keys=%s", label, code, result.get("data_keys"))
                push_fn("positions", result.get("data") or {})
                break
        except Exception as ex:
            pos_probes.append({"body": label, "error": str(ex)})
    _status["last_pos_response"] = pos_probes[0] if pos_probes else {}
    _status["last_pos_probes"] = pos_probes

    # ── History + balance history — per trader ───────────────────────────────
    for trader_name, pid in _TRADERS.items():
        logger.info("Polling history for trader=%s pid=%s", trader_name, pid)

        try:
            hist = await page.evaluate("""async (pid) => {
                try {
                    const r = await fetch('/v1/trace/mt5/trace/positionHistory', {
                        method: 'POST', credentials: 'include',
                        headers: {'Content-Type': 'application/json'},
                        body: JSON.stringify({portfolioId: pid, pageNo: 1, pageSize: 50}),
                    });
                    const text = await r.text();
                    if (text.trimStart().startsWith('<')) return {status: r.status, error: 'html_redirect'};
                    const j = JSON.parse(text);
                    return {status: r.status, data: j};
                } catch(e) { return {status: 0, error: String(e)}; }
            }""", pid)
            api_code = (hist.get("data") or {}).get("code")
            api_msg  = (hist.get("data") or {}).get("msg")
            logger.info("History[%s]: HTTP %s api_code=%s err=%s", trader_name, hist.get("status"), api_code, hist.get("error"))
            _status["last_hist_response"] = {
                "trader": trader_name, "http": hist.get("status"),
                "code": api_code, "msg": api_msg, "error": hist.get("error"),
            }
            if hist.get("status") == 200 and api_code in ("00000", "200", "0"):
                push_fn("history", hist["data"], trader_name)
        except Exception as e:
            logger.warning("Poll history error [%s]: %s", trader_name, e)

    _status["last_poll"] = datetime.now(BKK).strftime("%Y-%m-%d %H:%M:%S")
    _status["polls"] += 1


async def _fetch_balance(page, push_fn: Callable):
    # Try a single call without portfolioId — may return all followed portfolios at once.
    # If it returns multiple portfolioDetails, match each to a trader by portfolioId.
    # Fallback: per-trader calls.
    try:
        result = await page.evaluate("""async () => {
            try {
                const r = await fetch('/v1/trace/mt5/trace/getFollowPortfolios', {
                    method: 'POST', credentials: 'include',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({}),
                });
                const j = await r.json();
                return {status: r.status, code: j?.code, data: j?.data};
            } catch(e) { return {status: 0, error: String(e)}; }
        }""")

        code = result.get("code") if isinstance(result, dict) else None
        _status["last_balance_probes"] = {"getFollowPortfolios_all": {
            "http": result.get("status"), "code": code}}

        if result.get("status") == 200 and code in ("00000", "200", "0"):
            data = result.get("data") or {}
            details = data.get("portfolioDetails") or []
            if details and isinstance(details, list):
                matched = 0
                for portfolio in details:
                    if not isinstance(portfolio, dict):
                        continue
                    pid = str(portfolio.get("portfolioId") or portfolio.get("followPortfolioId") or "")
                    trader_name = _pid_to_name.get(pid)
                    if trader_name:
                        logger.info("getFollowPortfolios all: matched trader=%s pid=%s balance=%s",
                                    trader_name, pid, portfolio.get("balance"))
                        push_fn("copy_details", portfolio, trader_name)
                        matched += 1
                    else:
                        logger.info("getFollowPortfolios all: unmatched pid=%s keys=%s",
                                    pid, list(portfolio.keys())[:6])
                if matched > 0:
                    _status["scrapes"] += 1
                    _status["last_scrape"] = datetime.now(BKK).strftime("%Y-%m-%d %H:%M:%S")
                    return
                logger.warning("getFollowPortfolios all: got %d details but none matched known pids %s",
                               len(details), list(_pid_to_name.keys()))
    except Exception as e:
        logger.warning("getFollowPortfolios all error: %s", e)

    # Fallback: per-trader calls
    logger.info("Falling back to per-trader getFollowPortfolios calls")
    for trader_name, pid in _TRADERS.items():
        try:
            result = await page.evaluate("""async (pid) => {
                try {
                    const r = await fetch('/v1/trace/mt5/trace/getFollowPortfolios', {
                        method: 'POST', credentials: 'include',
                        headers: {'Content-Type': 'application/json'},
                        body: JSON.stringify({portfolioId: pid}),
                    });
                    const j = await r.json();
                    return {status: r.status, code: j?.code, data: j?.data};
                } catch(e) { return {status: 0, error: String(e)}; }
            }""", pid)

            code = result.get("code") if isinstance(result, dict) else None
            _status["last_balance_probes"][f"getFollowPortfolios_{trader_name}"] = {
                "http": result.get("status"), "code": code}

            if result.get("status") == 200 and code in ("00000", "200", "0"):
                data = result.get("data") or {}
                details = data.get("portfolioDetails") or []
                if details and isinstance(details[0], dict):
                    portfolio = details[0]
                    logger.info("getFollowPortfolios[%s]: balance=%s investment=%s",
                                trader_name, portfolio.get("balance"), portfolio.get("totalInvestment"))
                    push_fn("copy_details", portfolio, trader_name)
                    _status["last_scrape"] = datetime.now(BKK).strftime("%Y-%m-%d %H:%M:%S")
                    _status["scrapes"] += 1
                else:
                    logger.warning("getFollowPortfolios[%s]: no portfolioDetails", trader_name)
            else:
                logger.warning("getFollowPortfolios[%s] failed: http=%s code=%s",
                               trader_name, result.get("status"), code)
        except Exception as e:
            logger.warning("getFollowPortfolios[%s] error: %s", trader_name, e)
