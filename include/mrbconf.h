/*
** mrbconf.h - mruby core configuration
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBYCONF_H
#define MRUBYCONF_H

/* architecture selection: */
/* specify -DMRB_32BIT or -DMRB_64BIT to override */
#if !defined(MRB_32BIT) && !defined(MRB_64BIT)
#if UINT64_MAX == SIZE_MAX
#define MRB_64BIT
#else
#define MRB_32BIT
#endif
#endif

#if defined(MRB_32BIT) && defined(MRB_64BIT)
#error Cannot build for 32 and 64-bit architecture at the same time
#endif

/* configuration options: */
/* add -DMRB_USE_FLOAT32 to use float instead of double for floating-point numbers */
//#define MRB_USE_FLOAT32

/* exclude floating-point numbers */
//#define MRB_NO_FLOAT

/* obsolete configuration */
#if defined(MRB_USE_FLOAT)
# define MRB_USE_FLOAT32
#endif

/* obsolete configuration */
#if defined(MRB_WITHOUT_FLOAT)
# define MRB_NO_FLOAT
#endif

#if defined(MRB_USE_FLOAT32) && defined(MRB_NO_FLOAT)
#error Cannot define MRB_USE_FLOAT32 and MRB_NO_FLOAT at the same time
#endif

/* define on big endian machines; used by MRB_NAN_BOXING, etc. */
#ifndef MRB_ENDIAN_BIG
# if (defined(BYTE_ORDER) && defined(BIG_ENDIAN) && BYTE_ORDER == BIG_ENDIAN) || \
     (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#  define MRB_ENDIAN_BIG
# endif
#endif

/* represent mrb_value in boxed double; conflict with MRB_USE_FLOAT32 and MRB_NO_FLOAT */
//#define MRB_NAN_BOXING

/* represent mrb_value as a word (natural unit of data for the processor) */
//#define MRB_WORD_BOXING

/* represent mrb_value as a struct; occupies 2 words */
//#define MRB_NO_BOXING

/* if no specific boxing type is chosen */
#if !defined(MRB_NAN_BOXING) && !defined(MRB_WORD_BOXING) && !defined(MRB_NO_BOXING)
# define MRB_WORD_BOXING
#endif

/* if defined mruby does not inline float values in word boxing;
   all floats are heap-allocated as RFloat objects */
//#define MRB_WORDBOX_NO_INLINE_FLOAT

/* obsolete configuration */
#if defined(MRB_WORDBOX_NO_FLOAT_TRUNCATE)
# define MRB_WORDBOX_NO_INLINE_FLOAT
#endif

/* add -DMRB_INT32 to use 32-bit integer for mrb_int; conflict with MRB_INT64;
   Default for 32-bit CPU mode. */
//#define MRB_INT32

/* add -DMRB_INT64 to use 64-bit integer for mrb_int; conflict with MRB_INT32;
   Default for 64-bit CPU mode (unless using MRB_NAN_BOXING). */
//#define MRB_INT64

/* if no specific integer type is chosen */
#if !defined(MRB_INT32) && !defined(MRB_INT64)
# if defined(MRB_64BIT) && !defined(MRB_NAN_BOXING)
/* Use 64-bit integers on 64-bit architecture (without MRB_NAN_BOXING) */
#  define MRB_INT64
# else
/* Otherwise use 32-bit integers */
#  define MRB_INT32
# endif
#endif

/* MRB_INT64 on 32-bit with word/NaN boxing causes alignment issues
   for heap-allocated RInteger (int64_t needs 8-byte alignment but
   GC heap slots may not guarantee it); use MRB_NO_BOXING instead */
#if defined(MRB_INT64) && defined(MRB_32BIT) && !defined(MRB_NO_BOXING)
#error "MRB_INT64 on 32-bit requires MRB_NO_BOXING"
#endif

/* call malloc_trim(0) from mrb_full_gc() */
//#define MRB_USE_MALLOC_TRIM

/* string class to handle UTF-8 encoding */
//#define MRB_UTF8_STRING

/* maximum length of strings */
/* the default value is 1MB */
/* set this value to zero to skip the check */
//#define MRB_STR_LENGTH_MAX 1048576

/* maximum length of arrays */
/* the default value is 2**17 entries */
/* set this value to zero to skip the check */
//#define MRB_ARY_LENGTH_MAX 131072

/* argv max size in mrb_funcall */
//#define MRB_FUNCALL_ARGC_MAX 16

/* number of object per heap page */
//#define MRB_HEAP_PAGE_SIZE 1024

/* define if your platform does not support etext, edata */
//#define MRB_NO_DEFAULT_RO_DATA_P

/* define if your platform supports etext, edata */
//#define MRB_USE_RO_DATA_P_ETEXT
/* use MRB_USE_ETEXT_RO_DATA_P by default on Linux */
#if (defined(__linux__) && !defined(__KERNEL__))
#define MRB_USE_ETEXT_RO_DATA_P
#endif

/* you can provide and use mrb_ro_data_p() for your platform.
   prototype is `mrb_bool mrb_ro_data_p(const char *ptr)` */
//#define MRB_USE_CUSTOM_RO_DATA_P

/* turn off generational GC by default */
//#define MRB_GC_TURN_OFF_GENERATIONAL

/* initial size of khash table bucket */
//#define KHASH_INITIAL_SIZE 32

/* allocated memory address alignment */
//#define POOL_ALIGNMENT 4

/* page size of memory pool */
//#define POOL_PAGE_SIZE 16000

/* arena size */
//#define MRB_GC_ARENA_SIZE 100

/* fixed size GC arena */
//#define MRB_GC_FIXED_ARENA

/* state atexit stack size */
//#define MRB_FIXED_STATE_ATEXIT_STACK_SIZE 5

/* fixed size state atexit stack */
//#define MRB_FIXED_STATE_ATEXIT_STACK

/* -DMRB_NO_XXXX to drop following features */
//#define MRB_NO_STDIO /* use of stdio */

/* -DMRB_USE_XXXX to enable following features */
//#define MRB_USE_DEBUG_HOOK /* hooks for debugger */
//#define MRB_USE_ALL_SYMBOLS /* Symbol.all_symbols */

/* Symbol table configuration */
/* Threshold for switching from linear search to hash table */
#ifndef MRB_SYMBOL_LINEAR_THRESHOLD
#define MRB_SYMBOL_LINEAR_THRESHOLD 256
#endif

/* Maximum number of dynamic symbols (created at runtime via to_sym etc.)
   Presyms, inline symbols, and mrb_intern_static symbols are excluded.
   Set to 0 to disable the limit. */
#ifndef MRB_SYMBOL_MAX
#define MRB_SYMBOL_MAX 4096
#endif

/* Frozen string cache configuration */
/* Upper bound on the entries the cache of frozen strings holds. It is what
   `String#-@` answers out of, and where a `String` key stored in a `Hash`
   comes from. An entry is one
   pointer and nothing else, and the cache holds no string of its own, so what
   a build spends on it never passes MRB_FSTRING_CACHE_MAX * sizeof(void*)
   bytes -- 2KB on a 64-bit target at the default, 1KB on a 32-bit one. The
   table is allocated the first time one of those paths reaches it and grows
   toward the bound only as strings collide in it, so a program that shares a
   handful of strings pays for a handful.

   Reaching the bound costs sharing, not correctness: an insertion into a full
   row drops an entry, and the string it named is shared again the next time
   it is asked for. Set to 0 to build without the cache, where each of those
   paths answers with a frozen copy of its own. */
#ifndef MRB_FSTRING_CACHE_MAX
#define MRB_FSTRING_CACHE_MAX 256
#endif

/* Define to leave the string literals of loaded code uninterned.

   Every literal of every irep is otherwise interned as the irep is loaded,
   which is what CRuby's compiler does with the literals it compiles, and what
   makes the source answer for the bytes it spells: `-s` on a string the
   program built with the bytes of a literal answers with the literal. Those
   strings are held rather than cached -- a weak slot would let go of a
   literal the program is not holding -- so the cost is one string to a
   distinct literal, over the life of the state, and a build that would
   rather answer with whichever string asked first says so here. What a
   literal costs is its header alone where the bytes are the program's own
   read-only data, which is where compiled-in code stands. */
/* #define MRB_NO_FSTRING_LITERALS */

/* obsolete configurations */
#if defined(DISABLE_STDIO) || defined(MRB_DISABLE_STDIO)
# define MRB_NO_STDIO
#endif
#if defined(MRB_DISABLE_DIRECT_THREADING) || defined(MRB_NO_DIRECT_THREADING)
# define MRB_USE_VM_SWITCH_DISPATCH
#endif
#if defined(ENABLE_DEBUG) || defined(MRB_ENABLE_DEBUG_HOOK)
# define MRB_USE_DEBUG_HOOK
#endif
#ifdef MRB_ENABLE_ALL_SYMBOLS
# define MRB_USE_ALL_SYMBOLS
#endif
#ifdef MRB_ENABLE_CXX_ABI
# define MRB_USE_CXX_ABI
#endif
#ifdef MRB_ENABLE_CXX_EXCEPTION
# define MRB_USE_CXX_EXCEPTION
#endif

/* end of configuration */

#ifndef MRB_NO_STDIO
# include <stdio.h>
#endif

/*
** mruby tuning profiles
**/

/* A profile for micro controllers */
#if defined(MRB_CONSTRAINED_BASELINE_PROFILE)
# ifndef MRB_NO_METHOD_CACHE
#  define MRB_NO_METHOD_CACHE
# endif

# ifndef KHASH_INITIAL_SIZE
#  define KHASH_INITIAL_SIZE 16
# endif

# ifndef MRB_HEAP_PAGE_SIZE
#  define MRB_HEAP_PAGE_SIZE 256
# endif

/* A profile for default mruby */
#elif defined(MRB_BASELINE_PROFILE)

/* A profile for desktop computers or workstations; rich memory! */
#elif defined(MRB_MAIN_PROFILE)
# ifndef MRB_METHOD_CACHE_SIZE
#  define MRB_METHOD_CACHE_SIZE (1<<10)
# endif

# ifndef MRB_HEAP_PAGE_SIZE
#  define MRB_HEAP_PAGE_SIZE 4096
# endif

/* A profile for server; mruby vm is long life */
#elif defined(MRB_HIGH_PROFILE)
# ifndef MRB_METHOD_CACHE_SIZE
#  define MRB_METHOD_CACHE_SIZE (1<<12)
# endif

# ifndef MRB_HEAP_PAGE_SIZE
#  define MRB_HEAP_PAGE_SIZE 4096
# endif
#endif

#endif  /* MRUBYCONF_H */
