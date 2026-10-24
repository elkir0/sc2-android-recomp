package fr.sc2partout;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * Logique du lanceur (passe 10b), sans aucune dépendance Android : testable par javac seul
 * (android/src/test/CatalogueTest.java).
 *
 * Cartes : le dossier {@code <install>/Maps/Campaign/} (cartes extraites, au plus {@link #MAX_CARTES}).
 * Une installation normale n'a pas ce dossier : les 33 cartes de la campagne sont DANS l'archive
 * {@code Campaigns/Liberty.SC2Campaign/Base.SC2Maps}, où le moteur les trouve par le même chemin
 * « Maps/Campaign/X.SC2Map ». Dossier absent et archive présente : catalogue {@link #CARTES_ARCHIVE},
 * relevé dans le listfile de cette archive (mpqlist, 17/10).
 *
 * sc2.env : lignes KEY=VALEUR, lues par l'activité native au lancement (android/jni/sc2_android_main.c,
 * lire_env). Jouer une carte = lanceur simulé du runtime (passe 7h, voie -run de la passe 7p §3) :
 * SC2_RUN_SANS_BNET=1 et SC2_ARGS=-run Maps/Campaign/X.SC2Map (chemin complet relatif à l'install).
 *
 * V4.10 (23/10) — escarmouche contre l'IA : les cartes mêlée ne sont pas des fichiers mais des entrées du cache
 * Battle.net pré-rempli de {@link #ARCHIVE_MELEE} ; le runtime les retrouve et les prépare lui-même
 * (recomp-sc2/runtime/sc2_escarmouche.c). Le lanceur ne fait que choisir : {@link #CARTES_MELEE} (relevé par
 * SC2_ESCARMOUCHE=liste, mêmes noms et mêmes n°), race, race de l'IA, difficulté → sc2.env
 * ({@link #contenuEscarmouche}).
 */
public final class Catalogue {
    public static final int MAX_CARTES = 200;
    public static final int MAX_ENV = 64 * 1024;
    public static final String DOSSIER_CARTES = "Maps/Campaign";
    public static final String ARCHIVE_CARTES = "Campaigns/Liberty.SC2Campaign/Base.SC2Maps";
    public static final String EXTENSION = ".SC2Map";

    /** Les 33 cartes Maps/Campaign/*.SC2Map de Base.SC2Maps (listfile de l'archive), ordre alphabétique. */
    static final String[] CARTES_ARCHIVE = {
        "TArcade", "THanson01", "THanson02", "THanson03A", "THanson03B", "THorner01", "THorner02", "THorner03",
        "THorner04", "THorner05S", "TRaynor01", "TRaynor02", "TRaynor03", "TSimulator", "TStory01", "TTosh01",
        "TTosh02", "TTosh03A", "TTosh03B", "TTutorial01", "TTychus01", "TTychus02", "TTychus03", "TTychus04",
        "TTychus05", "TValerian01", "TValerian02A", "TValerian02B", "TValerian03", "TZeratul01", "TZeratul02",
        "TZeratul03", "TZeratul04",
    };

    /** V4.10 : archive qui porte le cache Battle.net des cartes mêlée (BattlenetCache\(N)Nom.SC2Map\…s2ma). */
    public static final String ARCHIVE_MELEE = "Mods/Liberty.SC2Mod/Base.SC2Data";

    /** V4.10 : les 63 cartes mêlée du cache, dans l'ordre de « SC2_ESCARMOUCHE=liste » (joueurs, puis nom ; relevé au banc
     *  le 23/10 sur l'installation de référence) — l'indice + 1 est le n° accepté par SC2_ESCARMOUCHE. */
    static final String[] CARTES_MELEE = {
        "AgriaValley", "BlisteringSands", "BurialGrounds", "Crossfire", "DesertOasis", "IncinerationZone", "JungleBasin",
        "JunkYard", "NoviceBlisteringSands", "NoviceDesertOasis", "NoviceSteppesofWar", "ScrapStation", "SteppesofWar",
        "WorldShip", "XelNagaCaverns",
        "Elysium",
        "AridWastes", "DebrisField", "DeltaQuadrant", "DiscordIV", "HighOrbit", "KulasRavine", "LostTemple", "Metalopolis",
        "MonlythRidge", "NewAntioch", "Nightmare", "NoviceDiscordIV", "NoviceKulasRavine", "NoviceMetalopolis",
        "NoviceMonlythRidge", "NoviceTerminus", "NoviceTwilightFortress", "RedStoneGulch", "SacredGround", "ScorchedHaven",
        "ShakurasPlateau", "TarsonisAssault", "Terminus", "TwilightFortress", "WarZone",
        "ArakanCitadel", "Colony426", "DigSite", "DirtSide", "Frontier", "Monsoon", "Quicksand", "TectonicRift", "TheBioLab",
        "Typhon", "UlaanDeeps",
        "Abyss", "Extinction", "ForbiddenPlanet", "HighGround", "LavaFlow", "Megaton", "Outpost", "Primeval", "SandCanyon",
        "Tempest", "ToxicSlums",
    };
    /** Nombre de joueurs de chaque carte de {@link #CARTES_MELEE} : 15 à 2, 1 à 3, 25 à 4, 11 à 6, 11 à 8. */
    static final int[] JOUEURS_MELEE = {
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        3,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    };
    /** Carte proposée par défaut (n° 13, la carte des essais du diagnostic V4.10). */
    public static final String MELEE_DEFAUT = "SteppesofWar";
    /** Races : {valeur de SC2_ESCARMOUCHE_RACE / _RACE_IA, libellé}. */
    public static final String[][] RACES = {
        { "terran", "Terran" }, { "zerg", "Zerg" }, { "protoss", "Protoss" }, { "aleatoire", "Aléatoire" },
    };
    /** Difficultés de l'IA : {valeur de SC2_ESCARMOUCHE_IA, libellé du jeu en français} — c_skirVeryEasy 0 … c_skirCheater 5. */
    public static final String[][] DIFFICULTES = {
        { "tres-facile", "Très facile" }, { "facile", "Facile" }, { "moyen", "Moyen" }, { "difficile", "Difficile" },
        { "tres-difficile", "Très difficile" }, { "dement", "Démentiel" },
    };

    private Catalogue() {}

    /** Ordre alphabétique sans casse, puis avec (listing stable). Pas de lambda : android.jar n'a pas LambdaMetafactory. */
    static final Comparator<String> ORDRE = new Comparator<String>() {
        @Override public int compare(String a, String b) { int c = a.compareToIgnoreCase(b); return c != 0 ? c : a.compareTo(b); }
    };

    /** Une carte : nom court (sans extension) et chemin relatif à l'install (séparateurs « / »). */
    public static final class Carte {
        public final String nom, chemin;
        public final boolean dansArchive;
        Carte(String nom, String chemin, boolean dansArchive) { this.nom = nom; this.chemin = chemin; this.dansArchive = dansArchive; }
    }

    /** Résultat du listing : cartes (jamais null), source lisible, message (null si rien à signaler). */
    public static final class Liste {
        public final List<Carte> cartes;
        public final String source, message;
        Liste(List<Carte> cartes, String source, String message) { this.cartes = cartes; this.source = source; this.message = message; }
    }

    static String nomCourt(String fichier) {
        int p = fichier.lastIndexOf('.');
        return p > 0 ? fichier.substring(0, p) : fichier;
    }

    static boolean estCarte(String nom) {
        return nom.length() > EXTENSION.length() && nom.regionMatches(true, nom.length() - EXTENSION.length(), EXTENSION, 0, EXTENSION.length());
    }

    /** Cartes jouables de cette install. Ne lève rien : toute erreur devient une liste vide et un message. */
    public static Liste lister(File install) {
        List<Carte> l = new ArrayList<Carte>();
        try {
            File dossier = new File(install, DOSSIER_CARTES);
            if (dossier.isDirectory()) {
                String[] noms = dossier.list();
                if (noms == null)
                    return new Liste(l, "dossier " + DOSSIER_CARTES, "Dossier " + dossier + " illisible (accès « tous les fichiers » accordé ?)");
                List<String> cartes = new ArrayList<String>();
                for (String n : noms) if (estCarte(n)) cartes.add(n);
                Collections.sort(cartes, ORDRE);
                int total = cartes.size();
                for (String n : cartes.subList(0, Math.min(total, MAX_CARTES)))
                    l.add(new Carte(nomCourt(n), DOSSIER_CARTES + "/" + n, false));
                String msg = total == 0 ? "Aucune carte " + EXTENSION + " dans " + dossier
                           : total > MAX_CARTES ? MAX_CARTES + " premières cartes affichées sur " + total : null;
                return new Liste(l, "dossier " + DOSSIER_CARTES, msg);
            }
            if (new File(install, ARCHIVE_CARTES).isFile()) {
                for (String n : CARTES_ARCHIVE) l.add(new Carte(n, DOSSIER_CARTES + "/" + n + EXTENSION, true));
                return new Liste(l, "archive " + ARCHIVE_CARTES, null);
            }
            return new Liste(l, "aucune", "Ni " + new File(install, DOSSIER_CARTES) + " ni " + new File(install, ARCHIVE_CARTES)
                             + " : données absentes, ou accès « tous les fichiers » non accordé");
        } catch (SecurityException e) {
            return new Liste(new ArrayList<Carte>(), "aucune", "Accès refusé à " + install + " : " + e.getMessage());
        }
    }

    /** La carte est-elle toujours là au moment du clic ? (fichier/dossier extrait, ou archive des cartes) */
    public static boolean disponible(File install, Carte c) {
        try {
            return c.dansArchive ? new File(install, ARCHIVE_CARTES).isFile() : new File(install, c.chemin).exists();
        } catch (SecurityException e) {
            return false;
        }
    }

    /** Contenu de sc2.env (UTF-8, au plus MAX_ENV octets), ou null s'il n'existe pas. */
    public static String lireEnv(File f) throws IOException {
        if (!f.isFile()) return null;
        InputStream in = new FileInputStream(f);
        try {
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            byte[] b = new byte[4096];
            int n;
            while (o.size() < MAX_ENV && (n = in.read(b, 0, Math.min(b.length, MAX_ENV - o.size()))) > 0) o.write(b, 0, n);
            return new String(o.toByteArray(), StandardCharsets.UTF_8);
        } finally {
            in.close();
        }
    }

    /** « Menu principal » : SC2_RUN_SANS_BNET=1 seul (plus de SC2_ARGS). */
    public static String contenuMenu() {
        return "SC2_RUN_SANS_BNET=1\n";
    }

    /** Jouer une carte : voie -run du lanceur simulé (7h/7p). Guillemets si le chemin contient une espace
     *  (args_decouper du runtime les comprend). */
    public static String contenuCarte(String chemin) {
        String c = chemin.indexOf(' ') >= 0 ? "\"" + chemin + "\"" : chemin;
        return "SC2_RUN_SANS_BNET=1\nSC2_ARGS=-run " + c + "\n";
    }

    /** V4.10 : une carte mêlée du cache (nom, nombre de joueurs, n° de « SC2_ESCARMOUCHE=liste »). */
    public static final class Melee {
        public final String nom;
        public final int joueurs, numero;
        Melee(String nom, int joueurs, int numero) { this.nom = nom; this.joueurs = joueurs; this.numero = numero; }
        /** Libellé du lanceur : « SteppesofWar (2 joueurs) ». */
        public String libelle() { return nom + " (" + joueurs + " joueurs)"; }
    }

    /** V4.10 : les cartes mêlée, dans l'ordre de « SC2_ESCARMOUCHE=liste ». */
    public static List<Melee> cartesMelee() {
        List<Melee> l = new ArrayList<Melee>();
        for (int i = 0; i < CARTES_MELEE.length; i++) l.add(new Melee(CARTES_MELEE[i], JOUEURS_MELEE[i], i + 1));
        return l;
    }

    /** V4.10 : le cache des cartes mêlée est-il là ? (sinon le runtime journalise l'échec et lance le jeu normalement) */
    public static boolean escarmoucheDisponible(File install) {
        try {
            return new File(install, ARCHIVE_MELEE).isFile();
        } catch (SecurityException e) {
            return false;
        }
    }

    /** Indice de valeur dans t (colonne 0 si t est un tableau de paires), sans casse ; defaut si absente ou null. */
    static int indice(String[][] t, String valeur, int defaut) {
        if (valeur != null)
            for (int i = 0; i < t.length; i++) if (t[i][0].equalsIgnoreCase(valeur.trim())) return i;
        return defaut;
    }

    /** Indice de la carte mêlée nommée (sans casse), ou defaut. */
    static int indiceMelee(String nom, int defaut) {
        if (nom != null)
            for (int i = 0; i < CARTES_MELEE.length; i++) if (CARTES_MELEE[i].equalsIgnoreCase(nom.trim())) return i;
        return defaut;
    }

    /** Valeur de la clé dans un contenu sc2.env (dernière ligne « cle=… » : lire_env de l'activité native les applique
     *  dans l'ordre), ou null ; null si contenu est null. Lignes « # » ignorées, comme dans lire_env. */
    public static String valeurEnv(String contenu, String cle) {
        if (contenu == null) return null;
        String v = null;
        for (String l : contenu.split("\\r?\\n")) {
            if (l.startsWith("#")) continue;
            int eg = l.indexOf('=');
            if (eg > 0 && l.substring(0, eg).equals(cle)) v = l.substring(eg + 1);
        }
        return v;
    }

    /** V4.10 — escarmouche contre l'IA : lanceur simulé + carte mêlée préparée par le runtime (SC2_ESCARMOUCHE). Pas de
     *  SC2_ARGS (un « -run » l'emporterait sur la carte d'escarmouche). race, raceIA : colonne 0 de {@link #RACES} ;
     *  difficulte : colonne 0 de {@link #DIFFICULTES}. */
    public static String contenuEscarmouche(String carte, String race, String raceIA, String difficulte) {
        return "SC2_RUN_SANS_BNET=1\nSC2_ESCARMOUCHE=" + carte + "\nSC2_ESCARMOUCHE_RACE=" + race
             + "\nSC2_ESCARMOUCHE_RACE_IA=" + raceIA + "\nSC2_ESCARMOUCHE_IA=" + difficulte + "\n";
    }

    /** V5 : chargement de sauvegardes pris en charge par le runtime depuis le lot L3 (24/10, SC2_SAUVEGARDE) — l'écran
     *  « Charger une partie » n'est plus gardé. */
    public static final boolean CHARGEMENT_PRET = true;

    /** V5 — charger une sauvegarde : lanceur simulé + SC2_SAUVEGARDE (L3, docs/VARIABLES.md : le runtime démarre le jeu
     *  directement sur la sauvegarde, mode 4 du lanceur simulé). cheminRelatifAuxDocuments : chemin du .SC2Save relatif
     *  aux documents du jeu (SC2_DOCUMENTS), séparateurs « / », p. ex. « Saves/Campaign/The Outlaws.SC2Save ». */
    public static String contenuSauvegarde(String cheminRelatifAuxDocuments) {
        return "SC2_RUN_SANS_BNET=1\nSC2_SAUVEGARDE=" + cheminRelatifAuxDocuments.replace('\\', '/') + "\n";
    }

    /** Écrit sc2.env : fichier temporaire voisin puis renommage (jamais de sc2.env à moitié écrit). */
    public static void ecrire(File cible, String contenu) throws IOException {
        File dir = cible.getAbsoluteFile().getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new IOException("dossier " + dir + " impossible à créer");
        File tmp = new File(dir, cible.getName() + ".tmp");
        OutputStream o = new FileOutputStream(tmp);
        try {
            o.write(contenu.getBytes(StandardCharsets.UTF_8));
        } finally {
            o.close();
        }
        if (!tmp.renameTo(cible)) {
            if (cible.exists() && !cible.delete()) { tmp.delete(); throw new IOException(cible + " non remplaçable"); }
            if (!tmp.renameTo(cible)) { tmp.delete(); throw new IOException("renommage " + tmp + " → " + cible + " refusé"); }
        }
    }

    static List<String> noms(List<Carte> l) {
        List<String> r = new ArrayList<String>();
        for (Carte c : l) r.add(c.nom);
        return r;
    }

    static List<String> cartesArchive() { return Arrays.asList(CARTES_ARCHIVE); }
}
