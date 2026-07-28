#!/usr/bin/env python3

"""Summarize native JUnit results and write Shields endpoint badge data."""

from __future__ import annotations

import argparse
import json
import os
from dataclasses import dataclass
from pathlib import Path
import xml.etree.ElementTree as ElementTree


@dataclass(frozen=True)
class TestGroup:
    label: str
    junit_file: str
    badge_file: str


@dataclass(frozen=True)
class TestCounts:
    ran: int
    passed: int
    failed: int
    skipped: int


TEST_GROUPS = (
    TestGroup("Device Client", "device-client.xml", "device-client.json"),
    TestGroup("aws-c-iot", "aws-c-iot.xml", "aws-c-iot.json"),
    TestGroup("aws-c-mqtt", "aws-c-mqtt.xml", "aws-c-mqtt.json"),
    TestGroup(
        "IoT Device Defender C++",
        "iot-device-defender-cpp.xml",
        "iot-device-defender-cpp.json",
    ),
)


def local_name(tag: str) -> str:
    """Return an XML tag without its optional namespace."""

    return tag.rsplit("}", 1)[-1]


def is_skipped(test_case: ElementTree.Element) -> bool:
    status = test_case.get("status", "").lower()
    result = test_case.get("result", "").lower()
    if status in {"disabled", "notrun", "skipped"}:
        return True
    if result in {"disabled", "notrun", "skipped", "suppressed"}:
        return True
    return any(local_name(child.tag) == "skipped" for child in test_case)


def is_failed(test_case: ElementTree.Element) -> bool:
    return any(
        local_name(child.tag) in {"error", "failure"} for child in test_case
    )


def read_counts(junit_path: Path) -> TestCounts:
    root = ElementTree.parse(junit_path).getroot()
    test_cases = [
        element
        for element in root.iter()
        if local_name(element.tag) == "testcase"
    ]
    if not test_cases:
        raise ValueError(f"{junit_path} contains no test cases")

    skipped = sum(is_skipped(test_case) for test_case in test_cases)
    ran_cases = [
        test_case for test_case in test_cases if not is_skipped(test_case)
    ]
    if not ran_cases:
        raise ValueError(f"{junit_path} contains no executed test cases")

    failed = sum(is_failed(test_case) for test_case in ran_cases)
    ran = len(ran_cases)

    return TestCounts(
        ran=ran,
        passed=ran - failed,
        failed=failed,
        skipped=skipped,
    )


def badge_payload(group: TestGroup, counts: TestCounts) -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "label": group.label,
        "message": f"{counts.passed}/{counts.ran} passed",
        "color": "brightgreen" if counts.failed == 0 else "red",
    }


def markdown_summary(results: list[tuple[TestGroup, TestCounts]]) -> str:
    lines = [
        "### Native test totals",
        "",
        "| Group | Ran | Passed | Failed | Skipped |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    lines.extend(
        (
            f"| {group.label} | {counts.ran} | {counts.passed} | "
            f"{counts.failed} | {counts.skipped} |"
        )
        for group, counts in results
    )
    return "\n".join(lines) + "\n"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--results-dir", required=True, type=Path)
    parser.add_argument("--badge-dir", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    args.badge_dir.mkdir(parents=True, exist_ok=True)

    results = []
    for group in TEST_GROUPS:
        counts = read_counts(args.results_dir / group.junit_file)
        results.append((group, counts))
        badge_path = args.badge_dir / group.badge_file
        badge_path.write_text(
            json.dumps(badge_payload(group, counts), indent=2) + "\n",
            encoding="utf-8",
        )

    summary = markdown_summary(results)
    print(summary, end="")

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with Path(summary_path).open("a", encoding="utf-8") as summary_file:
            summary_file.write(summary)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
