/* test_win32_compat.c — tests du socle Win32 de recomp-sc2 (passe 2).
 *
 * Rejoue, en C natif, ce que le boot de SC2.exe exige (CARTE-INIT §0, §1.2,
 * §1.4, §3.2) :
 *   1. 0x008092D0 : ConvertThreadToFiber ; CreateFiberEx(0, 0x800000, 0, …) ;
 *      SwitchToFiber — puis retour par SwitchToFiber(fibre principale) ;
 *   2. 00809FB0 / 00809B90 : shm « StarCraft II IPC Mem » + décision du
 *      contrôle launcher (modèle C de §1.2, table de vérité complète) ;
 *   3. 00802610 : instance unique par « Global\StarCraft II Game Application ».
 * Chaque assertion imprime PASS/FAIL ; code de sortie = nombre d'échecs.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "stubs_sc2.h"
#include "win32_compat.h"

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                   \
        int ok_ = (cond) ? 1 : 0;                               \
        printf("%s  ", ok_ ? "PASS" : "FAIL");                  \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        if (ok_) g_pass++; else g_fail++;                       \
    } while (0)

static int below_4g(const void *p) { return (uintptr_t)p < 0x100000000ull; }

/* ========================================================================== */
/* Modèle C de sc2_init_LauncherHandshakeIPC 0x00809B90 (CARTE-INIT §1.2)     */
/* ========================================================================== */

#define SHM32(shm, off) (((volatile uint32_t *)(shm))[(off) / 4])

/* Rend 1 (continuer) ou 0 (rejet → « Must run from the launcher. »). */
static int model_00809B90(void *shm, BOOL (*is_debugger_present)(void), int *aaac_out)
{
    uint32_t mode = 1;                                   /* étape 1 : LaunchParams[+0x18] = 1 */
    int aaac;
    if (!shm) return 1;                                  /* étape 2 : MapIpcMem a échoué */
    uint32_t prev = __sync_val_compare_and_swap(&SHM32(shm, SC2_IPC_OFF_STATE),
                                                SC2_IPC_STATE_LAUNCHER_READY, 1u);  /* étape 3 */
    if (prev == SC2_IPC_STATE_LAUNCHER_READY) {          /* étape 4 : launcher présent */
        aaac = SHM32(shm, 0x4C) != 0xFFFFFFFFu;          /* LaunchParams[+0x1C] != -1 */
        mode = SHM32(shm, SC2_IPC_OFF_PARAMS + 0x18);    /* copie de 0xB5C octets (champ mode) */
        SHM32(shm, SC2_IPC_OFF_WAIT) = 1;
        SHM32(shm, SC2_IPC_OFF_STATE) = 5;
        /* OpenProcess(PID launcher inexistant) = 0 → la boucle d'attente sort aussitôt */
        SHM32(shm, SC2_IPC_OFF_STATE) = 0;
    } else {
        aaac = is_debugger_present() == 0;               /* étape 5 : 009B3550 */
    }
    if (aaac_out) *aaac_out = aaac;
    return !(aaac && mode != 3 && mode != 4);            /* étape 6 */
}

static BOOL fake_no_debugger(void) { return FALSE; }

/* ========================================================================== */
/* Fibres                                                                     */
/* ========================================================================== */

typedef struct {
    LPVOID main_fiber;
    LPVOID self;
    int    step;
    int    saw_self, saw_data, stack_in_bounds, deep_ok;
    void  *stack_base;
    SIZE_T stack_size;
    int    ipc_ok;
} fiber_ctx;

/* Touche ~6 Mo de pile, au-delà du 1 Mo par défaut d'une fibre Windows et
 * proche des 8 Mo demandés par 008092D0. */
static int __attribute__((noinline)) deep_stack(int depth)
{
    volatile char frame[64 * 1024];
    memset((char *)frame, depth & 0xFF, sizeof frame);
    int r = depth > 0 ? deep_stack(depth - 1) : 0;
    return r + (frame[sizeof frame - 1] == (char)(depth & 0xFF));
}

static void sc2_ipc_and_instance_in_fiber(fiber_ctx *c);

