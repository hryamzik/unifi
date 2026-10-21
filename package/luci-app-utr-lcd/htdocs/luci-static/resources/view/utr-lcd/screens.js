'use strict';
'require fs';
'require ui';
'require uci';
'require view';

/*
 * Screens editor for the built-in LCD renderer (utr-lcd): add, order and edit screens, pick their
 * widgets and sizes, and see each screen rendered by the router's own renderer (luci-preview) while
 * editing, before anything is saved.
 */

var PREVIEW = '/usr/libexec/utr-lcd/luci-preview';
var STATUS = '/usr/libexec/utr-lcd/luci-status';
var SCALE = 1;   /* 1:1 - the real panel is tiny, a bigger picture would mislead */

/* ---- model (pure functions, kept free of the DOM so they can be tested) ---- */

var MAX_WIDGETS = 16;
var GAP = 2;
var ROW = 17;

/* widget types: label, allowed sizes, default size, unit of the size, group */
var TYPES = {
	ssid:        { label: 'Network name',          group: 'general', sizes: [16, 24, 32], def: 24 },
	dbm:         { label: 'Signal strength',       group: 'general', sizes: [16, 24, 32], def: 32 },
	bars:        { label: 'Signal (bars)',         group: 'general', sizes: [16, 24, 32], def: 24 },
	sigbar:      { label: 'Signal (line bar)',     group: 'general', sizes: [12, 16, 24], def: 16 },
	status:      { label: 'Online status',         group: 'general', sizes: [16, 24, 32], def: 16 },
	fan:         { label: 'Signal (Wi-Fi fan)',    group: 'general', sizes: [24, 32, 48], def: 32 },
	segbar:      { label: 'Signal (segment bar)',  group: 'general', sizes: [12, 16, 24], def: 16 },
	clients:     { label: 'Client count',          group: 'general', sizes: [16, 24, 32], def: 24 },
	wanip:       { label: 'WAN IP address',        group: 'general', sizes: [12, 16], def: 16 },
	lanip:       { label: 'LAN IP address',        group: 'general', sizes: [12, 16], def: 16 },
	vpn:         { label: 'VPN',                   group: 'general', sizes: [12, 16], def: 16 },
	uptime:      { label: 'Uptime',                group: 'general', sizes: [12, 16], def: 16 },
	rule:        { label: 'Divider line',          group: 'general', sizes: null, def: 2 },
	speed:       { label: 'Speed down / up',       group: 'traffic', sizes: [16, 24], def: 24 },
	speedbar:    { label: 'Speed by client (bar)', group: 'traffic', sizes: [8, 12, 16], def: 12 },
	usage:       { label: 'Total traffic',         group: 'traffic', sizes: [12, 16], def: 16 },
	usagebar:    { label: 'Total by client (bar)', group: 'traffic', sizes: [8, 12, 16], def: 12 },
	legend:      { label: 'Clients, speed',        group: 'traffic', sizes: [1, 2, 3, 4, 5, 6, 7, 8], def: 4, unit: 'rows' },
	legendtotal: { label: 'Clients, total',        group: 'traffic', sizes: [1, 2, 3, 4, 5, 6, 7, 8], def: 4, unit: 'rows' },
	donut:       { label: 'Speed by client (ring)', group: 'traffic', sizes: [48, 64, 80, 96], def: 80, unit: 'px' },
	multiwan:    { label: 'Multi-WAN shares',      group: 'traffic', sizes: [12, 16], def: 16 },
	cpu:         { label: 'CPU load',              group: 'resources', sizes: [12, 16], def: 16 },
	ram:         { label: 'Memory',                group: 'resources', sizes: [12, 16], def: 16 },
	flash:       { label: 'Flash storage',         group: 'resources', sizes: [12, 16], def: 16 },
	temp:        { label: 'Wi-Fi chip temperature', group: 'resources', sizes: [12, 16], def: 16 }
};

