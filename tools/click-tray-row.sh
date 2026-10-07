#!/bin/bash
# Click one row of the tray menu with a gate: the row is only clicked when a probe says a real
# menu (#32768) is on screen at that pixel. Without the gate a stale flyout turns the click into a
# click on whatever window sits behind it.
#   bash tools/click-tray-row.sh <row>        rows: quit next prev auto stop open
set -u
cd /h/wallpaper || exit 1
# Row centres measured from a 1:1 capture of the open menu (bld/gap-12.png, crop origin 3380,1600):
# 退出 208.5, 下一张 245.5, 上一张 284.5, 自动播放 323.5, 停止播放 362.5, 显示设置窗口 411.5.
# Guessing from an earlier screenshot cost two clicks that landed on the separator.
ROW="$1"
case "$ROW" in
  quit) Y=1808 ;; next) Y=1845 ;; prev) Y=1884 ;; auto) Y=1923 ;; stop) Y=1962 ;; open) Y=2011 ;;
  *) echo "unknown row: $ROW"; exit 2 ;;
esac
X=3520
PS="powershell -NoProfile -ExecutionPolicy Bypass -File"
click() { $PS tools/click-point.ps1 -X "$1" -Y "$2" ${3:+-$3} -Stay >/dev/null; }

CX=3478; CY=2035                                  # our icon's centre, read off the open flyout once

$PS tools/press-key.ps1 -Vk 27 >/dev/null; sleep 0.6
click 3558 2136; sleep 1.4                        # open the hidden-icons flyout
click "$CX" "$CY" Right                           # right-click our icon -> the menu
sleep 1.2
# Do NOT confirm the flyout with a UIA scan first: opening that query deactivates the flyout and it
# closes before the click lands. The row gate below is what keeps a stale popup from being clicked.
GATE=$($PS tools/click-point.ps1 -X "$X" -Y "$Y" -MoveOnly)
case "$GATE" in
  *#32768*|*QWindowPopup*) ;;
  *) echo "MENU_NOT_AT_ROW: $GATE (row $ROW y=$Y)"; $PS tools/press-key.ps1 -Vk 27 >/dev/null; exit 1 ;;
esac
click "$X" "$Y"
echo "clicked $ROW at $X,$Y (icon $CX,$CY)"
