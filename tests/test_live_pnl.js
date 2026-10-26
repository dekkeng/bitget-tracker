const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');

test('dashboard live-price updates preserve Open PnL from accounts without detailed positions', () => {
    const html = fs.readFileSync(path.resolve(__dirname, '../static/index.html'), 'utf8');
    const source = html.slice(html.indexOf('  function livePositions()'), html.indexOf('  async function fetchPrices()'));
    const elements = {};
    const context = {
        _lastPositions: [{ symbol: 'XAUUSD', side: 'long', entry_price: 4000, size: 0.01, unrealized_pnl: 2 }],
        _lastMt5: { open_positions_pnl: 12 }, _livePrices: { XAUUSD: 4005 }, LOT_MULT: { XAUUSD: 100 },
        _liveOpenDelta: 0, fmt: x => x, colorClass: () => '', renderPositions() {}, renderBalance() {},
        document: { getElementById: id => elements[id] ??= { style: {} } },
    };
    vm.runInNewContext(source + '\napplyLivePnl();', context);
    assert.equal(elements['open-pnl'].textContent, 15); // 12 + (5 - 2), preserving the other $10
    assert.equal(context._liveOpenDelta, 3);
});

test('displayed total equity includes existing floating profit as well as live price changes', () => {
    const html = fs.readFileSync(path.resolve(__dirname, '../static/index.html'), 'utf8');
    const formula = html.match(/const displayBalance = [\s\S]*?;/)[0];
    const value = vm.runInNewContext(formula + '\ndisplayBalance;', {
        mt5: { total_balance: 100, open_positions_pnl: 12 },
        _earnTotal: 20, _eliteTotal: 50, _liveOpenDelta: 3,
    });
    assert.equal(value, 185);
});
