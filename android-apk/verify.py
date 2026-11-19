"""Official CLI-only unsigned APK packaging. No app execution or output uploads."""
import argparse,hashlib,importlib.util,os,platform,re,shutil,subprocess,tarfile,tempfile,zipfile
from pathlib import Path
# Avoid shadowing Python's inspect module when importing dependencies.
from toolchain import receipts, cmake_version
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('apk_check',ROOT/'android-apk/package_check.py');check=importlib.util.module_from_spec(spec);spec.loader.exec_module(check)
def run(args,stage,capture=False):
    print('START: '+stage,flush=True)
    with tempfile.TemporaryFile() as log:
        try:
            subprocess.run([str(x) for x in args],stdout=log,stderr=log,check=True,timeout=600)
            if capture:
                log.seek(0);data=log.read(4*1024*1024+1)
                if len(data)>4*1024*1024:raise ValueError('bound')
                result=data.decode()
            else:result=None
        except Exception:
            print('FAIL: '+stage,flush=True);raise RuntimeError('stage failure') from None
    print('PASS: '+stage,flush=True);return result

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--sdk',required=True);sdk=Path(parser.parse_args().sdk).resolve()
    if platform.system()!='Linux' or platform.machine()!='x86_64':raise ValueError('host')
    ndk=sdk/'ndk/28.2.13676358';cmake=sdk/'cmake/3.22.1/bin/cmake';ninja=sdk/'cmake/3.22.1/bin/ninja';bt=sdk/'build-tools/35.0.0';android=sdk/'platforms/android-35/android.jar'
    receipts(sdk)
    for p in (cmake,ninja,bt/'aapt2',bt/'zipalign',android):
        if not p.is_file():raise ValueError('missing exact tool')
    if not cmake_version(run([cmake,'--version'],'cmake-version',True)):raise ValueError('CMake version')
    sysroot=ndk/'toolchains/llvm/prebuilt/linux-x86_64/sysroot';zlib=sysroot/'usr/lib/aarch64-linux-android/24/libz.so'
    if not zlib.is_file() or not (sysroot/'usr/include/zlib.h').is_file():raise ValueError('target zlib')
    common=[f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake','-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-24','-DANDROID_STL=c++_static',f'-DCMAKE_MAKE_PROGRAM={ninja}','-DCMAKE_BUILD_TYPE=Release',f'-DZLIB_INCLUDE_DIR={sysroot}/usr/include',f'-DZLIB_LIBRARY={zlib}']
    base=ROOT/'build/android-apk';base.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=base) as tmp:
        work=Path(tmp)
        with tempfile.TemporaryDirectory(dir=work) as prod:
            prod=Path(prod);src=prod/'source';src.mkdir();tar=prod/'source.tar'
            run(['git','-C',ROOT,'archive','--format=tar','-o',tar,'HEAD'],'source')
            with tarfile.open(tar) as t:t.extractall(src,filter='data')
            build=prod/'build';prefix=prod/'installed'
            run([cmake,'-S',src,'-B',build,'-G','Ninja',*common,f'-DCMAKE_INSTALL_PREFIX={prefix}','-DBUILD_TESTING=OFF','-DMADOBS_BUILD_RUNTIME=ON','-DMADOBS_BUILD_SHARED=OFF','-DMADOBS_BUILD_JNI=OFF','-DMADOBS_PACKAGE_BORINGSSL=OFF','-DIMOBS_SAN=','-DIMOBS_BORINGSSL_ROOT=','-DIMOBS_BORINGSSL_INCLUDE=','-DIMOBS_BORINGSSL_CRYPTO_LIB='],'producer-configure')
            run([cmake,'--build',build,'--target','imobs_core','--parallel','3'],'producer-build');run([cmake,'--install',build],'producer-install')
            installed=work/'relocated SDK';shutil.move(prefix,installed)
        archive=installed/'lib/libmadobs_runtime.a'
        with archive.open('rb') as stream:
            if stream.read(8)!=b'!<arch>\n':raise ValueError('archive')
        consumer=work/'consumer';consumer.mkdir()
        for name in ('activity.c','CMakeLists.txt','AndroidManifest.xml'):shutil.copyfile(ROOT/'android-apk'/name,consumer/name)
        build=work/'consumer-build'
        run([cmake,'-S',consumer,'-B',build,'-G','Ninja',*common,f'-DMadObs_DIR={installed}/lib/cmake/MadObs','-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF'],'consumer-configure')
        run([cmake,'--build',build,'--parallel','3'],'consumer-build')
        library=build/'libmadobs_preview.so'
        tools=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
        exports=run([tools/'llvm-nm','--dynamic','--defined-only',library],'activity-dynamic-export',True)
        if not re.search(r'(?m)^[0-9a-fA-F]+ T ANativeActivity_onCreate$',exports):raise ValueError('activity dynamic export')
        symbols=run([tools/'llvm-nm','--defined-only','--extern-only',library],'linked-symbols',True)
        for name in ('ANativeActivity_onCreate','madobs_get_generic_stats'):
            if not re.search(r'(?m)^[0-9a-fA-F]+ [Tt] '+name+r'$',symbols):raise ValueError('required definition')
        dynamic=run([tools/'llvm-readelf','-d',library],'linked-dependencies',True)
        needed=re.findall(r'\(NEEDED\).*?\[([^\]]+)\]',dynamic)
        if not needed or not set(needed)<={'libc.so','libm.so','libdl.so','libz.so','libandroid.so','liblog.so'}:raise ValueError('unexpected runtime dependency')
        so=library.read_bytes();check.elf(so)
        apk=work/'unsigned.apk'
        run([bt/'aapt2','link','-I',android,'--manifest',consumer/'AndroidManifest.xml','--min-sdk-version','24','--target-sdk-version','35','-o',apk],'manifest-link')
        with zipfile.ZipFile(apk,'a') as z:z.writestr(check.LIB,so,compress_type=zipfile.ZIP_STORED)
        aligned=work/'aligned-unsigned.apk'
        run([bt/'zipalign','-P','16','-f','4',apk,aligned],'zip-align')
        run([bt/'zipalign','-c','-P','16','-v','4',aligned],'zip-check')
        check.apk(aligned,so)
        print('PASS: unsigned NativeActivity APK package ZIP and ELF 16KiB; runtime NOT RUN',flush=True)
if __name__=='__main__':
    try:main()
    except Exception:print('FAIL: android-apk-preview');raise SystemExit(1) from None
