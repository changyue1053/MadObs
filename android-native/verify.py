"""Local build/link only. No APK, JNI, device, runtime execution or publication."""
import argparse, os, platform, re, shutil, struct, subprocess, tarfile, tempfile
from contextlib import contextmanager
from pathlib import Path
NDK='28.2.13676358'
STAGES={'installed-config','host-preflight','pinned-tools','ndk-revision','cmake-version-parse','sysroot-directory','zlib-library','zlib-header','workspace-base','workspace-temp','producer-temp','producer-source','cmake-version','source-archive','producer-configure','producer-build','producer-install','consumer-configure','consumer-build','elf-inspection'}
def run(args,stage,capture=False):
 if stage not in STAGES:raise ValueError('stage')
 print('ANDROID NATIVE START '+stage,flush=True)
 with tempfile.TemporaryFile() as log:
  try:subprocess.run([str(x) for x in args],stdout=log,stderr=log,check=True,timeout=600)
  except (OSError,subprocess.SubprocessError):
   print('ANDROID NATIVE FAIL '+stage,flush=True)
   if stage=='consumer-configure':
    log.seek(0,2);log.seek(max(0,log.tell()-131072))
    print('ANDROID NATIVE CLASS '+configure_class(log.read(131072).decode('utf-8',errors='replace')),flush=True)
   raise RuntimeError('tool failure') from None
  if capture:
   log.seek(0);data=log.read(65537)
   if len(data)>65536:raise ValueError('output bound')
   result=data.decode('utf-8')
  else:result=None
 print('ANDROID NATIVE PASS '+stage,flush=True)
 return result

def configure_class(text):
 # Emit exactly one fixed class; ambiguous or unrecognized errors stay other.
 if not re.search(r'^CMake Error',text,re.M):return 'configure-other'
 if re.search(r'(?m)^\s*MADOBS_EXACT_CONFIG_(?:REQUIRED|MISMATCH)\s*$',text):return 'configure-exact-config'
 matches=[]
 if re.search(r'Could NOT find ZLIB|Could not find a package configuration file provided by "ZLIB"',text):matches.append('dependency')
 if re.search(r'Could not find a package configuration file provided by "MadObs"|Could not find a configuration file for package "MadObs"|Could not find a package configuration file provided by\s+"MadObs"',text):matches.append('package')
 if re.search(r'is not able to compile a simple test program|Failed to configure test project build system|CMAKE_C_COMPILER.*not a full path',text):matches.append('try-compile')
 return 'configure-'+(matches[0] if len(matches)==1 else 'other')

def installed_config(prefix):
 directory=prefix/'lib/cmake/MadObs'
 with check('installed-config'):
  for name in ['MadObsConfig.cmake','MadObsConfigVersion.cmake','MadObsApiTargets.cmake','MadObsRuntimeTargets.cmake']:
   if not (directory/name).is_file():raise ValueError('installed package config incomplete')
 return directory

@contextmanager
def check(stage):
 if stage not in STAGES:raise ValueError('stage')
 print('ANDROID NATIVE START '+stage,flush=True)
 try:yield
 except Exception:
  print('ANDROID NATIVE FAIL '+stage,flush=True);raise
 print('ANDROID NATIVE PASS '+stage,flush=True)

def cmake_version_ok(text):
 # Exact release tuple; tolerate CRLF and CMake's git build suffix, not 3.22.10.
 first=text.splitlines()[0] if text else ''
 return re.fullmatch(r'cmake version 3\.22\.1(?:-g[0-9a-f]+(?:-dirty)?)?',first) is not None

def target_zlib(ndk):
 sysroot=ndk/'toolchains/llvm/prebuilt/linux-x86_64/sysroot'
 with check('sysroot-directory'):
  if not sysroot.is_dir():raise ValueError('sysroot absent')
 zlib=sysroot/'usr/lib/aarch64-linux-android/24/libz.so';headers=sysroot/'usr/include'
 with check('zlib-library'):
  if not zlib.is_file():raise ValueError('target library absent')
 with check('zlib-header'):
  if not (headers/'zlib.h').is_file():raise ValueError('target header absent')
 return zlib,headers

def inspect_elf(path):
 data=path.read_bytes()
 if len(data)<64 or data[:6]!=b'\x7fELF\x02\x01':raise ValueError('ELF64 little-endian required')
 kind,machine=struct.unpack_from('<HH',data,16)
 if kind!=3 or machine!=183:raise ValueError('ARM64 PIE required')
 phoff=struct.unpack_from('<Q',data,32)[0];size,count=struct.unpack_from('<HH',data,54)
 if size!=56 or count==0 or phoff+size*count>len(data):raise ValueError('program headers')
 loads=0;interpreters=0
 for i in range(count):
  typ,flags,offset,vaddr,paddr,filesz,memsz,align=struct.unpack_from('<IIQQQQQQ',data,phoff+i*size)
  if typ==3:
   interpreters+=1
   if offset+filesz>len(data) or data[offset:offset+filesz]!=b'/system/bin/linker64\0':raise ValueError('Android interpreter required')
  if typ==1:
   loads+=1
   if align<16384 or align&(align-1) or offset%align!=vaddr%align:raise ValueError('16 KiB LOAD alignment')
 if not loads:raise ValueError('no LOAD')
 if interpreters!=1:raise ValueError('one Android interpreter required')

