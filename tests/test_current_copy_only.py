import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


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


if __name__ == "__main__":
    unittest.main()
