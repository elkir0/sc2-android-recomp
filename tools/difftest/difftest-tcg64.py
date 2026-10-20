# Passe 10j : lance difftest-sc2.py TEL QUEL, avec un tampon TCG Unicorn de 64 Mo par instance (au lieu de 1 Go) et un gc
# à chaque création : sous ulimit -v 8000000, les Uc en attente de ramasse-miettes épuisaient l'espace virtuel
# (« Could not allocate dynamic translator buffer »). Aucun effet sur la sémantique de l'émulation.
import gc, runpy, sys, unicorn
_Uc = unicorn.Uc
def Uc(*a, **k):
    gc.collect()
    u = _Uc(*a, **k)
    u.ctl_set_tcg_buffer_size(64 << 20)
    return u
unicorn.Uc = Uc
try:
    import unicorn.unicorn_py3 as up3; up3.Uc = Uc
except Exception: pass
script = sys.argv[1]; sys.argv = sys.argv[1:]
runpy.run_path(script, run_name="__main__")
