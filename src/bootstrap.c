/* See bootstrap.h. Layout next to the program (the folder it is started from):
 *   *.pkg        your fake PKGs of the game and its updates (or a ready game folder)
 *   game/        the unpacked game (made on the first start; the PKGs can be deleted after)
 *   out/         the prepared executable (made on the first start, again after a game change)
 *   user/        saves, shader caches, last_run.log
 *   <program>.ini settings (in-game menu: Insert, or R3+L2 on a controller) */
#define _FILE_OFFSET_BITS 64
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "bootstrap.h"
#include "game.h"
#include "boot_pkg.h"
#include "boot_prepare.h"
#include "boot_util.h"
#include <SDL3/SDL.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <winevt.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#define TITLE GAME_TITLE
/* Bump when the prepared files change format: the next start prepares again. */
#define PREPARE_VERSION GAME_PROGRAM "-prepare-1"
static const char *const MODULES[] = GAME_MODULES;

static char exe_path[4096], base_dir[4096], game_dir[4096], out_dir[4096], user_dir[4096];

static void set_env(const char *name, const char *value, int overwrite);
static void message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void message(const char *fmt, ...) {
    char text[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    fprintf(stderr, "%s\n", text);
    fflush(NULL);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TITLE, text, NULL);
}

static int find_program_dir(void) {
#ifdef _WIN32
    wchar_t w[4096];
    DWORD n = GetModuleFileNameW(NULL, w, 4096);
    if (!n || n >= 4096 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, exe_path, sizeof(exe_path), NULL, NULL)) return -1;
    snprintf(base_dir, sizeof(base_dir), "%s", exe_path);
    char *slash = strrchr(base_dir, '\\');
#else
    ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (n <= 0) return -1;
    exe_path[n] = 0;
    snprintf(base_dir, sizeof(base_dir), "%s", exe_path);
    char *slash = strrchr(base_dir, '/');
#endif
    if (!slash) return -1;
    *slash = 0;
    return 0;
}
static int in_base(char *out, size_t size, const char *name) {
#ifdef _WIN32
    return boot_path(out, size, "%s\\%s", base_dir, name);
#else
    return boot_path(out, size, "%s/%s", base_dir, name);
#endif
}

/* ---------------------------------------------------------------- log */
/* The log is always written in append mode: the game process (watch_game) writes to the same
 * file, each write at its end. */
static int reopen_log(const char *path) {
#ifdef _WIN32
    wchar_t w[4096];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 4096)) return -1;
    if (!_wfreopen(w, L"a", stdout)) return -1;
    /* A GUI program starts without a standard error stream (no descriptor 2 to _dup2 onto):
     * stderr gets its own append-mode stream on the same file. */
    _wfreopen(w, L"a", stderr);
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
#else
    if (!freopen(path, "a", stdout)) return -1;
    dup2(fileno(stdout), fileno(stderr));
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    return 0;
}
void bootstrap_game_log(void) {
    const char *path = getenv("RECOMP_LOG");
    if (path && *path) reopen_log(path);
}
static void open_log(int append) {
    char path[4096];
    if (!boot_path(path, sizeof(path), "%s/last_run.log", user_dir)) return;
    if (!append) {
#ifdef _WIN32
        wchar_t w[4096];
        FILE *f = MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 4096) ? _wfopen(w, L"w") : NULL;
#else
        FILE *f = fopen(path, "w");
#endif
        if (f) fclose(f);
    }
    set_env("RECOMP_LOG", path, 1);
    if (reopen_log(path)) return;
    time_t now = time(NULL);
    char when[64];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("%s %s (%s)\n", append ? "Restarted" : "Started", when, exe_path);
}

