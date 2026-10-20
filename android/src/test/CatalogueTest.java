package fr.sc2partout;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;

/**
 * Test hors Android de la logique du lanceur (passe 10b) : javac seul, arborescence factice.
 *
 *   javac -encoding UTF-8 -d <classes> android/src/fr/sc2partout/Catalogue.java android/src/fr/sc2partout/EtatInstall.java \
 *         android/src/test/CatalogueTest.java          (EtatInstall : vérif 9, passe 10g ; ou bash android/test-javac.sh)
 *   java -cp <classes> fr.sc2partout.CatalogueTest android/src/test/reference <dossier de travail vide>
 *
 * Références (android/src/test/reference/) écrites à la main (printf), au format de android/DONNEES-INSTALL.md §4 :
 * sc2.env produit comparé OCTET À OCTET. Sortie : une ligne OK/ÉCHEC par vérification, code 1 au moindre échec.
 */
public final class CatalogueTest {
    static int echecs, oks;

    static void verifier(boolean ok, String quoi) {
        if (ok) oks++; else echecs++;
        System.out.println((ok ? "OK     " : "ÉCHEC  ") + quoi);
    }

    static void fichier(File f, String contenu) throws IOException {
        f.getParentFile().mkdirs();
        FileOutputStream o = new FileOutputStream(f);
        o.write(contenu.getBytes("UTF-8"));
        o.close();
    }

    /** Fichier creux de la taille exacte attendue (setLength : rien n'est écrit, quelques Go apparents sans place disque). */
    static void creux(File f, long taille) throws IOException {
        f.getParentFile().mkdirs();
        RandomAccessFile r = new RandomAccessFile(f, "rw");
        r.setLength(taille);
        r.close();
    }