static void fiber_main_00809030(LPVOID param)
{
    fiber_ctx *c = (fiber_ctx *)param;
    char probe;
    c->step = 1;
    c->saw_self = GetCurrentFiber() == c->self;
    c->saw_data = GetFiberData() == param;
    c->stack_in_bounds = (char *)&probe >= (char *)c->stack_base &&
                         (char *)&probe <  (char *)c->stack_base + c->stack_size;
    c->deep_ok = deep_stack(96) == 97;                   /* 97 cadres × 64 Ko ≈ 6,1 Mo */
    SwitchToFiber(c->main_fiber);

    c->step = 2;                                          /* reprise après le 2e Switch */
    sc2_ipc_and_instance_in_fiber(c);
    SwitchToFiber(c->main_fiber);

    c->step = 3;
    SwitchToFiber(c->main_fiber);                         /* comme 00809030 : ne retourne jamais */
}

/* Le jeu fait ces appels DEPUIS la fibre (GameMain → 00802610, 008080F0). */
static void sc2_ipc_and_instance_in_fiber(fiber_ctx *c)
{
    HANDLE hm = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, SC2_IPC_MEM_PROTECT,
                                   0, SC2_IPC_MEM_SIZE, SC2_IPC_MEM_NAME);
    void *shm = hm ? MapViewOfFile(hm, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    int aaac = -1;
    c->ipc_ok = shm && model_00809B90(shm, IsDebuggerPresent, &aaac) == 1 && aaac == 0;
    if (shm) UnmapViewOfFile(shm);
    if (hm) CloseHandle(hm);
}

static void test_fibers(void)
{
    printf("\n== Fibres (008092D0 → CreateFiberEx(0x800000, 00809030)) ==\n");
    CHECK(GetCurrentFiber() == NULL, "GetCurrentFiber() == NULL avant conversion");

    static int tag;
    LPVOID mainf = ConvertThreadToFiber(&tag);
    CHECK(mainf != NULL, "ConvertThreadToFiber rend une fibre (%p)", mainf);
    CHECK(GetCurrentFiber() == mainf && GetFiberData() == &tag,
          "GetCurrentFiber/GetFiberData sur la fibre principale");
    SetLastError(0);
    CHECK(ConvertThreadToFiber(NULL) == NULL && GetLastError() == ERROR_ALREADY_FIBER,
          "2e ConvertThreadToFiber : NULL + ERROR_ALREADY_FIBER (1280)");

    fiber_ctx c = { .main_fiber = mainf };
    LPVOID f = CreateFiberEx(0, 0x800000, 0, fiber_main_00809030, &c);
    c.self = f;
    CHECK(f != NULL, "CreateFiberEx(0, 0x800000, 0, start, &ctx) rend une fibre");
    CHECK(c.step == 0, "fibre non démarrée avant le premier SwitchToFiber");
    CHECK(w32_fiber_stack(f, &c.stack_base, &c.stack_size) && c.stack_size == 0x800000,
          "pile de la fibre = 0x800000 octets (8 Mo)");
    CHECK(below_4g(c.stack_base), "pile de la fibre sous 4 Go (%p) : adressable en VA invitée",
          c.stack_base);
    CHECK(!w32_fiber_stack(mainf, NULL, NULL), "w32_fiber_stack(fibre convertie) = FALSE");

    SwitchToFiber(f);
    CHECK(c.step == 1, "SwitchToFiber → la routine de fibre a tourné, retour à la principale");
    CHECK(GetCurrentFiber() == mainf, "de retour : GetCurrentFiber() == fibre principale");
    CHECK(c.saw_self && c.saw_data, "dans la fibre : GetCurrentFiber()==fibre, GetFiberData()==param");
    CHECK(c.stack_in_bounds, "dans la fibre : la pile native est bien celle de 8 Mo");
    CHECK(c.deep_ok, "dans la fibre : ~6 Mo de pile utilisés sans faute");

    SwitchToFiber(f);
    CHECK(c.step == 2, "2e SwitchToFiber : reprise de la fibre là où elle s'était arrêtée");
    CHECK(c.ipc_ok, "dans la fibre : shm IPC + contrôle launcher (00809B90) passent");

    SwitchToFiber(mainf);
    CHECK(c.step == 2, "SwitchToFiber(soi-même) : no-op");

    SwitchToFiber(f);
    CHECK(c.step == 3, "3e aller-retour fibre ↔ principale");

    DeleteFiber(f);
    CHECK(ConvertFiberToThread() == TRUE && GetCurrentFiber() == NULL,
          "DeleteFiber puis ConvertFiberToThread : thread redevenu ordinaire");
    SetLastError(0);
    CHECK(ConvertFiberToThread() == FALSE && GetLastError() == ERROR_ALREADY_THREAD,
          "2e ConvertFiberToThread : FALSE + ERROR_ALREADY_THREAD (1281)");
}

