# Upgrading pg_textsearch

An upgrade has two parts: restarting PostgreSQL with the new library, then
updating the extension in each database. Some upgrades also need an index
rebuild. The common procedure is below; the version notes record additional
requirements.

## Check your versions

```sql
SELECT
    current_setting('pg_textsearch.library_version', true) AS loaded_version,
    (SELECT extversion FROM pg_extension
     WHERE extname = 'pg_textsearch') AS installed_version;
```

`loaded_version` is the binary running in the server. `installed_version`
is the extension's SQL version in this database. They can differ. If the
loaded version is NULL, ask your database operator to check the installation
and preload configuration before proceeding.

## Check the requirements for your upgrade

Read the [version notes](#version-notes) for the changes you are crossing
**before replacing the old library**. For example, when upgrading from
v1.2.0 to v1.5.1, read all the notes from v1.3.0 through v1.5.1. You do not
need to install each intermediate binary.

Existing indexes created by older versions may or may not need to be
rebuilt after upgrading the extension. If an index was created under an
older version and has never been rebuilt, check the
[compatibility matrix](#compatibility-matrix) and all the
[intervening version notes](#version-notes) too. Upgrading the extension can
update the index’s header without rebuilding its contents.

## Upgrade

1. **Install the target release and restart PostgreSQL.** Install both the
   library and its SQL upgrade files. On a managed service, your provider
   handles this step. After restarting PostgreSQL, check that
   `loaded_version` matches your desired target version using the SQL above.

   With replicas, check the version notes before allowing the new server
   to write to or maintain indexes that older servers still use.

2. **Update each database where the extension is installed:**

   ```sql
   ALTER EXTENSION pg_textsearch UPDATE TO 'TARGET_VERSION';
   ```

   Replace `TARGET_VERSION` with the release you installed. PostgreSQL runs
   the available intermediate SQL upgrade scripts for you; you do not need
   to issue ALTER for each intervening release.

3. **Rebuild any indexes required by the version notes:**

   ```sql
   REINDEX INDEX public.docs_bm25;
   ```

   Substitute your index name. NOTE: standard `REINDEX` blocks writes to the
   table while it runs. `ALTER EXTENSION` alone does not rebuild indexes.

4. **Check the result.** Both versions should now match your target. Run
   familiar searches, confirm EXPLAIN uses the BM25 index, and check for
   missing expected documents or upgrade warnings.

## Compatibility matrix

"Metapage" means the index's header format. This table shows which header
versions each listed reader accepts. A Yes does not establish complete
query results; the version notes explain the rebuild cases.

| Metapage | Written at build by | v1.3.0 | v1.3.1 / v1.4.0 | v1.5.0 / v1.5.1 |
|----------|---------------------|--------|-----------------|-----------------|
| <=5 | v0.5.0 or earlier | No | No | No |
| 6 | v0.5.1–v1.2.0 | Yes | Yes | Yes |
| 7 | v1.3.0 | Yes | Yes | Yes |
| 8 | v1.3.1–v1.4.0 | No | Yes | Yes |
| 9 | v1.5.0–v1.5.1 | No | No | Yes |

The build column refers to the binary used for the last full build or
rebuild. The header may have advanced since then. Extend this table as
releases add formats or change reader support.

## Version notes

### Earlier releases

Indexes built with v0.5.0 or earlier use header versions that the readers
listed above cannot read. These indexes must be rebuilt. Once the target
extension version is installed and working, use `REINDEX`, or create
replacement indexes and drop the old ones.

### v1.3.0: unspilled documents from older indexes

Documents left in the pre-v1.3 memtable (not yet written into segments) can
be missing after changing binaries. If keeping these indexes, stop writes
and spill pending data with the old version before switching binaries.
Then check that expected documents are still found. If completeness is
uncertain, REINDEX rebuilds from the table.

The legacy docid pointer can also remain as stale bookkeeping after a
successful spill. Its presence, or a later release's migration warning,
is not proof that rows are missing.

### v1.3.1: header version 8

New indexes use header version 8. Existing version-6 and version-7 headers
upgrade when the new binary changes them. This format change alone does
not require REINDEX.

A v1.3.0 server cannot read version-8 headers. All servers that use an index
must be running a compatible binary before v1.3.1 changes its header. This
can happen before `ALTER EXTENSION`. Switching back to v1.3.0 does not undo
the format change.

### v1.5.0: Boolean completeness and a new header format

**Boolean queries:** indexes built before v1.5.0 omitted empty and
stopword-only documents. Rebuild if you need complete results for queries
such as `!missing` or `alpha | !missing`, which can match those documents.
Ordinary positive-term ranked searches do not need a rebuild for this
reason. Neither does a positively anchored query like `alpha & !missing`.
Include empty and stopword-only rows when checking negative Boolean queries.

An automatic upgrade to header version 9 does not backfill those documents.
Upgrading SQL or adding new rows does not backfill them either.

**Replicas and rollback:** v1.5.0 and v1.5.1 can change an older index to
header version 9 while writing to or maintaining it. Pre-v1.5 servers
cannot read that format. Upgrade all servers that use those indexes before
letting a new server change them. This can happen before `ALTER EXTENSION`;
delaying that command does not avoid the problem. Switching back to the old
binary does not undo the format change.

### v1.5.1

No additional index-format changes from v1.5.0.

In this release, there is no supported non-superuser SQL function to inspect
an index's header state. `bm25_dump_index()` shows the header version, but
requires superuser. If an index's history is unknown, rebuilding it removes
the uncertainty about omitted documents.
