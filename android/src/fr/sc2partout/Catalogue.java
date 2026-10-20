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
