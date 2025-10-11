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

# Postgres Full-Text Search: A Strong Foundation

Postgres ships with full-text search capabilities built into every instance—no plugins to install, no external services to configure. The `tsvector` and `tsquery` types, combined with GIN and GiST indexes, provide a complete text search solution that works out of the box. For many applications, especially those getting started or operating at modest scale, Postgres native full-text search delivers exactly what's needed: fast keyword matching with basic relevance ranking across dozens of supported languages.

The implementation is mature and battle-tested. Text processing handles stemming, stop words, and language-specific rules correctly. The query syntax supports Boolean operators, phrase matching, and proximity searches. GIN indexes make searches fast for datasets that fit the use case. For workloads like searching documentation, blog posts, or product catalogs with thousands to hundreds of thousands of entries, Postgres built-in search often performs admirably with minimal configuration.

## When Scale Meets Ranking

The challenges emerge when applications need both scale and sophisticated ranking. Consider a customer support system that stores conversation history. With 50,000 conversations, Postgres full-text search finds relevant past interactions in 50 milliseconds. A year later, with 10 million conversations, queries that rank results by relevance take 25-30 seconds. The feature becomes unusable.

This pattern repeats across production deployments. One developer reported queries going from under 1 second to 25-30 seconds on just 800,000 rows when using ts_rank for relevance sorting [sql - Postgres Full Text Search - Issues with Query Speed - Stack Overflow](https://stackoverflow.com/questions/66215974/postgres-full-text-search-issues-with-query-speed). Another team found that performance deteriorates significantly around 1 to 2 million rows [Full text search in milliseconds with PostgreSQL | May 01, 2015](https://www.lateral.io/resources-blog/full-text-search-in-milliseconds-with-postgresql).

## The Technical Constraints

Three specific limitations prevent Postgres full-text search from delivering high-quality ranked results at scale:

**Limited ranking signals.** The `ts_rank` and `ts_rank_cd` functions implement term frequency normalization and optional field weighting, but lack modern ranking features. They don't calculate inverse document frequency (IDF), so common words like "database" or "system" receive the same importance as rare, discriminating terms. They don't apply term frequency saturation, allowing documents that repeat a term excessively to dominate results. They don't normalize by corpus-average document length, causing longer documents to score higher regardless of relevance.

**All-or-nothing matching.** GIN and GiST indexes accelerate searches through the `@@` operator, which implements Boolean matching—all query terms must appear in a document for it to match. This creates a brittleness problem: a highly relevant document missing just one query term gets excluded entirely before ranking begins. You lose potentially excellent results due to strict Boolean requirements.

**Expensive ranking at scale.** Postgres must retrieve and score the `tsvector` for every matching document to compute rankings. When searching for common terms that match millions of rows, this becomes prohibitively expensive. The indexes help find matches but provide no assistance with ranking them. As one analysis noted, ranking requires consulting the tsvector of each matching document, which becomes I/O bound and slow at scale [Full-text search engine with PostgreSQL (part 2): Postgres ...](https://xata.io/blog/postgres-full-text-search-postgres-vs-elasticsearch).

## The Traditional Trade-offs

When Postgres native full-text search hits its limits, engineering teams face a difficult choice:

**Option 1: Work within Postgres constraints**

Many teams choose to stay with Postgres built-in capabilities and engineer around the limitations. This means accepting that `ts_rank` lacks modern relevance signals like IDF and term saturation that BM25 provides [Full-text search engine with PostgreSQL (part 2): Postgres ...](https://xata.io/blog/postgres-full-text-search-postgres-vs-elasticsearch). It means living with queries that take seconds instead of milliseconds when sorting millions of rows by relevance—Postgres must score every matching document with no way to efficiently retrieve just the top results [Comparing Native Postgres, ElasticSearch, and pg_search for Full-Text Search - Neon](https://neon.com/blog/postgres-full-text-search-vs-elasticsearch).

For write-heavy workloads, the situation gets worse. Each update triggers multiple index writes through Postgres MVCC system, and GIN indexes accumulate pending entries that cause unpredictable latency spikes [Lessons Learned From 5 Years of Scaling PostgreSQL](https://onesignal.com/blog/lessons-learned-from-5-years-of-scaling-postgresql/). Teams implement workarounds: caching layers, pre-computed rankings, query result limits, and careful index maintenance schedules.

**Option 2: Add dedicated search infrastructure**

The alternative is deploying Elasticsearch or similar search engines alongside Postgres. This delivers proper BM25 ranking, millisecond response times at scale, and sophisticated search features like faceting and aggregations.

The cost comes in operational complexity. You now maintain two data stores with different operational characteristics. Data synchronization becomes a critical path problem—every Postgres write must propagate to Elasticsearch, and any lag or failure creates inconsistency. You need expertise in two query languages, two backup strategies, two monitoring systems. Infrastructure costs multiply: not just the search cluster itself, but the data pipelines, monitoring, and engineering time to keep everything running.

## Why This Matters Now

Full-text search has renewed significance in modern application architectures, particularly with the rise of agentic systems. Chat agents need to store and retrieve conversation history to maintain context across interactions. Customer support bots search knowledge bases using natural language queries. RAG (Retrieval-Augmented Generation) systems require fast, accurate keyword search to find relevant documents before generating responses.

The appeal of keeping everything in Postgres is clear. The success of pgvector demonstrates that developers prefer extending Postgres over running separate infrastructure. You can store transactional data, time-series data, and vector embeddings in one database. Adding another external system just for text search feels like a step backward.

This is where pg_textsearch comes in. It implements modern BM25 ranking directly in Postgres, delivering the search quality of dedicated engines without the operational overhead. Combined with extensions like pgvector, you can build sophisticated hybrid search systems—mixing semantic vector search with keyword matching—entirely within your existing Postgres deployment.

# How Modern BM25 Indexes Work

Before diving into pg_textsearch's implementation, it helps to understand how modern search engines achieve both quality and performance at scale.

**The BM25 ranking formula** improves on basic TF-IDF by addressing its key weaknesses. It calculates inverse document frequency (IDF) to identify discriminating terms—rare words get higher weight than common ones. It applies term frequency saturation through a parameter k1 (typically 1.2) to prevent documents from gaming the system through repetition. And it normalizes scores by document length relative to the corpus average, controlled by parameter b (typically 0.75), ensuring long documents don't automatically dominate results.

**Hierarchical index architecture** enables fast writes while maintaining query performance. Modern search engines organize indexes in layers, similar to LSM (Log-Structured Merge) trees. New documents write to an in-memory structure (often called a memtable or buffer), providing microsecond-latency insertions. When memory fills, the system flushes data to immutable disk segments. Background processes merge smaller segments into larger ones, maintaining query efficiency. This design decouples write performance from index size—writes always hit memory first, while reads can leverage both memory and disk structures.

**Advanced query algorithms** make it possible to find top results without scoring every document. Block-Max WAND (Weak AND) divides posting lists into blocks, each tagged with its maximum possible score. During query execution, the algorithm can skip entire blocks that can't possibly make it into the top-k results. This transforms what would be an O(n) operation into something much faster in practice, often examining only a small fraction of matching documents.

**Compression techniques** reduce I/O and improve cache efficiency. Posting lists use delta encoding—storing differences between document IDs rather than absolute values. Frequencies are compressed with variable-byte or bit-packing schemes. These techniques can reduce index size by 70-90% while maintaining fast decompression. Less I/O means faster queries, especially when indexes exceed available memory.

Search engines like Lucene (powering Elasticsearch and Solr) and Tantivy (powering Meilisearch and Quickwit) implement these techniques and many more. They've evolved far beyond core BM25 ranking to include faceted search, geo-spatial queries, and complex aggregations. The algorithms and data structures are well-understood at this point—anyone can study the open source implementations to see exactly how high-performance text search works.

The Postgres ecosystem is seeing renewed interest in bringing these capabilities native to the database. ParadeDB has done impressive engineering work integrating Tantivy directly with Postgres at the transactional and page level, demonstrating both the demand for ranked search in Postgres and the technical feasibility of the integration. Their system offers the full breadth of Lucene-like features. However, it's licensed under AGPL and not widely available through managed Postgres providers.

We chose a different path for pg_textsearch: build a focused implementation targeting specifically BM25 ranked search.

# How pg_textsearch Works

pg_textsearch brings BM25 ranking to Postgres by implementing the memtable layer of a modern search index. For the preview release, we focus on in-memory performance—the foundation for the hierarchical architecture described above.

The implementation leverages Postgres Dynamic Shared Areas (DSA) to build an inverted index in shared memory. This allows all backend processes to share a single index structure, avoiding duplication and enabling efficient memory use. The index consists of three core components:

**A term dictionary** implemented as a DSA hash table with string interning. Each unique term maps to its posting list pointer and document frequency for IDF calculation. String interning ensures each term appears exactly once in memory, regardless of how many documents contain it.

**Posting lists** stored as dynamically-growing vectors in DSA memory. Each entry contains a document identifier (Postgres TID) and term frequency. Lists remain unsorted during insertion for write performance, then sort on-demand for query processing. This design optimizes for the common case of sequential writes followed by read queries.

**Document metadata** tracking each document's length and state. The length enables BM25's document normalization. The state supports crash recovery—on restart, the index rebuilds by rescanning documents marked as indexed. Corpus-wide statistics (document count, average length) live here too, updated incrementally as documents are added or removed.

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

### Query Processing

When you execute a BM25 query:

```sql
SELECT * FROM documents
ORDER BY content <@> to_tpquery('database performance', 'docs_idx')
LIMIT 10;
```

The extension evaluates it in several steps. First, it looks up each query term in the dictionary to retrieve posting lists and IDF values. Then, for documents appearing in any posting list, it computes the BM25 score by summing each term's contribution: IDF × normalized term frequency × length normalization. The normalization factors come from the k1 and b parameters, using the corpus statistics maintained in the index.

The `<@>` operator returns negative BM25 scores—a deliberate design choice. Postgres sorts NULL values last in ascending order, and treats positive infinity as larger than any finite value. By returning negative scores (better matches are more negative), we enable efficient index scans in ascending order without special NULL handling.

### Integration with Postgres

pg_textsearch deliberately builds on Postgres foundations rather than reimplementing them. Text processing uses Postgres's `to_tsvector` for tokenization, stemming, and stop-word removal. This provides immediate support for 29 languages without additional code—when you specify `text_config='french'`, you get French stemming and stop words automatically.

The extension integrates cleanly with SQL. You can combine BM25 scoring with filters, joins, and aggregations:

```sql
SELECT category, COUNT(*) as article_count
FROM articles
WHERE content <@> to_tpquery('machine learning', 'idx_articles') < -2.0
  AND published_date > '2024-01-01'
GROUP BY category
ORDER BY article_count DESC;
```

This finds articles about machine learning published this year with strong relevance (score < -2.0), grouped by category. The BM25 condition works alongside other SQL features—no special query language or separate system needed.

### Design Trade-offs

The preview release makes deliberate trade-offs to deliver a working system quickly while laying groundwork for future enhancements.

**Memory-only storage** provides fast writes and predictable performance. Documents insert in microseconds since they only update in-memory posting lists. No write-ahead logging, no disk flushes, no compaction stalls. The trade-off: each index is constrained by available shared memory (default 64MB, configurable via `pg_textsearch.index_memory_limit`). This suffices for many workloads—documentation, knowledge bases, product catalogs—where the corpus fits comfortably in RAM.

**String-based term storage** avoids a global term ID namespace. Each term is stored as a string rather than mapped to an integer ID. This uses more memory but simplifies the architecture significantly. When disk segments arrive in future releases, we can merge posting lists without ID translation or coordination between memory and disk structures.

**Simple query evaluation** processes all matching documents rather than using advanced algorithms like Block-Max WAND. For in-memory indexes this is actually fine—scanning a posting list in RAM is fast enough that the overhead of maintaining skip lists might not pay off. As we add disk segments and indexes grow larger, query optimization becomes critical. The current implementation establishes correctness; performance optimizations come next.

### Alternative Approaches We Considered

**Building on GIN indexes** seems natural—they already power Postgres full-text search. But GIN indexes are optimized for Boolean matching, not ranking. They lack the corpus statistics (document count, average length) and per-document metadata (length, term frequencies) that BM25 requires. We'd need separate structures for scoring anyway, negating the benefit of reusing GIN.

**Embedding Tantivy** (as pg_search does) would provide a mature search engine immediately. But Tantivy is designed as a standalone system with its own storage format, memory management, and query language. Wrapping it for Postgres requires complex translation layers and limits integration with Postgres features. We chose to build native Postgres code that directly leverages shared memory, DSA allocation, and the query executor.

**Using RUM indexes** could support ranking, but RUM isn't part of core Postgres and has its own complexity. We wanted pg_textsearch to work with standard Postgres installations without additional dependencies.

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
