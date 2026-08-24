# Vendored Maven artifacts

This is a small in-repo **Maven repository** holding third-party artifacts we don't
want to depend on an external server for. It's wired up in `settings.gradle`:

```groovy
maven {
    url = uri("${rootDir}/vendor/maven")
    content { includeGroup 'io.github.joshuatam' }   // only serves this group
}
```

Transitive dependencies of these artifacts (ktor, okhttp, protobuf, okio, coroutines,
kotlin-stdlib, …) are **not** vendored — they resolve from Maven Central as usual.
Only the artifacts below live here.

## What's here

### `io.github.joshuatam:javasteam` + `:javasteam-depotdownloader` — `1.8.0.1-26-20260801.180149-1`

GameNative's fork of JavaSteam (`joshuatam/JavaSteam @ gamenative-latest`). Its
depot-downloader disk-spools download chunks to temp files instead of holding them in
RAM, which fixes the large-game download OOM crash (issue **#408 / #380** — HITMAN WoA
~87 GB OOM'd at every speed tier on the public `in.dragonbra:javasteam:1.8.0`).

- **Device-proven** by the #408 reporter (HITMAN 87 GB downloaded to completion at the
  blazing tier). Proven CI build: run `32710611783`.
- Originally published **only** as a mutable `-SNAPSHOT` on Sonatype's snapshot repo
  (`central.sonatype.com/repository/maven-snapshots/`). We first pinned to the exact
  timestamped build (`…-20260801.180149-1`, buildNumber 1 — the only build that exists),
  then vendored the files here so a Sonatype snapshot purge can never break our build.
- The files are mirrored **verbatim** from Sonatype, including the `.pom`, Gradle
  `.module`, `maven-metadata.xml`, and all `.md5/.sha1/.sha256/.sha512` sidecars, under
  the original `1.8.0.1-26-SNAPSHOT/` directory. Because the dependency is pinned to the
  timestamped version, Gradle resolves it as a fixed (non-changing) artifact.

## How to update (when we intentionally bump the engine)

1. Pick the new fork version and find its timestamped snapshot build via
   `…/io/github/joshuatam/<artifact>/<X>-SNAPSHOT/maven-metadata.xml`.
2. Mirror the full `<X>-SNAPSHOT/` directory for **both** artifacts here — every
   `.jar/.pom/.module/maven-metadata.xml` plus their `.md5/.sha1/.sha256/.sha512`.
3. Bump the two `io.github.joshuatam:…` versions in `app/build.gradle`.
4. Delete the old version's files (this is the only time we touch them — "saved forever
   until we need to update").
5. Push to a branch and let CI resolve+compile before merging.

Do **not** edit any file in this tree by hand — the checksums must match the bytes.
