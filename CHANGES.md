# Changes

## 2026-10-23

- Added standalone build entry points for 33 LunaHook-derived engine adapters.
- Included the original LunaHook native source and MinHook dependency snapshots,
  their license texts, and the shared layout data required by Hook builds.
- Preserved the upstream `Copyright (C) 2026 HIllya51` declaration in component
  resource patches and added build-time copyright checks.
- Made build paths repository-relative and compiled Host/Injector from source.
- Added source-preparation checks and normalized patch line endings to LF.
- Limited the repository to Hook runtime sources, managed bridges and build
  dependencies. Build scripts do not package independent resource tools.
- Included the runtime display font with its original SIL Open Font License;
  retained component font lookup and packaging.