def main():
 parser=argparse.ArgumentParser();parser.add_argument('--sdk',required=True);args=parser.parse_args()
 with check('host-preflight'):
  if platform.system()!='Linux' or platform.machine()!='x86_64':raise ValueError('Linux x86_64 required')
 root=Path(__file__).resolve().parents[1];sdk=Path(args.sdk).resolve();ndk=sdk/'ndk'/NDK
 cmake=sdk/'cmake/3.22.1/bin/cmake';ninja=sdk/'cmake/3.22.1/bin/ninja'
 with check('pinned-tools'):
  if not all(x.is_file() for x in [cmake,ninja,ndk/'build/cmake/android.toolchain.cmake',ndk/'source.properties']):raise ValueError('pinned tools unavailable')
 with check('ndk-revision'):
  if 'Pkg.Revision = '+NDK not in (ndk/'source.properties').read_text():raise ValueError('NDK version')
 version=run([cmake,'--version'],'cmake-version',True)
 with check('cmake-version-parse'):
  if not cmake_version_ok(version):raise ValueError('CMake version')
 zlib,headers=target_zlib(ndk)
 common=[f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake','-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-24','-DANDROID_STL=c++_static',f'-DCMAKE_MAKE_PROGRAM={ninja}','-DCMAKE_BUILD_TYPE=Release',f'-DZLIB_INCLUDE_DIR={headers}',f'-DZLIB_LIBRARY={zlib}']
 base=root/'build/android-native'
 with check('workspace-base'):base.mkdir(parents=True,exist_ok=True)
 with check('workspace-temp'):work_context=tempfile.TemporaryDirectory(prefix='check-',dir=base)
 with work_context as work_string:
  work=Path(work_string)
  with check('producer-temp'):producer_context=tempfile.TemporaryDirectory(prefix='producer-',dir=work)
  with producer_context as temporary:
   producer=Path(temporary);source=producer/'source';tarpath=producer/'source.tar'
   with check('producer-source'):source.mkdir()
   run(['git','-C',root,'archive','--format=tar','-o',tarpath,'HEAD'],'source-archive')
   with tarfile.open(tarpath) as tar:tar.extractall(source,filter='data')
   build=producer/'build';prefix=producer/'installed'
   run([cmake,'-S',source,'-B',build,'-G','Ninja',*common,f'-DCMAKE_INSTALL_PREFIX={prefix}',
        '-DBUILD_TESTING=OFF','-DMADOBS_BUILD_RUNTIME=ON','-DMADOBS_BUILD_SHARED=OFF','-DMADOBS_BUILD_JNI=OFF',
        '-DMADOBS_PACKAGE_BORINGSSL=OFF','-DIMOBS_BORINGSSL_ROOT=','-DIMOBS_BORINGSSL_INCLUDE=','-DIMOBS_BORINGSSL_CRYPTO_LIB='],'producer-configure')
   run([cmake,'--build',build,'--target','imobs_core','--parallel','3'],'producer-build')
   run([cmake,'--install',build],'producer-install')
   relocated=work/'relocated SDK';shutil.move(prefix,relocated)
  assert not producer.exists()
  with (relocated/'lib/libmadobs_runtime.a').open('rb') as archive:
   assert archive.read(8)==b'!<arch>\n'
  # Reject any exported reference to the now-deleted disposable producer.
  for config in (relocated/'lib/cmake/MadObs').glob('*.cmake'):
   assert str(producer) not in config.read_text()
  config_directory=installed_config(relocated)
  consumer=work/'independent consumer';shutil.copytree(root/'android-native/consumer',consumer)
  build=work/'consumer-build'
  run([cmake,'-S',consumer,'-B',build,'-G','Ninja',*common,f'-DMadObs_DIR={config_directory}',
       '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF'],'consumer-configure')
  run([cmake,'--build',build,'--parallel','3'],'consumer-build')
  inspect_elf(build/'madobs_c_smoke');assert not producer.exists()
  print('ANDROID NATIVE PASS elf-inspection',flush=True)
 print('PASS: relocated static C SDK linked by independent ARM64 PIE; LOAD alignment >=16KiB. Runtime NOT RUN.')
if __name__=='__main__':
 try:main()
 except Exception:
  print('FAIL: Android native preview validation; no raw diagnostics');raise SystemExit(1) from None
