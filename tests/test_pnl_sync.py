import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from datetime import datetime
from unittest.mock import patch


class PnlSyncTests(unittest.TestCase):
    def load_app(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        env = {
            "COPY_CURRENT_ONLY": "false",
            "TRADERS": "ManuGoldPrime:1423679390962597888:cfd",
            "SETTINGS_PATH": str(root / "settings.json"),
            "TRADERS_PATH": str(root / "traders.json"),
            "HISTORY_PATH": str(root / "history.json"),
        }
        with patch.dict(os.environ, env):
            spec = importlib.util.spec_from_file_location(
                "tracker_pnl_test_app", Path(__file__).parents[1] / "main.py")
            app = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(app)
        return app, root / "settings.json"

    def test_mt5_float_survives_rebuild_and_resets_on_explicit_zero(self):
        app, _ = self.load_app()
        app._push_data("elite_account", {"balance": 100, "floatProfit": 8, "dayProfit": 2})
        app._push_data("earn", {"interest_24h": 0.1})
        self.assertEqual(app._mt5["summary"]["open_positions_pnl"], 8)
        app._push_data("elite_account", {"balance": 100, "floatProfit": 0})
        self.assertEqual(app._mt5["summary"]["open_positions_pnl"], 0)

    def test_latest_positions_or_account_owns_float(self):
        app, _ = self.load_app()
        app._push_data("elite_account", {"floatProfit": 8})
        app._push_data("elite_positions", {"rows": [{"symbol": "XAUUSD", "profit": 9}]})
        self.assertEqual(app._mt5["summary"]["open_positions_pnl"], 9)
        app._push_data("elite_account", {"floatProfit": 11})
        app._push_data("elite_trader", {"profit": 12, "unrealizedProfit": 200})
        self.assertEqual(app._mt5["summary"]["open_positions_pnl"], 11)
        app._push_data("elite_positions", {"rows": []})
        self.assertEqual(app._mt5["summary"]["open_positions_pnl"], 0)

    def test_zero_profit_keeps_open_position_count_and_zero_balance_updates(self):
        app, _ = self.load_app()
        app._push_data("elite_positions", {"rows": [{"symbol": "XAUUSD", "profit": 0}]})
        app._push_data("elite_account", {"balance": 100, "floatProfit": 0})
        self.assertEqual(app._mt5["summary"]["open_positions"], 1)
        app._push_data("elite_account", {"balance": 0, "equity": 0, "floatProfit": 0})
        self.assertEqual(app._elite["data"]["balance"], 0)
        self.assertEqual(app._elite["data"]["equity"], 0)
        app._push_data("copy_details", {"balance": 100}, "ManuGoldPrime")
        app._push_data("copy_details", {"balance": 0}, "ManuGoldPrime")
        self.assertEqual(app._mt5["summary"]["total_balance"], 0)

    def test_copy_float_latest_source_and_explicit_flat(self):
        app, _ = self.load_app()
        name = "ManuGoldPrime"
        app._push_data("copy_details", {"balance": 100, "equity": 107}, name)
        self.assertEqual(app._mt5["summary"]["open_positions_pnl"], 7)
        app._push_data("positions", [], name)
        self.assertEqual(app._mt5["summary"]["open_positions_pnl"], 0)

        app._push_data("copy_details", {"balance": 100, "equity": 109}, name)
        self.assertEqual(app._mt5["summary"]["open_positions_pnl"], 9)
        app._push_data("copy_details", {"balance": 100, "equity": 100}, name)
        self.assertEqual(app._mt5["summary"]["open_positions_pnl"], 0)

    def test_cash_balance_does_not_depend_on_json_key_order(self):
        app, _ = self.load_app()
        app._push_data("copy_details", {"equity": 109, "balance": 100}, "ManuGoldPrime")
        self.assertEqual(app._mt5["summary"]["total_balance"], 100)
        self.assertEqual(app._mt5["summary"]["open_positions_pnl"], 9)

    def test_daily_sums_counted_copy_and_mt5_and_ignores_empty_history_refresh(self):
        app, _ = self.load_app()
        start, end = app._bkk_today_range_ms()
        app._push_data("history", {"data": {"rows": [
            {"orderId": "today", "closeTime": start, "totalProfit": 7.5},
            {"orderId": "yesterday", "closeTime": start - 1, "totalProfit": 900},
            {"orderId": "tomorrow", "closeTime": end, "totalProfit": 800},
        ]}}, "ManuGoldPrime")
        app._push_data("elite_account", {"dayProfit": 2.5, "dayCount": 1})
        self.assertEqual(app._mt5["summary"]["daily_pnl"], 10)
        app._push_data("history", {"data": {"rows": []}}, "ManuGoldPrime")
        self.assertEqual(app._mt5["summary"]["daily_pnl"], 10)
        app._push_data("trader_config", {"count_in_total": False}, "ManuGoldPrime")
        self.assertEqual(app._mt5["summary"]["daily_pnl"], 2.5)
        app._push_data("trader_config", {"count_in_total": True}, "ManuGoldPrime")
        self.assertEqual(app._mt5["summary"]["daily_pnl"], 10)

    def test_same_symbol_and_second_keeps_distinct_deals_and_dedups_replays(self):
        app, _ = self.load_app()
        now = int(datetime.now(app.BKK).timestamp())
        a = {"ticket": 1, "symbol": "XAUUSD", "closeTime": now, "totalProfit": 3}
        b = {"ticket": 2, "symbol": "XAUUSD", "closeTime": now, "totalProfit": 4}
        rows = app._merge_history_rows([a], [b, b])
        self.assertEqual(len(rows), 2)
        self.assertEqual(sum(r["totalProfit"] for r in rows), 7)
        legacy = {k: v for k, v in a.items() if k != "ticket"}
        rows = app._merge_history_rows([legacy], [a, b])
        self.assertEqual(len(rows), 2)
