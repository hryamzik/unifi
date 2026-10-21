#!/bin/sh
# fake collector for offline previews; env overrides:
#   FAKE_SIGNAL (dBm), FAKE_VPN (none|up|down), FAKE_WAN_IP, FAKE_SSID, FAKE_UPS (1 or 2: a second uplink, Ethernet; or 2w: a second Wi-Fi)
echo up_state=up; echo wan_ip=${FAKE_WAN_IP:-192.168.87.221}; echo lan_ip=192.168.1.1; echo up_dev=phy0-sta0; echo up_type=wifi; echo "up_name=${FAKE_SSID:-iNomad}"
echo up_signal=${FAKE_SIGNAL:--52}; echo up_detail="-52 dBm"; echo up_speed=144; echo inet=ok; echo vpn_state=${FAKE_VPN:-up}; echo vpn_name=wg0; echo clients=3; echo uptime=9163
echo up0_dev=phy0-sta0; echo up0_type=wifi; echo "up0_name=${FAKE_SSID:-iNomad}"; echo up0_signal=${FAKE_SIGNAL:--52}; echo up0_speed=144; echo up0_metric=10
case "${FAKE_UPS:-1}" in
2) echo up1_dev=eth0; echo up1_type=eth; echo up1_name=Ethernet; echo up1_speed=1000; echo up1_metric=20; echo ups=2 ;;
2w) echo up1_dev=phy1-sta0; echo up1_type=wifi; echo "up1_name=Hotel WiFi"; echo up1_signal=-71; echo up1_speed=72; echo up1_metric=20; echo ups=2 ;;
*) echo ups=1 ;;
esac
