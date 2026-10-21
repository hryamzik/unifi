#!/bin/sh
# fake collector for offline previews; env overrides: FAKE_SIGNAL, FAKE_VPN (none|up|down), FAKE_WAN_IP, FAKE_SSID
echo up_state=up; echo wan_ip=${FAKE_WAN_IP:-192.168.87.221}; echo lan_ip=192.168.1.1; echo up_dev=phy0-sta0; echo up_type=wifi; echo "up_name=${FAKE_SSID:-iNomad}"
echo up_signal=${FAKE_SIGNAL:--52}; echo up_detail="-52 dBm"; echo up_speed=144; echo inet=ok; echo vpn_state=${FAKE_VPN:-up}; echo vpn_name=wg0; echo clients=3; echo uptime=9163
