# passe 9w — relift GARDÉ des fonctions qui portent un TODO d’instruction donné (lancer avec PCRECOMP_FPU_CW=1, comme lift-boot).
# relift-garde.py <lift-boot.py> <tools/lift> <exe> <dossier lifted> <regex mnémoniques> [--ecrire]
# Pour chaque fonction de la closure qui contient un RECOMP_TODO(…, "<mnémonique>…") : relift sur l'étendue D'ORIGINE
# (première instruction → fin de la dernière), garde : même nombre de lignes et toute ligne changée était un TODO de
# ces mnémoniques, sinon REFUS. --ecrire remplace le corps dans le fichier.
import importlib.util, re, sys, glob, os
lb_path, lift_dir, exe, dossier, mnem = sys.argv[1:6]; ecrire = "--ecrire" in sys.argv
spec = importlib.util.spec_from_file_location("liftboot", lb_path); lb = importlib.util.module_from_spec(spec); spec.loader.exec_module(lb)
sys.path.insert(0, lift_dir)
import lift32_cpu as L
pe = lb.PE(exe); L.IMAGE_BASE = pe.image_base
lifter = L.Lifter(None, pe.size_of_image, pe.read_va); lifter.reloc_vas = pe.relocs()
todo = re.compile(r'RECOMP_TODO\(0x[0-9A-F]+, "(%s)\b' % mnem)
TETE = re.compile(r'^void L_([0-9A-F]{8})\(CPU \*c\)$')
total = refus = 0
for f in sorted(glob.glob(os.path.join(dossier, "recomp_*.c"))):
    lignes = open(f).read().split("\n")
    if not any(todo.search(l) for l in lignes): continue
    debuts = [i for i, l in enumerate(lignes) if TETE.match(l)] + [len(lignes)]
    modif = False
    for k in range(len(debuts) - 1):
        a, b = debuts[k], debuts[k + 1]
        corps = lignes[a:b]
        if not any(todo.search(l) for l in corps): continue
        va = int(TETE.match(lignes[a]).group(1), 16)
        adrs = [int(m, 16) for m in re.findall(r'/\* ([0-9A-F]{8}):', "\n".join(corps))]
        der = max(adrs); insn = next(lifter.md.disasm(pe.read_va(der, 16), der)); fin = der + insn.size
        neuf = lifter.lift_function(pe.read_va(va, fin - va), va).split("\n")
        while corps and corps[-1] == "": corps = corps[:-1]
        while neuf and neuf[-1] == "": neuf = neuf[:-1]
        diff = [(x, y) for x, y in zip(corps, neuf) if x != y]
        ok = len(corps) == len(neuf) and all(todo.search(x) and "RECOMP_TODO" not in y for x, y in diff)
        total += 1; refus += not ok
        print(f"{os.path.basename(f)} L_{va:08X} [{va:08X}..{fin:08X}) : {len(diff)} ligne(s) TODO traduite(s) — {'OK' if ok else 'REFUS'}"
              + ("" if ok else f" (lignes {len(corps)} → {len(neuf)})"))
        for x, y in diff: print("    -", x.strip()[:110]); print("    +", y.strip()[:110])
        if ok and ecrire:
            lignes[a:a + len(corps)] = neuf; modif = True
    if modif: open(f, "w").write("\n".join(lignes))
print(f"{total} fonction(s), {refus} refus{' ; fichiers ÉCRITS' if ecrire else ' (essai à blanc)'}")
