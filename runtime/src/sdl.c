/*
 * The SDL2 API the game calls, implemented with the host's SDL2.
 *
 * Guest handles (windows, controllers, joysticks, audio streams) are small
 * guest blocks holding the host pointer at +8. Structures are written in the
 * guest's (64-bit Apple) layout. SDL_Event is 56 bytes on both sides; the
 * only pointer-carrying events the game uses are user events, whose data
 * fields hold guest addresses.
 */
#include "objc_rt.h"

#include <SDL.h>
#include <SDL_syswm.h>
#include <math.h>
#include <string.h>

extern const char *g_bundle_path;

/* ---- handles ---------------------------------------------------------------- */

#define H_WINDOW 0x57494E44u
#define H_CTRL 0x4354524Cu
#define H_JOY 0x4A4F5953u

static uint32_t handle_new(uint32_t kind, void *host)
{
    uint32_t a = rt_alloc(16, 16);
    rt_st32(G(a), kind);
    rt_st64(G(a + 8), (uint64_t)(uintptr_t)host);
    return a;
}
static void *handle_host(uint64_t a, uint32_t kind)
{
    if (!a || rt_ld32(G(a)) != kind)
        return NULL;
    return (void *)(uintptr_t)rt_ld64(G(a + 8));
}

#define MAX_WINDOWS 8
static struct { SDL_Window *host; uint32_t guest; } windows[MAX_WINDOWS];
static SDL_GLContext gl_context;

static uint32_t window_guest(SDL_Window *w)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].host == w)
            return windows[i].guest;
    return 0;
}
static SDL_Window *window_host(uint64_t g) { return handle_host(g, H_WINDOW); }

static SDL_Window *hidden_window;

static void gl_attributes(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
}

/* The GL context exists before the game's window (Metal resources are made
 * first): it starts on a hidden window and moves to the game's window. */
static void gl_ensure(void)
{
#if defined(__vita__)
    return;      /* the Vita renderer owns the display (vitaGL), not SDL */
#endif
    if (gl_context)
        return;
    gl_attributes();
    hidden_window = SDL_CreateWindow("mina", 0, 0, 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!hidden_window)
        rt_fatal("cannot create the GL window: %s", SDL_GetError());
    gl_context = SDL_GL_CreateContext(hidden_window);
    if (!gl_context)
        rt_fatal("cannot create an OpenGL context: %s", SDL_GetError());
    SDL_GL_MakeCurrent(hidden_window, NULL);
}

/* For the renderer: the window GL draws to and the context. */
SDL_Window *sdl_main_window(void)
{
    gl_ensure();
    return windows[0].host ? windows[0].host : hidden_window;
}
SDL_GLContext sdl_gl_context(void)
{
    gl_ensure();
    return gl_context;
}

static void put_rect(uint64_t a, const SDL_Rect *r)
{
    rt_st32(G(a), (uint32_t)r->x);
    rt_st32(G(a + 4), (uint32_t)r->y);
    rt_st32(G(a + 8), (uint32_t)r->w);
    rt_st32(G(a + 12), (uint32_t)r->h);
}
static void put_int(uint64_t a, int v) { if (a) rt_st32(G(a), (uint32_t)v); }

/* ---- main thread -------------------------------------------------------------------
 * Cocoa wants windows on the main thread: natives called elsewhere hand their
 * whole call (same Ctx: the caller is blocked) to the main thread and wait. */

static SDL_threadID main_tid;
typedef struct MainJob {
    void (*fn)(Ctx *);
    Ctx *c;
    SDL_sem *done;
} MainJob;
static SDL_mutex *jobs_lock;
static MainJob *jobs[64];
static int njobs;
static void mainq_wake(void);

static void drain_jobs(void)
{
    for (;;) {
        SDL_LockMutex(jobs_lock);
        MainJob *j = njobs ? jobs[0] : NULL;
        if (j)
            memmove(jobs, jobs + 1, (size_t)(--njobs) * sizeof jobs[0]);
        SDL_UnlockMutex(jobs_lock);
        if (!j)
            return;
        j->fn(j->c);
        SDL_SemPost(j->done);
    }
}

/* true if the caller must return: the call ran on the main thread */
static int marshal(void (*fn)(Ctx *), Ctx *c)
{
    if (!main_tid || SDL_ThreadID() == main_tid)
        return 0;
    MainJob j = { fn, c, SDL_CreateSemaphore(0) };
    SDL_LockMutex(jobs_lock);
    jobs[njobs++] = &j;
    SDL_UnlockMutex(jobs_lock);
    mainq_wake();
    SDL_SemWait(j.done);
    SDL_DestroySemaphore(j.done);
    return 1;
}
#define ON_MAIN(fn) do { if (marshal(fn, c)) return; } while (0)

/* ---- init, hints, misc ------------------------------------------------------------ */

static void gl_ensure(void);
static void wake_init(void);

