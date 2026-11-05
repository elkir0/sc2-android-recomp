#!/bin/bash
# lib-fold.sh — appareil adb des scripts Fold (V0.1, 22/10), SOURCÉE par bar-fold.sh, cinematique-fold.sh,
# fold-ab-auto.sh et fold-ab-run.sh (. "$R/scripts/lib-fold.sh"), APRÈS un éventuel adb factice (ADB_FAKE=1 de
# bar-fold.sh : la fonction adb est alors celle des fixtures) :
#   D=$(fold_serie) || exit 1
#                     numéro de série adb du Fold sur la sortie, état « device » vérifié (get-state) : SC2_ADB_SERIAL
#                     s'il est posé, sinon le premier appareil « device » de modèle SM_F976B dans « adb devices -l ».
#                     L'identifiant dépend du transport (nom mDNS du débogage sans fil,
#                     <IP>:<port> après adb connect, <série> en USB) : un identifiant mDNS figé dans les scripts les
#                     faisait tous échouer dès que le téléphone était connecté par son adresse.
#                     Introuvable ou hors ligne : message sur stderr et retour 1 (rien n'a été touché sur le téléphone).
FOLD_MODELE=SM_F976B

fold_serie() {
    local d=${SC2_ADB_SERIAL:-}
    [ -n "$d" ] || d=$(adb devices -l 2>/dev/null | awk -v m="model:$FOLD_MODELE" '
        $2 == "device" { for (i = 3; i <= NF; i++) if ($i == m) { print $1; exit } }')
    if [ -z "$d" ]; then
        echo "Fold ($FOLD_MODELE) introuvable dans « adb devices -l » : débogage sans fil coupé ? Le réactiver puis" \
             "adb connect <IP du Fold>:<port affiché sous Débogage sans fil> (adresse locale du téléphone)," \
             "ou SC2_ADB_SERIAL=<série> pour un autre appareil" >&2
        return 1
    fi
    if [ "$(adb -s "$d" get-state 2>/dev/null)" != device ]; then
        echo "Fold « $d » ${SC2_ADB_SERIAL:+(SC2_ADB_SERIAL) }pas dans l'état « device » (adb devices -l) : run annulé" >&2
        return 1
    fi
    echo "$d"
}
