/* Foundation subset: NSString, NSNumber, NSArray, NSDictionary, NSThread,
 * NSFileManager, NSURL, NSBundle, NSProcessInfo. */
#include "objc_rt.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern Class cls_NSObject;
const char *host_path(const char *p, char *buf, size_t cap);

/* ---- NSString ----------------------------------------------------------------
 * Host state: guest address of a NUL-terminated UTF-8 copy and its length.
 * Constant strings from the executable (__cfstring) are {isa, flags, chars, length}. */

typedef struct NSStr {
    uint32_t chars;
    uint32_t len;
} NSStr;

static Class cls_NSString, cls_NSConstantString, cls_NSNumber, cls_NSArray;

static void str_dealloc(void *h) { gfree(((NSStr *)h)->chars); }

/* UTF-8 contents of any string object (NULL for nil / non-strings). */
const char *ns_cstr(uint64_t s)
{
    if (!s)
        return NULL;
    Class *k = objc_class_of(s);
    if (k == &cls_NSConstantString)
        return gstr(rt_ld64(G(s + 16)));
    NSStr *h = objc_host(s);
    if (!h || !h->chars)
        return NULL;
    return gstr(h->chars);
}

uint64_t ns_string(const char *s)
{
    uint32_t o = objc_new_object(&cls_NSString);
    NSStr *h = objc_host(o);
    h->chars = gstrdup(s ? s : "");
    h->len = (uint32_t)strlen(s ? s : "");
    return o;
}

/* An autoreleased string (the usual return of convenience constructors). */
uint64_t ns_string_auto(const char *s) { return objc_autorelease_obj(ns_string(s)); }

/* printf with %@ (objects print as their string contents) */
static char *ns_format(const char *fmt, uint32_t va)
{
    /* rewrite %@ into %s with the description's guest address in the slot */
    size_t fl = strlen(fmt);
    char *f2 = malloc(fl + 1);
    memcpy(f2, fmt, fl + 1);
    uint32_t slot = va;
    for (char *p = f2; *p; p++) {
        if (*p != '%')
            continue;
        p++;
        while (*p && strchr("-+ #0123456789.*hlqjztL", *p)) {
            if (*p == '*') slot += 8;
            p++;
        }
        if (!*p)
            break;
        if (*p == '@') {
            *p = 's';
            uint64_t obj = rt_ld64(G(slot));
            const char *s = ns_cstr(obj);
            if (!s) {
                Class *k = objc_class_of(obj);
                s = k ? k->name : "(null)";
            }
            /* the slot now holds a guest string address */
            uint32_t g = gstrdup(s);
            rt_st64(G(slot), g);
        }
        if (*p != '%')
            slot += 8;
    }
    int n = gvformat(NULL, 0, f2, va);
    char *out = malloc((size_t)n + 1);
    gvformat(out, (size_t)n + 1, f2, va);
    free(f2);
    return out;
}

