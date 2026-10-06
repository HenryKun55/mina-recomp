# mina-recomp

A static recompilation of **Mina the Hollower** (Yacht Club Games) from the
arm64 slice of its macOS executable to portable C, aimed at running natively
on the **PlayStation Vita**.

No game data is included. You need your own copy of the game: the build
recompiles *your* executable, and the game reads its data from your
installation (`gamedata/app` → `Mina the Hollower.app`).

Status: phase 1 (analysis). See [docs/PLAN.md](docs/PLAN.md).

## Analysis tools

```sh
pip3 install capstone
python3 tools/analysis/census.py gamedata/app/Contents/MacOS/MinaTheHollower
```
