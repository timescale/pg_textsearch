# Support expression indexes

Design document for implementing expression index support in 
pg_textsearch.

## Goals

Currently, pg_textsearch can only index text columns. Adding expression
index support lifts this restriction and allows indexes to be created on
any expression that can be evaluated to a string.

For example:

```sql
CREATE TABLE product_logs (id serial PRIMARY KEY, data jsonb);

INSERT INTO product_logs (data) VALUES
('{"title": "Super Widget", "details": "High performance widget for database engineers"}'),
('{"title": "Mega Gadget", "details": "Optimized gadget with low latency"}')

CREATE INDEX idx_json_details ON product_logs
USING bm25 ((data->>'details'))
WITH (text_config='english');
```

## Implementation proposal

### Index build

#### Bulk build

In tp_build(), index builds for expression indexes are currently explicitly
disabled. This restriction needs to be removed.

```c
if (indexInfo->ii_IndexAttrNumbers[0] == 0)
{
      ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
               errmsg("BM25 indexes on expressions are not supported"),
               errhint("Create the index on a column directly, e.g., "
                     "CREATE INDEX ... USING bm25(content)")));
}
```

##### Document text extraction

Since the index may be built on an expression rather than a direct column,
the executor must be invoked to evaluate the expression and obtain the
document text.

This will be done using the utility function 
`FormIndexDatum(indexinfo, slot, estate, values, isnull)`. The pattern looks like this:

```c
estate	 = CreateExecutorState();
econtext = GetPerTupleExprContext(estate);
bool		isnull[INDEX_MAX_KEYS];
Datum		text_datum[INDEX_MAX_KEYS];

GetPerTupleExprContext(estate)->ecxt_scantuple = slot;
FormIndexDatum(index_info, slot, estate, text_datum, isnull);

/* Then we check if the value is NULL via isnull[0] */
/* non-NULL value can be extracted from text_datum[0] */
```

Both serial and parallel index build follow this pattern.

In the current parallel build implementation, we store the `attnum` in 
`TpParallelBuildShared`, once we add expression index support, this is 
no longer needed.

#### Insertion

During insertion, PostgreSQL already evaluates the index expression and 
passes the result via the values argument. Therefore, no changes are 
required in `tp_insert()`.

### Index resolution

Users are allowed to write queries without explicitly specifying which
bm25 index to use:

```sql
SELECT * FROM table
ORDER BY column <@> 'query string';

SELECT * FROM table
ORDER BY column <@> bm25query('query string');
```

This works because our planner hook locates a suitable bm25 index 
and rewrites the query to use it.

For simple column indexes, we extract relation OID and attribute number from the
Var node and scan `pg_index` for bm25 indexes on that column.

For expression indexes, we:

1. Collect all Var nodes from the expression using `pull_var_clause()`.
2. Skip the index resolution if 
   * there are no Vars 
   * Vars belong to different relations

     > Postgres does not allow you to create an index for more than 2 relations

   * any Var is from an outer query level (varlevelsup != 0).

4. Scan `pg_index` for bm25 indexes on that relation. For expression indexes
   (indkey = 0), we compare the query expression tree against `pg_index.indexprs`
   using `equal()`.

`pg_index.indexprs` stores Var nodes with varno = 1. However, in queries
involving multiple relations, the same column reference may have a different
varno:

```sql
-- json_column's varno is 1
CREATE INDEX idx ON bar USING bm25 (json_column->'content')

SELECT * FROM
foo JOIN bar
ON ...
-- rtable: [foo, bar]
-- so json_column's now has varno 2!
ORDER BY (json_column->'content') <@> 'query'
```

As a result, the two expression trees cannot be compared directly. To 
fix this, we normalize the stored expression (`pg_index.indexprs`) to 
the query's RT index using `ChangeVarNodes()` before comparison.

### Crash recovery

Same as index build, while re-indexing the heap tuples, we need to invoke the
executor to evalute the expression. This part follows the pattern mentioned
earlier.

### Explicit index validatioin

When user explicitly specify the index to use, we need to validate that index
is actually built for the referenced column/expression.

PR [#195](https://github.com/timescale/pg_textsearch/pull/195) implemented this 
for column index, the logic needs to be extended to support expression index:

1. Get the index OID
2. Find the corresponding pg_index tuple
3. Check if this index is built on the specified table
4. Depending on the LHS of <@>:
   1. a column (Var), check pg_index.indkey
   2. an expr, check pg_index.indexprs

Much of the required logic is similar to what we already do during index
resolution:

1. Extract relation OID from expr, check edge cases
2. Normalize `Var.varno`

This code could likely be reused or factored out to avoid duplication.

### Tests

We should cover the following cases:

* expression index, serial build
* expression index, parallel build
* expression index, crash recovery
* Index build edge case
  * expression evaluation result should be a string
* Index resolution
  * expr <@> 'query string'
  * expr <@> bm25query('query string')
* Index resolution edge cases
  * expr does not reference any relation, i.e., no Var nodes
  * expr references multiple relations
  * expr contains correlated subquery (Var.varlevelsup != 0)