#!/bin/bash
# emu-toucher.sh tape <x> <y> | deux <x1> <y1> <x2> <y2> | glisse <x1> <y1> <x2> <y2> (21/10) : gestes MULTIPOINTS dans
# l'émulateur (adb shell input n'en fait qu'un doigt) par sendevent sur l'écran tactile virtio (protocole B), en pixels
# d'écran. Exige « adb root » (image google_apis). EMU=emulator-5554, DEV=/dev/input/event1, LARGEUR/HAUTEUR de l'écran.
set -u
EMU=${EMU:-emulator-5554}; DEV=${DEV:-/dev/input/event1}; L=${LARGEUR:-2504}; H=${HAUTEUR:-2256}
sx() { echo $(( $1 * 32767 / L )); }; sy() { echo $(( $1 * 32767 / H )); }
ev() { C="$C sendevent $DEV $1 $2 $3;"; }
doigt() { ev 3 47 "$1"; ev 3 57 "$2"; ev 3 53 "$(sx "$3")"; ev 3 54 "$(sy "$4")"; ev 3 58 512; ev 3 48 6; }  # SLOT, ID, X, Y, PRESSION, TAILLE
leve() { ev 3 47 "$1"; ev 3 57 4294967295; }
syn() { ev 0 0 0; }
C=""
case ${1:?geste} in
tape)   doigt 0 101 "$2" "$3"; ev 1 330 1; syn; C="$C sleep 0.05;"; leve 0; ev 1 330 0; syn ;;
deux)   doigt 0 101 "$2" "$3"; ev 1 330 1; syn; C="$C sleep 0.03;"; doigt 1 102 "$4" "$5"; syn; C="$C sleep 0.08;"
        leve 1; syn; leve 0; ev 1 330 0; syn ;;
glisse) doigt 0 101 "$2" "$3"; ev 1 330 1; syn
        for i in 1 2 3 4 5 6 7 8; do C="$C sleep 0.03;"; ev 3 47 0
          ev 3 53 "$(sx $(( $2 + ($4 - $2) * i / 8 )))"; ev 3 54 "$(sy $(( $3 + ($5 - $3) * i / 8 )))"; syn; done
        leve 0; ev 1 330 0; syn ;;
*) echo "geste inconnu : $1"; exit 1 ;;
esac
~/Library/Android/sdk/platform-tools/adb -s "$EMU" shell "$C"
