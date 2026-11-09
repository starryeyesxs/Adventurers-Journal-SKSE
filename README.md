# AdventurersJournal

AdventurersJournal is a personal learning and immersion project for Skyrim
Special Edition. It adds an in-game journal interface built with Prisma UI,
including quest details, Field Notes, and a portrait album. This project was
built with AI assistance.

## In-Game Requirements (For Players)
- Skyrim Special Edition
- SKSE64
- Address Library for SKSE Plugins
- Prisma UI

## Build Requirements (Only If Compiling From Source)
- Xmake 3.0 or newer
- A C++23-capable MSVC toolchain (Visual Studio)


CommonLibSSE-NG is included as a Git submodule. The build also uses
`nlohmann/json` 3.12.0 through Xmake.

## Build

Initialize the submodule and build/package the plugin:

```powershell
git submodule update --init --recursive
xmake -y
```

The build produces the plugin DLL and creates a distributable folder under
`dist/AdventurersJournal_<version>/`, with the Prisma UI view and SKSE plugin in
their expected directory structure. `build/`, `.xmake/`, and `dist/` are local
generated output and are not tracked.

## Credits and third-party software

- **SKSE64** — Skyrim Script Extender, required for the native plugin.
- **Address Library for SKSE Plugins** — runtime address support used by the
  Skyrim plugin ecosystem.
- **Prisma UI** — in-game UI framework used to display the journal.
- **CommonLibSSE-NG** — Skyrim plugin development library, included as a
  submodule. See its repository and license for its terms.
- **nlohmann/json** — JSON for Modern C++, version 3.12.0, licensed under the
  MIT License.

Each third-party project and dependency remains subject to its own license and
terms. This README credits these projects; it does not imply endorsement.

## Project status

This is a personal project. Contributions, compatibility, and releases are not
guaranteed.
