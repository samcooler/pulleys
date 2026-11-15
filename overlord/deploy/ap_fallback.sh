#!/bin/bash
# Boot-time Wi-Fi fallback: if the Wi-Fi has not joined a network within
# OVERLORD_AP_WAIT seconds (default 20) of this unit starting, bring up the
# Overlord's own access point (NetworkManager connection "overlord-ap", made by
# install.sh) so the web GUI stays reachable at http://10.42.0.1:8080/.
#
# It stays an AP until the next reboot: one radio cannot host an AP and look
# for the phone at the same time, and rescanning would drop the GUI.
# A wired link does not count; only the Wi-Fi is checked.
set -u

wait_s="${OVERLORD_AP_WAIT:-20}"

wifi_dev() { nmcli -t -f DEVICE,TYPE dev | awk -F: '$2=="wifi"{print $1; exit}'; }

dev=""
for _ in $(seq 10); do
    dev="$(wifi_dev)"
    [ -n "$dev" ] && break
    sleep 1
done
[ -n "$dev" ] || { echo "ap-fallback: no Wi-Fi device, nothing to do"; exit 0; }

for _ in $(seq "$wait_s"); do
    state="$(nmcli -t -f GENERAL.STATE dev show "$dev" | cut -d: -f2)"
    conn="$(nmcli -t -f GENERAL.CONNECTION dev show "$dev" | cut -d: -f2)"
    if [[ "$state" == 100* ]] && [ "$conn" != overlord-ap ]; then
        echo "ap-fallback: $dev joined '$conn', no AP needed"
        exit 0
    fi
    sleep 1
done

echo "ap-fallback: $dev joined no network in ${wait_s}s, starting overlord-ap"
nmcli con up overlord-ap ifname "$dev" || { echo "ap-fallback: could not start overlord-ap"; exit 1; }
