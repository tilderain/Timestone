"""Run unittest test methods in parallel processes.

The native-mock suites compile one sanitizer build per test method, so a serial
run is bound by compiler time. Each method runs in its own process here.

    python3 tools/run_tests_parallel.py tools.test_lifetime_trace [more modules...]
    python3 tools/run_tests_parallel.py -k payload tools.test_lifetime_trace
"""

import argparse
import concurrent.futures
import os
import subprocess
import sys
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_ids(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from test_ids(item)
        else:
            yield item.id()


def run(test_id):
    start = time.monotonic()
    result = subprocess.run([sys.executable, "-m", "unittest", test_id], cwd=ROOT,
                            capture_output=True, text=True)
    return test_id, result, time.monotonic() - start


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("modules", nargs="+")
    parser.add_argument("-k", dest="patterns", action="append", default=[],
                        help="only run test ids containing this substring (repeatable)")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    sys.path.insert(0, str(ROOT))
    ids = [test_id for test_id in test_ids(unittest.defaultTestLoader.loadTestsFromNames(args.modules))
           if not args.patterns or any(pattern in test_id for pattern in args.patterns)]
    if not ids:
        sys.exit("no tests matched")

    start, failed = time.monotonic(), []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for test_id, result, seconds in pool.map(run, ids):
            status = "ok" if result.returncode == 0 else "FAIL"
            print(f"{status:4} {seconds:6.1f}s {test_id}", flush=True)
            if result.returncode:
                failed.append((test_id, result))
    for test_id, result in failed:
        print(f"\n===== {test_id} =====\n{result.stdout}{result.stderr}")
    print(f"\n{len(ids) - len(failed)}/{len(ids)} passed in {time.monotonic() - start:.1f}s")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
