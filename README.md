Biased runtime-flippable flags, with cross-modifying code
=========================================================

The `dynamic_flag` library is a two-file "C" library that offers
efficient biased conditionals for statically linked Linux/x86-64
executables: taking the "likely" path doesn't access memory or
evaluate any jump condition,[^second-order]
[it merely executes a `test eax` instruction that clobbers flags without any dependent instruction](https://godbolt.org/#g:!((g:!((g:!((h:codeEditor,i:(filename:'1',fontScale:14,fontUsePx:'0',j:1,lang:___c,selection:(endColumn:8,endLineNumber:4,positionColumn:8,positionLineNumber:4,selectionStartColumn:8,selectionStartLineNumber:4,startColumn:8,startLineNumber:4),source:'%23include+%3Cstdio.h%3E%0A%23include+%3Chttps://raw.githubusercontent.com/backtrace-labs/dynamic_flag/main/include/dynamic_flag.h%3E%0A%0Aint+foo()%0A%7B%0A++++if+(DF_FEATURE(flag_kind,+flag_name))+%7B%0A++++++++printf(%22unlikely+path%5Cn%22)%3B%0A++++++++return+1%3B%0A++++%7D%0A%0A++++return+0%3B%0A%7D'),l:'5',n:'0',o:'C+source+%231',t:'0')),k:50,l:'4',n:'0',o:'',s:0,t:'0'),(g:!((h:compiler,i:(compiler:cg112,filters:(b:'0',binary:'0',commentOnly:'0',demangle:'0',directives:'0',execute:'1',intel:'1',libraryCode:'0',trim:'1'),flagsViewOpen:'1',fontScale:14,fontUsePx:'0',j:1,lang:___c,libs:!(),options:'-O2+-c',selection:(endColumn:1,endLineNumber:1,positionColumn:1,positionLineNumber:1,selectionStartColumn:1,selectionStartLineNumber:1,startColumn:1,startLineNumber:1),source:1,tree:'1'),l:'5',n:'0',o:'x86-64+gcc+11.2+(C,+Editor+%231,+Compiler+%231)',t:'0')),k:50,l:'4',n:'0',o:'',s:0,t:'0')),l:'2',n:'0',o:'',t:'0')),version:4).

[^second-order]:  The compiler must still consider the slow/unlikely path as reachable, so the second order impact on compiler optimisations often dominates the effect of this additional instruction.

We write "C" in scare quotes because the code relies on
[inline `asm goto`](https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html#:~:text=6.47.2.7%20Goto%20Labels),
[an extension introduced in GCC 4.5](https://gcc.gnu.org/legacy-ml/gcc-patches/2009-07/msg01556.html)
and [adopted by clang 9](https://reviews.llvm.org/D69876),
and emits literal x86-64 machine code bytes.  There are fallback
implementations, but they're primarily
[meant for static analysers](https://github.com/backtrace-labs/dynamic_flag/blob/00381c2cab5c8628e6a7d18730a98f7d7e6712f2/include/dynamic_flag.h#L62),
or [to fuse flags in safe states, at compile-time](https://github.com/backtrace-labs/dynamic_flag/blob/00381c2cab5c8628e6a7d18730a98f7d7e6712f2/include/dynamic_flag.h#L61).

The library can be seen as a feature flag system, but its minimal
runtime overhead coupled with the ability to flip flags at runtime
opens up additional use cases, like disabling mutual exclusion logic
during single-threaded startup or toggling log statements. It's also
proved invaluable for crisis management, since we leave flags
(enabled by default) in established pieces of code without agonising
over their impact on application performance. These flags can serve as
ad hoc circuit breakers around full features or specific pieces of
code (e.g., to convert use-after-frees into resource leaks by nopping
destructor calls) when new inputs tickle old latent bugs.

The secret behind this minimal overhead? Cross-modifying machine code!

Guarding code with dynamic flags
--------------------------------

Include `dynamic_flag.h` in any file to register runtime-flippable
flags.  See `test/feature_flags.c` for sample usage and expected
behaviour.

Simply referring to a feature flag like

```
if (DF_FEATURE(my_kind, flag_name)) {
  ...
}
```

creates a flag named `my_kind:flag_name@[file]:[lineno]`.  The
`DF_FEATURE` expression evaluates to true if the flag is enabled,
and to false otherwise.  The compiler will assume that `DF_FEATURE()
== false` during optimisation, and the generated code will have
marginal overhead (a single `test eax, imm32` instruction) when the
flag is disabled, as expected.  When the flag is instead enabled, that
instruction becomes an unconditional `jmp rel32`.

When code should be enabled by default (e.g., when a feature is deemed
stable enough for mainline), we can instead use `DF_DEFAULT`:

```
if (DF_DEFAULT(my_other_kind, another_flag_name)) {
  ...
}
```

creates a flag named `my_other_kind, another_flag_name@[file]:[lineno]`.
The `DF_DEFAULT` expression evaluates to true when the flag is enabled,
the compiler assumes `DF_DEFAULT() == true` during optimisation, and the
generated code will have marginal overhead when the flag is enabled.  In
the unexpected case (the flag is disabled), that instruction becomes
an unconditional `jmp rel32`.

It's safe to use flags in macros, `inline`, or `static inline`
functions: each expansion site in the generated assembly is managed
independently, even if there is a name collision (but all operations
work off a flag's kind and name, so colliding flags will always be
toggled as a unit).

Flipping flag
-------------

An application that uses the dynamic flag library should invoke
`dynamic_flag_init_lib();` early during startup, before any other
call into the library.  Initialising the library gathers information
about all dynamic flags registered in the executable.  These flags
can be printed to `stdout` with
`dynamic_flag_list_state(".*", dynamic_flag_list_fprintf_cb, stdout);`

Once the library has been initialised, we can activate flags by regex
with `dynamic_flag_activate("regex pattern")`.  The regex is
implicitly anchored to the beginning of the string (but not the end):
we look for matches against each flag's name, and, since the names
look like "flag_kind:flag_name@[source file name]:[source line no]",
we usually care about prefixes.

We can deactivate flags by regex by calling
`dynamic_flag_deactivate("regex pattern")`, with the same rules for
the regex.

The `dynamic_flag` library does not stop other threads when flipping
flags at runtime, so activations and deactivations are not atomic.
However, each individual flag is always mutated atomically, so any
thread that evaluates a flag while it's being flipped will see either
a true or a false value, but not crash on an invalid instruction.

Opt-in code
-----------

When optimising checks for rare conditions (conditions so rare that
programs spend more time confirming the condition is false than
executing the cold path), it makes sense to guard the condition check
itself with a biased dynamic flag (usually false).  However, it's
always safe to let the flag be true.  That's when `DF_OPT` is useful:
a code snippet like

```
if (DF_OPT(fast_path_kind, some_name) && ctx->usually_false_field) {
  ...
}
```

is optimised like `DF_FEATURE`, and acts like `DF_FEATURE` when the
`dynamic_flag` library works correctly.  However, until the library is
initialised and finds its metadata for that `DF_OPT` flag, the
`DF_OPT` expression evaluates to true.  The `DF_OPT` macro makes sense
for code that is always safe to execute, but can be disabled for
performance.  Whenever a `usually_false_field` may be set to `true`,
we can enable all `DF_OPT(fast_path_kind, ...)` flags
programmatically.

Programmatic flag flips
-----------------------

In order to use `DF_OPT` flags, we usually need to enable all flags of
a certain kind whenever the rare condition may be true.  For example,
a server could let requests opt into detailed tracing, but know that
the vast majority of requests do not use that functionality. When
tracing requests are so rare that we spend orders of magnitude more
time checking that we wish to skip the tracing logic than we do
executing that logic, we can guard that check with `DF_OPT(request_tracing, ...)` flags.

The application now has to enable `request_tracing` flags whenever it
accepts a tracing request, and disable them once no tracing request is
in flight.  The `dynamic_flag` library supports this use case by
always counting activations: a flag is enabled whenever its activation
count is strictly positive, and deactivating a flag merely decrements
the count (until it hits zero and the flag is actually disabled).

Programmatic flag flips usually work at a coarse granularity, e.g.,
every flag in a given kind.  The application could execute
`dynamic_flag_activate_kind(request_tracing, NULL)` to activate
all flags under the `request_tracing` kind (with a pattern that
matches everything) whenever it accepts a new request that opts
into tracing, and pair that with
`dynamic_flag_deactivate_kind(request_tracing, NULL)`
when a tracing request has been fully handled (leaves the process).

Once a program plays that sort of trick, operators may want to
forcibly enable or disable a flag.

Flags activation are counted, so we can force-enable flags
interactively with `dynamic_flag_activate`: hopefully, programmatic
flag operations are paired correctly, so one manual activation
should let the activation count stay at a positive value.

We can't use `dynamic_flag_deactivate` to forcibly disable a flag:
activation counts remain at 0 instead of taking a negative value, to
make it easy to issue redundant `_deactivate` calls without leaving
the system in a weird state.

In order to prevent a flag from being activated programmatically, we
can use `dynamic_flag_unhook("regex pattern")` (and undo the call with
`dynamic_flag_rehook`): when a flag's unhook count is positive (has
been unhooked more often than rehooked), activation calls no-op
silently.  Once a flag has been unhooked, we can deactivate it
manually, and know that it won't be reactivated behind our back.

Just like flag activations, "unhook" counts do not go negative:
rehooking a flag more times than it has been unhooked is a no-op.

History
-------

This is a modified copy of `an_hook` from
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

This new interface is also distributed under the Apache license v2.
