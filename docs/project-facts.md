# Project facts

Verified engineering facts for ba-click-fx-desktop. Sources: source reading and git statistics on `main` @ `5b990e5` (2026-10-19); full analysis in [docs/analysis/2026-10-19-project-deep-analysis.md](analysis/2026-10-19-project-deep-analysis.md).

## Scale and shape

- ~103,300 lines of C++ under `src/`; 353 source files; 10 modules: `windows` (71 files), `desktop` (59), `control-center` (41), `core` (21), `fx` (9), `capture` (11), `config` (4), `identity-signer` (6), `reference` (6), `release_update` (4).
- 1,295 commits, 2026-08-09 → 2026-10-14; 27 released versions; current 0.3.1.
- Largest files: `src/desktop/main.cpp` (5,628 lines), `src/control-center/control_center_window.cpp` (4,647), `src/config/src/config.cpp` (3,700).

## Architecture boundaries that the code actually enforces

- Host is the sole runtime writer of `BAFX.config.json` (schema 20; atomic write via temp file + `MoveFileExW(WRITE_THROUGH)`; 1 MiB load cap). Control Center only sends validated commands over `\\.\pipe\BAFX.Host.v1` (single instance, `REJECT_REMOTE_CLIENTS`; patch commands carry `expectedGeneration` optimistic locking).
- One `DisplaySession` per monitor owns an independent D3D11 device, swapchain and WGC sensor (Adapter Resource Domain); cross-adapter moves rebuild, never share.
- WGC frames are borrowed: copy to owned texture, then `Close` immediately; a resource ledger counts live frames/pools/sessions.
- Background path is a binary contract: Differential Bloom and the final source-over payload enable or fall back to FX-only together (`BackgroundPathLatch`).
- FX profiles save `effects` only; ≤64 custom profiles; built-ins are not deletable.
- Render Owner single-thread ownership is convention + diagnostics only — not runtime-enforced (D3D11 device has no multithread protection).

## CI and verification boundaries

- GitHub CI (`windows-build-compat.yml`): build-compat matrix (SDK 19041/22621/26100, no tests) + `test-contracts` (`ctest -LE 'hardware|integration'`, no vcpkg/Spout2/smoke). **Release verification has no CI gate** — `release-verify` presets run locally by hand.
- Self-rolled C++ test framework (`tests/test_support.hpp`, static registry + `BAFX_TEST`); ~626 cases; 21 Python contract/acceptance scripts; process-boundary probes.
- Known 2026-10-14 local CTest run: 46/47 — `hotkey_process_boundary` timed out locally while remote CI passed; root cause unconfirmed (recorded in CHANGELOG).

## Known issues (from the 2026-10-19 deep analysis, severity-ordered)

- P1 `src/desktop/main.cpp` `runApplication` is one ~5,600-line function (main loop + device recovery + topology reconciliation + background transactions). Refactoring must update the source-text contract tests in `tests/host_render_loop_snapshot_contract_tests.py`.
- P1 Release assets have no automated verification gate.
- P2 Local IPC has no authentication: any same-user process can `Shutdown` or reconfigure the Host (default pipe ACL + `Local\` session isolation only).
- P2 Documentation drift: `docs/ROADMAP.md` still states version 0.2.19 (actual 0.3.1); ADR index review date 2026-08-25 is stale.
- P2/P3 Zero-test areas: multi-display coordination (`display_session_manager`, `coordinator_output_runtime`, `secondary_display_runtime`, `display_pointer_router`), `spout2_sender`, `winhttp_update_transport`, `appx_signer`, `host_hotkeys` registration, `control_center_display/hotkeys/refresh`.
- All 10 ADRs are Proposed. Windows 11 borderless WGC, HDR, multi-monitor and cross-adapter paths are implemented but **Not Run** on real hardware — never cite them as verified.
