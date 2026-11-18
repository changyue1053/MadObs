#!/usr/bin/env python3
"""Fast synthetic input-integrity tests; no third-party binaries required."""
import hashlib
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('prepare', Path(__file__).with_name('prepare.py'))
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class Integrity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='madobs-composition-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        (self.source / 'a').write_bytes(b'original bytes')
        self.digest = hashlib.sha256(b'original bytes').hexdigest()

    def test_snapshot_is_independent(self):
        output = self.root / 'out'
        prepare.copy_tree(self.source, output, {'a': self.digest})
        (self.source / 'a').write_bytes(b'later mutable input')
        self.assertEqual((output / 'a').read_bytes(), b'original bytes')

    def test_wrong_hash_writes_no_destination(self):
        output = self.root / 'out'
        with self.assertRaises(ValueError):
            prepare.copy_checked(self.source / 'a', output, '0' * 64)
        self.assertFalse(output.exists())

    def test_extra_file_rejected(self):
        (self.source / 'unexpected.cmake').write_text('untrusted extra')
        with self.assertRaises(ValueError):
            prepare.copy_tree(self.source, self.root / 'out', {'a': self.digest})

    def test_missing_file_rejected(self):
        with self.assertRaises(ValueError):
            prepare.copy_tree(self.source, self.root / 'out', {'missing': self.digest})

    def test_symlink_file_rejected(self):
        link = self.source / 'link'
        link.symlink_to(self.source / 'a')
        with self.assertRaises(ValueError):
            prepare.inventory(self.source)
        with self.assertRaises((OSError, ValueError)):
            prepare.copy_checked(link, self.root / 'out', self.digest)

    def test_symlink_directory_rejected(self):
        (self.source / 'linked').symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ValueError):
            prepare.inventory(self.source)

    def test_relative_path_bounds(self):
        for name in ('../out', '/absolute', 'a/../b', './a', '', '.', 'a//b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                prepare.relative(name)

    def test_no_overwrite(self):
        output = self.root / 'out'
        output.write_bytes(b'keep')
        with self.assertRaises(FileExistsError):
            prepare.copy_checked(self.source / 'a', output, self.digest)
        self.assertEqual(output.read_bytes(), b'keep')

    def test_sparse_oversize_rejected_before_read(self):
        with (self.source / 'huge').open('wb') as stream:
            stream.truncate(prepare.LIMIT + 1)
        with self.assertRaises(ValueError):
            prepare.copy_checked(self.source / 'huge', self.root / 'out', self.digest)

    def test_fifo_does_not_block(self):
        fifo = self.source / 'pipe'
        os.mkfifo(fifo)
        with self.assertRaises(ValueError):
            prepare.copy_checked(fifo, self.root / 'out', self.digest)


if __name__ == '__main__':
    unittest.main()
