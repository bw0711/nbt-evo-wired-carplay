#!/bin/sh
# Fetch the third-party sources the build needs, at the exact commits it was made and tested with:
#   upstream/nbt-android-auto       JayJay3071/nbt-android-auto 78b098e (the Mbed TLS x509 patch scripts, and the
#                                   reference for nbt-carplay-evo/ and probe-evo/)
#   nbt-carplay-evo/.cache/lwip-STABLE-2_2_1_RELEASE   lwIP 2.2.1 (77dcd25a)
#   nbt-carplay-evo/.cache/monocypher-4.0.2            Monocypher 4.0.2 (0d85f98c)
# Mbed TLS 3.6.7 is downloaded (SHA-256 checked) by probe-evo/build.sh itself.
# Existing checkouts at the right commit are kept; a checkout at another commit stops the script.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)

get() {   # url dir commit
    if [ -d "$2/.git" ]; then
        [ "$(git -C "$2" rev-parse HEAD)" = "$3" ] || { echo "$2 is at $(git -C "$2" rev-parse --short HEAD), not ${3%"${3#????????}"}: move it away first" >&2; exit 1; }
        echo "ok   $2"
        return
    fi
    mkdir -p "$(dirname "$2")"
    git clone -q "$1" "$2"
    git -C "$2" -c advice.detachedHead=false checkout -q "$3"
    echo "got  $2"
}

get https://github.com/JayJay3071/nbt-android-auto.git "$ROOT/upstream/nbt-android-auto" 78b098e1cffc3da5324a2b73870776c87850a5b4
get https://github.com/lwip-tcpip/lwip.git "$ROOT/nbt-carplay-evo/.cache/lwip-STABLE-2_2_1_RELEASE" 77dcd25a72509eb83f72b033d219b1d40cd8eb95
get https://github.com/LoupVaillant/Monocypher.git "$ROOT/nbt-carplay-evo/.cache/monocypher-4.0.2" 0d85f98c9d9b0227e42cf795cb527dff372b40a4
