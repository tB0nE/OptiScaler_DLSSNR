# ostool

Puts the OptiScaler-DLSSNR + PeripheralWarp setup (and, where the game supports it, the separate
DLSS frame-generation mod) into a Steam/Proton game, and takes it back out again. Python 3, standard
library only. **Nothing changes unless you pass `--apply`.**

## One-time: build the kit

The kit is a local copy of a setup that already works, made from your working game folder. It is not
redistributable (it contains the NR model and the frame-gen mod), so it lives only on your machine.

    python3 ostool.py kit init --from-game "/path/to/Cyberpunk 2077/bin/x64"
    python3 ostool.py kit show

Default location: `~/.local/share/ostool/kit` (override with `OSTOOL_KIT`).

## Per game

    python3 ostool.py detect                    # which Steam games look suitable (--all shows why not)
    python3 ostool.py install "High On Life" --profile warp          # dry run: prints the plan
    python3 ostool.py install "High On Life" --profile warp --apply  # do it
    python3 ostool.py verify "High On Life" --kernel                 # after one run of the game
    python3 ostool.py uninstall "High On Life" --apply               # put everything back

`GAME` is an appid, part of the name, or a folder path.

* **Profiles** (`--profile`): `default` (warp off), `warp` (59/80), `warp-rt` (28/64). Anything else:
  `--set Section.Key=Value` (repeatable).
* **Frame generation** (`--fg auto|separate|off`): `auto` installs the mod (as `version.dll`) only if the
  game already has native DLSS-G (Streamline `sl.dlss_g.dll` or `nvngx_dlssg.dll`). OptiScaler and the mod
  stay two separate proxies (`dxgi.dll` and `version.dll`); chaining one under the other did not work under Proton.
* **DLL overrides**: by default the tool prints the Steam launch options
  (`WINEDLLOVERRIDES="dxgi=n,b;version=n,b" ...`). `--edit-registry` writes them into the game's
  `user.reg` instead, but only when no wineserver or game is running, after backing it up.
* **Safety**: refuses to overwrite a `dxgi.dll` or `version.dll` it did not install (use `--force` to back up
  and replace), refuses non-DX12 games and detected anti-cheat, and flags known online-only games.

## What it records

`<game>/_mod_backups/ostool/manifest.json` lists every file added or replaced, with sha256 hashes, the
originals of anything replaced, the registry lines it touched, and the folders it created. `uninstall`
reverses exactly that, refuses if a file changed since install (unless `--force`), and restores the
registry byte-for-byte (or removes only its own lines if the file changed since).

## Test

    python3 ostool.py selftest

Builds a throwaway fake Steam game and checks discovery, dry run, conflict refusal, install, verify,
uninstall (byte-identical folder and registry afterwards) and the surgical registry undo.

## Not done yet

Upgrading an existing install in place (uninstall first), 32-bit games, non-Steam games, and the
`External = true` frame-generation setting, which is its own experiment.
