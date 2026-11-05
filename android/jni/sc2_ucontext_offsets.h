/* Décalages dans ucontext_t (bionic, AArch64) — vérifiés à la compilation par sc2_ucontext.c.
 *   uc_link 8 · uc_mcontext 176 : regs[31] 184, sp 432, pc 440, __reserved 464
 * Les registres flottants callee-saved d8..d15 sont rangés au début de __reserved (zone
 * opaque : seules nos trois fonctions la lisent ; aucun contexte de signal n'y transite). */
#define UC_LINK   8
#define UC_REGS   184
#define UC_SP     432
#define UC_PC     440
#define UC_FPR    464
