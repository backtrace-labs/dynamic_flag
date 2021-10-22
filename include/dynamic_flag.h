#pragma once

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

#if DYNAMIC_FLAG_IMPLEMENTATION_STYLE == 0
/**
 * DF_FEATURE defines a dynamic boolean flag that defaults to false,
 * and is always false when the dynamic_flag library does not run.
 *
 * It is useful for experimental feature flags.
 */
#define DF_FEATURE(KIND, NAME) 0

/**
 * DF_DEFAULT defines a dynamic boolean flag that defaults to true,
 * and is always true when the dynamic_flag library does not run.
 *
 * It is useful for code that is usually enabled.
 */
#define DF_DEFAULT(KIND, NAME) 1

/**
 * DF_OPT (optional or opt-in) defines a dynamic boolean flag that
 * defaults to false, and is always true when the dynamic_flag library
 * does not run.
 *
 * It is useful for code that is usually disabled, but should always
 * be safe to enable.
 */
#define DF_OPT(KIND, NAME) 1

#elif DYNAMIC_FLAG_IMPLEMENTATION_STYLE == 2

/*
 * Preferred implementation. We use an asm goto to execute a `testl
 * $..., %eax` and fall through to the "inactive" code block by
 * default, and overwrite the opcode to a `jmp rel` to activate the
 * code block.
 *
 * Using `testl` as our quasi-noop makes it possible to encode the
 * jump offset statically.
 */

#define DYNAMIC_FLAG_VALUE_ACTIVE 0xe9 /* jmp rel 32 */
#define DYNAMIC_FLAG_VALUE_INACTIVE 0xa9 /* testl $, %eax */

