#!/usr/bin/env bash
# Runs the TLS client tests in tests/tls/ against local `openssl s_server`
# instances with freshly generated self-signed certificates. Not part of
# `make test` (it needs the `openssl` binary and free ports 55443/55444).
# Usage: `make test-tls`.
#
# Server A (127.0.0.1:55443) has a certificate valid for localhost and
# 127.0.0.1; server B (127.0.0.1:55444) has a different one, valid only
# for other.example.test. Each certificate is its own CA, which is how the
# tests exercise a private CA, an unknown CA, and a host-name mismatch.
set -uo pipefail
cd "$(dirname "$0")/.."

BIN=./ziman
CERTS=tests/tls/.certs
if ! command -v openssl >/dev/null; then echo "SKIP: no openssl binary"; exit 0; fi
for port in 55443 55444; do
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
        echo "port $port is already in use; refusing to run" >&2; exit 1
    fi
done

rm -rf "$CERTS"; mkdir -p "$CERTS"
mk() { # name san
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 \
        -subj "/CN=$1" -addext "subjectAltName=$2" -keyout "$CERTS/$1.key" -out "$CERTS/$1.pem" 2>/dev/null
}
mk a "DNS:localhost,IP:127.0.0.1"
mk b "DNS:other.example.test"

pids=()
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done; rm -rf "$CERTS"; }
trap cleanup EXIT

openssl s_server -accept 55443 -cert "$CERTS/a.pem" -key "$CERTS/a.key" -www >/dev/null 2>&1 & pids+=($!)
openssl s_server -accept 55444 -cert "$CERTS/b.pem" -key "$CERTS/b.key" -www >/dev/null 2>&1 & pids+=($!)
for port in 55443 55444; do
    for _ in $(seq 1 50); do (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && break; sleep 0.1; done
done

pass=0; fail=0
for zm in tests/tls/*.zm; do
    name=$(basename "$zm" .zm)
    actual=$(timeout 60 "$BIN" "$zm" 2>&1)
    if [ "$actual" = "$(cat tests/tls/$name.out)" ]; then
        echo "PASS $name"; pass=$((pass + 1))
    else
        echo "FAIL $name"; diff <(echo "$actual") tests/tls/$name.out | sed 's/^/  /'; fail=$((fail + 1))
    fi
done
echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
