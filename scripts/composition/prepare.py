#!/usr/bin/env python3
"""Copy a hash-locked fixed host fixture, never build from mutable input paths.
Hashes identify copied bytes, not source provenance or universal compatibility.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat

LIMIT = 256 * 1024 * 1024


def relative(name):
    p = PurePosixPath(name)
    if not name or name == '.' or p.is_absolute() or '..' in p.parts or str(p) != name:
        raise ValueError('unsafe relative manifest path')
    return Path(*p.parts)


def inventory(root):
    if root.is_symlink() or not root.is_dir():
        raise ValueError('input directory must be a real directory')
    found = set()
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs:
            if (Path(directory) / name).is_symlink():
                raise ValueError('symlink directory in input')
        for name in files:
            path = Path(directory) / name
            if path.is_symlink() or not path.is_file():
                raise ValueError('nonregular file in input')
            found.add(path.relative_to(root).as_posix())
    return found


def copy_checked(source, destination, expected):
    if not re.fullmatch('[0-9a-f]{64}', expected):
        raise ValueError('invalid SHA256 in manifest')
    # Read a bounded regular-file snapshot without following a final symlink.
    before = os.lstat(source)
    if not stat.S_ISREG(before.st_mode) or before.st_size > LIMIT:
        raise ValueError('nonregular/oversized input')
    fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > LIMIT:
            raise ValueError('nonregular/oversized input')
        data = stream.read(LIMIT + 1)
    if len(data) > LIMIT or hashlib.sha256(data).hexdigest() != expected:
        raise ValueError('input SHA256 mismatch: ' + str(source))
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Build uses exactly these checked bytes, not a second read of the source.
    with destination.open('xb') as stream:
        stream.write(data)


def copy_tree(source, destination, expected):
    for name in expected:
        relative(name)
    if inventory(source) != set(expected):
        raise ValueError('input file inventory mismatch: ' + str(source))
    for name, digest in sorted(expected.items()):
        p = relative(name)
        copy_checked(source / p, destination / p, digest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('lock', 'package', 'madlink-source', 'madlink-archive',
                 'madlink-core-archive', 'ssl-archive', 'sample', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    lock = json.loads(args.lock.read_text())
    if lock.get('format') != 1:
        raise ValueError('unsupported lock format')
    args.output.mkdir(mode=0o700)  # existing output is deliberately rejected
    copy_tree(args.package, args.output / 'package', lock['package_files'])
    for directory, expected in lock['header_directories'].items():
        d = relative(directory)
        copy_tree(args.madlink_source / d, args.output / 'madlink' / d, expected)
    archives = {'libimlink.a': args.madlink_archive,
                'libimlink_core.a': args.madlink_core_archive,
                'libssl.a': args.ssl_archive}
    if set(lock['archives']) != set(archives):
        raise ValueError('unexpected archive set')
    for name, source in archives.items():
        copy_checked(source, args.output / 'archives' / name, lock['archives'][name])
    copy_tree(args.sample, args.output / 'sample', lock['sample_files'])
    (args.output / 'verified-lock.json').write_text(json.dumps(lock, indent=2) + '\n')
    print('All package/header/archive/sample bytes verified and copied')


if __name__ == '__main__':
    main()
