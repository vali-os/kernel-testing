"""Exercise the real registry and session core with a deterministic IPC boundary."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

with tempfile.TemporaryDirectory(prefix="net-registry-test-") as directory:
    out = Path(directory)
    subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                    "--service", str(ROOT / "protocols/contracts/netadapter.gr"),
                    "--out", directory, "--lang-c", "--client"], check=True)
    # Only readiness and logging are replaced. Registry locking uses host C11
    # mutexes; lifecycle allocation/destruction uses the production session core.
    (out / "ddk").mkdir()
    (out / "ddk/utils.h").write_text('#define WARNING(...) ((void)0)\n#define ERROR(...) ((void)0)\n'
                                     '#define TRACE(...) ((void)0)\n')
    (out / "io.h").write_text('int write(int, const void*, unsigned int);\nint read(int, void*, unsigned int);\nint close(int);\n')
    (out / "event.h").write_text('#define EVT_RESET_EVENT 0\nint eventd(int, int);\n')
    (out / "ioset.h").write_text('''#include <time.h>
#define IOSET_ADD 0
#define IOSETSYN 1
struct ioset_event { unsigned events; union {int iod;} data; };
int ioset(int);
int ioset_ctrl(int, int, int, struct ioset_event*);
int ioset_wait(int, struct ioset_event*, int, struct timespec*);
''')
    includes = ["testing/include", "testing", "services/netd/adapters",
                "librt/libos/include", "librt/libgracht/include"]
    sources = ["testing/netadapter/registry_test.c", "testing/net_shm_mock.c"]
    sources += [f"services/netd/adapters/{name}.c" for name in
                ("adapter", "buffers", "queue", "rx", "tx", "scheduler", "events")]
    command = [os.environ.get("CC", "clang"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
               "-DVALI", "-DTIME_MONOTONIC=2", "-DSERVICEAPI=static inline", "-DSERVICEABI=",
               "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare",
               "-ffunction-sections", "-fdata-sections", "-I" + directory]
    command += ["-I" + str(ROOT / path) for path in includes]
    command += [str(ROOT / path) for path in sources]
    command += ["-pthread", "-Wl,--gc-sections", "-o", str(out / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(out / "test")], check=True)