void nat_SDL_Init(Ctx *c)
{
    uint32_t flags = (uint32_t)A0;
    main_tid = SDL_ThreadID();
    if (!jobs_lock)
        jobs_lock = SDL_CreateMutex();
#if defined(__vita__)
    /* no SDL video on the Vita: vitaGL owns GXM (SDL's driver would set it up too) */
    int r = SDL_Init((flags & ~(uint32_t)SDL_INIT_VIDEO) | SDL_INIT_EVENTS);
#else
    int r = SDL_Init(flags | SDL_INIT_VIDEO | SDL_INIT_EVENTS);
#endif
    wake_init();
    gl_ensure();
    RET((uint64_t)(int64_t)r);
}
void nat_SDL_SetHint(Ctx *c)
{
    const char *n = gstr(A0), *v = gstr(A1);
    if (g_trace_imports)
        rt_log("SDL_SetHint(%s, %s)\n", n, v ? v : "(null)");
    if (strstr(n, "METAL") || strstr(n, "RENDER_DRIVER")) {
        RET(1);
        return;
    }
    RET(SDL_SetHint(n, v));
}
void nat_SDL_free(Ctx *c) { gfree((uint32_t)A0); }
void nat_SDL_memset(Ctx *c) { memset(G(A0), (int)A1, (size_t)A2); }
void nat_SDL_GetBasePath(Ctx *c)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/Contents/Resources/", g_bundle_path);
    RET(gstrdup(p));
}
void nat_SDL_GetPrefPath(Ctx *c)
{
    /* a separate folder, so the recompiled game never touches the original's saves */
    char app[256];
    snprintf(app, sizeof app, "%s (recomp)", gstr(A1));
    char *p = SDL_GetPrefPath(gstr(A0), app);
    uint32_t g = p ? gstrdup(p) : 0;
    SDL_free(p);
    RET(g);
}
void nat_SDL_OpenURL(Ctx *c) { ON_MAIN(nat_SDL_OpenURL); RET((uint64_t)(int64_t)SDL_OpenURL(gstr(A0))); }
void nat_SDL_ShowSimpleMessageBox(Ctx *c)
{ ON_MAIN(nat_SDL_ShowSimpleMessageBox);
    rt_log("[message box] %s: %s\n", gstr(A1), gstr(A2));
    RET((uint64_t)(int64_t)SDL_ShowSimpleMessageBox((Uint32)A0, gstr(A1), gstr(A2), NULL));
}

/* ---- windows ---------------------------------------------------------------------- */

#if defined(__vita__)
/* The Vita has no SDL window (the guest's handle stands for the 960x544
 * screen): the window calls answer for the screen instead of reaching SDL. */
#define SDL_GetWindowID(w) ((Uint32)1)
#define SDL_GetWindowPosition(w, x, y) (*(x) = 0, *(y) = 0)
#define SDL_SetWindowSize(w, x, y) ((void)0)
#define SDL_SetWindowPosition(w, x, y) ((void)0)
#define SDL_ShowWindow(w) ((void)0)
#define SDL_HideWindow(w) ((void)0)
#define SDL_RaiseWindow(w) ((void)0)
#define SDL_SetWindowTitle(w, t) ((void)(t))
#define SDL_SetWindowFullscreen(w, f) 0
#define SDL_SetWindowBordered(w, b) ((void)0)
#define SDL_SetWindowResizable(w, r) ((void)0)
#define SDL_SetWindowAlwaysOnTop(w, t) ((void)0)
#define SDL_SetWindowMinimumSize(w, x, y) ((void)0)
#define SDL_SetWindowOpacity(w, o) ((void)(o), 0)
#define SDL_GetWindowDisplayIndex(w) 0
#define SDL_DestroyWindow(w) ((void)0)
#endif

