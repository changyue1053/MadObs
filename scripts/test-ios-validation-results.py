#!/usr/bin/env python3
"""Synthetic result-parser checks, not evidence of running the iOS tests."""
import copy
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch
import subprocess
spec = importlib.util.spec_from_file_location('gate', Path(__file__).with_name('check-ios-validation-results.py'))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)

def fixture():
    return ({'testStatus':'Passed', 'totalTestCount':2, 'passedTests':2,
             'failedTests':0, 'skippedTests':0, 'expectedFailures':0},
            {'testNodes':[{'nodeType':'Test Suite', 'result':'Passed', 'children':[
                {'nodeType':'Test Case', 'nodeIdentifier':'MadObsNativeTests/'+name+'()', 'result':'Passed'}
                for name in gate.EXPECTED]}]})

class ResultsTests(unittest.TestCase):
    def test_exact_pass(self):
        gate.verify(*fixture())
    def test_missing_or_wrong_counts_fail(self):
        for key in ('totalTestCount','passedTests','failedTests','skippedTests'):
            for value in (None, 3, '0'):
                a,b=fixture(); a[key]=value
                with self.assertRaises(gate.ResultRejected): gate.verify(a,b)
    def test_skip_fail_expected_failure_rejected(self):
        for status in ('Skipped','Failed','Expected Failure','Not Run'):
            a,b=fixture(); b['testNodes'][0]['children'][0]['result']=status
            with self.assertRaises(gate.ResultRejected): gate.verify(a,b)
    def test_wrong_or_missing_name_rejected(self):
        for value in ('Other/testHeaderOnlyContract()', 'MadObsNativeTests/testOther()', None):
            a,b=fixture(); b['testNodes'][0]['children'][0]['nodeIdentifier']=value
            with self.assertRaises(gate.ResultRejected): gate.verify(a,b)
    def test_empty_duplicate_or_extra_cases_rejected(self):
        for mode in ('empty','duplicate','extra'):
            a,b=fixture(); cases=b['testNodes'][0]['children']
            if mode=='empty': cases.clear()
            elif mode=='duplicate': cases[1]=copy.deepcopy(cases[0])
            else: cases.append(copy.deepcopy(cases[0]))
            with self.assertRaises(gate.ResultRejected): gate.verify(a,b)
    def test_failed_summary_rejected(self):
        a,b=fixture(); a['testStatus']='Failed'
        with self.assertRaises(gate.ResultRejected): gate.verify(a,b)
    def test_schema_drift_rejected(self):
        with self.assertRaises(gate.ResultRejected): gate.verify({}, {})
    def test_nested_failed_attempt_rejected(self):
        a,b=fixture(); b['testNodes'][0]['children'][0]['children']=[{'nodeType':'Test Run','result':'Failed'}]
        with self.assertRaises(gate.ResultRejected): gate.verify(a,b)

class DiagnosticTests(unittest.TestCase):
    def rejected(self, a, b, expected):
        with self.assertRaises(gate.ResultRejected) as caught:
            gate.verify(a,b)
        self.assertEqual(str(caught.exception), expected)

    def test_count_schema_distinct_from_count_mismatch(self):
        a,b=fixture(); a.pop('totalTestCount')
        self.rejected(a,b,'FAIL: XCTest count schema mismatch')
        a,b=fixture(); a['totalTestCount']=1
        self.rejected(a,b,'FAIL: expected exactly 2 passed, 0 failed and 0 skipped XCTest cases')

    def test_status_schema_distinct_from_nonpassing_summary(self):
        a,b=fixture(); a.pop('testStatus')
        self.rejected(a,b,'FAIL: XCTest status schema mismatch')
        a,b=fixture(); a['testStatus']='Failed'
        self.rejected(a,b,'FAIL: XCTest summary did not report a clean pass')

    def test_identifier_distinct_from_case_status(self):
        a,b=fixture(); b['testNodes'][0]['children'][0]['nodeIdentifier']='untrusted-value'
        self.rejected(a,b,'FAIL: XCTest case identifier mismatch')
        a,b=fixture(); b['testNodes'][0]['children'][0].pop('result')
        self.rejected(a,b,'FAIL: XCTest case status missing')

    def test_query_fixed_reasons_do_not_leak_exception(self):
        cases=[(subprocess.TimeoutExpired('private-path',45), 'timeout'),
               (subprocess.CalledProcessError(1,'private-path',output=b'private-output'), 'command failed'),
               (OSError('private-path'), 'command failed')]
        for error, reason in cases:
            with patch.object(gate.subprocess,'run',side_effect=error):
                with self.assertRaises(gate.ResultRejected) as caught: gate.query('summary','unused')
            self.assertEqual(str(caught.exception),'FAIL: XCTest result query '+reason)

    def test_invalid_or_oversized_json_fixed_reasons(self):
        for output, message in [(b'not-json private-output','FAIL: XCTest result JSON invalid'),
                                (b' '*(2*1024*1024+1),'FAIL: XCTest result exceeds the bounded validation scope')]:
            with patch.object(gate.subprocess,'run',return_value=subprocess.CompletedProcess([],0,output)):
                with self.assertRaises(gate.ResultRejected) as caught: gate.query('tests','unused')
            self.assertEqual(str(caught.exception),message)

if __name__=='__main__': unittest.main()
