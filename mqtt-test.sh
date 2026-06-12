#!/usr/bin/env bash
# MQTT broker diagnostic script
# Usage: MQTT_HOST=192.168.1.x MQTT_USER=mqtt_devices MQTT_PASS=yourpassword ./mqtt-test.sh

HOST="${MQTT_HOST:-192.168.1.x}"
PORT="${MQTT_PORT:-1883}"
USER="${MQTT_USER:-mqtt_devices}"
PASS="${MQTT_PASS:-yourpassword}"

echo "=== 1. TCP reachability ==="
if nc -zw3 "$HOST" "$PORT" 2>/dev/null; then
    echo "OK — port $PORT is open on $HOST"
else
    echo "FAIL — cannot reach $HOST:$PORT (broker down or firewall?)"
    exit 1
fi

echo ""
echo "=== 2. Publish test message ==="
if command -v mosquitto_pub &>/dev/null; then
    mosquitto_pub -h "$HOST" -p "$PORT" -u "$USER" -P "$PASS" \
        -t "test/mqtt-diag" -m "hello" -d
else
    echo "mosquitto_pub not found — install with: brew install mosquitto"
fi

echo ""
echo "=== 3. Subscribe (5 s) — watch for retained tank sensor topics ==="
if command -v mosquitto_sub &>/dev/null; then
    echo "(Ctrl-C to stop early)"
    mosquitto_sub -h "$HOST" -p "$PORT" -u "$USER" -P "$PASS" \
        -t "homeassistant/sensor/hw_#" -t "test/mqtt-diag" \
        -v --retained-only -W 5 || true
else
    echo "mosquitto_sub not found — install with: brew install mosquitto"
fi
