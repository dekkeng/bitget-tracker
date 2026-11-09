import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from fastapi.testclient import TestClient


class CurrentCopyOnlyTests(unittest.TestCase):
    def load_app(self, current_only):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        settings = root / "settings.json"
        settings.write_text(json.dumps({
            "traders": {}, "cancelled_copy_pnl": -1121.18,
            "cancelled_copy_count": 10,
            "cancelled_trader_names": ["ManuGoldPrime"],
        }))
        env = {
            "COPY_CURRENT_ONLY": "true" if current_only else "false",
            "TRADERS": "ManuGoldPrime:1423679390962597888:cfd",
            "SETTINGS_PATH": str(settings),
            "TRADERS_PATH": str(root / "traders.json"),
            "HISTORY_PATH": str(root / "history.json"),
        }
        with patch.dict(os.environ, env):
            spec = importlib.util.spec_from_file_location(
                "tracker_test_app", Path(__file__).parents[1] / "main.py")
            app = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(app)
        return app, settings

    def test_removed_trader_and_cancelled_replay_cannot_restore_old_pnl(self):
        app, settings = self.load_app(True)
        app._push_data("positions", [{"symbol": "XAUUSD", "profit": -263.2}], "HaruJung")
        app._push_data("trader_config", {"count_in_total": True}, "HaruJung")
        app._push_data("cancelled_copies", [{"name": "ManuGoldPrime", "netProfit": -1121.18}])
        app._push_data("copy_details", {
            "balance": 246.94, "realizedPnl": 137.86, "sharedProfit": 26.62,
        }, "ManuGoldPrime")
        self.assertEqual(app._trader_names(), ["ManuGoldPrime"])
        self.assertNotIn("HaruJung", app._traders_cache)
        self.assertEqual(app._mt5["summary"]["cancelled_copy_pnl"], 0)
        self.assertEqual(app._mt5["summary"]["cancelled_copy_count"], 0)
        self.assertEqual(app._mt5["summary"]["active_trader_pnl"], 111.24)
        self.assertEqual(json.loads(settings.read_text())["cancelled_trader_names"], [])

    def test_registered_current_trader_still_receives_live_positions(self):
        app, _ = self.load_app(True)
        app._push_data("positions", [{"symbol": "XAUUSD", "profit": -4.52}], "ManuGoldPrime")
        self.assertEqual(app._mt5["summary"]["open_positions"], 1)
        self.assertEqual(app._mt5["summary"]["open_positions_pnl"], -4.52)

    def test_default_mode_preserves_auto_registration_and_cancelled_pnl(self):
        app, _ = self.load_app(False)
        app._push_data("positions", [], "HaruJung")
        app._push_data("cancelled_copies", [{"name": "Stopped", "netProfit": -262}])
        self.assertIn("HaruJung", app._trader_names())
        self.assertEqual(app._mt5["summary"]["cancelled_copy_pnl"], -262)

    def test_bitget_snapshot_replaces_the_roster_and_preserves_other_accounts_and_archive(self):
        app, settings = self.load_app(True)
        client = TestClient(app.app)
        old, new, pid = "ManuGoldPrime", "New Bitget trader", "1423679390962597999"
        app._save_history(new, [{"orderId": "old-subscription", "totalProfit": 99}])
        archive = app.HISTORY_FILE.read_text()
        app._push_data("copy_details", {"balance": 197}, old)
        app._push_data("earn", {"total": 288.28})
        app._elite["data"] = {"balance": 250}

        def push(kind, data, trader=None, portfolio_id=None):
            return client.post("/api/push/mt5", json={
                "kind": kind, "data": data, "trader": trader, "portfolioId": portfolio_id,
            })

        snapshot = {"portfolios": [{
            "name": new, "id": pid, "details": {"portfolioId": pid, "balance": 275, "realizedPnl": 0},
        }]}
        bad = {"portfolios": [*snapshot["portfolios"], {"name": "incomplete"}]}
        self.assertEqual(push("active_copy_portfolios", bad).status_code, 409)
        self.assertEqual(app._trader_names(), [old])
        self.assertTrue(push("active_copy_portfolios", snapshot).json()["activeCopyRoster"])
        self.assertEqual(client.get("/api/traders").headers["X-Trader-Roster"], "bitget")
        self.assertEqual(app._load_traders_list()[1], True)
        self.assertEqual(app._trader_names(), [new])
        self.assertEqual(app._mt5["summary"]["total_balance"], 275)
        self.assertEqual(app._mt5["summary"]["active_trader_pnl"], 0)
        self.assertNotIn(old, json.loads(settings.read_text())["traders"])
        self.assertEqual(app._elite["data"]["balance"], 250)
        self.assertEqual(app._earn["data"]["total"], 288.28)
        self.assertEqual(app.HISTORY_FILE.read_text(), archive)
        self.assertFalse(client.delete(f"/api/traders/{new}").json()["ok"])

        push("trader_config", {"count_in_total": False}, new)
        push("copy_details", {"balance": 5}, new)
        self.assertEqual(app._mt5["summary"]["total_balance"], 275)
        self.assertEqual(push("positions", [], old, "1423679390962597888").status_code, 409)
        self.assertEqual(push("positions", [], new, "123").status_code, 409)
        self.assertEqual(push("positions", [], new, pid).status_code, 200)
        self.assertTrue(push("active_copy_portfolios", {"portfolios": []}).json()["activeCopyRoster"])
        self.assertEqual(app._trader_names(), [])
        self.assertEqual(app._mt5["summary"]["total_balance"], 0)
        self.assertEqual(app.HISTORY_FILE.read_text(), archive)


if __name__ == "__main__":
    unittest.main()
