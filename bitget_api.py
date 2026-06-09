import asyncio
import base64
import hashlib
import hmac
import logging
import time

import httpx

logger = logging.getLogger(__name__)
BITGET_API_BASE = "https://api.bitget.com"


def _sign(ts: str, method: str, path_and_query: str, body: str, secret: str) -> str:
    prehash = ts + method.upper() + path_and_query + (body or "")
    return base64.b64encode(
        hmac.new(secret.encode(), prehash.encode(), hashlib.sha256).digest()
    ).decode()


def _auth_headers(method: str, path_and_query: str, body: str,
                  api_key: str, secret: str, passphrase: str) -> dict:
    ts = str(int(time.time() * 1000))
    return {
        "ACCESS-KEY": api_key,
        "ACCESS-SIGN": _sign(ts, method, path_and_query, body, secret),
        "ACCESS-TIMESTAMP": ts,
        "ACCESS-PASSPHRASE": passphrase,
        "Content-Type": "application/json",
        "locale": "en-US",
    }


def _row_amount(row: dict) -> float:
    """Extract the transaction amount — Bitget uses 'size' on v2, 'amount' on older paths."""
    for field in ("size", "amount", "qty", "quantity"):
        v = row.get(field)
        if v not in (None, "", "0", 0):
            try:
                return float(v)
            except (TypeError, ValueError):
                pass
    return 0.0


def _is_success(row: dict) -> bool:
    """Return True for completed/confirmed records regardless of case."""
    status = str(row.get("status", "")).lower().strip()
    return status in ("success", "successful", "complete", "completed", "4")


async def _get_all_pages(client: httpx.AsyncClient, path: str, base_params: dict,
                         api_key: str, secret: str, passphrase: str,
                         max_pages: int = 20) -> tuple[list[dict], dict]:
    """Fetch all pages for a single time window."""
    rows: list[dict] = []
    cursor: str | None = None
    meta: dict = {"pages_fetched": 0, "last_code": None, "last_msg": None, "error": None,
                  "first_response": None}

    for _ in range(max_pages):
        params = {**base_params, "limit": "100"}
        if cursor:
            params["idLessThan"] = cursor
        qs = "?" + "&".join(f"{k}={v}" for k, v in sorted(params.items()))
        hdrs = _auth_headers("GET", path + qs, "", api_key, secret, passphrase)
        try:
            r = await client.get(BITGET_API_BASE + path + qs, headers=hdrs)
            resp_body = r.json()
        except Exception as exc:
            meta["error"] = str(exc)
            logger.warning("Bitget API GET %s: %s", path, exc)
            break

        code = str(resp_body.get("code", ""))
        meta["last_code"] = code
        meta["last_msg"] = resp_body.get("msg")
        if meta["first_response"] is None:
            meta["first_response"] = {k: v for k, v in resp_body.items() if k != "data"}

        if code not in ("00000", "0", "200"):
            meta["error"] = f"code={code} msg={resp_body.get('msg')}"
            logger.warning("Bitget API %s code=%s msg=%s", path, code, resp_body.get("msg"))
            break

        meta["pages_fetched"] += 1
        page: list = []
        d = resp_body.get("data") or {}
        if isinstance(d, dict):
            page = d.get("rows") or d.get("list") or []
        elif isinstance(d, list):
            page = d

        if not page:
            break
        rows.extend(page)

        if len(page) < 100:
            break

        last = page[-1]
        cursor = str(last.get("orderId") or last.get("id") or last.get("billId") or "")
        if not cursor:
            break

    return rows, meta


async def _get_windowed(client: httpx.AsyncClient, path: str, base_params: dict,
                         api_key: str, secret: str, passphrase: str,
                         start_ms: int, now_ms: int,
                         window_days: int = 85) -> tuple[list[dict], dict]:
    """
    Fetch all records across multiple 85-day windows to work around Bitget's 90-day limit.
    Walks backwards from now_ms to start_ms.
    """
    all_rows: list[dict] = []
    combined: dict = {"pages_fetched": 0, "windows_fetched": 0,
                      "last_code": None, "last_msg": None, "error": None,
                      "first_response": None}

    window_ms = window_days * 24 * 3600 * 1000
    end = now_ms

    while end > start_ms:
        win_start = max(end - window_ms, start_ms)
        params = {**base_params, "startTime": str(win_start), "endTime": str(end)}
        rows, meta = await _get_all_pages(client, path, params, api_key, secret, passphrase)

        combined["pages_fetched"] += meta["pages_fetched"]
        combined["last_code"] = meta["last_code"]
        combined["last_msg"] = meta["last_msg"]
        if combined["first_response"] is None:
            combined["first_response"] = meta.get("first_response")

        if meta.get("error"):
            combined["error"] = meta["error"]
            break

        combined["windows_fetched"] += 1
        all_rows.extend(rows)
        end = win_start - 1  # next window ends just before this one started

    return all_rows, combined