    static boolean memesOctets(File a, File b) throws IOException {
        return Arrays.equals(Files.readAllBytes(a.toPath()), Files.readAllBytes(b.toPath()));
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage : java -cp <classes> fr.sc2partout.CatalogueTest <références> <dossier de travail vide>\n"
                + "  <références>          android/src/test/reference (sc2.env.* attendus)\n"
                + "  <dossier de travail>  dossier neuf, rempli par le test\n"
                + "exemple : java -cp /tmp/c fr.sc2partout.CatalogueTest android/src/test/reference \"$(mktemp -d)\"\n"
                + "(ou, tout compris : bash android/test-javac.sh)");
            System.exit(2);
        }
        File ref = new File(args[0]), w = new File(args[1]);

        // 1. listing exact : filtre .SC2Map (casse indifférente, fichier OU dossier extrait), ordre, noms courts, chemins
        File i1 = new File(w, "install1"), d1 = new File(i1, "Maps/Campaign");
        fichier(new File(d1, "TRaynor01.SC2Map"), "x");
        new File(d1, "THanson01.SC2Map").mkdirs();
        fichier(new File(d1, "lisez-moi.txt"), "x");
        fichier(new File(d1, "b.sc2map"), "x");
        fichier(new File(d1, "A.SC2Map"), "x");
        fichier(new File(d1, ".SC2Map"), "x");
        Catalogue.Liste l1 = Catalogue.lister(i1);
        System.out.println("  listing 1 : " + Catalogue.noms(l1.cartes) + " ; source " + l1.source + " ; message " + l1.message);
        verifier(Catalogue.noms(l1.cartes).equals(Arrays.asList("A", "b", "THanson01", "TRaynor01")), "listing exact (4 cartes, ordre, noms courts)");
        verifier(l1.cartes.get(3).chemin.equals("Maps/Campaign/TRaynor01.SC2Map") && !l1.cartes.get(3).dansArchive, "chemin complet relatif à l'install");
        verifier(l1.source.equals("dossier Maps/Campaign") && l1.message == null, "source dossier, pas de message");

        // 2. borne : 250 cartes → 200 affichées, message
        File i2 = new File(w, "install2"), d2 = new File(i2, "Maps/Campaign");
        for (int k = 0; k < 250; k++) fichier(new File(d2, String.format("M%03d.SC2Map", k)), "");
        Catalogue.Liste l2 = Catalogue.lister(i2);
        System.out.println("  listing 2 : " + l2.cartes.size() + " cartes, " + l2.cartes.get(0).nom + " … " + l2.cartes.get(l2.cartes.size() - 1).nom + " ; " + l2.message);
        verifier(l2.cartes.size() == 200 && l2.cartes.get(199).nom.equals("M199"), "borne 200 (M000 … M199)");
        verifier("200 premières cartes affichées sur 250".equals(l2.message), "message de borne");

        // 3. install normale : pas de Maps/Campaign, archive Base.SC2Maps présente → catalogue des 33 cartes
        File i3 = new File(w, "install3");
        fichier(new File(i3, Catalogue.ARCHIVE_CARTES), "");
        Catalogue.Liste l3 = Catalogue.lister(i3);
        System.out.println("  listing 3 : " + l3.cartes.size() + " cartes (" + l3.source + "), " + l3.cartes.get(0).chemin + " … " + l3.cartes.get(32).chemin);
        verifier(l3.cartes.size() == 33 && l3.cartes.get(10).chemin.equals("Maps/Campaign/TRaynor01.SC2Map") && l3.cartes.get(10).dansArchive,
                 "archive : 33 cartes, TRaynor01 → Maps/Campaign/TRaynor01.SC2Map");

        // 4. robustesse : install absente (et /sdcard absent du devbox) → liste vide + message, pas d'exception
        Catalogue.Liste l4 = Catalogue.lister(new File(w, "absente"));
        verifier(l4.cartes.isEmpty() && l4.message != null, "install absente : liste vide + message (« " + l4.message + " »)");
        Catalogue.Liste l5 = Catalogue.lister(new File("/sdcard/SC2Partout/install"));
        verifier(l5.cartes.isEmpty() && l5.message != null, "/sdcard/SC2Partout/install absent ici : liste vide + message");
        File i6 = new File(w, "install6"), d6 = new File(i6, "Maps/Campaign");
        d6.mkdirs(); d6.setReadable(false); d6.setExecutable(false);
        Catalogue.Liste l6 = Catalogue.lister(i6);
        d6.setReadable(true); d6.setExecutable(true);
        verifier(l6.cartes.isEmpty() && l6.message != null, "dossier illisible : liste vide + message (« " + l6.message + " »)");

        // 5. sc2.env écrit au format attendu, comparé octet à octet aux références ; remplace un contenu plus long
        File env = new File(w, "SC2Partout/sc2.env");
        fichier(env, "SC2_RUN_SANS_BNET=1\nSC2_ARGS=-run Maps/Campaign/TZeratul04.SC2Map\nSC2_SWAPCHAIN=1\nSC2_ECRAN=auto\n");
        Catalogue.ecrire(env, Catalogue.contenuCarte(l3.cartes.get(10).chemin));
        verifier(memesOctets(env, new File(ref, "sc2.env.TRaynor01")), "sc2.env carte TRaynor01 = référence octet à octet");
        Catalogue.ecrire(env, Catalogue.contenuMenu());
        verifier(memesOctets(env, new File(ref, "sc2.env.menu")), "sc2.env menu principal = référence (SC2_RUN_SANS_BNET=1 seul, SC2_ARGS effacé)");
        Catalogue.ecrire(env, Catalogue.contenuCarte("Maps/Campaign/Ma Carte.SC2Map"));
        verifier(memesOctets(env, new File(ref, "sc2.env.espace")), "chemin avec espace : entre guillemets = référence");
        verifier(!new File(env.getParentFile(), "sc2.env.tmp").exists(), "pas de sc2.env.tmp résiduel");
        File neuf = new File(w, "neuf/SC2Partout/sc2.env");
        Catalogue.ecrire(neuf, Catalogue.contenuMenu());
        verifier(memesOctets(neuf, new File(ref, "sc2.env.menu")), "dossier SC2Partout absent : créé, sc2.env écrit");

        // 6. reprise : contenu existant relu tel quel ; absent → null ; borne 64 Kio
        String existant = "SC2_RUN_SANS_BNET=1\nSC2_ARGS=-run Maps/Campaign/THorner05S.SC2Map\n# réglage à garder\n";
        fichier(env, existant);
        verifier(existant.equals(Catalogue.lireEnv(env)), "reprise : contenu relu à l'identique");
        verifier(Catalogue.lireEnv(new File(w, "pas-de-sc2.env")) == null, "reprise : sc2.env absent → pas de bouton (null)");
        StringBuilder gros = new StringBuilder();
        while (gros.length() < 100000) gros.append("SC2_X=0123456789\n");
        fichier(new File(w, "gros.env"), gros.toString());
        verifier(Catalogue.lireEnv(new File(w, "gros.env")).length() == Catalogue.MAX_ENV, "reprise : lecture bornée à 64 Kio");

        // 7. écriture impossible → IOException (le lanceur l'affiche et la journalise)
        File bloque = new File(w, "bloque"); fichier(bloque, "un fichier, pas un dossier");
        try { Catalogue.ecrire(new File(bloque, "sc2.env"), Catalogue.contenuMenu()); verifier(false, "écriture sous un fichier refusée"); }
        catch (IOException e) { verifier(true, "écriture impossible (parent = fichier) → IOException « " + e.getMessage() + " »"); }
        File ro = new File(w, "lecture-seule"); ro.mkdirs(); ro.setWritable(false);
        try { Catalogue.ecrire(new File(ro, "sc2.env"), Catalogue.contenuMenu()); verifier(false, "écriture dans un dossier en lecture seule refusée"); }
        catch (IOException e) { verifier(true, "écriture impossible (dossier en lecture seule) → IOException « " + e.getMessage() + " »"); }
        ro.setWritable(true);

        // 8. carte disparue entre le listing et le clic
        Catalogue.Carte c = l1.cartes.get(3);
        verifier(Catalogue.disponible(i1, c), "carte présente au clic → disponible");
        new File(d1, "TRaynor01.SC2Map").delete();
        verifier(!Catalogue.disponible(i1, c), "carte supprimée après le listing → indisponible (message, liste relue)");
        new File(i3, Catalogue.ARCHIVE_CARTES).delete();
        verifier(!Catalogue.disponible(i3, l3.cartes.get(10)), "archive Base.SC2Maps supprimée → indisponible");

        List<String> a = Catalogue.cartesArchive();
        verifier(a.size() == 33 && a.get(0).equals("TArcade") && a.get(32).equals("TZeratul04"), "catalogue : 33 cartes TArcade … TZeratul04");

        // 9. passe 10g — les DEUX états d'install, construits sur disque aux tailles exactes d'EtatInstall.ATTENDUS (fichiers
        //    creux) et lus par la vraie sonde EtatInstall.fichiers : partielle = déploiement Fold (constantes « F » seules, état
        //    d'avant 10d) ; complète = tous les requis + enUS (install de référence depuis 10d). Une ligne par scénario : verdict
        //    d'état (ce que le lanceur autorise) + catalogue (archive, 33 cartes, TRaynor01 disponible) + sc2.env = référence.
        for (int s = 0; s < 2; s++) {
            boolean complete = s == 1;
            String nom = complete ? "install complète" : "install partielle (déploiement Fold)";
            File in = new File(w, complete ? "etat-complete" : "etat-partielle");
            for (EtatInstall.Attendu at : EtatInstall.ATTENDUS) if (complete || at.source.equals("F")) creux(new File(in, at.chemin), at.taille);
            new File(in, EtatInstall.DOSSIER_SUPPORT).mkdirs();
            EtatInstall.Etat e = EtatInstall.evaluer(EtatInstall.fichiers(in));
            Catalogue.Liste l = Catalogue.lister(in);
            boolean catOk = l.cartes.size() == 33 && l.source.equals("archive " + Catalogue.ARCHIVE_CARTES) && l.message == null
                && l.cartes.get(10).chemin.equals("Maps/Campaign/TRaynor01.SC2Map") && Catalogue.disponible(in, l.cartes.get(10));
            File envS = new File(in, "SC2Partout/sc2.env");
            if (catOk) Catalogue.ecrire(envS, Catalogue.contenuCarte(l.cartes.get(10).chemin));
            boolean etatOk = complete
                ? e.global.equals(EtatInstall.COMPLET) && e.minimum && e.campagne && e.manqueMinimum.isEmpty() && e.manqueCampagne.isEmpty()
                  && e.manqueOptionnel.isEmpty()
                : e.global.equals(EtatInstall.PARTIEL) && e.minimum && !e.campagne && e.manqueCampagne.size() == 2
                  && e.manqueCampagne.get(0).startsWith("Mods/Liberty.SC2Mod/Base.SC2Data : absent")
                  && e.manqueCampagne.get(1).startsWith("Mods/LibertyMulti.SC2Mod/Base.SC2Data : absent");
            boolean envOk = catOk && memesOctets(envS, new File(ref, "sc2.env.TRaynor01"));
            System.out.println("  " + nom + " : " + e.resume() + " ; minimum " + e.minimum + " ; campagne " + e.campagne
                + " ; " + l.cartes.size() + " cartes (" + l.source + ") ; état " + etatOk + ", catalogue " + catOk + ", sc2.env " + envOk);
            verifier(etatOk && catOk && envOk, nom + (complete
                ? " : COMPLET, menu et cartes autorisés, 33 cartes de l'archive, sc2.env TRaynor01 = référence"
                : " : PARTIEL, menu autorisé, cartes grisées (Liberty + LibertyMulti Base.SC2Data absents), 33 cartes, sc2.env = référence"));
        }

        System.out.println("== " + oks + " OK, " + echecs + " échec(s)");
        System.exit(echecs == 0 ? 0 : 1);
    }
}
