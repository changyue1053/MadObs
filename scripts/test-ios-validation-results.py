#!/usr/bin/env python3
"""Synthetic result-parser checks, not evidence of running the iOS tests."""
import copy
import importlib.util
from pathlib import Path
import unittest
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

if __name__=='__main__': unittest.main()
