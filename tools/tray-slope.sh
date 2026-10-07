#!/bin/bash
# tools/tray-slope.sh - how much of the wallpaper actually reaches the taskbar, and whether an
# outside process can drive that number.
#
# Why a slope and not a screenshot: the taskbar's own mean colour tells you nothing, because an
# opaque fill and a translucent tint both "look different". So the wallpaper is switched between two
# solid fixtures 255 steps apart and the taskbar band is measured for each.
#     slope = (band over red - band over blue) / (wallpaper over red - wallpaper over blue)
# slope ~0 => nothing of the wallpaper reaches the band, so a slider would have nothing to scale.
# slope ~1 => the band is the wallpaper itself.
# EnableTransparency=0 is the control that has to read 0.
#
# The subtraction also cancels whatever an applied technique paints on its own: an accent fill is
# the same colour over red and over blue, so it cannot masquerade as transparency. Only wallpaper
# that survives into the band shows up as slope.
#
# Modes: setup | teardown | table | cal [label] | tech <label> <trayalpha args>
set -u
cd /h/wallpaper
CTL=bld/bin/RelWithDebInfo/WallpaperRenderer.exe
LOG=build/tmp/tray-fixtures.txt
TMP=build/tmp/tray-slope

ctl() { "$CTL" --ctl "$@" 2>&1 | tr -d '\r'; }

# "bandR,bandG,bandB wallpaperR,wallpaperG,wallpaperB bandSD" of the flat part of the band.
grab() {
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/tray-band.ps1 -Segments 8 2>&1 \
    | tr -d '\r' \
    | awk '/band seg3 /{split($4,m,"="); split($5,s,"="); b=m[2]" "s[2]}
           /ref above band/{split($4,m,"="); r=m[2]}
           END{print b" "r}'
}

sample() {   # sample <wallpaper id> <file>
    ctl apply "$1" >/dev/null
    sleep 3
    grab >"$2"
    awk '{printf "                         band=%s wallpaper=%s (sd=%s)\n", $1, $3, $2}' "$2"
}

# slope over the channels the fixtures actually differ in (both are zero in green).
slope() {   # slope <label> <red file> <blue file>
    awk -v L="$1" '
        FNR==1 {split($1,b,","); split($3,w,",");
                if (NR==1) {R1=b[1]; B1=b[3]; WR1=w[1]; WB1=w[3]}
                else       {R2=b[1]; B2=b[3]; WR2=w[1]; WB2=w[3]} }
        END {stepR=WR1-WR2; stepB=WB1-WB2
             # The two samples are only a slope if the wallpaper really was the two fixtures. Another
             # actor can change the desktop between them, and then the step is still non-zero but the
             # pair means nothing - so check the shape, not just the size.
             if (stepR == 0 || stepB == 0) {
                 printf "%-18s INVALID - wallpaper step was 0 (a fixture sample failed: red %s/%s, blue %s/%s)\n", \
                        L, WR1, WB1, WR2, WB2
             } else if (WR1 < 150 || WB2 < 150 || WB1 > 100 || WR2 > 100) {
                 printf "%-18s INVALID - wallpaper was not red then blue (saw R %s->%s, B %s->%s)\n", \
                        L, WR1, WR2, WB1, WB2
             } else {
                 printf "%-18s slope  R %6.3f   B %6.3f   band=%s|%s\n", L, (R1-R2)/stepR, (B1-B2)/stepB, R1","R2, B1","B2
             } }' \
        "$2" "$3"
}

row() {   # row <label> [trayalpha args...] : apply, measure both fixtures, restore
    local label="$1"; shift
    printf '%s\n' "-- $label"
    if [ "$#" -gt 0 ]; then
        build/trayalpha.exe "$@" 2>&1 | tr -d '\r' | sed 's/^/    /'
        sleep 3
    fi
    sample "$RID" "$TMP.r"
    sample "$BID" "$TMP.b"
    slope "$label" "$TMP.r" "$TMP.b"
    if [ "$#" -gt 0 ]; then build/trayalpha.exe restore >/dev/null 2>&1; sleep 3; fi
}

mkdir -p build/tmp
case "${1:-table}" in
    setup)
        powershell -NoProfile -ExecutionPolicy Bypass -File tools/make-solid.ps1 2>&1 | tr -d '\r'
        bld/bin/RelWithDebInfo/WallpaperRenderer.exe --ctl list 2>&1 | tr -d '\r' \
        | python -c "
import json,sys
c=[w['id'] for w in json.load(sys.stdin)['catalog']]
print(' '.join([i for i in c if 'red' in i or 'blue' in i][:2]))" >"$LOG" 2>/dev/null || true
        echo "fixtures: $(cat "$LOG" 2>/dev/null)   (import them with --ctl addimage if empty)"
        ;;
    teardown)
        ctl apply snowfall M0 >/dev/null; sleep 2
        for i in $RID $BID; do [ -n "$i" ] && echo "delimage $i -> $(ctl delimage "$i" | tail -1)"; done
        echo "showing now: $(python tools/swstatus.py 2>&1 | tail -1)"
        ;;
    table)
        read -r RID BID <"$LOG"
        row base
        row tr0-off transparency 0
        for a in 0 64 128 192 255; do
            row "acrylic-$a" accent 4 64 "$a" 255 255 255
        done
        row "grad-128" accent 2 64 128 255 255 255
        row "blur-128" accent 3 64 128 255 255 255
        for a in 64 128 192; do row "layered-$a" layered "$a"; done
        for b in 1 2 3; do row "backdrop-$b" backdrop "$b"; done
        build/trayalpha.exe restore >/dev/null 2>&1
        echo "--- done, restored"
        ;;
    cal)
        read -r RID BID <"$LOG"
        row "${2:-as-is}"
        ;;
    tech)
        read -r RID BID <"$LOG"
        label="$2"; shift 2
        row "$label" "$@"
        ;;
    *)  echo "modes: setup | table | cal [label] | tech <label> <trayalpha args> | teardown"; exit 2 ;;
esac
