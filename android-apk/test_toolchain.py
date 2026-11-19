import subprocess,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
from toolchain import properties,licenses,provision,cmake_version
class Tests(unittest.TestCase):
    def test_receipt_ambiguity(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'properties'
            p.write_text('Pkg.Revision = 2\nAndroidVersion.ApiLevel = 35\n')
            properties(p,{'Pkg.Revision':'2','AndroidVersion.ApiLevel':'35'})
            for text in ('Pkg.Revision=2\nPkg.Revision=2','Pkg.Revision=20','garbage','AndroidVersion.ApiLevel=35'):
                p.write_text(text)
                with self.assertRaises(ValueError):properties(p,{'Pkg.Revision':'2'})
    def test_cmake_suffix(self):
        for text in ('cmake version 3.22.1','cmake version 3.22.1-g123abc-dirty'):self.assertTrue(cmake_version(text))
        for text in ('cmake version 3.22.10','cmake version 3.22.1-custom','prefix cmake version 3.22.1'):self.assertFalse(cmake_version(text))
    def test_license_types(self):
        with tempfile.TemporaryDirectory() as tmp:
            sdk=Path(tmp);folder=sdk/'licenses';folder.mkdir();p=folder/'android-sdk-license'
            with self.assertRaises(ValueError):licenses(sdk)
            p.write_bytes(b'existing');self.assertIn('android-sdk-license',licenses(sdk))
            extra=folder/'link';extra.symlink_to(p)
            with self.assertRaises(ValueError):licenses(sdk)
            extra.unlink();extra.mkdir()
            with self.assertRaises(ValueError):licenses(sdk)
            extra.rmdir();p.write_bytes(b'')
            with self.assertRaises(ValueError):licenses(sdk)
    def test_failed_install_still_snapshots(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for effect in (subprocess.CompletedProcess([],1),subprocess.TimeoutExpired('sdkmanager',600)):
                with patch('toolchain.licenses',side_effect=[{'before':1},{'after':2}]) as snapshots, patch('toolchain.subprocess.run') as run:
                    if isinstance(effect,Exception):run.side_effect=effect
                    else:run.return_value=effect
                    with self.assertRaises(ValueError):provision(root,root/'log')
                    self.assertEqual(snapshots.call_count,2)
                    self.assertEqual(run.call_args.kwargs['stdin'],subprocess.DEVNULL)
                    self.assertEqual(run.call_args.kwargs['timeout'],600)
if __name__=='__main__':unittest.main()