/* Un 2e thread a sa propre fibre courante (TLS, ≈ TEB->FiberData). */
static int g_thr_ok;
typedef struct { int value; LPVOID back; } thr_ctx;
static void thr_fiber(LPVOID p) { thr_ctx *t = (thr_ctx *)p; t->value = 42; SwitchToFiber(t->back); }

static void *thread_body(void *arg)
{
    (void)arg;
    thr_ctx tc = { 0, NULL };
    int was_null = GetCurrentFiber() == NULL;
    tc.back = ConvertThreadToFiber(NULL);
    LPVOID f = CreateFiber(0, thr_fiber, &tc);
    SIZE_T sz = 0;
    w32_fiber_stack(f, NULL, &sz);
    SwitchToFiber(f);
    g_thr_ok = was_null && tc.value == 42 && sz == (1u << 20);
    DeleteFiber(f);
    ConvertFiberToThread();
    return NULL;
}

static void test_fiber_thread(void)
{
    printf("\n== Fibres sur un 2e thread ==\n");
    pthread_t t;
    pthread_create(&t, NULL, thread_body, NULL);
    pthread_join(t, NULL);
    CHECK(g_thr_ok, "thread neuf : pas de fibre héritée ; CreateFiber(0) = pile 1 Mo ; aller-retour OK");
}

/* ========================================================================== */
/* IsDebuggerPresent + shm IPC (CARTE-INIT §1.2, §1.5 prio 1-2)               */
/* ========================================================================== */

