# Plan: Lutris support for ostool

**Status: not started.** Paused until the Lutris library holds only games the user owns (GOG, Epic,
Battle.net, itch, etc.). No Lutris code exists yet. Everything below was worked out from reading the
Lutris file layout on the development machine (read-only) and from the current ostool design.

## Goal

`ostool detect / install / verify / uninstall` work on games managed by Lutris's Wine runner, with
the same guarantees as Steam games: dry run by default, hashed manifest, byte-exact undo.

Out of scope: editing Lutris's own config files, installer scripts, non-Wine runners (DOSBox, native
Linux, ...), Lutris "steam" entries (Steam discovery already covers those), 32-bit games.

## What Lutris stores (verified on this machine)

* **Database:** `~/.local/share/lutris/pga.db` (SQLite), table `games`. Columns seen: `id, name,
  sortname, slug, installer_slug, parent_slug, platform, runner, executable, directory, updated,
  lastplayed, installed, installed_at, year, configpath, has_custom_banner, has_custom_icon,
  has_custom_coverart_big, playtime, service, service_id, discord_id`.
  Use `runner = 'wine'` and `installed = 1`. Columns vary between Lutris versions: read by name.
* **Per-game config:** `~/.config/lutris/games/<configpath>.yml`. The part that matters is the
  top-level `game:` mapping (`exe`, `prefix`, sometimes `working_dir`, `arch`). The file also holds a
  large `script:` subtree with placeholders (`$GAMEDIR`, `_xXx_AUTO_WIN32_xXx_`): **ignore everything
  under `script:` and `installer:`**. `exe` can be empty (`''`).
* **Other locations to try:** Flatpak Lutris (`~/.var/app/net.lutris.Lutris/{data,config}/lutris`),
  and `XDG_DATA_HOME` / `XDG_CONFIG_HOME` overrides.
* **Layout:** the Wine prefix root holds `drive_c/` and `user.reg`. The game very often lives *inside*
  the prefix (`<prefix>/drive_c/...`), so `directory` may simply equal the prefix. Do not walk the whole
  prefix looking for exes.
* Lutris may be running: open the DB read-only (`file:...?mode=ro`).

## Design

1. `Game` gains `source` (`steam` | `lutris`), `slug`, and `exe_hint`. Lutris ids look like
   `lutris:<id>`; `resolve_game` also matches slug and name fragments.
2. **Discovery** in `games()` beside Steam: SQLite (stdlib `sqlite3`) plus a tiny YAML reader that only
   extracts first-level keys of the top-level `game:` block. No PyYAML dependency.
3. **Game root** (where `_mod_backups/ostool` goes): `directory` if it contains the exe, otherwise the
   exe's folder. Must be deterministic so `uninstall` finds the same place.
4. **Exe:** the config's `exe` if it is an existing 64-bit PE; otherwise the current scan, which must skip
   `windows/`, `dosdevices/`, `users/` and anything under a prefix's `drive_c/windows`. `--exe` still wins;
   the DX12-exe preference stays.
5. **Prefix:** config `prefix`, expanded; the registry file is `<prefix>/user.reg`, or
   `<prefix>/pfx/user.reg` (Proton-style runners). Fall back to `~/.wine` only if it exists; otherwise
   `--edit-registry` is unavailable and the tool prints manual steps.
6. **Registry edit** reuses the existing per-exe `AppDefaults\<exe>\DllOverrides` code unchanged.
   Fix the wineserver check first: match `WINEPREFIX` exactly (today it is looser than it should be for
   prefixes that are not under `compatdata/<appid>`).
7. **Manual alternative** printed instead of Steam launch options: Lutris > Configure > Runner options >
   DLL overrides > add `dxgi = n,b` (and `version = n,b` if the frame-gen mod is installed).
   Also the NVIDIA env var if the runner is Proton-based, under System options > Environment variables.
8. **DXVK note:** Lutris usually installs DXVK's own `dxgi.dll` into the prefix. OptiScaler in the game
   folder loads first and chains to the prefix's `dxgi.dll`, which is what we want; log it so a mismatch
   is visible.
9. `detect` prints Lutris games in the same table; `list` and `verify` need no changes beyond paths.

## Tests

* **Selftest fixture** (no real Lutris needed, mirrors the Steam one): temp `XDG` dirs with a real SQLite
  `games` table and YAML configs covering: normal game, empty `exe`, `$GAMEDIR` placeholders under
  `script:` (must be ignored), a `steam`-runner row and an `installed = 0` row (both skipped), a game
  inside its prefix's `drive_c`, and a prefix with a `pfx/` subfolder. Assert the same round trip:
  dry run changes nothing, install, verify, uninstall leaves the folder and `user.reg` byte-identical.
  Env hooks: `OSTOOL_LUTRIS_DATA`, `OSTOOL_LUTRIS_CONFIG` (the selftest must also point them at empty
  temp dirs for the Steam tests so it never reads the real Lutris).
* **Real test:** one legitimately owned game in Lutris. Dry run, `--edit-registry --apply`, launch,
  `verify --kernel`, `uninstall`, confirm the game folder hashes match the pre-install snapshot.

## Milestones

1. Preconditions: library cleaned; one owned test game chosen and installed in Lutris.
2. Discovery + `detect` (read-only), fixture test.
3. Install/uninstall parity with Steam, fixture round trip.
4. Wineserver check fix; manual-instructions output; README section.
5. Real-game test, then note results and any prefix quirks found.

## Risks

* Prefix variety (Wine, Wine-GE, Proton-GE inside Lutris): detect `user.reg` at both depths and refuse
  rather than guess.
* Lutris rewriting configs on launch: the tool never touches Lutris files, only the game folder and
  `user.reg`.
* Games launched through a Lutris "launcher" exe rather than the real game exe: `--exe` covers it.
* Anti-cheat and online-only titles are refused or flagged exactly as for Steam.
