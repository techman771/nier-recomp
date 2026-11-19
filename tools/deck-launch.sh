#!/bin/sh
# Steam Deck launcher for the game under Proton. Steam launch options:
#
#     "/path/to/NieR-Recomp/deck-launch.sh" %command%
#
# What it does, and why:
#
# 1. Holds the GPU at a fixed clock while the game is running. Left to itself SteamOS moves the
#    Deck's GPU between 200 and about 1040 MHz under this game (the GPU is idle for part of every
#    frame, waiting for the game's CPU work, so it is clocked down and up again all the time), and
#    the frames that catch it on the way up come late: 60 fps with a slight judder. At a fixed
#    1100 MHz, about what the automatic setting reaches anyway, the judder goes and the Deck draws
#    a little less (7.8 W against 8.6 in the same scene). The clock is set through steamosctl, the
#    same service the Performance menu's "Manual GPU Clock" uses, and what was set before is put
#    back when the game exits. NIER_GPU_CLOCK=<MHz> asks for another clock (1600 is a little evener
#    still in the heaviest scenes, for about 4 W more), NIER_GPU_CLOCK=max for the highest the
#    device allows, NIER_GPU_CLOCK=auto leaves the setting alone.
#
# 2. Makes Proton load the d3dcompiler_47.dll shipped beside nier.exe (Wine's own cannot compile
#    the game's shaders: only videos would draw).
#
# Without steamosctl (another Linux, an older SteamOS) only the second step happens.

clock=${NIER_GPU_CLOCK:-1100}
restore=

value() { sed -n 's/^[^:]*: *//p' | head -n 1; }   # "GPU performance level: auto" -> "auto"

if [ "$clock" != auto ] && command -v steamosctl >/dev/null 2>&1; then
    level_before=$(steamosctl get-gpu-performance-level 2>/dev/null | value)
    clock_before=$(steamosctl get-manual-gpu-clock 2>/dev/null | value)
    highest=$(steamosctl get-manual-gpu-clock-max 2>/dev/null | value)
    [ "$clock" = max ] && clock=$highest
    case "$clock" in ''|*[!0-9]*) clock= ;; esac
    case "$highest" in ''|*[!0-9]*) ;; *) [ -n "$clock" ] && [ "$clock" -gt "$highest" ] && clock=$highest ;; esac
    if [ -n "$level_before" ] && [ -n "$clock" ]; then
        if steamosctl set-gpu-performance-level manual >/dev/null 2>&1 && steamosctl set-manual-gpu-clock "$clock" >/dev/null 2>&1; then
            restore=yes
            echo "deck-launch: GPU clock $clock MHz while the game runs (was: $level_before${clock_before:+, $clock_before MHz})" >&2
        else
            steamosctl set-gpu-performance-level "$level_before" >/dev/null 2>&1
        fi
    fi
fi

put_back() {
    [ -n "$restore" ] || return 0
    restore=
    if [ "$level_before" = manual ]; then
        case "$clock_before" in ''|*[!0-9]*) ;; *) steamosctl set-manual-gpu-clock "$clock_before" >/dev/null 2>&1 ;; esac
    else
        steamosctl set-gpu-performance-level "$level_before" >/dev/null 2>&1
    fi
}
trap put_back EXIT
trap 'put_back; exit 143' INT TERM HUP

export WINEDLLOVERRIDES="d3dcompiler_47=n,b${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
"$@"
status=$?
put_back
exit $status
