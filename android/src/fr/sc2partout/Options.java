package fr.sc2partout;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.List;

/**
 * Options du lanceur (V5) : réglages de l'écran « Options », stockés dans /sdcard/SC2Partout/options.env
 * (lignes KEY=VALEUR, lues et écrites comme sc2.env) et combinés à CHAQUE lancement par {@link #composer}
 * (le contenu de lancement l'emporte sur une option de même clé). Le préréglage graphique est à part :
 * il remplace /sdcard/SC2Partout/Variables.txt par une copie octet à octet de {@link Prereglages},
 * l'ancien fichier étant sauvegardé ({@link #appliquerPrereglage}). Aucune dépendance Android : testable
 * par javac seul (android/src/test/OptionsTest.java).
 */
public final class Options {

    /** Résolutions : {valeur de SC2_ECRAN (docs/VARIABLES.md), libellé} — « auto » = plein écran tel quel. */
    public static final String[][] RESOLUTIONS = {
        { "auto", "Plein écran (auto)" },
        { "1280x720", "1280×720" },
        { "1280x1024", "1280×1024" },
        { "1600x1200", "1600×1200" },
        { "1920x1080", "1920×1080" },
    };

    /** Ombres : position 0 = selon le réglage du jeu (aucune ligne), 1 = coupées (SC2_OMBRES=0). */
    public static final String[] OMBRES = { "Selon le réglage du jeu", "Coupées" };

    /** Particules (instanciation) : position 0 = activées (aucune ligne), 1 = coupées (SC2_INSTANCES=0). */
    public static final String[] PARTICULES = { "Activées", "Coupées" };

    /** Préréglages graphiques : {nom de fichier (low…ultra, scripts/prereglages/), libellé}. */
    public static final String[][] PRESETS = {
        { "low", "Faible" }, { "medium", "Moyen" }, { "high", "Élevé" }, { "ultra", "Ultra" },
    };

    private Options() {}

    /** Contenu d'options.env pour ces choix (lignes SC2_* documentées ; rien pour un réglage « selon le jeu »). */
    public static String contenu(String resolution, boolean ombresCoupees, boolean particulesCoupees) {
        StringBuilder sb = new StringBuilder();
        sb.append("SC2_ECRAN=").append(resolution != null ? resolution : "auto").append('\n');
        if (ombresCoupees) sb.append("SC2_OMBRES=0\n");
        if (particulesCoupees) sb.append("SC2_INSTANCES=0\n");
        return sb.toString();
    }

    /** Indice de la résolution de ce options.env dans {@link #RESOLUTIONS} (0 = auto si absente ou inconnue). */
    public static int indiceResolution(String contenu) {
        return Catalogue.indice(RESOLUTIONS, Catalogue.valeurEnv(contenu, "SC2_ECRAN"), 0);
    }

    /** Ombres coupées dans ce options.env (SC2_OMBRES=0) ? */
    public static boolean ombresCoupees(String contenu) {
        return "0".equals(Catalogue.valeurEnv(contenu, "SC2_OMBRES"));
    }

    /** Particules coupées dans ce options.env (SC2_INSTANCES=0) ? */
    public static boolean particulesCoupees(String contenu) {
        return "0".equals(Catalogue.valeurEnv(contenu, "SC2_INSTANCES"));
    }

    /** Indice d'un préréglage dans {@link #PRESETS}, defaut si le nom est null ou inconnu. */
    public static int indicePreset(String nom, int defaut) {
        return Catalogue.indice(PRESETS, nom, defaut);
    }

