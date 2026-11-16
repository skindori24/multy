/* Small dependency-free SVG chart helpers for the dashboard.

   Every chart follows the same contract:
     - thin marks, hairline grid, labels in text colours (colour identifies marks only);
     - a legend whenever there are two or more series, plus a few direct labels;
     - a hover layer (crosshair + one tooltip listing every series) that is also
       reachable from the keyboard (focus the chart, then use the arrow keys);
     - a "Table view" with every plotted number, so no value is hover-only.

   All text taken from the data is inserted with textContent, never as HTML. */
(function (global) {
  'use strict';

  var NS = 'http://www.w3.org/2000/svg';
  var uid = 0;

  function svg(name, attrs, parent) {
    var e = document.createElementNS(NS, name);
    for (var k in attrs) if (Object.prototype.hasOwnProperty.call(attrs, k)) e.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(e);
    return e;
  }

  function h(tag, cls, text, parent) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== null && text !== undefined) e.textContent = text;
    if (parent) parent.appendChild(e);
    return e;
  }

  function isNum(v) { return typeof v === 'number' && isFinite(v); }

  function niceStep(raw) {
    var p = Math.pow(10, Math.floor(Math.log(raw) / Math.LN10));
    var f = raw / p;
    return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10) * p;
  }

  function ticks(lo, hi, count) {
    if (!(hi > lo)) return [lo];
    var step = niceStep((hi - lo) / Math.max(1, count));
    var out = [];
    for (var v = Math.ceil(lo / step - 1e-9) * step; v <= hi + step * 1e-9; v += step) {
      out.push(Math.abs(v) < step * 1e-9 ? 0 : +v.toPrecision(12));
    }
    return out;
  }

  function extent(arrays, extra) {
    var lo = Infinity, hi = -Infinity;
    arrays.forEach(function (a) {
      (a || []).forEach(function (v) { if (isNum(v)) { if (v < lo) lo = v; if (v > hi) hi = v; } });
    });
    (extra || []).forEach(function (v) { if (isNum(v)) { if (v < lo) lo = v; if (v > hi) hi = v; } });
    if (lo === Infinity) { lo = 0; hi = 1; }
    if (lo === hi) { lo -= 1; hi += 1; }
    return [lo, hi];
  }

  function pad(domain, frac) {
    var d = (domain[1] - domain[0]) * frac;
    return [domain[0] - d, domain[1] + d];
  }

  function slotColor(slot) { return 'var(--series-' + (slot || 1) + ')'; }

  /* ---- shared scaffolding -------------------------------------------------- */

  function scaffold(container, cfg) {
    container.textContent = '';
    var fig = h('figure', 'chart', null, container);
    var cap = h('figcaption', null, null, fig);
    h('div', 'chart-title', cfg.title || '', cap);
    if (cfg.subtitle) h('div', 'chart-sub', cfg.subtitle, cap);
    var legend = h('div', 'legend', null, fig);
    var plot = h('div', 'plot', null, fig);
    var tip = h('div', 'tooltip', null, plot);
    tip.hidden = true;
    return { fig: fig, legend: legend, plot: plot, tip: tip };
  }

  function drawLegend(el, items) {
    el.textContent = '';
    items.forEach(function (it) {
      var item = h('span', 'item', null, el);
      var key = h('span', it.type === 'rect' ? 'key-rect' : it.type === 'dot' ? 'key-dot' : 'key-line', null, item);
      if (it.type === 'line') key.style.borderTopColor = it.color;
      else key.style.background = it.color;
      if (it.opacity) key.style.opacity = it.opacity;
      h('span', null, it.name, item);
    });
  }

  function showTip(f, x, y, head, rows) {
    var tip = f.tip;
    tip.textContent = '';
    if (head) h('div', 'tt-head', head, tip);
    rows.forEach(function (r) {
      var row = h('div', 'tt-row', null, tip);
      if (r.color) {
        var key = h('span', 'tt-key', null, row);
        key.style.borderTopColor = r.color;
      }
      h('span', 'tt-val', r.value, row);   // the value leads, the name follows
      h('span', 'tt-name', r.name, row);
    });
    tip.hidden = false;
    var pw = f.plot.clientWidth, tw = tip.offsetWidth, th = tip.offsetHeight;
    var left = x + 14;
    if (left + tw > pw) left = x - tw - 14;
    if (left < 0) left = 0;
    var top = y - th - 10;
    if (top < 0) top = y + 16;
    tip.style.left = left + 'px';
    tip.style.top = top + 'px';
  }

  function hideTip(f) { f.tip.hidden = true; }

  function tableView(fig, columns, rows) {
    var d = h('details', 'table-view', null, fig);
    h('summary', null, 'Table view', d);
    var sc = h('div', 'table-scroll', null, d);
    var t = h('table', null, null, sc);
    var tr = h('tr', null, null, h('thead', null, null, t));
    columns.forEach(function (c) { h('th', c.num ? 'num' : null, c.name, tr); });
    var tb = h('tbody', null, null, t);
    rows.forEach(function (r) {
      var row = h('tr', null, null, tb);
      r.forEach(function (v, i) { h('td', columns[i].num ? 'num' : null, v, row); });
    });
  }

  /* Re-draws when the container width changes (layout is computed in pixels). */
  function responsive(f, render) {
    var last = 0;
    function run() {
      var w = Math.round(f.plot.clientWidth);
      if (w > 0 && w !== last) { last = w; render(w); }
    }
    run();
    if (typeof ResizeObserver !== 'undefined') new ResizeObserver(run).observe(f.plot);
    else window.addEventListener('resize', run);
  }

  function newSvg(f, W, H, label) {
    var old = f.plot.querySelector('svg');
    if (old) old.remove();
    var s = svg('svg', { viewBox: '0 0 ' + W + ' ' + H, width: W, height: H, role: 'img', tabindex: '0', 'aria-label': label });
    f.plot.insertBefore(s, f.tip);
    return s;
  }

  function pointerX(e, el, W) {
    var r = el.getBoundingClientRect();
    return { x: (e.clientX - r.left) * (W / r.width), y: e.clientY - r.top, scale: r.width / W };
  }

  /* ---- line chart (with optional bands and reference lines) ------------------ */

  function lineChart(container, cfg) {
    var f = scaffold(container, cfg);
    var series = cfg.series || [], bands = cfg.bands || [], refs = cfg.refLines || [];
    var xs = cfg.x.values, n = xs.length;
    var xfmt = cfg.x.format || function (v) { return String(v); };
    var xlab = cfg.x.labels || xs.map(xfmt);
    var yfmt = cfg.yFormat || function (v) { return String(v); };

    var items = series.map(function (s) { return { type: 'line', color: slotColor(s.slot), name: s.name }; })
      .concat(bands.map(function (b) { return { type: 'rect', color: slotColor(b.slot), opacity: b.opacity || 0.16, name: b.name }; }));
    if (items.length >= 2) drawLegend(f.legend, items); else f.legend.remove();

    var all = [];
    series.forEach(function (s) { all.push(s.values); });
    bands.forEach(function (b) { all.push(b.lo); all.push(b.hi); });
    var dom = cfg.yDomain || pad(extent(all, refs.map(function (r) { return r.y; })), 0.06);
    var x0 = xs[0], x1 = xs[n - 1];
    if (x1 === x0) x1 = x0 + 1;

    responsive(f, function (W) {
      var H = cfg.height || 250;
      var direct = cfg.directLabels !== false && series.length >= 2 && series.length <= 4;
      var longest = 0;
      series.forEach(function (s) { longest = Math.max(longest, (s.short || s.name).length); });
      var m = { l: 48, r: direct ? Math.min(130, 16 + 6.4 * longest) : 16, t: 10, b: cfg.x.title ? 44 : 26 };
      var pw = W - m.l - m.r, ph = H - m.t - m.b;
      function sx(v) { return m.l + (v - x0) / (x1 - x0) * pw; }
      function sy(v) { return m.t + (1 - (v - dom[0]) / (dom[1] - dom[0])) * ph; }
      var s = newSvg(f, W, H, (cfg.title || 'Chart') + '. Use the arrow keys to read values.');
      var clip = 'clip' + (++uid);
      svg('rect', { x: m.l, y: m.t, width: pw, height: ph }, svg('clipPath', { id: clip }, svg('defs', {}, s)));

      // grid and axes: solid hairlines, one step off the surface
      ticks(dom[0], dom[1], 5).forEach(function (t) {
        svg('line', { x1: m.l, x2: m.l + pw, y1: sy(t), y2: sy(t), stroke: 'var(--grid)', 'stroke-width': 1 }, s);
        svg('text', { x: m.l - 8, y: sy(t) + 3.5, 'text-anchor': 'end' }, s).textContent = yfmt(t);
      });
      svg('line', { x1: m.l, x2: m.l + pw, y1: m.t + ph, y2: m.t + ph, stroke: 'var(--axis)', 'stroke-width': 1 }, s);
      var xt = cfg.x.ticks || ticks(x0, x1, pw > 520 ? 8 : pw > 320 ? 5 : 3);
      xt.forEach(function (t) {
        if (t < x0 || t > x1) return;
        svg('line', { x1: sx(t), x2: sx(t), y1: m.t + ph, y2: m.t + ph + 4, stroke: 'var(--axis)', 'stroke-width': 1 }, s);
        svg('text', { x: sx(t), y: m.t + ph + 16, 'text-anchor': 'middle' }, s).textContent = xfmt(t);
      });
      if (cfg.x.title) svg('text', { x: m.l + pw / 2, y: H - 6, 'text-anchor': 'middle', 'class': 'axis-title' }, s).textContent = cfg.x.title;

      var g = svg('g', { 'clip-path': 'url(#' + clip + ')' }, s);
      // bands: a light wash of the series hue
      bands.forEach(function (b) {
        var d = '', i, started = false;
        for (i = 0; i < n; i++) if (isNum(b.lo[i]) && isNum(b.hi[i])) { d += (started ? 'L' : 'M') + sx(xs[i]) + ',' + sy(b.hi[i]); started = true; }
        for (i = n - 1; i >= 0; i--) if (isNum(b.lo[i]) && isNum(b.hi[i])) d += 'L' + sx(xs[i]) + ',' + sy(b.lo[i]);
        if (started) svg('path', { d: d + 'Z', fill: slotColor(b.slot), 'fill-opacity': b.opacity || 0.16, stroke: 'none' }, g);
      });
      // reference lines with a direct label
      refs.forEach(function (r) {
        if (r.y < dom[0] || r.y > dom[1]) return;
        svg('line', { x1: m.l, x2: m.l + pw, y1: sy(r.y), y2: sy(r.y), stroke: 'var(--ink-2)', 'stroke-width': 1 }, g);
        svg('text', { x: m.l + 6, y: sy(r.y) - 5, 'class': 'direct' }, s).textContent = r.label;
      });
      // lines: 2px, round joins
      var ends = [];
      series.forEach(function (sr) {
        var d = '', pen = false, last = -1;
        for (var i = 0; i < n; i++) {
          var v = sr.values[i];
          if (!isNum(v)) { pen = false; continue; }
          d += (pen ? 'L' : 'M') + sx(xs[i]).toFixed(1) + ',' + sy(v).toFixed(1);
          pen = true; last = i;
        }
        if (!d) return;
        svg('path', { d: d, fill: 'none', stroke: slotColor(sr.slot), 'stroke-width': 2, 'stroke-linejoin': 'round', 'stroke-linecap': 'round' }, g);
        ends.push({ s: sr, x: sx(xs[last]), y: sy(sr.values[last]) });
      });
      // end markers with a surface ring, and sparing direct labels
      ends.forEach(function (e) {
        var cy = Math.max(m.t, Math.min(m.t + ph, e.y));
        svg('circle', { cx: e.x, cy: cy, r: 4, fill: slotColor(e.s.slot), stroke: 'var(--surface)', 'stroke-width': 2 }, s);
        e.cy = cy; e.ly = cy;
      });
      if (direct) {
        ends.sort(function (a, b) { return a.ly - b.ly; });
        for (var i = 1; i < ends.length; i++) if (ends[i].ly - ends[i - 1].ly < 14) ends[i].ly = ends[i - 1].ly + 14;
        var over = ends.length ? ends[ends.length - 1].ly - (m.t + ph) : 0;
        if (over > 0) ends.forEach(function (e) { e.ly -= over; });
        ends.forEach(function (e) {
          if (Math.abs(e.ly - e.cy) > 3) svg('line', { x1: e.x + 5, y1: e.cy, x2: e.x + 9, y2: e.ly, stroke: 'var(--axis)', 'stroke-width': 1 }, s);
          svg('text', { x: e.x + 11, y: e.ly + 4, 'class': 'direct' }, s).textContent = e.s.short || e.s.name;
        });
      }

      // hover / keyboard layer: the crosshair finds the X, one tooltip lists every series
      var cross = svg('line', { y1: m.t, y2: m.t + ph, stroke: 'var(--ink-2)', 'stroke-width': 1, visibility: 'hidden' }, s);
      var dots = series.map(function (sr) {
        return svg('circle', { r: 4, fill: slotColor(sr.slot), stroke: 'var(--surface)', 'stroke-width': 2, visibility: 'hidden' }, s);
      });
      var idx = n - 1;
      function show(i, py) {
        idx = Math.max(0, Math.min(n - 1, i));
        var px = sx(xs[idx]);
        cross.setAttribute('x1', px); cross.setAttribute('x2', px); cross.setAttribute('visibility', 'visible');
        var rows = [];
        series.forEach(function (sr, k) {
          var v = sr.values[idx];
          if (isNum(v)) {
            dots[k].setAttribute('cx', px);
            dots[k].setAttribute('cy', Math.max(m.t, Math.min(m.t + ph, sy(v))));
            dots[k].setAttribute('visibility', 'visible');
            rows.push({ color: slotColor(sr.slot), value: yfmt(v), name: sr.name });
          } else dots[k].setAttribute('visibility', 'hidden');
        });
        bands.forEach(function (b) {
          if (isNum(b.lo[idx]) && isNum(b.hi[idx])) rows.push({ value: yfmt(b.lo[idx]) + ' to ' + yfmt(b.hi[idx]), name: b.name });
        });
        var scale = s.getBoundingClientRect().width / W;
        showTip(f, px * scale, py === undefined ? (m.t + 24) * scale : py, xlab[idx], rows);
      }
      function hide() {
        cross.setAttribute('visibility', 'hidden');
        dots.forEach(function (d) { d.setAttribute('visibility', 'hidden'); });
        hideTip(f);
      }
      var overlay = svg('rect', { x: m.l, y: m.t, width: pw, height: ph, fill: 'transparent' }, s);
      overlay.addEventListener('pointermove', function (e) {
        var p = pointerX(e, s, W), best = 0, bd = Infinity;
        for (var i = 0; i < n; i++) { var d = Math.abs(sx(xs[i]) - p.x); if (d < bd) { bd = d; best = i; } }
        show(best, p.y);
      });
      overlay.addEventListener('pointerleave', hide);
      s.addEventListener('focus', function () { show(idx); });
      s.addEventListener('blur', hide);
      s.addEventListener('keydown', function (e) {
        if (e.key === 'ArrowLeft') show(idx - 1);
        else if (e.key === 'ArrowRight') show(idx + 1);
        else if (e.key === 'Home') show(0);
        else if (e.key === 'End') show(n - 1);
        else if (e.key === 'Escape') hide();
        else return;
        e.preventDefault();
      });
    });

    var cols = [{ name: cfg.x.title || cfg.x.name || 'x' }];
    series.forEach(function (sr) { cols.push({ name: sr.name, num: true }); });
    bands.forEach(function (b) { cols.push({ name: b.name + ' (low)', num: true }); cols.push({ name: b.name + ' (high)', num: true }); });
    var rows = [];
    for (var i = 0; i < n; i++) {
      var r = [xlab[i]];
      series.forEach(function (sr) { r.push(isNum(sr.values[i]) ? yfmt(sr.values[i]) : ''); });
      bands.forEach(function (b) { r.push(isNum(b.lo[i]) ? yfmt(b.lo[i]) : ''); r.push(isNum(b.hi[i]) ? yfmt(b.hi[i]) : ''); });
      rows.push(r);
    }
    tableView(f.fig, cols, rows);
  }

  /* ---- interval chart: one row per item, bands for the 5-95% and 16-84% ranges ---- */

  function intervalChart(container, cfg) {
    var f = scaffold(container, cfg);
    var rows = cfg.rows || [], fmt = cfg.format || function (v) { return String(v); };
    var names = cfg.names || {};
    var hasMarker = rows.some(function (r) { return isNum(r.marker); });
    var items = [
      { type: 'rect', color: slotColor(1), opacity: 0.22, name: names.outer || '5-95% range' },
      { type: 'rect', color: slotColor(1), opacity: 0.6, name: names.inner || '16-84% range' },
      { type: 'dot', color: slotColor(1), name: names.mid || 'mean' }
    ];
    if (hasMarker) items.push({ type: 'dot', color: slotColor(2), name: names.marker || 'marker' });
    drawLegend(f.legend, items);

    var dom = pad(extent([rows.map(function (r) { return r.lo2; }), rows.map(function (r) { return r.hi2; }),
      rows.map(function (r) { return r.marker; })], [0]), 0.05);
    var rowH = 34;

    responsive(f, function (W) {
      var longest = 0;
      rows.forEach(function (r) { longest = Math.max(longest, r.label.length); });
      var m = { l: Math.min(150, 18 + 7 * longest), r: 16, t: 8, b: cfg.xTitle ? 44 : 26 };
      var H = m.t + rows.length * rowH + m.b, pw = W - m.l - m.r;
      function sx(v) { return m.l + (v - dom[0]) / (dom[1] - dom[0]) * pw; }
      var s = newSvg(f, W, H, (cfg.title || 'Chart') + '. Use the up and down arrow keys to read values.');
      ticks(dom[0], dom[1], pw > 520 ? 8 : 5).forEach(function (t) {
        svg('line', { x1: sx(t), x2: sx(t), y1: m.t, y2: H - m.b, stroke: t === 0 ? 'var(--axis)' : 'var(--grid)', 'stroke-width': 1 }, s);
        svg('text', { x: sx(t), y: H - m.b + 16, 'text-anchor': 'middle' }, s).textContent = fmt(t);
      });
      if (cfg.xTitle) svg('text', { x: m.l + pw / 2, y: H - 6, 'text-anchor': 'middle', 'class': 'axis-title' }, s).textContent = cfg.xTitle;

      var hi = svg('rect', { x: 0, width: W, height: rowH, fill: 'var(--wash)', visibility: 'hidden' }, s);
      rows.forEach(function (r, i) {
        var cy = m.t + i * rowH + rowH / 2;
        svg('text', { x: m.l - 10, y: cy + 4, 'text-anchor': 'end', 'class': 'direct' }, s).textContent = r.label;
        svg('rect', { x: sx(r.lo2), y: cy - 4, width: Math.max(1, sx(r.hi2) - sx(r.lo2)), height: 8, rx: 4, fill: slotColor(1), 'fill-opacity': 0.22 }, s);
        svg('rect', { x: sx(r.lo1), y: cy - 4, width: Math.max(1, sx(r.hi1) - sx(r.lo1)), height: 8, rx: 4, fill: slotColor(1), 'fill-opacity': 0.6 }, s);
        if (isNum(r.marker)) svg('circle', { cx: sx(r.marker), cy: cy, r: 4, fill: slotColor(2), stroke: 'var(--surface)', 'stroke-width': 2 }, s);
        svg('circle', { cx: sx(r.mid), cy: cy, r: 4.5, fill: slotColor(1), stroke: 'var(--surface)', 'stroke-width': 2 }, s);
      });
      var cur = 0;
      function show(i, px, py) {
        cur = Math.max(0, Math.min(rows.length - 1, i));
        var r = rows[cur];
        hi.setAttribute('y', m.t + cur * rowH); hi.setAttribute('visibility', 'visible');
        var list = [
          { color: slotColor(1), value: fmt(r.mid), name: names.mid || 'mean' },
          { value: fmt(r.lo1) + ' to ' + fmt(r.hi1), name: names.inner || '16-84% range' },
          { value: fmt(r.lo2) + ' to ' + fmt(r.hi2), name: names.outer || '5-95% range' }
        ];
        if (isNum(r.marker)) list.push({ color: slotColor(2), value: fmt(r.marker), name: names.marker || 'marker' });
        (r.extra || []).forEach(function (x) { list.push(x); });
        var scale = s.getBoundingClientRect().width / W;
        showTip(f, px === undefined ? sx(r.mid) * scale : px, py === undefined ? (m.t + cur * rowH) * scale : py, r.label, list);
      }
      function hide() { hi.setAttribute('visibility', 'hidden'); hideTip(f); }
      var overlay = svg('rect', { x: 0, y: m.t, width: W, height: rows.length * rowH, fill: 'transparent' }, s);
      overlay.addEventListener('pointermove', function (e) {
        var p = pointerX(e, s, W);
        show(Math.floor((p.y / p.scale - m.t) / rowH), p.x * p.scale, p.y);
      });
      overlay.addEventListener('pointerleave', hide);
      s.addEventListener('focus', function () { show(cur); });
      s.addEventListener('blur', hide);
      s.addEventListener('keydown', function (e) {
        if (e.key === 'ArrowUp') show(cur - 1);
        else if (e.key === 'ArrowDown') show(cur + 1);
        else if (e.key === 'Escape') hide();
        else return;
        e.preventDefault();
      });
    });

    var cols = [{ name: cfg.labelTitle || 'Item' }, { name: names.mid || 'mean', num: true },
      { name: '16%', num: true }, { name: '84%', num: true }, { name: '5%', num: true }, { name: '95%', num: true }];
    if (hasMarker) cols.push({ name: names.marker || 'marker', num: true });
    tableView(f.fig, cols, rows.map(function (r) {
      var out = [r.label, fmt(r.mid), fmt(r.lo1), fmt(r.hi1), fmt(r.lo2), fmt(r.hi2)];
      if (hasMarker) out.push(isNum(r.marker) ? fmt(r.marker) : '');
      return out;
    }));
  }

  /* ---- horizontal bars: one hue, thin, value at the tip ------------------------- */

  function barChart(container, cfg) {
    var f = scaffold(container, cfg);
    f.legend.remove();   // a single series: the title says what is plotted
    var rows = cfg.rows || [], fmt = cfg.format || function (v) { return String(v); };
    var dom = extent([rows.map(function (r) { return r.value; })], [0]);
    var rowH = 26, thick = 12;

    responsive(f, function (W) {
      var longest = 0;
      rows.forEach(function (r) { longest = Math.max(longest, Math.min(28, r.label.length)); });
      var m = { l: Math.min(210, 16 + 6.6 * longest), r: 62, t: 6, b: 8 };
      var neg = dom[0] < 0;
      var pw = W - m.l - m.r - (neg ? 56 : 0), left = m.l + (neg ? 56 : 0);
      var H = m.t + rows.length * rowH + m.b;
      function sx(v) { return left + (v - dom[0]) / (dom[1] - dom[0]) * pw; }
      var s = newSvg(f, W, H, (cfg.title || 'Chart') + '. Use the up and down arrow keys to read values.');
      var hi = svg('rect', { x: 0, width: W, height: rowH, fill: 'var(--wash)', visibility: 'hidden' }, s);
      svg('line', { x1: sx(0), x2: sx(0), y1: m.t, y2: H - m.b, stroke: 'var(--axis)', 'stroke-width': 1 }, s);
      rows.forEach(function (r, i) {
        var cy = m.t + i * rowH + rowH / 2;
        var label = r.label.length > 28 ? r.label.slice(0, 27) + '…' : r.label;
        svg('text', { x: m.l - 10, y: cy + 4, 'text-anchor': 'end', 'class': 'direct' }, s).textContent = label;
        var a = sx(0), b = sx(r.value), w = Math.abs(b - a), y0 = cy - thick / 2, rad = Math.min(4, w);
        if (w >= 0.5) {
          // rounded at the data end, square at the baseline
          var d = r.value >= 0
            ? 'M' + a + ',' + y0 + 'H' + (b - rad) + 'Q' + b + ',' + y0 + ' ' + b + ',' + (y0 + rad) + 'V' + (y0 + thick - rad) + 'Q' + b + ',' + (y0 + thick) + ' ' + (b - rad) + ',' + (y0 + thick) + 'H' + a + 'Z'
            : 'M' + a + ',' + y0 + 'H' + (b + rad) + 'Q' + b + ',' + y0 + ' ' + b + ',' + (y0 + rad) + 'V' + (y0 + thick - rad) + 'Q' + b + ',' + (y0 + thick) + ' ' + (b + rad) + ',' + (y0 + thick) + 'H' + a + 'Z';
          svg('path', { d: d, fill: slotColor(cfg.slot || 1) }, s);
        }
        svg('text', { x: r.value >= 0 ? b + 6 : b - 6, y: cy + 4, 'text-anchor': r.value >= 0 ? 'start' : 'end', 'class': 'direct' }, s).textContent = fmt(r.value);
      });
      var cur = 0;
      function show(i, px, py) {
        cur = Math.max(0, Math.min(rows.length - 1, i));
        var r = rows[cur];
        hi.setAttribute('y', m.t + cur * rowH); hi.setAttribute('visibility', 'visible');
        var list = [{ color: slotColor(cfg.slot || 1), value: fmt(r.value), name: cfg.valueName || 'value' }];
        (r.extra || []).forEach(function (x) { list.push(x); });
        var scale = s.getBoundingClientRect().width / W;
        showTip(f, px === undefined ? sx(r.value) * scale : px, py === undefined ? (m.t + cur * rowH) * scale : py, r.label, list);
      }
      function hide() { hi.setAttribute('visibility', 'hidden'); hideTip(f); }
      var overlay = svg('rect', { x: 0, y: m.t, width: W, height: rows.length * rowH, fill: 'transparent' }, s);
      overlay.addEventListener('pointermove', function (e) {
        var p = pointerX(e, s, W);
        show(Math.floor((p.y / p.scale - m.t) / rowH), p.x * p.scale, p.y);
      });
      overlay.addEventListener('pointerleave', hide);
      s.addEventListener('focus', function () { show(cur); });
      s.addEventListener('blur', hide);
      s.addEventListener('keydown', function (e) {
        if (e.key === 'ArrowUp') show(cur - 1);
        else if (e.key === 'ArrowDown') show(cur + 1);
        else if (e.key === 'Escape') hide();
        else return;
        e.preventDefault();
      });
    });

    var cols = [{ name: cfg.labelTitle || 'Item' }, { name: cfg.valueName || 'Value', num: true }];
    var extraNames = rows.length && rows[0].extra ? rows[0].extra.map(function (x) { return x.name; }) : [];
    extraNames.forEach(function (nm) { cols.push({ name: nm, num: true }); });
    tableView(f.fig, cols, rows.map(function (r) {
      var out = [r.label, fmt(r.value)];
      (r.extra || []).forEach(function (x) { out.push(x.value); });
      return out;
    }));
  }

  /* ---- a curve with observed points on top (e.g. a yield curve and its bonds) ----- */

  function scatterCurve(container, cfg) {
    var f = scaffold(container, cfg);
    var c = cfg.curve, pts = (cfg.points || []).slice().sort(function (a, b) { return a.x - b.x; });
    var xfmt = cfg.xFormat || function (v) { return String(v); };
    var yfmt = cfg.yFormat || function (v) { return String(v); };
    drawLegend(f.legend, [{ type: 'line', color: slotColor(1), name: c.name }, { type: 'dot', color: slotColor(2), name: cfg.pointsName || 'points' }]);
    // optionally show only the part of the curve where the points are
    var curveX = [], curveY = [];
    c.x.forEach(function (x, i) { if (!isNum(cfg.xMax) || x <= cfg.xMax) { curveX.push(x); curveY.push(c.y[i]); } });
    var xd = extent([curveX, pts.map(function (p) { return p.x; })], [0]);
    var yd = pad(extent([curveY, pts.map(function (p) { return p.y; })]), 0.1);

    responsive(f, function (W) {
      var H = cfg.height || 300, m = { l: 48, r: 18, t: 12, b: cfg.xTitle ? 44 : 26 };
      var pw = W - m.l - m.r, ph = H - m.t - m.b;
      function sx(v) { return m.l + (v - xd[0]) / (xd[1] - xd[0]) * pw; }
      function sy(v) { return m.t + (1 - (v - yd[0]) / (yd[1] - yd[0])) * ph; }
      var s = newSvg(f, W, H, (cfg.title || 'Chart') + '. Use the arrow keys to step through the points.');
      ticks(yd[0], yd[1], 5).forEach(function (t) {
        svg('line', { x1: m.l, x2: m.l + pw, y1: sy(t), y2: sy(t), stroke: 'var(--grid)', 'stroke-width': 1 }, s);
        svg('text', { x: m.l - 8, y: sy(t) + 3.5, 'text-anchor': 'end' }, s).textContent = yfmt(t);
      });
      svg('line', { x1: m.l, x2: m.l + pw, y1: m.t + ph, y2: m.t + ph, stroke: 'var(--axis)', 'stroke-width': 1 }, s);
      ticks(xd[0], xd[1], pw > 520 ? 8 : 5).forEach(function (t) {
        svg('text', { x: sx(t), y: m.t + ph + 16, 'text-anchor': 'middle' }, s).textContent = xfmt(t);
      });
      if (cfg.xTitle) svg('text', { x: m.l + pw / 2, y: H - 6, 'text-anchor': 'middle', 'class': 'axis-title' }, s).textContent = cfg.xTitle;
      var d = '';
      curveX.forEach(function (x, i) { d += (i ? 'L' : 'M') + sx(x).toFixed(1) + ',' + sy(curveY[i]).toFixed(1); });
      svg('path', { d: d, fill: 'none', stroke: slotColor(1), 'stroke-width': 2, 'stroke-linejoin': 'round', 'stroke-linecap': 'round' }, s);
      var placed = [];
      pts.forEach(function (p) {
        var cx = sx(p.x), cy = sy(p.y);
        svg('circle', { cx: cx, cy: cy, r: p.emphasis ? 5 : 4, fill: slotColor(2), stroke: 'var(--surface)', 'stroke-width': 2 }, s);
        if (p.emphasis && p.tag) {
          // label only the points the story is about; keep labels off each other
          // a point above the curve is labelled above it, one below the curve below it
          var w = 6.2 * p.tag.length, up = cy - 11, down = cy + 19, ly = p.tagBelow ? down : up;
          var lx = Math.max(m.l + w / 2, Math.min(m.l + pw - w / 2, cx));
          var clash = placed.some(function (q) { return Math.abs(q.x - lx) < (q.w + w) / 2 + 4 && Math.abs(q.y - ly) < 13; });
          if (clash) ly = p.tagBelow ? up : down;
          placed.push({ x: lx, y: ly, w: w });
          svg('text', { x: lx, y: ly, 'text-anchor': 'middle', 'class': 'direct' }, s).textContent = p.tag;
        }
      });
      var ring = svg('circle', { r: 8, fill: 'none', stroke: 'var(--ink)', 'stroke-width': 1.5, visibility: 'hidden' }, s);
      var cur = 0;
      function show(i) {
        if (!pts.length) return;
        cur = Math.max(0, Math.min(pts.length - 1, i));
        var p = pts[cur], scale = s.getBoundingClientRect().width / W;
        ring.setAttribute('cx', sx(p.x)); ring.setAttribute('cy', sy(p.y)); ring.setAttribute('visibility', 'visible');
        var list = [{ color: slotColor(2), value: yfmt(p.y), name: cfg.yName || 'y' }, { value: xfmt(p.x), name: cfg.xName || 'x' }];
        (p.rows || []).forEach(function (x) { list.push(x); });
        showTip(f, sx(p.x) * scale, sy(p.y) * scale, p.label, list);
      }
      function hide() { ring.setAttribute('visibility', 'hidden'); hideTip(f); }
      // nearest-point hover: the pointer only has to be closest, not dead-centre
      var overlay = svg('rect', { x: m.l, y: m.t, width: pw, height: ph, fill: 'transparent' }, s);
      overlay.addEventListener('pointermove', function (e) {
        var p = pointerX(e, s, W), best = -1, bd = Infinity;
        pts.forEach(function (q, i) {
          var dx = sx(q.x) - p.x, dy = sy(q.y) - p.y / p.scale, dist = dx * dx + dy * dy;
          if (dist < bd) { bd = dist; best = i; }
        });
        if (best >= 0) show(best);
      });
      overlay.addEventListener('pointerleave', hide);
      s.addEventListener('focus', function () { show(cur); });
      s.addEventListener('blur', hide);
      s.addEventListener('keydown', function (e) {
        if (e.key === 'ArrowLeft') show(cur - 1);
        else if (e.key === 'ArrowRight') show(cur + 1);
        else if (e.key === 'Escape') hide();
        else return;
        e.preventDefault();
      });
    });

    var cols = [{ name: cfg.labelTitle || 'Point' }, { name: cfg.xName || 'x', num: true }, { name: cfg.yName || 'y', num: true }];
    var extraNames = pts.length && pts[0].rows ? pts[0].rows.map(function (x) { return x.name; }) : [];
    extraNames.forEach(function (nm) { cols.push({ name: nm, num: true }); });
    tableView(f.fig, cols, pts.map(function (p) {
      var out = [p.label, xfmt(p.x), yfmt(p.y)];
      (p.rows || []).forEach(function (x) { out.push(x.value); });
      return out;
    }));
  }

  global.MCBCharts = { lineChart: lineChart, intervalChart: intervalChart, barChart: barChart, scatterCurve: scatterCurve, h: h };
})(window);