static void s_stringWithUTF8String(Ctx *c) { RET(MARG(0) ? ns_string_auto(gstr(MARG(0))) : 0); }
static void s_stringWithCString(Ctx *c) { RET(MARG(0) ? ns_string_auto(gstr(MARG(0))) : 0); }
static void s_string(Ctx *c) { RET(ns_string_auto("")); }
static void s_stringWithFormat(Ctx *c)
{
    char *s = ns_format(ns_cstr(MARG(0)), (uint32_t)c->sp);
    RET(ns_string_auto(s));
    free(s);
}
static void s_initWithUTF8String(Ctx *c)
{
    NSStr *h = objc_host(SELF);
    const char *s = gstr(MARG(0));
    h->chars = gstrdup(s ? s : "");
    h->len = (uint32_t)strlen(s ? s : "");
}
static void s_UTF8String(Ctx *c)
{
    if (objc_class_of(SELF) == &cls_NSConstantString)
        RET(rt_ld64(G(SELF + 16)));
    else
        RET(((NSStr *)objc_host(SELF))->chars);
}
static void s_length(Ctx *c) { RET(strlen(ns_cstr(SELF))); }
static void s_isEqualToString(Ctx *c)
{
    const char *a = ns_cstr(SELF), *b = ns_cstr(MARG(0));
    RET(a && b && strcmp(a, b) == 0);
}
static void s_hash(Ctx *c)
{
    const char *s = ns_cstr(SELF);
    uint64_t h = 1469598103934665603ull;
    while (s && *s)
        h = (h ^ (uint8_t)*s++) * 1099511628211ull;
    RET(h);
}
static void s_appendPath(Ctx *c)
{
    const char *a = ns_cstr(SELF), *b = ns_cstr(MARG(0));
    size_t la = strlen(a);
    char *r = malloc(la + strlen(b) + 2);
    strcpy(r, a);
    if (la && a[la - 1] != '/' && b[0] != '/')
        strcat(r, "/");
    strcat(r, b[0] == '/' && la && a[la - 1] == '/' ? b + 1 : b);
    RET(ns_string_auto(r));
    free(r);
}
static void s_appendString(Ctx *c)
{
    const char *a = ns_cstr(SELF), *b = ns_cstr(MARG(0));
    char *r = malloc(strlen(a) + strlen(b) + 1);
    strcpy(r, a);
    strcat(r, b);
    RET(ns_string_auto(r));
    free(r);
}
static void s_deleteLastPath(Ctx *c)
{
    char *r = strdup(ns_cstr(SELF));
    size_t n = strlen(r);
    while (n > 1 && r[n - 1] == '/')
        r[--n] = 0;
    char *slash = strrchr(r, '/');
    if (!slash)
        r[0] = 0;
    else if (slash == r)
        r[1] = 0;
    else
        *slash = 0;
    RET(ns_string_auto(r));
    free(r);
}
static void s_lastPathComponent(Ctx *c)
{
    const char *s = ns_cstr(SELF);
    const char *slash = strrchr(s, '/');
    RET(ns_string_auto(slash ? slash + 1 : s));
}
static void s_expandTilde(Ctx *c)
{
    const char *s = ns_cstr(SELF);
    if (s[0] == '~') {
        const char *home = getenv("HOME");
        char *r = malloc(strlen(home) + strlen(s) + 1);
        strcpy(r, home);
        strcat(r, s + 1);
        RET(ns_string_auto(r));
        free(r);
    }
}
static void s_intValue(Ctx *c) { RET((int64_t)atoll(ns_cstr(SELF))); }
static void s_doubleValue(Ctx *c) { RETD(atof(ns_cstr(SELF))); }
static void s_floatValue(Ctx *c) { RETF(atof(ns_cstr(SELF))); }
static void s_boolValue(Ctx *c)
{
    const char *s = ns_cstr(SELF);
    RET(s && (s[0] == 'Y' || s[0] == 'y' || s[0] == 'T' || s[0] == 't' || (s[0] >= '1' && s[0] <= '9')));
}
static void s_getCString(Ctx *c)
{
    const char *s = ns_cstr(SELF);
    size_t n = strlen(s);
    if (n + 1 > MARG(1)) {
        RET(0);
        return;
    }
    memcpy(G(MARG(0)), s, n + 1);
    RET(1);
}
static void s_description(Ctx *c) { (void)c; }

