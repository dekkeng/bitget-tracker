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
    """
    Fetch all pages from a Bitget list endpoint.
    Returns (rows, meta) where meta carries diagnostic info.
    """
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


async def _fetch_bills_deposits(client: httpx.AsyncClient, coin: str,
                                 api_key: str, secret: str, passphrase: str,
                                 start_ms: int, now_ms: int) -> tuple[float, dict]:
    """
    Fallback for deposit-records: sum deposit entries from spot account bills ledger.
    Bills ledger entries are always final (no status check needed).
    Returns (total_deposited, meta).
    """
    rows, meta = await _get_all_pages(
        client, "/api/v2/spot/account/bills",
        {"coin": coin, "bizType": "deposit",
         "startTime": str(start_ms), "endTime": str(now_ms)},
        api_key, secret, passphrase,
    )
    # Bills use 'size' (always positive for deposits) or 'amount'
    total = sum(abs(_row_amount(r)) for r in rows)
    logger.info("Bills fallback: %d deposit records, total=%.2f (code=%s)",
                len(rows), total, meta.get("last_code"))
    return total, meta, rows


async def fetch_net_investment(api_key: str, secret: str, passphrase: str,
                                coin: str = "USDT") -> dict:
    """
    Return net investment = total deposits - total withdrawals for coin.

    Primary path: deposit-records endpoint.
    Fallback: spot account bills (if deposit-records returns 400172).
    """
    now_ms = int(time.time() * 1000)
    start_ms = now_ms - 5 * 365 * 24 * 3600 * 1000

    async with httpx.AsyncClient(timeout=30) as client:
        # Run deposit-records and withdrawal-records in parallel
        (deposits_all, dep_meta), (withdrawals, wdw_meta) = await asyncio.gather(
            _get_all_pages(client, "/api/v2/spot/wallet/deposit-records",
                           {"startTime": str(start_ms), "endTime": str(now_ms)},
                           api_key, secret, passphrase),
            _get_all_pages(client, "/api/v2/spot/wallet/withdrawal-records",
                           {"coin": coin}, api_key, secret, passphrase),
        )

        bills_meta = None
        bills_rows: list[dict] = []

        # If deposit-records fails (common 400172), fall back to spot account bills
        if dep_meta.get("last_code") == "400172" or dep_meta.get("error"):
            logger.info("deposit-records failed (%s), trying spot account bills",
                        dep_meta.get("last_code"))
            dep_total, bills_meta, bills_rows = await _fetch_bills_deposits(
                client, coin, api_key, secret, passphrase, start_ms, now_ms
            )
            dep_source = "bills"
            dep_all_coins = list({str(r.get("coin", "")) for r in bills_rows})
            deposits_for_count = bills_rows
            dep_sample = bills_rows[:3]
            dep_success_count = len(bills_rows)  # bills entries are always final
        else:
            dep_source = "deposit-records"
            dep_all_coins = list({str(r.get("coin", "")) for r in deposits_all})
            # Prefix match: catches USDT-TRC20, USDT-ERC20 etc.
            deposits_filtered = [r for r in deposits_all
                                  if str(r.get("coin", "")).upper().startswith(coin.upper())]
            dep_success = [r for r in deposits_filtered if _is_success(r)]
            dep_total = sum(_row_amount(r) for r in dep_success)
            deposits_for_count = deposits_filtered
            dep_sample = deposits_all[:3]
            dep_success_count = len(dep_success)

    wdw_success = [r for r in withdrawals if _is_success(r)]
    wdw_total = sum(_row_amount(r) for r in wdw_success)

    dep_statuses = list({str(r.get("status", "")) for r in deposits_for_count})
    wdw_statuses = list({str(r.get("status", "")) for r in withdrawals})

    logger.info(
        "Investment fetch [%s]: dep_total=%.2f (%d records) wdw_total=%.2f (%d/%d success)",
        dep_source, dep_total, len(deposits_for_count),
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
        "_dep_source": dep_source,                  # which endpoint gave us deposits
        "_dep_statuses": dep_statuses,
        "_wdw_statuses": wdw_statuses,
        "_dep_meta": dep_meta,
        "_wdw_meta": wdw_meta,
        "_bills_meta": bills_meta,                  # set if bills fallback was used
        "_dep_all_coins": dep_all_coins,
        "_dep_raw_count": len(deposits_all),
        "_dep_sample": dep_sample,
        "_wdw_sample": withdrawals[:3],
    }
