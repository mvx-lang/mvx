Fixtures for the dependency half of the install (mvx#218) and the
native-library check (mvx#219), driven by `deps.cmake` from `scripts/test.sh`.

`mvpkg.json` is the shape of a real published manifest: a dependency that
applies everywhere (`curl`), one that is filtered off this system
(`json@!mvx`, built into the MVX runtime), and optional ones the install must
leave alone.

The end-to-end half of the test builds its packages into a temporary directory
and serves them over `file://` through `MVX_INSTALL_PKGBASE`, so the download,
the checksum, the native check, the EXPORTS merge and the dependency walk all
run without a network.
