# moonsweepers-trainer

A small C++ memory trainer for the **single-player** game *Moon Sweepers* (Steam, Unity IL2CPP, 64-bit).
It finds the in-game wallet in memory and sets or freezes the balance.

> Use it offline, in single-player only. Do not use it in any mode with online rankings or other players.
> This is an unofficial fan tool and is not affiliated with the game's developers.

## Download

Get `moonsweepers-trainer-vX.Y.Z-win-x64.zip` from the [Releases](../../releases) page, unpack it and run
`moonsweepers-trainer.exe` while the game is running. A SHA-256 file is published next to each archive.

## Usage

Start the game and load into a session, then run the trainer. Without options it opens a menu:

```
Wallet [0] balance: 186.20480069391442688
  1) set balance
  2) freeze balance
  3) rescan wallets
  0) quit
```

Command line:

| Option | Meaning |
| --- | --- |
| `--list` | show the wallets found in the running game |
| `--set AMOUNT` | set the balance, e.g. `--set 10000` |
| `--freeze AMOUNT` | keep the balance at `AMOUNT` until Ctrl+C |
| `--wallet N` | use wallet number `N` from `--list` |
| `--match AMOUNT` | pick the wallet whose balance shows as `AMOUNT` on screen (e.g. `--match 186.20`) |
| `--version`, `--help` | |

The game can contain more than one wallet object. If several are found, pick the one that equals the amount
shown in the game, either from the menu or with `--match`.

If the process cannot be opened, run the trainer as administrator.

## How it works

The balance is a `System.Decimal` field of the `TeamWallet` class. Its address changes on every launch, so the
trainer does not search for the number itself:

1. It looks for the ASCII name `TeamWallet` in the game's memory.
2. A pointer to that string stored at `Il2CppClass::name` (offset `0x10`) identifies the class.
3. Every object whose first 8 bytes are that class pointer is a wallet; the balance sits at offset `0x10`.

`System.Decimal` is stored as `flags` (scale in bits 16-23, sign in bit 31), `hi32` and `lo64`. Before every
write the trainer checks again that the object header still points at the class, so freed or moved objects are
never written to.

The class and field layout can change in a game update. If the trainer stops finding a wallet after an update,
please open an issue.

## Build

Windows, CMake 3.16+ and a C++17 compiler (Visual Studio 2019+ or MinGW-w64):

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

The executable is `build/Release/moonsweepers-trainer.exe`, linked against the static runtime so it runs on a
clean Windows install.

## Releases

GitHub Actions builds every push and pull request and runs the unit tests. Pushing a tag such as `v0.1.0` builds
the project, packages `moonsweepers-trainer-v0.1.0-win-x64.zip` with its checksum and publishes a GitHub release.
Tags containing a hyphen (for example `v0.2.0-rc1`) are marked as pre-releases.

```
git tag v0.1.0
git push origin v0.1.0
```

## License

MIT, see [LICENSE](LICENSE).

---

## По-русски

Небольшой трейнер на C++ для **одиночной** игры *Moon Sweepers* (Steam). Находит кошелёк в памяти игры и меняет
или замораживает баланс. Пользуйся только офлайн и в одиночной игре.

Скачай архив со страницы [Releases](../../releases), запусти игру, загрузи сессию и запусти
`moonsweepers-trainer.exe`. В меню: установить баланс, заморозить, пересканировать. Если кошельков несколько,
выбери тот, чья сумма совпадает с экраном (или используй `--match 186.20`). Если процесс не открывается,
запусти трейнер от имени администратора.

Баланс хранится как `System.Decimal` в поле класса `TeamWallet`; трейнер находит объект по имени класса, а не по
значению, поэтому после перезапуска игры ничего вводить не нужно.
