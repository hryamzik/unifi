#!/bin/sh
# collect.sh - print router status as key=value lines for utr-lcd-status and feed.sh. Read-only, BusyBox ash.
#
# Output keys:
#   up_state   up|down            default-route uplink present
#   up_type    wifi|eth|usb
#   up_dev     uplink device name
#   up_signal  wifi signal in dBm (number)
#   up_speed   link speed in Mbps (number)
#   up_name    SSID (wifi) or "Ethernet"
#   up_detail  signal (wifi) / link speed (eth) / device name
#   wan_ip     IPv4 address of the uplink device
#   lan_ip     IPv4 address of the LAN bridge
#   inet       ok|fail   a ping to 1.1.1.1 got through (only printed when up_state=up)
#   ups        number of uplinks (default routes in any table, VPN devices excluded, lowest metric first; at most 3)
#   upN_dev upN_type upN_name upN_signal upN_speed upN_metric   for N = 0..ups-1 (N=0 is the same uplink as up_*)
#   mws        number of mwan3 interfaces (only when mwan3 answers), mwN_if mwN_dev mwN_state mwN_pct
#              (state from `mwan3 status`, pct = share in the policy of the default rule, -1 = unknown)
#   vpn_state  up|down|none
#   vpn_name   VPN interface name
#   clients    LAN clients (complete ARP entries on the LAN bridge)
#   uptime     seconds
#
# Env: LCD_LAN_IF (default br-lan), LCD_VPN_RE (default ^(wg|tun|tap|tailscale|ppp|zt|ipsec))

LAN_IF=${LCD_LAN_IF:-br-lan}
VPN_RE=${LCD_VPN_RE:-'^(wg|tun|tap|tailscale|ppp|zt|ipsec)'}

is_vpn() { echo "$1" | grep -Eq "$VPN_RE"; }

# first default-route device that is not a VPN device
up_dev=""
for d in $(ip route show default 2>/dev/null | awk '{for(i=1;i<NF;i++) if($i=="dev") print $(i+1)}'); do
	is_vpn "$d" && continue
	up_dev=$d
	break
done

if [ -n "$up_dev" ]; then
	echo "up_state=up"
	wan_ip=$(ip -4 addr show dev "$up_dev" 2>/dev/null | awk '/inet /{sub("/.*","",$2); print $2; exit}')
	echo "wan_ip=$wan_ip"
	echo "up_dev=$up_dev"
	# internet reachability (one probe per run; consumers decide how many failures matter)
	if ping -c1 -W1 1.1.1.1 >/dev/null 2>&1; then echo "inet=ok"; else echo "inet=fail"; fi
	info=$(iwinfo "$up_dev" info 2>/dev/null)
	if echo "$info" | grep -q 'Mode: Client'; then
		echo "up_type=wifi"
		echo "up_name=$(echo "$info" | sed -n 's/.*ESSID: "\(.*\)"/\1/p' | head -n1)"
		sig=$(echo "$info" | sed -n 's/.*Signal: \(-\{0,1\}[0-9]*\) dBm.*/\1/p' | head -n1)
		echo "up_signal=$sig"
		echo "up_detail=${sig:+$sig dBm}"
		echo "up_speed=$(echo "$info" | sed -n 's/.*Bit Rate: \([0-9]*\).*/\1/p' | head -n1)"
	else
		case "$up_dev" in
		usb*|rndis*|enx*|rmnet*|wwan*) echo "up_type=usb"; echo "up_name=USB tethering" ;;
		*) echo "up_type=eth"; echo "up_name=Ethernet" ;;
		esac
		spd=$(cat "/sys/class/net/$up_dev/speed" 2>/dev/null)
		case "$spd" in
		''|-*) echo "up_detail=$up_dev"; echo "up_speed=0" ;;
		*) echo "up_detail=$spd Mbps"; echo "up_speed=$spd" ;;
		esac
	fi
else
	echo "up_state=down"
fi

