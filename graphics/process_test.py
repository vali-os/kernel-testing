"""Run the real server and two independent client processes without sleeps."""

import selectors
import subprocess
import sys
import uuid


def main():
    name = "vali-graphics-" + uuid.uuid4().hex
    clients = []
    server = subprocess.Popen(
        [sys.argv[1], name, *sys.argv[3:]], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    try:
        # Read readiness from the server; elapsed time alone does not establish
        # that the socket and protocols have finished registration.
        with selectors.DefaultSelector() as selector:
            selector.register(server.stdout, selectors.EVENT_READ)
            if not selector.select(timeout=5):
                raise RuntimeError("graphics server did not become ready")
        if server.stdout.readline().strip() != "READY":
            raise RuntimeError("graphics server failed during startup")
        for _ in range(2):
            clients.append(subprocess.Popen(
                [sys.argv[2], name], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
            ))
        for client in clients:
            output, error = client.communicate(timeout=5)
            if client.returncode:
                raise RuntimeError(f"graphics client failed: {output}{error}")
            print(output, end="")
        output, error = server.communicate(timeout=5)
        if server.returncode:
            raise RuntimeError(f"graphics server failed: {output}{error}")
        print(output, end="")
    finally:
        for process in clients + [server]:
            if process.poll() is None:
                process.kill()
            process.communicate()


if __name__ == "__main__":
    main()
