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
import android.util.Log;
import android.util.TypedValue;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.util.Map;

/**
 * Lanceur de SC2 Partout (passe 10b) : point d'entrée de l'icône. Liste les cartes de la campagne, pose
 * /sdcard/SC2Partout/sc2.env, puis lance le jeu (android.app.NativeActivity, processus « :jeu ») par
 * intention explicite. 100 % hors ligne, aucune permission propre : il lit et écrit dans /sdcard/SC2Partout
 * avec l'accès « tous les fichiers » que le jeu demande déjà (MANAGE_EXTERNAL_STORAGE).
 * Retour du jeu (appui long sur retour, ou fin du runtime) : le lanceur réapparaît et relit tout (onResume).
 * Passe 10c : écran d'état de l'installation (EtatInstall) ; lancement grisé tant que le minimum n'est pas là (les cartes :
 * tant que la campagne n'est pas complète), appui long = mode « je fais confiance » (caché).
 */
public class LanceurActivity extends Activity {
    static final String TAG = "SC2Lanceur";
    static final File RACINE = new File("/sdcard/SC2Partout");

    private LinearLayout boite;
    private TextView etat;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        ScrollView sv = new ScrollView(this);
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
        construire();
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

    private void bouton(String s, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        boite.addView(b, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
    }

    /* Passe 10c : bouton gardé. motif == null : bouton normal. Sinon grisé, motif affiché ; un clic redit le motif ;
     * mode « je fais confiance » CACHÉ : un appui long force le lancement malgré l'avertissement (journalisé). */
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

    /* Passe 10c : écran d'état de l'installation (EtatInstall) — composants, tailles réelles / attendues, état global,
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
        texte("SC2 Partout", 24, true);
        etat = texte("", 14, false);
        final File install = new File(RACINE, "install");
        final File env = new File(RACINE, "sc2.env");

        /* Android 11+ : sans l'accès « tous les fichiers » (MANAGE_EXTERNAL_STORAGE, déjà déclaré pour le jeu), ni le
         * lanceur ni le jeu ne lisent /sdcard/SC2Partout. Bouton vers l'écran système qui l'accorde (avant : adb appops). */
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            texte("L'accès « tous les fichiers » n'est pas accordé : le jeu ne peut pas lire /sdcard/SC2Partout.", 13, false);
            bouton("Autoriser l'accès aux fichiers", new View.OnClickListener() {
                @Override public void onClick(View v) { autoriser(); }
            });
        }
        EtatInstall.Etat ei = afficherEtat(install);
        final String motifMin = ei.minimum ? null : "minimum incomplet — " + EtatInstall.motif(ei.manqueMinimum);
        final String motifCarte = motifMin != null ? motifMin : ei.campagne ? null : "campagne incomplète — " + EtatInstall.motif(ei.manqueCampagne);

        boutonGarde("Menu principal", motifMin, new Runnable() {
            @Override public void run() { lancer(Catalogue.contenuMenu(), "menu principal"); }
        });
        String contenu = null;
        try {
            contenu = Catalogue.lireEnv(env);
        } catch (IOException | SecurityException e) {
            signaler("Lecture de " + env + " impossible : " + e.getMessage(), e);
        }
        if (contenu != null) {
            boutonGarde("Reprendre (sc2.env actuel)", motifMin, new Runnable() {
                @Override public void run() { demarrerJeu("reprise de " + env); }
            });
            texte(contenu.trim(), 11, false).setTypeface(Typeface.MONOSPACE);
        }
        File ext = getExternalFilesDir(null);              /* lu APRÈS /sdcard/SC2Partout/sc2.env par le jeu : il l'emporte */
        if (ext != null && new File(ext, "sc2.env").isFile())
            texte("Attention : " + new File(ext, "sc2.env") + " existe et l'emporte sur les réglages posés ici.", 12, false);

        Catalogue.Liste l = Catalogue.lister(install);
        texte("Cartes de la campagne (" + l.source + ")", 18, true);
        if (l.message != null) texte(l.message, 13, false);
        for (final Catalogue.Carte c : l.cartes)
            boutonGarde(c.nom, motifCarte, new Runnable() {
                @Override public void run() { jouer(install, c); }
            });
        Log.i(TAG, "lanceur : " + l.cartes.size() + " carte(s), source " + l.source + (l.message != null ? " ; " + l.message : ""));
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

    private void lancer(String contenu, String quoi) {
        File env = new File(RACINE, "sc2.env");
        try {
            Catalogue.ecrire(env, contenu);
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
