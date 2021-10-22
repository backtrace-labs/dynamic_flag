#include <stdio.h>

#include "dynamic_flag.h"


static void
run_all(void)
{

	if (DF_OPT(off, printf1)) {
		printf("off:printf1\n");
	}

	if (DF_OPT(off, printf2)) {
		printf("off:printf2\n");
	}

	if (DF_DEFAULT(on, printf1)) {
		printf("on:printf1\n");
	}

	if (DF_DEFAULT_SLOW(on, printf2)) {
		printf("on:printf2\n");
	}

	if (DF_DEFAULT(on, printf3)) {
		printf("on:printf3\n");
	}

	if (DF_OPT(untouched, printf1)) {
		printf("untouched:printf1\n");
	}

	if (DF_DEFAULT(untouched, printf2)) {
		printf("untouched:printf2\n");
	}

	if (DF_DEFAULT(feature_flag, default_on)) {
		printf("feature_flag:default_on\n");
	}

	if (DF_FEATURE(feature_flag, default_off)) {
		printf("feature_flag:default_off\n");
	}

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

int main(int argc, char **argv)
{

	(void)argc;
	(void)argv;

	printf("Before init\n");
	run_all();
	dynamic_flag_init_lib();

	printf("\nInitial:\n");
	run_all();

	wrapped_activate("off:printf1");
	run_all();

	wrapped_deactivate("on:.*");
	run_all();

	wrapped_activate("on:printf3");
	run_all();

	wrapped_deactivate("feature_flag:.*");
	run_all();

	wrapped_activate("feature_flag:default_off");
	run_all();

	printf("\nActivating feature_flag\n");
	dynamic_flag_activate_kind(feature_flag, ".*");
	run_all();

	printf("\nDeactivating feature_flag\n");
	dynamic_flag_deactivate_kind(feature_flag, ".*");
	run_all();

	printf("\nUnhooking feature_flag:.*");
	dynamic_flag_unhook("feature_flag:.*");
	wrapped_activate("feature_flag:.*");
	run_all();

	printf("\nDeactivating feature_flag:.*\n");
	dynamic_flag_deactivate_kind(feature_flag, ".*");
	run_all();

	printf("\nRehooking feature_flag:.*");
	dynamic_flag_rehook("feature_flag:.*");
	wrapped_activate("feature_flag:.*");
	run_all();

	printf("\nDeactivating feature_flag\n");
	dynamic_flag_deactivate_kind(feature_flag, ".*");
	run_all();

	return 0;
}
