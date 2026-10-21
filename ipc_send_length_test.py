"""Test production IPC length handling with stubbed kernel/queue dependencies.

The reservation fixture mutates the caller's descriptor at the point where a
full queue would wait. This isolates the send contract from queue scheduling.
Run with: python3 testing/ipc_send_length_test.py
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="vali-ipc-send-length-") as directory:
    output = Path(directory)
    headers = {
        "ddk/barrier.h": "",
        "debug.h": "#define TRACE(...)\n#define ERROR(...)\n",
        "handle.h": """
#include <os/types/ipc.h>
oserr_t LookupHandleByPath(const char*, uuid_t*);
""",
        "handle_set.h": """
#include <os/types/ipc.h>
oserr_t MarkHandle(uuid_t, unsigned int);
""",
        "memoryspace.h": """
#include <os/types/ipc.h>
typedef struct MSContextSecurity {
    uint64_t Sender;
    _Atomic(uint32_t) Capabilities;
} MSContextSecurity_t;
MSContextSecurity_t* MemorySpaceGetSecurityContext(void);
""",
        "ioset.h": "#define IOSETIN 1\n",
        "shm.h": """
#include <os/types/ipc.h>
oserr_t SHMKernelMapping(uuid_t, void**);
""",
    }
    for name, content in headers.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    binary = output / "test"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-DVALI", "-DKERNELAPI=", "-DKERNELABI=", "-UNDEBUG",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
               "-I" + directory]
    command += ["-I" + str(ROOT / path) for path in
                ("testing/include", "kernel/include", "librt/libos/include", "librt/libds/include")]
    command += [str(ROOT / "testing/ipc_send_length_test.c"),
                str(ROOT / "kernel/ipc.c"), "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
