# clumsy-jitter: per-handle WinDivert heartbeat and status dot

This builds on the earlier optimized Clumsy-jitter + Re-apply version.

## User interface

There is a small 8px circular indicator immediately to the right of Re-apply:

- **Grey:** stopped, or Start failed. No warnings.
- **Green:** watchdog enabled; the last driver ping was acknowledged; packet worker recently progressed.
- **Red:** heartbeat call failed, worker stalled, or no recent kernel acknowledgment. A single status-line warning is shown on entering failure. **Re-apply** restarts the interception handle and resets the warning.

The existing left-hand traffic activity indicator is retained. Module toggles and delay/jitter controls keep their existing live behavior; filter changes still need Re-apply. Re-apply does not stop the shared WinDivert service.

## Heartbeat and failure semantics

- A **per-handle**, **opt-in** watchdog: 1-second heartbeat and 3-second kernel lease.
- The heartbeat thread only sends acknowledgments if the packet-processing clock thread has progressed within 2 seconds. Idle receives waiting in `WinDivertRecv()` do not count as a hang; Clumsy's 40ms clock is responsible for continuing progress on idle links.
- The kernel independently checks the lease in the WFP network classify path, and once expired **allows new packets to continue** without diversion by this handle. The expired lease cannot be reactivated on the same handle; close/reopen with Re-apply.
- The kernel API uses custom WinDivert parameter codes: `WATCHDOG_TIMEOUT=100`, `WATCHDOG_PING=101`, `WATCHDOG_STATUS=102`. These are NOT features of stock WinDivert 2.2.
- Packets already intercepted/queued at the moment of failure **may be lost**. Fail-open applies to **new packets** evaluated after lease expiration.
- Multiple applications are isolated by their distinct WinDivert handles. Existing clients do not have the watchdog enabled.
- A green dot verifies watchdog communication and worker progress, **not** comprehensive kernel correctness. A full kernel hang may prevent the GUI from updating.

## Build and installation

1. In your `xyzzyx99/WinDivert` fork, run the supplied `apply_windivert_watchdog.py` from the repository root (on a clean branch), commit `include/windivert.h` and `sys/windivert.c`, and run your **existing optimized/test-signed driver** workflow.
2. In Clumsy-jitter, use `.github/workflows/clumsy-jitter-watchdog.yml` to build `clumsy.exe` (x64 only). This workflow is **new** and does not modify the original one.
3. Download the Clumsy artifact, and copy your **patched test-signed** `WinDivert64.sys` alongside the Clumsy executable. Do **not** use the stock driver extracted from upstream: it lacks the watchdog parameters.
4. `WinDivert.dll` from the 2.2 series works because its existing `WinDivertSetParam`/`WinDivertGetParam` functions forward the parameter values without DLL modifications. A DLL of an incompatible API version may not work.
5. On a dedicated driver-test VM, trust the corresponding public test certificate, enable the required test-signing configuration, and run Clumsy as administrator. A `577` error means Windows refused the driver signature/policy.
6. Close other WinDivert clients, unload prior resident driver services before swapping `.sys` builds, and check `sc.exe qc WinDivert` for the configured driver path.

## Manual validation checklist (Windows VM)

- Start with the custom driver and confirm **green**; Stop shows **grey**; failed Start also shows **grey**.
- Re-apply while running resets the handle; verify the new filter works and the dot returns to green.
- Test intentionally stalled worker progress in a VM; verify red warning appears once, and newly arriving packets pass through after 3s.
- Verify stock WinDivert rejects Start with a readable watchdog-not-supported message (rather than showing green).
- Repeat with an unrelated WinDivert client open to confirm only Clumsy's handle is affected.
- Observe Wireshark/ping/DNS or a packet capture during failure; packets already intercepted might be dropped.

**Not yet verified on a Windows build or kernel test VM.** Treat this as an experimental kernel change; do not deploy on the main workstation without validating with Driver Verifier and network stress tests.
