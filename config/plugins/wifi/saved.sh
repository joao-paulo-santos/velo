#!/bin/sh
# List saved WiFi networks for deletion
# Output as JSON array for hypr-tofi

result=$(nmcli -t -f NAME,TYPE connection show 2>/dev/null | grep ':802-11-wireless$' | cut -d: -f1 | while read -r ssid; do
    if [ -z "$ssid" ]; then
        continue
    fi
    ssid_escaped=$(echo "$ssid" | sed 's/\\/\\\\/g; s/"/\\"/g')
    printf '{"name":"%s","id":"%s","action":{"selection_type":"self","template":"nmcli connection delete \\\"%s\\\" && notify-send WiFi Forgotten %s || notify-send WiFi Failed to forget %s"}}\n' "$ssid" "$ssid_escaped" "$ssid_escaped" "$ssid_escaped" "$ssid_escaped"
done | paste -sd, -)

if [ -n "$result" ]; then
    echo "[$result]"
else
    echo "[]"
fi