static void test_ipc(void)
{
    printf("\n== IsDebuggerPresent / shm « StarCraft II IPC Mem » (00809FB0, 00809B90) ==\n");
    CHECK(IsDebuggerPresent() == TRUE, "IsDebuggerPresent() == TRUE");

    SetLastError(0xDEAD);
    HANDLE hm = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, SC2_IPC_MEM_PROTECT,
                                   0, SC2_IPC_MEM_SIZE, SC2_IPC_MEM_NAME);
    CHECK(hm != NULL && GetLastError() == ERROR_SUCCESS,
          "CreateFileMappingA(INVALID_HANDLE, 0x08000004, 0x10000, \"%s\") réussit", SC2_IPC_MEM_NAME);
    CHECK(((uintptr_t)hm & 3) == 0 && (uintptr_t)hm < 0x10000,
          "HANDLE petit et aligné sur 4 (%p) : tient dans un registre 32 bits", hm);

    uint8_t *shm = (uint8_t *)MapViewOfFile(hm, FILE_MAP_WRITE, 0, 0, 0);
    CHECK(shm != NULL && below_4g(shm), "MapViewOfFile(FILE_MAP_WRITE) : vue sous 4 Go (%p)", (void *)shm);
    int zero = 1;
    for (unsigned i = 0; shm && i < SC2_IPC_MEM_SIZE; i++) if (shm[i]) { zero = 0; break; }
    CHECK(shm && zero, "les 64 Ko de la shm sont à zéro (+0x20 == 0, ≠ 4)");

    /* --- Table de vérité de 00809B90 ---------------------------------- */
    int aaac = -1;
    CHECK(model_00809B90(shm, IsDebuggerPresent, &aaac) == 1 && aaac == 0,
          "shm à zéro + IsDebuggerPresent()=TRUE  → AAAC=0 → continue (choix retenu)");
    CHECK(SHM32(shm, SC2_IPC_OFF_STATE) == 0, "la CAS(+0x20, 1, 4) n'a rien écrit (prev=0)");
    CHECK(model_00809B90(shm, fake_no_debugger, &aaac) == 0 && aaac == 1,
          "shm à zéro + IsDebuggerPresent()=FALSE → AAAC=1, mode 1 → REJET (« Must run from the launcher. »)");
    CHECK(model_00809B90(NULL, fake_no_debugger, &aaac) == 1,
          "MapIpcMem en échec → rend 1 d'emblée (§1.5 prio 2 : « un échec convient aussi »)");

    /* Émulation fidèle du launcher (§1.5, dernier paragraphe). */
    SHM32(shm, SC2_IPC_OFF_STATE) = 4;
    SHM32(shm, 0x4C) = 0xFFFFFFFFu;                                   /* LaunchParams[+0x1C] = -1 */
    SHM32(shm, SC2_IPC_OFF_PARAMS + 0x18) = 1;                        /* mode 1 */
    CHECK(model_00809B90(shm, fake_no_debugger, &aaac) == 1 && aaac == 0
          && SHM32(shm, SC2_IPC_OFF_STATE) == 0 && SHM32(shm, SC2_IPC_OFF_WAIT) == 1,
          "launcher émulé (+0x20=4, +0x4C=-1) sans débogueur → continue ; +0x20 4→1→5→0");
    SHM32(shm, SC2_IPC_OFF_STATE) = 4;
    SHM32(shm, 0x4C) = 0;
    CHECK(model_00809B90(shm, fake_no_debugger, &aaac) == 0 && aaac == 1,
          "launcher émulé, +0x4C≠-1, mode 1 → REJET");
    SHM32(shm, SC2_IPC_OFF_STATE) = 4;
    SHM32(shm, SC2_IPC_OFF_PARAMS + 0x18) = 3;
    CHECK(model_00809B90(shm, fake_no_debugger, &aaac) == 1 && aaac == 1,
          "launcher émulé, +0x4C≠-1, mode 3 → continue (mode ∈ {3,4})");
    memset(shm, 0, SC2_IPC_MEM_SIZE);

    /* --- Partage entre handles/vues ------------------------------------ */
    __sync_fetch_and_add(&SHM32(shm, SC2_IPC_OFF_INSTANCES), 1);      /* InterlockedIncrement(+0) */
    SHM32(shm, SC2_IPC_OFF_GAME_PID) = 0x1234;
    HANDLE ho = OpenFileMappingA(FILE_MAP_WRITE, FALSE, SC2_IPC_MEM_NAME);
    uint8_t *v2 = ho ? (uint8_t *)MapViewOfFile(ho, FILE_MAP_READ, 0, 0, 0) : NULL;
    CHECK(v2 && SHM32(v2, 0) == 1 && SHM32(v2, SC2_IPC_OFF_GAME_PID) == 0x1234,
          "OpenFileMappingA + 2e vue : voit l'écriture (+0x00=1, +0x0C=pid)");

    static const WCHAR wname[] = u"StarCraft II IPC Mem";
    SetLastError(0);
    HANDLE hw = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, SC2_IPC_MEM_PROTECT, 0, 0x100, wname);
    CHECK(hw != NULL && GetLastError() == ERROR_ALREADY_EXISTS,
          "CreateFileMappingW même nom : section existante + ERROR_ALREADY_EXISTS");
    uint8_t *v3 = hw ? (uint8_t *)MapViewOfFile(hw, FILE_MAP_READ, 0, 0x20, 0) : NULL;
    CHECK(v3 == shm + 0x20, "MapViewOfFile avec offset 0x20 → base + 0x20");

    CloseHandle(hm); CloseHandle(ho); CloseHandle(hw);
    CHECK(SHM32(shm, SC2_IPC_OFF_GAME_PID) == 0x1234,
          "tous les handles fermés : la section survit tant qu'une vue est mappée");
    HANDLE again = OpenFileMappingA(FILE_MAP_READ, FALSE, SC2_IPC_MEM_NAME);
    CHECK(again != NULL, "… et reste ouvrable par son nom tant qu'elle vit");
    CloseHandle(again);
    UnmapViewOfFile(shm); UnmapViewOfFile(v2); UnmapViewOfFile(v3);
    SetLastError(0);
    CHECK(OpenFileMappingA(FILE_MAP_READ, FALSE, SC2_IPC_MEM_NAME) == NULL
          && GetLastError() == ERROR_FILE_NOT_FOUND,
          "dernière vue démappée : le nom disparaît (OpenFileMapping → ERROR_FILE_NOT_FOUND)");
    CHECK(UnmapViewOfFile(shm) == FALSE, "UnmapViewOfFile d'une vue déjà démappée → FALSE");
}

/* ========================================================================== */
/* Événements Global\ (00802610, 0099CA20 CreateEventW / 0099CB90 OpenEventW)  */
/* ========================================================================== */

static HANDLE g_auto_ev;
static void *signal_later(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, 50 * 1000000L };
    nanosleep(&ts, NULL);
    SetEvent(g_auto_ev);
    return NULL;
}

