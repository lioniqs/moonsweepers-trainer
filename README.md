# moonsweepers-trainer

A small C++ memory trainer for *Moon Sweepers* (Steam, Unity IL2CPP, 64-bit). It has a window (and a command-line
version) with:

- **Team wallet**: set the balance or hold a fixed amount.
- **Teleport and rewards**: sets the game's own teleport price to 0 and lets you change the reward per flag / per
  landing site.
- **Navigator**: all 195 country flags and 26 landing sites with latitude/longitude, the distance from you and the
  direction relative to where you are facing. Places already raised in the game are ticked, you can tick more by
  hand, add **marks of your own**, and open a small always-on-top **compass** window that guides you to the place
  you picked.

> The game is **co-op** (crews). Use the trainer only in your own session. Changing the wallet or prices in somebody
> else's crew spoils the game for them. This is an unofficial fan tool and is not affiliated with the developers.

## Download

Get `moonsweepers-trainer-vX.Y.Z-win-x64.zip` from the [Releases](../../releases) page. It contains

- `moonsweepers-trainer.exe` - the full window (wallet, prices, navigator),
- `moonsweepers-navigator.exe` - the same navigator **without any write access**: it only reads the game and never
  changes it, so it is the one to use in a crew with other players,
- `moonsweepers-trainer-cli.exe` - the command-line version.

A SHA-256 file is published next to each archive. Start the game, load into a session (not just the main menu), then
run the trainer. If the game process cannot be opened, run the trainer as administrator.

## The window

| Part | What it does |
| --- | --- |
| Team wallet | shows the balance; *Set balance* writes the amount, *Hold this amount* keeps it fixed |
| Free in-game teleport | sets `MoonWorld.TeleportPrice` (50 by default) to 0; unchecking restores the original |
| Rewards | edits the reward per flag (100) and per site (250); *Restore originals* puts everything back |
| Player | your position, latitude and longitude on the Moon |
| Navigator | sort by any column (click the header); filter All / Flags / Sites / My marks / Not visited / Visited; search by name |
| Seen column | ✓ = already raised in the game, ● = ticked by you (*Toggle "visited"*) |
| Guide me there | opens the compass window for the selected place (double-click a row does the same) |
| My marks | *Mark my position* saves where you stand under the name you typed; *Delete my mark* removes it |

### The compass

*Guide me there* opens a small window that stays on top of the game (it works with the game in windowed or borderless
mode). It shows the target's name, the distance along the surface and an arrow that points the way relative to
where you are facing; it turns green when you face the target and says *You are here* within 15 m.

Your marks and the ticks you set by hand are stored in `%APPDATA%\MoonSweepersTrainer` (`marks.txt`, `visited.txt`),
so they survive restarts. The game's own "raised" state is read live from the game, not from save files.

The trainer does **not** move the player by itself. It makes the game's own teleport free and tells you where each
place is and which way to go. Original prices and rewards are restored when you close the window.

## Command line

```
moonsweepers-trainer-cli --list
moonsweepers-trainer-cli --set 10000 --wallet 1
moonsweepers-trainer-cli --freeze 10000 --match 186.20
```

| Option | Meaning |
| --- | --- |
| `--list` | show the wallets found in the running game |
| `--set AMOUNT` | set the balance |
| `--freeze AMOUNT` | keep the balance at `AMOUNT` until Ctrl+C |
| `--wallet N` | use wallet number `N` from `--list` |
| `--match AMOUNT` | pick the wallet whose balance shows as `AMOUNT` on screen |
| `--version`, `--help` | |

The command-line version finds several `TeamWallet` objects; the one the game really uses is usually number 1.
The window does not guess: it reads the wallet that `MoonWorld.wallets` points to.

## How it works

Nothing is hard-coded to an address. On start the trainer reads the IL2CPP runtime structures of the running game:

1. It finds the classes `MoonWorld`, `MoonWalker`, `MoonLandmarks`, `MoonLandmark` and `TeamWallet` by name
   (a pointer to the name string sits at `Il2CppClass::name`).
2. Field offsets come from the class's `FieldInfo` table, looked up by field name.
3. Instances are objects whose first 8 bytes are the class pointer; the right ones are picked by checking fields
   (for example `MoonWalker.world` must point at the `MoonWorld` instance).
4. The balance is a `System.Decimal` (`flags`, `hi32`, `lo64`) in `TeamWallet.Balance`, referenced by
   `MoonWorld.wallets`. Prices and rewards are static `Decimal` fields of `MoonWorld`.
5. Landmarks: `MoonLandmarks.all` holds names and coordinates, `MoonWorld.landmarkDirections` holds a unit vector
   per landmark and `MoonWorld.landmarkRaised` which of them are already raised. The player's position comes from
   `MoonWalker.previousPosition` and the facing direction from `MoonWalker.heading`.

Before every write the trainer re-checks that the object header still points at the class. A game update that renames
or moves these fields can break it; please open an issue if so.

## Build

Windows, CMake 3.16+ and a C++17 compiler (Visual Studio 2019+ or MinGW-w64):

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

The executables are in `build/Release/` and use the static runtime, so they run on a clean Windows install.

## Releases

GitHub Actions builds every push and pull request and runs the unit tests. Pushing a tag such as `v0.2.0` builds the
project, packages the zip with its checksum and publishes a GitHub release. Tags containing a hyphen (for example
`v0.2.0-rc1`) are marked as pre-releases.

```
git tag v0.2.0
git push origin v0.2.0
```

## License

MIT, see [LICENSE](LICENSE).

---

## По-русски

Небольшой трейнер на C++ для *Moon Sweepers* (Steam): окно и консольная версия.

- **Кошелёк команды**: установить баланс или держать заданную сумму.
- **Телепорт и награды**: ставит цену встроенного телепорта игры в 0, меняет награду за флаг и за метку.
- **Навигатор**: все 195 флагов стран и 26 мест посадок с широтой и долготой, расстоянием от тебя и направлением
  относительно того, куда ты смотришь. Уже поднятые в игре места отмечены, можно отмечать вручную, добавлять **свои
  точки** и открывать маленькое окно-**компас** поверх игры, которое ведёт к выбранному месту (двойной клик по строке).
  Отдельный `moonsweepers-navigator.exe` только читает игру и ничего в ней не меняет: его можно держать открытым и в
  кооперативе. Свои точки и отметки хранятся в `%APPDATA%\MoonSweepersTrainer`.

Игра кооперативная: пользуйся только в своей сессии, не в чужих экипажах. Трейнер не перемещает игрока сам: он
делает бесплатным телепорт игры и показывает, где каждый флаг и метка. Запусти игру, загрузи сессию, затем
`moonsweepers-trainer.exe` (если процесс не открывается, запусти от имени администратора). Цены и награды
возвращаются на место при закрытии окна.
