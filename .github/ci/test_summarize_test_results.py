#!/usr/bin/env python3

import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import summarize_test_results


class SummarizeTestResultsTest(unittest.TestCase):
    def test_read_counts_excludes_skipped_and_counts_failures_and_errors(self):
        junit = """\
<testsuites>
  <testsuite>
    <testcase name="passes" status="run" result="completed"/>
    <testcase name="fails" status="run"><failure/></testcase>
    <testcase name="errors" status="run"><error/></testcase>
    <testcase name="disabled" status="notrun" result="suppressed"/>
    <testcase name="skipped" status="run"><skipped/></testcase>
  </testsuite>
</testsuites>
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            junit_path = Path(temp_dir) / "results.xml"
            junit_path.write_text(junit, encoding="utf-8")

            counts = summarize_test_results.read_counts(junit_path)

        self.assertEqual(
            counts,
            summarize_test_results.TestCounts(
                ran=3,
                passed=1,
                failed=2,
                skipped=2,
            ),
        )

    def test_read_counts_accepts_flat_ctest_output(self):
        junit = """\
<testsuite>
  <testcase name="passes" status="run"/>
  <testcase name="fails" status="fail">
    <failure/>
    <failure/>
  </testcase>
  <testcase name="notrun" status="notrun"><skipped/></testcase>
  <testcase name="disabled" status="disabled"/>
</testsuite>
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            junit_path = Path(temp_dir) / "results.xml"
            junit_path.write_text(junit, encoding="utf-8")

            counts = summarize_test_results.read_counts(junit_path)

        self.assertEqual(
            counts,
            summarize_test_results.TestCounts(
                ran=2,
                passed=1,
                failed=1,
                skipped=2,
            ),
        )

    def test_read_counts_rejects_an_all_skipped_report(self):
        junit = """\
<testsuite>
  <testcase name="notrun" status="notrun"><skipped/></testcase>
  <testcase name="disabled" status="disabled"/>
</testsuite>
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            junit_path = Path(temp_dir) / "results.xml"
            junit_path.write_text(junit, encoding="utf-8")

            with self.assertRaisesRegex(
                ValueError,
                "contains no executed test cases",
            ):
                summarize_test_results.read_counts(junit_path)

    def test_main_writes_one_green_badge_per_group_and_job_summary(self):
        junit = """\
<testsuites>
  <testsuite>
    <testcase name="one" status="run" result="completed"/>
    <testcase name="two" status="run" result="completed"/>
    <testcase name="disabled" status="notrun" result="suppressed"/>
  </testsuite>
</testsuites>
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            results_dir = root / "results"
            badge_dir = root / "badges"
            summary_path = root / "summary.md"
            results_dir.mkdir()
            for group in summarize_test_results.TEST_GROUPS:
                (results_dir / group.junit_file).write_text(
                    junit,
                    encoding="utf-8",
                )

            with mock.patch.dict(
                os.environ,
                {"GITHUB_STEP_SUMMARY": str(summary_path)},
            ):
                with contextlib.redirect_stdout(io.StringIO()):
                    result = summarize_test_results.main(
                        [
                            "--results-dir",
                            str(results_dir),
                            "--badge-dir",
                            str(badge_dir),
                        ]
                    )

            self.assertEqual(result, 0)
            self.assertEqual(
                {path.name for path in badge_dir.glob("*.json")},
                {group.badge_file for group in summarize_test_results.TEST_GROUPS},
            )
            for group in summarize_test_results.TEST_GROUPS:
                payload = json.loads(
                    (badge_dir / group.badge_file).read_text(encoding="utf-8")
                )
                self.assertEqual(
                    payload,
                    {
                        "schemaVersion": 1,
                        "label": group.label,
                        "message": "2/2 passed",
                        "color": "brightgreen",
                    },
                )

            summary = summary_path.read_text(encoding="utf-8")
            self.assertIn("| Device Client | 2 | 2 | 0 | 1 |", summary)
            self.assertIn(
                (
                    "| Eventstream RPC + Device Defender C++ "
                    "| 2 | 2 | 0 | 1 |"
                ),
                summary,
            )

    def test_main_marks_every_unavailable_result_and_returns_nonzero(self):
        unavailable_junit = {
            "aws-c-iot.xml": "<testsuite>",
            "aws-c-mqtt.xml": "<testsuite/>",
            "iot-device-defender-cpp.xml": """\
<testsuite>
  <testcase name="disabled" status="notrun"><skipped/></testcase>
</testsuite>
""",
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            results_dir = root / "results"
            badge_dir = root / "badges"
            summary_path = root / "summary.md"
            results_dir.mkdir()
            for junit_file, contents in unavailable_junit.items():
                (results_dir / junit_file).write_text(
                    contents,
                    encoding="utf-8",
                )

            stdout = io.StringIO()
            stderr = io.StringIO()
            with mock.patch.dict(
                os.environ,
                {"GITHUB_STEP_SUMMARY": str(summary_path)},
            ):
                with (
                    contextlib.redirect_stdout(stdout),
                    contextlib.redirect_stderr(stderr),
                ):
                    result = summarize_test_results.main(
                        [
                            "--results-dir",
                            str(results_dir),
                            "--badge-dir",
                            str(badge_dir),
                        ]
                    )

            self.assertEqual(result, 1)
            self.assertEqual(
                {path.name for path in badge_dir.glob("*.json")},
                {group.badge_file for group in summarize_test_results.TEST_GROUPS},
            )
            for group in summarize_test_results.TEST_GROUPS:
                payload = json.loads(
                    (badge_dir / group.badge_file).read_text(encoding="utf-8")
                )
                self.assertEqual(
                    payload,
                    {
                        "schemaVersion": 1,
                        "label": group.label,
                        "message": "results unavailable",
                        "color": "red",
                    },
                )

            summary = summary_path.read_text(encoding="utf-8")
            self.assertEqual(stdout.getvalue(), summary)
            for group in summarize_test_results.TEST_GROUPS:
                self.assertIn(
                    f"| {group.label} | N/A | N/A | N/A | N/A |",
                    summary,
                )

            errors = stderr.getvalue()
            self.assertIn(
                "Device Client: results unavailable:",
                errors,
            )
            self.assertIn(
                "aws-c-iot: results unavailable:",
                errors,
            )
            self.assertIn(
                "aws-c-mqtt: results unavailable:",
                errors,
            )
            self.assertIn(
                (
                    "Eventstream RPC + Device Defender C++: "
                    "results unavailable:"
                ),
                errors,
            )


if __name__ == "__main__":
    unittest.main()
