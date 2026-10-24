package fr.sc2partout;

import android.app.Activity;
import android.os.Build;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

/** 21/10 : plein écran IMMERSIF pour l'activité du jeu (NativeActivity) — barres d'état et de navigation masquées
 *  (elles recouvraient la barre de raccourcis), réaffichées un instant par un glissement depuis le bord. Appelé par
 *  JNI depuis la glue native (sc2_android_main.c, immersif()) : le travail se fait sur le fil de l'interface. */
public final class Immersif {
    private Immersif() {}

    public static void appliquer(final Activity a) {
        a.runOnUiThread(new Runnable() {
            @Override public void run() {
                if (Build.VERSION.SDK_INT >= 30) {
                    WindowInsetsController c = a.getWindow().getInsetsController();
                    if (c != null) {
                        c.hide(WindowInsets.Type.systemBars());
                        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                    }
                } else {
                    a.getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
                }
            }
        });
    }
}
