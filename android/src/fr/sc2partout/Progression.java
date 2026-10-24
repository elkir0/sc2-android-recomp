package fr.sc2partout;

import java.io.File;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Progression du joueur (V5), lue dans les documents du jeu (/sdcard/SC2Partout/StarCraft II) : banques
 * *.SC2Bank pour la seconde ligne du bouton « Continuer » ({@link #resume}), sauvegardes *.SC2Save pour
 * l'écran « Charger une partie » ({@link #sauvegardes}). Aucune dépendance Android : testable par javac
 * seul (android/src/test/OptionsTest.java).
 */
public final class Progression {

    /** Profondeur maximale de la recherche de sauvegardes sous le dossier Saves. */
    public static final int PROFONDEUR_MAX = 4;

    /** Une sauvegarde : fichier, date de modification, chemin relatif au dossier Saves (séparateurs « / »). */
    public static final class Sauvegarde {
        public final File fichier;
        public final long date;
        public final String relatif;
        Sauvegarde(File fichier, long date, String relatif) {
            this.fichier = fichier;
            this.date = date;
            this.relatif = relatif;
        }
        /** Nom du fichier sans l'extension. */
        public String nom() {
            String n = relatif.substring(relatif.lastIndexOf('/') + 1);
            return n.substring(0, n.length() - ".SC2Save".length());
        }
        /** Dossier parent relatif à Saves (« Unsaved », « Campagne », « … ») ; vide à la racine de Saves. */
        public String dossier() {
            int p = relatif.lastIndexOf('/');
            return p >= 0 ? relatif.substring(0, p) : "";
        }
        /** Libellé du bouton : « nom — dd/MM HH:mm ». */
        public String libelle() {
            return nom() + " — " + new SimpleDateFormat("dd/MM HH:mm", Locale.ROOT).format(new Date(date));
        }
    }

    /** Ordre d'affichage : la plus récente d'abord (date décroissante), puis chemin croissant. */
    static final Comparator<Sauvegarde> ORDRE = new Comparator<Sauvegarde>() {
        @Override public int compare(Sauvegarde a, Sauvegarde b) {
            if (a.date != b.date) return a.date > b.date ? -1 : 1;
            return a.relatif.compareTo(b.relatif);
        }
    };

    private Progression() {}

    /** Les sauvegardes du dossier, de la plus récente à la plus ancienne, au plus max : fichiers *.SC2Save
     *  (sans casse) à PROFONDEUR_MAX niveaux sous savesDir, récursifs ; liens symboliques jamais suivis,
     *  dossiers illisibles ignorés, dossier absent → liste vide. */
    public static List<Sauvegarde> sauvegardes(File savesDir, int max) {
        List<Sauvegarde> l = new ArrayList<Sauvegarde>();
        if (max > 0 && savesDir != null) parcourir(savesDir, "", 1, l);
        Collections.sort(l, ORDRE);
        return l.size() > max ? new ArrayList<Sauvegarde>(l.subList(0, max)) : l;
    }

    private static void parcourir(File dossier, String relatif, int profondeur, List<Sauvegarde> l) {
        if (profondeur > PROFONDEUR_MAX) return;
        File[] fs = null;
        try {
            fs = dossier.listFiles();
        } catch (SecurityException e) {
            return;                                                /* dossier illisible : ignoré */
        }
        if (fs == null) return;
        for (File f : fs) {
            if (Files.isSymbolicLink(f.toPath())) continue;        /* jamais suivi */
            String r = relatif.length() == 0 ? f.getName() : relatif + "/" + f.getName();
            if (f.isDirectory()) parcourir(f, r, profondeur + 1, l);
            else if (f.isFile() && estSauvegarde(f.getName())) l.add(new Sauvegarde(f, f.lastModified(), r));
        }
    }

    /** L3 (24/10) : taille minimale d'une sauvegarde utilisable. Avant le correctif du runtime, le jeu écrivait des
     *  archives VIDES de 1 181 o (« Save Failed », victoires des bancs et du Fold) : le jeu ne sait pas les charger. */
    public static final long TAILLE_MIN = 4096;

    /** La sauvegarde peut-elle être chargée (au moins TAILLE_MIN octets) ? */
    public static boolean utilisable(Sauvegarde s) {
        return s != null && s.fichier.length() >= TAILLE_MIN;
    }

    /** Seconde ligne du bouton « Continuer » : date de la banque la plus récente du dossier Banks
     *  (« Progression du dd/MM à HH:mm »), ou « Nouvelle campagne » sans banque. Horodatage borné à
     *  maintenant (horloge du téléphone mal réglée). */
    public static String resume(File banksDir, long maintenant) {
        long derniere = -1;
        File[] banques = null;
        try {
            banques = banksDir == null ? null : banksDir.listFiles();
        } catch (SecurityException e) {
            banques = null;
        }
        if (banques != null)
            for (File f : banques)
                if (f.isFile() && estBanque(f.getName()) && f.lastModified() > derniere) derniere = f.lastModified();
        if (derniere < 0) return "Nouvelle campagne";
        if (derniere > maintenant) derniere = maintenant;
        Date d = new Date(derniere);
        return "Progression du " + new SimpleDateFormat("dd/MM", Locale.ROOT).format(d)
             + " à " + new SimpleDateFormat("HH:mm", Locale.ROOT).format(d);
    }

    static boolean estSauvegarde(String nom) {
        return nom.length() > 8 && nom.regionMatches(true, nom.length() - 8, ".SC2Save", 0, 8);
    }

    static boolean estBanque(String nom) {
        return nom.length() > 8 && nom.regionMatches(true, nom.length() - 8, ".SC2Bank", 0, 8);
    }
}