/* ---------------------------------------------------------------- PKGs */
typedef struct {
    char path[4096];
    BootPkgInfo info;
    uint64_t size;
    int64_t mtime;
} FoundPkg;
typedef struct { FoundPkg list[16]; int count; const char *dir; } PkgScan;
static int ends_with_pkg(const char *name) {
    size_t n = strlen(name);
    return n > 4 && (!strcmp(name + n - 4, ".pkg") || !strcmp(name + n - 4, ".PKG"));
}
static int scan_one(void *ctx, const char *name) {
    PkgScan *s = ctx;
    if (!ends_with_pkg(name) || s->count == 16) return 0;
    FoundPkg *f = &s->list[s->count];
    char err[256];
    if (!boot_path(f->path, sizeof(f->path), "%s/%s", s->dir, name) || boot_stat(f->path, &f->size, &f->mtime)) return 0;
    if (boot_pkg_info(f->path, &f->info, err, sizeof(err))) {
        printf("PKG %s skipped: %s\n", name, err);
        return 0;
    }
    static const char *const ids[] = GAME_TITLE_IDS;
    int ours = strstr(f->info.title, GAME_TITLE_MATCH) != NULL;
    for (int i = 0; ids[i] && !ours; ++i) ours = !strcmp(f->info.title_id, ids[i]);
    if (!ours) {
        printf("PKG %s skipped: another game (%s, %s)\n", name, f->info.title_id, f->info.title);
        return 0;
    }
    if (strcmp(f->info.category, "gd") && strcmp(f->info.category, "gp")) {
        printf("PKG %s skipped: category %s (not the game or an update)\n", name, f->info.category);
        return 0;
    }
    ++s->count;
    return 0;
}
static int pkg_order(const void *a, const void *b) {  /* the game, then updates by version */
    const FoundPkg *x = a, *y = b;
    int gx = !strcmp(x->info.category, "gd"), gy = !strcmp(y->info.category, "gd");
    if (gx != gy) return gy - gx;
    return strcmp(x->info.app_ver, y->info.app_ver);
}
/* One line per PKG: what the unpacked folder was made from. */
static void pkg_stamp(const PkgScan *s, char *out, size_t size) {
    out[0] = 0;
    for (int i = 0; i < s->count; ++i) {
        size_t used = strlen(out);
        snprintf(out + used, size - used, "%s %s %s %" PRIu64 " %" PRId64 "\n", s->list[i].info.content_id,
                 s->list[i].info.category, s->list[i].info.app_ver, s->list[i].size, s->list[i].mtime);
    }
}

/* Progress window while the PKGs unpack (about 1 GB: a few seconds to a minute). */
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    uint64_t done_before, total;
    Uint64 last;
    int closed;
} Progress;
static void progress_draw(Progress *p, uint64_t done) {
    if (!p->renderer) return;
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) p->closed = 1;
    Uint64 now = SDL_GetTicks();
    if (now - p->last < 50 && done < p->total) return;
    p->last = now;
    double f = p->total ? (double)done / (double)p->total : 0;
    if (f > 1) f = 1;
    char title[128];
    snprintf(title, sizeof(title), TITLE " - first start: unpacking the game %d%%", (int)(f * 100));
    SDL_SetWindowTitle(p->window, title);
    SDL_SetRenderDrawColor(p->renderer, 16, 18, 28, 255);
    SDL_RenderClear(p->renderer);
    SDL_FRect frame = {20, 30, 520, 28}, bar = {24, 34, (float)(512 * f), 20};
    SDL_SetRenderDrawColor(p->renderer, 90, 100, 130, 255);
    SDL_RenderRect(p->renderer, &frame);
    SDL_SetRenderDrawColor(p->renderer, 70, 170, 255, 255);
    SDL_RenderFillRect(p->renderer, &bar);
    SDL_RenderPresent(p->renderer);
}
static void progress_cb(void *ctx, uint64_t done, uint64_t total) {
    (void)total;
    Progress *p = ctx;
    progress_draw(p, p->done_before + done);
}

static int import_pkgs(PkgScan *s) {
    qsort(s->list, (size_t)s->count, sizeof(FoundPkg), pkg_order);
    if (strcmp(s->list[0].info.category, "gd")) {
        message("Only an update PKG was found next to the program.\n\nPut " GAME_PKG_HINT " in the same folder too.");
        return 1;
    }
    for (int i = 1; i < s->count; ++i)
        if (!strcmp(s->list[i].info.category, "gd")) {
            message("More than one game PKG next to the program:\n%s\n%s\n\nKeep one of them.", s->list[0].path, s->list[i].path);
            return 1;
        }
    Progress p = {0};
    for (int i = 0; i < s->count; ++i) p.total += boot_pkg_payload_size(s->list[i].path);
    p.window = SDL_CreateWindow(TITLE " - first start", 560, 88, 0);
    /* Software: no GPU driver is loaded before the loader sets up the game's memory. */
    if (p.window) p.renderer = SDL_CreateRenderer(p.window, "software");
    char stamp_path[4096], err[512];
    if (!boot_path(stamp_path, sizeof(stamp_path), "%s/." GAME_PROGRAM "-import", game_dir)) return 1;
    boot_remove(stamp_path);
    int r = 0;
    for (int i = 0; i < s->count && !r; ++i) {
        printf("Unpacking %s (%s %s)\n", s->list[i].path, s->list[i].info.category, s->list[i].info.app_ver);
        fflush(stdout);
        Uint64 t0 = SDL_GetTicks();
        if (boot_pkg_extract(s->list[i].path, game_dir, progress_cb, &p, err, sizeof(err))) {
            message("Could not unpack %s:\n%s", s->list[i].path, err);
            r = 1;
            break;
        }
        p.done_before += boot_pkg_payload_size(s->list[i].path);
        printf("  done in %.1f s\n", (double)(SDL_GetTicks() - t0) / 1000.0);
    }
    if (!r) {
        char stamp[4096];
        pkg_stamp(s, stamp, sizeof(stamp));
        boot_write_file(stamp_path, stamp, strlen(stamp));
    }
    if (p.renderer) SDL_DestroyRenderer(p.renderer);
    if (p.window) SDL_DestroyWindow(p.window);
    return r;
}

