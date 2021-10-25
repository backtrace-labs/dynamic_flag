#pragma once

/**
 * The dynamic flag library defines global feature flags inline in
 * code.  For example, a `DF_FEATURE(kind, name)` expression defines a
 * feature flag named `name` in namespace `kind`, and evaluates to the
 * 0/1 truth value of that flag.
 *
 * Flags can be toggled by regex (`dynamic_flag_activate` /
 * `dynamic_flag_deactivate`) on the flags' full names:
 * `kind:name@file:line`.  Any flag with an strictly positive
 * activation count is true; activation counts are stored in a
 * `uint64_t`, and it takes n deactivations to turn a flag off after n
 * activations (including the initial one, if the flag defaults to
 * true).  However, deactivations saturate at 0.
 *
 * For programmatic flag flipping, it's often convenient to restrict
 * the regex to a specific `kind` namespace known at compile-time.
 * `dynamic_flag_activate_kind` / `dynamic_flag_deactivate_kind` offer
 * that functionality, with a nullable regex on the flags' full names.
 *
 * While activation counts saturate at 0 when deactivated, it's also
 * possible to "unhook" flags: a flag's unhook counter is initially 0,
 * incremented by each call to `dynamic_flag_unhook` for which the
 * flag's name matches the regex, and decremented by each call to
 * `dynamic_flag_rehook` when the flag's name matches the regex.  The
 * unhook counter too saturates at 0.
 *
 * When a flag's unhook counter is strictly positive, any attempt to
 * activate that flag will no-op, while (saturating) deactivations
 * will still go through.  This lets operators ensure flags remain
 * disabled when they would otherwise be activated programmatically.
 *
 * These regular expressions are POSIX extended regular expressions
 * implicitly anchored at the beginning of the flag name, but not at
 * the end of flag names.  Regular expressions can start with "^" to
 * make the start anchor explicit; otherwise, the pattern is
 * implicitly prefixed with a caret (left anchor).  The regular
 * expressions must use a `$` ancor to match (only) the full flag
 * name, and may start with `.*` to start matching from any location.
 *
 * The dynamic flag library is thread-safe but not async-signal-safe,
 * and can safely manipulate flags while other threads are evaluating
 * `DF_FEATURE` and similar dynamic flag expressions.  Whether the
 * surrounding application logic is ready for a flag's value to change
 * at runtime is a different question.
 *
 * Flags are most commonly manipulated at application startup.  It may
 * be convenient to describe these startup-time flag flips with a list
 * (the evlauation order is important: activation and unhook counts
 * saturate, and activation is a no-op for unhooked flags) of strings
 * in a configuration file.  For example, "+[regex]" could invoke
 * `dynamic_flag_activate` on that regex, "-[regex]" invoke
 * `dynamic_flag_deactivate`, "!regex" invoke `dynamic_flag_unhook`,
 * and, if necessary, "?regex" could invoke `dynamic_flag_rehook`.
 */

/*
 * Choose the implementation style:
 *
 *  0: fallback that hardcodes each flag to its default "safe" value
 *  1: dynamic flag implementation that only needs extended inline asm
 *  2: dynamic flag implementation that takes advantages of asm goto
 */
#ifndef DYNAMIC_FLAG_IMPLEMENTATION_STYLE
# if !defined(__GNUC__)
#  define DYNAMIC_FLAG_IMPLEMENTATION_STYLE 0
# elif defined(__clang_analyzer__) || defined(__COVERITY__) || defined(__CHECKER__)
#  define DYNAMIC_FLAG_IMPLEMENTATION_STYLE 1
# elif defined(__clang_major__)
#  if __clang_major__ >= 9  /* Need that for asm goto */
#   define DYNAMIC_FLAG_IMPLEMENTATION_STYLE 2
#  else
#   define DYNAMIC_FLAG_IMPLEMENTATION_STYLE 1
#  endif
# elif __GNUC__ >= 5 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5)
   /* GCC gained asm goto in 4.5 */
#  define DYNAMIC_FLAG_IMPLEMENTATION_STYLE 2
# else
#  define DYNAMIC_FLAG_IMPLEMENTATION_STYLE 1
# endif
#endif

