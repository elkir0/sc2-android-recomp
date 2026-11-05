# Marathon — recettes et plans

Ce dossier porte les briques du banc « marathon » : enchaîner plusieurs missions du mode histoire en UNE
seule exécution du jeu (hub du bar → mission → victoire par triche → « Continue » → retour au bar → mission
suivante…), piloté par un plan. Les briques sont des fragments du format 2 du banc d'entrées (voyez l'entête
de `recomp-sc2/runtime/sc2_entrees_banc.c`) : des événements RELATIFS (`+<s>:<geste>`), des commentaires
`#`, et JAMAIS de point-virgule dans un commentaire (le point-virgule coupe un élément, même en commentaire).

## Les briques

| Fichier             | Posé sous le segment… | Rôle                                                        | Événements |
|---------------------|-----------------------|-------------------------------------------------------------|-----------|
| `bar-<nom>.txt`     | `QUAND:PRET`          | hub : passer la cinématique, « Mission », « Launch »         | variable  |
| `chargement.txt`    | `QUAND:CARTE:<carte>` | écran « Press any key » : Espace toutes les 5 s (+20 → +120) | 21        |
| `triche.txt`        | `QUAND:PRET`          | code `whatisbestinlife` ×4 (une fois suffit), garde `QUIT`   | 89        |
| `victoire.txt`      | `QUAND:VICTOIRE`      | capture `victoire-{n}`, clics sur « Continue »               | 4         |

Les hubs connus : `bar-marsara.txt` (bar de Mar Sara, l'étape 2-3 éprouvée de
`scripts/entrees/bar-lancer-mission-evt.txt`), `bar-nouvelle.txt` (campagne NEUVE : après l'intro, la console
« Mission Selection » est déjà ouverte sur Liberation Day — « Launch » directement) et `bar-hyperion.txt`
(hub de l'Hyperion, après Zero Hour : onglet « Bridge », table holographique, planète `{planete}`, « Launch »,
puis l'Échap passe la cinématique d'avant-mission — jamais d'Échap la carte stellaire ouverte : il la referme).

`{n}` (numéro de la mission) est remplacé par le générateur `scripts/marathon-scenario.py` (seul
`victoire.txt` l'utilise) ; `{planete}` (bar-hyperion.txt) reçoit la coordonnée du plan. Les coordonnées
sont en 1280x720 (`REF:1280x720`, mode `@ui` sauf le clic « Mission » sur la scène).

## Le plan

`<nom>.plan` : une directive par ligne, `#` en tête de ligne commente, lignes vides ignorées.

```
hub <recette>        insère bar-<recette>.txt une fois le hub chargé (PRET du bar)
hub <recette> <x,y>  de même : la coordonnée remplace le marqueur {planete} de la brique (refusé si la
                     brique et le plan ne s'accordent pas sur le marqueur)
mission <Carte>      enchaîne la carte : chargement, triche, « Continue », retour au bar
mission *            carte inconnue : la mission est repérée à son écran de chargement (QUAND:CHARGEMENT
                     MAX:200), le reste est identique
```

- La première directive doit être un `hub` : c'est lui qui lance la première mission.
- Un `hub` par retour au bar : deux `hub` consécutifs sont refusés (le second attendrait un nouveau
  chargement qui ne viendrait jamais).
- `mission` compte les missions à partir de 1 pour `{n}` et les instantanés de progression.
- Limites du banc d'entrées : 1024 événements et 64 segments. Une mission (chargement + triche + victoire +
  retour) = 135 événements et 4 segments ; les hubs : Mar Sara 23, campagne neuve 22, Hyperion 37. La limite
  d'événements tombe vers la septième mission (l'acte 1 entier tient : `acte1.plan` = 990 événements,
  32 segments, garde 2640 s), celle de segments vers la quinzième — `scripts/marathon-scenario.py`
  refuse au-delà. Pour aller plus loin, resserrez les séries d'Échap des briques (l'Hyperion ci-dessus
  est déjà la version resserrée : Échap d'arrivée et de pont toutes les 4 s, de fin toutes les 10 s).

Exemples : `marsara.plan` (`hub marsara`, `mission TRaynor02`, `hub marsara`, `mission TRaynor03` — les
missions 2 et 3 depuis le bar de Mar Sara, avec la progression du banc) et `acte1.plan` (les SIX missions
de l'acte 1 en un lancement, depuis une campagne neuve : `hub nouvelle`, les barres de Mar Sara, puis
l'Hyperion et ses planètes — à jouer avec la progression VIDE
`~/dev/sc2-bancs/20261023/l4-marathon/progression-vide`).

## Ajouter un hub

Les hubs connus vivent dans les briques `bar-*.txt` : pour en ajouter un (autre décor du mode histoire),
sondez-le au banc (coordonnées 1280x720, marqueurs `f` pour les tours de pompe, `@ui` pour les boutons,
survol avant clic) et écrivez la brique sur le modèle des existantes — puis référencez-la dans un plan :
`hub <recette>`. Si la sélection de mission passe par une planète à cliquer, mettez le marqueur
`{planete}` dans la brique et la coordonnée dans le plan (`hub <recette> <x,y>`). Aucun changement
ailleurs : le générateur lit `bar-<recette>.txt` dans ce dossier.

Lancer le marathon : `scripts/marathon-bar.sh <binaire> <dossier> [plan] [progression]` — il génère le
scénario, joue le plan par `scripts/banc-bar.sh`, photographie la progression après chaque victoire et
écrit `<dossier>/marathon.md`.
