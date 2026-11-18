"""Exercise the DMA gate with real IRQ spinlocks and checked mutex calls."""
import argparse
from pathlib import Path
import signal
import subprocess
import tempfile
from dma_bound_test import ROOT, build_host_fixture

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-dma-gate-") as directory:
        output = Path(directory)
        flags, sources, environment = build_host_fixture(output)
        (output / "spinlock.h").unlink()
        mutex = output / "mutex.h"
        mutex.write_text(mutex.read_text().replace("static inline void MutexLock", "void TestGateMutex(void);\nstatic inline void MutexLock").replace(
            "{ assert(pthread_mutex_lock", "{ TestGateMutex(); assert(pthread_mutex_lock"))
        sources[0] = "testing/dma_operation_gate_test.c"
        sources += ["kernel/sync/spinlock.c"]
        flags += ["-Wl,--wrap=SHMDeviceContextAcquire"]
        binary = output / "gate"
        def run(paths, mutation=None):
            subprocess.run(flags + paths + ["-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], env=environment, capture_output=True, timeout=30)
            if mutation is None:
                print(result.stdout.decode(), end="")
                if result.returncode: print(result.stderr.decode())
                result.check_returncode()
            else:
                assert result.returncode == -signal.SIGABRT and b"Assertion" in result.stderr, (mutation, result.stderr)
                print("Gate mutation rejected:", mutation)
        paths = [str(ROOT / source) for source in sources]
        run(paths)
        if args.mutation_check:
            mutations = (
                ("miss release admitted during context collection", "kernel/memory/dma/recovery.c", "if (group->Access.Operations != 0) {", "if (false) {"),
                ("take mutex in healthy begin", "kernel/memory/dma/binding.c", "    SpinlockAcquireIrq(&group->Access.Lock);", "    MutexLock(&group->Lock);\n    MutexUnlock(&group->Lock);\n    SpinlockAcquireIrq(&group->Access.Lock);"),
            )
            for name, path, before, after in mutations:
                original = (ROOT / path).read_text()
                assert original.count(before) == (2 if name == "take mutex in healthy begin" else 1), name
                mutant = output / "mutant.c"
                mutant.write_text(original.replace(before, after))
                flags += ["-I" + str((ROOT / path).parent)]
                run([str(mutant) if source == str(ROOT / path) else source for source in paths], name)