void nat_SDL_CreateWindow(Ctx *c)
{ ON_MAIN(nat_SDL_CreateWindow); if (g_trace_imports) rt_log("[win] CreateWindow(%#llx %lld %lld %lld %lld %#llx)\n", (unsigned long long)A0, (long long)A1, (long long)A2, (long long)A3, (long long)A4, (unsigned long long)A5);
    uint32_t flags = (uint32_t)A5;
    flags &= ~(uint32_t)SDL_WINDOW_METAL;
    flags |= SDL_WINDOW_OPENGL;
    /* macOS puts fullscreen windows on their own Space: windowed unless asked */
    if (!getenv("MINA_FULLSCREEN"))
        flags &= ~(uint32_t)SDL_WINDOW_FULLSCREEN_DESKTOP;
    gl_attributes();
#if defined(__vita__)
    /* no SDL window: the handle stands for the Vita's screen */
    (void)flags;
    SDL_Window *w = (SDL_Window *)(uintptr_t)0x56495441u;
#else
    int ww = (int)A3, wh = (int)A4;
    /* a window the size of the whole display (the game asks fullscreen
     * desktop, which is dropped above) stays black on macOS: it fits the
     * display's usable area instead, below the menu bar and above the Dock,
     * with room for its title bar and the game's aspect */
    SDL_Rect ub;
    if (!(flags & SDL_WINDOW_FULLSCREEN) && SDL_GetDisplayUsableBounds(0, &ub) == 0 && ub.w > 0 && ub.h > 32) {
        const int maxw = ub.w, maxh = ub.h - 32;
        if (ww > maxw || wh > maxh) {
            const double k = (double)maxw / ww < (double)maxh / wh ? (double)maxw / ww : (double)maxh / wh;
            ww = (int)(ww * k);
            wh = (int)(wh * k);
        }
    }
    SDL_Window *w = SDL_CreateWindow(gstr(A0), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     ww, wh, flags);
#endif
#if !defined(__vita__)
    if (w)
        SDL_RaiseWindow(w);       /* started from a terminal, the app is not active */
#endif
    if (!w) {
        rt_log("SDL_CreateWindow failed: %s\n", SDL_GetError());
        RET(0);
        return;
    }
    gl_ensure();
    uint32_t g = handle_new(H_WINDOW, w);
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!windows[i].host) {
            windows[i].host = w;
            windows[i].guest = g;
            break;
        }
    RET(g);
}
void nat_SDL_DestroyWindow(Ctx *c)
{ ON_MAIN(nat_SDL_DestroyWindow); if (g_trace_imports) rt_log("[win] DestroyWindow(%#llx %lld %lld %lld %lld %#llx)\n", (unsigned long long)A0, (long long)A1, (long long)A2, (long long)A3, (long long)A4, (unsigned long long)A5);
    SDL_Window *w = window_host(A0);
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].host == w)
            windows[i].host = NULL;
    if (w && w != windows[0].host)
        SDL_DestroyWindow(w);
}
void nat_SDL_GetWindowID(Ctx *c) { RET(SDL_GetWindowID(window_host(A0))); }
void nat_SDL_GetWindowSize(Ctx *c)
{
#if defined(__vita__)
    put_int(A1, 960);
    put_int(A2, 544);
    return;
#endif
    ON_MAIN(nat_SDL_GetWindowSize);
    int w = 0, h = 0;
    SDL_GetWindowSize(window_host(A0), &w, &h);
    put_int(A1, w);
    put_int(A2, h);
}
static void window_resize(SDL_Window *w, int width, int height);
void nat_SDL_SetWindowSize(Ctx *c) { ON_MAIN(nat_SDL_SetWindowSize); if (g_trace_imports) rt_log("[win] SetWindowSize(%#llx %lld %lld %lld %lld %#llx)\n", (unsigned long long)A0, (long long)A1, (long long)A2, (long long)A3, (long long)A4, (unsigned long long)A5); window_resize(window_host(A0), (int)A1, (int)A2); }
void nat_SDL_GetWindowPosition(Ctx *c)
{ ON_MAIN(nat_SDL_GetWindowPosition);
    int x = 0, y = 0;
    SDL_GetWindowPosition(window_host(A0), &x, &y);
    put_int(A1, x);
    put_int(A2, y);
}
void nat_SDL_SetWindowPosition(Ctx *c) { ON_MAIN(nat_SDL_SetWindowPosition); if (g_trace_imports) rt_log("[win] SetWindowPosition(%#llx %lld %lld %lld %lld %#llx)\n", (unsigned long long)A0, (long long)A1, (long long)A2, (long long)A3, (long long)A4, (unsigned long long)A5); SDL_SetWindowPosition(window_host(A0), (int)A1, (int)A2); }
void nat_SDL_ShowWindow(Ctx *c) { ON_MAIN(nat_SDL_ShowWindow); if (g_trace_imports) rt_log("[win] ShowWindow(%#llx %lld %lld %lld %lld %#llx)\n", (unsigned long long)A0, (long long)A1, (long long)A2, (long long)A3, (long long)A4, (unsigned long long)A5); SDL_ShowWindow(window_host(A0)); }
void nat_SDL_HideWindow(Ctx *c) { ON_MAIN(nat_SDL_HideWindow); if (g_trace_imports) rt_log("[win] HideWindow(%#llx %lld %lld %lld %lld %#llx)\n", (unsigned long long)A0, (long long)A1, (long long)A2, (long long)A3, (long long)A4, (unsigned long long)A5); SDL_HideWindow(window_host(A0)); }
void nat_SDL_SetWindowTitle(Ctx *c) { ON_MAIN(nat_SDL_SetWindowTitle); SDL_SetWindowTitle(window_host(A0), gstr(A1)); }
void nat_SDL_SetWindowFullscreen(Ctx *c) { ON_MAIN(nat_SDL_SetWindowFullscreen); if (g_trace_imports) rt_log("[win] SetWindowFullscreen(%#llx %lld %lld %lld %lld %#llx)\n", (unsigned long long)A0, (long long)A1, (long long)A2, (long long)A3, (long long)A4, (unsigned long long)A5); if (!getenv("MINA_FULLSCREEN")) { RET(0); return; } RET((uint64_t)(int64_t)SDL_SetWindowFullscreen(window_host(A0), (Uint32)A1)); }
void nat_SDL_SetWindowBordered(Ctx *c) { ON_MAIN(nat_SDL_SetWindowBordered); if (g_trace_imports) rt_log("[win] SetWindowBordered(%#llx %lld %lld %lld %lld %#llx)\n", (unsigned long long)A0, (long long)A1, (long long)A2, (long long)A3, (long long)A4, (unsigned long long)A5); SDL_SetWindowBordered(window_host(A0), (SDL_bool)(A1 & 1)); }
void nat_SDL_SetWindowResizable(Ctx *c) { ON_MAIN(nat_SDL_SetWindowResizable); SDL_SetWindowResizable(window_host(A0), (SDL_bool)(A1 & 1)); }
void nat_SDL_SetWindowAlwaysOnTop(Ctx *c) { ON_MAIN(nat_SDL_SetWindowAlwaysOnTop); SDL_SetWindowAlwaysOnTop(window_host(A0), (SDL_bool)(A1 & 1)); }
void nat_SDL_SetWindowMinimumSize(Ctx *c) { ON_MAIN(nat_SDL_SetWindowMinimumSize); SDL_SetWindowMinimumSize(window_host(A0), (int)A1, (int)A2); }
void nat_SDL_SetWindowOpacity(Ctx *c) { ON_MAIN(nat_SDL_SetWindowOpacity); RET((uint64_t)(int64_t)SDL_SetWindowOpacity(window_host(A0), FARG(0))); }
void nat_SDL_GetWindowDisplayIndex(Ctx *c) { ON_MAIN(nat_SDL_GetWindowDisplayIndex); RET((uint64_t)(int64_t)SDL_GetWindowDisplayIndex(window_host(A0))); }
void nat_SDL_GetWindowWMInfo(Ctx *c)
{
    /* version (3 bytes), subsystem (int at +4): report Cocoa, no native handles */
    uint8_t *info = G(A1);
    memset(info + 4, 0, 64);
    rt_st32(info + 4, SDL_SYSWM_COCOA);
    RET(1);
}

