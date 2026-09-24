#!/usr/bin/env bash
# Runs the MySQL driver tests in tests/mysql/ against a throwaway MySQL 8.0
# server in a container (docker or podman). Not part of `make test`: it needs
# a container runtime and a `mysql:8.0` image, so the ordinary suite stays
# hermetic. Usage: `make test-mysql`.
#
# The container is named ziman-mysql-test-<pid>, listens only on
# 127.0.0.1:55433, and is removed on exit -- it never touches any other MySQL.
# Users: `root` (caching_sha2_password, the 8.0 default), `sha2_user`
# (caching_sha2_password) and `nat_user` (mysql_native_password), so both
# authentication plugins and caching_sha2's TLS-only full exchange are
# exercised; MySQL 8 generates a self-signed certificate at first start, so
# every `sslmode` is too.
set -uo pipefail
cd "$(dirname "$0")/.."

BIN=${BIN:-./ziman}
PORT=55434
NAME="ziman-mysql-test-$$"
RUNTIME=$(command -v docker || command -v podman || true)
if [ -z "$RUNTIME" ]; then
    echo "SKIP: no container runtime (docker/podman) found"
    exit 0
fi
if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
    echo "port $PORT is already in use; refusing to run against something else" >&2
    exit 1
fi

cleanup() { "$RUNTIME" rm -f "$NAME" >/dev/null 2>&1; rm -f tests/mysql/.hostport; }
trap cleanup EXIT

"$RUNTIME" run -d --name "$NAME" -e MYSQL_ROOT_PASSWORD=secret -e MYSQL_DATABASE=zimandb \
    -p "127.0.0.1:$PORT:3306" mysql:8.0 >/dev/null || exit 1

mysqlx() { "$RUNTIME" exec "$NAME" mysql -h127.0.0.1 -uroot -psecret "$@" 2>&1 | grep -v "Using a password"; }
# The image restarts the server once after initialising, so wait for it to
# answer over TCP twice in a row.
ok=0
for _ in $(seq 1 240); do
    if "$RUNTIME" exec "$NAME" mysql -h127.0.0.1 -uroot -psecret -e "select 1" >/dev/null 2>&1; then
        ok=$((ok + 1)); [ "$ok" -ge 2 ] && break
    else
        ok=0
    fi
    sleep 1
done
[ "$ok" -ge 2 ] || { echo "mysql did not come up" >&2; exit 1; }

mysqlx -e "CREATE USER 'nat_user'@'%' IDENTIFIED WITH mysql_native_password BY 'natpw';
           CREATE USER 'sha2_user'@'%' IDENTIFIED WITH caching_sha2_password BY 'sha2pw';
           GRANT ALL ON zimandb.* TO 'nat_user'@'%', 'sha2_user'@'%';" >/dev/null

echo "127.0.0.1:$PORT" > tests/mysql/.hostport

pass=0; fail=0
# mysql_auth first: it relies on sha2_user's login not being cached yet.
for zm in tests/mysql/mysql_auth.zm $(ls tests/mysql/*.zm | grep -v mysql_auth.zm); do
    name=$(basename "$zm" .zm)
    actual=$(timeout 90 "$BIN" "$zm" 2>&1)
    # UPDATE=1 rewrites the expected output from this (fresh-server) run.
    [ -n "${UPDATE:-}" ] && echo "$actual" > "tests/mysql/$name.out"
    if [ "$actual" = "$(cat tests/mysql/$name.out)" ]; then
        echo "PASS $name"; pass=$((pass + 1))
    else
        echo "FAIL $name"; diff <(echo "$actual") tests/mysql/$name.out | sed 's/^/  /'; fail=$((fail + 1))
    fi
done
echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