static const Method nsstring_i[] = {
    { "initWithUTF8String:", s_initWithUTF8String }, { "UTF8String", s_UTF8String },
    { "fileSystemRepresentation", s_UTF8String }, { "cStringUsingEncoding:", s_UTF8String },
    { "length", s_length }, { "isEqualToString:", s_isEqualToString }, { "isEqual:", s_isEqualToString },
    { "hash", s_hash }, { "stringByAppendingPathComponent:", s_appendPath },
    { "stringByAppendingString:", s_appendString },
    { "stringByDeletingLastPathComponent", s_deleteLastPath }, { "lastPathComponent", s_lastPathComponent },
    { "stringByExpandingTildeInPath", s_expandTilde }, { "stringByStandardizingPath", s_description },
    { "intValue", s_intValue }, { "integerValue", s_intValue }, { "longLongValue", s_intValue },
    { "doubleValue", s_doubleValue }, { "floatValue", s_floatValue }, { "boolValue", s_boolValue },
    { "getCString:maxLength:encoding:", s_getCString }, { "description", s_description },
    { "copy", s_description },
    { NULL, NULL },
};
static const Method nsstring_c[] = {
    { "stringWithUTF8String:", s_stringWithUTF8String }, { "stringWithCString:encoding:", s_stringWithCString },
    { "stringWithFormat:", s_stringWithFormat }, { "string", s_string },
    { NULL, NULL },
};
static Class cls_NSString = { "NSString", "NSObject", nsstring_i, nsstring_c, sizeof(NSStr), str_dealloc };
static Class cls_NSConstantString = { "__NSCFConstantString", "NSString", NULL, NULL, 0, NULL };

/* ---- NSNumber --------------------------------------------------------------------- */

typedef struct NSNum {
    int is_float;
    int64_t i;
    double d;
} NSNum;

uint64_t ns_number_int(int64_t v)
{
    uint32_t o = objc_new_object(&cls_NSNumber);
    NSNum *n = objc_host(o);
    n->i = v;
    n->d = (double)v;
    return objc_autorelease_obj(o);
}
static void n_numberWithInt(Ctx *c) { RET(ns_number_int((int64_t)(int32_t)MARG(0))); }
static void n_numberWithLong(Ctx *c) { RET(ns_number_int((int64_t)MARG(0))); }
static void n_numberWithBool(Ctx *c) { RET(ns_number_int(MARG(0) & 1)); }
static void n_numberWithDouble(Ctx *c)
{
    uint32_t o = objc_new_object(&cls_NSNumber);
    NSNum *n = objc_host(o);
    n->is_float = 1;
    n->d = DARG(0);
    n->i = (int64_t)n->d;
    RET(objc_autorelease_obj(o));
}
static void n_intValue(Ctx *c) { RET(((NSNum *)objc_host(SELF))->i); }
static void n_doubleValue(Ctx *c) { RETD(((NSNum *)objc_host(SELF))->d); }
static void n_floatValue(Ctx *c) { RETF(((NSNum *)objc_host(SELF))->d); }
static const Method nsnumber_i[] = {
    { "intValue", n_intValue }, { "integerValue", n_intValue }, { "longLongValue", n_intValue },
    { "unsignedIntValue", n_intValue }, { "boolValue", n_intValue }, { "doubleValue", n_doubleValue },
    { "floatValue", n_floatValue },
    { NULL, NULL },
};
static const Method nsnumber_c[] = {
    { "numberWithInt:", n_numberWithInt }, { "numberWithInteger:", n_numberWithLong },
    { "numberWithLongLong:", n_numberWithLong }, { "numberWithBool:", n_numberWithBool },
    { "numberWithDouble:", n_numberWithDouble },
    { NULL, NULL },
};
static Class cls_NSNumber = { "NSNumber", "NSObject", nsnumber_i, nsnumber_c, sizeof(NSNum), NULL };

/* ---- NSArray ------------------------------------------------------------------------ */

typedef struct NSArr {
    uint64_t *items;
    uint32_t n, cap;
} NSArr;