var DEFAULT_SCREENS = [
	{ name: 'Status', dwell: '8', spacing: 'auto', enabled: true, backfill: false,
	  widgets: [ ['status', 16], ['segbar', 16], ['dbm', 32], ['ssid', 24, false, true, true], ['clients', 24] ] },
	{ name: 'Traffic', dwell: '8', spacing: 'auto', enabled: true, backfill: true,
	  widgets: [ ['speed', 24], ['speedbar', 12], ['usage', 16], ['usagebar', 12], ['legend', 4] ] },
	{ name: 'Details', dwell: '5', spacing: 'auto', enabled: true, backfill: true,
	  widgets: [ ['wanip', 16], ['lanip', 16], ['vpn', 16], ['uptime', 16], ['cpu', 16], ['ram', 16], ['flash', 16], ['temp', 16] ] }
];

/* Height in pixels a widget needs (the same table as widget_h in lcd-status.c; widgets with nothing to
 * show right now take no room, so this is the worst case). */
function widgetHeight(type, size, wrap) {
	size = +size;
	if (type === 'ssid' && wrap) return 2 * size + 2;   /* wrapped name may take a second line */
	switch (type) {
	case 'rule': return 6;
	case 'speed': return 2 * (size + 2);
	case 'legend':
	case 'legendtotal': return size * ROW;
	case 'multiwan': return 2 * ((size >= 16 ? 16 : 12) + 2 + (size >= 16 ? 8 : 6) + 4);
	default: return size + 2;
	}
}

/* Room for widgets (content now starts at the top margin; the status is a widget). */
function availableHeight() {
	return 233 - 6;
}

/* Heights of the lines: widgets joined with the next one share a line (its height is the max). */
function lineHeights(widgets) {
	var lines = [], cur = 0;
	widgets.forEach(function(w, i) {
		cur = Math.max(cur, widgetHeight(w[0], w[1], w[3]));
		if (!w[2] || i === widgets.length - 1) { lines.push(cur); cur = 0; }
	});
	return lines;
}

function usedHeight(widgets, spacing) {
	var lines = lineHeights(widgets), sum = 0;
	lines.forEach(function(h) { sum += h; });
	var gap = spacing && spacing !== 'auto' ? +spacing : GAP;
	return sum + (lines.length > 1 ? gap * (lines.length - 1) : 0);
}

/* How many widgets are in the joined line containing index wi (at most 3 fit, see MAX_SUB). */
function groupSize(widgets, wi) {
	var n = 1, i;
	for (i = wi; i > 0 && widgets[i - 1][2]; i--) n++;
	for (i = wi; i < widgets.length - 1 && widgets[i][2]; i++) n++;
	return n;
}

function parseWidget(str) {
	var p = String(str).split(':'), t = TYPES[p[0]];
	if (!t) return null;
	var size = parseInt(p[1], 10);
	if (!size || (t.sizes && t.sizes.indexOf(size) < 0)) size = t.def;
	return [p[0], size, false, /w/.test(p[1] || ''), /s/.test(p[1] || '')];   /* 4th: wrap to two lines (ssid:24w); 5th: shrink to fit (ssid:24ws) */
}

function widgetSpec(w) {
	return w[0] + ':' + w[1] + (w[3] ? 'w' : '') + (w[4] ? 's' : '');
}

/* One config item: "type:size" or several joined by '+' for one shared line. */
function parseWidgetList(str) {
	var parts = String(str).split('+').map(parseWidget).filter(function(w) { return w; });
	parts.forEach(function(w, i) { w[2] = i < parts.length - 1; });
	return parts;
}

/* The layout in the flat key=value format the renderer reads (same as utr-lcd.init writes). */
function flatten(main, screens) {
	var out = [
		'mode=' + main.screen_mode, 'pin=' + main.screen_pin, 'playlist=' + main.playlist,
		'fill_dim=' + main.fill_dim, 'fill_edge=' + (main.fill_edge ? 1 : 0), 'name_len=' + main.name_len
	];
	screens.forEach(function(s) {
		out.push('screen=' + String(s.name).replace(/[\r\n]/g, ' '), 'dwell=' + s.dwell, 'spacing=' + (s.spacing || 'auto'), 'enabled=' + (s.enabled ? 1 : 0),
			'indicator=none', 'backfill=' + (s.backfill ? 1 : 0));
		var line = [];
		s.widgets.forEach(function(w, i) {
			line.push(widgetSpec(w));
			if (!w[2] || i === s.widgets.length - 1) { out.push('widget=' + line.join('+')); line = []; }
		});
	});
	return out.join('\n');
}