/* The game folder: game/ next to the program (unpacked from the PKGs there when needed), or the
 * program's own folder when eboot.bin is right next to it. */
static int find_game(void) {
    char path[4096];
    if (in_base(path, sizeof(path), "eboot.bin") && boot_is_file(path)) {
        snprintf(game_dir, sizeof(game_dir), "%s", base_dir);
        return 0;
    }
    if (!in_base(game_dir, sizeof(game_dir), "game")) return 1;
    PkgScan s = {.dir = base_dir};
    boot_list_dir(base_dir, scan_one, &s);
    char eboot[4096], stamp_path[4096];
    boot_path(eboot, sizeof(eboot), "%s/eboot.bin", game_dir);
    boot_path(stamp_path, sizeof(stamp_path), "%s/." GAME_PROGRAM "-import", game_dir);
    int have_game = boot_is_file(eboot);
    if (!s.count) {
        if (have_game) return 0;
        message(GAME_TITLE "'s game files were not found.\n\n"
                "Put " GAME_PKG_HINT " (and its updates, if you have them) in the same folder as this program:\n%s\n\n"
                "Or a game folder named \"game\" (with eboot.bin in it).",
                base_dir);
        return 1;
    }
    qsort(s.list, (size_t)s.count, sizeof(FoundPkg), pkg_order);
    char want[4096];
    pkg_stamp(&s, want, sizeof(want));
    size_t have_size = 0;
    char *have = (char *)boot_read_file(stamp_path, &have_size);
    int up_to_date = have && have_size == strlen(want) && !memcmp(have, want, have_size);
    free(have);
    /* A game folder that was not made from these PKGs (no stamp) is the player's own: kept. */
    if (have_game && (up_to_date || !boot_is_file(stamp_path))) return 0;
    if (boot_mkdir_p(game_dir)) {
        message("Cannot create the folder %s", game_dir);
        return 1;
    }
    return import_pkgs(&s);
}

/* ---------------------------------------------------------------- prepare */
static void file_stamp(const char *path, char *out, size_t size) {
    uint64_t n = 0;
    int64_t t = 0;
    boot_stat(path, &n, &t);
    size_t used = strlen(out);
    snprintf(out + used, size - used, "%" PRIu64 " %" PRId64 "\n", n, t);
}
static int prepare(void) {
    char stamp[2048] = PREPARE_VERSION "\n", path[4096];
    boot_path(path, sizeof(path), "%s/eboot.bin", game_dir);
    file_stamp(path, stamp, sizeof(stamp));
    for (int i = 0; MODULES[i]; ++i) {
        boot_path(path, sizeof(path), "%s/sce_module/%s", game_dir, MODULES[i]);
        file_stamp(path, stamp, sizeof(stamp));
    }
    char stamp_path[4096];
    boot_path(stamp_path, sizeof(stamp_path), "%s/prepared.stamp", out_dir);
    size_t have_size = 0;
    char *have = (char *)boot_read_file(stamp_path, &have_size);
    int same = have && have_size == strlen(stamp) && !memcmp(have, stamp, have_size);
    free(have);
    const char *files[] = {"boot-linked.bin", "functions.bin", "content.bin"};
    for (size_t i = 0; same && i < 3; ++i) {
        boot_path(path, sizeof(path), "%s/%s", out_dir, files[i]);
        same = boot_is_file(path);
    }
    if (same) return 0;
    char err[512];
    boot_remove(stamp_path);
    if (boot_prepare(game_dir, out_dir, MODULES, err, sizeof(err))) {
        message("Could not prepare the game:\n%s\n\nGame folder: %s", err, game_dir);
        return 1;
    }
    boot_write_file(stamp_path, stamp, strlen(stamp));
    return 0;
}

