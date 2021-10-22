This is a vendored and modified copy of `an_hook` from
[ACF](https://github.com/appnexus/acf).  See LICENSE in this folder
for the original Apache 2 license.

Backtrace I/O implemented the updated version found in
`dynamic_flag.[ch]`, which offers a more flexible interface for the
same dynamic flag flipping implementation based on cross-modifying
x86-64 machine code.  The old interface is still available in
`an_hook.h`, a mostly backward compatible (the only differences are
that regular expressions are implicitly anchored at the start, and
`AN_HOOK_DEBUG` switches on `NDEBUG` instead of `DISABLE_DEBUG`)
facade built on top of `dynamic_flag.h`.

See `tests/feature_flags.c` for sample usage.

This new contribution is also distributed under the Apache license v2.