#define DYNAMIC_FLAG_IMPL_(DEFAULT, INITIAL, FLIPPED,			\
    KIND, NAME, FILE, LINE, GENSYM)					\
	({								\
	    unsigned char r = 0;					\
									\
	    asm goto ("1:\n\t"						\
		      ".byte "#DEFAULT"\n\t"				\
		      ".long %l[dynamic_flag_"#GENSYM"_label] - (1b + 5)\n\t"\
									\
		      ".pushsection .rodata\n\t"			\
		      "2: .asciz \"" #KIND ":" #NAME "@" FILE ":" #LINE "\"\n\t" \
		      ".popsection\n\t"					\
									\
		      ".pushsection dynamic_flag_list,\"a\",@progbits\n\t"\
		      "3:\n\t"						\
		      ".quad 1b\n\t"					\
		      ".quad %l[dynamic_flag_"#GENSYM"_label]\n\t" 	\
		      ".quad 2b\n\t"					\
		      ".byte "#INITIAL"\n\t"				\
		      ".byte "#FLIPPED"\n\t"				\
		      ".fill 6\n\t"					\
		      ".popsection\n\t"					\
									\
		      ".pushsection dynamic_flag_"#KIND"_list,\"a\",@progbits\n\t" \
		      ".quad 3b\n\t"					\
		      ".popsection"					\
		      ::: "cc" : dynamic_flag_##GENSYM##_label);	\
	    if (0) {							\
	    dynamic_flag_##GENSYM##_label: __attribute__((__cold__));	\
		    r = 1;						\
	    }								\
									\
	    r;								\
	})
#elif DYNAMIC_FLAG_IMPLEMENTATION_STYLE == 1

/*
 * Fallback implementation: mov a constant into a variable, and let
 * the compiler test on that.
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
    KIND, NAME, FILE, LINE, GENSYM)					\
	({								\
	    unsigned char r;						\
									\
	    asm ("1:\n\t"						\
		 "movb $"#DEFAULT", %0\n\t"				\
									\
		 ".pushsection .rodata\n\t"				\
		 "2: .asciz \"" #KIND ":" #NAME "@" FILE ":" #LINE "\"\n\t" \
		 ".popsection\n\t"					\
									\
		 ".pushsection dynamic_flag_list,\"a\",@progbits\n\t"	\
		 "3:\n\t"						\
		 ".quad 1b\n\t"						\
		 ".quad 0\n\t"						\
		 ".quad 2b\n\t"						\
		 ".byte "#INITIAL"\n\t"					\
		 ".byte "#FLIPPED"\n\t"					\
		 ".fill 6\n\t"						\
		 ".popsection\n\t"					\
									\
		 ".pushsection dynamic_flag_"#KIND"_list,\"a\",@progbits\n\t"\
		 ".quad 3b\n\t"						\
		 ".popsection"						\
		:"=r"(r));						\
	    !!r;							\
	})

#endif

#if DYNAMIC_FLAG_IMPLEMENTATION_STYLE != 0
#define DYNAMIC_FLAG_IMPL(DEFAULT, INITIAL, FLIPPED,			\
			  KIND, NAME, FILE, LINE, GENSYM)		\
	DYNAMIC_FLAG_IMPL_(DEFAULT, INITIAL, FLIPPED,			\
			   KIND, NAME, FILE, LINE, GENSYM)

#define DF_FEATURE(KIND, NAME)						\
	__builtin_expect(DYNAMIC_FLAG_IMPL(				\
	    DYNAMIC_FLAG_VALUE_INACTIVE, DYNAMIC_FLAG_VALUE_INACTIVE, 0, \
	    KIND, NAME, __FILE__, __LINE__, __COUNTER__),		\
        0)

#define DF_DEFAULT(KIND, NAME)						\
	__builtin_expect(!DYNAMIC_FLAG_IMPL(				\
	    DYNAMIC_FLAG_VALUE_INACTIVE, DYNAMIC_FLAG_VALUE_INACTIVE, 1, \
	    KIND, NAME, __FILE__, __LINE__, __COUNTER__),		\
	1)

#define DF_OPT(KIND, NAME)						\
	__builtin_expect(DYNAMIC_FLAG_IMPL(				\
	    DYNAMIC_FLAG_VALUE_ACTIVE, DYNAMIC_FLAG_VALUE_INACTIVE, 0,	\
	    KIND, NAME, __FILE__, __LINE__, __COUNTER__),		\
	0)
#endif

#define DYNAMIC_FLAG_DUMMY(KIND)					\
	do {								\
		if (DF_FEATURE(KIND, dummy)) {				\
			asm volatile("");				\
		}							\
	} while (0)

#ifdef NDEBUG
# define DF_DEBUG(NAME) DF_DEFAULT(debug, NAME)
#else
# define DF_DEBUG(NAME) DF_FEATURE(debug, NAME)
#endif

#if DYNAMIC_FLAG_IMPLEMENTATION_STYLE != 0
/**
 * @brief (de)activate all hooks of kind @a KIND; if @a PATTERN is
 *  non-NULL, the hook names must match @a PATTERN as a regex.
 */
#define dynamic_flag_activate_kind(KIND, PATTERN)			\
	do {								\
		int dynamic_flag_activate_kind_inner(const void **start,\
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
		int dynamic_flag_deactivate_kind_inner(const void **start,\
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
 * @brief activate all hooks that match @a regex, regardless of the kind.
 * @return negative on failure, 0 on success.
 */
int dynamic_flag_activate(const char *regex);

/**
 * @brief deactivate all hooks that match @a regex, regardless of the kind.
 * @return negative on failure, 0 on success.
 */
int dynamic_flag_deactivate(const char *regex);

/**
 * @brief disable hooking for all hooks that match @a regex, regardless of the kind.
 * @return negative on failure, 0 on success.
 *
 * If a hook is unhooked, activating that hook does nothing and does
 * not increment the activation count.
 */
int dynamic_flag_unhook(const char *regex);

/**
 * @brief reenable hooking all hooks that match @a regex, regardless of the kind.
 * @return negative on failure, 0 on success.
 */
int dynamic_flag_rehook(const char *regex);

/**
 * @brief initializes the dynamic_flag subsystem.
 *
 * It is safe if useless to call this function multiple times.
 */
void dynamic_flag_init_lib(void);
#else
#define dynamic_flag_activate dynamic_flag_dummy
#define dynamic_flag_deactivate dynamic_flag_dummy
#define dynamic_flag_unhook dynamic_flag_dummy
#define dynamic_flag_rehook dynamic_flag_dummy
#define dynamic_flag_init_lib dynamic_flag_init_lib_dummy

#define dynamic_flag_activate_kind(KIND, PATTERN) dynamic_flag_dummy((PATTERN))
#define dynamic_flag_deactivate_kind(KIND, PATTERN) dynamic_flag_dummy((PATTERN))
#endif

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