/* Metal view / layer: the layer is the renderer's CAMetalLayer object. */
uint64_t metal_layer_for_window(SDL_Window *w);
void nat_SDL_Metal_CreateView(Ctx *c) { RET(A0); }
void nat_SDL_Metal_GetLayer(Ctx *c) { RET(metal_layer_for_window(window_host(A0))); }

/* ---- displays ----------------------------------------------------------------------- */

void nat_SDL_GetNumVideoDisplays(Ctx *c) { RET((uint64_t)(int64_t)SDL_GetNumVideoDisplays()); }
void nat_SDL_GetDisplayBounds(Ctx *c)
{ ON_MAIN(nat_SDL_GetDisplayBounds);
    SDL_Rect r;
    int e = SDL_GetDisplayBounds((int)A0, &r);
    if (!e) put_rect(A1, &r);
    RET((uint64_t)(int64_t)e);
}
void nat_SDL_GetDisplayUsableBounds(Ctx *c)
{ ON_MAIN(nat_SDL_GetDisplayUsableBounds);
    SDL_Rect r;
    int e = SDL_GetDisplayUsableBounds((int)A0, &r);
    if (!e) put_rect(A1, &r);
    RET((uint64_t)(int64_t)e);
}

/* ---- mouse -------------------------------------------------------------------------- */

void nat_SDL_GetMouseState(Ctx *c)
{
    int x, y;
    Uint32 b = SDL_GetMouseState(&x, &y);
    put_int(A0, x);
    put_int(A1, y);
    RET(b);
}
void nat_SDL_GetGlobalMouseState(Ctx *c)
{
    int x, y;
    Uint32 b = SDL_GetGlobalMouseState(&x, &y);
    put_int(A0, x);
    put_int(A1, y);
    RET(b);
}
void nat_SDL_GetMouseFocus(Ctx *c) { RET(window_guest(SDL_GetMouseFocus())); }
void nat_SDL_GetRelativeMouseMode(Ctx *c) { RET(SDL_GetRelativeMouseMode()); }
void nat_SDL_SetRelativeMouseMode(Ctx *c) { ON_MAIN(nat_SDL_SetRelativeMouseMode); RET((uint64_t)(int64_t)SDL_SetRelativeMouseMode((SDL_bool)(A0 & 1))); }
void nat_SDL_WarpMouseGlobal(Ctx *c) { ON_MAIN(nat_SDL_WarpMouseGlobal); RET((uint64_t)(int64_t)SDL_WarpMouseGlobal((int)A0, (int)A1)); }
void nat_SDL_ShowCursor(Ctx *c) { ON_MAIN(nat_SDL_ShowCursor); RET((uint64_t)(int64_t)SDL_ShowCursor((int)A0)); }

/* ---- events ----------------------------------------------------------------------------- */

static uint64_t filter_fn, filter_ud;

/* A host event, as the guest sees it (per-thread scratch). */
static uint32_t guest_event(const SDL_Event *e)
{
#if defined(__vita__)
#define scratch (host_tls()->scratch)
#else
    static __thread uint32_t scratch;
#endif
    if (!scratch)
        scratch = rt_alloc(64, 16);
    memcpy(G(scratch), e, sizeof *e);
    return scratch;
#undef scratch
}

static Uint32 wake_event;

/* SDL runs the filter in whatever thread adds the event, possibly inside a
 * native that still needs its context: the context is saved around the call. */
static int SDLCALL host_filter(void *ud, SDL_Event *e)
{
    (void)ud;
    if (!filter_fn || (wake_event && e->type == wake_event))
        return 1;
    Ctx *c = host_thread_ctx();
    Ctx saved = *c;
    uint32_t g = guest_event(e);
    int r = (int)(int32_t)guest_call(c, filter_fn, 2, filter_ud, (uint64_t)g);
    memcpy(e, G(g), sizeof *e);
    *c = saved;
    return r;
}

