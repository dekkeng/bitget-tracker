// SETUP: In Scriptable, run this script once manually first.
// It will prompt you to enter your server URL (e.g. https://bitget-tracker-v2.onrender.com)
// and save it to Keychain automatically.
// Supports: Medium (home screen) and accessoryRectangular (lock screen).

const KEYCHAIN_KEY = "bitget_tracker_url";
const GREEN  = new Color("#00c47a");
const RED    = new Color("#ff4d4d");
const AMBER  = new Color("#f59e0b");
const WHITE  = new Color("#ffffff");
const MUTED  = new Color("#888888");
const BG     = new Color("#111111");
const SEP_C  = new Color("#333333");

// ── First-run setup ──
if (!config.runsInWidget) {
  const stored = Keychain.contains(KEYCHAIN_KEY) ? Keychain.get(KEYCHAIN_KEY) : null;
  const prompt = new Alert();
  prompt.title = "Bitget Tracker Setup";
  prompt.message = "Enter your server URL (e.g. https://bitget-tracker-v2.onrender.com)";
  prompt.addTextField("Server URL", stored || "https://bitget-tracker-v2.onrender.com");
  prompt.addAction("Save");
  prompt.addCancelAction("Cancel");
  const idx = await prompt.presentAlert();
  if (idx === 0) {
    const url = prompt.textFieldValue(0).replace(/\/$/, "");
    Keychain.set(KEYCHAIN_KEY, url);
    const done = new Alert();
    done.title = "Saved!";
    done.message = `URL saved: ${url}\n\nAdd a Medium widget (home) or Rectangular widget (lock screen).`;
    done.addAction("OK");
    await done.presentAlert();
  }
  Script.complete();
  return;
}

// ── Shared helpers ──
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
  t.minimumScaleFactor = 0.7;
  return t;
}

function fmtUSD(n) {
  if (n == null) return "$0";
  return "$" + Math.abs(n).toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 });
}

function fmtPnL(n) {
  return (n >= 0 ? "+" : "-") + fmtUSD(n);
}

function fmtShort(n) {
  if (n == null) return "$0";
  const abs = Math.abs(n);
  if (abs >= 1000) return (n >= 0 ? "+" : "-") + "$" + (abs / 1000).toFixed(1) + "k";
  return (n >= 0 ? "+" : "-") + "$" + abs.toFixed(2);
}

// ── Fetch data ──
if (!Keychain.contains(KEYCHAIN_KEY)) {
  const w = new ListWidget();
  w.backgroundColor = BG;
  const t = w.addText("⚙ Run script to set up");
  t.font = Font.systemFont(10);
  t.textColor = MUTED;
  Script.setWidget(w);
  Script.complete();
  return;
}

const baseUrl = Keychain.get(KEYCHAIN_KEY);
const { data, stale } = await fetchData(baseUrl);

