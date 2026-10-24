package fr.sc2partout;

import android.app.Activity;
import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.StatFs;
import android.provider.Settings;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.RelativeSizeSpan;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.util.List;
import java.util.Map;

/**
 * Lanceur de SC2 Partout (V5) : écran titre plein écran — Continuer (campagne au bar, progression lue
 * des banques), Charger une partie (sauvegardes .SC2Save, gardée tant que {@link Catalogue#CHARGEMENT_PRET}
 * est false), Escarmouche, Missions, Options (options.env combiné à CHAQUE lancement par
 * {@link Options#composer} ; préréglage graphique de Variables.txt), Installation, Quitter. Chaque écran
 * secondaire vit dans la même activité : retour par « ‹ Retour » ou la touche retour.
 * Hors ligne, aucune permission propre : lecture/écriture dans /sdcard/SC2Partout avec l'accès « tous les
 * fichiers » du jeu (MANAGE_EXTERNAL_STORAGE). Retour du jeu : le lanceur réapparaît et relit tout
 * (onResume). Garde des lancements inchangée : minimum requis, campagne complète ; appui long = mode
 * « je fais confiance » (caché).
 */
public class LanceurActivity extends Activity {
    static final String TAG = "SC2Lanceur";
    static final File RACINE = new File("/sdcard/SC2Partout");
    /** Documents du jeu (SC2_DOCUMENTS) : Banks/ (progression) et Saves/ (sauvegardes). */
    static final File DOCUMENTS = new File(RACINE, "StarCraft II");

    static final int ECRAN_TITRE = 0, ECRAN_CHARGER = 1, ECRAN_ESCARMOUCHE = 2,
                     ECRAN_MISSIONS = 3, ECRAN_OPTIONS = 4, ECRAN_INSTALLATION = 5;

