package fr.sc2partout;

import java.io.File;
import java.util.HashMap;
import java.util.Map;

/**
 * Test hors Android de l'état de l'installation (passe 10c) : sondes factices chemin → taille, verdicts attendus.
 *
 *   javac -encoding UTF-8 -d <classes> android/src/fr/sc2partout/EtatInstall.java android/src/test/EtatInstallTest.java
 *   java -cp <classes> fr.sc2partout.EtatInstallTest [<install réelle, lecture seule>]
 *
 * Sortie : une ligne OK/ÉCHEC par vérification, code 1 au moindre échec.
 */
public final class EtatInstallTest {
    static int oks, echecs;

    static void verifier(boolean ok, String quoi) {
        if (ok) oks++; else echecs++;
        System.out.println((ok ? "OK     " : "ÉCHEC  ") + quoi);
    }

    /** Sonde factice : chemins → tailles, dossiers présents. */
    static final class Factice implements EtatInstall.Sonde {
        final Map<String, Long> t = new HashMap<String, Long>();
        boolean support = true;
        public long taille(String c) { Long v = t.get(c); return v == null ? -1 : v; }
        public boolean dossier(String c) { return c.equals("Support") && support; }
    }

    /** Le dernier déploiement du Fold : toutes les constantes « F », aucune « M ». */
    static Factice fold() {
        Factice f = new Factice();
        for (EtatInstall.Attendu a : EtatInstall.ATTENDUS) if (a.source.equals("F")) f.t.put(a.chemin, a.taille);
        return f;
    }

    static void montrer(String nom, EtatInstall.Etat e) {
        System.out.println("  " + nom + " : " + e.resume() + " ; minimum " + e.minimum + " ; campagne " + e.campagne + " ; composants " + e.composants());
        if (!e.manqueMinimum.isEmpty()) System.out.println("    manque (minimum) : " + e.manqueMinimum);
        if (!e.manqueCampagne.isEmpty()) System.out.println("    manque (campagne) : " + e.manqueCampagne);
    }

