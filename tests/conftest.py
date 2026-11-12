"""Keep expensive unittest fixtures visible in serial and distributed CI runs."""
from collections import defaultdict
import json
from pathlib import Path
import sys

import pytest


MODULE_SECONDS = defaultdict(lambda: defaultdict(float))
STORAGE = []


def pytest_runtest_logreport(report):
    MODULE_SECONDS[report.nodeid.split("::", 1)[0]][report.when] += report.duration


def _storage_timings():
    module = sys.modules.get("test_evq_storage")
    return getattr(module, "TIMINGS", {})


def pytest_sessionfinish(session):
    if hasattr(session.config, "workeroutput"):
        session.config.workeroutput["storage_timings"] = _storage_timings()


@pytest.hookimpl(optionalhook=True)
def pytest_testnodedown(node, error):
    timing = getattr(node, "workeroutput", {}).get("storage_timings")
    if timing:
        STORAGE.append(timing)


def pytest_terminal_summary(terminalreporter):
    if hasattr(terminalreporter.config, "workerinput"):
        return
    # logreport events arrive at the controller from every xdist worker. Each
    # phase is counted once; their sum is work time, not parallel wall time.
    modules = {name: dict(phases) for name, phases in MODULE_SECONDS.items()}
    storage = STORAGE or ([_storage_timings()] if _storage_timings() else [])
    Path("python-tooling-timings.json").write_text(json.dumps({
        "module_seconds": modules, "storage": storage,
    }, indent=2) + "\n")
    terminalreporter.section("Python tooling inclusive module timings (setup / call / teardown)")
    for name, phases in sorted(modules.items(), key=lambda row: -sum(row[1].values()))[:15]:
        terminalreporter.write_line(
            f"{sum(phases.values()):7.2f}s  "
            f"{phases.get('setup', 0):7.2f} / {phases.get('call', 0):7.2f} / "
            f"{phases.get('teardown', 0):7.2f}  {name}")
    for timing in storage:
        if not all(key in timing for key in ("prebuild_seconds", "pool_seconds", "workers",
                                             "pool_child_cpu_seconds", "scenarios")):
            terminalreporter.write_line("Storage timing incomplete: fixture did not finish")
            continue
        terminalreporter.write_line(
            f"Storage: prebuild={timing['prebuild_seconds']:.2f}s, "
            f"pool={timing['pool_seconds']:.2f}s, workers={timing['workers']}, "
            f"child CPU={timing['pool_child_cpu_seconds']:.2f}s, "
            f"combinations={len(timing['scenarios'])}")
