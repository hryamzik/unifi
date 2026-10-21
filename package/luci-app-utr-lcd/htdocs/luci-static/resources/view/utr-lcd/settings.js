'use strict';
'require dom';
'require form';
'require fs';
'require poll';
'require view';

var STATUS_HELPER = '/usr/libexec/utr-lcd/luci-status';

function callStatus() {
	return fs.exec_direct(STATUS_HELPER, [], 'json').catch(function() { return null; });
}

function row(label, value) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td left', 'width': '33%' }, E('strong', label)),
		E('td', { 'class': 'td left' }, value)
	]);
}

function statusRows(s) {
	if (!s)
		return [ row(_('Status'), _('Could not read the status (is the utr-lcd package installed?)')) ];

	var st = s.state || {};
	var modeText = {
		native: _('Built-in renderer'),
		stock: _('Stock UniFi look'),
		stopped: _('Stopped')
	}[s.mode] || s.mode;

	var uplink = _('No uplink');
	if (st.up_state === 'up') {
		var kind = { wifi: _('Wi-Fi'), eth: _('Ethernet'), usb: _('USB tethering') }[st.up_type] || st.up_type;
		uplink = kind + (st.up_name && st.up_type === 'wifi' ? ' "' + st.up_name + '"' : '') +
			(st.up_detail ? ' (' + st.up_detail + ')' : '');
	}

	var vpn = st.vpn_state === 'up' ? _('Connected') + (st.vpn_name ? ' (' + st.vpn_name + ')' : '')
		: (st.vpn_state === 'down' ? _('Disconnected') : _('Not active'));

	return [
		row(_('Display'), E('span', { 'style': 'color:' + (s.mode === 'stopped' ? 'red' : 'green') }, modeText)),
		row(_('LCD panel driver'), s.drm ? _('Present (/dev/dri/card0)') : _('Missing, the display driver is not loaded')),
		row(_('Stock UI bundle'), s.stock_installed ? _('Installed') + ' (' + (s.stock_version || '?') + ')' : _('Not installed')),
		row(_('Uplink'), uplink),
		row(_('WAN IP'), st.wan_ip || '-'),
		row(_('VPN'), vpn),
		row(_('Connected clients'), st.clients != null ? String(st.clients) : '-')
	];
}

return view.extend({
	load: function() {
		return callStatus();
	},

	render: function(status) {
		var m, s, o;

		m = new form.Map('utr-lcd', _('LCD Display'),
			_('Status display for the UniFi Travel Router LCD. Saving restarts the display service.'));

		s = m.section(form.NamedSection, 'main', 'utr-lcd', _('Settings'));
		s.addremove = false;

		o = s.option(form.Flag, 'enabled', _('Enable'));
		o.default = o.enabled;
		o.rmempty = false;

		o = s.option(form.ListValue, 'mode', _('Display mode'),
			_('Automatic uses the stock UniFi look when its bundle is installed, otherwise the built-in renderer.'));
		o.value('auto', _('Automatic'));
		o.value('native', _('Built-in renderer'));
		o.value('stock', _('Stock UniFi look (needs the stock bundle)'));
		o.default = 'auto';

		o = s.option(form.Value, 'interval', _('Refresh interval'), _('Seconds between status updates.'));
		o.datatype = 'range(1,60)';
		o.default = '3';
		o.placeholder = '3';

		o = s.option(form.ListValue, 'rotate', _('Rotation'),
			_('The Long-Range model is mounted upside down, so it uses 180°.'));
		o.value('180', '180°');
		o.value('0', '0°');
		o.default = '180';

		o = s.option(form.Value, 'brightness', _('Brightness'),
			_('Backlight in percent (1-100), applied when the service starts. Empty leaves it unchanged.'));
		o.datatype = 'range(1,100)';
		o.optional = true;
		o.placeholder = '100';

		o = s.option(form.ListValue, 'theme', _('Theme'), _('Stock UniFi look only.'));
		['blue', 'aqua', 'green', 'pink', 'yellow', 'orange', 'purple'].forEach(function(t) {
			o.value(t, t.charAt(0).toUpperCase() + t.slice(1));
		});
		o.default = 'blue';
		o.depends('mode', 'auto');
		o.depends('mode', 'stock');

		o = s.option(form.Value, 'offset_x', _('Horizontal offset'),
			_('Stock UniFi look only: shifts the 135 px wide image inside the 136 px panel, in pixels.'));
		o.datatype = 'range(-8,8)';
		o.default = '0';
		o.depends('mode', 'auto');
		o.depends('mode', 'stock');

		o = s.option(form.Value, 'offset_y', _('Vertical offset'), _('Stock UniFi look only, in pixels.'));
		o.datatype = 'range(-8,8)';
		o.default = '0';
		o.depends('mode', 'auto');
		o.depends('mode', 'stock');

		var statusTable = E('table', { 'class': 'table' }, statusRows(status));

		poll.add(function() {
			return callStatus().then(function(st) {
				dom.content(statusTable, statusRows(st));
			});
		}, 5);

		return m.render().then(function(mapEl) {
			return E([
				E('div', { 'class': 'cbi-section' }, [ E('h3', _('Status')), statusTable ]),
				mapEl
			]);
		});
	}
});
