# Durable semaphore notification delivery tests

Date: 2026-10-02

These host fixtures compile the current `p4_cleanup_worker.c` implementation
directly. They cover its protected-worker delivery path and the service loop's
notification/cleanup alternation. Registry claim/finish transactions are
mocked here; their actual registry tests are separate work. No hardware,
configured SMP image, or concurrent-hart behavior is qualified by these runs.

## Results

The adapted cleanup-worker fixture passes 450 checks at both `-O1` and `-O2`.
The earlier 314-check fixture recorded on 2026-10-01 predates semaphore
notification delivery, so the counts are contextual rather than a direct
before/after coverage comparison. The adapted service-loop case delivers two
durable batches, processes one cleanup item between them, checks the alternating
drain order, then injects a wake at the drain-to-`Wait` edge and confirms the
second pass observes the sticky signal.

The new delivery fixture passes 241 checks at both optimization levels. It
checks empty and malformed claims, live delivery and queue-effect order, retired
target `NOT_LIVE` consumption, absence of a public `Signal()` call, and a
reentrant adapter hook that completes/recycles the operation and publishes a
new pending batch before the old snapshot is finished. The snapshot has only
identity/count/generation fields; the old snapshot's exact count and generation
are finished while the newly published count and pins remain. The mock durable
reservation keeps both live and retired target storage available through the
last adapter access.

## Reproduction

Run from the repository root with Apple Clang 21.0.0. Both fixtures use strict
C11 warnings, AddressSanitizer and UndefinedBehaviorSanitizer. Each executable
run is bounded to 15 seconds; leak detection is disabled for these stack-only
fixtures.

```sh
clang -std=c11 -O1 -g -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer arch/riscv-esp32p4/exec/tests/p4-cleanup-worker-test.c -o /tmp/p4-semaphore-notification-delivery-20261002-egHUQR/p4-cleanup-worker-final-o1
clang -std=c11 -O2 -g -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer arch/riscv-esp32p4/exec/tests/p4-cleanup-worker-test.c -o /tmp/p4-semaphore-notification-delivery-20261002-egHUQR/p4-cleanup-worker-final-o2
clang -std=c11 -O1 -g -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer arch/riscv-esp32p4/exec/tests/p4-semaphore-notification-delivery-test.c -o /tmp/p4-semaphore-notification-delivery-20261002-egHUQR/p4-semaphore-notification-delivery-final-o1
clang -std=c11 -O2 -g -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer arch/riscv-esp32p4/exec/tests/p4-semaphore-notification-delivery-test.c -o /tmp/p4-semaphore-notification-delivery-20261002-egHUQR/p4-semaphore-notification-delivery-final-o2

python3 -c 'import os,subprocess,sys; e=os.environ.copy(); e["ASAN_OPTIONS"]="detect_leaks=0:halt_on_error=1"; e["UBSAN_OPTIONS"]="halt_on_error=1"; subprocess.run([sys.argv[1]],env=e,check=True,timeout=15)' /tmp/p4-semaphore-notification-delivery-20261002-egHUQR/p4-cleanup-worker-final-o1
python3 -c 'import os,subprocess,sys; e=os.environ.copy(); e["ASAN_OPTIONS"]="detect_leaks=0:halt_on_error=1"; e["UBSAN_OPTIONS"]="halt_on_error=1"; subprocess.run([sys.argv[1]],env=e,check=True,timeout=15)' /tmp/p4-semaphore-notification-delivery-20261002-egHUQR/p4-cleanup-worker-final-o2
python3 -c 'import os,subprocess,sys; e=os.environ.copy(); e["ASAN_OPTIONS"]="detect_leaks=0:halt_on_error=1"; e["UBSAN_OPTIONS"]="halt_on_error=1"; subprocess.run([sys.argv[1]],env=e,check=True,timeout=15)' /tmp/p4-semaphore-notification-delivery-20261002-egHUQR/p4-semaphore-notification-delivery-final-o1
python3 -c 'import os,subprocess,sys; e=os.environ.copy(); e["ASAN_OPTIONS"]="detect_leaks=0:halt_on_error=1"; e["UBSAN_OPTIONS"]="halt_on_error=1"; subprocess.run([sys.argv[1]],env=e,check=True,timeout=15)' /tmp/p4-semaphore-notification-delivery-20261002-egHUQR/p4-semaphore-notification-delivery-final-o2
```

Observed output, in command order:

```text
P4 cleanup-worker checks: 450 passed
P4 cleanup-worker checks: 450 passed
P4 semaphore-notification-delivery checks: 241 passed
P4 semaphore-notification-delivery checks: 241 passed
```

## Source and artifact identities

Current worker source SHA-256: `p4_cleanup_worker.c`
`6643e6ecb579e981f0d13a8f51f5b94c3c65dbfcc368adb171027447747d51ab`;
`p4_cleanup_worker.h`
`47427be628ed4e2a69405af6e245ff4d9ee24e6117e4f18205bcdcc1fe8d4172`.

Fixture source SHA-256: `p4-cleanup-worker-test.c`
`09f8f5d1f05c81811b04982711ce448d9ce7f8f04ef6cc75aa63d28264174221`;
`p4-semaphore-notification-delivery-test.c`
`096cf1a1ef141548dbf2c61442274be6d405a9f1567ea6c5a4624ff0ee64e166`.

| Fresh executable | Size | SHA-256 |
| --- | ---: | --- |
| `p4-cleanup-worker-final-o1` | 112,104 bytes | `8904efe4489e30ff719bfee80e219821369c4619295efa3ece1dd3b54b026269` |
| `p4-cleanup-worker-final-o2` | 112,104 bytes | `5a02f7137e28f76ce6eadb904514ae52ea2348f743965168710ebf83d504de87` |
| `p4-semaphore-notification-delivery-final-o1` | 85,192 bytes | `eb5f1ebdf1f1b497e07affda61e117a7043d06cc744c1d7670644eea1bf9d4d4` |
| `p4-semaphore-notification-delivery-final-o2` | 85,192 bytes | `91cadf6ddc86ae995b725440c364b53c9cb516b726250e56dd7a777ede620360` |