/* "count:rrggbb" runs (see write_rle in lcd-status.c) into [w, h, Uint8 RGBA]. */
function decodeRle(text) {
	var lines = text.split('\n'), wh = lines[0].trim().split(' '), w = +wh[0], h = +wh[1];
	var data = new Uint8ClampedArray(w * h * 4), pos = 0;
	(lines[1] || '').trim().split(' ').forEach(function(run) {
		var p = run.split(':'), n = +p[0], c = parseInt(p[1], 16);
		for (var i = 0; i < n && pos < w * h; i++, pos++) {
			data[pos * 4] = (c >> 16) & 255;
			data[pos * 4 + 1] = (c >> 8) & 255;
			data[pos * 4 + 2] = c & 255;
			data[pos * 4 + 3] = c === 0xff00ff ? 0 : 255;   /* ff00ff = hidden by the bezel: transparent */
		}
	});
	return { w: w, h: h, data: data, filled: pos };
}

/* what-if scenarios: overrides of what the collector reports */
var SCENARIOS = {
	none:       [],
	weak:       [ 'up_signal=-82', 'up0_signal=-82' ],
	longssid:   [ 'up_name=Hotel-Guest-WiFi-Conference-Room-5G', 'up0_name=Hotel-Guest-WiFi-Conference-Room-5G' ],
	ethernet:   [ 'up_type=eth', 'up_name=Ethernet', 'up_speed=1000', 'up_signal=', 'up0_type=eth', 'up0_name=Ethernet', 'up0_speed=1000', 'up0_signal=' ],
	nouplink:   [ 'up_state=down', 'ups=0' ],
	nointernet: [ 'inet=fail' ],
	vpnup:      [ 'vpn_state=up', 'vpn_name=wg0' ],
	vpndown:    [ 'vpn_state=down', 'vpn_name=wg0' ],
	novpn:      [ 'vpn_state=none' ]
};

/* made-up state for the "example data" source */
var EXAMPLE = [
	'up_state=up', 'up_type=wifi', 'up_name=iNomad', 'up_signal=-52', 'up_speed=144', 'wan_ip=192.168.87.221', 'lan_ip=192.168.1.1',
	'inet=ok', 'vpn_state=up', 'vpn_name=wg0', 'clients=3', 'uptime=9163',
	'up0_dev=phy0-sta0', 'up0_type=wifi', 'up0_name=iNomad', 'up0_signal=-52', 'up0_speed=144', 'up0_metric=10',
	'up1_dev=eth0', 'up1_type=eth', 'up1_name=Ethernet', 'up1_speed=1000', 'up1_metric=20', 'ups=2'
];

/* ---- end of the model ---- */

function bool(v, def) {
	return v == null || v === '' ? def : (v === '1' || v === 'true' || v === 'yes' || v === 'on' || v === true);
}

function callStatus() {
	return fs.exec_direct(STATUS, [], 'json').catch(function() { return null; });
}

function drawRle(canvas, text) {
	var img = decodeRle(text);
	if (!img.w || !img.h || img.filled < img.w * img.h) throw new Error('bad preview data');
	canvas.width = img.w;
	canvas.height = img.h;
	canvas.style.width = (img.w * SCALE) + 'px';
	canvas.style.height = (img.h * SCALE) + 'px';
	var ctx = canvas.getContext('2d');
	ctx.putImageData(new ImageData(img.data, img.w, img.h), 0, 0);
}