# all uplinks (multi-WAN: failover or balancing, mwan3 or plain routes), lowest metric first
n=0
for e in $(ip route show table all 2>/dev/null | awk '$1=="default"{m=0; d=""; for(i=1;i<NF;i++){if($i=="dev")d=$(i+1); if($i=="metric")m=$(i+1)} if(d!="") print m":"d}' | sort -t: -k1,1n); do
	d=${e#*:}
	is_vpn "$d" && continue
	case " $seen " in *" $d "*) continue ;; esac
	seen="$seen $d"
	[ "$n" -ge 3 ] && break
	info=$(iwinfo "$d" info 2>/dev/null)
	echo "up${n}_dev=$d"
	echo "up${n}_metric=${e%%:*}"
	if echo "$info" | grep -q 'Mode: Client'; then
		echo "up${n}_type=wifi"
		echo "up${n}_name=$(echo "$info" | sed -n 's/.*ESSID: "\(.*\)"/\1/p' | head -n1)"
		echo "up${n}_signal=$(echo "$info" | sed -n 's/.*Signal: \(-\{0,1\}[0-9]*\) dBm.*/\1/p' | head -n1)"
		echo "up${n}_speed=$(echo "$info" | sed -n 's/.*Bit Rate: \([0-9]*\).*/\1/p' | head -n1)"
	else
		case "$d" in
		usb*|rndis*|enx*|rmnet*|wwan*) echo "up${n}_type=usb"; echo "up${n}_name=USB" ;;
		*) echo "up${n}_type=eth"; echo "up${n}_name=Ethernet" ;;
		esac
		spd=$(cat "/sys/class/net/$d/speed" 2>/dev/null)
		case "$spd" in ''|-*) echo "up${n}_speed=0" ;; *) echo "up${n}_speed=$spd" ;; esac
	fi
	n=$((n + 1))
done
echo "ups=$n"

# mwan3 interfaces and their state (optional: absent on routers without mwan3)
if command -v mwan3 >/dev/null 2>&1 && [ -f /etc/config/mwan3 ]; then
	mw_raw=$(mwan3 status 2>/dev/null)
	if [ -n "$mw_raw" ]; then
		mw_pol=$(uci -q get mwan3.default_rule_v4.use_policy 2>/dev/null)
		[ -n "$mw_pol" ] || mw_pol=$(uci -q get mwan3.default_rule_v6.use_policy 2>/dev/null)
		printf '%s\n' "$mw_raw" | awk -v pol="$mw_pol" '
			/^Interface status:/ { sec=1; next }
			/^Current ipv4 policies:/ { sec=2; next }
			/^Current ipv6|^Directly/ { sec=0 }
			sec==1 && $1=="interface" { st[$2]=$4; ord[++n]=$2 }
			sec==2 && /^[^ \t].*:$/ { cur=$0; sub(/:$/, "", cur) }
			sec==2 && cur==pol && $2 ~ /^\(/ { p=$2; gsub(/[()%]/, "", p); pct[$1]=p }
			END { print "mws=" n; for (i=1;i<=n;i++) printf "mw%d_if=%s\nmw%d_state=%s\nmw%d_pct=%d\n", i-1, ord[i], i-1, st[ord[i]], i-1, (ord[i] in pct) ? pct[ord[i]] : -1 }'
		mw_i=0
		for mw_if in $(printf '%s\n' "$mw_raw" | awk '/^Interface status:/{s=1;next} /^Current/{s=0} s && $1=="interface"{print $2}'); do
			mw_dev=$(uci -q get "network.$mw_if.device" 2>/dev/null)
			[ -n "$mw_dev" ] || mw_dev=$(uci -q get "network.$mw_if.ifname" 2>/dev/null)
			[ -n "$mw_dev" ] || mw_dev=$mw_if
			echo "mw${mw_i}_dev=$mw_dev"
			mw_i=$((mw_i + 1))
			[ "$mw_i" -ge 3 ] && break
		done
	fi
fi

# VPN: first matching interface
vpn=""
for n in $(ls /sys/class/net 2>/dev/null); do
	is_vpn "$n" && { vpn=$n; break; }
done
if [ -z "$vpn" ]; then
	echo "vpn_state=none"
else
	echo "vpn_name=$vpn"
	flags=$(cat "/sys/class/net/$vpn/flags" 2>/dev/null)
	if [ -n "$flags" ] && [ $(( flags & 1 )) -eq 1 ] && ip -4 addr show dev "$vpn" 2>/dev/null | grep -q 'inet '; then
		echo "vpn_state=up"
	else
		echo "vpn_state=down"
	fi
fi

echo "lan_ip=$(ip -4 addr show dev "$LAN_IF" 2>/dev/null | awk '/inet /{sub("/.*","",$2); print $2; exit}')"
echo "clients=$(awk -v d="$LAN_IF" '$6==d && $3=="0x2"{n++} END{print n+0}' /proc/net/arp)"
echo "uptime=$(awk '{print int($1)}' /proc/uptime)"
