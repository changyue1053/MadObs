"""Fail-closed official receipt and existing-license checks; no raw diagnostics."""
import hashlib
import re
import stat
import subprocess
import sys
from pathlib import Path

PACKAGES=('ndk;28.2.13676358','cmake;3.22.1','build-tools;35.0.0','platforms;android-35')

def properties(path, required):
    found={}
    for raw in path.read_text(encoding='utf-8').splitlines():
        line=raw.strip()
        if not line or line.startswith('#'):continue
        if '=' not in line:raise ValueError('malformed receipt')
        key,value=(x.strip() for x in line.split('=',1))
        if not key or key in found:raise ValueError('ambiguous receipt')
        found[key]=value
    for key,value in required.items():
        if found.get(key)!=value:raise ValueError('receipt mismatch')

def receipts(sdk):
    for relative,required in [
      ('ndk/28.2.13676358',{'Pkg.Revision':'28.2.13676358'}),
      ('cmake/3.22.1',{'Pkg.Revision':'3.22.1'}),
      ('build-tools/35.0.0',{'Pkg.Revision':'35.0.0'}),
      ('platforms/android-35',{'Pkg.Revision':'2','AndroidVersion.ApiLevel':'35'})]:
        properties(sdk/relative/'source.properties',required)

def cmake_version(text):
    lines=text.splitlines()
    return bool(lines and re.fullmatch(r'cmake version 3\.22\.1(?:-g[0-9a-f]+(?:-dirty)?)?',lines[0]))

def licenses(sdk):
    directory=sdk/'licenses'
    if not stat.S_ISDIR(directory.lstat().st_mode):raise ValueError('license directory type')
    snapshot={}
    for path in directory.iterdir():
        info=path.lstat()
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= 1024*1024:raise ValueError('license type/size')
        data=path.read_bytes()
        if len(data)!=info.st_size:raise ValueError('license changed')
        snapshot[path.name]={'type':'regular','sha256':hashlib.sha256(data).hexdigest()}
    if 'android-sdk-license' not in snapshot:raise ValueError('required license absent')
    return snapshot

def provision(sdk, log):
    before=licenses(sdk)
    success=False
    try:
        with log.open('wb') as stream:
            result=subprocess.run([str(sdk/'cmdline-tools/latest/bin/sdkmanager'),'--install',*PACKAGES],stdin=subprocess.DEVNULL,stdout=stream,stderr=stream,timeout=600)
            success=result.returncode==0
    except (OSError,subprocess.SubprocessError):pass
    # Always inspect after return, failure, timeout or startup error. No repairs.
    after=licenses(sdk)
    if before!=after or not success:raise ValueError('provision or license mutation')
    receipts(sdk)

if __name__=='__main__':
    try:
        if len(sys.argv)!=3:raise ValueError('arguments')
        provision(Path(sys.argv[1]),Path(sys.argv[2]))
    except Exception:
        print('FAIL: existing official SDK preparation');raise SystemExit(1) from None
    print('PASS: exact official SDK receipts and unchanged existing licenses')