/* ---------------------------------------------------------------- environment */
static void set_env(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
static void wait_for_process(unsigned long pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (h) {
        WaitForSingleObject(h, 30000);
        CloseHandle(h);
    }
#else
    (void)pid;  /* Linux restarts replace the process (exec): nothing to wait for */
#endif
}

#ifdef _WIN32
/* ---------------------------------------------------------------- watching the game */
/* The game runs in a child process (this program again, with the loader's arguments) so that an
 * end no handler inside the game sees (a fast fail, a driver killing the process) is still
 * recorded: the exit code, and Windows' own Application Error report for it, go to the log. */
static void quote_arg(wchar_t *out, size_t cap, size_t *len, const char *arg) {
    wchar_t w[4400];
    if (!MultiByteToWideChar(CP_UTF8, 0, arg, -1, w, 4400)) w[0] = 0;
    size_t n = *len;
    if (n && n + 1 < cap) out[n++] = L' ';
    if (n + 1 < cap) out[n++] = L'"';
    for (const wchar_t *c = w; *c; ++c) {
        size_t slashes = 0;
        while (c[slashes] == L'\\') ++slashes;
        if (slashes) {  /* backslashes are doubled only before a quote or the closing quote */
            size_t times = (!c[slashes] || c[slashes] == L'"') ? 2 : 1;
            for (size_t i = 0; i < slashes * times && n + 1 < cap; ++i) out[n++] = L'\\';
            c += slashes - 1;
            continue;
        }
        if (*c == L'"' && n + 1 < cap) out[n++] = L'\\';
        if (n + 1 < cap) out[n++] = *c;
    }
    if (n + 1 < cap) out[n++] = L'"';
    out[n] = 0;
    *len = n;
}
static const char *exit_name(DWORD code) {
    switch (code) {
    case 3: return "crash, reported above";
    case 0xC0000005: return "access violation";
    case 0xC0000409: return "fast fail (stack buffer overrun or a failed security check)";
    case 0xC0000374: return "heap corruption";
    case 0xC00000FD: return "stack overflow";
    case 0xC000001D: return "illegal instruction";
    case 0xC0000142: return "a DLL failed to initialize";
    case 0xC0000135: return "a DLL is missing";
    case 0x40010004: return "ended from outside (debugger or Task Manager)";
    case 0xE0000008: return "out of memory";
    default: return "";
    }
}
/* Windows' Application Error and Windows Error Reporting events for this program in the last
 * two minutes (they name the faulting module, its offset and the exception code). */
static int log_windows_report(void) {
    typedef EVT_HANDLE (WINAPI *Query)(EVT_HANDLE, LPCWSTR, LPCWSTR, DWORD);
    typedef BOOL (WINAPI *Next)(EVT_HANDLE, DWORD, EVT_HANDLE *, DWORD, DWORD, PDWORD);
    typedef BOOL (WINAPI *Format)(EVT_HANDLE, EVT_HANDLE, DWORD, DWORD, PEVT_VARIANT, DWORD, DWORD, LPWSTR, PDWORD);
    typedef EVT_HANDLE (WINAPI *Publisher)(EVT_HANDLE, LPCWSTR, LPCWSTR, LCID, DWORD);
    typedef BOOL (WINAPI *Close)(EVT_HANDLE);
    typedef BOOL (WINAPI *Render)(EVT_HANDLE, EVT_HANDLE, DWORD, DWORD, PVOID, PDWORD, PDWORD);
    HMODULE m = LoadLibraryW(L"wevtapi.dll");
    if (!m) return 0;
    Query query = (Query)(void *)GetProcAddress(m, "EvtQuery");
    Next next = (Next)(void *)GetProcAddress(m, "EvtNext");
    Format format = (Format)(void *)GetProcAddress(m, "EvtFormatMessage");
    Publisher publisher = (Publisher)(void *)GetProcAddress(m, "EvtOpenPublisherMetadata");
    Close close = (Close)(void *)GetProcAddress(m, "EvtClose");
    Render render = (Render)(void *)GetProcAddress(m, "EvtRender");
    if (!query || !next || !format || !publisher || !close || !render) return 0;
    const wchar_t *q = L"*[System[(Provider[@Name='Application Error'] or Provider[@Name='Windows Error Reporting'])"
                       L" and TimeCreated[timediff(@SystemTime) <= 120000]]]";
    EVT_HANDLE results = query(NULL, L"Application", q, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!results) return 0;
    int found = 0;
    EVT_HANDLE events[8];
    DWORD got = 0;
    static wchar_t text[16384];
    char utf8[32768];
    while (next(results, 8, events, 2000, 0, &got) && got) {
        for (DWORD i = 0; i < got; ++i) {
            /* The provider name, for its message table. */
            wchar_t name[128] = L"";
            DWORD used = 0, props = 0;
            static wchar_t xml[16384];
            if (render(NULL, events[i], 1 /* EvtRenderEventXml */, sizeof(xml), xml, &used, &props)) {
                const wchar_t *a = wcsstr(xml, L"Provider Name='");
                if (a) {
                    a += 15;
                    size_t k = 0;
                    while (a[k] && a[k] != L'\'' && k + 1 < 128) { name[k] = a[k]; ++k; }
                    name[k] = 0;
                }
            }
            /* Only reports about this program. */
            if (!wcsstr(xml, L"" GAME_PROGRAM)) { close(events[i]); continue; }
            EVT_HANDLE meta = name[0] ? publisher(NULL, name, NULL, 0, 0) : NULL;
            DWORD n = 0;
            if (meta && format(meta, events[i], 0, 0, NULL, 1 /* EvtFormatMessageEvent */, 16384, text, &n)) {
                WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL);
                fprintf(stderr, "Windows report (%ls):\n%s\n", name, utf8);
            } else {
                WideCharToMultiByte(CP_UTF8, 0, xml, -1, utf8, sizeof(utf8), NULL, NULL);
                fprintf(stderr, "Windows report:\n%s\n", utf8);
            }
            if (meta) close(meta);
            close(events[i]);
            ++found;
        }
    }
    close(results);
    return found;
}
static void watch_game(int argc, char **argv) {
    static wchar_t cmd[32768];
    wchar_t exe[4096];
    size_t len = 0;
    MultiByteToWideChar(CP_UTF8, 0, exe_path, -1, exe, 4096);
    quote_arg(cmd, 32768, &len, exe_path);
    for (int i = 1; i < argc; ++i) quote_arg(cmd, 32768, &len, argv[i]);
    set_env("RECOMP_WATCHED", "1", 1);
    /* The game ends with this program (closed from Task Manager); what it starts (the restart)
     * does not. */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        memset(&limits, 0, sizeof(limits));
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    /* The game opens last_run.log itself (RECOMP_LOG, bootstrap_game_log). */
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "Starting the game failed (error %lu)\n", GetLastError());
        message(GAME_TITLE " could not start the game (error %lu).", GetLastError());
        ExitProcess(1);
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (code == 0) ExitProcess(0);
    const char *what = exit_name(code);
    fprintf(stderr, "The game ended with code 0x%08lx%s%s%s\n", code, *what ? " (" : "", what, *what ? ")" : "");
    if (code == 3 || code >= 0x80000000u) {
        /* Windows writes its report a moment after the process ends. */
        int found = 0;
        for (int i = 0; i < 20 && !found; ++i) {
            Sleep(500);
            found = log_windows_report();
        }
        if (!found) fprintf(stderr, "Windows wrote no Application Error report for it.\n");
        fflush(NULL);
        wchar_t log[4200];
        char where[4300];
        snprintf(where, sizeof(where), GAME_TITLE " stopped unexpectedly.\n\nThe details are in\n%s\\last_run.log", user_dir);
        MultiByteToWideChar(CP_UTF8, 0, where, -1, log, 4200);
        MessageBoxW(NULL, log, L"" GAME_TITLE, MB_OK | MB_ICONERROR);
    }
    ExitProcess(code);
}
#endif