void nat_SDL_SetEventFilter(Ctx *c)
{
    filter_fn = A0;
    filter_ud = A1;
    SDL_SetEventFilter(filter_fn ? host_filter : NULL, NULL);
}
void nat_SDL_FilterEvents(Ctx *c)
{
    uint64_t fn = A0, ud = A1;
    SDL_Event ev[64];
    /* run the guest filter over the queue, keeping what it accepts */
    int n = SDL_PeepEvents(ev, 64, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
    for (int i = 0; i < n; i++) {
        uint32_t g = guest_event(&ev[i]);
        if ((int)(int32_t)guest_call(c, fn, 2, ud, (uint64_t)g)) {
            memcpy(&ev[i], G(g), sizeof ev[i]);
            SDL_PushEvent(&ev[i]);
        }
    }
}
void nat_SDL_RegisterEvents(Ctx *c) { RET(SDL_RegisterEvents((int)A0)); }
/* MINA_INPUT="8:Return 12:Down 13.5:Return": scripted key taps (seconds since
 * start : SDL key name), injected as real key events for testing. */
void mem_report(void);
/* A window's size changes on the main thread, which updates the GL context
 * the renderer may be drawing with on its thread: macOS's GL faults in a blit
 * (or the window stays black) unless no frame is being drawn meanwhile. */
static void window_resize(SDL_Window *w, int width, int height)
{
#if !defined(__vita__)
    void gpu_lock(void);
    void gpu_unlock(void);
    gpu_lock();
    SDL_SetWindowSize(w, width, height);
    gpu_unlock();
#else
    SDL_SetWindowSize(w, width, height);
#endif
}
static void test_resize(void)
{   /* MINA_TEST_RESIZE="secs w h": the window is resized then (debug: the game's pixel scale option) */
    static int done;
    const char *v = getenv("MINA_TEST_RESIZE");
    int secs, w, h;
    if (done || !v || sscanf(v, "%d %d %d", &secs, &w, &h) != 3 || SDL_GetTicks() < (Uint32)secs * 1000u)
        return;
    done = 1;
    window_resize(sdl_main_window(), w, h);
    rt_log("[win] test resize to %dx%d\n", w, h);
}
/* "vita:<button>" in MINA_INPUT: held buttons of the Vita's own pad, read by
 * SDL through sceCtrlPeekBufferPositive2 (wrapped at link time): the game gets
 * them exactly as the real pad's */
static uint32_t script_buttons;
static int script_ly, script_lx;      /* left stick offset from the centre */
#if defined(__vita__)
#include <psp2/ctrl.h>
int __real_sceCtrlPeekBufferPositive2(int port, SceCtrlData *pad, int count);
int __wrap_sceCtrlPeekBufferPositive2(int port, SceCtrlData *pad, int count)
{
    const int r = __real_sceCtrlPeekBufferPositive2(port, pad, count);
    if (r > 0 && pad && port <= 1) {
        pad->buttons |= script_buttons;
        if (script_ly)
            pad->ly = (uint8_t)(128 + script_ly);
        if (script_lx)
            pad->lx = (uint8_t)(128 + script_lx);
    }
    return r;
}
#endif
static int vita_button(const char *n)
{
    static const struct { const char *name; uint32_t bit; } b[] = {
        { "select", 0x1 }, { "start", 0x8 }, { "up", 0x10 }, { "right", 0x20 }, { "down", 0x40 }, { "left", 0x80 },
        { "l", 0x100 }, { "r", 0x200 }, { "triangle", 0x1000 }, { "circle", 0x2000 }, { "cross", 0x4000 },
        { "square", 0x8000 },
        { "lup", 0x10000001 }, { "ldown", 0x10000002 }, { "lleft", 0x10000003 }, { "lright", 0x10000004 },
    };
    for (size_t i = 0; i < sizeof b / sizeof b[0]; i++)
        if (!strcmp(n, b[i].name))
            return (int)b[i].bit;
    return 0;
}

/* MINA_LEVELS: the level table Game::Init fills (names for -loadlevel), once */
static void levels_dump(void)
{
    static int done;
    if (done || !getenv("MINA_LEVELS") || SDL_GetTicks() < 15000)
        return;
    done = 1;
    const uint32_t tab = GUEST_IMAGE_BASE + 0x05044f68u;   /* Mach-O 0x105044f68: entries (72 bytes), then the count */
    const uint32_t base = (uint32_t)rt_ld64(G(tab));
    const int count = (int)(int32_t)rt_ld64(G(tab + 8));
    rt_log("[levels] %d\n", count);
    for (int i = 0; i < count && i < 400; i++) {
        const uint32_t name = (uint32_t)rt_ld64(G(base + 72u * (uint32_t)i + 0x30));
        rt_log("[levels] %d %s\n", i, name ? (const char *)G(name) : "-");
    }
}
static void scripted_input(void)
{
    test_resize();
    levels_dump();
    static Uint32 last_mem;
    if (getenv("MINA_MEMSTAT") && SDL_GetTicks() - last_mem > 5000) {
        last_mem = SDL_GetTicks();
        mem_report();
    }
    static int parsed, n, next;
    /* "pad:<button>" (SDL game controller names: a, b, x, y, start, dpdown...):
     * the first controller's button, as its events (what the Vita's pad sends) */
    static struct { Uint32 at; SDL_Scancode sc; int up; int pad; Uint32 hold; } ev[512];
    if (!parsed) {
        parsed = 1;
        const char *s = getenv("MINA_INPUT");
        while (s && *s && n < 510) {
            double t;
            char name[64];
            int len;
            if (sscanf(s, " %lf:%63[^ ]%n", &t, name, &len) != 2)
                break;
            /* "<name>/<seconds>": held that long (default 0.12) */
            Uint32 hold = 120;
            char *slash = strchr(name, '/');
            if (slash) {
                hold = (Uint32)(atof(slash + 1) * 1000);
                *slash = 0;
            }
            int pad = -1;
            SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
            if (!strncmp(name, "pad:", 4))
                pad = SDL_GameControllerGetButtonFromString(name + 4);
            else if (!strncmp(name, "vita:", 5))
                pad = 3000 + n / 2, ev[n].sc = (SDL_Scancode)vita_button(name + 5);
            else if (!strncmp(name, "joy:", 4))
                pad = 2000 + atoi(name + 4);     /* "joy:<n>": the joystick's button n */
            else if (!strncmp(name, "axis:", 5) && strlen(name) > 6) {
                /* "axis:lefty+" / "axis:leftx-": the stick pushed all the way */
                const size_t l = strlen(name);
                const int neg = name[l - 1] == '-';
                name[l - 1] = 0;
                pad = 1000 + SDL_GameControllerGetAxisFromString(name + 5) * 2 + neg;
            } else
                sc = SDL_GetScancodeFromName(name);
            if (pad >= 3000)
                sc = ev[n].sc;           /* the button's bit */
            ev[n].at = (Uint32)(t * 1000);
            ev[n].sc = sc;
            ev[n].pad = pad;
            ev[n++].up = 0;
            ev[n].at = (Uint32)(t * 1000) + hold;
            ev[n].sc = sc;
            ev[n].pad = pad;
            ev[n++].up = 1;
            s += len;
        }
    }
    {   /* MINA_INPUT_UNTIL_PLAY: no more keys once gameplay starts (after a key's release) */
        extern int g_in_play;
        static int until = -1;
        if (until < 0)
            until = getenv("MINA_INPUT_UNTIL_PLAY") != NULL;
        if (until && g_in_play && next < n && !ev[next].up) {
            rt_log("[input] gameplay: the remaining keys are dropped\n");
            next = n;
        }
    }
    while (next < n && SDL_GetTicks() >= ev[next].at) {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        if (ev[next].pad >= 3000) {
            const uint32_t bit = (uint32_t)ev[next].sc;
            const int up = ev[next].up;
            if (bit >> 28) {
                const int k = (int)(bit & 0xf), v = up ? 0 : 127;
                if (k == 1) script_ly = -v;
                if (k == 2) script_ly = v;
                if (k == 3) script_lx = -v;
                if (k == 4) script_lx = v;
            } else if (up)
                script_buttons &= ~bit;
            else
                script_buttons |= bit;
            rt_log("[input] %s vita 0x%x\n", up ? "up" : "down", bit);
            next++;
            continue;
        }
        if (ev[next].pad >= 2000) {
            e.type = ev[next].up ? SDL_JOYBUTTONUP : SDL_JOYBUTTONDOWN;
            e.jbutton.which = SDL_NumJoysticks() > 0 ? SDL_JoystickGetDeviceInstanceID(0) : 0;
            e.jbutton.button = (Uint8)(ev[next].pad - 2000);
            e.jbutton.state = ev[next].up ? SDL_RELEASED : SDL_PRESSED;
            e.jbutton.timestamp = SDL_GetTicks();
            rt_log("[input] %s joy %d\n", ev[next].up ? "up" : "down", ev[next].pad - 2000);
            SDL_PushEvent(&e);
            next++;
            continue;
        }
        if (ev[next].pad >= 1000) {
            const int axis = (ev[next].pad - 1000) / 2, neg = (ev[next].pad - 1000) & 1;
            e.type = SDL_CONTROLLERAXISMOTION;
            e.caxis.which = SDL_NumJoysticks() > 0 ? SDL_JoystickGetDeviceInstanceID(0) : 0;
            e.caxis.axis = (Uint8)axis;
            e.caxis.value = ev[next].up ? 0 : neg ? -32768 : 32767;
            e.caxis.timestamp = SDL_GetTicks();
            rt_log("[input] axis %d = %d\n", axis, e.caxis.value);
            SDL_PushEvent(&e);
            next++;
            continue;
        }
        if (ev[next].pad >= 0) {
            e.type = ev[next].up ? SDL_CONTROLLERBUTTONUP : SDL_CONTROLLERBUTTONDOWN;
            e.cbutton.which = SDL_NumJoysticks() > 0 ? SDL_JoystickGetDeviceInstanceID(0) : 0;
            e.cbutton.button = (Uint8)ev[next].pad;
            e.cbutton.state = ev[next].up ? SDL_RELEASED : SDL_PRESSED;
            e.cbutton.timestamp = SDL_GetTicks();
            rt_log("[input] %s pad %s\n", ev[next].up ? "up" : "down",
                   SDL_GameControllerGetStringForButton((SDL_GameControllerButton)ev[next].pad));
            SDL_PushEvent(&e);
            next++;
            continue;
        }
        e.type = ev[next].up ? SDL_KEYUP : SDL_KEYDOWN;
        e.key.state = ev[next].up ? SDL_RELEASED : SDL_PRESSED;
        e.key.keysym.scancode = ev[next].sc;
        e.key.keysym.sym = SDL_GetKeyFromScancode(ev[next].sc);
        e.key.windowID = windows[0].host ? SDL_GetWindowID(windows[0].host) : 0;
        e.key.timestamp = SDL_GetTicks();
        rt_log("[input] %s %s\n", ev[next].up ? "up" : "down", SDL_GetScancodeName(ev[next].sc));
        SDL_PushEvent(&e);
        next++;
    }
}

/* Main-queue blocks (dispatch_async to the main queue) run on the main thread
 * while it waits for events; posting this event wakes it. */
void mainq_drain(Ctx *c);
extern void (*g_mainq_wake)(void);
static void wake_mainq(void) { mainq_wake(); }
static void mainq_wake(void)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = wake_event;
    SDL_PushEvent(&e);
}
static void wake_init(void)
{
    if (!wake_event) {
        wake_event = SDL_RegisterEvents(1);
        g_mainq_wake = wake_mainq;
    }
}

