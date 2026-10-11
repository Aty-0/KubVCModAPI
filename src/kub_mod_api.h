/*
 * KubVC mod API (shared between the application and mod libraries).
 *
 * A mod is a dynamic library (.dll / .so) that defines custom math functions
 * and constants. This header is the ONLY thing a mod needs to include.
 *
 * Design rules (why this is plain C):
 *   - No C++ types cross the library boundary: no std::string, no virtual
 *     classes, no exceptions, no ownership transfer. The library may be built
 *     with a different compiler / CRT / C++ standard than the application.
 *   - Everything is versioned by `api_version` and `struct_size`, so an old mod
 *     keeps working with a newer application and vice versa.
 *   - The mod never links against the application. The application passes a
 *     table of callbacks (kub_host_api) to the mod instead.
 *
 * Lifecycle (all on the main thread, once, at application start):
 *
 *     kub_mod_query()                 host reads the mod's identity and API version
 *     kub_mod_register(host, self)    mod registers its functions / constants
 *     ... application runs, registered function pointers are called from
 *         several worker threads at once ...
 *     kub_mod_unload(self)            optional, at application shutdown
 *
 * The registry is closed after all mods are registered. Registering later
 * returns KUB_ERR_REGISTRY_CLOSED. Mods are never unloaded while the
 * application is running.
 */
#ifndef KUB_MOD_API_H
#define KUB_MOD_API_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Platform / export macros                                                  */
/* ------------------------------------------------------------------------- */
#ifdef __cplusplus
    #define KUB_EXTERN_C extern "C"
#else
    #define KUB_EXTERN_C
#endif

#if defined(_WIN32)
    #define KUB_CALL __cdecl
    #define KUB_MOD_EXPORT KUB_EXTERN_C __declspec(dllexport)
#else
    #define KUB_CALL
    #define KUB_MOD_EXPORT KUB_EXTERN_C __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Versioning                                                                */
/* ------------------------------------------------------------------------- */
#define KUB_MAKE_API_VERSION(major, minor) ((((uint32_t)(major)) << 16) | ((uint32_t)(minor) & 0xFFFFu))
#define KUB_API_VERSION_MAJOR(v)           ((uint32_t)(v) >> 16)
#define KUB_API_VERSION_MINOR(v)           ((uint32_t)(v) & 0xFFFFu)

/* Current API version.
 * Compatibility rule enforced by the host:
 *   mod.major == host.major  AND  mod.minor <= host.minor
 * Minor versions only append fields to the end of structs and add new flags. */
#define KUB_MOD_API_VERSION KUB_MAKE_API_VERSION(1, 0)

/* Names of the symbols the host looks up in the library. */
#define KUB_MOD_QUERY_SYMBOL    "kub_mod_query"
#define KUB_MOD_REGISTER_SYMBOL "kub_mod_register"
#define KUB_MOD_UNLOAD_SYMBOL   "kub_mod_unload"

/* ------------------------------------------------------------------------- */
/* Basic types                                                               */
/* ------------------------------------------------------------------------- */

/* Complex number, layout-compatible with std::complex<double> and C99 double _Complex. */
typedef struct kub_complex {
    double re;
    double im;
} kub_complex;

/* Opaque handle identifying one mod. Owned by the host. Pass it back to every
 * host callback. Never dereference it. */
typedef struct kub_mod_context* kub_mod_handle;

/* Result codes. A fixed-width integer, so the size doesn't depend on the compiler. */
typedef int32_t kub_result;
enum {
    KUB_OK                       = 0,
    KUB_ERR_INVALID_ARGUMENT     = 1,  /* null pointer, struct_size too small, both fn pointers null, ... */
    KUB_ERR_INVALID_NAME         = 2,  /* name doesn't follow the naming rules (see below) */
    KUB_ERR_DUPLICATE            = 3,  /* name is already used by the application or another mod */
    KUB_ERR_REGISTRY_CLOSED      = 4,  /* called outside kub_mod_register() */
    KUB_ERR_UNSUPPORTED          = 5,  /* feature / arity / API version is not supported by the host */
    KUB_ERR_FAILED               = 6   /* generic failure, returned by the mod itself */
};

/* Log levels for kub_host_api::log. There is intentionally no "fatal":
 * a mod must not be able to terminate the application through the logger. */
enum {
    KUB_LOG_DEBUG   = 0,
    KUB_LOG_WARNING = 1,
    KUB_LOG_ERROR   = 2
};

/* ------------------------------------------------------------------------- */
/* Naming rules (checked by the host, violations return KUB_ERR_INVALID_NAME) */
/* ------------------------------------------------------------------------- */
/*  - Regex: [A-Za-z][A-Za-z0-9]*, at least 2 and at most 32 characters.
 *      A single letter is always parsed as a variable (x, y, z, w, a, b, ...).
 *  - Function and constant names share ONE namespace and are compared
 *      case-insensitively, because the parser lower-cases function names.
 *      "Sinc" and "sinc" are the same name. Constants keep the spelling you
 *      registered ("invPi" is displayed and inserted as written).
 *  - Names that already exist in the application or in a previously loaded
 *      mod are rejected (KUB_ERR_DUPLICATE). Mods are loaded in alphabetical
 *      order of their file path, so the first one wins.
 */

/* ------------------------------------------------------------------------- */
/* Function / constant contract                                              */
/* ------------------------------------------------------------------------- */
/* A registered function is called very often (tens of thousands of times for a
 * single redraw) and from SEVERAL THREADS AT THE SAME TIME. Therefore:
 *   - It must be thread-safe: no unsynchronised global / static mutable state.
 *   - It must be fast and non-blocking: no I/O, no locks, no allocation if possible.
 *   - It must not call back into the host, must not throw C++ exceptions and
 *     must not longjmp out. Return NaN for arguments outside the domain.
 *   - It must be deterministic for the same argument. If it is not (random
 *     numbers, time), set KUB_FUNCTION_IMPURE: the root finder used for
 *     implicit equations assumes continuity and may give garbage otherwise.
 */
