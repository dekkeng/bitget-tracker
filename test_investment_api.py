"""
Quick test for Bitget API investment tracking.
Run: python test_investment_api.py
Set credentials via env vars or edit the constants below.
"""
import asyncio
import json
import os
import sys

# ── Credentials ───────────────────────────────────────────────────────────────
API_KEY    = os.environ.get("BITGET_API_KEY", "")
API_SECRET = os.environ.get("BITGET_API_SECRET", "")
PASSPHRASE = os.environ.get("BITGET_PASSPHRASE", "")

if not all([API_KEY, API_SECRET, PASSPHRASE]):
    print("ERROR: set BITGET_API_KEY, BITGET_API_SECRET, BITGET_PASSPHRASE env vars")
    sys.exit(1)


async def main():
    from bitget_api import (
        BITGET_API_BASE, _auth_headers, _get_all_pages, fetch_net_investment
    )
    import httpx

    print(f"API key: {API_KEY[:6]}...{API_KEY[-4:]}")
    print(f"Base URL: {BITGET_API_BASE}\n")

    # ── Step 1: Test auth with a lightweight endpoint ─────────────────────────
    print("── Step 1: Auth check via /api/v2/user/basicInfo ──")
    async with httpx.AsyncClient(timeout=10) as client:
        path = "/api/v2/user/basicInfo"
        hdrs = _auth_headers("GET", path, "", API_KEY, API_SECRET, PASSPHRASE)
        r = await client.get(BITGET_API_BASE + path, headers=hdrs)
    body = r.json()
    code = body.get("code")
    if code == "00000":
        uid = (body.get("data") or {}).get("userId", "unknown")
        print(f"  OK — userId={uid}")
    else:
        print(f"  FAIL — code={code} msg={body.get('msg')}")
        print("  Check your API key, secret and passphrase.")
        sys.exit(1)

    # ── Step 2: Fetch first page of deposits ──────────────────────────────────
    print("\n── Step 2: First page of USDT deposit records ──")
    async with httpx.AsyncClient(timeout=15) as client:
        dep_rows, dep_meta = await _get_all_pages(
            client, "/api/v2/spot/wallet/deposit-records",
            {"coin": "USDT"}, API_KEY, API_SECRET, PASSPHRASE,
            max_pages=1,
        )
    if dep_meta.get("error"):
        print(f"  FAIL — {dep_meta['error']}")
    else:
        print(f"  OK — {len(dep_rows)} deposit records on first page")
        if dep_rows:
            sample = dep_rows[0]
            print(f"  Sample record keys: {list(sample.keys())}")
            print(f"  Sample status: {sample.get('status')}  size: {sample.get('size')}  coin: {sample.get('coin')}")
            all_statuses = {r.get("status") for r in dep_rows}
            print(f"  Status values seen: {all_statuses}")

    # ── Step 3: Fetch first page of withdrawals ───────────────────────────────
    print("\n── Step 3: First page of USDT withdrawal records ──")
    async with httpx.AsyncClient(timeout=15) as client:
        wdw_rows, wdw_meta = await _get_all_pages(
            client, "/api/v2/spot/wallet/withdrawal-records",
            {"coin": "USDT"}, API_KEY, API_SECRET, PASSPHRASE,
            max_pages=1,
        )
    if wdw_meta.get("error"):
        print(f"  FAIL — {wdw_meta['error']}")
    else:
        print(f"  OK — {len(wdw_rows)} withdrawal records on first page")
        if wdw_rows:
            sample = wdw_rows[0]
            print(f"  Sample record keys: {list(sample.keys())}")
            print(f"  Sample status: {sample.get('status')}  size: {sample.get('size')}  coin: {sample.get('coin')}")
            all_statuses = {r.get("status") for r in wdw_rows}
            print(f"  Status values seen: {all_statuses}")

    # ── Step 4: Full net investment calculation ───────────────────────────────
    print("\n── Step 4: Full net investment (all pages) ──")
    result = await fetch_net_investment(API_KEY, API_SECRET, PASSPHRASE)
    print(f"  Deposits total:       ${result['deposits']:,.2f}  ({result['deposit_count']} records, {result['deposit_success_count']} success)")
    print(f"  Withdrawals total:    ${result['withdrawals']:,.2f}  ({result['withdrawal_count']} records, {result['withdrawal_success_count']} success)")
    print(f"  Net invested:         ${result['net']:,.2f}")
    print(f"  Deposit statuses:     {result['_dep_statuses']}")
    print(f"  Withdrawal statuses:  {result['_wdw_statuses']}")

    if result['deposit_count'] > 0 and result['deposit_success_count'] == 0:
        print("\n  WARNING: deposits found but none matched as 'success'.")
        print(f"  Actual statuses: {result['_dep_statuses']}")
        print("  Update _is_success() in bitget_api.py to match these values.")

    print("\nDone.")


asyncio.run(main())