#if DYNAMIC_FLAG_IMPLEMENTATION_STYLE < 0 || DYNAMIC_FLAG_IMPLEMENTATION_STYLE > 2
# error "Invalid DYNAMIC_FLAG_IMPLEMENTATION_STYLE value.  " \
	"Must be 0 (static flag), 1 (non-asm-goto fallback), "	\
	"or 2 (preferred asm-goto implementation)."
#endif

/*
 * DYNAMIC_FLAG_CTL_INTERFACE=0 makes this header define the DF_* macros and
 * none of the control functions.
 */
#ifndef DYNAMIC_FLAG_CTL_INTERFACE
#define DYNAMIC_FLAG_CTL_INTERFACE 1
#endif

/**
 * DF_FEATURE defines a dynamic boolean flag that defaults to false,
 * and is always false when the dynamic_flag library does not run.
 *
 * It is useful for experimental feature flags.
 *
 * The third argument is an optional docstring.
 */
#define DF_FEATURE(KIND, NAME, ...)					\
	__builtin_expect(DYNAMIC_FLAG_IMPL(				\
	    DYNAMIC_FLAG_VALUE_INACTIVE, DYNAMIC_FLAG_VALUE_INACTIVE, 0, \
	    KIND, NAME, __FILE__, __LINE__, "" __VA_ARGS__),		\
        0)

/**
 * DF_DEFAULT defines a dynamic boolean flag that defaults to true,
 * and is always true when the dynamic_flag library does not run.
 *
 * It is useful for code that is usually enabled.
 *
 * The third argument is an optional docstring.
 */
#define DF_DEFAULT(KIND, NAME, ...)					\
	__builtin_expect(!DYNAMIC_FLAG_IMPL(				\
	    DYNAMIC_FLAG_VALUE_INACTIVE, DYNAMIC_FLAG_VALUE_INACTIVE, 1, \
	    KIND, NAME, __FILE__, __LINE__, "" __VA_ARGS__),		\
	1)

/**
 * DF_DEFAULT_SLOW is semantically identical to DF_DEFAULT, but optimises
 * for the flag being set to false.
 *
 * It is useful for code that is usually enabled, but should be
 * most efficient when disabled.
 *
 * The third argument is an optional docstring.
 */
#define DF_DEFAULT_SLOW(KIND, NAME, ...)				\
	__builtin_expect(DYNAMIC_FLAG_IMPL(				\
	    DYNAMIC_FLAG_VALUE_ACTIVE, DYNAMIC_FLAG_VALUE_ACTIVE, 0,	\
	    KIND, NAME, __FILE__, __LINE__, "" __VA_ARGS__),		\
	0)

/**
 * DF_OPT (optional or opt-in) defines a dynamic boolean flag that
 * defaults to false, but is always true when the dynamic_flag library
 * does not run.
 *
 * It is useful for code that is usually disabled, but should always
 * be safe to enable.
 *
 * The third argument is an optional docstring.
 */
#define DF_OPT(KIND, NAME, ...)						\
	__builtin_expect(DYNAMIC_FLAG_IMPL(				\
	    DYNAMIC_FLAG_VALUE_ACTIVE, DYNAMIC_FLAG_VALUE_INACTIVE, 0,	\
	    KIND, NAME, __FILE__, __LINE__, "" __VA_ARGS__),		\
	0)

/**
 * Defines a flag for a given KIND: dynamic_flag_activate_kind and
 * dynamic_flag_deactivate_kind will fail to link if there are no
 * flag for that kind.
 *
 * The second argument is an optional docstring.
 */
