#!/usr/bin/env python3
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from shm_device_test import run_host_test

run_host_test('x86-boot-resources', 'machine.h', ('testing/x86-boot/resources_test.c',),
              extra_flags=('-D__amd64__', '-DPRIuIN="zu"', '-DPRIxIN="zx"',
                           '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-atomic-alignment', '-latomic'))
