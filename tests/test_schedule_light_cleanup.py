"""Exercise production trace/exit/command/reset functions with a fake AMBIT.

Extract the actual C functions to avoid bringing the full ESP-IDF task and event
store into a host binary. Types, schedule compiler and UART deadline math are
the production versions. The fake models an autonomous persisted run, lost ACK,
completed fetch, LED current clamping, and competing UART lock owners. It is
not a hardware/photometry test.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^(?:static )?(?:const )?[\w *]+\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    assert match, name
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class LightCleanupTest(unittest.TestCase):
    def test_production_light_paths(self):
        actions = (ROOT / "components/sched_runner/sched_runner_actions.c").read_text()
        uart = (ROOT / "components/uart_sensors/uart_sensors.c").read_text()
        commands = (ROOT / "components/device_commands/device_commands.c").read_text()
        runner = (ROOT / "components/sched_runner/sched_runner.c").read_text()
        # Lifecycle wiring matters: even idle/between-action stops must clean up
        # before completion allows a replacement schedule to start.
        task = function(runner, "sched_runner_task")
        self.assertLess(task.index("sched_runner_cleanup_lights();"),
                        task.index("sched_lifecycle_complete("))
        extracted = [function(uart, "uart_sensors_reset_all"),
                     function(commands, "cmd_ambit_actinic")]
        extracted += [function(actions, name) for name in (
            "sched_runner_cleanup_lights", "step_input", "channels_mask",
            "ch_present", "act_fail", "act_sleep_ms", "find_protocol", "act_ambit_trace")]
        declarations = "\n".join(line for line in actions.splitlines() if
            line.startswith(("#define POLL_", "#define TRACE_", "static bool s_persist_attempted;",
                             "static ambit_trace_pending_t s_pend[")))
        with tempfile.TemporaryDirectory(prefix="light-cleanup-") as tmp:
            tmp = Path(tmp)
            (tmp / "production.inc").write_text(declarations + "\n" + "\n".join(extracted))
            binary = tmp / "test"
            includes = [ROOT / "tests/host_stubs", ROOT / "components/sched_runner", tmp]
            includes += list((ROOT / "components").glob("*/include"))
            sources = list((ROOT / "components/sched_spec").glob("*.c")) + [
                ROOT / "components/time_sync/time_sync.c",
                ROOT / "components/uart_stream/uart_stream_support.c"]
            subprocess.run([
                os.environ.get("CC", shutil.which("clang") or "cc"),
                "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                *(f"-I{p}" for p in includes), str(ROOT / "tests/schedule_light_cleanup_host.c"),
                *(str(p) for p in sources), "-lm", "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
