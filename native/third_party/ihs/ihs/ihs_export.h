
#ifndef IHS_EXPORT_H
#define IHS_EXPORT_H

#ifdef IHS_STATIC_DEFINE
#  define IHS_EXPORT
#  define IHS_NO_EXPORT
#else
#  ifndef IHS_EXPORT
#    ifdef ihs_shared_EXPORTS
        /* We are building this library */
#      define IHS_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define IHS_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef IHS_NO_EXPORT
#    define IHS_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef IHS_DEPRECATED
#  define IHS_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef IHS_DEPRECATED_EXPORT
#  define IHS_DEPRECATED_EXPORT IHS_EXPORT IHS_DEPRECATED
#endif

#ifndef IHS_DEPRECATED_NO_EXPORT
#  define IHS_DEPRECATED_NO_EXPORT IHS_NO_EXPORT IHS_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef IHS_NO_DEPRECATED
#    define IHS_NO_DEPRECATED
#  endif
#endif

#endif /* IHS_EXPORT_H */
