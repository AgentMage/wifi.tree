#!/bin/bash
set -e
export PATH=$PATH:/usr/sbin:/sbin

PHY=phy0
WLAN=wlan0
AP=uap0
HOSTAPD_CONF=/etc/hostapd/hostapd.conf

# wait for wlan0 to associate (up to 30s) so we know its channel
for i in $(seq 1 30); do
    CHAN=$(iw dev "$WLAN" info 2>/dev/null | awk '/channel/ {print $2; exit}')
    if [ -n "$CHAN" ]; then
        break
    fi
    sleep 1
done

if [ -z "$CHAN" ]; then
    echo "wifitree-uap0-setup: wlan0 not associated, defaulting to channel 1" >&2
    CHAN=1
fi

# (re)create the virtual AP interface on the same phy/channel
if iw dev "$AP" info >/dev/null 2>&1; then
    ip link set "$AP" down || true
    iw dev "$AP" del || true
fi
iw phy "$PHY" interface add "$AP" type __ap

ip addr flush dev "$AP" || true
ip addr add 10.42.0.1/24 dev "$AP"
ip link set "$AP" up

# make sure hostapd.conf's channel matches wlan0's actual current channel
sed -i "s/^channel=.*/channel=$CHAN/" "$HOSTAPD_CONF"

echo "wifitree-uap0-setup: uap0 ready on channel $CHAN"