    public static void main(String[] args) {
        long total = 0, minimum = 0, optionnels = 0; int nOpt = 0;
        for (EtatInstall.Attendu a : EtatInstall.ATTENDUS) {
            if (a.niveau == EtatInstall.OPTIONNEL) { optionnels += a.taille; nOpt++; continue; }
            total += a.taille; if (a.niveau == EtatInstall.MINIMUM) minimum += a.taille;
        }
        System.out.println("  attendu : " + total + " o (" + EtatInstall.Etat.go(total) + "), minimum " + minimum + " o (" + EtatInstall.Etat.go(minimum) + ")");
        verifier(EtatInstall.ATTENDUS.length == 20 && nOpt == 8 && EtatInstall.Etat.go(total).equals("5,8 Go") && EtatInstall.Etat.go(minimum).equals("2,4 Go"),
                 "constantes : 12 fichiers requis (5,8 Go, minimum 2,4 Go) + 8 optionnels enUS (" + EtatInstall.Etat.go(optionnels) + ", hors totaux)");

        // 1. le déploiement du Fold (5,8 Go) : menu possible, missions non (Liberty Base.SC2Data + LibertyMulti absents)
        EtatInstall.Etat e1 = EtatInstall.evaluer(fold());
        montrer("déploiement Fold", e1);
        verifier(e1.global.equals(EtatInstall.PARTIEL) && e1.minimum && !e1.campagne, "déploiement Fold : PARTIEL, minimum atteint, campagne non");
        verifier(e1.manqueCampagne.size() == 2 && e1.manqueCampagne.get(0).startsWith("Mods/Liberty.SC2Mod/Base.SC2Data : absent")
                 && e1.manqueCampagne.get(1).startsWith("Mods/LibertyMulti.SC2Mod/Base.SC2Data : absent"), "déploiement Fold : manquent Liberty Base.SC2Data et LibertyMulti");
        verifier("incomplet".equals(e1.composants().get(EtatInstall.C_LIBERTY)) && "absent".equals(e1.composants().get(EtatInstall.C_MULTI))
                 && "ok".equals(e1.composants().get(EtatInstall.C_VERSIONS)) && "ok".equals(e1.composants().get(EtatInstall.C_SUPPORT)),
                 "composants : Liberty incomplet, LibertyMulti absent, Versions et Support ok");
        verifier(e1.resume().startsWith("PARTIEL (") && e1.resume().endsWith(" / ~5,8 Go)"), "résumé « " + e1.resume() + " »");

        // 2. complet
        Factice f2 = fold();
        for (EtatInstall.Attendu a : EtatInstall.ATTENDUS) f2.t.put(a.chemin, a.taille);
        EtatInstall.Etat e2 = EtatInstall.evaluer(f2);
        montrer("complet", e2);
        verifier(e2.global.equals(EtatInstall.COMPLET) && e2.minimum && e2.campagne && e2.resume().equals("COMPLET") && e2.manqueOptionnel.isEmpty(),
                 "tout présent (enUS compris) : COMPLET, tout autorisé, aucun optionnel manquant");

        // 2b. passe 10d : tous les requis, AUCUN enUS → toujours COMPLET (optionnels hors verdict), enUS signalés
        Factice f2b = new Factice();
        for (EtatInstall.Attendu a : EtatInstall.ATTENDUS) if (a.niveau != EtatInstall.OPTIONNEL) f2b.t.put(a.chemin, a.taille);
        EtatInstall.Etat e2b = EtatInstall.evaluer(f2b);
        montrer("requis sans enUS", e2b);
        verifier(e2b.global.equals(EtatInstall.COMPLET) && e2b.campagne && e2b.manqueOptionnel.size() == 8
                 && "absent".equals(e2b.composants().get(EtatInstall.C_ENUS)) && e2b.attendus == e2.attendus,
                 "requis sans enUS : COMPLET, campagne oui, 8 optionnels signalés, composant enUS absent, totaux inchangés");
        Factice f2c = fold(); f2c.t.put("Mods/Core.SC2Mod/enUS.SC2Data", 885052L);
        EtatInstall.Etat e2c = EtatInstall.evaluer(f2c);
        verifier(e2c.manqueOptionnel.size() == 7 && "incomplet".equals(e2c.composants().get(EtatInstall.C_ENUS))
                 && e2c.presents == e1.presents, "1 enUS sur 8 : composant incomplet, octets présents (requis) inchangés");

        // 3. rien (install absente ou accès non accordé)
        Factice f3 = new Factice(); f3.support = false;
        EtatInstall.Etat e3 = EtatInstall.evaluer(f3);
        montrer("vide", e3);
        verifier(e3.global.equals(EtatInstall.ABSENT) && !e3.minimum && !e3.campagne && e3.presents == 0, "rien : ABSENT, lancement refusé");

        // 4. push en cours : SC2.exe et Core là, Liberty base.SC2Assets à 1,0 Go sur 2,2, le reste pas encore
        Factice f4 = new Factice();
        f4.t.put("Versions/Base15405/SC2.exe", 16449536L);
        f4.t.put("Mods/Core.SC2Mod/Base.SC2Data", 8238904L); f4.t.put("Mods/Core.SC2Mod/base.SC2Assets", 29653983L);
        f4.t.put("Mods/Core.SC2Mod/Index.SC2Locale", 611L);
        f4.t.put("Mods/Liberty.SC2Mod/base.SC2Assets", 1073741824L);
        EtatInstall.Etat e4 = EtatInstall.evaluer(f4);
        montrer("push en cours", e4);
        verifier(e4.global.equals(EtatInstall.PARTIEL) && !e4.minimum, "push en cours : PARTIEL, minimum NON atteint (lancement grisé)");
        verifier(EtatInstall.motif(e4.manqueMinimum).startsWith("Versions/Shaders14513/baselineCache.bin : absent (+ 1 autre(s))")
                 && e4.manqueMinimum.get(1).startsWith("Mods/Liberty.SC2Mod/base.SC2Assets : incomplet (1,0 Go / 2,2 Go)"),
                 "motif : « " + EtatInstall.motif(e4.manqueMinimum) + " » ; fichier en cours vu « incomplet (1,0 Go / 2,2 Go) »");
        verifier(e4.presents == 16449536L + 8238904L + 29653983L + 611L + 1073741824L, "octets présents = somme (fichier partiel compté à sa taille réelle)");

        // 5. tout présent sauf le dossier Support/
        Factice f5 = fold(); for (EtatInstall.Attendu a : EtatInstall.ATTENDUS) f5.t.put(a.chemin, a.taille); f5.support = false;
        EtatInstall.Etat e5 = EtatInstall.evaluer(f5);
        verifier(e5.global.equals(EtatInstall.PARTIEL) && !e5.minimum && e5.manqueMinimum.get(0).equals("Support/ : absent"), "Support/ absent : PARTIEL, minimum non atteint");

        // 6. SC2.exe d'une autre version (plus grand) : présent, signalé, ne bloque pas
        Factice f6 = fold(); f6.t.put("Versions/Base15405/SC2.exe", 16458904L);   // taille du manifeste DVD
        EtatInstall.Etat e6 = EtatInstall.evaluer(f6);
        verifier(e6.minimum && e6.lignes.get(0).etat().startsWith("taille différente (16458904 o, attendu 16449536)"),
                 "SC2.exe plus grand : « " + e6.lignes.get(0).etat() + " », minimum atteint");

        // 7. seul SC2.exe manque
        Factice f7 = fold(); f7.t.remove("Versions/Base15405/SC2.exe");
        EtatInstall.Etat e7 = EtatInstall.evaluer(f7);
        verifier(!e7.minimum && EtatInstall.motif(e7.manqueMinimum).equals("Versions/Base15405/SC2.exe : absent"), "SC2.exe absent : minimum non atteint, motif exact");

        // 8. formatage
        verifier(EtatInstall.Etat.go(0).equals("0,0 Go") && EtatInstall.Etat.go(2684354560L).equals("2,5 Go"), "Go : 0 → 0,0 Go ; 2,5 Gio → 2,5 Go");

        // 9. vraie install (lecture seule) : les constantes « F » collent au déploiement mesuré. Passe 10g : DEUX états admis,
        //    nommés — partielle (déploiement Fold, état d'avant 10d) ou complète (install de référence depuis 10d, tous les
        //    requis exacts). Les deux états construits sur disque sont verrouillés sans argument dans CatalogueTest §9.
        if (args.length > 0) {
            EtatInstall.Etat r = EtatInstall.evaluer(EtatInstall.fichiers(new File(args[0])));
            montrer("install réelle " + args[0], r);
            boolean fOk = true, requisOk = true;
            for (EtatInstall.Ligne l : r.lignes) {
                if (l.a.source.equals("F") && !l.ok()) fOk = false;
                if (l.a.niveau != EtatInstall.OPTIONNEL && !l.ok()) requisOk = false;
            }
            boolean partielle = fOk && r.minimum && !r.campagne && r.global.equals(EtatInstall.PARTIEL);
            boolean complete = requisOk && r.minimum && r.campagne && r.global.equals(EtatInstall.COMPLET);
            verifier(partielle || complete, "install réelle reconnue : " + (complete ? "install complète (tous les requis à la taille exacte)"
                     : partielle ? "install partielle (= déploiement Fold : tailles « F » exactes, minimum atteint, campagne incomplète)"
                     : "NI partielle NI complète (" + r.resume() + ", tailles F exactes " + fOk + ")"));
        }
        System.out.println("== " + oks + " OK, " + echecs + " échec(s)");
        System.exit(echecs == 0 ? 0 : 1);
    }
}