/* SDL may run the guest's event filter on this thread (with this context):
 * every argument is read before calling into SDL. */
void nat_SDL_PollEvent(Ctx *c)
{
    uint64_t out = A0;
    SDL_Event e;
    wake_init();
    int r;
    for (;;) {
        drain_jobs();
        mainq_drain(c);
        scripted_input();
        r = SDL_PollEvent(&e);
        if (!r || e.type != wake_event)
            break;
    }
    if (r && out)
        memcpy(G(out), &e, sizeof e);
    else if (r && !out)
        SDL_PushEvent(&e);
    RET((uint64_t)(int64_t)r);
}
void nat_SDL_WaitEvent(Ctx *c)
{
    uint64_t out = A0;
    SDL_Event e;
    wake_init();
    int r;
    for (;;) {
        drain_jobs();
        mainq_drain(c);
        scripted_input();
        r = getenv("MINA_INPUT") ? SDL_WaitEventTimeout(&e, 20) : SDL_WaitEvent(&e);
        if (!r && getenv("MINA_INPUT"))
            continue;
        if (!r || e.type != wake_event)
            break;
    }
    if (r && out)
        memcpy(G(out), &e, sizeof e);
    else if (r && !out)
        SDL_PushEvent(&e);
    RET((uint64_t)(int64_t)r);
}
void nat_SDL_PeepEvents(Ctx *c)
{
    uint64_t events = A0;
    int n = (int)A1, action = (int)A2;
    Uint32 minT = (Uint32)A3, maxT = (Uint32)A4;
    if (n < 0) n = 0;
    SDL_Event *ev = n ? calloc((size_t)n, sizeof *ev) : NULL;
    if (action == SDL_ADDEVENT && n)
        memcpy(ev, G(events), (size_t)n * sizeof *ev);
    int r = SDL_PeepEvents(ev, n, (SDL_eventaction)action, minT, maxT);
    if (action != SDL_ADDEVENT && r > 0 && events)
        memcpy(G(events), ev, (size_t)r * sizeof *ev);
    free(ev);
    RET((uint64_t)(int64_t)r);
}

