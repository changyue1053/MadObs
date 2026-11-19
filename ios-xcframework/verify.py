"""Internal static-library XCFramework validation; no redistribution or device run."""
import importlib.util, json, os, platform, plistlib, re, shutil, subprocess, tarfile, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
HEADERS=('madobs/runtime.h','madobs/observer.h','madobs/privacy.h','imobs/imobs_c.h')
STAGES={'xcode-version','sdk-device','sdk-simulator','sdk-version-device','sdk-version-simulator','archive-source','configure-device','configure-simulator','build-device','build-simulator','install-device','install-simulator','arch-device','arch-simulator','xcframework-create','device-link','sim-create','sim-boot','sim-ready','sim-build','sim-test','result-summary','result-tests','sim-delete'}
def run(args,stage,cwd=None,capture=False):
 if stage not in STAGES:raise ValueError('stage')
 print('IOS PACKAGE START '+stage,flush=True)
 with tempfile.TemporaryFile() as log:
  env=os.environ.copy();env.pop('SDKROOT',None)
  try:subprocess.run([str(x) for x in args],cwd=cwd,env=env,stdout=log,stderr=log,check=True,timeout=600)
  except (OSError,subprocess.SubprocessError):
   print('IOS PACKAGE FAIL '+stage,flush=True);raise RuntimeError('tool failed') from None
  if capture:
   log.seek(0);data=log.read(2*1024*1024+1)
   if len(data)>2*1024*1024:raise ValueError('output bound')
   result=data.decode('utf-8')
  else:result=None
 print('IOS PACKAGE PASS '+stage,flush=True)
 return result

def load(path,name):
 spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def inspect(xc):
 libraries=plistlib.loads((xc/'Info.plist').read_bytes())['AvailableLibraries']
 if len(libraries)!=2:raise ValueError('slice count')
 result={};reference=None
 for lib in libraries:
  if lib['SupportedPlatform']!='ios' or lib['SupportedArchitectures']!=['arm64']:raise ValueError('platform')
  variant=lib.get('SupportedPlatformVariant','device')
  if variant not in {'device','simulator'} or variant in result:raise ValueError('variant')
  # Do not trust metadata to escape the relocated artifact.
  paths=[]
  for value in [lib['LibraryIdentifier'],lib['LibraryPath'],lib['HeadersPath']]:
   path=Path(value)
   if path.is_absolute() or '..' in path.parts:raise ValueError('package path')
   paths.append(path)
  base=xc/paths[0];archive=base/paths[1];headers=base/paths[2]
  for path in [archive,headers]:
   if not path.resolve().is_relative_to(xc.resolve()):raise ValueError('package containment')
  with archive.open('rb') as f:
   if f.read(8)!=b'!<arch>\n':raise ValueError('static archive')
  actual={str(p.relative_to(headers)) for p in headers.rglob('*') if p.is_file()}
  if actual!=set(HEADERS)|{'module.modulemap'}:raise ValueError('public header surface')
  contents=[(headers/h).read_bytes() for h in HEADERS]
  if reference is not None and contents!=reference:raise ValueError('header disagreement')
  reference=contents;result[variant]=(archive,headers)
 return result

def consumer_project(consumer):
 generator=load(ROOT/'ios-validation/Consumer/generate_project.py','consumer_generator')
 for number in (64,65):
  settings=generator.OBJECTS[generator.ref(number)]['buildSettings']
  settings['HEADER_SEARCH_PATHS']=['$(inherited)','"$(MADOBS_HEADERS)"']
  settings['OTHER_LDFLAGS']=['$(inherited)','"$(MADOBS_ARCHIVE)"','-lz','-lc++','-framework','Foundation','-framework','XCTest']
 for number in (62,63):
  generator.OBJECTS[generator.ref(number)]['buildSettings']['OTHER_LDFLAGS']=['-framework','Foundation','-framework','UIKit']
 project=consumer/'MadObsValidation.xcodeproj';project.mkdir()
 shutil.copytree(ROOT/'ios-validation/Consumer/MadObsValidation.xcodeproj/xcshareddata',project/'xcshareddata')
 for directory in ('App','Tests'):shutil.copytree(ROOT/'ios-validation/Consumer'/directory,consumer/directory)
 # Original fixtures remain byte-identical; this prefix only verifies compilation ABI.
 (consumer/'Arm64Guard.h').write_text('#if !defined(__arm64__)\n#error "ARM64 required"\n#endif\n')
 # Explicit prefix applies to both device and simulator consumer compilations.
 for number in (60,61):
  generator.OBJECTS[generator.ref(number)]['buildSettings']['GCC_PREFIX_HEADER']='$(SRCROOT)/Arm64Guard.h'
 (project/'project.pbxproj').write_text(generator.project_text())