typedef double      (KUB_CALL *kub_real_fn)(double x);
typedef kub_complex (KUB_CALL *kub_complex_fn)(kub_complex z);

enum {
    KUB_FUNCTION_IMPURE = 1u << 0   /* result is not a pure function of the argument */
};

/* Describes one function. Zero the struct first, then set struct_size. */
typedef struct kub_function_desc {
    uint32_t       struct_size;   /* sizeof(kub_function_desc) as seen by the mod */
    uint32_t       flags;         /* KUB_FUNCTION_* */
    const char*    name;          /* required, see naming rules. Copied by the host. */
    const char*    description;   /* optional (may be NULL). Shown as a tooltip. Copied by the host. */
    uint32_t       arity;         /* number of arguments; 0 means 1. Only 1 is supported in API 1.0 */
    uint32_t       reserved0;     /* must be 0 */
    kub_real_fn    real_fn;       /* used in Real mode.    NULL = function is not available in Real mode */
    kub_complex_fn complex_fn;    /* used in Complex mode. NULL = function is not available in Complex mode */
    /* At least one of real_fn / complex_fn must be non-NULL. */
} kub_function_desc;

/* Describes one constant (real value, in both modes). */
typedef struct kub_constant_desc {
    uint32_t    struct_size;      /* sizeof(kub_constant_desc) */
    uint32_t    flags;            /* must be 0 */
    const char* name;             /* required */
    const char* description;      /* optional */
    double      value;
} kub_constant_desc;

/* Minimal acceptable struct_size for API 1.0 (a mod built against a newer API may pass more). */
#define KUB_STRUCT_END(type, field) (offsetof(type, field) + sizeof(((type*)0)->field))
#define KUB_FUNCTION_DESC_SIZE_1_0  KUB_STRUCT_END(kub_function_desc, complex_fn)
#define KUB_CONSTANT_DESC_SIZE_1_0  KUB_STRUCT_END(kub_constant_desc, value)

/* ------------------------------------------------------------------------- */
/* Mod identity                                                              */
/* ------------------------------------------------------------------------- */
typedef struct kub_mod_info {
    uint32_t    struct_size;      /* sizeof(kub_mod_info) */
    uint32_t    api_version;      /* KUB_MOD_API_VERSION the mod was built against */
    const char* id;               /* required: unique, [a-z0-9_], 2..32 chars. Used in logs and in the UI */
    const char* name;             /* required: display name */
    const char* version;          /* required: free-form, e.g. "1.2.0" */
    const char* author;           /* optional */
    const char* description;      /* optional */
} kub_mod_info;

/* ------------------------------------------------------------------------- */
/* What the host gives to the mod                                            */
/* ------------------------------------------------------------------------- */
typedef struct kub_host_api {
    uint32_t struct_size;         /* sizeof(kub_host_api) as seen by the host. Check before using newer fields. */
    uint32_t api_version;         /* KUB_MOD_API_VERSION of the host */

    /* Write a message to the application log. `message` is a plain string (not a format string). */
    void (KUB_CALL *log)(kub_mod_handle mod, int32_t level, const char* message);

    /* Registration. Only valid inside kub_mod_register(). The descriptor and the
     * strings in it only need to live until the call returns. */
    kub_result (KUB_CALL *register_function)(kub_mod_handle mod, const kub_function_desc* desc);
    kub_result (KUB_CALL *register_constant)(kub_mod_handle mod, const kub_constant_desc* desc);
} kub_host_api;

/* ------------------------------------------------------------------------- */
/* What the mod exports (the host looks them up by the *_SYMBOL names)       */
/* ------------------------------------------------------------------------- */
/* required: return a pointer to a static kub_mod_info. Must not do anything else:
 * the host calls it first to decide whether to run the mod at all. */
typedef const kub_mod_info* (KUB_CALL *kub_mod_query_fn)(void);

/* required: register everything. Return KUB_OK on success. If it returns anything
 * else, everything this mod has registered so far is rolled back and the
 * library is unloaded. */
typedef kub_result (KUB_CALL *kub_mod_register_fn)(const kub_host_api* host, kub_mod_handle self);

/* optional: called once at application shutdown, after all workers have stopped.
 * Free whatever you allocated in kub_mod_register. */
typedef void (KUB_CALL *kub_mod_unload_fn)(kub_mod_handle self);

/* ------------------------------------------------------------------------- */
/* Convenience helpers for mod authors (header-only, optional)               */
/* ------------------------------------------------------------------------- */

/* Register a function with a real and/or complex implementation (either may be NULL). */
static inline kub_result kub_register_function(const kub_host_api* host, kub_mod_handle self,
                                               const char* name, const char* description,
                                               kub_real_fn real_fn, kub_complex_fn complex_fn) {
    kub_function_desc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = (uint32_t)sizeof(d);
    d.name = name;
    d.description = description;
    d.arity = 1;
    d.real_fn = real_fn;
    d.complex_fn = complex_fn;
    return host->register_function(self, &d);
}

static inline kub_result kub_register_constant(const kub_host_api* host, kub_mod_handle self,
                                               const char* name, const char* description, double value) {
    kub_constant_desc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = (uint32_t)sizeof(d);
    d.name = name;
    d.description = description;
    d.value = value;
    return host->register_constant(self, &d);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KUB_MOD_API_H */