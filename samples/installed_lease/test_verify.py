"""Offline tests of bounded IPC, cleanup and installed metadata guards."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("verify", Path(__file__).with_name("verify.py"))
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class Guards(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="madobs-lease-mock-")
        self.addCleanup(self.tmp.cleanup)
        self.cwd = Path(self.tmp.name)
        self.env = {"PATH": os.environ["PATH"]}

    def start(self, snippet):
        return verify.child([sys.executable, "-I", "-B", "-c", snippet], cwd=self.cwd,
                            env=self.env, protocol=True)

    def test_exact_round_trip(self):
        source = 'import sys; print("A",flush=True); assert input()=="S"; print("S",flush=True)'
        with self.start(source) as proc:
            verify.expect(proc, "A", "marker", 3)
            verify.send(proc, "S", "control")
            verify.expect(proc, "S", "marker", 3)
            verify.done(proc, "exit")
        self.assertEqual(proc.returncode, 0)

    def test_unexpected_marker_and_timeout_reap(self):
        for prefix in ('print("Z",flush=True); ', ''):
            with self.subTest(prefix=bool(prefix)):
                with self.assertRaisesRegex(verify.Failure, "^marker$"):
                    with self.start("import time; " + prefix + "time.sleep(60)") as proc:
                        verify.expect(proc, "A", "marker", 0.5)
                self.assertIsNotNone(proc.poll())

    def test_extra_output_does_not_pass(self):
        with self.start('print("A"); print("unexpected")') as proc:
            verify.expect(proc, "A", "marker", 3)
            with self.assertRaisesRegex(verify.Failure, "^exit$"):
                verify.done(proc, "exit")

    def test_inventory_and_symlinks_fail_closed(self):
        sdk = self.cwd / "sdk"
        sdk.mkdir()
        with self.assertRaisesRegex(verify.Failure, "^installed-inventory$"):
            verify.package_fingerprint(sdk, self.cwd / "producer")
        (sdk / "link").symlink_to("missing")
        with self.assertRaisesRegex(verify.Failure, "^installed-symlink$"):
            verify.package_fingerprint(sdk, self.cwd / "producer")

    def test_exported_producer_path_rejected(self):
        sdk = self.cwd / "sdk"
        sdk.mkdir()
        producer = self.cwd / "producer"
        (sdk / "bad.cmake").write_text(str(producer))
        with self.assertRaisesRegex(verify.Failure, "^package-producer-path$"):
            verify.package_fingerprint(sdk, producer)


if __name__ == "__main__":
    unittest.main()