#define DYNAMIC_FLAG_DUMMY(KIND, ...)					\
	do {								\
	  if (DF_FEATURE(KIND, dummy, ##__VA_ARGS__)) {			\
			asm volatile("");				\
		}							\
	} while (0)

/**
 * DF_DEBUG flags are enabled by default in regular builds, and
 * disabled by default in NDEBUG (release) builds.
 *
 * The second argument is an optional docstring.
 */
#ifdef NDEBUG
# define DF_DEBUG(NAME, ...) DF_DEFAULT_SLOW(debug, NAME, ##__VA_ARGS__)
#else
# define DF_DEBUG(NAME, ...) DF_FEATURE(debug, NAME, ##__VA_ARGS__)
#endif

#if DYNAMIC_FLAG_CTL_INTERFACE
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#if DYNAMIC_FLAG_IMPLEMENTATION_STYLE != 0
/**
 * @brief (de)activate all flags of kind @a KIND; if @a PATTERN is
 *  non-NULL, the flag names must match @a PATTERN as a regex.
 * @return the number of matched flags on success, negative on failure.
 */
#define dynamic_flag_activate_kind(KIND, PATTERN)			\
	do {								\
		ssize_t dynamic_flag_activate_kind_inner(const void **start,\
		    const void **end, const char *regex);		\
		extern const void *__start_dynamic_flag_##KIND##_list[];\
		extern const void *__stop_dynamic_flag_##KIND##_list[]; \
									\
		dynamic_flag_activate_kind_inner(			\
		    __start_dynamic_flag_##KIND##_list,			\
		    __stop_dynamic_flag_##KIND##_list,			\
		    (PATTERN));						\
	} while (0)

#define dynamic_flag_deactivate_kind(KIND, PATTERN)			\
	do {								\
		ssize_t dynamic_flag_deactivate_kind_inner(const void **start,\
		    const void **end, const char *regex);		\
		extern const void *__start_dynamic_flag_##KIND##_list[];\
		extern const void *__stop_dynamic_flag_##KIND##_list[]; \
									\
		dynamic_flag_deactivate_kind_inner(			\
		    __start_dynamic_flag_##KIND##_list,			\
		    __stop_dynamic_flag_##KIND##_list,			\
		    (PATTERN));						\
	} while (0)

/**
 * @brief activate all flags that match @a regex, regardless of the kind.
 * @return the number of matched flags on success, negative on failure.
 */
ssize_t dynamic_flag_activate(const char *regex);

/**
 * @brief deactivate all flags that match @a regex, regardless of the kind.
 * @return the number of matched flags on success, negative on failure.
 */
ssize_t dynamic_flag_deactivate(const char *regex);

/**
 * @brief disable hooking for all flags that match @a regex, regardless of the kind.
 * @return the number of matched flags on success, negative on failure.
 *
 * If a flag is unhooked, activating that flag does nothing and does
 * not increment the activation count.
 */
ssize_t dynamic_flag_unhook(const char *regex);

/**
 * @brief reenable hooking all flags that match @a regex, regardless of the kind.
 * @return the number of matched flags on success, negative on failure.
 */
ssize_t dynamic_flag_rehook(const char *regex);

/**
 * Description for a given dynamic flag's state.
 *
 * All pointees have static lifetimes, and may be read outside the
 * callback's invocation.  The C strings in `name` and `doc` are
 * immutable, although the machine code at `hook` and `destination`
 * may change.
 */
struct dynamic_flag_state {
	const char *name;  /* Flag's full name (kind:name@file:line) */
	const char *doc;  /* Docstring for the flag (empty string if none) */
	uint64_t activation;  /* Number of activations (active if > 0) */
	uint64_t unhook;  /* unhook depth (activations disabled if > 0) */
	const void *hook;  /* Address of the hook instruction */

	/*
	 * This field is only useful if DYNAMIC_FLAG_IMPLEMENTATION_STYLE == 2;
	 * otherwise it's always 0.
	 */
	const void *destination;  /* Address of the slow path code. */
	bool duplicate;
};

/**
 * @brief invokes @a cb with a list of all flags that match @a regex, until
 *   @a cb returns a non-zero value.
 * @return -1 if we failed to compile the regex, the first non-zero value
 *   returned by @a cb if any, or the number of flags listed otherwise.
 */
ssize_t dynamic_flag_list_state(const char *regex,
    ssize_t (*cb)(void *ctx, const struct dynamic_flag_state *), void *ctx);

/**
 * @brief Prints all non-duplicate entries to the `FILE *` ctx, or to `stderr` if
 *   NULL.
 * @return 0.
 */
ssize_t dynamic_flag_list_fprintf_cb(void *ctx, const struct dynamic_flag_state *);

/**
 * @brief initializes the dynamic_flag subsystem.
 *
 * It is safe if useless to call this function multiple times.
 */
void dynamic_flag_init_lib(void);
#else

#define dynamic_flag_activate_kind(KIND, PATTERN) dynamic_flag_dummy((PATTERN))
#define dynamic_flag_deactivate_kind(KIND, PATTERN) dynamic_flag_dummy((PATTERN))

#define dynamic_flag_activate dynamic_flag_dummy
#define dynamic_flag_deactivate dynamic_flag_dummy
#define dynamic_flag_unhook dynamic_flag_dummy
#define dynamic_flag_rehook dynamic_flag_dummy
#define dynamic_flag_init_lib dynamic_flag_init_lib_dummy
#define dynamic_flag_list dynamic_flag_list_state_dummy

#endif  /* DYNAMIC_FLAG_IMPLEMENTATION_STYLE */

inline int
dynamic_flag_dummy(const char *regex)
{

	(void)regex;
	return 0;
}

inline void
dynamic_flag_init_lib_dummy(void)
{

	return;
}

inline long long
dynamic_flag_list_state_dummy(const char *regex,
    long long (*cb)(void *ctx, const struct dynamic_flag_state *), void *ctx)
{

	(void)regex;
	(void)cb;
	(void)ctx;
	return 0;
}
#endif  /* DYNAMIC_FLAG_CTL_INTERFACE */

#if DYNAMIC_FLAG_IMPLEMENTATION_STYLE == 0

#define DYNAMIC_FLAG_VALUE_ACTIVE 1
#define DYNAMIC_FLAG_VALUE_INACTIVE 0
#define DYNAMIC_FLAG_IMPL_(DEFAULT, ...) DEFAULT

#elif DYNAMIC_FLAG_IMPLEMENTATION_STYLE == 1

/*
 * Fallback implementation: mov a constant into a variable, and let
 * the compiler test on the resulting value.  We flip a dynamic flag
 * by modifying the immediate literal value in the mov instruction.
 *
 * 0xF4 is HLT, a privileged instruction that shuts down the core; it
 * should be rare in machine code, so we'll be able to figure out if
 * the patch location is wrong.
 *
 * DEFAULT is the flag value in the machine code, before the dynamic
 * flag machinery is activated.
 *
 * INITIAL is the flag value set by the dynamic flag machinery on
 * startup.
 *
 * FLIPPED means that activating the flag should write
 * `DYNAMIC_FLAG_VALUE_INACTIVE`.
 */
#define DYNAMIC_FLAG_VALUE_ACTIVE 0xF4
#define DYNAMIC_FLAG_VALUE_INACTIVE 0

#define DYNAMIC_FLAG_IMPL_(DEFAULT, INITIAL, FLIPPED,			\
    KIND, NAME, FILE, LINE, DOC)					\
	({								\
		unsigned char r;					\
									\
		asm("1:\n\t"						\
		    "movb $"#DEFAULT", %0\n\t"				\
									\
		    ".pushsection .rodata\n\t"				\
		    "2: .asciz \"" #KIND ":" #NAME "@" FILE ":" #LINE "\"\n\t" \
		    ".asciz \"" DOC "\"\n\t"				\
		    ".popsection\n\t"					\
									\
		    ".pushsection dynamic_flag_list,\"a\",@progbits\n\t" \
		    "3:\n\t"						\
		    ".quad 1b\n\t"					\
		    ".quad 0\n\t"					\
		    ".quad 2b\n\t"					\
		    ".byte "#INITIAL"\n\t"				\
		    ".byte "#FLIPPED"\n\t"				\
		    ".fill 6\n\t"					\
		    ".popsection\n\t"					\
									\
		    ".pushsection dynamic_flag_"#KIND"_list,\"a\",@progbits\n\t" \
		    ".quad 3b\n\t"					\
		    ".popsection"					\
		    :"=r"(r));						\
		!!r;							\
	})

