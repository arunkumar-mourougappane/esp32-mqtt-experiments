#!/usr/bin/env bash
# Start (or restart) the Mosquitto broker used by the Tab5 IMU app.
#   8883 -> MQTT over TLS + password, reachable from the LAN (the Tab5 connects here)
#   9001 -> MQTT over WebSockets, 127.0.0.1 only, read-only, no login (the browser viewer connects here)
# Run broker/setup-tls.sh once first.
set -euo pipefail

NAME=tab5-mqtt
IMAGE=docker.io/library/eclipse-mosquitto:2
DIR="$(cd "$(dirname "$0")" && pwd)"

command -v podman >/dev/null || { echo "podman not found (brew install podman)" >&2; exit 1; }

# macOS runs containers in a VM; start it if it isn't up.
podman info >/dev/null 2>&1 || podman machine start

[[ -f "$DIR/certs/server.crt" && -f "$DIR/passwd" ]] || { echo "run broker/setup-tls.sh first" >&2; exit 1; }

podman rm -f "$NAME" >/dev/null 2>&1 || true
podman run -d --name "$NAME" --restart unless-stopped \
  -p 8883:8883 \
  -p 127.0.0.1:9001:9001 \
  -v "$DIR/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro" \
  -v "$DIR/passwd:/mosquitto/config/passwd:ro" \
  -v "$DIR/acl.devices:/mosquitto/config/acl.devices:ro" \
  -v "$DIR/acl.viewer:/mosquitto/config/acl.viewer:ro" \
  -v "$DIR/certs:/mosquitto/certs:ro" \
  "$IMAGE" >/dev/null

echo "broker '$NAME' running"
echo "  LAN address: $(ipconfig getifaddr en0 2>/dev/null || echo '?')  (must match MQTT_HOST in include/secrets.h)"
echo "  watch:  podman exec $NAME mosquitto_sub -t tab5/imu"
echo "  viewer: open $DIR/../viewer/index.html"
