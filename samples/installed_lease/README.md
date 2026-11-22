# Independent installed MadObs C clients: exporter failure and WAL lease handoff

This Linux host fixture uses the existing null-provider static CMake package and
public `madobs/private.h`. Run it with existing CMake, GNU Make, GCC/G++ and
system zlib development files:

```sh
python3 -I -B samples/installed_lease/test_verify.py
python3 -I -B samples/installed_lease/verify.py
```

The gate builds and installs from a disposable producer, moves the installation
to a spaced prefix and deletes the producer before configuring either consumer.
The holder and contender are separate C11 projects and processes. Both must link
the exact same relocated `MadObs::runtime` archive. Neither reads library
sources. The gate inventories the package and checks its bytes after use.

1. Holder A admits one synthetic event and a local callback validates its
   canonical policy batch and refuses it with `done(0)`.
2. With A still running, contender B rejects A's event under B's disjoint
   policy, admits/drains its own event with a local discard callback, and must
   receive `WAL_BUSY` when attempting to take A's lease.
3. A stops but retains its C handle and process. B must reject the same path
   with a different policy, then open it using the exact policy. Any historical
   data offered at B's transferred-lease callback must be canonical for A's
   policy and is declined, never acknowledged. No count or replay is required.
   B rejects its own event under the transferred policy.
4. B's original runtime must independently drain its second event; two
   successful admissions, two drains, two policy-valid local batches and no
   queue/WAL drops are required. A's stopped stats and callback counts remain
   stable before its handle is destroyed.

The parent enforces the order with fixed pipe markers and deadlines, closes or
reaps fixture processes and removes only its disposable temp directories. No
sibling SDK, third-party downloads, network calls, model, WAL content reads by
the harness, raw payload/path reporting, or artifact upload are part of the
gate. It does not prove replay, crash/power-loss durability, remote delivery,
interoperability with real services, or a mobile package. The exact dedicated
Actions branch runs the full CMake gate; local direct-toolchain execution without
CMake counts only as behavior evidence, not installed-package evidence.
