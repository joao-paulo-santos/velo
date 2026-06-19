#!/bin/sh
# List WiFi networks with status, signal, and rate
# Output as JSON array for velo

nmcli device wifi rescan 2>/dev/null
sleep 0.3

# Get saved connections
SAVED=$(nmcli -t -f NAME connection show 2>/dev/null)

# Get wifi interface
IFACE=$(nmcli -t -f DEVICE,TYPE device 2>/dev/null | grep ':wifi$' | cut -d: -f1 | head -1)

# Get results and format as JSON array
result=$(nmcli -t -f IN-USE,SSID,SIGNAL,RATE device wifi list 2>/dev/null | sort -t: -k3 -rn | while IFS=: read -r in_use ssid signal rate; do
    if [ -z "$ssid" ]; then
        continue
    fi
    
    # Escape ssid for JSON (escape backslashes and quotes)
    ssid_escaped=$(echo "$ssid" | sed 's/\\/\\\\/g; s/"/\\"/g')
    
    if [ "$in_use" = "*" ]; then
        # Currently connected - disconnect
        printf '{"name":"✓ %s [%s%% %s]","id":"%s","action":{"selection_type":"self","template":"nmcli device disconnect %s && notify-send WiFi Disconnected from %s || notify-send WiFi Failed to disconnect"}}\n' "$ssid" "$signal" "$rate" "$ssid_escaped" "$IFACE" "$ssid_escaped"
    elif echo "$SAVED" | grep -qx "$ssid"; then
        # Saved network - connect using stored credentials
        printf '{"name":"🔑 %s [%s%% %s]","id":"%s","action":{"selection_type":"self","template":"nmcli connection up \\\"%s\\\" && notify-send WiFi Connected to %s || notify-send WiFi Failed to connect"}}\n' "$ssid" "$signal" "$rate" "$ssid_escaped" "$ssid_escaped" "$ssid_escaped"
    else
        # Unknown network - ask for password, then connect
        printf '{"name":"  %s [%s%% %s]","id":"%s","action":{"selection_type":"input","as":"password","prompt":"Password for %s:\",\"sensitive\":true,\"template\":\"nmcli device wifi connect \\\"%s\\\" password \\\"{password}\\\" && notify-send WiFi Connected to %s || notify-send WiFi Failed to connect\"}}\n' "$ssid" "$signal" "$rate" "$ssid_escaped" "$ssid_escaped" "$ssid_escaped" "$ssid_escaped"
    fi
done | paste -sd, -)

# Wrap in array
if [ -n "$result" ]; then
    echo "[$result]"
else
    echo "[]"
fi
