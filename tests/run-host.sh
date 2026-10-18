#!/bin/bash
# Builds and runs the tests natively (x86_64 or any Linux host) with ASan and
# UBSan. Needs gcc, glib and pbnjson_c visible to pkg-config; CI builds
# pbnjson from webosose/libpbnjson first.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${OUT:-/tmp/hyperion-webos-tests}
mkdir -p "$OUT"
FLAGS="-std=gnu11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
DEPS="$(pkg-config --cflags --libs pbnjson_c glib-2.0)"

# shellcheck disable=SC2086
gcc $FLAGS -Isrc -Itests tests/test_settings.c tests/log_stub.c src/settings.c $DEPS -o "$OUT/test_settings"
# shellcheck disable=SC2086
gcc $FLAGS -Isrc -Itests tests/test_receiver.c tests/log_stub.c src/receiver_test.c -o "$OUT/test_receiver"
# shellcheck disable=SC2086
gcc $FLAGS -Isrc -Itests tests/test_lockdown.c tests/log_stub.c src/lockdown.c -o "$OUT/test_lockdown"

status=0
for t in test_settings test_receiver test_lockdown; do
    echo "== $t"
    "$OUT/$t" || status=1
done
# Ownership changes need root; run that test again with sudo when available
if [ "$(id -u)" != 0 ] && sudo -n true 2>/dev/null; then
    echo "== test_lockdown (root)"
    sudo -n "$OUT/test_lockdown" || status=1
fi
exit $status
