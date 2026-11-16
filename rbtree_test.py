"""Exercise the red-black tree: invariants, ordered navigation and concurrent removal."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "librt/libds/rbtree.c"


def build(flags, output, name, source_override=None):
    binary = output / name
    command = flags[:]
    if source_override is not None:
        command += ["-DRBTREE_SOURCE=\"" + source_override.as_posix() + "\""]
    subprocess.run(command + [str(ROOT / "testing/rbtree_test.c"), "-o", str(binary)], check=True)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-pthread"]
    flags += ["-I" + str(ROOT / path) for path in ("testing/include", "librt/libds/include", "librt/libddk/include")]
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    with tempfile.TemporaryDirectory(prefix="vali-rbtree-") as directory:
        output = Path(directory)
        subprocess.run([str(build(flags, output, "rbtree_test"))], check=True,
                       env=environment, timeout=120)
        if not args.mutation_check:
            return
        mutations = (
            ("floor walks the wrong way", "if (result < 0) {\n            best = i;\n            i = i->right;",
             "if (result > 0) {\n            best = i;\n            i = i->right;"),
            ("successor climbs the wrong side", "leaf == parent->right",
             "leaf == parent->left"),
            ("remove releases the lock between lookup and unlink",
             "    TREE_LOCK;\n    leaf = find_leaf(tree, key);\n    if (!leaf) {\n        TREE_UNLOCK;\n        return NULL;\n    }\n",
             "    leaf = rb_tree_lookup(tree, key);\n    if (!leaf) {\n        return NULL;\n    }\n    TREE_LOCK;\n"),
        )
        original = SOURCE.read_text()
        for name, before, after in mutations:
            assert original.count(before) == 1, name
            mutant = output / "rbtree_mutant.c"
            mutant.write_text(original.replace(before, after, 1))
            binary = build(flags, output, "rbtree_mutant_test", mutant)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    env=environment, timeout=120)
            if result.returncode == 0:
                raise RuntimeError("Mutation was not detected: " + name)
            print("Red-black tree: " + name + " mutation rejected")


if __name__ == "__main__":
    main()
