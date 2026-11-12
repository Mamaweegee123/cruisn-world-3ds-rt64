# Development status

**Classification: PARTIALLY WORKING feasibility experiment; not playable.**

## Verified evidence
- The latest documented Task230 Azahar run reached startup substate 3 (`0x78`) with continuing graphics/VI activity.
- A same-build in-process Quick Load restored a counter-50 state and advanced to counter 100 in a later documented observation.
- The host adapter tests construct a synthetic FullSync/EndDL list and exercise the interpreter adapter; they do not use an authentic Cruis’n game task.

## Not established
No integrated RT64 game frame, visible gameplay, menus, input, race, or original hardware support has been verified. The adapter has not been connected to the actual graphics task completion path.

## Separation
This repository is not the ARM11 software-RDP port. It also does not include the external RT64-3DS source tree or the Phase 6 runtime-baseline build.

## Handoff checks
The candidate payload and source hashes were verified. The host test was not run because its required RT64-3DS renderer source is an external dependency and was not copied into this source-only candidate. No game build was attempted.