/* ---- game controllers ---------------------------------------------------------------------- */

void nat_SDL_NumJoysticks(Ctx *c) { RET((uint64_t)(int64_t)SDL_NumJoysticks()); }
void nat_SDL_IsGameController(Ctx *c) { RET(SDL_IsGameController((int)A0)); }
void nat_SDL_GameControllerOpen(Ctx *c)
{
    SDL_GameController *gc = SDL_GameControllerOpen((int)A0);
    RET(gc ? handle_new(H_CTRL, gc) : 0);
}
void nat_SDL_GameControllerClose(Ctx *c)
{
    SDL_GameController *gc = handle_host(A0, H_CTRL);
    if (gc) SDL_GameControllerClose(gc);
}
void nat_SDL_GameControllerGetJoystick(Ctx *c)
{
    SDL_GameController *gc = handle_host(A0, H_CTRL);
    SDL_Joystick *j = gc ? SDL_GameControllerGetJoystick(gc) : NULL;
    RET(j ? handle_new(H_JOY, j) : 0);
}
void nat_SDL_JoystickInstanceID(Ctx *c)
{
    SDL_Joystick *j = handle_host(A0, H_JOY);
    RET((uint64_t)(int64_t)(j ? SDL_JoystickInstanceID(j) : -1));
}
static uint64_t guest_name(const char *s)
{
    /* strings SDL owns: interned guest copies */
    if (!s) return 0;
    return gstrdup(s);
}
void nat_SDL_GameControllerNameForIndex(Ctx *c) { RET(guest_name(SDL_GameControllerNameForIndex((int)A0))); }
void nat_SDL_GameControllerPathForIndex(Ctx *c) { RET(guest_name(SDL_GameControllerPathForIndex((int)A0))); }
void nat_SDL_GameControllerTypeForIndex(Ctx *c) { RET((uint64_t)(int64_t)SDL_GameControllerTypeForIndex((int)A0)); }
void nat_SDL_GameControllerSetLED(Ctx *c)
{
    SDL_GameController *gc = handle_host(A0, H_CTRL);
    RET((uint64_t)(int64_t)(gc ? SDL_GameControllerSetLED(gc, (Uint8)A1, (Uint8)A2, (Uint8)A3) : -1));
}
void nat_SDL_GameControllerAddMapping(Ctx *c) { RET((uint64_t)(int64_t)SDL_GameControllerAddMapping(gstr(A0))); }

