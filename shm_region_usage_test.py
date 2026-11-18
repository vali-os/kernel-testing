"""Run the shared region usage core with real IRQ spinlocks and sanitizer checks."""
import argparse
from pathlib import Path
import tempfile
from shm_device_test import ROOT, run_host_test

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    core = "kernel/memory/shm/region_usage.c"
    sources = ("testing/shm_region_usage_test.c", core, "kernel/sync/spinlock.c")
    run_host_test("region-usage", "shm/region_usage.h", sources)
    if args.mutation_check:
        original = (ROOT / core).read_text()
        mutations = (
            ("allow writer with readers", "(write && region->Readers != 0)", "false"),
            ("ignore faults", "if (region->Faults != 0)", "if (false)"),
            ("recover over live readers", "region->Writer || region->Readers != 0 || region->Recovering", "region->Writer || region->Recovering"),
            ("block recovery on stopped readers", "region->Writer || region->Readers != 0 || region->Recovering", "region->Writer || region->Readers != 0 || region->Stopped != 0 || region->Recovering"),
            ("allow last fault removal too soon", "region->Faults == 1 && (region->Stopped != 0 || region->Recovering)", "false"),
        )
        with tempfile.TemporaryDirectory(prefix="vali-region-mutants-") as directory:
            mutant = Path(directory) / "usage.c"
            for name, before, after in mutations:
                assert original.count(before) == 1, name
                mutant.write_text(original.replace(before, after))
                run_host_test("region-mutant", "shm/region_usage.h",
                              (sources[0], str(mutant), sources[2]), mutation=name)
                print("Region mutation rejected:", name)