return view.extend({
	load: function() {
		return Promise.all([ uci.load('utr-lcd'), callStatus() ]);
	},

	render: function(data) {
		var self = this, status = data[1] || {};
		var stockActive = status.mode === 'stock';   /* what is running right now */

		/* ---- state from UCI ---- */
		var main = {
			screen_mode: uci.get('utr-lcd', 'main', 'screen_mode') === 'pin' ? 'pin' : 'rotate',
			screen_pin: uci.get('utr-lcd', 'main', 'screen_pin') || '1',
			playlist: uci.get('utr-lcd', 'main', 'playlist') || '',
			fill_dim: uci.get('utr-lcd', 'main', 'fill_dim') || '30',
			fill_edge: bool(uci.get('utr-lcd', 'main', 'fill_edge'), true),
			name_len: uci.get('utr-lcd', 'main', 'name_len') || '8'
		};
		var screens = uci.sections('utr-lcd', 'screen').map(function(s) {
			var list = s.widget || [];
			if (!Array.isArray(list)) list = [ list ];
			return {
				name: s.name || _('Screen'), dwell: s.dwell || '8', enabled: bool(s.enabled, true),
				spacing: (s.spacing === 'auto' || /^[0-9]+$/.test(s.spacing || '')) ? (s.spacing || 'auto') : 'auto',
				backfill: bool(s.backfill, true),
				widgets: list.reduce(function(acc, item) { return acc.concat(parseWidgetList(item)); }, [])
			};
		});
		if (!screens.length)
			screens = DEFAULT_SCREENS.map(function(s) {
				return { name: s.name, dwell: s.dwell, spacing: s.spacing, enabled: s.enabled, backfill: s.backfill,
					widgets: s.widgets.map(function(w) { return w.slice(); }) };
			});

		var preview = { source: 'live', scenario: 'none' };
		var canvases = [], notes = [];
		var container = E('div', {});
		var globals = E('div', {});
		var timer = null, seq = 0;

		/* ---- small DOM helpers ---- */
		function select(options, value, onchange, extra) {
			var sel = E('select', Object.assign({ 'class': 'cbi-input-select', 'change': function() { onchange(this.value); } }, extra || {}),
				options.map(function(o) { return E('option', { 'value': o[0], 'selected': String(o[0]) === String(value) ? '' : null }, o[1]); }));
			return sel;
		}
		function text(value, onchange, width, type) {
			return E('input', { 'type': type || 'text', 'class': 'cbi-input-text', 'value': value, 'style': 'width:' + width + 'px',
				'change': function() { onchange(this.value); } });
		}
		function check(value, onchange, label) {
			return E('label', { 'style': 'margin-right:1em; white-space:nowrap' }, [
				E('input', { 'type': 'checkbox', 'checked': value ? '' : null, 'change': function() { onchange(this.checked); } }), ' ', label ]);
		}
		function btn(label, onclick, disabled, title) {
			return E('button', { 'class': 'cbi-button', 'type': 'button', 'click': onclick, 'disabled': disabled ? '' : null, 'title': title || null }, label);
		}
		function sizeLabel(t, s) {
			return t.unit === 'rows' ? s + ' ' + (s === 1 ? _('row') : _('rows')) : t.unit === 'px' ? s + ' px' : s + ' px';
		}

		/* ---- preview ---- */
		function overrides() {
			var o = preview.source === 'example' ? EXAMPLE.slice() : [];
			return o.concat(SCENARIOS[preview.scenario] || []);
		}

		function runPreview() {
			/* lines joined by \x1f, not \n: exec_direct escapes whitespace in arguments (see luci-preview) */
			var my = ++seq, conf = flatten(main, screens).replace(/\n/g, '\x1f'), ov = overrides(), demo = preview.source === 'example' ? '1' : '0';
			var chain = Promise.resolve();
			screens.forEach(function(s, i) {
				chain = chain.then(function() {
					if (my !== seq || !canvases[i]) return;
					return fs.exec_direct(PREVIEW, [ String(i + 1), demo, conf ].concat(ov), 'text').then(function(out) {
						if (my !== seq || !canvases[i]) return;
						drawRle(canvases[i], out);
						notes[i].textContent = '';
					}).catch(function(err) {
						if (notes[i]) notes[i].textContent = _('Preview not available: ') + (err && err.message ? err.message : err);
					});
				});
			});
		}

		function schedulePreview() {
			clearTimeout(timer);
			timer = setTimeout(runPreview, 400);
		}

		/* ---- screens ---- */
		function move(arr, i, d) {
			var j = i + d;
			if (j < 0 || j >= arr.length) return;
			var t = arr[i]; arr[i] = arr[j]; arr[j] = t;
		}

		function widgetRow(sc, wi) {
			var w = sc.widgets[wi], t = TYPES[w[0]];
			var joinFull = !w[2] && groupSize(sc.widgets, wi) >= 3;
			return E('tr', {}, [
				E('td', {}, select(Object.keys(TYPES).map(function(k) { return [ k, _(TYPES[k].label) ]; }), w[0], function(v) {
					w[0] = v; w[1] = TYPES[v].def; redraw();
				}, { 'style': 'width:100%; max-width:13em' })),
				E('td', { 'style': 'text-align:center' }, E('input', { 'type': 'checkbox', 'title': _('Share one line with the next widget (up to three)'),
					'checked': w[2] ? '' : null, 'disabled': (wi === sc.widgets.length - 1 || joinFull) ? '' : null,
					'change': function() { w[2] = this.checked; redraw(); } })),
				E('td', { 'style': 'text-align:center' }, w[0] === 'ssid' ? E('input', { 'type': 'checkbox', 'title': _('Wrap a long name to a second line instead of cutting it'),
					'checked': w[3] ? '' : null,
					'change': function() { w[3] = this.checked; redraw(); } }) : E('span', { 'style': 'opacity:.6' }, '-')),
				E('td', { 'style': 'text-align:center' }, w[0] === 'ssid' ? E('input', { 'type': 'checkbox', 'title': _('If the name still does not fit (on two lines when wrapping), reduce the font size step by step until it does'),
					'checked': w[4] ? '' : null,
					'change': function() { w[4] = this.checked; redraw(); } }) : E('span', { 'style': 'opacity:.6' }, '-')),
				E('td', {}, t.sizes ? select(t.sizes.map(function(s) { return [ s, sizeLabel(t, s) ]; }), w[1], function(v) {
					w[1] = +v; redraw();
				}) : E('span', { 'style': 'opacity:.6' }, '-')),
				E('td', { 'style': 'text-align:right; opacity:.7; white-space:nowrap' }, widgetHeight(w[0], w[1], w[3]) + ' px'),
				E('td', { 'style': 'white-space:nowrap' }, [
					btn('\u2191', function() { move(sc.widgets, wi, -1); redraw(); }, wi === 0, _('Move up')),
					btn('\u2193', function() { move(sc.widgets, wi, 1); redraw(); }, wi === sc.widgets.length - 1, _('Move down')),
					btn('\u2715', function() { sc.widgets.splice(wi, 1); redraw(); }, false, _('Remove'))
				])
			]);
		}

		function screenCard(sc, si) {
			var avail = availableHeight(), used = usedHeight(sc.widgets, sc.spacing), over = used > avail;
			var remaining = avail - used - (sc.widgets.length ? (sc.spacing && sc.spacing !== 'auto' ? +sc.spacing : GAP) : 0);

			var addOptions = [ [ '', _('Add a widget...') ] ], groups = { general: _('General'), traffic: _('Traffic'), resources: _('Resources') };
			var addSel = E('select', { 'class': 'cbi-input-select', 'change': function() {
				var k = this.value;
				if (!k || sc.widgets.length >= MAX_WIDGETS) return;
				sc.widgets.push([ k, TYPES[k].def ]);
				redraw();
			} }, [ E('option', { 'value': '' }, addOptions[0][1]) ].concat([ 'general', 'traffic', 'resources' ].map(function(g) {
				return E('optgroup', { 'label': groups[g] }, Object.keys(TYPES).filter(function(k) { return TYPES[k].group === g; }).map(function(k) {
					var fits = widgetHeight(k, TYPES[k].def) <= remaining;
					return E('option', { 'value': k, 'disabled': fits ? null : '' }, _(TYPES[k].label) + ' (' + widgetHeight(k, TYPES[k].def) + ' px)');
				}));
			})));
			if (sc.widgets.length >= MAX_WIDGETS) addSel.disabled = true;

			var meter = E('div', { 'style': 'margin:6px 0' }, [
				E('div', { 'style': 'height:8px; background:#ddd; border-radius:4px; overflow:hidden; max-width:360px' },
					E('div', { 'style': 'height:8px; width:' + Math.min(100, used * 100 / avail) + '%; background:' + (over ? '#d33' : used > avail * 0.9 ? '#e90' : '#3a3') })),
				E('small', { 'style': over ? 'color:#d33' : '' }, over
					? _('Too much: %d of %d px used, the widgets do not fit on the screen.').format(used, avail)
					: _('%d of %d px used, %d px free').format(used, avail, avail - used))
			]);

			var canvas = E('canvas', { 'width': 136, 'height': 240, 'style': 'width:' + (136 * SCALE) + 'px; height:' + (240 * SCALE) + 'px; image-rendering:pixelated' });
			var note = E('div', { 'style': 'color:#d33; max-width:' + (136 * SCALE) + 'px' });
			if (!sc._folded) { canvases[si] = canvas; notes[si] = note; }

			var header = E('div', { 'style': 'display:flex; flex-wrap:wrap; gap:10px; align-items:center; margin-bottom:' + (sc._folded ? '0' : '8px') }, [
					E('strong', {}, _('Screen %d').format(si + 1)),
					text(sc.name, function(v) { sc.name = v; schedulePreview(); }, 130),
					E('span', {}, [ text(sc.dwell, function(v) {
						var n = parseFloat(v);
						sc.dwell = isNaN(n) || n < 0.5 ? '0.5' : String(n);
						redraw();
					}, 50), ' ', _('s on screen') ]),
					check(sc.enabled, function(v) { sc.enabled = v; redraw(); }, _('In rotation')),
					select([ [ 'auto', _('Spacing: auto') ], [ '2', _('Spacing: 2 px') ], [ '4', _('Spacing: 4 px') ], [ '6', _('Spacing: 6 px') ], [ '8', _('Spacing: 8 px') ], [ '12', _('Spacing: 12 px') ], [ '18', _('Spacing: 18 px') ] ], sc.spacing, function(v) { sc.spacing = v; redraw(); }),
					check(sc.backfill, function(v) { sc.backfill = v; redraw(); }, _('Signal background')),
					btn('\u2191', function() { move(screens, si, -1); redraw(); }, si === 0, _('Move screen up')),
					btn('\u2193', function() { move(screens, si, 1); redraw(); }, si === screens.length - 1, _('Move screen down')),
					btn(_('Remove screen'), function() {
						if (screens.length > 1 && confirm(_('Remove this screen?'))) { screens.splice(si, 1); redraw(); }
					}, screens.length <= 1),
					btn(_('Duplicate'), function() {
						if (screens.length >= 12) return;
						screens.splice(si + 1, 0, {
							name: sc.name + ' ' + _('copy'), dwell: sc.dwell, spacing: sc.spacing, enabled: sc.enabled,
							backfill: sc.backfill, _folded: false,
							widgets: sc.widgets.map(function(w) { return w.slice(); })
						});
						redraw();
					}, screens.length >= 12, _('Copy this screen below')),
					btn(sc._folded ? _('Unfold') : _('Fold'), function() { sc._folded = !sc._folded; redraw(); }, false,
						sc._folded ? _('Show widgets and preview') : _('Hide widgets and preview'))
				]);

			var parts = [ header ];
			if (!sc._folded) parts.push(E('div', { 'style': 'display:flex; flex-wrap:wrap; gap:20px; align-items:flex-start' }, [
					E('div', { 'style': 'flex:1 1 300px; min-width:0' }, [
						sc.widgets.length ? E('div', { 'style': 'overflow-x:auto' }, E('table', { 'class': 'table', 'style': 'width:100%' }, [
							E('tr', { 'class': 'tr table-titles' }, [ E('th', { 'class': 'th' }, _('Widget')), E('th', { 'class': 'th' }, _('Join next')), E('th', { 'class': 'th' }, _('Wrap')), E('th', { 'class': 'th' }, _('Shrink')), E('th', { 'class': 'th' }, _('Size')), E('th', { 'class': 'th' }, _('Height')), E('th', { 'class': 'th' }, '') ])
						].concat(sc.widgets.map(function(_w, wi) { return widgetRow(sc, wi); })))) : E('em', {}, _('Nothing on this screen yet.')),
						E('div', { 'style': 'margin-top:8px' }, addSel),
						meter,
						sc.backfill ? E('small', { 'style': 'opacity:.7; display:block; margin-top:4px' },
							_('The signal background colours the screen from the bottom by the Wi-Fi signal (red when poor, green when full). It is only used for Wi-Fi uplinks; with several Wi-Fi uplinks the screen is split horizontally between them.')) : ''
					]),
					E('div', { 'style': 'flex:none' }, [ canvas, note ])
				]));
			return E('div', { 'class': 'cbi-section', 'style': 'border:1px solid #bbb; border-radius:4px; padding:10px; margin-bottom:14px; opacity:' + (sc.enabled ? 1 : 0.7) }, parts);
		}

		function globalsView() {
			var nscr = screens.length, pinOptions = screens.map(function(s, i) { return [ String(i + 1), (i + 1) + ' - ' + s.name ]; });
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Rotation')),
				E('div', { 'style': 'display:flex; flex-wrap:wrap; gap:14px; align-items:center' }, [
					select([ [ 'rotate', _('Rotate through the screens') ], [ 'pin', _('Show one screen only') ] ], main.screen_mode, function(v) { main.screen_mode = v; redraw(); }),
					main.screen_mode === 'pin'
						? select(pinOptions, Math.min(+main.screen_pin || 1, nscr), function(v) { main.screen_pin = v; schedulePreview(); })
						: E('span', {}, [ _('Order: '), text(main.playlist, function(v) { main.playlist = v.replace(/[^0-9 ,]/g, ''); }, 140),
							' ', E('small', { 'style': 'opacity:.7' }, _('screen numbers, e.g. "1 2 1 3"; empty = all screens in order')) ])
				]),
				E('h3', { 'style': 'margin-top:14px' }, _('Look')),
				E('div', { 'style': 'display:flex; flex-wrap:wrap; gap:14px; align-items:center' }, [
					E('span', {}, [ _('Signal background brightness'), ' ', text(main.fill_dim, function(v) {
						var n = parseInt(v, 10); main.fill_dim = String(isNaN(n) ? 30 : Math.max(5, Math.min(100, n))); redraw();
					}, 50, 'number'), ' %' ]),
					check(main.fill_edge, function(v) { main.fill_edge = v; redraw(); }, _('Bright line at the edge of the fill')),
					E('span', {}, [ _('Client names up to'), ' ', text(main.name_len, function(v) {
						var n = parseInt(v, 10); main.name_len = String(isNaN(n) ? 8 : Math.max(3, Math.min(16, n))); redraw();
					}, 50, 'number'), ' ', _('characters') ])
				]),
				E('h3', { 'style': 'margin-top:14px' }, _('Preview data')),
				E('div', { 'style': 'display:flex; flex-wrap:wrap; gap:14px; align-items:center' }, [
					select([ [ 'live', _('Live data from this router') ], [ 'example', _('Example data (Wi-Fi + Ethernet, VPN, clients)') ] ], preview.source, function(v) { preview.source = v; schedulePreview(); }),
					select([ [ 'none', _('As is') ], [ 'weak', _('What if: weak Wi-Fi') ], [ 'longssid', _('What if: long Wi-Fi name') ], [ 'ethernet', _('What if: wired uplink') ], [ 'nouplink', _('What if: no uplink') ],
						[ 'nointernet', _('What if: no internet') ], [ 'vpnup', _('What if: VPN connected') ], [ 'vpndown', _('What if: VPN down') ], [ 'novpn', _('What if: no VPN') ] ],
						preview.scenario, function(v) { preview.scenario = v; schedulePreview(); }),
					E('small', { 'style': 'opacity:.7' }, _('The preview is drawn by the router itself with the settings on this page, saved or not. The rounded corners are cut off by the bezel.'))
				])
			]);
		}

		function redraw() {
			canvases = []; notes = [];
			dom_content(globals, globalsView());
			dom_content(container, screens.map(screenCard));
			schedulePreview();
		}

		function dom_content(node, children) {
			while (node.firstChild) node.removeChild(node.firstChild);
			(Array.isArray(children) ? children : [ children ]).forEach(function(c) { node.appendChild(c); });
		}

		this.model = { main: main, screens: screens };

		var wrap = E('div', { 'style': stockActive ? 'opacity:.5; pointer-events:none' : '' }, [
			globals, container,
			E('div', {}, btn(_('Add screen'), function() {
				if (screens.length >= 12) return;
				screens.push({ name: _('Screen') + ' ' + (screens.length + 1), dwell: '8', spacing: 'auto', enabled: true, backfill: true, widgets: [] });
				redraw();
			}, false))
		]);
		redraw();

		/* LuCI's E() turns an array argument into elements one by one (createElement per item),
		 * so only nodes may go in here - no empty strings or nulls as placeholders. */
		var page = [
			E('h2', {}, _('LCD Screens')),
			E('div', { 'class': 'cbi-map-descr' }, _('Screens of the built-in LCD renderer: the screens rotate on their own dwell times, in the order below or by the playlist. Save & Apply restarts the display service.'))
		];
		if (stockActive)
			page.push(E('div', { 'class': 'alert-message warning' }, _('The display currently uses the stock UniFi look, which has its own screens. Switch the display mode to "Built-in renderer" on the Settings tab to use these.')));
		page.push(wrap);
		return E(page);
	},

	handleSave: function() {
		var m = this.model;
		if (!m) return Promise.resolve();
		uci.set('utr-lcd', 'main', 'screen_mode', m.main.screen_mode);
		uci.set('utr-lcd', 'main', 'screen_pin', m.main.screen_pin);
		uci.set('utr-lcd', 'main', 'playlist', m.main.playlist.replace(/\s+/g, ' ').trim());
		uci.set('utr-lcd', 'main', 'fill_dim', m.main.fill_dim);
		uci.set('utr-lcd', 'main', 'fill_edge', m.main.fill_edge ? '1' : '0');
		uci.set('utr-lcd', 'main', 'name_len', m.main.name_len);

		uci.sections('utr-lcd', 'screen').forEach(function(s) { uci.remove('utr-lcd', s['.name']); });
		m.screens.forEach(function(s) {
			var sid = uci.add('utr-lcd', 'screen');
			uci.set('utr-lcd', sid, 'name', s.name);
			uci.set('utr-lcd', sid, 'dwell', s.dwell);
			uci.set('utr-lcd', sid, 'spacing', s.spacing || 'auto');
			uci.set('utr-lcd', sid, 'enabled', s.enabled ? '1' : '0');
			uci.set('utr-lcd', sid, 'indicator', 'none');
			uci.set('utr-lcd', sid, 'backfill', s.backfill ? '1' : '0');
			var wlist = [], line = [];
			s.widgets.forEach(function(w, i) {
				line.push(widgetSpec(w));
				if (!w[2] || i === s.widgets.length - 1) { wlist.push(line.join('+')); line = []; }
			});
			uci.set('utr-lcd', sid, 'widget', wlist);
		});
		return uci.save();
	},

	handleReset: function() {
		window.location.reload();
	}
});
