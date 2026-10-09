# DeviceTree reader and retained tree checks

The allocation-free parser lives in `librt/libfdt`. The loader, kernel, and
service compile the same source with their own ABI and register policy. Kernel
allocation and resource ownership remain in `kernel/devicetree`.

Run the retained-tree tests with Clang and sanitizer runtimes installed:

```sh
python3 testing/devicetree/test_tree.py
```

They exercise forward references, aliases, high-address translation, allocation
failure cleanup, malformed input, and real firmware trees. The loader's parser
consumers are covered by:

```sh
python3 testing/rpi-loader/test_platform.py
python3 testing/rpi-loader/test_resources.py
python3 testing/rpi-loader/test_pe.py
```

The service reader's borrowed views and publication boundary are covered by
`testing/firmware/reader_test.c`, built by `testing/firmware/test.sh`. This links
the shared parser without any kernel tree allocation or controller policy.

Parser callbacks may accumulate private state while traversing. They must not
publish it until the complete parse succeeds: a valid prefix cannot establish a
valid tree. Names and values borrow the immutable input. All consumers preserve
this contract even though they have different ownership and publication models.
