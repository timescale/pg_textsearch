# Upgrading to pg_textsearch v1.5.1

An upgrade has two parts: restarting PostgreSQL with the new library, then
updating the extension in each database. Some older indexes also need a
rebuild.

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

## Do I need to rebuild my indexes?

Use the binary version each index was last **built or rebuilt with**, not
just the currently installed SQL version. "Metapage" means the index's
header format.

| Built or rebuilt with | Metapage at build | Rebuild for v1.5.1? |
|-----------------------|-------------------|--------------------|
| v0.5.0 or earlier | <=5 | Yes: incompatible format |
| v0.5.1–v1.2.0 | 6 | If unspilled documents may be missing |
| v1.3.0 | 7 | For Boolean completeness; see below |
| v1.3.1–v1.4.0 | 8 | For Boolean completeness; see below |
| v1.5.0–v1.5.1 | 9 | No, for indexes built under these versions |

**Pre-v1.3 indexes:** documents left in the old memtable (not yet written
into segments) can be missing after changing binaries. If keeping these
indexes, stop writes and spill pending data with the old version before
switching binaries, then check that expected documents are still found.
If completeness is uncertain, REINDEX rebuilds from the table. A warning
about incomplete results after a pre-v1.3 upgrade is a reason to check;
it can also reflect stale bookkeeping rather than actual missing rows.

**Boolean queries:** indexes built before v1.5.0 omitted empty and
stopword-only documents. Rebuild if you need complete results for queries
such as `!missing` or `alpha | !missing`, which can match those documents.
Ordinary positive-term ranked searches do not need a rebuild for this
reason. Neither does a positively anchored query like `alpha & !missing`.

**If you've upgraded before:** a header can upgrade automatically without
rebuilding the index or fixing either completeness issue. Even a version-9
header is not enough to prove that an older index contains all documents.

There is currently **no supported non-superuser SQL function to inspect
this state**. `bm25_dump_index()` shows the header version, but requires
superuser. If you don't know an index's history, a rebuild removes the
uncertainty.

## Upgrade

1. **Install v1.5.1 and restart PostgreSQL.** Install both the library and
   its SQL upgrade files. On a managed service, your provider handles this
   step. Check that `loaded_version` is now `1.5.1`.

   **With replicas:** a v1.5.x server can change an index's format while
   writing to or maintaining it. Once that happens, older servers cannot
   read that index. This can happen before `ALTER EXTENSION`, so delaying
   that command does not make it safe to leave old and new servers using
   the same indexes. Upgrade all servers that can access those indexes
   before letting v1.5.x write to or maintain them. Switching back to an
   older binary will not make a changed index readable again.

2. **Update each database where the extension is installed:**

   ```sql
   ALTER EXTENSION pg_textsearch UPDATE TO '1.5.1';
   ```

   PostgreSQL runs the intermediate upgrade scripts for you. You do not
   need to issue ALTER for each intervening release. Versions older than
   v0.2.0 and development versions need a separate migration plan; they
   are not covered by the stable upgrade tests.

3. **Rebuild the indexes that need it:**

   ```sql
   REINDEX INDEX public.docs_bm25;
   ```

   Substitute your index name. This preserves its definition, but standard
   REINDEX blocks writes to the table while it runs. ALTER alone does not
   rebuild indexes or recover missing documents.

4. **Check the result.** Both versions should now read `1.5.1`. Run familiar
   searches, confirm EXPLAIN uses the BM25 index, and check for missing
   expected documents or upgrade warnings. If you use negative Boolean
   queries, include empty and stopword-only rows in those checks.