#elif DYNAMIC_FLAG_IMPLEMENTATION_STYLE == 2

/*
 * Preferred implementation. We use an asm goto to execute a `testl
 * $..., %eax` and fall through to the "inactive" code block by
 * default, and overwrite the opcode to a `jmp rel` to activate the
 * code block.
 *
 * Using `testl` as our quasi-noop makes it possible to encode the
 * jump offset statically.
 *
 * Implementation details:
 *
 *  The first line introduces a local label just before a 5-byte
 *   testl $..., %eax.
 *  We use that instruction instead of a nop (and declare a clobber on
 *  EFLAGS) to simplify hotpatching with concurrent execution: we
 *  can turn TEST into a JMP REL to foo_hook with a byte write.
 *
 *  The rest stashes metadata in a couple sections.
 *
 *   1. the name of the flag, in the normal read-only section.
 *   2. the hook struct:
 *        - a pointer to the hook instruction;
 *        - the address of the destination;
 *        - a pointer to the flag name (as a C string).
 *   3. a reference to the struct, in the kind's custom section.
 *
 *  The if condition tells the compiler to skip the next block of code
 *  (the conditional is false) and to consider it unlikely to be
 *  executed, despite the asm-visible label.
 *
 * Numerical labels (from 1 to 9) can be repeated however many times
 * as necessary; "1f" refers to the next label named 1 (1 forward),
 * while "1b" searches backward.
 *
 * The push/pop section stuff gives us out of line metadata from a
 * contiguous macro expansion.
 *
 * Inspired by tracepoints in the Linux kernel. <http://lwn.net/Articles/350714/>
 */

