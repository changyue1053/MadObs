#!/usr/bin/env python3
"""Local recipe/project consistency checks; never an Apple compilation claim."""
import ast
import pathlib
import runpy
import xml.etree.ElementTree as ET
root = pathlib.Path(__file__).resolve().parents[1]
consumer = root / 'ios-validation/Consumer'
source = consumer / 'generate_project.py'
ast.parse(source.read_text())
namespace = runpy.run_path(str(source))
assert namespace['project_text']() == (consumer / 'MadObsValidation.xcodeproj/project.pbxproj').read_text()
objects = namespace['OBJECTS']
for item in objects.values():
    assert item['isa'] != 'PBXShellScriptBuildPhase'
    if item['isa'] == 'PBXSourcesBuildPhase':
        assert item['files']
    if item['isa'] == 'XCBuildConfiguration':
        settings = item['buildSettings']
        if 'HEADER_SEARCH_PATHS' in settings:
            assert settings['HEADER_SEARCH_PATHS'] == ['$(inherited)', '$(MADOBS_STAGE)/include']
            assert settings['OTHER_LDFLAGS'] == ['$(inherited)', '$(MADOBS_STAGE)/lib/libmadobs_runtime.a', '-lz', '-lc++']
scheme = ET.parse(consumer / 'MadObsValidation.xcodeproj/xcshareddata/xcschemes/MadObsValidation.xcscheme')
for reference in scheme.findall('.//BuildableReference'):
    assert reference.attrib['BlueprintIdentifier'] in objects
api = (consumer / 'Tests/APIOnly.c').read_text()
assert '#include "madobs/privacy.h"' in api
assert 'madobs/runtime.h' not in api
assert 'madobs_create' not in api
build = (root / 'scripts/build-ios-validation.sh').read_text()
for flag in ['-DBUILD_TESTING=OFF', '-DMADOBS_BUILD_RUNTIME=ON', '-DMADOBS_BUILD_SHARED=OFF',
             '-DMADOBS_BUILD_JNI=OFF', '-DMADOBS_PACKAGE_BORINGSSL=OFF',
             '-DZLIB_LIBRARY="$sdkroot/usr/lib/libz.tbd"']:
    assert flag in build
assert 'simctl' not in build
print('PASS: local source recipe/project consistency only (not Xcode build or runtime)')