static void arr_dealloc(void *h)
{
    NSArr *a = h;
    for (uint32_t i = 0; i < a->n; i++)
        objc_release_obj(a->items[i]);
    free(a->items);
}
uint64_t ns_array_new(void) { return objc_new_object(&cls_NSArray); }
void ns_array_add(uint64_t arr, uint64_t item)
{
    NSArr *a = objc_host(arr);
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->items = realloc(a->items, a->cap * sizeof *a->items);
    }
    a->items[a->n++] = objc_retain_obj(item);
}
static void a_count(Ctx *c) { RET(((NSArr *)objc_host(SELF))->n); }
static void a_objectAtIndex(Ctx *c)
{
    NSArr *a = objc_host(SELF);
    RET(MARG(0) < a->n ? a->items[MARG(0)] : 0);
}
static void a_firstObject(Ctx *c)
{
    NSArr *a = objc_host(SELF);
    RET(a->n ? a->items[0] : 0);
}
static void a_lastObject(Ctx *c)
{
    NSArr *a = objc_host(SELF);
    RET(a->n ? a->items[a->n - 1] : 0);
}
static void a_addObject(Ctx *c) { ns_array_add(SELF, MARG(0)); }
static void a_array(Ctx *c) { RET(objc_autorelease_obj(ns_array_new())); }
static const Method nsarray_i[] = {
    { "count", a_count }, { "objectAtIndex:", a_objectAtIndex },
    { "objectAtIndexedSubscript:", a_objectAtIndex }, { "firstObject", a_firstObject },
    { "lastObject", a_lastObject }, { "addObject:", a_addObject },
    { NULL, NULL },
};
static const Method nsarray_c[] = { { "array", a_array }, { NULL, NULL } };
static Class cls_NSArray = { "NSArray", "NSObject", nsarray_i, nsarray_c, sizeof(NSArr), arr_dealloc };
static Class cls_NSMutableArray = { "NSMutableArray", "NSArray", NULL, NULL, 0, NULL };

/* NSSearchPathForDirectoriesInDomains(directory, domainMask, expandTilde) */
static void imp_NSSearchPathForDirectoriesInDomains(Ctx *c)
{
    const char *home = getenv("HOME");
    char path[1024];
    switch ((int)A0) {
    case 14: snprintf(path, sizeof path, "%s/Library/Application Support", home); break;   /* ApplicationSupport */
    case 13: snprintf(path, sizeof path, "%s/Library/Caches", home); break;
    case 9: snprintf(path, sizeof path, "%s/Documents", home); break;
    case 5: snprintf(path, sizeof path, "%s/Library", home); break;
    default: snprintf(path, sizeof path, "%s", home); break;
    }
    uint64_t arr = ns_array_new();
    ns_array_add(arr, ns_string_auto(path));
    RET(objc_autorelease_obj(arr));
}

/* ---- NSURL ------------------------------------------------------------------------- */

typedef struct NSUrl {
    uint64_t path;           /* NSString */
} NSUrl;
static Class cls_NSURL;
static void url_dealloc(void *h) { objc_release_obj(((NSUrl *)h)->path); }
static void u_fileURLWithPath(Ctx *c)
{
    uint32_t o = objc_new_object(&cls_NSURL);
    ((NSUrl *)objc_host(o))->path = objc_retain_obj(MARG(0));
    RET(objc_autorelease_obj(o));
}
static void u_path(Ctx *c) { RET(((NSUrl *)objc_host(SELF))->path); }
static void u_fileSystemRepresentation(Ctx *c)
{
    uint64_t p = ((NSUrl *)objc_host(SELF))->path;
    RET(gstrdup(ns_cstr(p)));
}
static const Method nsurl_i[] = {
    { "path", u_path }, { "fileSystemRepresentation", u_fileSystemRepresentation },
    { NULL, NULL },
};
static const Method nsurl_c[] = { { "fileURLWithPath:", u_fileURLWithPath }, { NULL, NULL } };
static Class cls_NSURL = { "NSURL", "NSObject", nsurl_i, nsurl_c, sizeof(NSUrl), url_dealloc };

/* ---- NSFileManager -------------------------------------------------------------------- */

static uint64_t file_manager;
static Class cls_NSFileManager;

