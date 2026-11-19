"""Synthetic parser/negative tests, not cross-compiler or Android evidence."""
import contextlib, io, struct, sys, tempfile
from pathlib import Path
from verify import inspect_elf, run
with tempfile.TemporaryDirectory() as tmp:
 path=Path(tmp)/'fixture'
 def elf(align=16384,machine=183,kind=3):
  interp=b'/system/bin/linker64\0';total=176+len(interp)
  data=bytearray(total);data[:6]=b'\x7fELF\x02\x01'
  struct.pack_into('<HH',data,16,kind,machine);struct.pack_into('<Q',data,32,64)
  struct.pack_into('<HH',data,54,56,2);struct.pack_into('<IIQQQQQQ',data,64,1,5,0,0,0,total,total,align)
  struct.pack_into('<IIQQQQQQ',data,120,3,4,176,176,176,len(interp),len(interp),1)
  data[176:]=interp
  return data
 path.write_bytes(elf());inspect_elf(path)
 no_interp=elf();struct.pack_into('<I',no_interp,120,0)
 wrong_interp=elf();wrong_interp[177]=ord('X')
 for data in [elf(4096),elf(machine=62),elf(kind=1),b'invalid',elf()[:70],no_interp,wrong_interp]:
  path.write_bytes(data)
  try:inspect_elf(path)
  except ValueError:pass
  else:raise AssertionError('invalid ELF accepted')
 out=io.StringIO()
 with contextlib.redirect_stdout(out):
  try:run([sys.executable,'-c','import sys;print("SECRET RAW LOG");sys.exit(7)'],'consumer-build')
  except RuntimeError:pass
  else:raise AssertionError('failure accepted')
 assert 'SECRET' not in out.getvalue() and 'FAIL consumer-build' in out.getvalue()
 try:run([], 'unknown')
 except ValueError:pass
 else:raise AssertionError('unknown stage accepted')
print('PASS: ARM64 PIE/alignment negative cases and fixed-output subprocess failure')
