import struct,tempfile,unittest,zipfile
from pathlib import Path
from package_check import elf,apk,LIB

def sample():
    b=bytearray(128);b[:7]=b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HH',b,16,3,183);struct.pack_into('<Q',b,32,64);struct.pack_into('<HH',b,54,56,1)
    struct.pack_into('<IIQQQQQQ',b,64,1,5,0,0,0,128,128,16384);return bytes(b)

def package(path,data,align=True):
    with zipfile.ZipFile(path,'w') as z:
        info=zipfile.ZipInfo(LIB);info.compress_type=zipfile.ZIP_STORED
        if align:
            padding=16384-30-len(LIB.encode());info.extra=struct.pack('<HH',0xcafe,padding-4)+bytes(padding-4)
        z.writestr(info,data);z.writestr('AndroidManifest.xml',b'fixture only')
class Tests(unittest.TestCase):
    def test_good_structural_fixture(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)/'sample.apk';package(p,sample());apk(p,sample())
    def test_zip_and_payload_failures(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)/'sample.apk';package(p,sample(),False)
            with self.assertRaises(ValueError):apk(p,sample())
            package(p,sample())
            with self.assertRaises(ValueError):apk(p,b'wrong')
            with zipfile.ZipFile(p,'a') as z:z.writestr('classes.dex',b'forbidden')
            with self.assertRaises(ValueError):apk(p,sample())
    def test_elf_failures(self):
        for offset,fmt,value in [(18,'H',62),(112,'Q',4096),(80,'Q',1),(64,'I',3),(96,'Q',9999)]:
            b=bytearray(sample());struct.pack_into('<'+fmt,b,offset,value)
            with self.assertRaises(ValueError):elf(b)
    def test_fixture_no_managed_lane(self):
        root=Path(__file__).parent
        manifest=(root/'AndroidManifest.xml').read_text()
        self.assertIn('android:hasCode="false"',manifest);self.assertNotIn('uses-permission',manifest)
        activity=(root/'activity.c').read_text()
        self.assertNotIn('madobs_start(',activity);self.assertNotIn('madobs_create(',activity)
if __name__=='__main__':unittest.main()
