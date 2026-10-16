# Working with branches and pull requests

`main` is the shared, buildable starting point. Create a short-lived branch for each feature, bug fix, documentation update, or CubeMX configuration change. Use names such as `feature/flash-driver`, `fix/spi-timeout`, or `docs/storage-format`. Keep one purpose per branch so somebody else can review and merge it independently.

## Get a checkout and start work

Clone once; after that, update your existing checkout. The repository is called `training`; the CubeMX project and firmware target are called `datalogger`.

```sh
git clone git@github.com:University-at-Buffalo-SEDS/training.git
cd training
git switch main
git pull --ff-only origin main
git switch -c feature/flash-driver
python3 build.py build --debug
```

Before switching branches, run `git status`. Commit your work to its own branch, or deliberately stash it, rather than carrying unrelated edits onto another branch. If a fast-forward pull fails, inspect the divergence with a teammate before changing history.

## Make changes and share progress

Change the smallest useful piece at a time. Add application files through the root `CMakeLists.txt`. Keep edits to generated files inside CubeMX `USER CODE` sections where possible. Coordinate IOC edits: two people changing the pinout at the same time can produce difficult merges.

For hardware changes, edit `datalogger.ioc`, generate into this checkout, and review the IOC and generated diff together. Commit both in the same change. Keep the project name `datalogger` and toolchain CMake/GCC. Do not manually patch `cmake/stm32cubemx/CMakeLists.txt` as a lasting fix; CubeMX regenerates it.

```sh
git diff
python3 build.py build --debug
python3 build.py build --release
git add Core/Src/flash.c Core/Inc/flash.h CMakeLists.txt
git diff --cached
git commit -m "Add bounded SPI flash reads"
git push -u origin feature/flash-driver
```

The `git add` paths above are examples for a future flash driver; stage the actual files you changed. Review the staged diff before every commit. Drivers and middleware supplied by CubeMX belong in Git so fresh clones build without sibling repositories or a regeneration step. Their license files belong with them. Build outputs and machine-specific configuration do not.

## Bring in teammates' changes

With your work committed and the working tree clean, merge the latest main into your feature branch:

```sh
git fetch origin
git merge origin/main
```

Resolve each conflict by understanding both changes, stage the resolved files, then run `git merge --continue` and rebuild. For IOC conflicts, agree on the intended settings and regenerate; do not blindly accept one side or leave duplicate configuration keys. Use `git merge --abort` if you need to return to the state before the merge and discuss the resolution. Merging main preserves shared branch history and does not require a force push.

## When to open a PR

Open a **draft pull request to `main`** once there is a useful design or partial implementation to discuss, especially for shared interfaces, flash record formats, or IOC changes. Push further commits to the same branch to update it.

Mark it ready for review when its stated scope is complete, Debug and Release build, conflicts are resolved, and you have reviewed the diff yourself. A documentation-only change needs its links and instructions checked; firmware tests should match the change. Request review from a teammate before merging.

Include:

- What problem the change solves and how behavior changes.
- How you tested it, including build commands and relevant hardware results.
- Any pin, clock, peripheral, storage-format, or interface changes.
- Known limitations and what remains untested. A successful build is not a hardware test.

Keep unrelated cleanup in another PR. A small, complete driver change can merge before the entire logger is finished. Address review comments, rerun affected checks after changes, and merge through the PR after approval. Repository administrators must configure any enforced branch protection or required reviews on GitHub; this guide does not enable those settings.

## After the PR merges

```sh
git switch main
git pull --ff-only origin main
git branch -d feature/flash-driver
```

Delete the merged remote branch using the PR page. Git can refuse local `-d` after a squash merge because the commit histories differ; verify the PR is merged and your work is present before removing that branch. Start the next task from updated `main`, not the old feature branch. Rebuild after changing branches; for a changed toolchain or stale configuration, remove the relevant ignored build directory and configure again.

## Avoid

- Direct commits or pushes to `main`, long-running catch-all branches, and unrelated work in one PR.
- Force pushes to `main` or a teammate's branch. Do not rebase a shared branch without agreement.
- Committing `build/`, ELF/BIN files, absolute local paths, credentials, or personal editor settings.
- Broad `git add .` without checking the staged diff, or destructive reset/clean commands used to hide an unresolved problem.
- Editing generated code outside preserved sections without documenting how it survives regeneration.
- Claiming flash erase/program, USB export, or sensor behavior works solely because it compiles.
