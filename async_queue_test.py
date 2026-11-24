"""Run the asynchronous queue and futex wait records under ASan/UBSan.

--mutation-check also builds copies of kernel/sync/async_queue.c and
kernel/sync/futex.c with one known mistake each, and requires every copy to
fail at an assertion.
"""
import argparse
from pathlib import Path
import tempfile

from shm_device_test import run_host_test

ROOT = Path(__file__).resolve().parents[1]
QUEUE = "kernel/sync/async_queue.c"
FUTEX = "kernel/sync/futex.c"
SOURCES = ("testing/async_queue_test.c", QUEUE, FUTEX, "librt/libds/list.c")
FLAGS = ("-D__LIBDS_KERNEL__", "-D_GNU_SOURCE")

# FutexCompleteAsyncWait checks the cookie again under the futex lock. That only
# matters when a record is woken and reused between AsyncQueueCancel's lookup
# and that lock, which these single-threaded checks cannot reach, so it has no
# mutation here.
MUTATIONS = (
    ("use a capacity that is not a power of two", QUEUE,
     "        capacity *= 2;\n",
     "        capacity += 1;\n"),
    ("reserve one record more than the ring holds", QUEUE,
     "if (queue->ReservedCount + __UnreadEntries(queue) < queue->Capacity && queue->FreeWaits != NULL) {",
     "if (queue->ReservedCount + __UnreadEntries(queue) <= queue->Capacity && queue->FreeWaits != NULL) {"),
    ("trust a ReadIndex ahead of WriteIndex", QUEUE,
     "    if (unread > queue->Capacity) {\n        return queue->Capacity;\n    }\n",
     ""),
    ("keep the reservation count when a record is freed", QUEUE,
     "    queue->ReservedCount--;\n",
     ""),
    ("leave queued records behind on destroy", QUEUE,
     "            FutexCancelAsyncWait(&queue->Waits[i]);\n",
     "            (void)i;\n"),
    ("queue a record without checking the value", FUTEX,
     "    if (atomic_load(futex) != expectedValue) {\n        SpinlockReleaseIrq(&futexItem->BlockQueueSyncObject);\n        AsyncQueueReservationCancel(wait);",
     "    if (atomic_load(futex) != expectedValue && false) {\n        SpinlockReleaseIrq(&futexItem->BlockQueueSyncObject);\n        AsyncQueueReservationCancel(wait);"),
    ("complete records from a threads-only wake", FUTEX,
     "return __FutexWake(futex, count, flags, __FUTEX_WAKE_THREADS_ONLY);",
     "return __FutexWake(futex, count, flags, __FUTEX_WAKE_ALL_WAITERS);"),
    ("end a timed out record without unlinking it", FUTEX,
     "        (void)list_remove(&futexItem->BlockQueue, &wait->Header);\n        (void)atomic_fetch_sub(&futexItem->Waiters, 1);\n        completedQueue = AsyncQueueComplete(wait, status);",
     "        (void)atomic_fetch_sub(&futexItem->Waiters, 1);\n        completedQueue = AsyncQueueComplete(wait, status);"),
    ("report a timeout as a normal result", FUTEX,
     "        completedQueue = AsyncQueueComplete(wait, status);",
     "        completedQueue = AsyncQueueComplete(wait, OS_EOK + (status - status));"),
    ("wake threads before records", FUTEX,
     "    if (mode == __FUTEX_WAKE_ALL_WAITERS) {\n        waiter = list_front(&futexItem->BlockQueue);\n    } else {",
     "    if (mode == __FUTEX_WAKE_ALL_WAITERS && false) {\n    } else {"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    run_host_test("async-queue-test", "async_queue.h", SOURCES, FLAGS)
    if not args.mutation_check:
        return

    for name, path, old, new in MUTATIONS:
        original = (ROOT / path).read_text()
        assert original.count(old) == 1, name
        with tempfile.TemporaryDirectory(prefix="vali-async-queue-mutation-") as directory:
            mutant = Path(directory) / Path(path).name
            mutant.write_text(original.replace(old, new))
            sources = tuple(str(mutant) if source == path else source for source in SOURCES)
            run_host_test("async-queue-mutant", "async_queue.h", sources, FLAGS, mutation=name)
            print("Rejected mutation:", name, flush=True)


if __name__ == "__main__":
    main()
