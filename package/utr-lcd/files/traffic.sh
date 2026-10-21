#!/bin/sh
# traffic.sh - sample uplink and per-client traffic for the built-in renderer. Read-only, BusyBox ash.
#
# Every INTERVAL seconds it reads byte counters (uplink device, wired LAN ports, Wi-Fi stations from
# `iw station dump`) and keeps running totals and current rates in RAM only; nothing touches flash.
# Result, rewritten atomically as $UTR_RUN/traffic:
#   t=<uptime seconds>
#   down_bps= up_bps=            rates of all uplinks together, in bits/s
#   down_total= up_total=        uplink bytes since the sampler started (counters are summed over resets)
#   uplink=<dev>|<down bps>|<up bps>|<down bytes>|<up bytes>   one line per uplink (several with multi-WAN)
#   client=<id>|<name>|<bps>|<bytes>|<online>     id is a MAC, or LAN for all wired ports together;
#                                                  bps and bytes count both directions
# Started by the init script only while a configured screen shows traffic widgets.
#
# Env: UTR_RUN (/var/run/utr-lcd), UTR_TRAFFIC_INTERVAL (2), LCD_LAN_IF (br-lan),
#      LCD_VPN_RE (default ^(wg|tun|tap|tailscale|ppp|zt|ipsec))

RUN=${UTR_RUN:-/var/run/utr-lcd}
INTERVAL=${UTR_TRAFFIC_INTERVAL:-2}
LAN_IF=${LCD_LAN_IF:-br-lan}
VPN_RE=${LCD_VPN_RE:-'^(wg|tun|tap|tailscale|ppp|zt|ipsec)'}
OUT=$RUN/traffic
mkdir -p "$RUN"

# every uplink device (default routes in any table, no VPN devices), at most 3
up_devs() {
	local n=0 d
	for d in $(ip route show table all 2>/dev/null | awk '$1=="default"{for(i=1;i<NF;i++) if($i=="dev") print $(i+1)}' | awk '!s[$0]++'); do
		echo "$d" | grep -Eq "$VPN_RE" && continue
		[ "$n" -ge 3 ] && break
		echo "$d"
		n=$((n + 1))
	done
}

snapshot() {
	echo "T $(cut -d' ' -f1 /proc/uptime)"
	local d p
	for d in $(up_devs); do
		[ -r "/sys/class/net/$d/statistics/rx_bytes" ] &&
			echo "U $d $(cat /sys/class/net/$d/statistics/rx_bytes) $(cat /sys/class/net/$d/statistics/tx_bytes)"
	done
	for p in $(ls /sys/class/net/$LAN_IF/brif 2>/dev/null); do
		if [ -e "/sys/class/net/$p/phy80211" ]; then
			iw dev "$p" station dump 2>/dev/null | awk '/^Station/{mac=$2} /rx bytes:/{rx=$3} /tx bytes:/{print "S", mac, rx, $3}'
		else
			# a wired port: what the port receives is what the client uploaded
			echo "L $p $(cat /sys/class/net/$p/statistics/rx_bytes) $(cat /sys/class/net/$p/statistics/tx_bytes)"
		fi
	done
	awk '{print "H", $2, $3, $4}' /tmp/dhcp.leases 2>/dev/null
	echo "E"
}

(
	while :; do
		snapshot
		sleep "$INTERVAL"
	done
) | awk -v out="$OUT" '
function delta(key, cur,    d) {          # counter delta, summed over resets and first sight
	if (!(key in prev)) d = cur
	else if (cur >= prev[key]) d = cur - prev[key]
	else d = cur
	prev[key] = cur
	return d
}
function add_client(id, bytes_rx, bytes_tx,    d) {
	d = delta(id, bytes_rx + bytes_tx)
	cdelta[id] += d
	ctotal[id] += d
	seen_now[id] = 1
}
$1 == "T" { t = $2; for (k in cdelta) delete cdelta[k]; for (k in seen_now) delete seen_now[k]; for (k in udd) { delete udd[k]; delete uud[k] }; up_d = 0; down_d = 0 }
$1 == "U" { udd[$2] = delta("U:" $2 ":rx", $3); uud[$2] = delta("U:" $2 ":tx", $4); down_d += udd[$2]; up_d += uud[$2]; down_total += udd[$2]; up_total += uud[$2]; utot_d[$2] += udd[$2]; utot_u[$2] += uud[$2] }
$1 == "S" { add_client($2, $3, $4) }
$1 == "L" { add_client("LAN:" $2, $3, $4); wired["LAN:" $2] = 1 }
$1 == "H" { name[$2] = ($4 == "*" || $4 == "") ? $3 : $4 }
$1 == "E" {
	dt = t - last_t; last_t = t
	if (!started) { started = 1; next }          # no rate from the first sample
	if (dt <= 0) next
	tmp = out ".new"
	printf "t=%s\ndown_bps=%.0f\nup_bps=%.0f\ndown_total=%.0f\nup_total=%.0f\n", t, down_d * 8 / dt, up_d * 8 / dt, down_total, up_total > tmp
	for (d in udd) printf "uplink=%s|%.0f|%.0f|%.0f|%.0f\n", d, udd[d] * 8 / dt, uud[d] * 8 / dt, utot_d[d], utot_u[d] > tmp
	# wired ports are merged into one client
	lan_rate = 0; lan_total = 0; lan_seen = 0
	for (id in ctotal) {
		if (id in wired) { lan_rate += cdelta[id] * 8 / dt; lan_total += ctotal[id]; if (id in seen_now) lan_seen = 1; continue }
		n = (id in name) ? name[id] : substr(id, 10)
		on = (id in seen_now) ? 1 : 0
		printf "client=%s|%s|%.0f|%.0f|%d\n", id, n, on ? cdelta[id] * 8 / dt : 0, ctotal[id], on > tmp
	}
	if (lan_total > 0) printf "client=LAN|LAN|%.0f|%.0f|%d\n", lan_rate, lan_total, lan_seen > tmp
	close(tmp)
	system("mv " tmp " " out)
}'
