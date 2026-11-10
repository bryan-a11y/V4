/*
 * tsuhan_pt  -  proxy dinput8.dll para "Haruka Ayumu no Tsuuhan Seikatsu" (tsu_han.exe)
 *
 *  - Salva o progresso (chama a propria rotina de salvar do jogo, que grava tsu_han.dat)
 *      * automaticamente a cada N segundos (padrao 60)
 *      * ao apertar Ctrl+F4 (salva e fecha o jogo) ou Alt+F4 (salva e deixa o Windows fechar)
 *  - Velocidade do jogo: F5 = 1x, F6 = 2x, F7 = 3x, F8 = 4x, F9 = 5x  (Num+ / Num- sobem/descem)
 *
 * Funcionamento: o exe importa DirectInput8Create de dinput8.dll. Esta DLL fica na pasta do jogo,
 * repassa a chamada para o dinput8.dll real do Windows e, ao carregar, troca a entrada
 * timeGetTime da tabela de imports do exe por uma versao que (1) acelera o tempo, (2) le o teclado
 * e (3) dispara o autosave, tudo na thread principal do jogo.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define EXE_PREF_BASE   0x400000u
#define RVA_HOLDER      (0x51dba8u - EXE_PREF_BASE)   /* singleton: [0]=jogo, [4]=objeto de configuracao/dados */
#define RVA_VTBL_SETTING (0x4e5e90u - EXE_PREF_BASE)  /* vtable da classe que contem load/save de tsu_han.dat */
#define VSLOT_SAVE      7                              /* vtable[7] = salvar tsu_han.dat */

static char  g_dir[MAX_PATH];
static char  g_log[MAX_PATH];
static char  g_ini[MAX_PATH];
static int   g_autosave_sec = 60;
static int   g_logging = 1;
static DWORD g_main_tid;

static void logf_(const char *fmt, ...)
{
    FILE *f; va_list ap; SYSTEMTIME t;
    if (!g_logging) return;
    f = fopen(g_log, "ab");
    if (!f) return;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

/* ------------------------------------------------------------------ dinput8 real */
typedef HRESULT (WINAPI *PFN_DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPVOID);
static PFN_DI8Create g_realCreate;

static void install_hook(void);

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID *o, LPVOID u)
{
    if (!g_realCreate) {
        char sys[MAX_PATH]; HMODULE m;
        GetSystemDirectoryA(sys, MAX_PATH);
        strcat(sys, "\\dinput8.dll");
        m = LoadLibraryA(sys);
        if (m) g_realCreate = (PFN_DI8Create)GetProcAddress(m, "DirectInput8Create");
        logf_("dinput8 real: %s -> %p", sys, (void *)g_realCreate);
    }
    install_hook();   /* garante o gancho mesmo se a IAT ainda nao estava resolvida no DllMain */
    if (!g_realCreate) return E_FAIL;
    return g_realCreate(h, v, r, o, u);
}

/* ------------------------------------------------------------------ salvar */
typedef unsigned char (__attribute__((thiscall)) *SaveFn)(void *self);

static int do_save(const char *why)
{
    static volatile LONG busy;
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    void **holder = (void **)(base + RVA_HOLDER);
    void *obj; void **vtbl; unsigned char r;
    if (InterlockedExchange(&busy, 1)) return 0;
    obj = IsBadReadPtr(holder, 8) ? NULL : holder[1];
    if (!obj || IsBadReadPtr(obj, 0x1c)) { InterlockedExchange(&busy, 0); return 0; }
    vtbl = *(void ***)obj;
    if ((BYTE *)vtbl != base + RVA_VTBL_SETTING) {
        logf_("save(%s): vtable inesperada %p (esperado %p) - ignorado", why, (void *)vtbl, (void *)(base + RVA_VTBL_SETTING));
        InterlockedExchange(&busy, 0); return 0;
    }
    r = ((SaveFn)vtbl[VSLOT_SAVE])(obj);
    logf_("save(%s) -> %d", why, (int)r);
    InterlockedExchange(&busy, 0);
    return 1;
}

/* ------------------------------------------------------------------ janela / velocidade */
static volatile LONG g_speed = 1;
static DWORD g_anchor_real, g_anchor_virt;
static int   g_time_init;
static HWND  g_hwnd;
static char  g_title0[256];

static BOOL CALLBACK enum_cb(HWND h, LPARAM lp)
{
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { *(HWND *)lp = h; return FALSE; }
    return TRUE;
}
static HWND game_window(void)
{
    if (g_hwnd && IsWindow(g_hwnd)) return g_hwnd;
    g_hwnd = NULL; EnumWindows(enum_cb, (LPARAM)&g_hwnd);
    if (g_hwnd) GetWindowTextA(g_hwnd, g_title0, sizeof g_title0);
    return g_hwnd;
}
static DWORD WINAPI beep_thread(LPVOID p) { Beep(450 + 180 * (DWORD)(INT_PTR)p, 55); return 0; }

