package fr.sc2partout;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Test hors Android des options et de la progression du lanceur (V5) : javac seul, arborescence factice.
 *
 *   javac -encoding UTF-8 -d <classes> android/src/fr/sc2partout/{Catalogue,Options,Prereglages,Progression}.java \
 *         android/src/test/OptionsTest.java          (ou bash android/test-javac.sh)
 *   java -cp <classes> fr.sc2partout.OptionsTest <racine du dépôt> <dossier de travail vide>
 *
 * Couvre Options.composer (contenu sc2.env de CHAQUE bouton de lancement), appliquerPrereglage (sauvegarde
 * de l'ancien Variables.txt, application octet à octet), prereglageActuel, l'identité Prereglages ←>
 * scripts/prereglages/*.txt (octet à octet), et Progression (sauvegardes, progression des banques).
 * Sortie : une ligne OK/ÉCHEC par vérification, code 1 au moindre échec.
 */
public final class OptionsTest {
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

    static boolean memesOctets(byte[] a, File b) throws IOException {
        return Arrays.equals(a, Files.readAllBytes(b.toPath()));
    }

    static String heure(long t) {
        Date d = new Date(t);
        return new SimpleDateFormat("dd/MM", Locale.ROOT).format(d) + " à " + new SimpleDateFormat("HH:mm", Locale.ROOT).format(d);
    }

    /** « dd/MM HH:mm », le format des boutons de sauvegardes (sans « à »). */
    static String dateHeure(long t) {
        return new SimpleDateFormat("dd/MM HH:mm", Locale.ROOT).format(new Date(t));
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage : java -cp <classes> fr.sc2partout.OptionsTest <racine du dépôt> <dossier de travail vide>\n"
                + "  <racine du dépôt>     lit scripts/prereglages/{low,medium,high,ultra}.txt (copies de Prereglages)\n"
                + "  <dossier de travail>  dossier neuf, rempli par le test\n"
                + "exemple : java -cp /tmp/c fr.sc2partout.OptionsTest . \"$(mktemp -d)\"\n"
                + "(ou, tout compris : bash android/test-javac.sh)");
            System.exit(2);
        }
        File racine = new File(args[0]), w = new File(args[1]);

        // 1. composer : options PUIS lancement ; clé répétée → la ligne de LANCEMENT l'emporte
        verifier(Options.composer("SC2_ECRAN=1280x720\nSC2_OMBRES=0\n", "SC2_RUN_SANS_BNET=1\nSC2_ECRAN=auto\n")
                 .equals("SC2_OMBRES=0\nSC2_RUN_SANS_BNET=1\nSC2_ECRAN=auto\n"),
                 "composer : clé répétée (SC2_ECRAN) → ligne du lancement l'emporte, option perdue");
        verifier(Options.composer("# réglage à garder\n\nSC2_OMBRES=0\n", "SC2_RUN_SANS_BNET=1\n")
                 .equals("SC2_OMBRES=0\nSC2_RUN_SANS_BNET=1\n"),
                 "composer : commentaires « # » et lignes vides des options perdus");
        verifier(Options.composer("pas-de-cle\nSC2_OMBRES=0", null).equals("SC2_OMBRES=0\n"),
                 "composer : ligne sans « = » perdue, saut de ligne final ajouté");
        verifier(Options.composer(null, "SC2_RUN_SANS_BNET=1\n").equals("SC2_RUN_SANS_BNET=1\n")
                 && Options.composer("", "SC2_RUN_SANS_BNET=1\n").equals("SC2_RUN_SANS_BNET=1\n"),
                 "composer : options absentes ou vides → contenu du lancement seul");
        verifier(Options.composer("SC2_OMBRES=0\n", null).equals("SC2_OMBRES=0\n")
                 && Options.composer(null, null).isEmpty(),
                 "composer : lancement absent → options seules ; tout absent → vide");
        verifier(Options.composer(null, "SC2_RUN_SANS_BNET=1").equals("SC2_RUN_SANS_BNET=1\n"),
                 "composer : lancement sans saut de ligne final → un saut de ligne ajouté");

        // 2. contenu sc2.env de chaque bouton de lancement : options (options.env) puis lignes du lancement
        String opts = Options.contenu("1280x720", true, true);
        verifier(opts.equals("SC2_ECRAN=1280x720\nSC2_OMBRES=0\nSC2_INSTANCES=0\n"),
                 "Options.contenu : résolution + ombres et particules coupées (3 lignes)");
        verifier(Options.contenu("auto", false, false).equals("SC2_ECRAN=auto\n"),
                 "Options.contenu : réglages « selon le jeu » → aucune ligne");
        verifier(Options.composer(opts, Catalogue.contenuMenu())
                 .equals("SC2_ECRAN=1280x720\nSC2_OMBRES=0\nSC2_INSTANCES=0\nSC2_RUN_SANS_BNET=1\n"),
                 "Continuer : sc2.env = options + menu (SC2_RUN_SANS_BNET=1 seul)");
        verifier(Options.composer(opts, Catalogue.contenuCarte("Maps/Campaign/TRaynor01.SC2Map"))
                 .equals("SC2_ECRAN=1280x720\nSC2_OMBRES=0\nSC2_INSTANCES=0\nSC2_RUN_SANS_BNET=1\n"
                         + "SC2_ARGS=-run Maps/Campaign/TRaynor01.SC2Map\n"),
                 "Missions : sc2.env = options + -run carte");
        verifier(Options.composer(opts, Catalogue.contenuEscarmouche("SteppesofWar", "terran", "zerg", "facile"))
                 .equals("SC2_ECRAN=1280x720\nSC2_OMBRES=0\nSC2_INSTANCES=0\nSC2_RUN_SANS_BNET=1\n"
                         + "SC2_ESCARMOUCHE=SteppesofWar\nSC2_ESCARMOUCHE_RACE=terran\nSC2_ESCARMOUCHE_RACE_IA=zerg\n"
                         + "SC2_ESCARMOUCHE_IA=facile\n"),
                 "Escarmouche : sc2.env = options + 5 lignes d'escarmouche");
        verifier(Options.composer(opts, Catalogue.contenuSauvegarde("Saves\\Unsaved\\TRaynor01.SC2Save"))
                 .equals("SC2_ECRAN=1280x720\nSC2_OMBRES=0\nSC2_INSTANCES=0\nSC2_RUN_SANS_BNET=1\n"
                         + "SC2_SAUVEGARDE=Saves/Unsaved/TRaynor01.SC2Save\n"),
                 "Charger : sc2.env = options + sauvegarde (chemin documents, séparateurs « / »)");
        verifier(Options.indiceResolution(opts) == 1 && Options.indiceResolution(null) == 0
                 && Options.ombresCoupees(opts) && Options.particulesCoupees(Options.contenu("auto", false, true))
                 && !Options.particulesCoupees(Options.contenu("auto", false, false))
                 && Options.indicePreset("Ultra", 0) == 3 && Options.indicePreset(null, 1) == 1,
                 "relues : indice de résolution, ombres/particules coupées, indice de préréglage");

        // 3. Prereglages : chaque constante = scripts/prereglages/<nom>.txt, OCTET À OCTET (CRLF compris)
        String[][] paires = { { "low", "LOW" }, { "medium", "MEDIUM" }, { "high", "HIGH" }, { "ultra", "ULTRA" } };
        for (String[] p : paires) {
            byte[] attendu = Files.readAllBytes(new File(racine, "scripts/prereglages/" + p[0] + ".txt").toPath());
            byte[] constantes = contenuPreset(p[0]).getBytes("UTF-8");
            verifier(Arrays.equals(attendu, constantes) && new String(attendu, "UTF-8").indexOf("\r\n") >= 0,
                     "Prereglages." + p[1] + " = scripts/prereglages/" + p[0] + ".txt octet à octet ("
                     + attendu.length + " octets, CRLF)");
        }

        // 4. appliquerPrereglage : sauvegarde de l'ancien Variables.txt puis écriture du préréglage
        File r1 = new File(w, "racine1");
        String ancien = "lastDeviceId=1234567890\r\nGraphicsOptionLightingQuality=1\r\n";
        fichier(new File(r1, "Variables.txt"), ancien);
        long t0 = 1770000000123L;
        File secours = Options.appliquerPrereglage(r1, "high", t0);
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss").format(new Date(t0));
        verifier(secours != null && secours.getName().equals("Variables.txt.sauvegarde-" + stamp)
                 && memesOctets(ancien.getBytes("UTF-8"), secours),
                 "appliquerPrereglage : ancien Variables.txt sauvegardé octet à octet (Variables.txt.sauvegarde-" + stamp + ")");
        verifier(memesOctets(Prereglages.HIGH.getBytes("UTF-8"), new File(r1, "Variables.txt")),
                 "appliquerPrereglage : Variables.txt = préréglage high octet à octet");
        File secours2 = Options.appliquerPrereglage(r1, "high", t0);   /* même seconde → suffixe -2 */
        verifier(secours2 != null && secours2.getName().equals("Variables.txt.sauvegarde-" + stamp + "-2")
                 && memesOctets(Prereglages.HIGH.getBytes("UTF-8"), secours2),
                 "appliquerPrereglage : 2 applications à la même seconde → suffixe -2, première sauvegarde gardée");
        verifier(memesOctets(ancien.getBytes("UTF-8"), secours)
                 && memesOctets(Prereglages.HIGH.getBytes("UTF-8"), new File(r1, "Variables.txt")),
                 "appliquerPrereglage : après la 2e application, la 1re sauvegarde tient toujours l'ancien contenu");
        File r2 = new File(w, "racine2");
        r2.mkdirs();
        File aucun = Options.appliquerPrereglage(r2, "low", t0);
        String[] rests = r2.list();
        verifier(aucun == null && memesOctets(Prereglages.LOW.getBytes("UTF-8"), new File(r2, "Variables.txt"))
                 && rests != null && rests.length == 1 && rests[0].equals("Variables.txt"),
                 "appliquerPrereglage : Variables.txt absent → aucune sauvegarde (null), seul Variables.txt créé");

        // 5. prereglageActuel : seules les lignes GraphicsOption* sont comparées
        verifier(Options.prereglageActuel(Prereglages.MEDIUM).equals("medium")
                 && Options.prereglageActuel(Prereglages.ULTRA).equals("ultra"),
                 "prereglageActuel : Variables.txt d'un préréglage → son nom");
        StringBuilder modifie = new StringBuilder("lastDeviceId=987654321\r\n");
        for (String l : Prereglages.MEDIUM.split("\r\n")) modifie.append(l).append("\r\n");
        verifier(Options.prereglageActuel(modifie.toString()).equals("medium"),
                 "prereglageActuel : lignes du jeu ajoutées (lastDeviceId…) → préréglage reconnu quand même");
        verifier(Options.prereglageActuel(ancien) == null && Options.prereglageActuel(null) == null,
                 "prereglageActuel : contenu inconnu ou absent → null");

        // 6. Progression.sauvegardes : ordre (récent d'abord, puis chemin), profondeur, extensions, borne
        File saves = new File(w, "Saves");
        File f;
        f = new File(saves, "Unsaved/A.SC2Save"); fichier(f, "a"); f.setLastModified(1000000L);
        f = new File(saves, "Unsaved/A2.SC2Save"); fichier(f, "a2"); f.setLastModified(1000000L);
        f = new File(saves, "Unsaved/B.SC2Save"); fichier(f, "b"); f.setLastModified(3000000L);
        f = new File(saves, "Unsaved/E.sc2save"); fichier(f, "e"); f.setLastModified(1500000L);
        f = new File(saves, "Unsaved/notes.txt"); fichier(f, "n"); f.setLastModified(9999999L);
        f = new File(saves, "Campagne/C.SC2Save"); fichier(f, "c"); f.setLastModified(2000000L);
        f = new File(saves, "Campagne/Sous/D.SC2Save"); fichier(f, "d"); f.setLastModified(2500000L);
        f = new File(saves, "Campagne/Sous/Profond/F.SC2Save"); fichier(f, "f"); f.setLastModified(5000000L);
        f = new File(saves, "Campagne/Sous/Profond/Trop/G.SC2Save"); fichier(f, "g"); f.setLastModified(6000000L);
        List<Progression.Sauvegarde> ss = Progression.sauvegardes(saves, 50);
        StringBuilder noms = new StringBuilder();
        for (Progression.Sauvegarde s : ss) noms.append(s.relatif).append(' ');
        verifier(noms.toString().equals("Campagne/Sous/Profond/F.SC2Save Unsaved/B.SC2Save Campagne/Sous/D.SC2Save "
                 + "Campagne/C.SC2Save Unsaved/E.sc2save Unsaved/A.SC2Save Unsaved/A2.SC2Save "),
                 "sauvegardes : récentes d'abord, égalité de date par chemin (« " + noms.toString().trim() + " »)");
        verifier(Progression.sauvegardes(saves, 2).size() == 2
                 && Progression.sauvegardes(saves, 2).get(0).relatif.equals("Campagne/Sous/Profond/F.SC2Save"),
                 "sauvegardes : borne max respectée (les plus récentes d'abord)");
        verifier(Progression.sauvegardes(new File(w, "Saves-absent"), 50).isEmpty(),
                 "sauvegardes : dossier absent → liste vide");
        boolean profondeur = true;
        for (Progression.Sauvegarde s : ss) profondeur &= !s.relatif.contains("Trop");
        verifier(profondeur && ss.size() <= 7 && Progression.sauvegardes(saves, 50).size() >= 6,
                 "sauvegardes : profondeur bornée (rien au-delà de Saves/a/b/c/) ; « g » 5e niveau exclu");
        boolean autres = true;
        for (Progression.Sauvegarde s : ss) autres &= s.relatif.endsWith(".SC2Save") || s.relatif.endsWith(".sc2save");
        verifier(autres && !noms.toString().contains("notes.txt"),
                 "sauvegardes : autres extensions ignorées (.txt) ; .SC2Save sans casse");
        Progression.Sauvegarde premiere = Progression.sauvegardes(saves, 1).get(0);
        verifier(premiere.nom().equals("F") && premiere.dossier().equals("Campagne/Sous/Profond")
                 && premiere.libelle().equals("F — " + dateHeure(5000000L)),
                 "Sauvegarde : nom sans extension, dossier relatif, libellé « F — dd/MM HH:mm »");
        Path lien = new File(saves, "Lien").toPath();
        try {
            Files.createSymbolicLink(lien, new File(saves, "Unsaved").toPath());
            int doubles = 0;
            for (Progression.Sauvegarde s : Progression.sauvegardes(saves, 50))
                if (s.relatif.equals("Unsaved/B.SC2Save")) doubles++;
            verifier(doubles == 1, "sauvegardes : lien symbolique jamais suivi (B une seule fois)");
        } catch (IOException | UnsupportedOperationException e) {
            System.out.println("  (liens symboliques impossibles ici : " + e.getMessage() + ")");
        }
        File verrou = new File(saves, "Verrouille");
        fichier(new File(verrou, "H.SC2Save"), "h"); new File(verrou, "H.SC2Save").setLastModified(8000000L);
        verrou.setReadable(false); verrou.setExecutable(false);
        boolean ignore = true;
        for (Progression.Sauvegarde s : Progression.sauvegardes(saves, 50)) ignore &= !s.relatif.startsWith("Verrouille");
        verrou.setReadable(true); verrou.setExecutable(true);
        verifier(ignore, "sauvegardes : dossier illisible ignoré");

        // 6 bis. L3 : sauvegardes vides (1 181 o, d'avant le correctif du runtime) inutilisables
        File vide = new File(saves, "Unsaved/Vide Victory.SC2Save"); fichier(vide, new String(new char[1181]).replace('\0', 'x'));
        File pleine = new File(saves, "Campaign/Pleine.SC2Save"); fichier(pleine, new String(new char[5000]).replace('\0', 'x'));
        boolean vu = false, okVide = false, okPleine = false;
        for (Progression.Sauvegarde s : Progression.sauvegardes(saves, 50)) {
            if (s.relatif.equals("Unsaved/Vide Victory.SC2Save")) { vu = true; okVide = !Progression.utilisable(s); }
            if (s.relatif.equals("Campaign/Pleine.SC2Save")) okPleine = Progression.utilisable(s);
        }
        verifier(vu && okVide && okPleine && !Progression.utilisable(null) && Catalogue.CHARGEMENT_PRET,
                 "utilisable : 1 181 o → non, 5 000 o → oui ; chargement activé (L3)");

        // 7. Progression.resume : banque la plus récente, ou nouvelle campagne
        verifier(Progression.resume(new File(w, "Banks-absent"), 5000000L).equals("Nouvelle campagne"),
                 "resume : aucune banque (dossier absent) → « Nouvelle campagne »");
        File banks = new File(w, "Banks");
        f = new File(banks, "X.SC2Bank"); fichier(f, "x"); f.setLastModified(2000000000000L);
        f = new File(banks, "Y.SC2Bank"); fichier(f, "y"); f.setLastModified(2000007200000L);
        f = new File(banks, "z.txt"); fichier(f, "z"); f.setLastModified(9000000000000L);
        new File(banks, "Dossier.SC2Bank").mkdirs();
        verifier(Progression.resume(banks, 9000000000000L)
                 .equals("Progression du " + heure(2000007200000L)),
                 "resume : banque la plus récente (« Progression du dd/MM à HH:mm »), .txt et dossiers ignorés");
        long avance = 2001000000000L;
        f = new File(banks, "Y.SC2Bank"); f.setLastModified(avance + 86400000L);
        verifier(Progression.resume(banks, avance).equals("Progression du " + heure(avance)),
                 "resume : horloge du téléphone en retard → horodatage borné à maintenant");

        System.out.println("== " + oks + " OK, " + echecs + " échec(s)");
        System.exit(echecs == 0 ? 0 : 1);
    }

    /** Contenu du préréglage nommé (réflexion inutile : les 4 noms en clair). */
    static String contenuPreset(String nom) {
        if ("low".equals(nom)) return Prereglages.LOW;
        if ("medium".equals(nom)) return Prereglages.MEDIUM;
        if ("high".equals(nom)) return Prereglages.HIGH;
        if ("ultra".equals(nom)) return Prereglages.ULTRA;
        return null;
    }
}
