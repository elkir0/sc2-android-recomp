package fr.sc2partout;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * État de l'installation des données (passe 10c), sans aucune dépendance Android : testable par javac seul
 * (android/src/test/EtatInstallTest.java, sonde factice chemin → taille).
 *
 * Pourquoi : lancer le jeu pendant qu'un push de données est en cours donne un écran noir immédiat qui IMITE un bug du
 * moteur (piège vécu). Le lanceur compare donc chaque fichier attendu à sa taille réelle avant de proposer « Jouer ».
 *
 * Tailles attendues ({@link #ATTENDUS}) — d'où vient chaque constante :
 *   F = dernier déploiement connu du Fold : le jeu de données « 5,8 Go » (NOTES.md 17/10 22:50 AST « données 5,8 Go
 *       intactes » sur le Fold), poussé du M4 au serveur Linux (extraction/install, sentinelle .POUSSE_FINI
 *       2026-10-17T05:19:14Z) et mesuré là, fichier par fichier, le 17/10 (find -printf %s) ;
 *   M = android/manifest-install.txt (installation de référence DVD, passes 6a/7h) pour les fichiers ABSENTS de ce
 *       déploiement — exigés par les missions (sans Liberty.SC2Mod/Base.SC2Data : dialogue « GameLaunchInvalidFiles »,
 *       BUS 8e3) ; le Fold atteint le menu sans eux.
 * Niveaux : MINIMUM = boot et écran-titre (Versions + Mods + Support, ~2,4 Go : documenté « ~2,5 Go ») ; CAMPAGNE =
 * en plus, pour jouer une carte ; OPTIONNEL (passe 10d) = les 8 archives de locale enUS (M) : libellés @UI/… du menu
 * natif (GameStrings.txt) et GameHotkeys.txt (Échap / F10), voix et briefings. Le jeu démarre et joue sans elles
 * (déploiement Fold) : elles ne font PAS partie du minimum viable ni du verdict COMPLET/PARTIEL, seulement affichées.
 */
public final class EtatInstall {
    public static final int MINIMUM = 0, CAMPAGNE = 1, OPTIONNEL = 2;
    public static final String COMPLET = "COMPLET", PARTIEL = "PARTIEL", ABSENT = "ABSENT";

    /** Ce que le lanceur sait de l'install : taille d'un fichier (-1 : absent), présence d'un dossier. */
    public interface Sonde {
        long taille(String chemin);
        boolean dossier(String chemin);
    }

    /** Sonde du vrai système de fichiers (File.length / isFile / isDirectory). */
    public static Sonde fichiers(final File racine) {
        return new Sonde() {
            @Override public long taille(String c) {
                try { File f = new File(racine, c); return f.isFile() ? f.length() : -1; } catch (SecurityException e) { return -1; }
            }
            @Override public boolean dossier(String c) {
                try { return new File(racine, c).isDirectory(); } catch (SecurityException e) { return false; }
            }
        };
    }

    /** Un fichier attendu : chemin relatif à l'install, taille, niveau, source de la constante (F ou M). */
    public static final class Attendu {
        public final String composant, chemin, source;
        public final long taille;
        public final int niveau;
        Attendu(String composant, String chemin, long taille, int niveau, String source) {
            this.composant = composant; this.chemin = chemin; this.taille = taille; this.niveau = niveau; this.source = source;
        }
    }

    static final String C_VERSIONS = "Versions/Base15405 (SC2.exe)", C_SHADERS = "Shaders (Versions/Shaders14513)",
        C_CORE = "Mods/Core.SC2Mod", C_LIBERTY = "Mods/Liberty.SC2Mod (+ Base.SC2Data)",
        C_MULTI = "Mods/LibertyMulti.SC2Mod (+ Base.SC2Data)", C_SUPPORT = "Support/", C_CAMPAGNES = "Campaigns/",
        C_ENUS = "Locale enUS (optionnelle : libellés du menu, raccourcis, voix)";

    /** La map nom → taille : petites constantes, pas de fichier externe à maintenir. */
    static final Attendu[] ATTENDUS = {
        new Attendu(C_VERSIONS, "Versions/Base15405/SC2.exe", 16449536L, MINIMUM, "F"),
        new Attendu(C_SHADERS, "Versions/Shaders14513/baselineCache.bin", 141923542L, MINIMUM, "F"),   // sans lui : écran noir (7y)
        new Attendu(C_CORE, "Mods/Core.SC2Mod/Base.SC2Data", 8238904L, MINIMUM, "F"),
        new Attendu(C_CORE, "Mods/Core.SC2Mod/base.SC2Assets", 29653983L, MINIMUM, "F"),
        new Attendu(C_CORE, "Mods/Core.SC2Mod/Index.SC2Locale", 611L, MINIMUM, "F"),
        new Attendu(C_LIBERTY, "Mods/Liberty.SC2Mod/base.SC2Assets", 2405613201L, MINIMUM, "F"),
        new Attendu(C_LIBERTY, "Mods/Liberty.SC2Mod/Base.SC2Data", 99082368L, CAMPAGNE, "M"),
        new Attendu(C_MULTI, "Mods/LibertyMulti.SC2Mod/Base.SC2Data", 17277L, CAMPAGNE, "M"),
        new Attendu(C_CAMPAGNES, "Campaigns/Liberty.SC2Campaign/base.SC2Assets", 3489270406L, CAMPAGNE, "F"),
        new Attendu(C_CAMPAGNES, "Campaigns/Liberty.SC2Campaign/Base.SC2Data", 964116L, CAMPAGNE, "F"),
        new Attendu(C_CAMPAGNES, "Campaigns/Liberty.SC2Campaign/Base.SC2Maps", 71836547L, CAMPAGNE, "F"),
        new Attendu(C_CAMPAGNES, "Campaigns/LibertyStory.SC2Campaign/Base.SC2Data", 2040300L, CAMPAGNE, "F"),
        // passe 10d : locale enUS, OPTIONNELLE (tailles M : manifest-install.txt, absente du serveur Linux comme du Fold)
        new Attendu(C_ENUS, "Mods/Core.SC2Mod/enUS.SC2Data", 885052L, OPTIONNEL, "M"),
        new Attendu(C_ENUS, "Mods/Core.SC2Mod/enUS.SC2Assets", 863964L, OPTIONNEL, "M"),
        new Attendu(C_ENUS, "Mods/Liberty.SC2Mod/enUS.SC2Data", 164576L, OPTIONNEL, "M"),
        new Attendu(C_ENUS, "Mods/Liberty.SC2Mod/enUS.SC2Assets", 336321819L, OPTIONNEL, "M"),
        new Attendu(C_ENUS, "Mods/LibertyMulti.SC2Mod/enUS.SC2Data", 3245L, OPTIONNEL, "M"),
        new Attendu(C_ENUS, "Campaigns/Liberty.SC2Campaign/enUS.SC2Data", 254482L, OPTIONNEL, "M"),
        new Attendu(C_ENUS, "Campaigns/Liberty.SC2Campaign/enUS.SC2Assets", 934371560L, OPTIONNEL, "M"),
        new Attendu(C_ENUS, "Campaigns/LibertyStory.SC2Campaign/enUS.SC2Data", 442170L, OPTIONNEL, "M"),
    };
    static final String[] ORDRE = { C_VERSIONS, C_SHADERS, C_CORE, C_LIBERTY, C_MULTI, C_SUPPORT, C_CAMPAGNES, C_ENUS };
    /** Support/ : dossier de travail du jeu, vide suffit (les DLL sont émulées) — présence seule, niveau MINIMUM. */
    static final String DOSSIER_SUPPORT = "Support";

    /** Un fichier vérifié : taille réelle (-1 absent) ; ok si égale à l'attendue ; plus grande = autre version (signalé). */
    public static final class Ligne {
        public final Attendu a;
        public final long reel;
        Ligne(Attendu a, long reel) { this.a = a; this.reel = reel; }
        public boolean present() { return reel >= 0; }
        public boolean ok() { return reel == a.taille; }
        public String etat() {
            return reel < 0 ? "absent" : reel == a.taille ? "ok" : reel < a.taille ? "incomplet (" + Etat.go(reel) + " / " + Etat.go(a.taille) + ")"
                 : "taille différente (" + reel + " o, attendu " + a.taille + ")";
        }
    }

    /** Résultat : lignes, composants, verdict global, octets présents / attendus, minimum et campagne atteints, motifs. */
    public static final class Etat {
        public final List<Ligne> lignes;
        public final boolean support;
        public final String global;
        public final long presents, attendus;
        public final boolean minimum, campagne;
        public final List<String> manqueMinimum, manqueCampagne, manqueOptionnel;

        Etat(List<Ligne> lignes, boolean support, String global, long presents, long attendus, boolean minimum, boolean campagne,
             List<String> manqueMinimum, List<String> manqueCampagne, List<String> manqueOptionnel) {
            this.lignes = lignes; this.support = support; this.global = global; this.presents = presents; this.attendus = attendus;
            this.minimum = minimum; this.campagne = campagne; this.manqueMinimum = manqueMinimum; this.manqueCampagne = manqueCampagne;
            this.manqueOptionnel = manqueOptionnel;
        }

        /** Octets en Go (Gio, comme du -h : le jeu « 5,8 Go »), une décimale, virgule française. */
        public static String go(long o) {
            long dixiemes = Math.round(o / (1024.0 * 1024.0 * 1024.0) * 10);
            return (dixiemes / 10) + "," + (dixiemes % 10) + " Go";
        }

        /** « COMPLET », « PARTIEL (x Go / ~6 Go) » ou « ABSENT ». */
        public String resume() {
            return global.equals(PARTIEL) ? PARTIEL + " (" + go(presents) + " / ~" + go(attendus) + ")" : global;
        }

        /** Composant → état agrégé (ok / incomplet / absent), dans l'ordre d'affichage. */
        public Map<String, String> composants() {
            Map<String, String> m = new LinkedHashMap<String, String>();
            for (String c : ORDRE) m.put(c, c.equals(C_SUPPORT) ? (support ? "ok" : "absent") : null);
            for (Ligne l : lignes) {
                String e = l.present() && l.reel >= l.a.taille ? "ok" : l.present() ? "incomplet" : "absent";
                String p = m.get(l.a.composant);
                m.put(l.a.composant, p == null || p.equals(e) ? e : "incomplet");
            }
            return m;
        }
    }

    private EtatInstall() {}

    /** Évalue l'install vue par la sonde. Un fichier PLUS GRAND que l'attendu (autre version) compte comme présent.
     *  Les OPTIONNELS sont relevés (lignes, manqueOptionnel) mais hors verdict, totaux, minimum et campagne. */
    public static Etat evaluer(Sonde s) {
        List<Ligne> lignes = new ArrayList<Ligne>();
        List<String> mMin = new ArrayList<String>(), mCamp = new ArrayList<String>(), mOpt = new ArrayList<String>();
        long presents = 0, attendus = 0;
        boolean rien = true;
        for (Attendu a : ATTENDUS) {
            long t = s.taille(a.chemin);
            Ligne l = new Ligne(a, t);
            lignes.add(l);
            if (a.niveau == OPTIONNEL) { if (t < a.taille) mOpt.add(a.chemin + " : " + l.etat()); continue; }
            attendus += a.taille;
            if (t >= 0) { rien = false; presents += Math.min(t, a.taille); }
            boolean bon = t >= a.taille;
            if (!bon) (a.niveau == MINIMUM ? mMin : mCamp).add(a.chemin + " : " + l.etat());
        }
        boolean support = s.dossier(DOSSIER_SUPPORT);
        if (!support) mMin.add(DOSSIER_SUPPORT + "/ : absent");
        else rien = false;
        boolean minimum = mMin.isEmpty(), campagne = minimum && mCamp.isEmpty();
        boolean toutEgal = support;
        for (Ligne l : lignes) if (l.a.niveau != OPTIONNEL) toutEgal &= l.present() && l.reel >= l.a.taille;
        String global = rien ? ABSENT : toutEgal ? COMPLET : PARTIEL;
        return new Etat(Collections.unmodifiableList(lignes), support, global, presents, attendus, minimum, campagne,
                        Collections.unmodifiableList(mMin), Collections.unmodifiableList(mCamp), Collections.unmodifiableList(mOpt));
    }

    /** Motif court pour un bouton grisé : premier manque (+ nombre des autres). */
    public static String motif(List<String> manques) {
        if (manques.isEmpty()) return "";
        return manques.get(0) + (manques.size() > 1 ? " (+ " + (manques.size() - 1) + " autre(s))" : "");
    }
}
