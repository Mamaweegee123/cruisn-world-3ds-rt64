# Cruis’n World 3DS RT64 experiment

A separate experimental developer handoff exploring an OSTask-to-RT64-3DS bridge. This project is independent of the ARM11/software-RDP experiment and is not a playable port.

## Current state

The last documented Azahar Task230 run reached startup substate 3 (counter `0x78`) while graphics and VI activity continued. A later in-process Quick Load observation advanced a same-build state from counter 50 to 100, but cross-executable save-state compatibility is not established. No menu, race, player input, visible gameplay, or original-hardware support has been verified.

The RT64 adapter remains disconnected from the authentic game rendering path. Host tests use a synthetic FullSync/EndDL list; they do not prove game rendering.

## Build and tests

This is a **source handoff, not a standalone port build**. The host test target requires a separate pinned RT64-3DS renderer source tree. See [build requirements](BUILD_REQUIREMENTS.md). Tests were not run as part of preparing this candidate.

## Contents and licensing

This candidate includes the bridge/snapshot code, its host test, test runner, and CMake configuration. The scoped MIT notice covers those included owner-directed contributions only; it does not cover the external RT64-3DS dependency, generated/game-derived code, ROMs, assets, or Nintendo marks. See [third-party notices](THIRD_PARTY_NOTICES.md).

## Preservation note

> This repository is an incomplete experimental source handoff, not a full backup of the original development environment.