static void test_events(void)
{
    printf("\n== Événements Global\\ (instance unique, 00802610) ==\n");
    static const WCHAR game[]   = u"Global\\StarCraft II Game Application";
    static const WCHAR repair[] = u"Global\\StarCraft II Repair";

    SetLastError(0xDEAD);
    HANDLE h1 = CreateEventW(NULL, TRUE, FALSE, game);
    CHECK(h1 != NULL && GetLastError() == ERROR_SUCCESS,
          "1re instance : CreateEventW(\"%s\") crée, LastError = 0", SC2_EVENT_GAME_APP);
    HANDLE h2 = CreateEventW(NULL, TRUE, FALSE, game);
    CHECK(h2 != NULL && h2 != h1 && GetLastError() == ERROR_ALREADY_EXISTS,
          "2e instance : même nom → ERROR_ALREADY_EXISTS (SC2 ferait TerminateProcess)");
    SetLastError(0);
    CHECK(OpenEventW(SYNCHRONIZE, FALSE, repair) == NULL && GetLastError() == ERROR_FILE_NOT_FOUND,
          "OpenEventW(\"…Repair\") : absent → NULL + ERROR_FILE_NOT_FOUND (pas de réparation en cours)");
    HANDLE h3 = OpenEventA(EVENT_ALL_ACCESS, FALSE, SC2_EVENT_GAME_APP);
    CHECK(h3 != NULL, "OpenEventA du même nom en ANSI → même objet");

    CHECK(WaitForSingleObject(h1, 0) == WAIT_TIMEOUT, "événement manuel non signalé : Wait(0) = WAIT_TIMEOUT");
    CHECK(SetEvent(h3) && WaitForSingleObject(h1, 0) == WAIT_OBJECT_0
          && WaitForSingleObject(h2, 0) == WAIT_OBJECT_0,
          "SetEvent par un handle → signalé pour tous (manuel : reste signalé)");
    CHECK(ResetEvent(h2) && WaitForSingleObject(h1, 0) == WAIT_TIMEOUT, "ResetEvent → non signalé");

    SetLastError(0);
    CHECK(CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 16, SC2_EVENT_GAME_APP) == NULL
          && GetLastError() == ERROR_INVALID_HANDLE,
          "espace de noms commun : mapping sur un nom d'événement → ERROR_INVALID_HANDLE");

    CloseHandle(h1); CloseHandle(h2); CloseHandle(h3);
    SetLastError(0xDEAD);
    HANDLE h4 = CreateEventW(NULL, TRUE, FALSE, game);
    CHECK(h4 != NULL && GetLastError() == ERROR_SUCCESS,
          "tous les handles fermés : le nom est libéré, une nouvelle instance peut démarrer");
    CloseHandle(h4);

    HANDLE l1 = CreateEventA(NULL, FALSE, FALSE, "Local\\SC2Test");
    HANDLE l2 = CreateEventA(NULL, FALSE, FALSE, "SC2Test");
    CHECK(l1 && l2 && GetLastError() == ERROR_ALREADY_EXISTS, "« Local\\X » ≡ « X »");
    CloseHandle(l1); CloseHandle(l2);

    g_auto_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(WaitForSingleObject(g_auto_ev, 30) == WAIT_TIMEOUT, "auto-reset anonyme : Wait(30 ms) expire");
    pthread_t t;
    pthread_create(&t, NULL, signal_later, NULL);
    CHECK(WaitForSingleObject(g_auto_ev, 2000) == WAIT_OBJECT_0,
          "auto-reset : SetEvent depuis un autre thread réveille Wait(2000)");
    pthread_join(t, NULL);
    CHECK(WaitForSingleObject(g_auto_ev, 0) == WAIT_TIMEOUT, "auto-reset : le signal a été consommé");
    CHECK(CloseHandle(g_auto_ev) && !CloseHandle(g_auto_ev) && GetLastError() == ERROR_INVALID_HANDLE,
          "double CloseHandle → FALSE + ERROR_INVALID_HANDLE");
    CHECK(WaitForSingleObject(g_auto_ev, 0) == WAIT_FAILED, "Wait sur handle fermé → WAIT_FAILED");
}

int main(void)
{
    printf("test_win32_compat — socle win32hle SC2 (passe 2)\n");
    test_fibers();
    test_fiber_thread();
    test_ipc();
    test_events();
    printf("\nRÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
