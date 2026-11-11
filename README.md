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

## Quest history

Quest timestamps, completion ordering, tracked-quest snapshots, and field notes
are stored in each save's SKSE cosave. Existing version 1 quest-time records
are read when loading older saves. The journal only shows quests present in the
loaded game's live quest data; it does not import quest history from shared
user-data files. On initialization, visible existing active quests without an
acceptance timestamp and completed quests without a completion timestamp are
assigned the current in-game date. These approximate dates are saved in the
cosave on the next save and are not the quests' actual historical dates.
Portrait metadata remains shared in `UserData/sketch-metadata.json`.

The Active Quests index groups tracked, main, guild, Civil War, side, mod, and
miscellaneous quests. Category expansion preferences are shared across saves in
`UserData/quest-categories.json`; Tracked resets to expanded whenever the journal
opens. Quest details show the quest's underlying category.

`Data/SKSE/Plugins/AdventurersJournal.ini` is created with defaults if missing.
Edit it as UTF-8 before starting the game to customize category labels, pause
the game while the journal is open, set the default Tasks & Errands expansion,
or hide plugin names in Chronicles detail tags. An existing saved category
preference takes precedence over the Tasks default. The INI and category
preferences are shared across saves and do not alter save data.
Keep your customized INI when installing an update; the release includes a
default copy and the plugin never overwrites an existing INI on startup.

Active quests can be hidden with Abandon Quest and restored from the dedicated
Abandoned Quests index, accessible from Active Quests. Hidden quests are stored
per save, excluded from both quest lists, and are untracked when abandoned;
their underlying quest progress is unchanged.

Active quest details include Find on Map. Ordinary objective rows remain
read-only: Skyrim tracks these at quest level, and changing their displayed
state can affect quest scripts.

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
