# clumsy-jitter — Optimized Release / Re-apply button

This variation uses the uploaded clumsy-jitter sources. The existing
`.github/workflows/clumsy-jitter-build.yml` is **not changed**.

## GUI behavior

* A **Re-apply** button appears immediately to the right of **Start / Stop**.
* Before Start and after Stop, **Re-apply** is disabled (greyed out).
* When filtering starts successfully, **Re-apply** is enabled.
* Clicking **Re-apply** stops packet interception, flushes the outstanding packet
  queues using the same logic as Stop, and immediately starts interception again
  using the current filter and module settings.
* The filter text may now be edited while running. To apply a changed filter,
  click Re-apply. Other module controls retain their original behavior.
* If the restart fails (e.g. invalid filter), the application stays stopped,
  **Start** is available, and **Re-apply** stays disabled. The error is shown
  in the status message.
* Re-apply restarts **this application's WinDivert interception handle**; it
  does NOT stop, delete, or restart the Windows kernel driver service. This
  avoids affecting other programs that may be using WinDivert.
* A resource cleanup fix closes three Win32 handles after every successful Stop
  so repeated Re-apply operations do not leak thread/mutex handles.

## Optimized build

The new manual GitHub Actions workflow is
`.github/workflows/clumsy-jitter-optimized.yml`. It creates both x64 and x86
Release artifacts, with `/O2 /Oi /Gy /GL` compiler switches and
`/LTCG /OPT:REF /OPT:ICF` linker switches. The optional `enable_avx2`
workflow input applies `/arch:AVX2` to x64 only (requires a suitable CPU).

The optimization switches are injected by `genie.lua` **only** when
`CLUMSY_OPTIMIZED=1` during project generation. Normal project generation and
original workflow keep the prior build settings.

The source archive doesn't contain WinDivert or IUP precompiled dependencies.
The workflow fetches those binary folders from the original `jagt/clumsy`
repository if they are missing. It builds **clumsy.exe** only, not WinDivert.
The artifact contains upstream WinDivert binaries; to use a separately compiled
optimized WinDivert build, replace the DLL and driver with a compatible pair.
A modified driver must satisfy Windows kernel signing requirements.

## Install

1. Upload these sources to your fork of clumsy-jitter, preserving paths.
2. Commit and push `.github/workflows/clumsy-jitter-optimized.yml`,
   `src/main.c`, `src/divert.c`, `genie.lua`, and `README-optimized.md`.
3. Open GitHub > Actions > **Build clumsy-jitter (Optimized + Re-apply)** >
   **Run workflow**; choose AVX2 if desired.
4. Download `clumsy-jitter-optimized-x64` artifact and extract its inner ZIP.
5. Run `clumsy.exe` as Administrator. The x86 build is intended for 32-bit
   Windows; this code refuses to run as 32-bit on 64-bit Windows.

## Notes

No Windows build or runtime testing was possible in the Linux build environment.
The YAML, source changes, and patch were inspected; a GitHub Actions build is
required to validate MSVC/GENie compatibility, and driver behavior needs a
Windows test environment. Work with a test interface to avoid disrupting DNS,
SSH, or other traffic.
