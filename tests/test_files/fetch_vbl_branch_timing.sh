#!/bin/sh
# Fetches two timing suites this project had never run: Blargg's vbl_nmi_timing
# and branch_timing_tests. Ten ROMs, all NROM (mapper 0), 16KB PRG with CHR-RAM.
#
# Found by asking which timing oracles in the mirror were unused, during the
# DMC DMA investigation - sprdma_and_dmc_dma reports an eleven-cycle excess on a
# timed code sequence, so anything that times the CPU or the CPU/PPU
# relationship was worth checking. Neither suite is the cause; both pass
# unmodified. They are kept because passing oracles that guard nothing are
# coverage left on the floor.
#
# MEASURED, all ten passing on first contact:
#
#   1.frame_basics        PASSED     4.vbl_clear_timing   PASSED
#   2.vbl_timing          PASSED     5.nmi_suppression    PASSED
#   3.even_odd_frames     PASSED     6.nmi_disable        PASSED
#                                    7.nmi_timing         PASSED
#   1.Branch_Basics       PASSED
#   2.Backward_Branch     PASSED
#   3.Forward_Branch      PASSED
#
# NOT REDUNDANT WITH WHAT IS ALREADY HERE, though it overlaps. ppu_vbl_nmi
# (fetched as blargg_ppu) covers the same vblank and NMI ground through the
# $6000 protocol; these are the older screen-reporting suite and reach it by
# different code. instr_timing covers instruction cycle counts but states it
# does not time the 8 branches - branch_timing_tests does, from the outside,
# which is a second angle on what 2-branch_timing checks.
#
# THEY REPORT ON SCREEN in capitals - "PASSED", not the $6000 protocol's
# "Passed" - so they go through tests/nametable_screen.h and a case-sensitive
# match. Matching the wrong case would pass a screen that never said so.
#
# Frame budgets: all ten settle well inside 900 frames; measured with a fixed
# count rather than by waiting for the screen to stop changing.
#
# (c) Shay Green (blargg), from the nes-test-roms collection. Not committed:
# gitignored like every other fetched fixture.
#
# Usage: tests/test_files/fetch_vbl_branch_timing.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/vbl_branch_timing"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master"

mkdir -p "$DEST"

ROMS="vbl_nmi_timing/1.frame_basics 719bf4b4ea9022aad4216b159f59846ea57f98c8bddc5069b70286f5b810e998
vbl_nmi_timing/2.vbl_timing 39231f002784b62e5d0ab67735855198b6b62c6c6ef06b0549333cd818879fb2
vbl_nmi_timing/3.even_odd_frames 268d5e305ab73a29ff524906f92db7da1944763ea3de464e14a44c5b4199049e
vbl_nmi_timing/4.vbl_clear_timing 8552a65de49b677cdf7fd419f5ac51f96766786e1e1442f6f9d62c87f4c15c2c
vbl_nmi_timing/5.nmi_suppression 059c8df6a9b41a95e827608f1c0edb496b763e0c2b861f22614f5f228f1e10aa
vbl_nmi_timing/6.nmi_disable 4573dcff3c57351b0e6ce76d86334586ba3d1cd6d4f9ec014887e94912f543cf
vbl_nmi_timing/7.nmi_timing 26a2475828ef9ebd6178129f530a8aaa6f9f1ddd493616f1524d8323d6e56a4c
branch_timing_tests/1.Branch_Basics 7b69e3044eaeb86317147a900d1f4a467b666f59d375ec1ba6658233f23786cd
branch_timing_tests/2.Backward_Branch f7966e9b86b04b4adb987439a442e926e9cfe6bb71436dd5dd56f41f9eb029a4
branch_timing_tests/3.Forward_Branch d0fbc6b1899bc948172c45f37a981eb0dade212d7e807cc56efedae93d6f9c9b"

echo "$ROMS" | while read -r path want; do
    name=$(basename "$path")
    dest="$DEST/$name.nes"

    if [ -f "$dest" ]; then
        have=$(sha256sum "$dest" | cut -d' ' -f1)
        if [ "$have" = "$want" ]; then
            echo "ok (cached): $name.nes"
            continue
        fi
        echo "checksum mismatch on cached $name.nes, refetching" >&2
    fi

    echo "fetching $name.nes..."
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 120 -o "$dest.tmp" "$BASE/$path.nes"

    have=$(sha256sum "$dest.tmp" | cut -d' ' -f1)
    if [ "$have" != "$want" ]; then
        rm -f "$dest.tmp"
        echo "SHA256 mismatch for $name.nes" >&2
        echo "  expected: $want" >&2
        echo "  actual:   $have" >&2
        exit 1
    fi

    mv "$dest.tmp" "$dest"
    echo "ok: $name.nes"
done

count=$(ls -1 "$DEST"/*.nes 2>/dev/null | wc -l)
if [ "$count" -ne 10 ]; then
    echo "expected 10 ROMs in $DEST, found $count" >&2
    exit 1
fi

echo "done: $count ROMs in $DEST"
