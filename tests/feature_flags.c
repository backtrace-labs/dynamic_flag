#include "dynamic_flag.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static __attribute__((noinline, cold)) void run_all_tail(void)
{
	if (DF_OPT(untouched, printf1)) {
		printf("untouched:printf1\n");
	}

	if (DF_DEFAULT(untouched, printf2)) {
		printf("untouched:printf2\n");
	}

	if (DF_DEFAULT(feature_flag, default_on)) {
		printf("feature_flag:default_on\n");
	}

	if (DF_FEATURE(feature_flag, default_off,
	    "DF_FEATURE flags are classic feature flags: off initially "
	    "and if the dynamic_flag machine can't find them, "
	    "and the compiler expects them to be disabled")) {
		printf("feature_flag:default_off\n");
	}

	return;
}

static void
run_all(void)
{

	if (DF_OPT(off, printf1,
	    "DF_OPT flags are usually disabled, "
	    "but should always be safe to enable")) {
		printf("off:printf1\n");
	}

	if (DF_OPT(off, printf2)) {
		printf("off:printf2\n");
	}

	if (DF_DEFAULT(on, printf1,
	    "DF_DEFAULT flags are enabled initially and when the library can't find them.")) {
		printf("on:printf1\n");
	}

	if (DF_DEFAULT_SLOW(on, printf2,
	    "DF_DEFAULT_SLOW flags are enabled like DF_DEFAULT, "
	    "but instruct the compiler to expect them to be disabled.")) {
		printf("on:printf2\n");
	}

	if (DF_DEFAULT(on, printf3)) {
		printf("on:printf3\n");
	}

	/* Catch unanchored regexes. */
	if (DF_OPT(test, on:printf3)) {
		printf("test:on:printf3\n");
	}

	run_all_tail();
	return;
}

static void
wrapped_activate(const char *pat)
{

	printf("\nActivating %s\n", pat);
	dynamic_flag_activate(pat);
	return;
}

static void
wrapped_deactivate(const char *pat)
{

	printf("\nDeactivating %s\n", pat);
	dynamic_flag_deactivate(pat);
	return;
}