static DWORD (WINAPI *real_timeGetTime)(void);

static DWORD virt_time(DWORD now)
{
    return g_anchor_virt + (DWORD)((unsigned long long)(DWORD)(now - g_anchor_real) * (unsigned)g_speed);
}
static void set_speed(int s, DWORD now)
{
    HWND w; char buf[300];
    if (s < 1) s = 1;
    if (s > 5) s = 5;
    if (s == g_speed) return;
    g_anchor_virt = virt_time(now); g_anchor_real = now; g_speed = s;   /* sem saltos no tempo */
    logf_("velocidade = %dx", s);
    w = game_window();
    if (w) {
        if (s == 1) snprintf(buf, sizeof buf, "%s", g_title0);
        else snprintf(buf, sizeof buf, "%s  [%dx]", g_title0, s);
        SetWindowTextA(w, buf);
    }
    CloseHandle(CreateThread(NULL, 0, beep_thread, (LPVOID)(INT_PTR)s, 0, NULL));
}

/* ------------------------------------------------------------------ teclado + autosave (thread principal) */
static int key_edge(int vk, int *prev)
{
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int edge = down && !*prev; *prev = down; return edge;
}
static void poll(DWORD now)
{
    static int pF[5], pPlus, pMinus, pF4;
    HWND fg = GetForegroundWindow(); DWORD pid = 0; int i;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;           /* so reage com o jogo em foco */
    for (i = 0; i < 5; i++) if (key_edge(VK_F5 + i, &pF[i])) set_speed(i + 1, now);
    if (key_edge(VK_ADD, &pPlus))      set_speed(g_speed + 1, now);
    if (key_edge(VK_SUBTRACT, &pMinus)) set_speed(g_speed - 1, now);
    if (key_edge(VK_F4, &pF4)) {
        int ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        int alt  = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        if (ctrl || alt) {
            do_save(ctrl ? "Ctrl+F4" : "Alt+F4");
            if (ctrl) { HWND w = game_window(); if (w) PostMessageA(w, WM_CLOSE, 0, 0); }
        }
    }
}

static DWORD g_last_poll, g_last_save;
static DWORD WINAPI my_timeGetTime(void)
{
    DWORD now = real_timeGetTime();
    if (!g_time_init) { g_anchor_real = g_anchor_virt = now; g_last_poll = g_last_save = now; g_time_init = 1; }
    if (GetCurrentThreadId() == g_main_tid) {
        static int busy;
        if (!busy) {
            busy = 1;
            if ((DWORD)(now - g_last_poll) >= 15) { g_last_poll = now; poll(now); }
            if (g_autosave_sec > 0 && (DWORD)(now - g_last_save) >= (DWORD)g_autosave_sec * 1000u) { g_last_save = now; do_save("autosave"); }
            busy = 0;
        }
    }
    return virt_time(now);
}

/* ------------------------------------------------------------------ troca da IAT do exe */
static void install_hook(void)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    IMAGE_IMPORT_DESCRIPTOR *d;
    if (!rva) return;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); d->Name; d++) {
        const char *dll = (const char *)(base + d->Name);
        IMAGE_THUNK_DATA *oft, *ft;
        if (lstrcmpiA(dll, "winmm.dll") != 0) continue;
        oft = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        ft  = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, "timeGetTime") == 0) {
                DWORD old;
                if ((void *)ft->u1.Function == (void *)my_timeGetTime) return;           /* ja instalado */
                if (!real_timeGetTime) real_timeGetTime = (DWORD (WINAPI *)(void))ft->u1.Function;
                if (VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                    ft->u1.Function = (DWORD_PTR)my_timeGetTime;
                    VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
                    logf_("gancho timeGetTime instalado (iat=%p real=%p)", (void *)&ft->u1.Function, (void *)real_timeGetTime);
                }
                return;
            }
        }
    }
    logf_("timeGetTime nao encontrado na IAT");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        char *p;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        p = strrchr(g_dir, '\\'); if (p) p[1] = 0;
        snprintf(g_log, sizeof g_log, "%stsuhan_pt.log", g_dir);
        snprintf(g_ini, sizeof g_ini, "%stsuhan_pt.ini", g_dir);
        g_autosave_sec = GetPrivateProfileIntA("Config", "AutoSaveSeconds", 60, g_ini);
        g_logging      = GetPrivateProfileIntA("Config", "Log", 1, g_ini);
        if (g_logging) { FILE *f = fopen(g_log, "wb"); if (f) fclose(f); }   /* recomeca o log a cada execucao */
        g_main_tid = GetCurrentThreadId();
        logf_("tsuhan_pt carregado (autosave=%ds)", g_autosave_sec);
        install_hook();
    }
    return TRUE;
}