def main():
 if platform.system()!='Darwin' or platform.machine()!='arm64':raise ValueError('Apple ARM64 required')
 if run(['xcodebuild','-version'],'xcode-version',capture=True).strip()!='Xcode 26.2\nBuild version 17C52':raise ValueError('Xcode version')
 base=ROOT/'build/ios-xcframework';base.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='check-',dir=base) as temporary:
  work=Path(temporary);artifacts=[]
  with tempfile.TemporaryDirectory(prefix='producer-',dir=work) as disposable:
   producer=Path(disposable);source=producer/'source';source.mkdir();tarpath=producer/'source.tar'
   run(['git','-C',ROOT,'archive','--format=tar','-o',tarpath,'HEAD'],'archive-source')
   with tarfile.open(tarpath) as tar:tar.extractall(source,filter='data')
   for variant,sdk in [('device','iphoneos'),('simulator','iphonesimulator')]:
    sdkroot=run(['xcrun','--sdk',sdk,'--show-sdk-path'],'sdk-'+variant,capture=True).strip()
    if run(['xcrun','--sdk',sdk,'--show-sdk-version'],'sdk-version-'+variant,capture=True).strip()!='26.2':raise ValueError('SDK version')
    assert (Path(sdkroot)/'usr/lib/libz.tbd').is_file() and (Path(sdkroot)/'usr/include/zlib.h').is_file()
    build=producer/(variant+'-build');prefix=producer/(variant+'-installed')
    run(['cmake','-S',source,'-B',build,'-G','Xcode','-DCMAKE_SYSTEM_NAME=iOS',f'-DCMAKE_OSX_SYSROOT={sdkroot}',
         '-DCMAKE_OSX_ARCHITECTURES=arm64','-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0','-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO',
         f'-DCMAKE_INSTALL_PREFIX={prefix}','-DBUILD_TESTING=OFF','-DMADOBS_BUILD_RUNTIME=ON','-DMADOBS_BUILD_SHARED=OFF','-DMADOBS_BUILD_JNI=OFF',
         '-DMADOBS_PACKAGE_BORINGSSL=OFF','-DIMOBS_SAN=','-DIMOBS_BORINGSSL_ROOT=','-DIMOBS_BORINGSSL_INCLUDE=','-DIMOBS_BORINGSSL_CRYPTO_LIB=',
         f'-DZLIB_INCLUDE_DIR={sdkroot}/usr/include',f'-DZLIB_LIBRARY={sdkroot}/usr/lib/libz.tbd'],'configure-'+variant)
    run(['cmake','--build',build,'--config','Release','--target','imobs_core','--parallel','3'],'build-'+variant)
    run(['cmake','--install',build,'--config','Release'],'install-'+variant)
    archive=prefix/'lib/libmadobs_runtime.a'
    if run(['xcrun','lipo','-archs',archive],'arch-'+variant,capture=True).strip()!='arm64':raise ValueError('archive architecture')
    headers=producer/(variant+'-headers');headers.mkdir()
    for name in HEADERS:
     dest=headers/name;dest.parent.mkdir(exist_ok=True);shutil.copyfile(prefix/'include'/name,dest)
    (headers/'module.modulemap').write_text('module MadObsRuntime { header "madobs/runtime.h" export * }\n')
    artifacts.extend(['-library',archive,'-headers',headers])
   produced=producer/'MadObsRuntime.xcframework'
   run(['xcodebuild','-create-xcframework',*artifacts,'-output',produced],'xcframework-create')
   relocated=work/'relocated artifact';relocated.mkdir();xc=relocated/'MadObsRuntime.xcframework';shutil.move(produced,xc)
  assert not producer.exists()
  slices=inspect(xc);consumer=work/'independent consumer';consumer.mkdir();consumer_project(consumer)
  common=['-project','MadObsValidation.xcodeproj','-scheme','MadObsValidation','ARCHS=arm64','ONLY_ACTIVE_ARCH=YES','-parallel-testing-enabled','NO']
  def inputs(variant):
   archive,headers=slices[variant]
   return [f'MADOBS_ARCHIVE={archive}',f'MADOBS_HEADERS={headers}']
  run(['xcodebuild','build-for-testing',*common,*inputs('device'),'-destination','generic/platform=iOS','-derivedDataPath',work/'device-consumer','CODE_SIGNING_ALLOWED=NO'],'device-link',cwd=consumer)
  uuid=run(['xcrun','simctl','create','MadObsPackageSynthetic','com.apple.CoreSimulator.SimDeviceType.iPhone-16','com.apple.CoreSimulator.SimRuntime.iOS-26-2'],'sim-create',capture=True).strip()
  if not re.fullmatch(r'[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}',uuid):raise ValueError('simulator identity')
  try:
   run(['xcrun','simctl','boot',uuid],'sim-boot');run(['xcrun','simctl','bootstatus',uuid,'-b'],'sim-ready')
   args=[*common,*inputs('simulator'),'-destination','platform=iOS Simulator,id='+uuid,'-derivedDataPath',work/'sim-consumer','CODE_SIGNING_ALLOWED=YES','CODE_SIGN_IDENTITY=-','CODE_SIGNING_REQUIRED=NO']
   run(['xcodebuild','build-for-testing',*args],'sim-build',cwd=consumer)
   result=work/'result.xcresult'
   run(['xcodebuild','test-without-building',*args,'-resultBundlePath',result],'sim-test',cwd=consumer)
   gate=load(ROOT/'scripts/check-ios-validation-results.py','results')
   summary=json.loads(run(['xcrun','xcresulttool','get','test-results','summary','--path',result],'result-summary',capture=True))
   tests=json.loads(run(['xcrun','xcresulttool','get','test-results','tests','--path',result],'result-tests',capture=True))
   gate.verify(summary,tests)
  finally:run(['xcrun','simctl','delete',uuid],'sim-delete')
  assert not producer.exists()
 print('PASS: relocated static XCFramework, independent device link and two named simulator tests; no device runtime or distribution')
if __name__=='__main__':
 try:main()
 except Exception:
  print('FAIL: internal iOS XCFramework validation; no raw diagnostics');raise SystemExit(1) from None
