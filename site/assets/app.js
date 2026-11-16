/* Fills the dashboard from window.MCB_RESULTS (written by build/mcb_report). */
(function () {
  'use strict';

  var C = window.MCBCharts, h = C.h;
  var R = window.MCB_RESULTS;
  var MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

  function $(id) { return document.getElementById(id); }
  function isNum(v) { return typeof v === 'number' && isFinite(v); }
  function fx(v, d) {
    if (!isNum(v)) return 'n/a';
    var t = v.toFixed(d === undefined ? 2 : d);
    if (+t === 0) t = t.replace('-', '');            // never show a negative zero
    return t.replace('-', '\u2212');                 // typographic minus sign
  }
  function cur(symbol, v) { return isNum(v) ? (v < 0 ? '\u2212' : '') + symbol + Math.abs(Math.round(v)).toLocaleString('en-US') : 'n/a'; }
  function pct(v, d) { return isNum(v) ? fx(v, d) + '%' : 'n/a'; }
  function signed(v, d) {
    if (!isNum(v)) return 'n/a';
    var r = +v.toFixed(d === undefined ? 2 : d);   // sign of the rounded value, so a tiny negative is not shown as "-0.00"
    return (r > 0 ? '+' : r < 0 ? '\u2212' : '') + Math.abs(r).toFixed(d === undefined ? 2 : d);
  }
  function money(v) { return isNum(v) ? (v < 0 ? '−' : '') + Math.abs(Math.round(v)).toLocaleString('en-US') : 'n/a'; }
  function sci(v) {
    if (!isNum(v)) return 'n/a';
    if (v === 0) return '0';
    var a = Math.abs(v);
    return ((a < 1e-3 || a >= 1e5) ? v.toExponential(1) : String(+v.toPrecision(3))).replace(/^-/, '\u2212');
  }
  function find(list, key, value) {
    for (var i = 0; i < (list || []).length; i++) if (list[i][key] === value) return list[i];
    return null;
  }
  function unavailable(el, section, what) {
    if (!el) return;
    el.textContent = '';
    h('div', 'empty', what + ' is not available in this run' + (section && section.reason ? ': ' + section.reason : '.'), el);
  }

  /* Builds a table. Cells may be strings or DOM nodes. */
  function table(el, columns, rows) {
    el.textContent = '';
    var t = h('table', null, null, el);
    var tr = h('tr', null, null, h('thead', null, null, t));
    columns.forEach(function (c) { h('th', c.num ? 'num' : null, c.name, tr); });
    var tb = h('tbody', null, null, t);
    rows.forEach(function (r) {
      var row = h('tr', null, null, tb);
      r.forEach(function (v, i) {
        var td = h('td', ((columns[i].num ? 'num ' : '') + (columns[i].cls || '')).trim() || null, null, row);
        if (v && v.nodeType) td.appendChild(v); else td.textContent = v === null || v === undefined ? '' : v;
      });
    });
    return t;
  }

  function tile(el, label, value, sub) {
    var t = h('div', 'tile', null, el);
    h('div', 'label', label, t);
    h('div', 'value', value, t);
    if (sub) h('div', 'sub', sub, t);
  }

  /* Direction is carried by the icon and the word; colour only tints the pill. */
  function signalPill(signal) {
    var cls = signal === 'BUY' ? 'buy' : signal === 'SELL' ? 'sell' : 'hold';
    var icon = signal === 'BUY' ? '▲' : signal === 'SELL' ? '▼' : '–';
    var p = h('span', 'pill ' + cls, null, null);
    h('span', null, icon, p).setAttribute('aria-hidden', 'true');
    h('span', null, signal, p);
    return p;
  }

  function statusCell(ok) {
    var s = h('span', 'status ' + (ok === null || ok === undefined ? 'info' : ok ? 'ok' : 'bad'), null, null);
    h('span', null, ok === null || ok === undefined ? 'ℹ' : ok ? '✓' : '✗', s).setAttribute('aria-hidden', 'true');
    h('span', null, ok === null || ok === undefined ? 'Info' : ok ? 'Pass' : 'Fail', s);
    return s;
  }

  function divBar(value, scale) {
    var wrap = h('div', 'divbar', null, null);
    h('span', 'mid', null, wrap);
    var fill = h('span', 'fill ' + (value >= 0 ? 'pos' : 'neg'), null, wrap);
    fill.style.width = Math.min(50, Math.abs(value) / scale * 50) + '%';
    wrap.title = signed(value, 0) + ' bp';
    return wrap;
  }

  function dateAxis(dates, maxTicks) {
    var vals = dates.map(function (d) { return Date.parse(d + 'T00:00:00Z'); });
    var n = dates.length, k = Math.min(maxTicks || 6, n), tk = [];
    for (var i = 0; i < k; i++) tk.push(vals[Math.round(i * (n - 1) / Math.max(1, k - 1))]);
    var shortSpan = (vals[n - 1] - vals[0]) < 150 * 864e5;   // under five months: show the day as well
    return {
      values: vals, labels: dates, ticks: tk, name: 'Date',
      format: function (v) {
        var d = new Date(v);
        return shortSpan ? d.getUTCDate() + ' ' + MONTHS[d.getUTCMonth()] : MONTHS[d.getUTCMonth()] + ' ' + String(d.getUTCFullYear()).slice(2);
      }
    };
  }
  function yearsAxis(years) {
    return { values: years, title: 'Maturity (years)', labels: years.map(function (y) { return (+y.toFixed(2)) + ' years'; }),
      format: function (v) { return String(v); } };
  }
  var pct2 = function (v) { return pct(v, 2); };
  var pct1 = function (v) { return pct(v, 1); };

  /* ---- theme ------------------------------------------------------------------ */
  (function () {
    var btn = $('theme'), modes = ['auto', 'light', 'dark'], cur = 'auto';
    try { cur = localStorage.getItem('mcb-theme') || 'auto'; } catch (e) { cur = 'auto'; }
    function apply() {
      if (cur === 'auto') document.documentElement.removeAttribute('data-theme');
      else document.documentElement.setAttribute('data-theme', cur);
      btn.textContent = 'Theme: ' + cur;
    }
    btn.addEventListener('click', function () {
      cur = modes[(modes.indexOf(cur) + 1) % modes.length];
      try { localStorage.setItem('mcb-theme', cur); } catch (e) { /* storage unavailable */ }
      apply();
    });
    apply();
  })();

  if (!R) {
    $('meta').textContent = 'No results found.';
    var o = $('tiles');
    h('div', 'empty', 'No results yet. Run scripts/fetch_data.sh and then build/mcb_report to create site/data/results.js.', o);
    return;
  }

  /* ---- overview ---------------------------------------------------------------- */
  function renderOverview() {
    var m = R.meta || {}, usd = R.usd || {};
    $('meta').textContent = 'Generated ' + (m.generatedAt || '').replace('T', ' ').replace('Z', ' UTC') + ' · ' + (m.engine || '');
    if (m.disclaimer) $('disclaimer').textContent = m.disclaimer;

    var tiles = $('tiles');
    var tsy = usd.treasury && usd.treasury.available ? usd.treasury : null;
    var sofr = usd.sofr && usd.sofr.available ? usd.sofr : null;
    if (tsy) {
      var t10 = find(tsy.quotes, 'tenor', '10Y'), t2 = find(tsy.quotes, 'tenor', '2Y');
      if (t10) tile(tiles, '10Y Treasury par yield', pct(t10.parYield, 2), 'as of ' + tsy.asOf);
      if (t10 && t2) tile(tiles, 'Treasury 2s10s slope', signed((t10.parYield - t2.parYield) * 100, 0) + ' bp', '10Y minus 2Y par yield');
    }
    if (sofr) {
      var s10 = find(sofr.quotes, 'tenor', '10Y');
      if (s10) tile(tiles, '10Y SOFR swap rate', pct(s10.rate, 2), 'as of ' + sofr.asOf);
    }
    var sp10 = find(usd.swapSpreads, 'tenor', '10Y');
    if (sp10) tile(tiles, '10Y swap spread', signed(sp10.spreadBp, 1) + ' bp', 'z-score ' + signed(sp10.zScore, 2) + ' over ' + sp10.observations + ' days');
    if (usd.sofrFixing) tile(tiles, 'SOFR overnight fixing', pct(usd.sofrFixing.rate, 2), 'for ' + usd.sofrFixing.date);
    if (R.moex && R.moex.available && R.moex.curve) {
      var i10 = R.moex.curve.years.indexOf(10);
      if (i10 >= 0) tile(tiles, 'OFZ 10Y zero yield (G-curve)', pct(R.moex.curve.yield[i10], 2), 'as of ' + R.moex.asOf);
    }
    var passed = m.checksPassed || 0, failed = m.checksFailed || 0;
    tile(tiles, 'Validation checks passed', passed + ' / ' + (passed + failed), failed ? failed + ' failed, see Validation' : 'all passed');

    var sig = R.signals || [];
    if (sig.length) {
      table($('signals'), [{ name: 'Market' }, { name: 'Asset' }, { name: 'Signal' }, { name: 'Edge', num: true },
        { name: 'Confidence', num: true }, { name: 'Horizon', cls: 'nowrap' }, { name: 'Why', cls: 'why' }],
        sig.map(function (s) {
          return [s.market, s.asset, signalPill(s.signal), s.edgeUnit.indexOf('bp') === 0 ? signed(s.edge, 0) + ' bp' : signed(s.edge, 2) + '%',
            fx(s.confidence, 2), s.horizon, s.why];
        }));
    } else unavailable($('signals'), null, 'The signal table');

    var rules = $('signal-rules');
    h('strong', null, 'How the signals are made. ', rules);
    var parts = [];
    if (R.stats && R.stats.returns) parts.push('Treasuries: ' + R.stats.returns.rule + '.');
    if (R.moex && R.moex.ofz && R.moex.ofz.rule) parts.push('Government bonds on the Moscow Exchange: ' + R.moex.ofz.rule);
    parts.push('Edge is the size of the expected advantage (percent of expected excess return, or basis points of spread). ' +
      'Confidence is the simulated probability of a positive return for Treasuries and the size of the mispricing in standard deviations for individual bonds.');
    rules.appendChild(document.createTextNode(parts.join(' ')));

    table($('sources'), [{ name: 'Source' }, { name: 'Data as of' }, { name: 'Status' }, { name: 'Note' }],
      (m.sources || []).map(function (s) {
        var a = h('a', null, s.name, null);
        a.href = s.url; a.rel = 'noopener';
        var st = h('span', 'status ' + (s.status === 'ok' || s.status === 'frozen' ? 'ok' : s.status === 'fallback' ? 'info' : 'bad'), null, null);
        h('span', null, s.status === 'ok' || s.status === 'frozen' ? '✓' : s.status === 'fallback' ? 'ℹ' : '✗', st).setAttribute('aria-hidden', 'true');
        h('span', null, s.status, st);
        return [a, s.asOf || '—', st, s.note];
      }));
  }

  /* ---- USD ------------------------------------------------------------------- */
  function renderUsd() {
    var usd = R.usd || {};
    var tsy = usd.treasury && usd.treasury.available ? usd.treasury : null;
    var sofr = usd.sofr && usd.sofr.available ? usd.sofr : null;
    if (!tsy && !sofr) {
      unavailable($('usd-zero'), usd.sofr, 'USD market data');
      return;
    }
    var base = (tsy || sofr).curve, zs = [], fs = [];
    if (tsy) { zs.push({ name: 'Treasury', values: tsy.curve.zero, slot: 1 }); fs.push({ name: 'Treasury', values: tsy.curve.forward, slot: 1 }); }
    if (sofr) { zs.push({ name: 'SOFR swaps', values: sofr.curve.zero, slot: 2 }); fs.push({ name: 'SOFR swaps', values: sofr.curve.forward, slot: 2 }); }
    C.lineChart($('usd-zero'), { title: 'Zero rates', subtitle: 'Continuously compounded, bootstrapped from ' + (tsy ? tsy.asOf : sofr.asOf) + ' quotes',
      x: yearsAxis(base.years), series: zs, yFormat: pct2 });
    C.lineChart($('usd-fwd'), { title: 'Instantaneous forward rates', subtitle: 'The rate each curve implies for a very short loan starting at that maturity',
      x: yearsAxis(base.years), series: fs, yFormat: pct2 });

    if (tsy && tsy.history && tsy.history.dates.length > 2) {
      C.lineChart($('usd-hist'), { title: 'Treasury par yields over the past year', subtitle: 'Daily closing par yields',
        x: dateAxis(tsy.history.dates, 6),
        series: [{ name: '2-year', short: '2Y', values: tsy.history.y2, slot: 1 }, { name: '10-year', short: '10Y', values: tsy.history.y10, slot: 2 },
          { name: '30-year', short: '30Y', values: tsy.history.y30, slot: 3 }], yFormat: pct2 });
    } else unavailable($('usd-hist'), usd.treasury, 'Treasury history');

    var sh = usd.swapSpreadHistory;
    if (sh && sh.dates.length > 2) {
      C.lineChart($('usd-spread'), { title: '10-year swap spread', subtitle: 'SOFR swap rate minus Treasury par yield, basis points',
        x: dateAxis(sh.dates, 5), series: [{ name: '10Y swap spread', values: sh.spreadBp, slot: 1 }],
        yFormat: function (v) { return fx(v, 0) + ' bp'; } });
    } else unavailable($('usd-spread'), null, 'Swap spread history');

    if (sofr) {
      table($('usd-sofr-table'), [{ name: 'SOFR swap' }, { name: 'Par rate', num: true }, { name: 'Zero rate', num: true },
        { name: 'Discount factor', num: true }, { name: 'DV01 per $1mm', num: true }],
        sofr.quotes.map(function (q) { return [q.tenor, pct(q.rate, 3), pct(q.zero, 3), fx(q.discount, 5), cur('$', q.dv01)]; }));
      var note = 'Bootstrapped with ' + sofr.bootstrap.interpolation + ' interpolation; every swap reprices to ' + sci(sofr.bootstrap.maxRepricingErrorBp) + ' bp.';
      if (sofr.eris) note += ' Against the discount factors Eris publishes itself, our zero rates differ by at most ' + fx(sofr.eris.maxZeroDiffBp, 2) +
        ' bp across ' + sofr.eris.dates.toLocaleString('en-US') + ' dates.';
      $('usd-sofr-note').textContent = note;
    } else unavailable($('usd-sofr-table'), usd.sofr, 'The SOFR curve');

    if (tsy) {
      table($('usd-tsy-table'), [{ name: 'Treasury' }, { name: 'Par yield', num: true }, { name: 'Zero rate', num: true },
        { name: 'Duration', num: true }, { name: 'DV01 per $100', num: true }],
        tsy.quotes.map(function (q) { return [q.tenor, pct(q.parYield, 2), pct(q.zero, 3), isNum(q.duration) ? fx(q.duration, 2) : '—', isNum(q.dv01) ? fx(q.dv01, 4) : '—']; }));
      $('usd-tsy-note').textContent = 'Source: ' + tsy.source + '. Bills (under one year) are simple yields; one year and longer are semi-annual par bonds. Interpolation: ' + tsy.bootstrap.interpolation + '.';
    } else unavailable($('usd-tsy-table'), usd.treasury, 'The Treasury curve');

    if (sofr && sofr.forwardSwaps) {
      table($('usd-fwdswaps'), [{ name: 'Forward SOFR swap' }, { name: 'Rate', num: true }],
        sofr.forwardSwaps.map(function (f) { return [f.label + '  (starts in ' + f.label.split('Y')[0] + 'Y, runs ' + f.label.split('Y')[1] + 'Y)', pct(f.rate, 3)]; }));
    } else unavailable($('usd-fwdswaps'), usd.sofr, 'Forward swap rates');

    if (usd.swapSpreads && usd.swapSpreads.length) {
      table($('usd-spreads-table'), [{ name: 'Maturity' }, { name: 'Swap spread', num: true }, { name: 'Average', num: true },
        { name: 'Z-score', num: true }, { name: 'Days', num: true }],
        usd.swapSpreads.map(function (s) { return [s.tenor, signed(s.spreadBp, 1) + ' bp', signed(s.meanBp, 1) + ' bp', signed(s.zScore, 2), String(s.observations)]; }));
    } else unavailable($('usd-spreads-table'), null, 'Swap spreads');

    var pf = usd.portfolio;
    if (pf && pf.items && pf.items.length) {
      table($('usd-portfolio'), [{ name: 'Position' }, { name: 'Notional', num: true }, { name: 'Rate / coupon', num: true },
        { name: 'Fair rate / yield', num: true }, { name: 'Curve price', num: true }, { name: 'Market price', num: true },
        { name: 'Rich (+) / cheap (−)', num: true }, { name: 'Spread to SOFR', num: true }, { name: 'Value (USD)', num: true }, { name: 'DV01 (USD)', num: true }],
        pf.items.map(function (it) {
          var isBond = it.type === 'UST';
          return [isBond ? it.description + ' (' + it.side.toLowerCase() + ')' : it.description, money(it.notional), pct(it.rate, 3),
            isBond ? pct(it.yield, 3) : pct(it.fairRate, 3), isBond ? fx(it.modelPrice, 3) : '—',
            isNum(it.marketPrice) ? fx(it.marketPrice, 3) : '—', isNum(it.richCheap) ? signed(it.richCheap, 3) : '—',
            isNum(it.zSpreadSofrBp) ? signed(it.zSpreadSofrBp, 1) + ' bp' : '—', money(it.value), money(it.dv01)];
        }));
      var risk = function (el, r, title, sub) {
        if (!r || !r.buckets) { el.textContent = ''; return; }
        var rows = r.buckets.filter(function (b) { return Math.abs(b.jacobian) >= 0.5 || Math.abs(b.bump || 0) >= 0.5; })
          .map(function (b) { return { label: b.id.replace('SOFR', 'SOFR ').replace('UST_', 'Treasury '), value: b.jacobian,
            extra: [{ value: money(b.bump), name: 'bump-and-rebuild' }] }; });
        C.barChart(el, { title: title, subtitle: sub + ' Buckets sum to ' + money(r.sumJacobian) + '; all quotes moved together: ' + money(r.parallel) + '.',
          rows: rows, format: money, valueName: 'USD per +1bp (Jacobian)', labelTitle: 'Curve input' });
      };
      risk($('usd-risk-swaps'), pf.swapRisk, 'Swap risk by SOFR tenor', 'Gain or loss in USD if one swap quote rises 1bp.');
      risk($('usd-risk-bonds'), pf.bondRisk, 'Bond risk by Treasury tenor', 'Gain or loss in USD if one par yield rises 1bp.');
    } else unavailable($('usd-portfolio'), pf, 'The sample portfolio');
  }

  /* ---- forecast ---------------------------------------------------------------- */
  function renderForecast() {
    var st = R.stats;
    if (!st || !st.available) { unavailable($('fc-fans'), st, 'The forecast'); return; }
    var fc = st.forecast, steps = [], labels = [];
    for (var i = 0; i < fc.dates.length; i++) { steps.push(i); labels.push(i === 0 ? fc.dates[0] + ' (today)' : fc.dates[i] + ' (+' + 3 * i + ' months)'); }
    var xAxis = { values: steps, labels: labels, name: 'Date', ticks: [0, 2, 4, 6, 8].filter(function (s) { return s < steps.length; }),
      format: function (s) { return s === 0 ? 'now' : '+' + (3 * s) + 'M'; } };
    var wrap = $('fc-fans');
    wrap.textContent = '';
    ['2Y', '5Y', '10Y'].forEach(function (tenor) {
      var fan = find(fc.fans, 'tenor', tenor);
      if (!fan) return;
      var pre = function (a) { return [fan.current].concat(a); };
      var hasFwd = fan.forwardImplied.some(isNum);
      var series = [{ name: 'Median simulated path', short: 'median', values: pre(fan.q50), slot: 1 }];
      if (hasFwd) series.push({ name: 'Implied by today’s forwards', short: 'forwards', values: pre(fan.forwardImplied), slot: 2 });
      C.lineChart(h('div', null, null, wrap), {
        title: tenor + ' Treasury zero yield', subtitle: 'Now ' + pct(fan.current, 2) + '; next two years, ' + fc.paths.toLocaleString('en-US') + ' simulated paths',
        x: xAxis, series: series, height: 230,
        bands: [{ name: '5-95% of paths', lo: pre(fan.q05), hi: pre(fan.q95), slot: 1, opacity: 0.12 },
          { name: '16-84% of paths', lo: pre(fan.q16), hi: pre(fan.q84), slot: 1, opacity: 0.2 }],
        yFormat: pct1
      });
    });

    C.lineChart($('fc-short'), { title: 'Three-month rate: forwards versus history-based expectation',
      subtitle: 'The gap is the average term premium that forwards carry', x: xAxis, height: 250,
      series: [{ name: 'Average simulated path', short: 'simulated', values: fc.shortRate.expected, slot: 1 },
        { name: 'Forward curve today', short: 'forwards', values: fc.shortRate.forward, slot: 2 }], yFormat: pct2 });

    var rows = st.returns.rows;
    C.intervalChart($('fc-returns'), { title: 'One-year excess return over a 1Y bill, by maturity', subtitle: 'Distribution across simulated paths, percent',
      names: { mid: 'simulated mean', inner: '16-84% of paths', outer: '5-95% of paths', marker: 'carry and roll-down' }, labelTitle: 'Treasury',
      format: function (v) { return signed(v, 1) + '%'; },
      rows: rows.map(function (r) {
        return { label: r.tenor, lo2: r.simulated.q05, lo1: r.simulated.q16, mid: r.simulated.mean, hi1: r.simulated.q84, hi2: r.simulated.q95,
          marker: r.carryRollDown, extra: [{ value: pct(r.probPositive * 100, 0), name: 'probability of a positive return' },
            { value: signed(r.regressionExpected, 2) + '%', name: 'regression forecast' }] };
      }) });

    table($('fc-table'), [{ name: 'Treasury' }, { name: 'Zero yield', num: true }, { name: 'Carry + roll-down', num: true },
      { name: 'Simulated mean (16-84% range)', num: true }, { name: 'P(return > 0)', num: true },
      { name: 'Regression forecast (68% band)', num: true }, { name: 'Slope p: bootstrap / normal', num: true }, { name: 'Signal' }],
      rows.map(function (r) {
        return [r.tenor, pct(r.yield, 2), signed(r.carryRollDown, 2) + '%',
          signed(r.simulated.mean, 2) + '%  (' + signed(r.simulated.q16, 1) + ' to ' + signed(r.simulated.q84, 1) + ')', pct(r.probPositive * 100, 0),
          signed(r.regressionExpected, 2) + '%  (' + signed(r.regressionLo68, 1) + ' to ' + signed(r.regressionHi68, 1) + ')',
          fx(r.slopePBootstrap, 2) + ' / ' + fx(r.slopePAsymptotic, 2), signalPill(r.signal)];
      }));
    $('fc-note').textContent = 'Starting curve: ' + st.current.source + ' (' + st.current.date + '). "Excess return" is the log return of buying the bond and selling it a year later, minus the one-year yield. ' +
      'Carry and roll-down is that return if the curve does not move. The regression uses the level, slope and curvature of the curve; ' +
      'its slope p-value is shown twice because the textbook (normal) p-value overstates significance for persistent predictors, which is the paper’s point.';
  }

  /* ---- paper replication -------------------------------------------------------- */
  var EH_TEXT = {
    'CS2 (m=4)': 'Campbell-Shiller: change in the long yield over 1 year on the scaled yield spread',
    'CS2 (m=20)': 'Campbell-Shiller: change in the long yield over 5 years on the scaled yield spread',
    'CS1': 'Campbell-Shiller: average future short rate on the yield spread',
    'FB': 'Fama-Bliss: future short rate on the forward spread',
    'CG (m=4)': 'The paper’s test: forward rate 1 year ahead on today’s forward',
    'CG (m=20)': 'The paper’s test: forward rate 5 years ahead on today’s forward'
  };

  function renderPaper() {
    var st = R.stats;
    if (!st || !st.available) { unavailable($('paper-tiles'), st, 'The paper replication'); return; }
    var s = st.sample, tiles = $('paper-tiles');
    tile(tiles, 'Sample', s.observations + ' quarters', s.from + ' to ' + s.to);
    tile(tiles, 'Maturities', s.maturities + ' quarterly', '3 months to 10 years');
    tile(tiles, 'Block length', s.blockLength + ' quarters', 'rule of thumb (T × N)^(2/5)');
    tile(tiles, 'Autocorrelation of the 5Y yield', fx(s.autocorrYield5y, 2), 'yields are very persistent');
    tile(tiles, 'Largest autocorrelation of a difference return', fx(s.maxAutocorrDifferenceReturn, 2), 'the resampled objects are close to news');
    tile(tiles, 'Variance in the first principal component', pct(s.pcaYields[0], 1), 'yields; difference returns: ' + pct(s.pcaDifferenceReturns[0], 1));

    var ex = st.exampleHistory;
    C.lineChart($('paper-history'), { title: 'The real history and one bootstrapped history of the 10-year yield',
      subtitle: 'Every bootstrapped history is internally consistent: its yields, forwards and returns satisfy the same identities as real data',
      x: dateAxis(ex.dates, 8), height: 260,
      series: [{ name: 'Observed 10Y yield', short: 'observed', values: ex.real10y, slot: 1 },
        { name: 'One bootstrap draw', short: 'bootstrap', values: ex.boot10y, slot: 2 }], yFormat: pct1 });

    var wrap = $('paper-eh');
    wrap.textContent = '';
    st.ehTests.forEach(function (t) {
      C.lineChart(h('div', null, null, wrap), { title: t.test, subtitle: EH_TEXT[t.test] || '', height: 240,
        x: { values: t.years, title: 'Bond maturity (years)', labels: t.years.map(function (y) { return y + ' years'; }), format: function (v) { return String(v); } },
        series: [{ name: 'Estimated slope', values: t.slope, slot: 1 }],
        bands: [{ name: '5-95% across bootstrap histories', lo: t.q05, hi: t.q95, slot: 1, opacity: 0.16 }],
        refLines: [{ y: 1, label: 'EH: slope = 1' }], yDomain: [-3.5, 3.5],
        yFormat: function (v) { return fx(v, 2); } });
    });

    var cols = [{ name: 'Test' }, { name: 'Maturity', num: true }, { name: 'Slope', num: true }, { name: 'Bootstrap 5-95%', num: true },
      { name: 'p-value: slope = 1', num: true }, { name: 'p-value: slope = 0', num: true }];
    var out = [];
    st.ehTests.forEach(function (t) {
      [5, 10].forEach(function (y) {
        var i = t.years.indexOf(y);
        if (i < 0) return;
        out.push([t.test, y + 'Y', fx(t.slope[i], 2), fx(t.q05[i], 2) + ' to ' + fx(t.q95[i], 2), fx(t.pSlopeIsOne[i], 3), fx(t.pSlopeIsZero[i], 3)]);
      });
    });
    table($('paper-table'), cols, out);
  }

  /* ---- EUR ---------------------------------------------------------------------- */
  function renderEur() {
    var e = R.eur;
    if (!e || !e.available) { unavailable($('eur-zero'), e, 'The EUR multicurve'); return; }
    var c = e.curves, yrs = c['EUR-OIS'].years;
    C.lineChart($('eur-zero'), { title: 'Zero rates of the three curves', subtitle: 'Market of ' + e.asOf + ', log-cubic interpolation',
      x: yearsAxis(yrs), yFormat: pct2,
      series: [{ name: 'OIS (discounting)', short: 'OIS', values: c['EUR-OIS'].zero, slot: 1 },
        { name: 'Euribor 3M projection', short: '3M', values: c['EUR-3M'].zero, slot: 2 },
        { name: 'Euribor 6M projection', short: '6M', values: c['EUR-6M'].zero, slot: 3 }] });
    var f = e.forwards;
    C.lineChart($('eur-fwd'), { title: 'Forward rates: the tenor basis', subtitle: 'Six-month forwards on the OIS and 6M curves, three-month forwards on the 3M curve',
      x: { values: f.years, title: 'Forward start (years)', labels: f.years.map(function (y) { return 'in ' + y + ' years'; }), format: function (v) { return String(v); } },
      yFormat: pct2,
      series: [{ name: 'OIS-implied 6M rate', short: 'OIS', values: f.ois6m, slot: 1 },
        { name: 'Euribor 3M forward', short: '3M', values: f.euribor3m, slot: 2 },
        { name: 'Euribor 6M forward', short: '6M', values: f.euribor6m, slot: 3 }] });

    if (e.quantlib) {
      table($('eur-ql'), [{ name: 'Curve' }, { name: 'Interpolation' }, { name: 'Dates compared', num: true }, { name: 'Largest discount-factor gap', num: true }],
        e.quantlib.map(function (q) { return [q.curve, q.interpolation, String(q.dates), sci(q.maxDfDiff)]; }));
    } else unavailable($('eur-ql'), null, 'The QuantLib comparison');

    table($('eur-latency'), [{ name: 'Operation' }, { name: 'Median', num: true }, { name: '95th percentile', num: true }],
      e.latency.map(function (l) {
        var f1 = function (us) { return us >= 1000 ? fx(us / 1000, 2) + ' ms' : fx(us, 1) + ' µs'; };
        return [l.operation, f1(l.medianMicros), f1(l.p95Micros)];
      }));

    var r = e.riskExample, tiles = $('eur-risk-tiles');
    tile(tiles, 'Position', 'Pay 2.00%', '10Y vs Euribor 6M, EUR 1,000,000');
    tile(tiles, 'Value today', cur('\u20AC', r.npv), 'discounted on the OIS curve');
    tile(tiles, 'Fair swap rate', pct(r.parRate, 3), 'single-curve shortcut would give ' + pct(r.parRateSingleCurve, 3));
    tile(tiles, 'Parallel DV01', cur('\u20AC', r.parallel), 'all 90 quotes up 1bp together');
    tile(tiles, 'Sum of bucket deltas', cur('\u20AC', r.sumBump), 'Jacobian method: ' + cur('\u20AC', r.sumJacobian));
    var buckets = r.buckets.slice().sort(function (a, b) { return Math.abs(b.bump) - Math.abs(a.bump); }).slice(0, 12);
    C.barChart($('eur-risk'), { title: 'Which quotes does this swap depend on?', subtitle: 'Gain or loss in EUR if one market quote rises 1bp (largest 12 of 90); the two methods agree',
      rows: buckets.map(function (b) { return { label: b.id.replace(/_/g, ' '), value: b.bump, extra: [{ value: fx(b.jacobian, 2), name: 'Jacobian method' }, { value: b.curve, name: 'curve' }] }; }),
      format: function (v) { return fx(v, 2); }, valueName: 'EUR per +1bp (bump-and-rebuild)', labelTitle: 'Market quote' });

    table($('eur-instruments'), [{ name: 'Curve' }, { name: 'Instrument' }, { name: 'Type' }, { name: 'Quote', num: true },
      { name: 'Pillar date' }, { name: 'Discount factor', num: true }, { name: 'Repricing error', num: true }],
      e.instruments.map(function (i) { return [i.curve, i.id, i.kind, fx(i.quote, 4), i.pillar, fx(i.discount, 6), sci(i.error)]; }));
  }

  /* ---- MOEX --------------------------------------------------------------------- */
  function renderMoex() {
    var m = R.moex;
    if (!m || !m.available) { unavailable($('moex-curve'), m, 'Moscow Exchange data'); return; }
    var ofz = m.ofz && m.ofz.bonds ? m.ofz : null;
    var shortName = function (b) { var mm = /SU(\d{5})/.exec(b.secid); return mm ? mm[1] : b.secid; };
    var maxDuration = 0;
    if (ofz) ofz.bonds.forEach(function (b) { maxDuration = Math.max(maxDuration, b.duration); });
    C.scatterCurve($('moex-curve'), {
      title: 'G-curve and the bonds priced against it', subtitle: 'Zero-coupon curve of ' + m.asOf + ' ' + (m.curveTime || '') + '; each dot is one OFZ at its duration',
      curve: { x: m.curve.years, y: m.curve.yield, name: 'G-curve (zero-coupon yield)' }, pointsName: 'OFZ bonds (yield at duration); labelled bonds have a signal',
      xTitle: 'Duration (years)', xName: 'duration (years)', yName: 'yield', labelTitle: 'Bond', xMax: ofz ? Math.ceil(maxDuration + 1) : 20,
      xFormat: function (v) { return fx(v, 1); }, yFormat: pct2,
      points: ofz ? ofz.bonds.map(function (b) {
        return { x: b.duration, y: b.yield, label: 'OFZ ' + shortName(b) + ', due ' + b.maturity, emphasis: b.signal !== 'HOLD',
          tag: shortName(b) + ' ' + b.signal, tagBelow: b.signal === 'SELL',
          rows: [{ value: signed(b.zSpreadBp, 0) + ' bp', name: 'Z-spread' }, { value: signed(b.residualBp, 0) + ' bp', name: 'vs similar bonds' },
            { value: fx(b.price, 2), name: 'price' }, { value: b.signal, name: 'signal' }] };
      }) : []
    });
    if (ofz) {
      var scale = Math.max(40, 2 * ofz.thresholdBp);
      var sorted = ofz.bonds.slice().sort(function (a, b) { return b.residualBp - a.residualBp; });
      table($('moex-table'), [{ name: 'Bond', cls: 'nowrap' }, { name: 'Matures', cls: 'nowrap' }, { name: 'Coupon', num: true }, { name: 'Price', num: true },
        { name: 'Curve price', num: true }, { name: 'Yield', num: true }, { name: 'Duration', num: true }, { name: 'Z-spread', num: true },
        { name: 'Vs similar bonds', num: true }, { name: '\u2190 rich / cheap \u2192' }, { name: 'Worth (points)', num: true }, { name: 'Signal' }],
        sorted.map(function (b) {
          return ['OFZ ' + shortName(b), b.maturity, pct(b.coupon, 2), fx(b.price, 2), fx(b.modelPrice, 2), pct(b.yield, 2), fx(b.duration, 1),
            signed(b.zSpreadBp, 0) + ' bp', signed(b.residualBp, 0) + ' bp', divBar(b.residualBp, scale), signed(b.priceEdge, 2), signalPill(b.signal)];
        }));
      $('moex-note').textContent = 'Settlement ' + ofz.settle + '. Across these bonds a coupon one point higher goes with a spread ' + signed(ofz.spreadPerCouponPoint, 1) +
        ' bp wider, and one more year of duration with ' + signed(ofz.spreadPerDurationYear, 1) + ' bp; signals are measured after removing that pattern. ' +
        'A signal needs a gap beyond ' + fx(ofz.thresholdBp, 0) + ' bp that is worth at least ' + fx(ofz.minPriceEdge, 2) + ' price points.';
    } else unavailable($('moex-table'), m.ofz, 'OFZ bond prices');

    var fut = m.futures && m.futures.contracts ? m.futures : null;
    if (fut) {
      table($('moex-futures'), [{ name: 'Future' }, { name: 'Underlying' }, { name: 'Expiry' }, { name: 'Settlement price', num: true }, { name: 'Open interest', num: true }],
        fut.contracts.map(function (f) { return [f.name, f.asset === 'RUONIA' ? 'RUONIA index' : 'Key rate', f.expiry, fx(f.settle, f.asset === 'RUONIA' ? 4 : 2), money(f.openInterest)]; }));
      table($('moex-implied'), [{ name: 'Between RUONIA futures' }, { name: 'Period' }, { name: 'Implied overnight rate', num: true }],
        fut.ruoniaImplied.map(function (r) { return [r.label, r.start + ' to ' + r.end, pct(r.rate, 2)]; }));
    } else { unavailable($('moex-futures'), m.futures, 'Rate futures'); $('moex-implied').textContent = ''; }
  }

  /* ---- validation --------------------------------------------------------------- */
  function renderValidation() {
    table($('validation-table'), [{ name: 'Area' }, { name: 'Check' }, { name: 'Result', num: true }, { name: 'Limit', num: true }, { name: 'Status' }, { name: 'Detail', cls: 'why' }],
      (R.validation || []).map(function (v) {
        return [v.group, v.check, sci(v.value) + (v.unit ? ' ' + v.unit : ''), isNum(v.limit) ? '≤ ' + sci(v.limit) : '—', statusCell(v.ok), v.detail];
      }));
  }

  [renderOverview, renderUsd, renderForecast, renderPaper, renderEur, renderMoex, renderValidation].forEach(function (fn) {
    try { fn(); } catch (err) { if (window.console) console.error('Dashboard section failed:', fn.name, err); }
  });
})();