    /** sc2.env final : lignes d'options PUIS lignes du contenu de lancement. Clé répétée : la ligne de
     *  LANCEMENT l'emporte (celle de l'option est perdue). Lignes vides, commentaires « # » et lignes sans
     *  « = » des options perdus ; le résultat, s'il n'est pas vide, finit par un saut de ligne. */
    public static String composer(String optionsEnv, String contenuLancement) {
        List<String> cles = new ArrayList<String>();
        if (contenuLancement != null)
            for (String l : contenuLancement.split("\\r?\\n")) {
                int eg = l.indexOf('=');
                if (eg > 0) cles.add(l.substring(0, eg));
            }
        StringBuilder sb = new StringBuilder();
        if (optionsEnv != null)
            for (String l : optionsEnv.split("\\r?\\n")) {
                String t = l.trim();
                if (t.length() == 0 || t.startsWith("#")) continue;
                int eg = t.indexOf('=');
                if (eg <= 0 || cles.contains(t.substring(0, eg))) continue;
                sb.append(t).append('\n');
            }
        if (contenuLancement != null) sb.append(contenuLancement);
        if (sb.length() > 0 && sb.charAt(sb.length() - 1) != '\n') sb.append('\n');
        return sb.toString();
    }

    /** Contenu du préréglage nommé (low/medium/high/ultra), ou null si le nom est inconnu. */
    public static String contenuPreset(String nom) {
        if (nom == null) return null;
        String n = nom.trim().toLowerCase(java.util.Locale.ROOT);
        if (n.equals("low")) return Prereglages.LOW;
        if (n.equals("medium")) return Prereglages.MEDIUM;
        if (n.equals("high")) return Prereglages.HIGH;
        if (n.equals("ultra")) return Prereglages.ULTRA;
        return null;
    }

    /** Applique le préréglage à racine/Variables.txt : l'ancien fichier, s'il existe, est d'abord recopié
     *  OCTET À OCTET vers Variables.txt.sauvegarde-<yyyyMMdd-HHmmss> (suffixe -2, -3… si la même seconde
     *  réapplique) ; retourne cette sauvegarde, ou null s'il n'y avait rien à sauvegarder. */
    public static File appliquerPrereglage(File racine, String nom, long maintenant) throws IOException {
        String preset = contenuPreset(nom);
        if (preset == null) throw new IOException("préréglage inconnu : " + nom);
        File variables = new File(racine, "Variables.txt");
        File sauvegarde = null;
        if (variables.isFile()) {
            String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss").format(new Date(maintenant));
            sauvegarde = new File(racine, "Variables.txt.sauvegarde-" + stamp);
            for (int n = 2; sauvegarde.exists(); n++)                 /* deux applications dans la même seconde */
                sauvegarde = new File(racine, "Variables.txt.sauvegarde-" + stamp + "-" + n);
            copier(variables, sauvegarde);
        }
        Catalogue.ecrire(variables, preset);
        return sauvegarde;
    }

    /** Nom du préréglage appliqué dans ce contenu de Variables.txt, ou null : seules les lignes
     *  « GraphicsOption… » sont comparées (le jeu ajoute les siennes, lastDeviceId…). */
    public static String prereglageActuel(String contenu) {
        if (contenu == null) return null;
        List<String> actuelles = lignesGraphiques(contenu);
        for (String[] p : PRESETS)
            if (actuelles.equals(lignesGraphiques(contenuPreset(p[0])))) return p[0];
        return null;
    }

    /** Lignes « GraphicsOption… » du contenu, triées (comparaison indépendante de l'ordre du fichier). */
    private static List<String> lignesGraphiques(String contenu) {
        List<String> r = new ArrayList<String>();
        for (String l : contenu.split("\\r?\\n"))
            if (l.startsWith("GraphicsOption")) r.add(l);
        Collections.sort(r);
        return r;
    }

    private static void copier(File source, File cible) throws IOException {
        FileInputStream i = new FileInputStream(source);
        try {
            FileOutputStream o = new FileOutputStream(cible);
            try {
                byte[] b = new byte[8192];
                int n;
                while ((n = i.read(b)) > 0) o.write(b, 0, n);
            } finally {
                o.close();
            }
        } finally {
            i.close();
        }
    }
}
