"""Generate and exercise the real netadapter bindings without a live driver.

Run directly, or through CTest. This verifies the protocol/generator boundary;
ownership and replay state machines remain the responsibility of session tests.
NETADAPTER_TEST_CFLAGS can enable sanitizers without maintaining a second harness.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="netadapter-contract-") as directory:
        output = Path(directory)
        subprocess.run([
            "python3", str(ROOT / "librt/libgracht/generator/parser.py"),
            "--service", str(ROOT / "protocols/contracts/netadapter.gr"),
            "--out", directory, "--lang-c", "--client", "--server",
        ], check=True)
        client = output / "ctt_netadapter_service_client.c"
        for operation in ("post_rx_batch", "submit_tx_batch", "acknowledge", "drain"):
            if f"ctt_netadapter_{operation}_result(" in client.read_text():
                raise AssertionError(f"{operation} must not require a synchronous result")
        header = (output / "ctt_netadapter_service_client.h").read_text()
        for retired in ("post_rx", "submit_tx", "start", "stop", "ack_batches", "ack_completions", "drain_completions"):
            if f"ctt_netadapter_{retired}(" in header:
                raise AssertionError(f"retired v1 method {retired} was regenerated")
        # ASan retains protocol callback tables as instrumented globals even if
        # the harness never registers them. Supply aborting callbacks for the
        # unrelated operations using their generated signatures, not duplicates
        # of the schema. Accidentally dispatching one must fail the test.
        implemented = {"ctt_netadapter_event_" + name + "_invocation" for name in
                       ("batch_admitted", "completions", "drain_end", "ack_progress")}
        stubs = ['#include <stdlib.h>', '#include "ctt_netadapter_service_client.h"',
                 '#include "ctt_netadapter_service_server.h"']
        for side in ("client", "server"):
            header = (output / f"ctt_netadapter_service_{side}.h").read_text()
            for name, parameters in re.findall(r"void (\w+_invocation)\(([^;]+)\);", header):
                if name in implemented:
                    continue
                unused = ["(void)" + re.search(r"(\w+)$", p.strip()).group(1) + ";"
                          for p in parameters.split(",")]
                stubs.append(f"void {name}({parameters}) {{ {' '.join(unused)} abort(); }}")
        stub_file = output / "unused_callbacks.c"
        stub_file.write_text("\n".join(stubs))
        executable = output / "contract-test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                   "-Werror", "-Wno-unused-function", "-Wno-sign-compare", "-ffunction-sections", "-fdata-sections",
                   "-I" + str(ROOT / "testing/include"),
                   "-I" + str(ROOT / "librt/libos/include"),
                   "-I" + str(ROOT / "librt/libgracht/include"),
                   "-isystem", directory,
                   str(ROOT / "testing/netadapter_contract_test.c"), str(client),
                   str(output / "ctt_netadapter_service_server.c"),
                   str(stub_file),
                   "-Wl,--gc-sections", "-o", str(executable)]
        command.extend(shlex.split(os.environ.get("NETADAPTER_TEST_CFLAGS", "")))
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
