#!/usr/bin/env python3
"""Require the two exact XCTest passes; print only fixed labels/counts, never result JSON."""
import json
from pathlib import Path
import subprocess
import sys

EXPECTED = ('testHeaderOnlyContract', 'testInstalledArchiveLocalSinkAndLifecycle')

class ResultRejected(Exception):
    pass

def verify(summary, tests):
    if not isinstance(summary, dict) or not isinstance(tests, dict):
        raise ResultRejected('FAIL: unexpected XCTest result schema')
    for key, expected in [('totalTestCount', 2), ('passedTests', 2), ('failedTests', 0), ('skippedTests', 0)]:
        if type(summary.get(key)) is not int:
            raise ResultRejected('FAIL: XCTest count schema mismatch')
        if summary[key] != expected:
            raise ResultRejected('FAIL: expected exactly 2 passed, 0 failed and 0 skipped XCTest cases')
    # Supplemental only: exact counts and named case results remain mandatory.
    # Do not infer alternate status fields/values from an absent optional field.
    if 'testStatus' in summary and not isinstance(summary['testStatus'], str):
        raise ResultRejected('FAIL: XCTest status schema mismatch')
    if ('testStatus' in summary and summary['testStatus'] != 'Passed') or summary.get('expectedFailures', 0) != 0:
        raise ResultRejected('FAIL: XCTest summary did not report a clean pass')
    roots = tests.get('testNodes')
    if not isinstance(roots, list):
        raise ResultRejected('FAIL: missing XCTest case tree')
    found = []
    def walk(node):
        if not isinstance(node, dict):
            raise ResultRejected('FAIL: unexpected XCTest tree schema')
        if 'result' in node and node['result'] != 'Passed':
            raise ResultRejected('FAIL: a XCTest node did not pass')
        if node.get('nodeType') == 'Test Case':
            identifier = node.get('nodeIdentifier')
            matched = None
            for name in EXPECTED:
                # Exact class/method identifiers, tolerating only optional empty parentheses.
                if identifier in ('MadObsNativeTests/' + name, 'MadObsNativeTests/' + name + '()'):
                    matched = name
            if matched is None:
                raise ResultRejected('FAIL: XCTest case identifier mismatch')
            if node.get('result') != 'Passed':
                raise ResultRejected('FAIL: XCTest case status missing')
            found.append(matched)
        children = node.get('children', [])
        if not isinstance(children, list):
            raise ResultRejected('FAIL: unexpected XCTest child schema')
        for child in children:
            walk(child)
    for node in roots:
        walk(node)
    if len(found) != 2 or sorted(found) != sorted(EXPECTED):
        raise ResultRejected('FAIL: missing, duplicated or additional XCTest case')

def query(kind, path):
    try:
        result = subprocess.run(['xcrun', 'xcresulttool', 'get', 'test-results', kind,
                                 '--path', str(path), '--compact'],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                check=True, timeout=45)
        if len(result.stdout) > 2 * 1024 * 1024:
            raise ResultRejected('FAIL: XCTest result exceeds the bounded validation scope')
        return json.loads(result.stdout)
    except subprocess.TimeoutExpired:
        raise ResultRejected('FAIL: XCTest result query timeout') from None
    except (OSError, subprocess.SubprocessError):
        raise ResultRejected('FAIL: XCTest result query command failed') from None
    except ValueError:
        raise ResultRejected('FAIL: XCTest result JSON invalid') from None

def main():
    path = Path(__file__).resolve().parents[1] / 'build/ios-validation/simulator/result.xcresult'
    if not path.is_dir():
        raise ResultRejected('FAIL: required XCTest result bundle is absent')
    verify(query('summary', path), query('tests', path))
    # These strings come only from the fixed source allowlist, never raw result fields.
    for name in EXPECTED:
        print('PASS: MadObsNativeTests/' + name)
    print('PASS: XCTest total=2 passed=2 failed=0 skipped=0')

if __name__ == '__main__':
    try:
        main()
    except ResultRejected as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
    except Exception:
        # Never expose unexpected parser exception text, paths or raw values.
        print('FAIL: unexpected XCTest parser error', file=sys.stderr)
        sys.exit(1)
