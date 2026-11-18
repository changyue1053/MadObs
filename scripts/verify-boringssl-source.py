#!/usr/bin/env python3
"""Offline source verification. Reads only; does not extract or download archives."""
import argparse
import hashlib
import json
import pathlib
import tarfile

parser = argparse.ArgumentParser()
parser.add_argument('--archive', required=True)
parser.add_argument('--source', required=True)
args = parser.parse_args()
lock = json.loads((pathlib.Path(__file__).resolve().parents[1] / 'deps/boringssl.lock.json').read_text())
archive = pathlib.Path(args.archive)
source = pathlib.Path(args.source).resolve()
if hashlib.sha256(archive.read_bytes()).hexdigest() != lock['archive_sha256']:
    raise SystemExit('BoringSSL archive checksum mismatch')
count = 0
expected = set()
with tarfile.open(archive) as tar:
    for item in tar:
        path = pathlib.PurePosixPath(item.name)
        if path.is_absolute() or '..' in path.parts or not path.parts or path.parts[0] != 'boringssl-' + lock['commit']:
            raise SystemExit('Unsafe/unexpected archive path')
        if item.issym() or item.islnk():
            raise SystemExit('Archive links are not supported')
        if not item.isfile():
            continue
        relative = pathlib.Path(*path.parts[1:])
        local = source / relative
        if local.is_symlink() or not local.is_file() or not local.resolve().is_relative_to(source):
            raise SystemExit('Missing or unsafe source file: ' + str(relative))
        if hashlib.sha256(local.read_bytes()).digest() != hashlib.sha256(tar.extractfile(item).read()).digest():
            raise SystemExit('Source content mismatch: ' + str(relative))
        expected.add(relative.as_posix())
        count += 1
# Out-of-tree builds required. Extra inputs could alter compilation unnoticed.
actual = {p.relative_to(source).as_posix() for p in source.rglob('*') if p.is_file() or p.is_symlink()}
if actual != expected:
    raise SystemExit('Unexpected extra source files; use pristine out-of-tree source')
print(json.dumps({'source_commit': lock['commit'], 'archive_sha256': lock['archive_sha256'], 'verified_files': count}))