if (!data) {
  const w = new ListWidget();
  w.backgroundColor = BG;
  const t = w.addText("⚠ No data");
  t.font = Font.boldSystemFont(11);
  t.textColor = AMBER;
  Script.setWidget(w);
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

const family = config.widgetFamily;

// ═══════════════════════════════════════════════════════════════════════════
// LOCK SCREEN — accessoryRectangular (small rectangle on lock screen)
// ═══════════════════════════════════════════════════════════════════════════
if (family === "accessoryRectangular") {
  const lw = new ListWidget();
  lw.setPadding(0, 0, 0, 0);

  const pnlStr = (pnl >= 0 ? "+" : "-") + fmtUSD(pnl);
  const oPnlStr = (oPnl >= 0 ? "+" : "-") + fmtUSD(oPnl);
  const allStr = (allPnl >= 0 ? "+" : "-") + fmtUSD(allPnl);
  const oPnlC = oPnl >= 0 ? GREEN : RED;
  const allC = allPnl >= 0 ? GREEN : RED;
  const posLabel = nPos === 1 ? "1 position" : nPos + " positions";

  // Row 1: Bal  $1,005.53
  const r1 = lw.addStack();
  r1.layoutHorizontally();
  r1.centerAlignContent();
  txt(r1, "Bal  " + fmtUSD(bal), 12, WHITE, true);
  r1.addSpacer();
  if (stale) { txt(r1, "stale", 10, AMBER); }

  lw.addSpacer(1);

  // Row 2: Today  +$39.42
  const r2 = lw.addStack();
  r2.layoutHorizontally();
  r2.centerAlignContent();
  txt(r2, "Today  ", 10, MUTED);
  txt(r2, pnlStr, 12, pnlColor, true);

  lw.addSpacer(1);

  // Row 3: Open +$0.00 | 0 positions
  const r3 = lw.addStack();
  r3.layoutHorizontally();
  r3.centerAlignContent();
  txt(r3, "Open " + oPnlStr + "  |  " + posLabel, 10, oPnlC);

  lw.addSpacer(1);

  // Row 4: All time  +$81.17
  const r4 = lw.addStack();
  r4.layoutHorizontally();
  r4.centerAlignContent();
  txt(r4, "All time  ", 10, MUTED);
  txt(r4, allStr, 12, allC, true);

  Script.setWidget(lw);
  Script.complete();
  return;
}

// ═══════════════════════════════════════════════════════════════════════════
// HOME SCREEN — medium (default)
// ═══════════════════════════════════════════════════════════════════════════
const widget = new ListWidget();
widget.backgroundColor = BG;
widget.setPadding(14, 14, 14, 14);
widget.url = baseUrl;

// Row 1: Header
const row1 = widget.addStack();
row1.layoutHorizontally();
row1.centerAlignContent();
txt(row1, "BITGET", 10, MUTED, true);
row1.addSpacer(4);
txt(row1, "· DKTrading", 10, MUTED);
row1.addSpacer();
if (stale) { txt(row1, "⚠", 10, AMBER); row1.addSpacer(2); }
txt(row1, updAt, 10, MUTED);

widget.addSpacer(6);

// Row 2: Balance & Investment
const row2 = widget.addStack();
row2.layoutHorizontally();

const balCol = row2.addStack();
balCol.layoutVertically();
txt(balCol, "BALANCE", 8, MUTED);
txt(balCol, fmtUSD(bal), 16, WHITE, true);

row2.addSpacer();

const invCol = row2.addStack();
invCol.layoutVertically();
txt(invCol, "INVESTED", 8, MUTED);
txt(invCol, fmtUSD(inv), 16, WHITE, true);

widget.addSpacer(6);

// Separator
const sepRow = widget.addStack();
const sep = sepRow.addText("─────────────────────────────");
sep.font = Font.systemFont(6);
sep.textColor = SEP_C;

widget.addSpacer(6);

// Row 3: Daily PnL
const row3 = widget.addStack();
row3.layoutHorizontally();
row3.centerAlignContent();
txt(row3, "TODAY", 8, MUTED);
row3.addSpacer(8);
txt(row3, fmtPnL(pnl), 20, pnlColor, true);
row3.addSpacer();

widget.addSpacer(6);

// Row 4: Open PnL | Positions | All-time
const row4 = widget.addStack();
row4.layoutHorizontally();

const col1 = row4.addStack();
col1.layoutVertically();
txt(col1, "OPEN P&L", 8, MUTED);
const oPnlColor = oPnl >= 0 ? GREEN : RED;
txt(col1, fmtPnL(oPnl), 11, oPnlColor, true);

row4.addSpacer();

const col2 = row4.addStack();
col2.layoutVertically();
txt(col2, "POS", 8, MUTED);
txt(col2, String(nPos), 11, WHITE, true);

row4.addSpacer();

const col3 = row4.addStack();
col3.layoutVertically();
txt(col3, "ALL-TIME", 8, MUTED);
const allColor = allPnl >= 0 ? GREEN : RED;
txt(col3, fmtPnL(allPnl), 11, allColor, true);

Script.setWidget(widget);
Script.complete();
