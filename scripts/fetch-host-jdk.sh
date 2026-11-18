#!/usr/bin/env bash
# CI/development tool only. No JDK binary is included in MadObs packages.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
if [[ $# != 1 || $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
  echo 'Usage: fetch-host-jdk.sh NEW_DIRECTORY (Linux x86_64 only)' >&2; exit 2
fi
dest=$1
# Reject existing paths rather than trusting a stale/prepopulated installation.
mkdir -- "$dest"
url=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["archive_url"])' "$root/deps/temurin-host-jdk.lock.json")
sha=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["archive_sha256"])' "$root/deps/temurin-host-jdk.lock.json")
curl --fail --location --retry 2 --max-time 180 "$url" -o "$dest/archive.tar.gz"
printf '%s  %s\n' "$sha" "$dest/archive.tar.gz" | sha256sum --check --strict
python3 - "$dest/archive.tar.gz" "$dest" <<'PY'
import sys,tarfile
with tarfile.open(sys.argv[1]) as archive:
    archive.extractall(sys.argv[2], filter='data')
PY
echo "JAVA_HOME=$dest/jdk-21.0.13+9"