static void fm_defaultManager(Ctx *c)
{
    if (!file_manager)
        file_manager = objc_new_object(&cls_NSFileManager);
    RET(file_manager);
}
static void fm_fileExistsAtPath(Ctx *c)
{
    char pb[1024];
    struct stat st;
    const char *p = ns_cstr(MARG(0));
    int ok = p && stat(host_path(p, pb, sizeof pb), &st) == 0;
    RET(ok);
}
static void fm_fileExistsAtPath_isDirectory(Ctx *c)
{
    char pb[1024];
    struct stat st;
    const char *p = ns_cstr(MARG(0));
    int ok = p && stat(host_path(p, pb, sizeof pb), &st) == 0;
    if (MARG(1))
        rt_st8(G(MARG(1)), ok && S_ISDIR(st.st_mode));
    RET(ok);
}
static int mkdir_p(const char *path)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    return mkdir(buf, 0755) == 0 || errno == EEXIST;
}
static void fm_createDirectoryAtURL(Ctx *c)
{
    uint64_t url = MARG(0);
    const char *p = ns_cstr(((NSUrl *)objc_host(url))->path);
    char pb[1024];
    RET(p ? mkdir_p(host_path(p, pb, sizeof pb)) : 0);
}
static void fm_createDirectoryAtPath(Ctx *c)
{
    const char *p = ns_cstr(MARG(0));
    char pb[1024];
    RET(p ? mkdir_p(host_path(p, pb, sizeof pb)) : 0);
}
static const Method nsfm_i[] = {
    { "fileExistsAtPath:", fm_fileExistsAtPath },
    { "fileExistsAtPath:isDirectory:", fm_fileExistsAtPath_isDirectory },
    { "createDirectoryAtURL:withIntermediateDirectories:attributes:error:", fm_createDirectoryAtURL },
    { "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", fm_createDirectoryAtPath },
    { NULL, NULL },
};
static const Method nsfm_c[] = { { "defaultManager", fm_defaultManager }, { NULL, NULL } };
static Class cls_NSFileManager = { "NSFileManager", "NSObject", nsfm_i, nsfm_c, 0, NULL };

/* ---- NSBundle / NSProcessInfo ------------------------------------------------------------ */

const char *g_bundle_path = "gamedata/app";
static uint64_t main_bundle, process_info;
static Class cls_NSBundle, cls_NSProcessInfo;

static void b_mainBundle(Ctx *c)
{
    if (!main_bundle)
        main_bundle = objc_new_object(&cls_NSBundle);
    RET(main_bundle);
}
static void b_bundlePath(Ctx *c) { RET(ns_string_auto(g_bundle_path)); }
static void b_resourcePath(Ctx *c)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/Contents/Resources", g_bundle_path);
    RET(ns_string_auto(p));
}
static void b_objectForInfoDictionaryKey(Ctx *c)
{
    const char *k = ns_cstr(MARG(0));
    if (k && (strcmp(k, "CFBundleShortVersionString") == 0 || strcmp(k, "CFBundleVersion") == 0))
        RET(ns_string_auto("1.0"));
    else if (k && strcmp(k, "CFBundleName") == 0)
        RET(ns_string_auto("Mina the Hollower"));
    else
        RET(0);
}
static const Method nsbundle_i[] = {
    { "bundlePath", b_bundlePath }, { "resourcePath", b_resourcePath },
    { "objectForInfoDictionaryKey:", b_objectForInfoDictionaryKey },
    { NULL, NULL },
};
static const Method nsbundle_c[] = { { "mainBundle", b_mainBundle }, { NULL, NULL } };
static Class cls_NSBundle = { "NSBundle", "NSObject", nsbundle_i, nsbundle_c, 0, NULL };

