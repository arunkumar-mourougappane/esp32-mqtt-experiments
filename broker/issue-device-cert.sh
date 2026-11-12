#!/usr/bin/env bash
# Issue a device certificate for the Tab5 from the broker's private CA, for signing in with a certificate
# (mutual TLS, port 8884) instead of a username and password.
#
# Usage: broker/issue-device-cert.sh [name]          (default name: tab5-device)
#
# Writes, into broker/provision/ (git-ignored):
#   tab5.crt   the device certificate, signed by the CA in broker/certs/
#   tab5.key   its private key (PKCS#8, no passphrase, mode 600)
# The name becomes the certificate's common name, and the broker uses it as the MQTT username.
# Upload ca.crt, tab5.crt and tab5.key on the Tab5 setup page (choose "Device certificate").
# Run broker/setup-tls.sh first (it creates the CA). Re-running makes a new key and certificate.
set -euo pipefail
cd "$(dirname "$0")"
NAME="${1:-tab5-device}"
[[ "$NAME" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "the name may only contain letters, digits, . _ -" >&2; exit 1; }
C=certs; P=provision
[[ -f "$C/ca.key" && -f "$C/ca.crt" ]] || { echo "no CA yet: run broker/setup-tls.sh first" >&2; exit 1; }
mkdir -p "$P"

T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
openssl ecparam -name prime256v1 -genkey -noout -out "$T/key.sec1"
openssl pkcs8 -topk8 -nocrypt -in "$T/key.sec1" -out "$P/tab5.key"
chmod 600 "$P/tab5.key"
openssl req -new -key "$P/tab5.key" -subj "/CN=$NAME" -out "$T/dev.csr"
printf 'basicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nextendedKeyUsage=clientAuth\n' > "$T/ext"
openssl x509 -req -in "$T/dev.csr" -CA "$C/ca.crt" -CAkey "$C/ca.key" -CAcreateserial -days 825 -sha256 \
  -extfile "$T/ext" -out "$P/tab5.crt" 2>/dev/null
chmod 644 "$P/tab5.crt"
cp "$C/ca.crt" "$P/ca.crt"

openssl verify -CAfile "$C/ca.crt" "$P/tab5.crt" >/dev/null
echo "issued device certificate '$NAME' (valid 825 days)"
echo "  files for the Tab5 setup page: broker/provision/ca.crt  tab5.crt  tab5.key"
echo "  the broker's certificate port is 8884; restart the broker with broker/run.sh if you have not since this was added"