async def fetch_net_investment(api_key: str, secret: str, passphrase: str,
                                coin: str = "USDT") -> dict:
    """
    Return net investment = total deposits - total withdrawals for coin.

    Bitget enforces a 90-day max window per request, so we walk backwards
    in 85-day chunks covering 2 years.

    Primary deposit source: deposit-records (no coin filter — 400172 if coin= passed).
    Fallback: spot account bills (if deposit-records returns a non-range error).
    """
    now_ms = int(time.time() * 1000)
    # 2 years back covers all realistic deposit history
    start_ms = now_ms - 2 * 365 * 24 * 3600 * 1000

    async with httpx.AsyncClient(timeout=60) as client:
        # Run deposit-records and withdrawal-records concurrently
        (deposits_all, dep_meta), (withdrawals, wdw_meta) = await asyncio.gather(
            _get_windowed(client, "/api/v2/spot/wallet/deposit-records",
                          {},  # no coin filter — 400172 if coin= passed
                          api_key, secret, passphrase, start_ms, now_ms),
            _get_windowed(client, "/api/v2/spot/wallet/withdrawal-records",
                          {"coin": coin},
                          api_key, secret, passphrase, start_ms, now_ms),
        )

        bills_meta = None
        bills_rows: list[dict] = []

        # Fall back to spot account bills if deposit-records has a non-range error
        dep_error_code = dep_meta.get("last_code") or ""
        if dep_meta.get("error") and dep_error_code not in ("00000", "0", "200", "00001"):
            logger.info("deposit-records failed (%s), trying spot account bills", dep_error_code)
            bills_rows, bills_meta = await _get_windowed(
                client, "/api/v2/spot/account/bills",
                {"coin": coin, "bizType": "deposit"},
                api_key, secret, passphrase, start_ms, now_ms,
            )

    dep_source = "deposit-records"
    dep_all_coins = list({str(r.get("coin", "")) for r in deposits_all})

    if bills_rows:
        # Bills fallback: all ledger entries are final, no status check needed
        dep_source = "bills"
        dep_total = sum(abs(_row_amount(r)) for r in bills_rows)
        deposits_for_count = bills_rows
        dep_success_count = len(bills_rows)
        dep_sample = bills_rows[:3]
        dep_statuses: list = []
    else:
        # Prefix match: catches USDT-TRC20, USDT-ERC20 etc.
        deposits_filtered = [r for r in deposits_all
                              if str(r.get("coin", "")).upper().startswith(coin.upper())]
        dep_success = [r for r in deposits_filtered if _is_success(r)]
        dep_total = sum(_row_amount(r) for r in dep_success)
        deposits_for_count = deposits_filtered
        dep_success_count = len(dep_success)
        dep_sample = deposits_all[:3]
        dep_statuses = list({str(r.get("status", "")) for r in deposits_filtered})

    wdw_success = [r for r in withdrawals if _is_success(r)]
    wdw_total = sum(_row_amount(r) for r in wdw_success)
    wdw_statuses = list({str(r.get("status", "")) for r in withdrawals})

    logger.info(
        "Investment [%s]: dep=%.2f (%d records, %d windows) wdw=%.2f (%d/%d success)",
        dep_source, dep_total, len(deposits_for_count),
        dep_meta.get("windows_fetched", 0),
        wdw_total, len(wdw_success), len(withdrawals),
    )

    return {
        "deposits": round(dep_total, 2),
        "withdrawals": round(wdw_total, 2),
        "net": round(dep_total - wdw_total, 2),
        "deposit_count": len(deposits_for_count),
        "withdrawal_count": len(withdrawals),
        "deposit_success_count": dep_success_count,
        "withdrawal_success_count": len(wdw_success),
        "coin": coin,
        "_dep_source": dep_source,
        "_dep_statuses": dep_statuses,
        "_wdw_statuses": wdw_statuses,
        "_dep_meta": dep_meta,
        "_wdw_meta": wdw_meta,
        "_bills_meta": bills_meta,
        "_dep_all_coins": dep_all_coins,
        "_dep_raw_count": len(deposits_all),
        "_dep_sample": dep_sample,
        "_wdw_sample": withdrawals[:3],
    }


