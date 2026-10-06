/*
 * Runtime internals: guest memory, loader, dispatch, imports, threads.
 */
#ifndef MINA_RUNTIME_H
#define MINA_RUNTIME_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp.h"

/* ---- guest memory ------------------------------------------------------ */

void mem_init(void);
/* Guest heap (thread-safe). Addresses are guest addresses; 0 on failure. */
uint32_t gmalloc(uint64_t size);
uint32_t gcalloc(uint64_t n, uint64_t size);
uint32_t grealloc(uint32_t a, uint64_t size);
uint32_t gmemalign(uint64_t align, uint64_t size);
void gfree(uint32_t a);
uint64_t gmalloc_usable(uint32_t a);
/* Runtime area (never freed): import thunks' data, TLS keys, small tables. */
uint32_t rt_alloc(uint32_t size, uint32_t align);
/* A NUL-terminated copy of a host string in guest memory (gmalloc'ed). */
uint32_t gstrdup(const char *s);

static inline uint32_t guest_addr(const void *p) { return (uint32_t)((uintptr_t)p - (uintptr_t)g_mem); }
static inline const char *gstr(uint64_t a) { return a ? (const char *)G(a) : NULL; }

/* ---- loader ---------------------------------------------------------- */

/* Loads the arm64 slice of the executable at `path`, applies rebases and binds. */
void loader_load(const char *path);

/* ---- dispatch ---------------------------------------------------------- */

void cpu_init(void);
GuestFn cpu_lookup(uint32_t addr);
/* Calls guest function at addr with x0..x3 and returns x0 (a fresh frame on
 * c's stack). */
uint64_t guest_call(Ctx *c, uint64_t addr, int nargs, ...);

/* ---- imports ----------------------------------------------------------- */

typedef struct ImportImpl {
    const char *name;
    GuestFn fn;              /* function import */
    uint32_t data_size;      /* data import: size of the block (0 = function) */
    void (*init)(uint32_t addr);   /* data import: initializer */
} ImportImpl;

void imports_init(void);
/* Guest address the binder gives import i. */
uint32_t import_addr(uint32_t i);
/* Guest address of a data import by name (0 if not imported). */
uint32_t import_data(const char *name);
/* Index of an import by name (-1 if not imported). */
int import_index(const char *name);

/* Registration (each module provides a table, see imports.c). */
extern const ImportImpl g_impl_libc[];
extern const ImportImpl g_impl_pthread[];
extern const ImportImpl g_impl_objc[];
extern const ImportImpl g_impl_cf[];
extern const ImportImpl g_impl_foundation[];
extern const ImportImpl g_impl_metal[];
extern const ImportImpl g_impl_misc[];

/* Arguments / results for import implementations. */
#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define A5 (c->x[5])
#define A6 (c->x[6])
#define A7 (c->x[7])
#define RET(val) (c->x[0] = (uint64_t)(val))
#define RETF(val) (c->v[0] = vset_f32((float)(val)))
#define RETD(val) (c->v[0] = vset_f64((double)(val)))
#define FARG(i) (c->v[i].f[0])
#define DARG(i) (c->v[i].df[0])

/* Variadic arguments: on Apple arm64 every variadic argument is in its own
 * 8-byte stack slot starting at sp. */
typedef struct GVaList {
    uint32_t next;           /* guest address of the next slot */
} GVaList;

/* printf-family on guest format string and guest variadic area. */
int gvformat(char *out, size_t cap, const char *fmt, uint32_t va);

/* ---- threads ----------------------------------------------------------- */

void threads_init_main(Ctx *c);
/* Runs fn(ctx, arg) on a new guest thread; returns its pthread_t. */
uint32_t thread_spawn(void (*fn)(Ctx *, void *), void *arg, uint32_t stack_size);
int thread_is_main(Ctx *c);
Ctx *host_thread_ctx(void);
Ctx *thread_ctx(void);
uint32_t tls_block(Ctx *c);

/* ---- logging ------------------------------------------------------------ */

extern int g_trace_imports;
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#endif
