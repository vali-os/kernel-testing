"""Run direct DMA preparation and installation without an SHM context."""
import argparse
from pathlib import Path
import tempfile
from shm_device_test import ROOT, run_host_test


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    sources = ("testing/dma_address_test.c", "kernel/memory/dma/address.c",
               "kernel/memory/dma/direct.c")
    run_host_test("dma-address-test", "dma/address.h", sources)
    if not args.mutation_check:
        return

    original = (ROOT / "kernel/memory/dma/address.c").read_text()
    mutations = (
        ("accept unknown mapping direction",
         "direction != DMAToDevice && direction != DMAFromDevice && direction != DMABidirectional",
         "false"),
        ("allow repeated installation", "mapping->State != DMAAddressMappingPrepared", "false"),
        ("forget completed installation", "mapping->State = DMAAddressMappingInstalled;",
         "// Mutant forgets installation."),
        ("allocate during direct installation", "mapping->State = DMAAddressMappingInstalled;",
         "void* extra = kmalloc(1); if (extra == NULL) { return OS_EOOM; } kfree(extra);\n"
         "    mapping->State = DMAAddressMappingInstalled;"),
    )
    for name, before, after in mutations:
        assert original.count(before) == 1, name
        with tempfile.TemporaryDirectory(prefix="vali-dma-address-mutant-") as directory:
            mutant = Path(directory) / "address.c"
            mutant.write_text(original.replace(before, after))
            changed = tuple(str(mutant) if path == "kernel/memory/dma/address.c" else path
                            for path in sources)
            run_host_test("dma-address-mutant", "dma/address.h", changed,
                          ("-I" + str(ROOT / "kernel/memory/dma"),), mutation=name)
            print("Rejected DMA address mutation:", name, flush=True)


if __name__ == "__main__":
    main()
