Bake cache maintenance (Windows / .NET 10)

Build:
  dotnet build tools/bake-cache-maintenance -c Release

Plan only (default), using explicit diagnostic cache roots and a fresh JSON report:
  dotnet run --project tools/bake-cache-maintenance -c Release -- --root D:/reports --report D:/dedup-plan.json

Add --apply to consolidate identical payloads with NTFS hard links. The utility:
- Considers only hexadecimal .bake filenames; no directory, map or authoring document is removed.
- Compares exact SHA-256 content within same-key/size groups and the same volume.
- Writes the complete plan before replacing file entries, preserves all original paths, and verifies file identity.
- Skips reparse points, checks absolute target paths against the explicit roots, and refuses changed files.
- Records byte counts freed when the final standalone link to a duplicate payload is replaced.

Use only immutable bake payloads whose writers replace files atomically (as OpenRemix's cache writer does).
Changing shared bytes in place would change all linked copies. Live caches need not be included; pass only
diagnostic report roots to preserve the installed testing cache independently. No roots are discovered automatically.

Self-test, with one fresh root and report:
  dotnet run --project tools/bake-cache-maintenance -- --self-test --root D:/reports/new-fixture --report D:/reports/new-fixture.json

Build outputs live under the repository's _work/build/bake-cache-maintenance directory.
