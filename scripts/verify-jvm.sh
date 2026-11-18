#!/usr/bin/env bash
set -euo pipefail
: "${JAVA_HOME:?Point JAVA_HOME at a development JDK}"
root=$(cd "$(dirname "$0")/.." && pwd)
cmake=${CMAKE:-cmake}
ctest=${CTEST:-ctest}
work=$(mktemp -d "${TMPDIR:-/tmp}/madobs-jvm-check.XXXXXX")
echo "JVM verification artifacts: $work"
"$cmake" -S "$root" -B "$work/build" -DMADOBS_BUILD_JNI=ON -DCMAKE_BUILD_TYPE=Release
"$cmake" --build "$work/build" --parallel 4
"$ctest" --test-dir "$work/build" --output-on-failure
"$cmake" --install "$work/build" --prefix "$work/installed"
cp -a "$work/installed" "$work/relocated"
mkdir "$work/consumer"
"$JAVA_HOME/bin/javac" -Xlint:all -Werror -cp "$work/relocated/share/MadObs/java/madobs-jvm.jar" -d "$work/consumer" "$root/bindings/jvm/tests/JvmSmoke.java"
"$JAVA_HOME/bin/java" -Xcheck:jni "-Djava.library.path=$work/relocated/lib" -cp "$work/consumer:$work/relocated/share/MadObs/java/madobs-jvm.jar" JvmSmoke
python3 - "$work/relocated/lib/libmadobs_jni.so" <<'PY'
import subprocess,sys
names={line.split()[-1] for line in subprocess.check_output(['nm','-D','--defined-only',sys.argv[1]],text=True).splitlines()}
expected={'JNI_OnLoad'}|{'Java_org_madobs_PrivateRuntime_'+s for s in ('nOpen','nEmit','nClose','nInCallback','nFailures')}
assert names==expected,(names-expected,expected-names)
print('Exact JNI export surface passed')
PY
if "$cmake" -S "$root" -B "$work/invalid" -DMADOBS_BUILD_RUNTIME=OFF -DMADOBS_BUILD_JNI=ON; then
  echo 'JNI without runtime unexpectedly configured' >&2; exit 1
fi
echo 'Host-JVM build/tests/installed consumer/exports and negative configuration passed'
