#!/bin/sh
# Run idf.py for the MeowBox firmware in a container (ESP-IDF pinned in Dockerfile). Linux / macOS / Git Bash.
#   firmware/docker/idf.sh                 build (default)
#   firmware/docker/idf.sh fullclean       any other idf.py arguments
# Engine: $MEOWBOX_DOCKER if set, else kiri if installed, else docker.
# Output goes to firmware/build-docker/ (separate from a native build/). Flash from the host: firmware/README.md.
set -e
here=$(cd "$(dirname "$0")" && pwd)
fw=$(dirname "$here")
engine=${MEOWBOX_DOCKER:-$(command -v kiri >/dev/null 2>&1 && echo kiri || echo docker)}

# (Re)build the image only when the Dockerfile changed: exporting the 4 GB image takes minutes even when cached
sum=$( (sha256sum "$here/Dockerfile" 2>/dev/null || shasum -a 256 "$here/Dockerfile") | cut -c1-12)
image="meowbox-idf:$sum"
"$engine" image inspect "$image" >/dev/null 2>&1 || "$engine" build -t "$image" "$here"

[ $# -gt 0 ] || set -- build
opts=""
[ -t 0 ] && [ -t 1 ] && opts="-it"
# On Linux, write the build as the calling user instead of root
[ "$(uname -s)" = Linux ] && opts="$opts --user $(id -u):$(id -g) -e HOME=/tmp"
# Mount the whole repo: the firmware includes ../hw. Git Bash: give the engine a Windows path (D:/...), and stop
# MSYS from rewriting /meowbox
src=$(cd "$fw/.." && { pwd -W 2>/dev/null || pwd; })
MSYS_NO_PATHCONV=1 exec "$engine" run --rm $opts -v "$src:/meowbox" -w /meowbox/firmware "$image" \
    idf.py -B build-docker -DSDKCONFIG=build-docker/sdkconfig "$@"
