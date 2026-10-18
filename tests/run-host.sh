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
# The capture loop with a fake backend; needs libyuv (libyuv-dev)
# shellcheck disable=SC2086
gcc $FLAGS -Isrc -Iunicapture -Itests tests/test_unicapture.c tests/log_stub.c unicapture/unicapture.c \
    unicapture/converter.c src/utils.c -lyuv -lpthread -ldl -o "$OUT/test_unicapture"

# hyperion_client needs flatcc at the same commit as fbs/CMakeLists.txt, plus
# the verifier and reply builder for the fake receiver
FLATCC_REF=29201734bf2d12713a7a1a035d31e5123aac9c93
FLATCC_SRC="$OUT/flatcc"
if [ ! -x "$FLATCC_SRC/bin/flatcc" ]; then
    rm -rf "$FLATCC_SRC" && mkdir -p "$FLATCC_SRC"
    git -C "$FLATCC_SRC" init -q
    git -C "$FLATCC_SRC" fetch -q --depth 1 https://github.com/dvidelabs/flatcc.git "$FLATCC_REF"
    git -C "$FLATCC_SRC" checkout -q FETCH_HEAD
    cmake -S "$FLATCC_SRC" -B "$FLATCC_SRC/build" -DFLATCC_TEST=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$FLATCC_SRC/build" -j"$(nproc)" >/dev/null
fi
mkdir -p "$OUT/fbs"
"$FLATCC_SRC/bin/flatcc" -a -o "$OUT/fbs" fbs/hyperion_reply.fbs fbs/hyperion_request.fbs
RT=""
for f in builder emitter refmap verifier; do
    # Third-party runtime: sanitized, but not held to our warning flags
    # shellcheck disable=SC2086
    gcc ${FLAGS/-Werror/} -w -isystem "$FLATCC_SRC/include" -c "$FLATCC_SRC/src/runtime/$f.c" -o "$OUT/flatcc_$f.o"
    RT="$RT $OUT/flatcc_$f.o"
done
# shellcheck disable=SC2086
gcc $FLAGS -Isrc -Itests -isystem "$OUT/fbs" -isystem "$FLATCC_SRC/include" tests/test_hyperion_client.c tests/log_stub.c \
    src/hyperion_client.c $RT -lpthread -o "$OUT/test_hyperion_client"

status=0
for t in test_settings test_receiver test_lockdown test_hyperion_client test_unicapture; do
    echo "== $t"
    "$OUT/$t" || status=1
done
# Ownership changes need root; run that test again with sudo when available
if [ "$(id -u)" != 0 ] && sudo -n true 2>/dev/null; then
    echo "== test_lockdown (root)"
    sudo -n "$OUT/test_lockdown" || status=1
fi
exit $status
