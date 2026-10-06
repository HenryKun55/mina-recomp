/*
 * Minimal Objective-C runtime for the guest.
 *
 * Classes are either host classes (implemented here in C: Foundation, Metal,
 * ...) or guest classes (defined in the executable's __objc_classlist; their
 * methods are guest functions). A guest class may inherit from a host class.
 *
 * Every class has a guest "class object" address: the data import
 * _OBJC_CLASS_$_Name for host classes, the class_t for guest classes.
 * Objects are guest memory blocks starting with their isa; host state lives
 * in a side table (Obj) keyed by the object's address.
 */
#ifndef MINA_OBJC_RT_H
#define MINA_OBJC_RT_H

#include "runtime.h"

typedef struct Method {
    const char *sel;
    GuestFn fn;
} Method;

typedef struct Class Class;
typedef void (*DeallocFn)(void *host);

struct Class {
    const char *name;
    const char *super_name;
    const Method *imethods;      /* host instance methods (NULL-terminated) */
    const Method *cmethods;      /* host class methods */
    uint32_t host_size;          /* bytes of host state per instance (0: none) */
    DeallocFn dealloc;           /* frees host resources inside the state */
    GuestFn fallback;            /* handles any selector the tables miss (x1 = sel) */
    /* filled at registration */
    Class *super;
    uint32_t obj;                /* guest class object */
    uint32_t meta;               /* guest metaclass object (guest classes) */
    uint32_t instance_size;      /* guest bytes per instance */
    int guest;                   /* defined in the executable */
    uint32_t guest_imethods;     /* method_list_t of a guest class */
    uint32_t guest_cmethods;
};

typedef struct Obj {
    uint32_t addr;
    Class *cls;
    int32_t refs;
    void *host;                  /* host_size bytes, zeroed */
} Obj;

void objc_init(void);
void objc_register(Class *cls);

/* Objects */
uint32_t objc_new_object(Class *cls);          /* refcount 1 */
Obj *objc_obj(uint64_t addr);                 /* NULL if unknown / nil */
void *objc_host(uint64_t addr);               /* host state of an object */
Class *objc_class_named(const char *name);
Class *objc_class_of(uint64_t addr);
int objc_is_class_object(uint64_t addr);
uint64_t objc_retain_obj(uint64_t a);
void objc_release_obj(uint64_t a);
uint64_t objc_autorelease_obj(uint64_t a);

/* Messaging from the host */
uint32_t objc_sel(const char *name);          /* guest selector address */
uint64_t objc_send(Ctx *c, uint64_t recv, const char *sel, int nargs, ...);

/* Blocks: invoke(block, ...) */
uint64_t block_call(Ctx *c, uint64_t block, int nargs, ...);
uint64_t block_copy(Ctx *c, uint64_t block);

/* Host method helpers */
#define SELF (c->x[0])
#define MARG(i) (c->x[2 + (i)])      /* i-th explicit argument */

#endif