/* ---- audio ------------------------------------------------------------------------------------
 * Guest SDL_AudioSpec: freq +0, format +4, channels +6, silence +7, samples +8,
 * size +12, callback +16, userdata +24. */

static uint64_t audio_cb, audio_ud;
static uint32_t audio_buf, audio_buf_size;

static void SDLCALL host_audio(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    Ctx *c = host_thread_ctx();
    if ((uint32_t)len > audio_buf_size) {
        gfree(audio_buf);
        audio_buf = gmalloc((uint64_t)len);
        audio_buf_size = (uint32_t)len;
    }
    memset(G(audio_buf), 0, (size_t)len);
    guest_call(c, audio_cb, 3, audio_ud, (uint64_t)audio_buf, (uint64_t)len);
    memcpy(stream, G(audio_buf), (size_t)len);
    /* MINA_MEMSTAT: output level once a second (checks that music plays) */
    static double acc;
    static long nsamp;
    static Uint32 last;
    if (getenv("MINA_MEMSTAT")) {
        const float *f = (const float *)stream;
        for (int i = 0; i < len / 4; i++)
            acc += (double)f[i] * f[i];
        nsamp += len / 4;
        if (SDL_GetTicks() - last > 1000) {
            last = SDL_GetTicks();
            rt_log("[audio] rms %.4f\n", nsamp ? sqrt(acc / (double)nsamp) : 0.0);
            acc = 0;
            nsamp = 0;
        }
    }
}

static void spec_from_guest(SDL_AudioSpec *s, uint64_t g)
{
    memset(s, 0, sizeof *s);
    s->freq = (int)rt_ld32(G(g));
    s->format = (SDL_AudioFormat)rt_ld16(G(g + 4));
    s->channels = rt_ld8(G(g + 6));
    s->samples = (Uint16)rt_ld16(G(g + 8));
}
static void spec_to_guest(uint64_t g, const SDL_AudioSpec *s)
{
    rt_st32(G(g), (uint32_t)s->freq);
    rt_st16(G(g + 4), s->format);
    rt_st8(G(g + 6), s->channels);
    rt_st8(G(g + 7), s->silence);
    rt_st16(G(g + 8), s->samples);
    rt_st32(G(g + 12), s->size);
}

void nat_SDL_OpenAudioDevice(Ctx *c)
{
    SDL_AudioSpec want, have;
    spec_from_guest(&want, A2);
    audio_cb = rt_ld64(G(A2 + 16));
    audio_ud = rt_ld64(G(A2 + 24));
    want.callback = audio_cb ? host_audio : NULL;
    SDL_AudioDeviceID id = SDL_OpenAudioDevice(A0 ? gstr(A0) : NULL, (int)A1, &want, &have, (int)A4);
    if (id && A3) {
        spec_to_guest(A3, &have);
        rt_st64(G(A3 + 16), audio_cb);
        rt_st64(G(A3 + 24), audio_ud);
    }
    if (!id)
        rt_log("SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    RET(id);
}
void nat_SDL_PauseAudioDevice(Ctx *c) { SDL_PauseAudioDevice((SDL_AudioDeviceID)A0, (int)A1); }
void nat_SDL_CloseAudioDevice(Ctx *c) { SDL_CloseAudioDevice((SDL_AudioDeviceID)A0); }

/* CFRunLoopRunInMode(mode, seconds, returnAfterSourceHandled): the game's main
 * loop spins it; main-thread work and SDL events are serviced while it waits. */
static void imp_CFRunLoopRunInMode(Ctx *c)
{
    double secs = DARG(0);
    Uint32 end = SDL_GetTicks() + (Uint32)(secs > 0 ? secs * 1000.0 : 0);
    do {
        drain_jobs();
        mainq_drain(c);
        scripted_input();
        SDL_PumpEvents();
        if (secs > 0)
            SDL_Delay(1);
    } while (secs > 0 && SDL_TICKS_PASSED(end, SDL_GetTicks()) == 0);
    RET(3);   /* kCFRunLoopRunTimedOut */
}
static void imp_CFRunLoopGetCurrent(Ctx *c) { RET(1); }
static void imp_nop_sdl(Ctx *c) { (void)c; }

const ImportImpl g_impl_cf[] = {
    { "_CFRunLoopRunInMode", imp_CFRunLoopRunInMode, 0, NULL },
    { "_CFRunLoopGetCurrent", imp_CFRunLoopGetCurrent, 0, NULL },
    { "_CFRunLoopGetMain", imp_CFRunLoopGetCurrent, 0, NULL },
    { "_CFRunLoopWakeUp", imp_nop_sdl, 0, NULL },
    { "_CFRunLoopStop", imp_nop_sdl, 0, NULL },
    { NULL, NULL, 0, NULL },
};
