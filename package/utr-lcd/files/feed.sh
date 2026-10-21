#!/bin/sh
# utr-lcd feeder - replaces Ubiquiti's mcad for the stock LCD UI: reads OpenWrt state and pushes it
# to the stock UI's ubus object (ui state / ui info). BusyBox ash.
#
#   UTR_RUN       state dir (default /var/run/utr-lcd)
#   UTR_LIBEXEC   helper dir (default /usr/libexec/utr-lcd)
#   UTR_THEME     blue|aqua|green|pink|yellow|orange|purple (default blue)
#   UTR_BRIGHTNESS backlight percent 1-100 (default: leave the UI's own value)
#   UTR_TIMEOUT   screen timeout in seconds, 0 = always on (default 0); raise-to-wake is wake.sh's job
#   UTR_INTERVAL  seconds between updates (default 3)
BASE=${UTR_RUN:-/var/run/utr-lcd}
LIBEXEC=${UTR_LIBEXEC:-/usr/libexec/utr-lcd}
COLLECT=$LIBEXEC/collect.sh
THEME=${UTR_THEME:-blue}
INTERVAL=${UTR_INTERVAL:-3}
TIMEOUT=${UTR_TIMEOUT:-0}
case "$TIMEOUT" in ''|*[!0-9]*) TIMEOUT=0 ;; esac
BRIGHT=${UTR_BRIGHTNESS:-}
case "$BRIGHT" in ''|*[!0-9]*) BRIGHT="" ;; esac
[ -n "$BRIGHT" ] && { [ "$BRIGHT" -lt 1 ] && BRIGHT=1; [ "$BRIGHT" -gt 100 ] && BRIGHT=100; }
CALL=$LIBEXEC/ui-call.sh
mkdir -p "$BASE"
fails=0
last=""

jstr() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }

while :; do
	up_state=down up_type="" up_name="" up_speed=0 up_signal="" up_dev=""
	vpn_state=none vpn_name="" clients=0
	# parse key=value lines safely (values may contain spaces; never eval collector output)
	$COLLECT > "$BASE/collect.out" 2>/dev/null
	while IFS= read -r line; do
		k=${line%%=*}; v=${line#*=}
		case "$k" in
		up_state) up_state=$v ;;   up_type) up_type=$v ;;     up_name) up_name=$v ;;
		up_speed) up_speed=$v ;;   up_signal) up_signal=$v ;; up_dev) up_dev=$v ;;
		vpn_state) vpn_state=$v ;; vpn_name) vpn_name=$v ;;   clients) clients=$v ;;
		esac
	done < "$BASE/collect.out"
	case "$up_speed" in ''|*[!0-9]*) up_speed=0 ;; esac
	case "$clients" in ''|*[!0-9]*) clients=0 ;; esac
	case "$up_signal" in ''|*[!0-9-]*) up_signal="" ;; esac

	# uplink interface name as the stock UI knows it
	case "$up_type" in
	wifi) iface=ath4 ;;
	usb)  iface=usb0 ;;
	*)    iface=eth0 ;;
	esac

	# Stock UI reads signal from /proc/net/wireless for its "ath4" interface; mirror ours there.
	if [ "$up_type" = wifi ] && [ -n "$up_signal" ]; then
		{
			echo "Inter-| sta-|   Quality        |   Discarded packets               | Missing"
			echo " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon"
			echo " ath4: 0000   70.  $up_signal.  -95.        0      0      0      0      0        0"
		} > "$BASE/wireless.tmp" && mv "$BASE/wireless.tmp" "$BASE/wireless"
	fi

	# internet reachability: 3 consecutive failures before declaring it down
	if [ "$up_state" = up ]; then
		if ping -c1 -W2 1.1.1.1 >/dev/null 2>&1; then fails=0; else fails=$((fails + 1)); fi
	fi

	if [ "$up_state" != up ]; then state=noUplink
	elif [ "$fails" -ge 3 ]; then state=noInternet
	else state=ready; fi

	case "$vpn_state" in up) vs=connected ;; down) vs=disconnected ;; *) vs=disconnected ;; esac
	case "$vpn_name" in
	wg*|tailscale*) vt=wireguard ;;
	tun*|tap*|ppp*) vt=openvpn ;;
	*) vt="" ;;
	esac
	[ "$vpn_state" = none ] && { vs=disconnected; vt=""; vpn_name=""; }

	json=$(printf '{"clients":%d,"meshUplinkInterface":"%s","meshLinkSpeed":%d,"meshSsid":"%s","meshStatus":"connected","vpnStatus":"%s","vpnName":"%s","vpnType":"%s","theme":"%s"}' \
		"${clients:-0}" "$iface" "${up_speed:-0}" "$(jstr "$up_name")" "$vs" "$(jstr "$vpn_name")" "$vt" "$THEME")
	[ -n "$BRIGHT" ] && json="${json%\}},\"brightness\":$BRIGHT}"
	# the stock UI only honours the timeout while raiseToWake is on
	if [ "$TIMEOUT" -gt 0 ]; then json="${json%\}},\"raiseToWake\":true,\"screenTimeout\":$TIMEOUT}"
	else json="${json%\}},\"raiseToWake\":false}"; fi
	sig="$state|$json"
	if [ "$sig" != "$last" ]; then
		# only remember the new signature once the UI accepted it (it may still be starting)
		if $CALL info "$json" >/dev/null 2>&1 && $CALL state "{\"value\":\"$state\"}" >/dev/null 2>&1; then
			last=$sig
		fi
	fi
	sleep "$INTERVAL"
done
