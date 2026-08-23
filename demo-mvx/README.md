# demo-mvx — an MVX-only demo account

What will not travel. The programs here depend on MVX terminal support that
is not available on every MultiValue platform — `MOUSE-DEMO` needs mouse
reporting, which UniData has through curses and UniVerse has not — so they
would fail as a portability example rather than serve as one.

The cross-platform demo account is the `demo/` submodule
([mvx-lang/demo](https://github.com/mvx-lang/demo)); anything that should run
everywhere belongs there, not here.

```sh
build/bin/mvx -a demo-mvx
:MOUSE-DEMO
```
