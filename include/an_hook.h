#pragma once

#include "dynamic_flag.h"

/**
 * Defaults to inactive, unless the an_hook machinery can't get to it,
 * then it's always active.
 */
#define AN_HOOK(KIND, NAME) if (DF_OPT(KIND, NAME))

/**
 * Same as AN_HOOK, but defaults to active.
 */
#define AN_HOOK_ON(KIND, NAME) if (DF_DEFAULT(KIND, NAME))

/**
 * Defaults to inactive, even if unreachable by the an_hook machinery.
 */
#define AN_HOOK_UNSAFE(KIND, NAME) if (DF_FEATURE(KIND, NAME))

/**
 * Hook that should be skipped to activate the corresponding code
 * sequence.  Useful for code that is usually executed.
 *
 * Defaults to skipped hook.
 */
#define AN_HOOK_FLIP(KIND, NAME) if (!DF_DEFAULT(KIND, NAME))

/**
 * Hook that should be skipped to activate the corresponding code
 * sequence, and defaults to executing the hooked code.  Useful for
 * feature flags where the hooked code skips the feature.
 *
 * Defaults to executing the hook (i.e., deactivating the feature).
 */
#define AN_HOOK_FLIP_OFF(KIND, NAME) if (!DF_FEATURE(KIND, NAME))

/**
 * Ensure a hook point exists for kind KIND.
 */
#define AN_HOOK_DUMMY(KIND) DYNAMIC_FLAG_DUMMY(KIND)

#define AN_HOOK_DEBUG(NAME) DF_DEBUG(NAME)

#define an_hook_activate dynamic_flag_activate
#define an_hook_deactivate dynamic_flag_deactivate
#define an_hook_unhook dynamic_flag_unhook
#define an_hook_rehook dynamic_flag_rehook
#define an_hook_init_lib dynamic_flag_init_lib
#define an_hook_activate_kind dynamic_flag_activate_kind
#define an_hook_deactivate_kind dynamic_flag_deactivate_kind
