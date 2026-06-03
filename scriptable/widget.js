// SETUP: In Scriptable, run this script once manually first.
// It will prompt you to enter your server URL (e.g. https://bitget-tracker.onrender.com)
// and save it to Keychain automatically.
// After setup, add a Medium-sized Scriptable widget to your home screen.

const KEYCHAIN_KEY = "bitget_tracker_url";
const GREEN  = new Color("#00c47a");
const RED    = new Color("#ff4d4d");
const AMBER  = new Color("#f59e0b");
const WHITE  = new Color("#ffffff");
const MUTED  = new Color("#888888");
const BG     = new Color("#111111");
const SEP_C  = new Color("#333333");

// ── First-run setup (only when running inside the app, not as a widget) ──
if (!config.runsInWidget) {
  const stored = Keychain.contains(KEYCHAIN_KEY) ? Keychain.get(KEYCHAIN_KEY) : null;
  const prompt = new Alert();
  prompt.title = "Bitget Tracker Setup";
  prompt.message = "Enter your server URL (e.g. https://bitget-tracker.onrender.com)";
  prompt.addTextField("Server URL", stored || "https://bitget-tracker.onrender.com");
  prompt.addAction("Save");
  prompt.addCancelAction("Cancel");
  const idx = await prompt.presentAlert();
  if (idx === 0) {
    const url = prompt.textFieldValue(0).replace(/\/$/, "");
    Keychain.set(KEYCHAIN_KEY, url);
    const done = new Alert();
    done.title = "Saved!";
    done.message = `URL saved: ${url}\n\nNow add a Medium Scriptable widget to your home screen.`;
    done.addAction("OK");
    await done.presentAlert();
    Script.complete();
    return;
  } else {
    Script.complete();
    return;
  }
}

// ── Widget mode ──
const widget = new ListWidget();
widget.backgroundColor = BG;
widget.setPadding(8, 10, 8, 10);
widget.url = Keychain.contains(KEYCHAIN_KEY) ? Keychain.get(KEYCHAIN_KEY) : "";

async function fetchData(baseUrl) {
  const req = new Request(`${baseUrl}/api/widget`);
  req.timeoutInterval = 8;
  try {
    const data = await req.loadJSON();
    return { data, stale: data.stale || false };
  } catch (e) {
    const cached = Keychain.contains("bitget_widget_cache")
      ? JSON.parse(Keychain.get("bitget_widget_cache"))
      : null;
    return { data: cached, stale: true };
  }
}

function txt(stack, content, size, color, bold = false) {
  const t = stack.addText(content);
  t.font = bold ? Font.boldSystemFont(size) : Font.systemFont(size);
  t.textColor = color;
  t.lineLimit = 1;
  return t;
}

function fmtUSD(n) {
  if (n == null) return "$0.00";
  return "$" + Math.abs(n).toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 });
}

function fmtPnL(n) {
  return (n >= 0 ? "+" : "-") + fmtUSD(n);
}

// ── Build widget ──
if (!Keychain.contains(KEYCHAIN_KEY)) {
  const row = widget.addStack();
  const t = row.addText("⚙ Open Scriptable & run\nthis script to set up.");
  t.font = Font.systemFont(10);
  t.textColor = MUTED;
  t.centerAlignText();
  Script.setWidget(widget);
  Script.complete();
  return;
}

const baseUrl = Keychain.get(KEYCHAIN_KEY);
const { data, stale } = await fetchData(baseUrl);

if (!data) {
  const row = widget.addStack();
  const t = row.addText("⚠ No data\nCheck server");
  t.font = Font.boldSystemFont(11);
  t.textColor = AMBER;
  t.centerAlignText();
  Script.setWidget(widget);
  Script.complete();
  return;
}

Keychain.set("bitget_widget_cache", JSON.stringify(data));

const pnl    = data.daily_pnl ?? 0;
const bal    = data.total_balance ?? 0;
const inv    = data.total_investment ?? 0;
const nPos   = data.open_positions ?? 0;
const oPnl   = data.open_positions_pnl ?? 0;
const allPnl = data.all_time_pnl ?? 0;
const updAt  = data.updated_at ?? "--:--";

const pnlColor = pnl >= 0 ? GREEN : RED;

// ── Row 1: Header ──
const row1 = widget.addStack();
row1.layoutHorizontally();
row1.centerAlignContent();
txt(row1, "BITGET", 8, MUTED, true);
row1.addSpacer(4);
txt(row1, "· DKTrading", 8, MUTED);
row1.addSpacer();
if (stale) { txt(row1, "⚠", 8, AMBER); row1.addSpacer(2); }
txt(row1, updAt, 8, MUTED);

widget.addSpacer(4);

// ── Row 2: Balance & Investment side by side ──
const row2 = widget.addStack();
row2.layoutHorizontally();

const balCol = row2.addStack();
balCol.layoutVertically();
txt(balCol, "BALANCE", 7, MUTED);
txt(balCol, fmtUSD(bal), 13, WHITE, true);

row2.addSpacer();

const invCol = row2.addStack();
invCol.layoutVertically();
txt(invCol, "INVESTED", 7, MUTED);
txt(invCol, fmtUSD(inv), 13, WHITE, true);

widget.addSpacer(3);

// ── Separator ──
const sepRow = widget.addStack();
const sep = sepRow.addText("───────────────────────");
sep.font = Font.systemFont(5);
sep.textColor = SEP_C;

widget.addSpacer(3);

// ── Row 3: Daily PnL (big) ──
const row3 = widget.addStack();
row3.layoutHorizontally();
row3.centerAlignContent();
txt(row3, "TODAY", 7, MUTED);
row3.addSpacer(6);
txt(row3, fmtPnL(pnl), 16, pnlColor, true);
row3.addSpacer();

widget.addSpacer(2);

// ── Row 4: Open PnL | Positions | All-time ──
const row4 = widget.addStack();
row4.layoutHorizontally();

const col1 = row4.addStack();
col1.layoutVertically();
txt(col1, "OPEN PNL", 7, MUTED);
const oPnlColor = oPnl >= 0 ? GREEN : RED;
txt(col1, fmtPnL(oPnl), 10, oPnlColor, true);

row4.addSpacer();

const col2 = row4.addStack();
col2.layoutVertically();
txt(col2, "POSITIONS", 7, MUTED);
txt(col2, String(nPos), 10, WHITE, true);

row4.addSpacer();

const col3 = row4.addStack();
col3.layoutVertically();
txt(col3, "ALL-TIME", 7, MUTED);
const allColor = allPnl >= 0 ? GREEN : RED;
txt(col3, fmtPnL(allPnl), 10, allColor, true);

Script.setWidget(widget);
Script.complete();