    private LinearLayout boite;
    private TextView etat;
    private int ecran = ECRAN_TITRE;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(0xFF0B0E14);                 /* fond sombre de l'écran titre (V5) */
        boite = new LinearLayout(this);
        boite.setOrientation(LinearLayout.VERTICAL);
        int p = dp(16);
        boite.setPadding(p, p, p, p);
        sv.addView(boite);
        setContentView(sv);
    }

    @Override
    protected void onResume() {
        super.onResume();
        Immersif.appliquer(this);
        construire();                       /* redessine l'écran ouvert : progression, install, options */
    }

    @Override
    public void onBackPressed() {           /* retour d'un écran secondaire → écran titre ; sinon défaut */
        if (ecran != ECRAN_TITRE) { ecran = ECRAN_TITRE; construire(); } else super.onBackPressed();
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private TextView texte(String s, float sp, boolean gras) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        if (gras) t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(6), 0, dp(6));
        boite.addView(t);
        return t;
    }

    /* V5 : liste déroulante (libellé au-dessus), élément choisi d'avance. */
    private Spinner choix(String libelle, List<String> valeurs, int choisi) {
        texte(libelle, 14, false);
        Spinner sp = new Spinner(this);
        ArrayAdapter<String> a = new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, valeurs);
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(a);
        if (choisi >= 0 && choisi < valeurs.size()) sp.setSelection(choisi);
        boite.addView(sp, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        return sp;
    }

    private static List<String> libelles(String[][] t) {
        List<String> l = new java.util.ArrayList<String>();
        for (String[] e : t) l.add(e[1]);
        return l;
    }

    /* V5 : largeur des boutons de l'écran titre : le plus étroit entre l'écran moins 32 dp et 520 dp. */
    private int largeurMenu() {
        return Math.min(getResources().getDisplayMetrics().widthPixels - dp(32), dp(520));
    }

    private void bouton(String s, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        boite.addView(b, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
    }

    /* Bouton de l'écran titre : ≥ 64 dp, texte 22 sp, 12 dp d'écart ; sousLigne et motif en petit.
     * accent : bleu #2E7DFF, texte blanc. motif == null : bouton normal ; sinon grisé, motif affiché, un
     * clic redit le motif, un appui long force (mode « je fais confiance » caché, journalisé). */
    private void boutonMenu(String s, String sousLigne, boolean accent, String motif, final Runnable action) {
        Button b = new Button(this);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        b.setMinimumHeight(dp(64));
        String sous = sousLigne != null ? sousLigne : "";
        String garde = motif != null ? "(indisponible : " + motif + ")" : "";
        String t = s + (sous.length() > 0 ? "\n" + sous : "") + (garde.length() > 0 ? "\n" + garde : "");
        SpannableString sp = new SpannableString(t);
        if (sous.length() > 0)
            sp.setSpan(new RelativeSizeSpan(0.64f), s.length() + 1, s.length() + 1 + sous.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        if (garde.length() > 0)
            sp.setSpan(new RelativeSizeSpan(0.6f), t.length() - garde.length(), t.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        b.setText(sp, TextView.BufferType.SPANNABLE);
        if (accent) { b.setBackgroundColor(0xFF2E7DFF); b.setTextColor(Color.WHITE); }
        if (motif == null) {
            b.setOnClickListener(new View.OnClickListener() { @Override public void onClick(View v) { action.run(); } });
        } else {
            b.setAlpha(0.45f);
            b.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) { signaler("Indisponible : " + motif, null); }
            });
            b.setOnLongClickListener(new View.OnLongClickListener() {
                @Override public boolean onLongClick(View v) {
                    Log.w(TAG, "mode « je fais confiance » : lancement forcé malgré « " + motif + " »");
                    Toast.makeText(LanceurActivity.this, "Lancement forcé malgré : " + motif, Toast.LENGTH_LONG).show();
                    action.run();
                    return true;
                }
            });
        }
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(largeurMenu(), LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(12);
        lp.gravity = Gravity.CENTER_HORIZONTAL;
        boite.addView(b, lp);
    }

    /* Passe 10c : bouton gardé (écrans secondaires). motif == null : bouton normal. Sinon grisé, motif
     * affiché ; un clic redit le motif ; un appui long force le lancement (journalisé). */
    private void boutonGarde(String s, final String motif, final Runnable action) {
        Button b = new Button(this);
        b.setAllCaps(false);
        if (motif == null) {
            b.setText(s);
            b.setOnClickListener(new View.OnClickListener() { @Override public void onClick(View v) { action.run(); } });
        } else {
            b.setText(s + "\n(indisponible : " + motif + ")");
            b.setAlpha(0.45f);
            b.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) { signaler("Indisponible : " + motif, null); }
            });
            b.setOnLongClickListener(new View.OnLongClickListener() {
                @Override public boolean onLongClick(View v) {
                    Log.w(TAG, "mode « je fais confiance » : lancement forcé malgré « " + motif + " »");
                    Toast.makeText(LanceurActivity.this, "Lancement forcé malgré : " + motif, Toast.LENGTH_LONG).show();
                    action.run();
                    return true;
                }
            });
        }
        boite.addView(b, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
    }

    /* V5 : en-tête des écrans secondaires : retour vers l'écran titre + titre. */
    private void entete(String titre) {
        bouton("‹ Retour", new View.OnClickListener() {
            @Override public void onClick(View v) { ecran = ECRAN_TITRE; construire(); }
        });
        TextView t = texte(titre, 20, true);
        t.setGravity(Gravity.CENTER_HORIZONTAL);
    }

    /* V4.10 : section « Escarmouche contre l'IA ». contenuEnv : sc2.env actuel (présélection), peut être null. */
    private void sectionEscarmouche(final File install, String contenuEnv, String motifCarte) {
        texte("Escarmouche contre l'IA", 18, true);
        texte("Une carte mêlée du jeu (cache Battle.net de l'installation), vous contre une IA ; la carte est préparée par le "
              + "runtime au lancement.", 12, false);
        final List<Catalogue.Melee> cartes = Catalogue.cartesMelee();
        List<String> noms = new java.util.ArrayList<String>();
        for (Catalogue.Melee m : cartes) noms.add(m.libelle());
        final Spinner carte = choix("Carte", noms,
            Catalogue.indiceMelee(Catalogue.valeurEnv(contenuEnv, "SC2_ESCARMOUCHE"), Catalogue.indiceMelee(Catalogue.MELEE_DEFAUT, 0)));
        final Spinner race = choix("Votre race", libelles(Catalogue.RACES),
            Catalogue.indice(Catalogue.RACES, Catalogue.valeurEnv(contenuEnv, "SC2_ESCARMOUCHE_RACE"), 0));
        final Spinner raceIA = choix("Race de l'IA", libelles(Catalogue.RACES),
            Catalogue.indice(Catalogue.RACES, Catalogue.valeurEnv(contenuEnv, "SC2_ESCARMOUCHE_RACE_IA"), 3));
        final Spinner diff = choix("Difficulté de l'IA", libelles(Catalogue.DIFFICULTES),
            Catalogue.indice(Catalogue.DIFFICULTES, Catalogue.valeurEnv(contenuEnv, "SC2_ESCARMOUCHE_IA"), 1));
        String motif = motifCarte != null ? motifCarte
                     : Catalogue.escarmoucheDisponible(install) ? null : Catalogue.ARCHIVE_MELEE + " absent (cartes mêlée)";
        boutonGarde("Lancer l'escarmouche", motif, new Runnable() {
            @Override public void run() {
                if (!Catalogue.escarmoucheDisponible(install)) {
                    signaler("Cartes mêlée introuvables : " + new File(install, Catalogue.ARCHIVE_MELEE), null);
                    return;
                }
                Catalogue.Melee m = cartes.get(carte.getSelectedItemPosition());
                String r = Catalogue.RACES[race.getSelectedItemPosition()][0];
                String ri = Catalogue.RACES[raceIA.getSelectedItemPosition()][0];
                String d = Catalogue.DIFFICULTES[diff.getSelectedItemPosition()][0];
                lancer(Catalogue.contenuEscarmouche(m.nom, r, ri, d), "escarmouche " + m.nom + " : " + r + " contre IA " + ri + " " + d);
            }
        });
    }

    /* Passe 10c : état de l'installation (EtatInstall) — composants, tailles réelles / attendues, état global,
     * espace libre ; rend l'état pour garder les boutons de lancement. */
    private EtatInstall.Etat afficherEtat(File install) {
        EtatInstall.Etat e = EtatInstall.evaluer(EtatInstall.fichiers(install));
        texte("État de l'installation", 18, true);
        TextView g = texte(e.resume(), 16, true);
        g.setTextColor(e.global.equals(EtatInstall.COMPLET) ? Color.rgb(0x6c, 0xd0, 0x6c)
                       : e.global.equals(EtatInstall.PARTIEL) ? Color.rgb(0xf0, 0xb0, 0x40) : Color.rgb(0xf0, 0x60, 0x60));
        StringBuilder sb = new StringBuilder();
        Map<String, String> comp = e.composants();
        for (Map.Entry<String, String> c : comp.entrySet()) {
            long reel = 0, att = 0;
            for (EtatInstall.Ligne l : e.lignes)
                if (l.a.composant.equals(c.getKey())) { reel += Math.max(0, l.reel); att += l.a.taille; }
            String sym = "ok".equals(c.getValue()) ? "✓" : "absent".equals(c.getValue()) ? "✗" : "…";
            sb.append(sym).append(' ').append(c.getKey()).append(" — ").append(c.getValue());
            if (att > 0) sb.append(" (").append(EtatInstall.Etat.go(reel)).append(" / ").append(EtatInstall.Etat.go(att)).append(')');
            sb.append('\n');
        }
        try {
            StatFs fs = new StatFs((RACINE.isDirectory() ? RACINE : Environment.getExternalStorageDirectory()).getPath());
            sb.append("Espace libre : ").append(EtatInstall.Etat.go(fs.getAvailableBytes()));
        } catch (RuntimeException ex) {
            sb.append("Espace libre : inconnu");
        }
        texte(sb.toString(), 13, false);
        if (!e.minimum) texte("Minimum pour démarrer (Versions + Mods + Support, ~2,5 Go) incomplet : " + EtatInstall.motif(e.manqueMinimum)
                              + ". Un transfert en cours donne un écran noir qui imite un bug du moteur : attendre la fin.", 12, false);
        else if (!e.campagne) texte("Campagne incomplète : " + EtatInstall.motif(e.manqueCampagne) + ".", 12, false);
        if (!e.manqueOptionnel.isEmpty())                     /* passe 10d : enUS hors verdict, mais à dire */
            texte("Locale enUS incomplète (optionnelle) : " + EtatInstall.motif(e.manqueOptionnel)
                  + " — le jeu tourne, mais sans les libellés du menu natif ni les raccourcis (Échap / F10).", 12, false);
        bouton("Rafraîchir l'état", new View.OnClickListener() { @Override public void onClick(View v) { construire(); } });
        Log.i(TAG, "état de l'installation : " + e.resume() + " ; minimum " + (e.minimum ? "atteint" : "NON atteint " + e.manqueMinimum)
                   + " ; campagne " + (e.campagne ? "complète" : "incomplète " + e.manqueCampagne));
        return e;
    }

    private void construire() {
        boite.removeAllViews();
        etat = texte("", 12, false);
        boite.setGravity(ecran == ECRAN_TITRE ? Gravity.CENTER_HORIZONTAL : Gravity.NO_GRAVITY);
        if (ecran == ECRAN_CHARGER) ecranCharger();
        else if (ecran == ECRAN_ESCARMOUCHE) ecranEscarmouche();
        else if (ecran == ECRAN_MISSIONS) ecranMissions();
        else if (ecran == ECRAN_OPTIONS) ecranOptions();
        else if (ecran == ECRAN_INSTALLATION) ecranInstallation();
        else ecranTitre();
    }

    /* Motifs de garde (passe 10c) : minimum requis pour lancer, campagne complète pour les cartes. */
    private String motifMinimum(EtatInstall.Etat ei) {
        return ei.minimum ? null : "minimum incomplet — " + EtatInstall.motif(ei.manqueMinimum);
    }

    private String motifCartes(EtatInstall.Etat ei, String motifMin) {
        return motifMin != null ? motifMin : ei.campagne ? null : "campagne incomplète — " + EtatInstall.motif(ei.manqueCampagne);
    }

    /* V5 : écran titre — titre, bandeau d'avertissement, boutons Continuer … Quitter. */
    private void ecranTitre() {
        TextView titre = texte("SC2 Partout", 34, true);
        titre.setGravity(Gravity.CENTER_HORIZONTAL);
        TextView sous = texte("Wings of Liberty — hors ligne", 14, false);
        sous.setGravity(Gravity.CENTER_HORIZONTAL);
        sous.setTextColor(Color.rgb(0x9a, 0xa4, 0xb0));

        /* Android 11+ : sans l'accès « tous les fichiers » (MANAGE_EXTERNAL_STORAGE, déjà déclaré pour le jeu), ni le
         * lanceur ni le jeu ne lisent /sdcard/SC2Partout. Bandeau orange → écran Installation. */
        File install = new File(RACINE, "install");
        boolean accesFichiers = Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
        EtatInstall.Etat ei = EtatInstall.evaluer(EtatInstall.fichiers(install));
        final String motifMin = motifMinimum(ei);
        if (!accesFichiers || motifMin != null) {
            TextView bandeau = texte("⚠ " + (!accesFichiers ? "accès aux fichiers manquant" : "installation incomplète")
                                     + " — toucher pour ouvrir Installation", 13, false);
            bandeau.setGravity(Gravity.CENTER_HORIZONTAL);
            bandeau.setTextColor(Color.rgb(0xf0, 0xb0, 0x40));
            bandeau.setPadding(dp(8), dp(8), dp(8), dp(8));
            bandeau.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) { ecran = ECRAN_INSTALLATION; construire(); }
            });
        }
        final String motifCarte = motifCartes(ei, motifMin);

        /* 21/10 : SC2_RUN_SANS_BNET=1 seul ouvre la carte d'histoire TStory01 (lanceur par défaut du runtime) :
         * la campagne au bar, à la progression enregistrée (banques .SC2Bank). */
        boutonMenu("Continuer", Progression.resume(new File(DOCUMENTS, "Banks"), System.currentTimeMillis()), true, motifMin,
            new Runnable() { @Override public void run() { lancer(Catalogue.contenuMenu(), "campagne au bar"); } });
        boutonMenu("Charger une partie", null, false,
            Catalogue.CHARGEMENT_PRET ? null : "chargement des sauvegardes pas encore pris en charge",
            new Runnable() { @Override public void run() { ecran = ECRAN_CHARGER; construire(); } });
        boutonMenu("Escarmouche", null, false, motifCarte,
            new Runnable() { @Override public void run() { ecran = ECRAN_ESCARMOUCHE; construire(); } });
        boutonMenu("Missions", null, false, motifCarte,
            new Runnable() { @Override public void run() { ecran = ECRAN_MISSIONS; construire(); } });
        boutonMenu("Options", null, false, null,
            new Runnable() { @Override public void run() { ecran = ECRAN_OPTIONS; construire(); } });
        boutonMenu("Installation", null, false, null,
            new Runnable() { @Override public void run() { ecran = ECRAN_INSTALLATION; construire(); } });
        boutonMenu("Quitter", null, false, null, new Runnable() {
            @Override public void run() { finishAndRemoveTask(); }
        });
    }

    /* V5 : charger une partie — les sauvegardes .SC2Save des documents du jeu, la plus récente d'abord. */
    private void ecranCharger() {
        entete("Charger une partie");
        if (!Catalogue.CHARGEMENT_PRET)
            texte("Le chargement d'une sauvegarde n'est pas encore pris en charge par le jeu (lot à venir) : "
                  + "le lancement ci-dessous est expérimental.", 12, false);
        File saves = new File(DOCUMENTS, "Saves");
        List<Progression.Sauvegarde> ss = new java.util.ArrayList<Progression.Sauvegarde>();
        int ratees = 0;
        for (Progression.Sauvegarde s : Progression.sauvegardes(saves, 80))
            if (Progression.utilisable(s)) { if (ss.size() < 50) ss.add(s); } else ratees++;
        if (ss.isEmpty()) texte("Aucune sauvegarde .SC2Save dans " + saves + ".", 13, false);
        if (ratees > 0)                                    /* L3 : archives vides d'avant le correctif du runtime */
            texte(ratees + " sauvegarde(s) vide(s) d'avant le 24/10 masquée(s) (le jeu ne peut pas les charger).", 11, false);
        for (final Progression.Sauvegarde s : ss) {
            String dossier = s.dossier();
            String petit = dossier.length() > 0 && !dossier.equals("Unsaved") ? dossier : "";
            String t = s.libelle() + (petit.length() > 0 ? "\n" + petit : "");
            Button b = new Button(this);
            b.setAllCaps(false);
            b.setText(t, TextView.BufferType.SPANNABLE);
            if (petit.length() > 0) {
                SpannableString sp = new SpannableString(t);
                sp.setSpan(new RelativeSizeSpan(0.7f), s.libelle().length() + 1, t.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                b.setText(sp, TextView.BufferType.SPANNABLE);
            }
            b.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) { lancer(Catalogue.contenuSauvegarde("Saves/" + s.relatif), "sauvegarde " + s.relatif); }
            });
            boite.addView(b, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        }
    }

    private void ecranEscarmouche() {
        entete("Escarmouche");
        File install = new File(RACINE, "install");
        String contenu = lireSc2Env();
        EtatInstall.Etat ei = EtatInstall.evaluer(EtatInstall.fichiers(install));
        sectionEscarmouche(install, contenu, motifCartes(ei, motifMinimum(ei)));
    }

    private void ecranMissions() {
        entete("Missions");
        File install = new File(RACINE, "install");
        EtatInstall.Etat ei = EtatInstall.evaluer(EtatInstall.fichiers(install));
        final String motifCarte = motifCartes(ei, motifMinimum(ei));
        Catalogue.Liste l = Catalogue.lister(install);
        texte("Cartes de la campagne (" + l.source + ")", 18, true);
        if (l.message != null) texte(l.message, 13, false);
        for (final Catalogue.Carte c : l.cartes)
            boutonGarde(c.nom, motifCarte, new Runnable() {
                @Override public void run() { jouer(install, c); }
            });
        Log.i(TAG, "lanceur : " + l.cartes.size() + " carte(s), source " + l.source + (l.message != null ? " ; " + l.message : ""));
    }

    /* V5 : options — options.env (combiné à chaque lancement) et préréglage graphique de Variables.txt. */
    private void ecranOptions() {
        entete("Options");
        final File fichierOptions = new File(RACINE, "options.env");
        String contenu = lireFichier(fichierOptions);
        final Spinner resolution = choix("Résolution", libelles(Options.RESOLUTIONS), Options.indiceResolution(contenu));
        List<String> ombres = new java.util.ArrayList<String>();
        for (String o : Options.OMBRES) ombres.add(o);
        final Spinner ombresSp = choix("Ombres", ombres, Options.ombresCoupees(contenu) ? 1 : 0);
        List<String> particules = new java.util.ArrayList<String>();
        for (String p : Options.PARTICULES) particules.add(p);
        final Spinner particulesSp = choix("Particules (instanciation)", particules, Options.particulesCoupees(contenu) ? 1 : 0);
        bouton("Enregistrer", new View.OnClickListener() {
            @Override public void onClick(View v) {
                try {
                    Catalogue.ecrire(fichierOptions, Options.contenu(
                        Options.RESOLUTIONS[resolution.getSelectedItemPosition()][0],
                        ombresSp.getSelectedItemPosition() == 1,
                        particulesSp.getSelectedItemPosition() == 1));
                    Toast.makeText(LanceurActivity.this, "Options enregistrées", Toast.LENGTH_SHORT).show();
                } catch (IOException | SecurityException e) {
                    signaler("Écriture de " + fichierOptions + " impossible : " + e.getMessage(), e);
                }
            }
        });

        texte("Préréglage graphique (Variables.txt)", 18, true);
        String variables = lireFichier(new File(RACINE, "Variables.txt"));
        String actuel = Options.prereglageActuel(variables);
        texte("Préréglage actuel : " + (variables == null ? "aucun (Variables.txt absent)"
              : actuel != null ? libelles(Options.PRESETS).get(Options.indicePreset(actuel, 0)) : "inconnu (Variables.txt modifié)"), 13, false);
        final Spinner preset = choix("Préréglage", libelles(Options.PRESETS), 1);
        bouton("Appliquer le préréglage", new View.OnClickListener() {
            @Override public void onClick(View v) {
                int i = preset.getSelectedItemPosition();
                try {
                    File secours = Options.appliquerPrereglage(RACINE, Options.PRESETS[i][0], System.currentTimeMillis());
                    Toast.makeText(LanceurActivity.this, "Préréglage " + Options.PRESETS[i][1] + " appliqué"
                        + (secours != null ? " — ancien Variables.txt : " + secours.getName() : ""), Toast.LENGTH_LONG).show();
                    construire();
                } catch (IOException | SecurityException e) {
                    signaler("Préréglage non appliqué : " + e.getMessage(), e);
                }
            }
        });
    }

    /* V5 : installation — état, autorisation d'accès aux fichiers, reprise, avertissements. */
    private void ecranInstallation() {
        entete("Installation");
        final File env = new File(RACINE, "sc2.env");
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            texte("L'accès « tous les fichiers » n'est pas accordé : le jeu ne peut pas lire /sdcard/SC2Partout.", 13, false);
            bouton("Autoriser l'accès aux fichiers", new View.OnClickListener() {
                @Override public void onClick(View v) { autoriser(); }
            });
        }
        EtatInstall.Etat ei = afficherEtat(new File(RACINE, "install"));
        String motifMin = motifMinimum(ei);
        String contenu = lireSc2Env();
        if (contenu != null) {
            boutonGarde("Reprendre (sc2.env actuel)", motifMin, new Runnable() {
                @Override public void run() { demarrerJeu("reprise de " + env); }   /* fichier tel quel */
            });
            texte(contenu.trim(), 11, false).setTypeface(Typeface.MONOSPACE);
        }
        File ext = getExternalFilesDir(null);              /* lu APRÈS /sdcard/SC2Partout/sc2.env par le jeu : il l'emporte */
        if (ext != null && new File(ext, "sc2.env").isFile())
            texte("Attention : " + new File(ext, "sc2.env") + " existe et l'emporte sur les réglages posés ici.", 12, false);
    }

    /** sc2.env actuel (présélection, reprise), ou null ; toute erreur de lecture est signalée. */
    private String lireSc2Env() {
        try {
            return Catalogue.lireEnv(new File(RACINE, "sc2.env"));
        } catch (IOException | SecurityException e) {
            signaler("Lecture de " + new File(RACINE, "sc2.env") + " impossible : " + e.getMessage(), e);
            return null;
        }
    }

    /** Contenu d'un petit fichier texte (options.env, Variables.txt), ou null s'il n'existe pas. */
    private String lireFichier(File f) {
        try {
            return Catalogue.lireEnv(f);
        } catch (IOException | SecurityException e) {
            signaler("Lecture de " + f + " impossible : " + e.getMessage(), e);
            return null;
        }
    }

    private void autoriser() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        } catch (RuntimeException e) {                                /* écran absent sur certaines surcouches */
            try { startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)); }
            catch (RuntimeException e2) { signaler("Écran d'autorisation introuvable : " + e2, e2); }
        }
    }

    private void jouer(File install, Catalogue.Carte c) {
        if (!Catalogue.disponible(install, c)) {                    /* disparue entre le listing et le clic */
            signaler("Carte introuvable : " + c.chemin + " — liste relue.", null);
            construire();
            return;
        }
        lancer(Catalogue.contenuCarte(c.chemin), c.chemin);
    }

    /* V5 : pose sc2.env = options (options.env) puis contenu du lancement (les lignes du lancement l'emportent),
     * puis démarre le jeu. La « reprise » passe par demarrerJeu : sc2.env n'est pas retouché. */
    private void lancer(String contenu, String quoi) {
        File env = new File(RACINE, "sc2.env");
        String options = null;
        try {
            options = Catalogue.lireEnv(new File(RACINE, "options.env"));
        } catch (IOException | SecurityException e) {
            signaler("Options ignorées : lecture de " + new File(RACINE, "options.env") + " impossible : " + e.getMessage(), e);
        }
        try {
            Catalogue.ecrire(env, Options.composer(options, contenu));
        } catch (IOException | SecurityException e) {
            signaler("Écriture de " + env + " impossible (accès « tous les fichiers » accordé ?) : " + e.getMessage(), e);
            return;
        }
        demarrerJeu(quoi);
    }

    private void demarrerJeu(String quoi) {
        Log.i(TAG, "lancement du jeu : " + quoi);
        try {
            startActivity(new Intent(this, NativeActivity.class));
        } catch (RuntimeException e) {
            signaler("Lancement du jeu impossible : " + e, e);
        }
    }

    private void signaler(String msg, Throwable e) {
        if (e != null) Log.e(TAG, msg, e); else Log.w(TAG, msg);
        if (etat != null) etat.setText(msg);
        Toast.makeText(this, msg, Toast.LENGTH_LONG).show();
    }
}