async def fetch_earn_balance(api_key: str, secret: str, passphrase: str) -> dict:
    """
    Return earn (savings/flexible) balance and weighted APR across all earn positions.

    Tries Bitget v2 earn endpoints in order:
      1. GET /api/v2/earn/savings/assets  — flexible savings positions
      2. GET /api/v2/earn/account         — consolidated earn account summary
    """
    async with httpx.AsyncClient(timeout=30) as client:
        candidates = [
            "/api/v2/earn/savings/assets",
            "/api/v2/earn/account",
        ]
        for path in candidates:
            qs = ""
            hdrs = _auth_headers("GET", path, "", api_key, secret, passphrase)
            try:
                r = await client.get(BITGET_API_BASE + path, headers=hdrs)
                body = r.json()
            except Exception as exc:
                logger.warning("Earn API %s: %s", path, exc)
                continue

            code = str(body.get("code", ""))
            if code not in ("00000", "0", "200"):
                logger.info("Earn API %s: code=%s msg=%s", path, code, body.get("msg"))
                continue

            data = body.get("data") or {}
            items: list[dict] = []
            if isinstance(data, list):
                items = data
            elif isinstance(data, dict):
                items = (data.get("list") or data.get("assets") or
                         data.get("rows") or data.get("data") or [])
                # Flat summary: {totalAmount, ...}
                if not items and data.get("totalAmount") is not None:
                    total = 0.0
                    try:
                        total = float(data["totalAmount"])
                    except (TypeError, ValueError):
                        pass
                    return {"total": round(total, 2), "items": [], "apr": None,
                            "source": path, "error": None}

            if not items:
                continue

            parsed = []
            for it in items:
                if not isinstance(it, dict):
                    continue
                coin = str(it.get("coinName") or it.get("coin") or it.get("currency") or "")
                # Amount / principal
                amt = 0.0
                for f in ("holdAmount", "amount", "totalAmount", "principal",
                          "holdingAmount", "currentAmount"):
                    v = it.get(f)
                    if v not in (None, "", "0", 0):
                        try:
                            amt = float(v)
                            break
                        except (TypeError, ValueError):
                            pass
                if amt <= 0:
                    continue
                # APR — stored as decimal (0.05) or percent (5.0) depending on endpoint
                apr = None
                for f in ("annualInterestRate", "interestRate", "apr", "apy",
                          "annualRate", "yield", "currentRate"):
                    v = it.get(f)
                    if v not in (None, ""):
                        try:
                            fv = float(v)
                            # Normalize: if > 1 assume already in % form; else convert
                            apr = fv if fv > 1 else fv * 100
                            break
                        except (TypeError, ValueError):
                            pass
                parsed.append({"coin": coin, "amount": round(amt, 4), "apr": apr})

            if not parsed:
                continue

            total = sum(p["amount"] for p in parsed)
            # Weighted average APR across items that have APR data
            apr_items = [p for p in parsed if p["apr"] is not None]
            weighted_apr = None
            if apr_items:
                apr_total = sum(p["amount"] for p in apr_items)
                if apr_total > 0:
                    weighted_apr = round(
                        sum(p["amount"] * p["apr"] for p in apr_items) / apr_total, 2
                    )

            logger.info("Earn balance: total=%.2f items=%d weighted_apr=%s source=%s",
                        total, len(parsed), weighted_apr, path)
            return {
                "total": round(total, 2),
                "items": parsed,
                "apr": weighted_apr,
                "source": path,
                "error": None,
            }

    return {"total": 0.0, "items": [], "apr": None, "source": None,
            "error": "no earn endpoint returned data"}
