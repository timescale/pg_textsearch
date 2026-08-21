# Scripts

## `package-deb.sh`

Builds a Debian package for pg_textsearch.

## `msvc-build.ps1`

Builds `pg_textsearch.dll` with MSVC against a PostgreSQL 17 or 18 x64 tree,
on Windows. PGXS cannot drive MSVC, so this script is the Windows build
recipe; it reads `OBJS` and `DATA` from the `Makefile` instead of keeping a
second source list. Requires Visual Studio 2022 Build Tools with the
"Desktop development with C++" workload.

```powershell
.\scripts\msvc-build.ps1 -PgRoot 'C:\Program Files\PostgreSQL\18'
```

`-PgRoot` is any installation or extracted EDB binaries tree containing
`include\server` and `lib\postgres.lib`. Output goes to `-WorkDir`, by default
`target\msvc`; nothing is written into `-PgRoot` and nothing is installed.
`-ExpectedMajor 17|18` refuses a tree of another major version.

The build compiles with `/W3 /WX` and suppresses three conversion warnings
(C4244, C4267, C4305) that already appear on `main`; every other warning
fails the build. After compilation two gates run: `src/layout_check.c` stops
the compile if MSVC lays out a packed or aligned on-disk struct differently
from the reference GCC x86-64 build, and the export gate stops the build if
the DLL does not export a symbol that the `DATA` SQL files use through
`MODULE_PATHNAME`.

Only `OBJS` and `DATA` are read from the `Makefile`; the version comes from
`pg_textsearch.control`. Everything else that affects the build lives in two
places: a semantic `-D` or `-I` added to `PG_CPPFLAGS`, or a link dependency
added via `SHLIB_LINK`, must be mirrored in `scripts/msvc-build.ps1` in the
same change, or the Windows DLL diverges from the PGXS build.

The `build-msvc` job in `.github/workflows/ci.yml` runs the same script
against EDB binary zips pinned by URL and SHA-256. When EDB replaces the
release and the URL stops working, pick the new URL, compute its hash, and
update URL and hash together in one commit; never change one side of a pin
alone. If the hash does not match while the URL is unchanged, treat it as an
incident and do not update the pin until the replacement archive's provenance
has been independently verified.
