#!/usr/bin/env bash
# Runs the Postgres driver tests in tests/pg/ against a throwaway
# Postgres server in a container (docker or podman). Not part of `make
# test`: it needs a container runtime and a `postgres:17` image, so the
# ordinary suite stays hermetic. Usage: `make test-pg`.
#
# The container is named ziman-pg-test-<pid>, listens only on
# 127.0.0.1:55432, and is removed on exit -- it never touches any other
# Postgres. Roles: `ziman` (SCRAM-SHA-256, the default), `trust_user`
# (trust) and `clear_user` (cleartext password), so every authentication
# method the driver supports is exercised, and the server has TLS enabled
# with a freshly generated self-signed certificate (valid for localhost and
# 127.0.0.1) so every `sslmode` is too. Needs the `openssl` binary.
set -uo pipefail
cd "$(dirname "$0")/.."

BIN=${BIN:-./ziman}
PORT=55432
NAME="ziman-pg-test-$$"
RUNTIME=$(command -v docker || command -v podman || true)
if [ -z "$RUNTIME" ]; then
    echo "SKIP: no container runtime (docker/podman) found"
    exit 0
fi
if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
    echo "port $PORT is already in use; refusing to run against something else" >&2
    exit 1
fi

cleanup() { "$RUNTIME" rm -f "$NAME" >/dev/null 2>&1; rm -rf tests/pg/.url tests/pg/.certs; }
trap cleanup EXIT

"$RUNTIME" run -d --name "$NAME" -e POSTGRES_USER=ziman -e POSTGRES_PASSWORD=secret \
    -e POSTGRES_DB=zimandb -p "127.0.0.1:$PORT:5432" postgres:17 >/dev/null || exit 1

psqlx() { "$RUNTIME" exec -e PGPASSWORD=secret "$NAME" psql -h 127.0.0.1 -U ziman zimandb -q "$@"; }
for _ in $(seq 1 60); do psqlx -c "select 1" >/dev/null 2>&1 && break; sleep 1; done
psqlx -c "select 1" >/dev/null 2>&1 || { echo "postgres did not come up" >&2; exit 1; }

psqlx -c "CREATE ROLE trust_user LOGIN;" -c "CREATE ROLE clear_user LOGIN PASSWORD 'clearpw';" \
      -c "GRANT ALL ON DATABASE zimandb TO trust_user, clear_user;" \
      -c "GRANT ALL ON SCHEMA public TO trust_user, clear_user;"
"$RUNTIME" exec "$NAME" bash -c "sed -i '1i host zimandb trust_user all trust\nhost zimandb clear_user all password' /var/lib/postgresql/data/pg_hba.conf"
psqlx -c "SELECT pg_reload_conf();" >/dev/null

# TLS: a self-signed server certificate (its own CA), enabled with a config reload.
command -v openssl >/dev/null || { echo "openssl is required for the TLS tests" >&2; exit 1; }
mkdir -p tests/pg/.certs
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" -keyout tests/pg/.certs/server.key -out tests/pg/.certs/server.crt 2>/dev/null
"$RUNTIME" cp tests/pg/.certs/server.crt "$NAME:/tmp/server.crt"
"$RUNTIME" cp tests/pg/.certs/server.key "$NAME:/tmp/server.key"
"$RUNTIME" exec -u root "$NAME" bash -c "install -o postgres -g postgres -m 644 /tmp/server.crt /var/lib/postgresql/ssl.crt && install -o postgres -g postgres -m 600 /tmp/server.key /var/lib/postgresql/ssl.key"
psqlx -c "ALTER SYSTEM SET ssl = on;" -c "ALTER SYSTEM SET ssl_cert_file = '/var/lib/postgresql/ssl.crt';" \
      -c "ALTER SYSTEM SET ssl_key_file = '/var/lib/postgresql/ssl.key';" -c "SELECT pg_reload_conf();" >/dev/null
sleep 1

echo "postgres://ziman:secret@127.0.0.1:$PORT/zimandb" > tests/pg/.url

pass=0; fail=0
for zm in tests/pg/*.zm; do
    name=$(basename "$zm" .zm)
    actual=$(timeout 60 "$BIN" "$zm" 2>&1)
    if [ "$actual" = "$(cat tests/pg/$name.out)" ]; then
        echo "PASS $name"; pass=$((pass + 1))
    else
        echo "FAIL $name"; diff <(echo "$actual") tests/pg/$name.out | sed 's/^/  /'; fail=$((fail + 1))
    fi
done
echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
