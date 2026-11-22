"""Synthetic package/project tests, not evidence of Apple compilation or execution."""
import contextlib, io, plistlib, sys, tempfile
from pathlib import Path
from verify import ROOT, HEADERS, inspect, consumer_project, run
with tempfile.TemporaryDirectory(prefix='package test ') as tmp:
 xc=Path(tmp)/'relocated artifact/MadObsRuntime.xcframework';xc.mkdir(parents=True)
 entries=[]
 for identifier,variant in [('ios-arm64',None),('ios-arm64-simulator','simulator')]:
  entry={'LibraryIdentifier':identifier,'LibraryPath':'libmadobs_runtime.a','HeadersPath':'Headers','SupportedPlatform':'ios','SupportedArchitectures':['arm64']}
  if variant:entry['SupportedPlatformVariant']=variant
  entries.append(entry);base=xc/identifier;base.mkdir();(base/'libmadobs_runtime.a').write_bytes(b'!<arch>\n')
  for header in HEADERS:
   path=base/'Headers'/header;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('synthetic')
  (base/'Headers/module.modulemap').write_text('synthetic')
 info=xc/'Info.plist';info.write_bytes(plistlib.dumps({'AvailableLibraries':entries}))
 assert set(inspect(xc))=={'device','simulator'}
 def rejected():
  try:inspect(xc)
  except ValueError:return
  raise AssertionError('bad package accepted')
 entries[0]['LibraryPath']='../outside';info.write_bytes(plistlib.dumps({'AvailableLibraries':entries}));rejected()
 entries[0]['LibraryPath']='libmadobs_runtime.a';entries[0]['SupportedArchitectures']=['x86_64'];info.write_bytes(plistlib.dumps({'AvailableLibraries':entries}));rejected()
 entries[0]['SupportedArchitectures']=['arm64'];info.write_bytes(plistlib.dumps({'AvailableLibraries':entries}))
 (xc/'ios-arm64-simulator/Headers/madobs/runtime.h').write_text('different');rejected()
 consumer=Path(tmp)/'consumer';consumer.mkdir();consumer_project(consumer)
 text=(consumer/'MadObsValidation.xcodeproj/project.pbxproj').read_text()
 assert 'MADOBS_STAGE' not in text and 'MADOBS_ARCHIVE' in text and 'MADOBS_HEADERS' in text
 assert str(ROOT) not in text
 for path in ['App/main.m','Tests/APIOnly.c','Tests/NativeTests.mm']:
  assert (consumer/path).read_bytes()==(ROOT/'ios-validation/Consumer'/path).read_bytes()
 assert text.count('Arm64Guard.h')==2
 out=io.StringIO()
 with contextlib.redirect_stdout(out):
  try:run([sys.executable,'-c','print("SECRET PATH");raise SystemExit(4)'],'device-link')
  except RuntimeError:pass
  else:raise AssertionError('command failure accepted')
 assert 'SECRET' not in out.getvalue() and 'FAIL device-link' in out.getvalue()
print('PASS: synthetic slice validation, path/header negatives, unchanged fixtures, independent project and fixed errors')