int bootstrap_wanted(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if ((!strcmp(argv[i], "--after") || !strcmp(argv[i], "--timeout")) && i + 1 < argc) { ++i; continue; }
        if (!strcmp(argv[i], "--check-imports")) continue;
        return 0;
    }
    return 1;
}

int bootstrap(int *argc, char ***argv) {
    unsigned long after = 0;
    const char *timeout = "0";
    int check_imports = 0;
    for (int i = 1; i < *argc; ++i) {
        if (!strcmp((*argv)[i], "--after") && i + 1 < *argc) after = strtoul((*argv)[++i], NULL, 10);
        else if (!strcmp((*argv)[i], "--timeout") && i + 1 < *argc) timeout = (*argv)[++i];
        else if (!strcmp((*argv)[i], "--check-imports")) check_imports = 1;
    }
    if (after) wait_for_process(after);
    SDL_SetHint(SDL_HINT_APP_NAME, TITLE);
    SDL_Init(SDL_INIT_VIDEO);
    if (find_program_dir()) {
        message("Cannot find the program's own folder.");
        return 1;
    }
    in_base(out_dir, sizeof(out_dir), "out");
    in_base(user_dir, sizeof(user_dir), "user");
    char music[4200];
    boot_path(music, sizeof(music), "%s/music", user_dir);
    if (boot_mkdir_p(user_dir) || boot_mkdir_p(music) || boot_mkdir_p(out_dir)) {
        message("Cannot write to the program's folder:\n%s\n\nMove the program to a folder you can write to.", base_dir);
        return 1;
    }
    open_log(after != 0);
    int r = find_game();
    if (!r) r = prepare();
    SDL_Quit();
    if (r) return r;
    printf("Game folder: %s\n", game_dir);
    fflush(stdout);

    char path[4200];
    in_base(path, sizeof(path), GAME_PROGRAM ".ini");
    set_env("GOW3_CONFIG", path, 0);
    /* stereo_eye=left|right in the settings file: which image of the game's left/right display
     * pair is shown (test build: right). */
    {
        const char *eye = "right";
        FILE *ini = boot_fopen(path, "r");
        char line[256];
        static char value[16];
        while (ini && fgets(line, sizeof(line), ini)) {
            if (!strncmp(line, "stereo_eye=", 11)) {
                snprintf(value, sizeof(value), "%s", line + 11);
                value[strcspn(value, "\r\n ")] = 0;
                eye = value;
            }
        }
        if (ini) fclose(ini);
        set_env("GOW3_STEREO_EYE", eye, 0);
        printf("Display: the %s-eye image (stereo_eye= in %s)\n", eye, path);
    }
    set_env("GOW3_GPU_USER_DIR", user_dir, 0);
    /* As run.py for a game without a frame rate patch: the engine steps 1/60 s per frame. */
    set_env("GOW3_VBLANK_HZ", "60", 0);
    set_env("GOW3_ENGINE_FPS", "60", 0);
    set_env("GOW3_UPSCALER", "none", 0);
    set_env("GOW3_READBACK_LINEAR", "1", 0);
    /* The frames are light: the two-stage draw pipeline (a second thread recording draws)
     * is off. With it the GPU scheduler was used from both threads at once (an endRendering
     * with none open, flicker, NVIDIA driver faults). GOW3_DRAW_PIPE=1 turns it back on. */
    set_env("GOW3_DRAW_PIPE", "0", 0);
#ifndef _WIN32
    set_env("SDL_APP_ID", GAME_PROGRAM, 0);
#endif
    /* The in-game menu's "Apply and restart" starts the program again. */
    char restart[4400];
#ifdef _WIN32
    snprintf(restart, sizeof(restart), "\"%s\" --after %lu", exe_path, (unsigned long)GetCurrentProcessId());
#else
    snprintf(restart, sizeof(restart), "'%s'", exe_path);  /* exec replaces this process */
    if (strchr(exe_path, '\'')) restart[0] = 0;
#endif
    if (restart[0]) set_env("GOW3_RESTART_COMMAND", restart, 1);

    static char *args[16];
    static char boot[4200], content[4200], app0[4096], user[4096];
    boot_path(boot, sizeof(boot), "%s/boot-linked.bin", out_dir);
    boot_path(content, sizeof(content), "%s/content.bin", out_dir);
    snprintf(app0, sizeof(app0), "%s", game_dir);
    snprintf(user, sizeof(user), "%s", user_dir);
    int n = 0;
    args[n++] = (*argv)[0];
    args[n++] = boot;
    args[n++] = "--content-profile"; args[n++] = content;
    args[n++] = "--app0"; args[n++] = app0;
    args[n++] = "--user"; args[n++] = user;
    args[n++] = "--timeout"; args[n++] = (char *)timeout;
    if (check_imports) args[n++] = "--check-imports";
    args[n] = NULL;
    *argc = n;
    *argv = args;
#ifdef _WIN32
    watch_game(n, args);  /* does not return */
#endif
    return 0;
}

void bootstrap_cpu_message(const char *missing, int fatal) {
    if (fatal)
        message("This processor lacks instruction sets the game's code uses: %s\n\n"
                "An Intel Sandy Bridge (2011) or newer, or an AMD FX or newer processor, is needed.", missing);
    else
        message("This processor lacks %s, which the game's code uses.", missing);
}
