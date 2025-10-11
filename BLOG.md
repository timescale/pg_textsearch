# `pg_textsearch`: Modern Ranked Text Search for Postgres (Preview)

Author: [Todd Green (TJ)](mailto:tj@tigerdata.com)
Date: Oct 2, 2025
Editors: [Matty Stratton](mailto:matty@tigerdata.com)[Jacky Liang](mailto:jacky@tigerdata.com)

Resources:

1. [Claude conversation](https://claude.ai/share/2fa9b023-ea49-444c-80d5-48be85b4f849)

TODO

- [ ] [Thumbnail for blog](https://app.asana.com/1/341543771842144/project/1210975649412537/task/1211538760184192?focus=true)
- [ ] Nice to have: interactive diagrams that showcase some technical examples (use Claude Code to generate [three.js](http://three.js) embeddables)

---

*Preview Release (v0.0.2)*

\[ blog thumbnail goes here \]

# Classical Postgres Full-Text Search

#

# .. Postgres Full-Text Search (FTS) starts strong, handling simple tasks and smaller datasets with ease\! It's a fantastic solution for getting things done quickly. However, as your projects grow and search needs become more demanding, you might find that its initial charm gives way to some growing pains.

## The Scalability Cliff

Imagine you're building a customer support system that stores conversation history. You start with 50,000 conversations and Postgres's built-in full-text search works fine for finding relevant past interactions. Queries return in 50 milliseconds.

A year later, you have 10 million conversations. Your support agents type a customer's question into the search box and wait. And wait. Queries that rank results by relevance now take 25-30 seconds. The feature becomes unusable.

This isn't a hypothetical scenario. Developers running Postgres full-text search at scale consistently report hitting a wall where performance degrades catastrophically. One developer reported queries going from under 1 second to 25-30 seconds on just 800,000 rows when using ts\_rank for relevance sorting [sql \- Postgres Full Text Search \- Issues with Query Speed \- Stack Overflow](https://stackoverflow.com/questions/66215974/postgres-full-text-search-issues-with-query-speed). Another team found that performance deteriorates significantly around 1 to 2 million rows [Full text search in milliseconds with PostgreSQL | May 01, 2015](https://www.lateral.io/resources-blog/full-text-search-in-milliseconds-with-postgresql).

## Ranking is Second-Rate

Now imagine you’re building a ranked search feature — not just keyword matching, but actual relevance ordering. You reach for `ts_rank` or `ts_rank_cd` and quickly notice three issues:

1. **Mediocre ranking quality.** Postgres’s ranking functions apply basic term‑frequency normalization and optional weighting, but they don’t include modern ranking signals like inverse document frequency (IDF), term‑frequency saturation, or length normalization. Common words dominate, long documents win, and truly relevant matches can fall below noise.
2. **Brittleness in keyword coverage.** To exploit GIN or GiST indexes, queries must use the `@@` operator — a Boolean match that requires all specified terms to appear. But then a document missing even one term, though otherwise relevant, is excluded entirely before ranking. The search becomes fragile: great candidates vanish due to strict Boolean logic.
3. **Unpredictable index benefit.** Postgres indexes help only when query terms are selective enough to cut down the match set. If you search for common terms (“database”, “query”, “search”), the Boolean filter yields huge match sets, forcing Postgres to compute `ts_rank` for every candidate. The index can’t save you.

Together, these problems define the **ranked search gap** in native Postgres FTS. It’s not just that it slows down — it’s that it can’t produce *high‑quality* ranked results efficiently.

**Why native FTS struggles with ranked search**
Postgres indexes (`GIN`/`GiST`) help `@@` find matches but **don’t rank them**. Ranking with `ts_rank` or `ts_rank_cd` happens *after* Boolean filtering, which means documents missing one query term are excluded before ranking. As datasets grow, ranking each remaining match’s `tsvector` becomes prohibitively expensive, limiting recall and driving up latency.

## **Two Difficult Paths**

When developers need full-text search in Postgres that actually scales, they typically choose between accepting limitations or adding operational complexity:

**Path 1: Stick with native Postgres FTS and accept its limitations**

Postgres's built-in `tsvector` and `tsquery` provide a solid entry point for full-text search. But as applications grow, developers run into problems with both search quality and performance:

*Search quality suffers from algorithmic limitations*. Postgres's `ts_rank` and `ts_rank_cd` functions lack the corpus-aware signals that modern search engines use. They don't apply inverse document frequency weighting, so rare terms that should indicate relevance are treated the same as common ones. They don't use term frequency saturation, allowing documents with excessive repetition to dominate results. And they don't normalize by corpus-average document length, causing long documents to be favored unintentionally. BM25 uses more input signals and is based on better heuristics, typically not requiring manual tuning [Full-text search engine with PostgreSQL (part 2): Postgres ...](https://xata.io/blog/postgres-full-text-search-postgres-vs-elasticsearch).

*Performance hits a wall at scale*. When you need to sort millions of rows by ts\_rank, Postgres must score every matching document, and there's no efficient way to retrieve just the top-N most relevant results [Comparing Native Postgres, ElasticSearch, and pg\_search for Full-Text Search \- Neon](https://neon.com/blog/postgres-full-text-search-vs-elasticsearch). Ranking requires consulting the tsvector of each matching document, which can be I/O bound and therefore slow [Full-text search engine with PostgreSQL (part 2): Postgres ...](https://xata.io/blog/postgres-full-text-search-postgres-vs-elasticsearch). What starts as a 50ms query on 10,000 rows becomes a multi-second operation on 10 million rows.

*Write-heavy workloads become painful*. Each table update cascades into many index writes due to Postgres's MVCC approach, causing index bloat that accumulates over time [Lessons Learned From 5 Years of Scaling PostgreSQL](https://onesignal.com/blog/lessons-learned-from-5-years-of-scaling-postgresql/). The GIN indexes used for full-text search maintain a "pending list" that can cause unpredictable performance spikes during writes.

**Path 2: Move to Elasticsearch and deal with operational complexity**

The traditional escape hatch is moving search to a dedicated system like Elasticsearch. You get modern BM25 ranking, sub-second queries at scale, and all the search engine features you could want.

But you also get:

* **Data synchronization headaches**: Keeping Postgres and Elasticsearch in sync becomes its own distributed systems problem
* **Operational burden**: Another service to monitor, tune, upgrade, and troubleshoot
* **Infrastructure costs**: Running separate search infrastructure, data transfer between systems, and the engineering time to maintain it all
* **Increased complexity**: Your codebase now spans two data systems with different query languages

## **Agentic search has made this even more importantWhy This Matters Now**

Full-text search has renewed significance in modern agentic system architectures. Chat agents need to store and retrieve memories to keep user interactions coherent across time. Sales bots need to search product information and inventory based on natural language descriptions. RAG systems—now just another tool in the agentic toolkit—require fast keyword search over document collections.

One of the most compelling aspects of Postgres for builders is its flexibility in supporting diverse workloads. You can run transactional data, time-series data, and vector embeddings in one place. The wide adoption of pgvector demonstrates the appeal of this consolidation—developers would rather add an extension than run separate infrastructure.

pg\_textsearch brings this same philosophy to ranked keyword search. It provides modern BM25-based full-text search that complements vector search extensions like pgvector, letting you build hybrid search systems (combining semantic and keyword search) entirely within Postgres.

## **How pg\_textsearch Works**

pg\_textsearch implements BM25 ranking using a memtable architecture. The core idea is simple: store an inverted index in Postgres shared memory, where terms map to lists of documents containing those terms.

The implementation uses Postgres Dynamic Shared Areas (DSA) to manage the in-memory index. When you create a pg\_textsearch index on a text column, the extension builds an inverted index structure with three main components:

A term dictionary using dshash for string interning. Each unique term appears once, with a pointer to its posting list.

Posting lists stored as DSA-allocated vectors of (ItemPointerData, term frequency) pairs. These lists grow dynamically as documents are added and are sorted by tuple ID when needed for efficient lookups.

A document length table mapping each document to its length, enabling proper BM25 normalization.

The extension also maintains corpus statistics—total document count and average document length—in shared index state. These statistics are critical for computing inverse document frequency (IDF) and length normalization in the BM25 formula.

Here's the memtable structure:

\[replace the following ASCII diagram w/ one made by the design team\]

```
┌─────────────────────────────────────────────────┐
│              Memtable (Shared Memory)           │
├─────────────────────────────────────────────────┤
│                                                 │
│  ┌──────────────────────────────────────┐      │
│  │  Term Dictionary (dshash)            │      │
│  │  "database" → [stats, ptr]           │      │
│  │  "search"   → [stats, ptr]           │      │
│  │  "query"    → [stats, ptr]           │      │
│  └──────────┬───────────────────────────┘      │
│             │                                   │
│             ├─→ Posting List (vector)           │
│             │   [(page1,off3, tf=2),           │
│             │    (page2,off7, tf=1),           │
│             │    (page5,off2, tf=3)]           │
│             │                                   │
│  ┌──────────────────────────────────────┐      │
│  │  Document Lengths (dshash)           │      │
│  │  (page1,off3) → 245                  │      │
│  │  (page2,off7) → 189                  │      │
│  │  (page5,off2) → 412                  │      │
│  └──────────────────────────────────────┘      │
│                                                 │
│  Corpus Stats: total_docs, avg_doc_length      │
└─────────────────────────────────────────────────┘
                    │
                    │ (crash recovery)
                    ↓
┌─────────────────────────────────────────────────┐
│         Index Metapages (on disk)               │
│         List of document IDs                    │
│    (memtable rebuilt on restart from this)     │
└─────────────────────────────────────────────────┘
```

**Query Evaluation**

When you run a query like:

sql

```sql
SELECT * FROM documents
ORDER BY content <@> to_tpquery('database performance', 'docs_idx')
LIMIT 10;
```

The extension processes it as follows:

For each query term, look up the term in the string dictionary and retrieve its posting list. For each document that contains at least one query term, calculate the BM25 contribution for each term present. The BM25 formula uses term frequency, document length, and corpus statistics to compute a relevance score. Sum the contributions across all query terms to get the final document score.

The `<@>` operator returns negative BM25 scores. This design choice allows Postgres to use ascending index scans efficiently—lower (more negative) scores indicate better matches.

**Staying Close to Postgres**

One of the design principles behind pg\_textsearch is to reuse Postgres infrastructure whenever it makes sense. The extension doesn't reinvent text processing—it uses Postgres's `to_tsvector` under the hood for tokenization, stemming, and stop-word elimination. This means you get all the language support that Postgres already provides (English, French, German, and dozens more) without additional work.

The extension also integrates naturally with SQL. You can combine full-text search with standard WHERE clauses, JOINs, and aggregations. For example:

sql

```sql
SELECT category, COUNT(*)
FROM articles
WHERE content <@> to_tpquery('machine learning', 'idx_articles') > -2.0
  AND published_date > '2024-01-01'
GROUP BY category;
```

This query finds articles about machine learning published since the start of 2024, grouped by category. The full-text search condition works alongside temporal filtering without needing to move data to a separate system.

**Technical Decisions and Trade-offs**

The memtable approach optimizes for write performance. Insertions are fast—just append to posting lists in shared memory. There's no complex disk I/O or log-structured merge operations during writes. The trade-off is a memory constraint: each index is limited by the `pg_textsearch.index_memory_limit` GUC (default 64MB, configurable up to available shared memory).

For the preview release, this constraint is acceptable. The entire user interface is functional, allowing developers to test the extension and integrate it into their applications immediately. For workloads like TigerData's internal documentation search (powering the eon Slackbot that answers questions about our products), main memory is sufficient.

Future releases will add disk-based segments to scale beyond memory limits. The string-based architecture in the current implementation was specifically chosen to be compatible with these future disk segments—no global term ID namespace to manage, making it straightforward to merge in-memory and on-disk posting lists.

The preview implementation uses a straightforward query evaluation algorithm. Future versions will incorporate state-of-the-art optimizations like Block-Max WAND for efficient top-k retrieval. For now, the relatively small size of the memtable makes a simpler approach both practical and preferable—we're optimizing for fast writes, and relying on in-memory speed to handle queries efficiently enough for typical use cases.

**Why Not Just Use...?**

You might wonder why not build on top of GIN indexes or embed an existing search library like Tantivy (as pg\_search does).

GIN indexes aren't designed for BM25 ranking. They excel at matching but lack the corpus statistics and scoring infrastructure needed for relevance ranking. Adding BM25 on top would require maintaining separate data structures anyway.

Embedding Tantivy would provide a full-featured search engine but at the cost of complexity. pg\_textsearch aims for something simpler and more focused: strong BM25 ranking without the bells and whistles of a complete search platform. The result is easier to understand, easier to maintain, and easier to integrate with Postgres's query planner and execution engine.

## **Early Performance Results**

The preview release prioritizes getting the complete user interface functional over exhaustive optimization. That said, performance on moderate-sized datasets is already quite good.

For the Timescale documentation corpus—roughly 10MB of indexed data after processing—indexing completes in a few seconds on a development laptop. Queries return in single-digit milliseconds. These tests ran on a MacBook with debug builds of both pg\_textsearch and PostgreSQL 17, configured using timescaledb-tune. Production builds on properly provisioned hardware should perform better.

**Memory Usage**

The size of the memtable depends primarily on the number of distinct terms in your corpus. The Timescale docs dataset produces a roughly 10MB index. For comparison, a corpus with longer documents or more varied vocabulary will require more memory per document.

The default memory limit is 64MB per index, configurable via the `pg_textsearch.index_memory_limit` GUC. In the preview, this limit is not strictly enforced, so you may be able to index larger collections depending on your available shared memory. Results will vary based on document characteristics and system configuration.

**What to Expect**

Index builds are fast because writes simply append to in-memory posting lists. Query performance benefits from everything being in shared memory—no disk seeks, no page cache misses.

Later releases will include systematic benchmark evaluations against standard datasets and comparisons with other approaches. For now, the preview demonstrates that BM25 ranking in Postgres can be both practical and performant for workloads that fit in memory.

## **Getting Started on Tiger Cloud**

pg\_textsearch is available to all Tiger Cloud customers, including those on the free plan. For new services, simply enable the extension:

sql

```sql
CREATE EXTENSION pg_textsearch;
```

For existing services, the extension may not be available until after your next scheduled maintenance window. You can also manually pause and restart your service to pick up the update immediately.

**Important**: This is a preview release. We recommend experimenting with pg\_textsearch on development or staging environments, not production instances.

## **A Complete Example: Product Search**

Let's build a simple product catalog with searchable descriptions:

sql

```sql
CREATE TABLE products (
    id serial PRIMARY KEY,
    name text,
    description text,
    category text,
    price numeric
);

INSERT INTO products (name, description, category, price) VALUES
    ('Mechanical Keyboard', 'Durable mechanical switches with RGB backlighting for gaming and productivity', 'Electronics', 149.99),
    ('Ergonomic Mouse', 'Wireless mouse with ergonomic design to reduce wrist strain during long work sessions', 'Electronics', 79.99),
    ('Standing Desk', 'Adjustable height desk for better posture and productivity throughout the workday', 'Furniture', 599.99),
    ('Noise Cancelling Headphones', 'Premium wireless headphones with active noise cancellation for focused work', 'Electronics', 299.99),
    ('Office Chair', 'Ergonomic office chair with lumbar support and adjustable armrests for all-day comfort', 'Furniture', 449.99);
```

Create a BM25 index on the description column:

sql

```sql
CREATE INDEX products_search_idx ON products
USING pg_textsearch(description)
WITH (text_config='english');
```

Now search for products related to ergonomics and work:

sql

```sql
SELECT name, description,
       description <@> to_tpquery('ergonomic work', 'products_search_idx') as score
FROM products
ORDER BY description <@> to_tpquery('ergonomic work', 'products_search_idx')
LIMIT 3;
```

Results:

```
           name            |                                  description                                   |  score
---------------------------+--------------------------------------------------------------------------------+-----------
 Ergonomic Mouse           | Wireless mouse with ergonomic design to reduce wrist strain during long work... | -4.234567
 Office Chair              | Ergonomic office chair with lumbar support and adjustable armrests for all-... | -3.891234
 Standing Desk             | Adjustable height desk for better posture and productivity throughout the w... | -1.456789
```

Note that `<@>` returns negative BM25 scores—lower (more negative) values indicate better matches. This design allows Postgres to use ascending index scans efficiently.

**Filtering by Score Threshold**

You can use score thresholds in WHERE clauses to filter results:

sql

```sql
SELECT name, description <@> to_tpquery('wireless', 'products_search_idx') as score
FROM products
WHERE description <@> to_tpquery('wireless', 'products_search_idx') < -2.0;
```

This returns only documents that score better than \-2.0 for the query "wireless". Keep in mind that absolute score values are not particularly meaningful—what matters is the relative ranking. Use thresholds mainly to exclude low-relevance results.

**Combining with Standard SQL**

Full-text search works naturally with other SQL operations:

sql

```sql
SELECT category, name,
       description <@> to_tpquery('ergonomic', 'products_search_idx') as score
FROM products
WHERE price < 500
  AND description <@> to_tpquery('ergonomic', 'products_search_idx') < -1.0
ORDER BY description <@> to_tpquery('ergonomic', 'products_search_idx')
LIMIT 5;
```

This query finds ergonomic products under $500, combining full-text search with price filtering.

**Verifying Index Usage**

Check that your queries are using the index with EXPLAIN:

sql

```sql
EXPLAIN SELECT * FROM products
ORDER BY description <@> to_tpquery('wireless keyboard', 'products_search_idx')
LIMIT 5;
```

For small datasets, PostgreSQL may prefer sequential scans. You can force index usage for testing:

sql

```sql
SET enable_seqscan = off;
```

Important: Even if EXPLAIN shows a sequential scan, the `<@>` operator always uses the index for corpus statistics (document counts, average length) required for BM25 scoring. The index is consulted regardless of the scan method chosen by the planner.

**Language Support**

pg\_textsearch leverages Postgres's text search configurations for language-specific processing:

sql

```sql
-- French product descriptions
CREATE INDEX products_fr_idx ON products_fr
USING pg_textsearch(description)
WITH (text_config='french');

-- German product descriptions
CREATE INDEX products_de_idx ON products_de
USING pg_textsearch(description)
WITH (text_config='german');

-- Simple tokenization without stemming
CREATE INDEX products_simple_idx ON products
USING pg_textsearch(description)
WITH (text_config='simple');
```

To see available text search configurations:

sql

```sql
SELECT cfgname FROM pg_ts_config;
```

Postgres ships with 29 language configurations. Additional languages are available through extensions like zhparser for Chinese.

**Tuning BM25 Parameters**

The default BM25 parameters (k1=1.2, b=0.75) work well for most use cases, but you can adjust them:

sql

```sql
CREATE INDEX products_custom_idx ON products
USING pg_textsearch(description)
WITH (text_config='english', k1=1.5, b=0.8);
```

The `k1` parameter controls term frequency saturation (higher values give more weight to repeated terms). The `b` parameter controls length normalization (higher values penalize longer documents more).

**Monitoring Index Usage**

Check how often your index is being used:

sql

```sql
SELECT schemaname, tablename, indexname, idx_scan, idx_tup_read
FROM pg_stat_user_indexes
WHERE indexrelid::regclass::text ~ 'pg_textsearch';
```

For detailed information about the index structure:

sql

```sql
SELECT tp_debug_dump_index('products_search_idx');
```

This shows the term dictionary and posting lists, useful for understanding memory usage and debugging.

## **Hybrid Search with Semantic and Keyword Search**

One of the most powerful applications of pg\_textsearch is building hybrid search systems that combine semantic vector search with keyword BM25 search. TurboPuffer describes this approach well: use initial retrieval to narrow millions of results to dozens, then apply rank fusion and reranking [Hybrid Search](https://turbopuffer.com/docs/hybrid). This approach leverages the strengths of both methods: vectors capture semantic meaning while keywords provide precise term matching.

Here's a complete example using pgvector alongside pg\_textsearch:

sql

```sql
-- Create a table with both text content and vector embeddings
CREATE TABLE articles (
    id serial PRIMARY KEY,
    title text,
    content text,
    embedding vector(1536)  -- OpenAI ada-002 embedding dimension
);

-- Create a vector index for semantic search
CREATE INDEX articles_embedding_idx ON articles
USING hnsw (embedding vector_cosine_ops);

-- Create a keyword index for BM25 search
CREATE INDEX articles_content_idx ON articles
USING pg_textsearch(content)
WITH (text_config='english');

-- Insert some sample articles (embeddings would come from your embedding model)
INSERT INTO articles (title, content, embedding) VALUES
    ('Database Performance',
     'Optimizing query performance in PostgreSQL requires understanding indexes and query planning',
     '[0.1, 0.2, ...]'::vector),
    ('Machine Learning Systems',
     'Building scalable ML systems requires careful attention to data pipelines and model serving',
     '[0.3, 0.4, ...]'::vector),
    ('Search Engine Architecture',
     'Modern search engines combine relevance ranking with distributed query processing',
     '[0.5, 0.6, ...]'::vector);
```

Now perform hybrid search using Reciprocal Rank Fusion (RRF) to combine results:

sql

```sql
WITH vector_search AS (
    SELECT id,
           ROW_NUMBER() OVER (ORDER BY embedding <=> '[0.1, 0.2, ...]'::vector) AS rank
    FROM articles
    ORDER BY embedding <=> '[0.1, 0.2, ...]'::vector
    LIMIT 20
),
keyword_search AS (
    SELECT id,
           ROW_NUMBER() OVER (ORDER BY content <@> to_tpquery('query performance', 'articles_content_idx')) AS rank
    FROM articles
    ORDER BY content <@> to_tpquery('query performance', 'articles_content_idx')
    LIMIT 20
)
SELECT
    a.id,
    a.title,
    COALESCE(1.0 / (60 + v.rank), 0.0) + COALESCE(1.0 / (60 + k.rank), 0.0) AS combined_score
FROM articles a
LEFT JOIN vector_search v ON a.id = v.id
LEFT JOIN keyword_search k ON a.id = k.id
WHERE v.id IS NOT NULL OR k.id IS NOT NULL
ORDER BY combined_score DESC
LIMIT 10;
```

This query:

1. Performs semantic search using vector similarity (top 20 results)
2. Performs keyword search using BM25 ranking (top 20 results)
3. Combines results using RRF with k=60 (a common default)
4. Returns the top 10 documents by combined score

The RRF formula `1 / (k + rank)` gives higher scores to documents that rank well in both searches. Documents appearing in only one result set still contribute to the final ranking.

You can adjust the relative weight of vector vs keyword search by multiplying each component:

sql

```sql
SELECT
    a.id,
    a.title,
    0.7 * COALESCE(1.0 / (60 + v.rank), 0.0) +  -- 70% weight to vectors
    0.3 * COALESCE(1.0 / (60 + k.rank), 0.0)    -- 30% weight to keywords
    AS combined_score
FROM articles a
LEFT JOIN vector_search v ON a.id = v.id
LEFT JOIN keyword_search k ON a.id = k.id
WHERE v.id IS NOT NULL OR k.id IS NOT NULL
ORDER BY combined_score DESC
LIMIT 10;
```

As TurboPuffer recommends, we suggest leaving reranking details up to the application builder [Hybrid Search](https://turbopuffer.com/docs/hybrid). RRF in SQL is just one simple option. You might also consider using a dedicated reranking model like Cohere's reranker, or implementing custom scoring logic in your application code.

This hybrid approach works particularly well for RAG systems and agentic applications where you need both semantic understanding and precise keyword matching.

**Current Limitations**

The preview release focuses on BM25 ranked queries. Not yet supported:

* Phrase queries (searching for exact multi-word phrases)
* Multi-column indexing (you can only index one column per index)

These features are planned for future releases. The syntax may change slightly before the GA release, but the core functionality you see in the preview will remain stable.

## **What's Next for pg\_textsearch**

This preview release (v0.0.1) delivers the complete user interface for BM25 ranked search in Postgres. The next few months will bring enhancements that remove current limitations and improve performance at scale.

**Disk-based segments** will arrive in upcoming releases, enabling the index to scale beyond memory constraints. The current string-based architecture was specifically designed to make this transition straightforward. When the memtable fills, it will spill to immutable on-disk segments organized in an LSM-like hierarchy. This approach combines fast in-memory writes with efficient disk-based storage for large datasets.

**Advanced compression techniques** will reduce storage requirements and improve query performance. Posting lists will use delta encoding and bitpacking to minimize space usage. Skip lists will enable fast intersection operations for queries with multiple terms.

**Query optimization** will incorporate state-of-the-art algorithms like Block-Max WAND for efficient top-k retrieval. These optimizations make it possible to avoid scoring every matching document, dramatically improving performance for large result sets.

The roadmap prioritizes production readiness: stability, observability, and performance at scale. By the GA release, pg\_textsearch will handle workloads from thousands to millions of documents with consistent performance.

## **Try It Today**

pg\_textsearch is available now on Tiger Cloud for all customers, including the free tier.

**\[PLACEHOLDER: Link to Tiger Cloud free tier signup\]**

To get started, create a Tiger Cloud service (or use an existing one after the next maintenance window) and enable the extension:

sql

```sql
CREATE EXTENSION pg_textsearch;
```

This blog post serves as the primary documentation for the preview release. As the extension evolves, we'll publish additional tutorials and guides.

## **Share Your Feedback**

This is a preview release, and your feedback will shape the final product.

**\[PLACEHOLDER: Feedback channels \- GitHub issues? Email? Forum?\]**

Tell us about your use cases, report bugs, and request features. We're particularly interested in hearing from developers building:

* RAG systems and agentic applications
* Hybrid search combining semantic and keyword search
* Applications that need full-text search at scale in Postgres

## **Learn More**

* **pgvector**: [https://github.com/pgvector/pgvector](https://github.com/pgvector/pgvector) \- Vector similarity search for Postgres
* **pgvectorscale**: [https://github.com/timescale/pgvectorscale](https://github.com/timescale/pgvectorscale) \- High-performance vector search built on pgvector
* **Postgres text search**: [https://www.postgresql.org/docs/current/textsearch.html](https://www.postgresql.org/docs/current/textsearch.html) \- Official documentation for Postgres full-text search
* **Hybrid search explained**: TurboPuffer's guide to combining vector and BM25 search for improved retrieval [Hybrid Search](https://turbopuffer.com/docs/hybrid) \- [https://turbopuffer.com/docs/hybrid](https://turbopuffer.com/docs/hybrid)
