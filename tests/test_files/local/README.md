# Locally-supplied ROMs

Drop ROMs you own here. Nothing in this directory is fetched, and nothing in it
is committed — the whole directory is gitignored except this file.

```
tests/test_files/local/smb.nes
```

## Why this is separate from every other fixture

Every other directory under `test_files/` is filled by a SHA256-pinned script,
so any checkout can reproduce it and CI can too. This one cannot: the contents
are yours, obtained from a cartridge you own, and no script here will ever
download a commercial game.

That has a consequence worth stating rather than discovering. A test that needs
a ROM from here **must skip when it is absent, not fail** — otherwise the suite
would be permanently red for everyone else and in CI. And because `GTEST_SKIP`
exits 0, ctest counts a skipped test as a passing one, so a skip here is a
*silent* pass. Tests using this directory say so in their skip message, and the
suite's honest figure is always "executed", not "passed".

## Expected names

Tests look for specific filenames so they can skip cleanly when one is missing:

| File | What uses it |
|---|---|
| `smb.nes` | `tests/local_rom_tests.cpp` — end-to-end smoke test: boots, renders, responds to Start |

Add the file, re-run `ctest`, and the corresponding test switches from SKIPPED
to executed. Nothing else needs configuring.
