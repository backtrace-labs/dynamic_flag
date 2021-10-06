#include <stdio.h>

#include "an_hook.h"


static void
run_all(void)
{

	AN_HOOK(off, printf1) {
		printf("off:printf1\n");
	}

	AN_HOOK(off, printf2) {
		printf("off:printf2\n");
	}

	AN_HOOK_ON(on, printf1) {
		printf("on:printf1\n");
	}

	AN_HOOK_ON(on, printf2) {
		printf("on:printf2\n");
	}

	AN_HOOK_ON(on, printf3) {
		printf("on:printf3\n");
	}

	AN_HOOK(untouched, printf1) {
		printf("untouched:printf1\n");
	}

	AN_HOOK_ON(untouched, printf2) {
		printf("untouched:printf2\n");
	}

	AN_HOOK_FLIP(feature_flag, default_on) {
	} else {
		printf("feature_flag:default_on\n");
	}

	AN_HOOK_FLIP_OFF(feature_flag, default_off) {
	} else {
		printf("feature_flag:default_off\n");
	}

	return;
}

static void
wrapped_activate(const char *pat)
{

	printf("\nActivating %s\n", pat);
	an_hook_activate(pat);
	return;
}

static void
wrapped_deactivate(const char *pat)
{

	printf("\nDeactivating %s\n", pat);
	an_hook_deactivate(pat);
	return;
}

int main(int argc, char **argv)
{

	printf("Before init\n");
	run_all();
	an_hook_init_lib();

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

	return 0;
}
