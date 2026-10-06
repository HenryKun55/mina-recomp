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
    int r = SDL_Init(flags | SDL_INIT_VIDEO | SDL_INIT_EVENTS);
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
    SDL_Window *w = SDL_CreateWindow(gstr(A0), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     (int)A3, (int)A4, flags);
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
void nat_SDL_SetWindowSize(Ctx *c) { ON_MAIN(nat_SDL_SetWindowSize); if (g_trace_imports) rt_log("[win] SetWindowSize(%#llx %lld %lld %lld %lld %#llx)\n", (unsigned long long)A0, (long long)A1, (long long)A2, (long long)A3, (long long)A4, (unsigned long long)A5); SDL_SetWindowSize(window_host(A0), (int)A1, (int)A2); }
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
    static __thread uint32_t scratch;
    if (!scratch)
        scratch = rt_alloc(64, 16);
    memcpy(G(scratch), e, sizeof *e);
    return scratch;
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
static void scripted_input(void)
{
    static Uint32 last_mem;
    if (getenv("MINA_MEMSTAT") && SDL_GetTicks() - last_mem > 5000) {
        last_mem = SDL_GetTicks();
        mem_report();
    }
    static int parsed, n, next;
    static struct { Uint32 at; SDL_Scancode sc; int up; } ev[256];
    if (!parsed) {
        parsed = 1;
        const char *s = getenv("MINA_INPUT");
        while (s && *s && n < 254) {
            double t;
            char name[64];
            int len;
            if (sscanf(s, " %lf:%63[^ ]%n", &t, name, &len) != 2)
                break;
            SDL_Scancode sc = SDL_GetScancodeFromName(name);
            ev[n].at = (Uint32)(t * 1000);
            ev[n].sc = sc;
            ev[n++].up = 0;
            ev[n].at = (Uint32)(t * 1000) + 120;
            ev[n].sc = sc;
            ev[n++].up = 1;
            s += len;
        }
    }
    while (next < n && SDL_GetTicks() >= ev[next].at) {
        SDL_Event e;
        memset(&e, 0, sizeof e);
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
