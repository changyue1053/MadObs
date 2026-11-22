"""Portable structure/result checks only, no Apple or runtime execution."""
from pathlib import Path
import tempfile
import unittest
import verify

NAME = 'testInstalledIndependentRuntimePrivacyAndStop'
TEST = 'MadObsNativeTests/'

class Isolation(unittest.TestCase):
    def test_default_sources_unchanged_and_new_method_added(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            old, new = root/'old', root/'new'
            old.mkdir(); new.mkdir()
            verify.consumer_project(old)
            verify.consumer_project(new, True)
            file = Path('Tests/NativeTests.mm')
            original = (verify.ROOT/'ios-validation/Consumer'/file).read_bytes()
            self.assertEqual((old/file).read_bytes(), original)
            self.assertIn(original.removesuffix(b'\n@end\n'), (new/file).read_bytes())
            self.assertEqual((new/file).read_text().count('- (void)'+NAME), 1)
            project = Path('MadObsValidation.xcodeproj/project.pbxproj')
            self.assertEqual((old/project).read_bytes(), (new/project).read_bytes())

    def test_exact_three_requires_new_named_result(self):
        gate = verify.load(verify.ROOT/'scripts/check-ios-validation-results.py', 'gate')
        names = gate.EXPECTED+(NAME,)
        summary = dict(totalTestCount=3, passedTests=3, failedTests=0, skippedTests=0)
        nodes = [dict(nodeType='Test Case', nodeIdentifier=TEST+n, result='Passed') for n in names]
        gate.verify(summary, dict(testNodes=nodes), names)
        for broken in [nodes[:-1], nodes+[nodes[-1]], nodes[:-1]+[dict(nodes[-1], result='Skipped')]]:
            with self.assertRaises(gate.ResultRejected):
                gate.verify(summary, dict(testNodes=broken), names)
        with self.assertRaises(gate.ResultRejected):
            gate.verify(summary, dict(testNodes=nodes))

if __name__ == '__main__':
    unittest.main()
