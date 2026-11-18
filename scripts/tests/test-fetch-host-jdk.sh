#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/madobs-jdk-negative.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
mkdir "$work/bin" "$work/existing"
cat > "$work/bin/curl" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
while [[ $# -gt 0 ]]; do
  if [[ $1 == -o ]]; then printf 'not the pinned JDK archive\n' > "$2"; exit 0; fi
  shift
done
exit 1
SH
chmod +x "$work/bin/curl"
if PATH="$work/bin:$PATH" "$root/scripts/fetch-host-jdk.sh" "$work/new" >"$work/corrupt.log" 2>&1; then
  echo 'Corrupt download unexpectedly accepted' >&2; exit 1
fi
grep -q 'FAILED' "$work/corrupt.log"
test ! -e "$work/new/jdk-21.0.13+9"
if "$root/scripts/fetch-host-jdk.sh" "$work/existing" >"$work/existing.log" 2>&1; then
  echo 'Existing directory unexpectedly accepted' >&2; exit 1
fi
echo 'Corrupt download rejected before extraction; existing destination rejected'
