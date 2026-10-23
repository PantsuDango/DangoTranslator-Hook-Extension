# Copyright and third-party notices

## Dango Hook integration

The Dango Hook integration sources and modifications are distributed under the
GNU General Public License, version 3. See LICENSE. Existing file-level notices
remain in effect. Standalone build changes are
recorded in CHANGES.md and SOURCE_MANIFEST.json.

## LunaTranslator / LunaHook

Upstream: https://github.com/HIllya51/LunaTranslator

Copyright (C) 2026 HIllya51

The original native Hook source and build-support snapshot is included under
third_party/LunaTranslator with its GPLv3 LICENSE. Integration changes are
provided as patches and additional sources under src/script. Component product
names do not replace upstream attribution.

The upstream snapshot has no verified Git commit identity. SOURCE_MANIFEST.json
identifies its files by SHA-256; a version string is not used as a commit ID.

## MinHook and Hacker Disassembler Engine

MinHook: https://github.com/TsudaKageyu/minhook

Copyright (C) 2009-2017 Tsuda Kageyu.
Portions copyright (c) 2008-2009, Vyacheslav Patkov.

The original BSD-style terms and file notices are retained in
third_party/minhook/LICENSE.txt. The NScripter native decoder also preserves
its notices in src/game/format/nscripter/native/LICENSE.txt.

## KiriKiri SDK

The SDK sources retain W.Dee and contributors' notices and original terms in
src/script/kirikiri/runtime/sdk/license.txt. See THIRD_PARTY_LICENSES.txt for
the collected license texts.

## External compilation and integration dependencies

Unity/.NET reference assemblies are external compilation interfaces and are not
bundled. Application-generated display resources are not included. Each external
dependency retains its own license and terms.

## Source Han Sans

The Hook display font `config/SourceHanSansSC-Regular.otf` retains its original
SIL Open Font License, version 1.1. See `config/SourceHanSansSC-LICENSE.txt`.
The font is not relicensed under the GPL. Keep its license with redistributed
font files.
