"""Generate a binary-only in-repository validation consumer; no external packages."""
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parent
OBJECTS = {}
def ref(n): return f'{n:024X}'
def add(n, isa, **fields): OBJECTS[ref(n)] = dict(isa=isa, **fields)
add(1, 'PBXProject', attributes={'TargetAttributes': {ref(21): {'TestTargetID': ref(20)}}}, buildConfigurationList=ref(50), compatibilityVersion='Xcode 14.0', developmentRegion='en', hasScannedForEncodings='0', knownRegions=['en','Base'], mainGroup=ref(2), productRefGroup=ref(3), projectDirPath='', projectRoot='', targets=[ref(20),ref(21)])
add(2, 'PBXGroup', children=[ref(4),ref(5),ref(3)], sourceTree='<group>')
add(3, 'PBXGroup', name='Products', children=[ref(10),ref(11)], sourceTree='<group>')
add(4, 'PBXGroup', path='App', children=[ref(7)], sourceTree='<group>')
add(5, 'PBXGroup', path='Tests', children=[ref(8),ref(9)], sourceTree='<group>')
for n, typ, path in [(7,'sourcecode.c.objc','main.m'),(8,'sourcecode.cpp.objcpp','NativeTests.mm'),(9,'sourcecode.c.c','APIOnly.c')]:
    add(n, 'PBXFileReference', lastKnownFileType=typ, path=path, sourceTree='<group>')
add(10,'PBXFileReference',explicitFileType='wrapper.application',includeInIndex='0',path='MadObsValidation.app',sourceTree='BUILT_PRODUCTS_DIR')
add(11,'PBXFileReference',explicitFileType='wrapper.cfbundle',includeInIndex='0',path='MadObsValidationTests.xctest',sourceTree='BUILT_PRODUCTS_DIR')
for n, file in [(12,7),(13,8),(14,9)]: add(n,'PBXBuildFile',fileRef=ref(file))
add(20,'PBXNativeTarget',buildConfigurationList=ref(51),buildPhases=[ref(30),ref(31),ref(32)],buildRules=[],dependencies=[],name='MadObsValidation',productName='MadObsValidation',productReference=ref(10),productType='com.apple.product-type.application')
add(21,'PBXNativeTarget',buildConfigurationList=ref(52),buildPhases=[ref(34),ref(35),ref(36)],buildRules=[],dependencies=[ref(40)],name='MadObsValidationTests',productName='MadObsValidationTests',productReference=ref(11),productType='com.apple.product-type.bundle.unit-test')
for n, isa, files in [(30,'PBXSourcesBuildPhase',[12]),(31,'PBXFrameworksBuildPhase',[]),(32,'PBXResourcesBuildPhase',[]),(34,'PBXSourcesBuildPhase',[13,14]),(35,'PBXFrameworksBuildPhase',[]),(36,'PBXResourcesBuildPhase',[])]:
    add(n,isa,buildActionMask='2147483647',files=list(map(ref,files)),runOnlyForDeploymentPostprocessing='0')
add(40,'PBXTargetDependency',target=ref(20),targetProxy=ref(41))
add(41,'PBXContainerItemProxy',containerPortal=ref(1),proxyType='1',remoteGlobalIDString=ref(20),remoteInfo='MadObsValidation')
for n, configs in [(50,[60,61]),(51,[62,63]),(52,[64,65])]:
    add(n,'XCConfigurationList',buildConfigurations=list(map(ref,configs)),defaultConfigurationIsVisible='0',defaultConfigurationName='Release')
base={'CLANG_ENABLE_MODULES':'YES','CLANG_ENABLE_OBJC_ARC':'YES','GCC_ENABLE_CPP_EXCEPTIONS':'YES','GCC_ENABLE_OBJC_EXCEPTIONS':'YES','CLANG_CXX_LANGUAGE_STANDARD':'c++20','CLANG_CXX_LIBRARY':'libc++','GCC_C_LANGUAGE_STANDARD':'c11','IPHONEOS_DEPLOYMENT_TARGET':'15.0','SDKROOT':'iphoneos','SUPPORTED_PLATFORMS':'iphoneos iphonesimulator','TARGETED_DEVICE_FAMILY':'1,2','ARCHS':'arm64','CODE_SIGNING_ALLOWED':'NO'}
for n, name in [(60,'Debug'),(61,'Release')]:
    settings=dict(base,GCC_OPTIMIZATION_LEVEL='0' if name=='Debug' else 's')
    add(n,'XCBuildConfiguration',name=name,buildSettings=settings)
app={'PRODUCT_BUNDLE_IDENTIFIER':'org.example.madobs.localvalidation','PRODUCT_NAME':'$(TARGET_NAME)','GENERATE_INFOPLIST_FILE':'YES','INFOPLIST_KEY_UILaunchScreen_Generation':'YES','LD_RUNPATH_SEARCH_PATHS':['$(inherited)','@executable_path/Frameworks']}
tests={'PRODUCT_BUNDLE_IDENTIFIER':'org.example.madobs.localvalidation.tests','PRODUCT_NAME':'$(TARGET_NAME)','GENERATE_INFOPLIST_FILE':'YES','TEST_HOST':'$(BUILT_PRODUCTS_DIR)/MadObsValidation.app/MadObsValidation','BUNDLE_LOADER':'$(TEST_HOST)','LD_RUNPATH_SEARCH_PATHS':['$(inherited)','@executable_path/Frameworks','@loader_path/Frameworks'],'HEADER_SEARCH_PATHS':['$(inherited)','$(MADOBS_STAGE)/include'],'OTHER_LDFLAGS':['$(inherited)','$(MADOBS_STAGE)/lib/libmadobs_runtime.a','-lz','-lc++']}
for n,name,settings in [(62,'Debug',app),(63,'Release',app),(64,'Debug',tests),(65,'Release',tests)]: add(n,'XCBuildConfiguration',name=name,buildSettings=settings)
def emit(v,level=0):
    if isinstance(v,dict): return '{\n'+''.join('\t'*(level+1)+json.dumps(k)+' = '+emit(x,level+1)+';\n' for k,x in v.items())+'\t'*level+'}'
    if isinstance(v,list): return '('+', '.join(emit(x,level) for x in v)+')'
    return json.dumps(v)
def project_text():
    return '// !$*UTF8*$!\n'+emit(dict(archiveVersion='1',classes={},objectVersion='56',objects=OBJECTS,rootObject=ref(1)))+'\n'
if __name__=='__main__':
    (ROOT/'MadObsValidation.xcodeproj/project.pbxproj').write_text(project_text())
