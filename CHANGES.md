# Changes

## 2026-11-13 — DangoTranslator 7.0.4

- Synchronized the KiriKiri runtime with the 7.0.4 release and added the
  64-bit KiriKiri Z SDK runtime (`KiriKiriRuntime64.dll`) and injector
  (`KiriKiriInjector64.exe`); the 64-bit path does not build a LunaHook-derived
  Host/Bridge/Hook chain.
- Ren'Py now builds x86 and x64 components; updated the Ren'Py patches,
  Python 2.7/3 interpreter entry points and the injected Python bridge.
- Updated the shared Host source (`livemaker/main.cpp`) and the Unity Mono
  reader (`Reader.cs`, `teamnimbus.hpp`), and added `TimelineReader.cs` and the
  compile-only `UnityEngineReference.cs` reference stub.
- Removed the CatSystem2 adapter: its Hook components were replaced by an
  independent implementation that no longer contains LunaHook code. The previous
  LunaHook-derived CatSystem2 sources remain available in the 7.0.3 history.

## 2026-10-31 — DangoTranslator 7.0.3

- Synchronized the Unity Mono Hook runtime with the 7.0.3 release.
- Added the managed `LiveReader.cs` runtime used by the real-time display path.
- Updated `Reader.cs`, `teamnimbus.hpp`, and the Unity Mono build references for
  older Mono runtimes and modular Unity assemblies.
- Kept the repository scoped to Hook runtimes and their build inputs; the
  independent resource tools and desktop client remain outside this repository.

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
