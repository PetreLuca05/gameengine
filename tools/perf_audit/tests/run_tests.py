#!/usr/bin/env python3
"""Runs the perf_audit tool tests (ctest: perf_audit_tools). Exit 1 on a failure."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

if __name__ == "__main__":
    suite = unittest.defaultTestLoader.discover(HERE, pattern="test_*.py")
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    sys.exit(0 if res.wasSuccessful() else 1)