int
main(int argc, char **argv)
{

	printf("Before init\n");
	/*
	 * Expected:
	 * Before init
	 * off:printf1
	 * off:printf2
	 * on:printf1
	 * on:printf2
	 * on:printf3
	 * test:on:printf3
	 * untouched:printf1
	 * untouched:printf2
	 * feature_flag:default_on
	 */
	run_all();
	dynamic_flag_init_lib();

	if (argc > 1) {
		dynamic_flag_set_minimal_write_mode(atoi(argv[1]));
	}

	printf("\nList all flags\n");
	/*
	 * Expected:
	 * List all flags
	 * feature_flag:default_off@tests/feature_flags.c:55 (off): DF_FEATURE flags are classic feature flags: off initially and if the dynamic_flag machine can't find them, and the compiler expects them to be disabled
	 * feature_flag:default_on@tests/feature_flags.c:48 (on)
	 * none:dummy@src/dynamic_flag.c:158 (off): This dummy flag does nothing. It lets the dynamic_flag library compile even when no other flag is defined.
	 * off:printf1@tests/feature_flags.c:12 (off): DF_OPT flags are usually disabled, but should always be safe to enable
	 * off:printf2@tests/feature_flags.c:16 (off)
	 * on:printf1@tests/feature_flags.c:21 (on): DF_DEFAULT flags are enabled initially and when the library can't find them.
	 * on:printf2@tests/feature_flags.c:27 (on): DF_DEFAULT_SLOW flags are enabled like DF_DEFAULT, but instruct the compiler to expect them to be disabled.
	 * on:printf3@tests/feature_flags.c:31 (on)
	 * test:on:printf3@tests/feature_flags.c:36 (off)
	 * untouched:printf1@tests/feature_flags.c:40 (off)
	 * untouched:printf2@tests/feature_flags.c:44 (on)
	 */
	dynamic_flag_list_state(".*", dynamic_flag_list_fprintf_cb, stdout);

	printf("\nInitial:\n");
	/*
	 * Expected:
	 * Initial:
	 * on:printf1
	 * on:printf2
	 * on:printf3
	 * untouched:printf2
	 * feature_flag:default_on
	 */
	run_all();

	wrapped_activate("off:printf1");
	/*
	 * Expected:
	 * Activating off:printf1
	 * off:printf1
	 * on:printf1
	 * on:printf2
	 * on:printf3
	 * untouched:printf2
	 * feature_flag:default_on
	 */
	run_all();

	wrapped_activate("^test:on:printf3");
	/*
	 * Expected:
	 * Activating ^test:on:printf3
	 * off:printf1
	 * on:printf1
	 * on:printf2
	 * on:printf3
	 * test:on:printf3
	 * untouched:printf2
	 * feature_flag:default_on
	 */
	run_all();

	wrapped_deactivate(".*on:.*");
	/*
	 * Expected:
	 * Deactivating .*on:.*
	 * off:printf1
	 * untouched:printf2
	 * feature_flag:default_on
	 */
	run_all();

	wrapped_activate("on:printf3");
	/*
	 * Expected:
	 * Activating on:printf3
	 * off:printf1
	 * on:printf3
	 * untouched:printf2
	 * feature_flag:default_on
	 */
	run_all();

	wrapped_deactivate("feature_flag:.*");
	/*
	 * Expected:
	 * Deactivating feature_flag:.*
	 * off:printf1
	 * on:printf3
	 * untouched:printf2
	 */
	run_all();

	wrapped_activate("feature_flag:default_off");
	/*
	 * Expected:
	 * Activating feature_flag:default_off
	 * off:printf1
	 * on:printf3
	 * untouched:printf2
	 * feature_flag:default_off
	 */
	run_all();

	printf("\nActivating feature_flag\n");
	dynamic_flag_activate_kind(feature_flag, ".*");
	/*
	 * Expected:
	 * Activating feature_flag
	 * off:printf1
	 * on:printf3
	 * untouched:printf2
	 * feature_flag:default_on
	 * feature_flag:default_off
	 */
	run_all();

	printf("\nDeactivating feature_flag\n");
	dynamic_flag_deactivate_kind(feature_flag, ".*");
	/* 
	 * Expected:
	 * Deactivating feature_flag
	 * off:printf1
	 * on:printf3
	 * untouched:printf2
	 * feature_flag:default_off
	 */
	run_all();

	printf("\nUnhooking feature_flag:.*");
	dynamic_flag_unhook("feature_flag:.*");
	wrapped_activate("feature_flag:.*");
	/*
	 * Expected:
	 * Unhooking feature_flag:.*
	 * Activating feature_flag:.*
	 * off:printf1
	 * on:printf3
	 * untouched:printf2
	 * feature_flag:default_off
	 */
	run_all();

	printf("\nDeactivating feature_flag:.*\n");
	dynamic_flag_deactivate_kind(feature_flag, NULL);
	/*
	 * Expected:
	 * Deactivating feature_flag:.*
	 * off:printf1
	 * on:printf3
	 * untouched:printf2
	 */
	run_all();

	printf("\nRehooking feature_flag:.*");
	dynamic_flag_rehook("feature_flag:.*");
	wrapped_activate("feature_flag:.*");
	/*
	 * Expected:
	 * Rehooking feature_flag:.*
	 * Activating feature_flag:.*
	 * off:printf1
	 * on:printf3
	 * untouched:printf2
	 * feature_flag:default_on
	 * feature_flag:default_off
	 */
	run_all();

	printf("\nDeactivating feature_flag\n");
	dynamic_flag_deactivate_kind(feature_flag, NULL);
	/*
	 * Expected:
	 * Deactivating feature_flag
	 * off:printf1
	 * on:printf3
	 * untouched:printf2
	 */
	run_all();

	return 0;
}