static void p_processInfo(Ctx *c)
{
    if (!process_info)
        process_info = objc_new_object(&cls_NSProcessInfo);
    RET(process_info);
}
static void p_processName(Ctx *c) { RET(ns_string_auto("MinaTheHollower")); }
static void p_processorCount(Ctx *c) { RET(4); }
static void p_systemUptime(Ctx *c)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    RETD((double)ts.tv_sec + ts.tv_nsec / 1e9);
}
static const Method nspi_i[] = {
    { "processName", p_processName }, { "processorCount", p_processorCount },
    { "activeProcessorCount", p_processorCount }, { "systemUptime", p_systemUptime },
    { NULL, NULL },
};
static const Method nspi_c[] = { { "processInfo", p_processInfo }, { NULL, NULL } };
static Class cls_NSProcessInfo = { "NSProcessInfo", "NSObject", nspi_i, nspi_c, 0, NULL };

/* ---- NSThread ------------------------------------------------------------------------------ */

typedef struct NSThr {
    uint64_t stack_size;
    int started, finished;
} NSThr;
static Class cls_NSThread;

static void thread_body(Ctx *c, void *arg)
{
    uint64_t self = (uint64_t)(uintptr_t)arg;
    objc_send(c, self, "main", 0);
    NSThr *t = objc_host(self);
    __atomic_store_n(&t->finished, 1, __ATOMIC_SEQ_CST);
    objc_release_obj(self);
}
static void t_setStackSize(Ctx *c) { ((NSThr *)objc_host(SELF))->stack_size = MARG(0); }
static void t_start(Ctx *c)
{
    NSThr *t = objc_host(SELF);
    t->started = 1;
    objc_retain_obj(SELF);
    thread_spawn(thread_body, (void *)(uintptr_t)SELF, t->stack_size ? (uint32_t)t->stack_size : 512 * 1024);
}
static void t_isFinished(Ctx *c) { RET(__atomic_load_n(&((NSThr *)objc_host(SELF))->finished, __ATOMIC_SEQ_CST)); }
static void t_isExecuting(Ctx *c)
{
    NSThr *t = objc_host(SELF);
    RET(t->started && !__atomic_load_n(&t->finished, __ATOMIC_SEQ_CST));
}
static void t_nop(Ctx *c) { (void)c; }
static void t_isMainThread(Ctx *c) { RET(thread_is_main(c)); }
static void t_sleepForTimeInterval(Ctx *c)
{
    double s = DARG(0);
    if (s > 0) {
        struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
        nanosleep(&ts, NULL);
    }
}
static const Method nsthread_i[] = {
    { "setStackSize:", t_setStackSize }, { "start", t_start }, { "isFinished", t_isFinished },
    { "isExecuting", t_isExecuting }, { "setQualityOfService:", t_nop }, { "setName:", t_nop },
    { "setThreadPriority:", t_nop }, { "isMainThread", t_isMainThread },
    { NULL, NULL },
};
static const Method nsthread_c[] = {
    { "isMainThread", t_isMainThread }, { "sleepForTimeInterval:", t_sleepForTimeInterval },
    { NULL, NULL },
};
static Class cls_NSThread = { "NSThread", "NSObject", nsthread_i, nsthread_c, sizeof(NSThr), NULL };

/* ---- registration ------------------------------------------------------------------------- */

Class *const g_classes_foundation[] = {
    &cls_NSObject, &cls_NSString, &cls_NSConstantString, &cls_NSNumber, &cls_NSArray,
    &cls_NSMutableArray, &cls_NSURL, &cls_NSFileManager, &cls_NSBundle, &cls_NSProcessInfo,
    &cls_NSThread, NULL,
};

/* Constant strings point at ___CFConstantStringClassReference: that block is
 * the class object of __NSCFConstantString. */
uint32_t foundation_constant_string_class(void) { return import_data("___CFConstantStringClassReference"); }

const ImportImpl g_impl_foundation[] = {
    { "_NSSearchPathForDirectoriesInDomains", imp_NSSearchPathForDirectoriesInDomains, 0, NULL },
    { NULL, NULL, 0, NULL },
};
