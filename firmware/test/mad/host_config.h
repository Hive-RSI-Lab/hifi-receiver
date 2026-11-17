/* libmad's config for the host test (forced in with -include): the board's
   config.h (third_party/libmad/src/config.h) but with this host's long. */
#define FPM_DEFAULT 1
#define SIZEOF_INT 4
#define SIZEOF_LONG __SIZEOF_LONG__
#define SIZEOF_LONG_LONG 8
#define HAVE_ASSERT_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
