"""Qualify two separate C apps from one relocated, null-provider CMake package."""
import contextlib
import hashlib
import os
from pathlib import Path
import select
import shlex
import shutil
import signal
import subprocess
import tempfile
import time


class Failure(Exception):
    """An allowlisted test stage failed."""


def check(condition, label):
    if not condition:
        raise Failure(label)


@contextlib.contextmanager
def child(argv, *, env, cwd, protocol=False):
    proc = subprocess.Popen(
        argv, env=env, cwd=cwd, start_new_session=True,
        stdin=subprocess.PIPE if protocol else subprocess.DEVNULL,
        stdout=subprocess.PIPE if protocol else subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        yield proc
    finally:
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        proc.wait(timeout=8)
        if protocol:
            proc.stdin.close()
            proc.stdout.close()


def run(argv, *, cwd, env, label, timeout=150):
    try:
        with child(argv, cwd=cwd, env=env) as proc:
            check(proc.wait(timeout=timeout) == 0, label)
    except (OSError, subprocess.TimeoutExpired):
        raise Failure(label) from None


def expect(proc, mark, label, timeout=20):
    """Read only two expected control bytes; never echo a child's stdout."""
    until = time.monotonic() + timeout
    for expected in (mark.encode("ascii"), b"\n"):
        remaining = max(0, until - time.monotonic())
        fd = proc.stdout.fileno()
        check(bool(select.select([fd], [], [], remaining)[0]), label)
        check(os.read(fd, 1) == expected, label)


def send(proc, mark, label):
    try:
        proc.stdin.write(mark.encode("ascii") + b"\n")
        proc.stdin.flush()
    except OSError:
        raise Failure(label) from None


def done(proc, label):
    try:
        check(proc.wait(timeout=10) == 0, label)
        fd = proc.stdout.fileno()
        check(bool(select.select([fd], [], [], 1)[0]), label)
        check(os.read(fd, 1) == b"", label)
    except subprocess.TimeoutExpired:
        raise Failure(label) from None


def verify_protocol(holder, contender, work, env):
    """Use only anonymous local pipes and fresh, owned filesystem directories."""
    shared = work / "a lease"
    own = work / "b independent"
    with child([holder, shared], cwd=work, env=env, protocol=True) as a:
        expect(a, "A", "holder-export-rejected")
        with child([contender, shared, own], cwd=work, env=env, protocol=True) as b:
            check(a.pid != b.pid, "process-independence")
            expect(b, "B", "peer-drain-and-busy")
            send(a, "S", "holder-stop")
            expect(a, "S", "holder-stop")
            send(b, "T", "lease-transfer")
            expect(b, "T", "lease-transfer-and-second-peer-drain")
            check(a.poll() is None, "holder-not-alive-at-transfer")
            done(b, "lease-peer-exit")
        send(a, "X", "holder-final-stats")
        expect(a, "X", "holder-final-stats")
        done(a, "holder-exit")


def package_fingerprint(prefix, producer):
    inventory = {}
    for p in sorted(prefix.rglob("*")):
        check(not p.is_symlink(), "installed-symlink")
        if p.is_dir():
            continue
        check(p.is_file(), "installed-entry-type")
        with p.open("rb") as f:
            inventory[str(p.relative_to(prefix))] = hashlib.file_digest(f, "sha256").digest()
        if p.suffix == ".cmake":
            check(str(producer) not in p.read_text(), "package-producer-path")
    expected = {
        "include/imobs/imobs_c.h", "include/madobs/observer.h", "include/madobs/privacy.h",
        "include/madobs/private.h", "include/madobs/private_runtime.hpp",
        "include/madobs/private_wal.hpp", "include/madobs/runtime.h", "lib/libmadobs_runtime.a",
        "lib/cmake/MadObs/MadObsApiTargets.cmake", "lib/cmake/MadObs/MadObsConfig.cmake",
        "lib/cmake/MadObs/MadObsConfigVersion.cmake", "lib/cmake/MadObs/MadObsRuntimeTargets.cmake",
        "lib/cmake/MadObs/MadObsRuntimeTargets-release.cmake",
    }
    check(set(inventory) == expected, "installed-inventory")
    return inventory


def main():
    fixture = Path(__file__).resolve().parent
    root = fixture.parents[1]
    os.umask(0o077)
    env = {k: os.environ[k] for k in ("PATH", "HOME", "TMPDIR") if k in os.environ}
    env["LC_ALL"] = "C"
    tools = dict((name, shutil.which(name)) for name in ("cmake", "make", "gcc", "g++"))
    check(all(tools.values()), "existing-toolchain")
    cmake = tools["cmake"]
    with tempfile.TemporaryDirectory(prefix="madobs-installed-lease-") as base:
        work = Path(base).resolve()
        prefix = work / "relocated installed MadObs"
        with tempfile.TemporaryDirectory(prefix="producer-", dir=work) as temp:
            producer = Path(temp)
            source = producer / "source"
            source.mkdir()
            for dirname in ("imobs", "include", "src", "cmake"):
                shutil.copytree(root / dirname, source / dirname)
            shutil.copyfile(root / "CMakeLists.txt", source / "CMakeLists.txt")
            build = producer / "build"
            install = producer / "install"
            options = [
                "-G", "Unix Makefiles", "-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_C_COMPILER=" + tools["gcc"], "-DCMAKE_CXX_COMPILER=" + tools["g++"],
                "-DCMAKE_MAKE_PROGRAM=" + tools["make"],
            ]
            run([cmake, "-S", source, "-B", build, *options, "-DBUILD_TESTING=OFF",
                 "-DMADOBS_BUILD_RUNTIME=ON", "-DMADOBS_BUILD_SHARED=OFF", "-DMADOBS_BUILD_JNI=OFF",
                 "-DMADOBS_PACKAGE_BORINGSSL=OFF", "-DIMOBS_BORINGSSL_ROOT=",
                 "-DIMOBS_BORINGSSL_INCLUDE=", "-DIMOBS_BORINGSSL_CRYPTO_LIB="],
                cwd=work, env=env, label="producer-configure")
            run([cmake, "--build", build, "--parallel", "2"],
                cwd=work, env=env, label="producer-build")
            run([cmake, "--install", build, "--prefix", install],
                cwd=work, env=env, label="producer-install")
            shutil.move(install, prefix)
        check(not producer.exists(), "producer-removal")
        before = package_fingerprint(prefix, producer)
        archive = prefix / "lib/libmadobs_runtime.a"
        binaries = []
        for name in ("holder", "contender"):
            app = work / f"app-{name}"
            shutil.copytree(fixture / name, app)
            shutil.copyfile(fixture / "fixture.h", app / "fixture.h")
            obj = work / f"obj-{name}"
            run([cmake, "-S", app, "-B", obj, *options,
                 "-DSDK_PREFIX=" + str(prefix), "-DMadObs_DIR=" + str(prefix / "lib/cmake/MadObs")],
                cwd=work, env=env, label=name + "-configure")
            actual = (obj / "runtime-path").read_text()
            check(Path(actual) == archive, name + "-package-selection")
            run([cmake, "--build", obj, "--parallel", "2"],
                cwd=work, env=env, label=name + "-build")
            link = (obj / "CMakeFiles" / f"{name}.dir" / "link.txt").read_text()
            check(shlex.split(link).count(str(archive)) == 1 and
                  str(producer) not in link and str(root) not in link, name + "-archive-link")
            binaries.append(obj / name)
        check(len(set(binaries)) == 2, "different-consumers")
        verify_protocol(*binaries, work, env)
        check(before == package_fingerprint(prefix, producer), "installed-bytes-changed")
    print("PASS: two installed C apps; exporter rejection isolation, fail-closed policy, graceful lease handoff")


if __name__ == "__main__":
    try:
        main()
    except Failure as err:
        raise SystemExit("FAIL: " + str(err)) from None
    except (OSError, UnicodeError, ValueError, subprocess.SubprocessError):
        raise SystemExit("FAIL: qualification infrastructure") from None