#define DYNAMIC_FLAG_VALUE_ACTIVE 0xe9 /* jmp rel 32 */
#define DYNAMIC_FLAG_VALUE_INACTIVE 0xa9 /* testl $, %eax */

#if defined(__GNUC__) && !defined(__clang__)
#define DYNAMIC_FLAG_IMPL_COLD __attribute__((__cold__))
#else
#define DYNAMIC_FLAG_IMPL_COLD
#endif

#define DYNAMIC_FLAG_IMPL_(DEFAULT, INITIAL, FLIPPED,			\
    KIND, NAME, FILE, LINE, DOC)						\
	({								\
		__label__ DYNAMIC_FLAG_IMPL_label;			\
		unsigned char r = 0;					\
									\
		asm goto("1:\n\t"					\
			 ".byte "#DEFAULT"\n\t"				\
			 ".long %l[DYNAMIC_FLAG_IMPL_label] - (1b + 5)\n\t" \
									\
			 ".pushsection .rodata\n\t"			\
			 "2: .asciz \"" #KIND ":" #NAME "@" FILE ":" #LINE "\"\n\t" \
			 ".asciz \"" DOC "\"\n\t"			\
			 ".popsection\n\t"				\
									\
			 ".pushsection dynamic_flag_list,\"a\",@progbits\n\t" \
			 "3:\n\t"					\
			 ".quad 1b\n\t"					\
			 ".quad %l[DYNAMIC_FLAG_IMPL_label]\n\t" 	\
			 ".quad 2b\n\t"					\
			 ".byte "#INITIAL"\n\t"				\
			 ".byte "#FLIPPED"\n\t"				\
			 ".fill 6\n\t"					\
			 ".popsection\n\t"				\
									\
			 ".pushsection dynamic_flag_"#KIND"_list,\"a\",@progbits\n\t" \
			 ".quad 3b\n\t"					\
			 ".popsection"					\
			 ::: "cc" : DYNAMIC_FLAG_IMPL_label);		\
		if (0) {						\
		DYNAMIC_FLAG_IMPL_label: DYNAMIC_FLAG_IMPL_COLD;	\
			r = 1;						\
		}							\
									\
		r;							\
	})
#endif

#define DYNAMIC_FLAG_IMPL(DEFAULT, INITIAL, FLIPPED, KIND, NAME, FILE, LINE, DOC) \
	DYNAMIC_FLAG_IMPL_(DEFAULT, INITIAL, FLIPPED, KIND, NAME, FILE, LINE, DOC)
