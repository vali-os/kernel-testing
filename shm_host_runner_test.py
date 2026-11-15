"""Check that SHM mutation checks distinguish assertions from test errors."""
from contextlib import redirect_stdout
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from shm_device_test import main, run_host_test


class MutationRunnerTests(unittest.TestCase):
    def run_source(self, source):
        with tempfile.TemporaryDirectory(prefix="vali-runner-test-") as directory:
            path = Path(directory) / "test.c"
            path.write_text(source)
            run_host_test("runner-check", "stddef.h", (str(path),),
                          mutation="runner regression")

    def test_assertion_rejects_mutation(self):
        self.run_source('#include <assert.h>\nint main(void) { assert(0); }\n')

    def test_surviving_mutation_fails(self):
        with self.assertRaisesRegex(AssertionError, "did not fail at an assertion"):
            self.run_source("int main(void) { return 0; }\n")

    def test_other_runtime_failure_is_not_a_rejected_mutation(self):
        with self.assertRaisesRegex(AssertionError, "did not fail at an assertion"):
            self.run_source("int main(void) { return 1; }\n")

    def test_compile_failure_is_not_a_rejected_mutation(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_source('#error "intentional compiler failure"\n')

    def test_timeout_is_not_a_rejected_mutation(self):
        run = subprocess.run

        def timeout_binary(command, **kwargs):
            if len(command) == 1:
                raise subprocess.TimeoutExpired(command, kwargs["timeout"])
            return run(command, **kwargs)

        with patch("shm_device_test.subprocess.run", side_effect=timeout_binary):
            with self.assertRaises(subprocess.TimeoutExpired):
                self.run_source("int main(void) { return 0; }\n")

    def test_mutation_loop_propagates_runner_errors(self):
        failures = (
            subprocess.CalledProcessError(1, ["compiler"]),
            subprocess.TimeoutExpired(["mutant"], 30),
        )
        for failure in failures:
            with self.subTest(failure=type(failure).__name__):
                output = io.StringIO()
                # Let the baseline pass, then fail the first mutant. The driver
                # must preserve the error without reporting a rejected mutation.
                with patch("sys.argv", ["shm_device_test.py", "--mutation-check"]):
                    with patch("shm_device_test.run_host_test", side_effect=[None, failure]):
                        with redirect_stdout(output), self.assertRaises(type(failure)):
                            main()
                self.assertNotIn("Rejected mutation:", output.getvalue())


if __name__ == "__main__":
    unittest.main()
