#!/bin/sh
# Fetches Blargg's cpu_interrupts_v2 test ROMs (the individual NROM builds).
#
# These are the oracle for the CPU's interrupt behaviour: CLI/SEI latency, NMI
# interaction with BRK and IRQ, IRQ during OAM DMA, and the extra branch delay.
# Nothing else in this project covers any of it - the SingleStepTests vectors
# carry no interrupts at all, and the ppu_vbl_nmi ROMs only reach NMI.
#
# Not committed: redistributable-but-unlicensed dumps. SHA256-pinned so a
# truncated download fails here rather than as an inexplicable emulation bug.
#
# Usage: tests/test_files/fetch_cpu_interrupts.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/cpu_interrupts"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/cpu_interrupts_v2/rom_singles"

mkdir -p "$DEST"

ROMS="1-cli_latency e402d36118f77dcbbe8ddca90c15fc76a46bcb30b25cb028c383e4a621de5fc0
2-nmi_and_brk 6e6bf6205930afcfebdc213c583df53986a688a8b36f8856b805ef4c1853e6eb
3-nmi_and_irq 3008a9524d174a8aca562ff0361eba81da53e38cf1ebb5125322fe151f14d945
4-irq_and_dma 6d7b4c1947ada64679af56cf0c227286b2408afe1747dfaa4dc7363d57ff87f6
5-branch_delays_irq f9e10b4a24d8f3cd3e51fb7457c72858aab96a6467fdbbd806d0661c2d32fdc7"

failed=""
echo "$ROMS" | while read -r name want; do
    [ -n "$name" ] || continue
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
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 120 -o "$dest.tmp" "$BASE/$name.nes"

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

count=$(ls -1 "$DEST"/*.nes 2>/dev/null | wc -l | tr -d ' ')
if [ "$count" -ne 5 ]; then
    echo "incomplete: $count/5 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
