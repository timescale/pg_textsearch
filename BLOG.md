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

Modern search increasingly relies on hybrid approaches—combining semantic vector search with keyword matching for better results. Vector embeddings capture conceptual similarity but can miss exact terms that matter. Keyword search provides precision but lacks semantic understanding. The best systems use both, typically through reciprocal rank fusion or learned reranking models.

The appeal of keeping everything in Postgres is clear. The success of pgvector demonstrates that developers prefer extending Postgres over running separate infrastructure. You can store transactional data, time-series data, and vector embeddings in one database. Adding another external system just for text search feels like a step backward.

This is where pg_textsearch comes in. It implements modern BM25 ranking directly in Postgres, delivering the search quality of dedicated engines without the operational overhead. Combined with pgvector, you can build complete hybrid search systems—getting both semantic understanding and keyword precision from a single database query.

# How Modern BM25 Indexes Work

Before diving into pg_textsearch's implementation, it helps to understand how modern search engines achieve both quality and performance at scale.

**The BM25 ranking formula** improves on basic TF-IDF by addressing its key weaknesses. It calculates inverse document frequency (IDF) to identify discriminating terms—rare words get higher weight than common ones. It applies term frequency saturation through a parameter k1 (typically 1.2) to prevent documents from gaming the system through repetition. And it normalizes scores by document length relative to the corpus average, controlled by parameter b (typically 0.75), ensuring long documents don't automatically dominate results.

**Hierarchical index architecture** enables fast writes while maintaining query performance. Modern search engines organize indexes in layers, similar to LSM (Log-Structured Merge) trees. New documents write to an in-memory structure (often called a memtable or buffer), providing microsecond-latency insertions. When memory fills, the system flushes data to immutable disk segments. Background processes merge smaller segments into larger ones, maintaining query efficiency. This design decouples write performance from index size—writes always hit memory first, while reads can leverage both memory and disk structures.

**Advanced query algorithms** make it possible to find top results without scoring every document. Block-Max WAND (Weak AND) divides posting lists into blocks, each tagged with its maximum possible score. During query execution, the algorithm can skip entire blocks that can't possibly make it into the top-k results. This transforms what would be an O(n) operation into something much faster in practice, often examining only a small fraction of matching documents.

**Compression techniques** reduce I/O and improve cache efficiency. Posting lists use delta encoding—storing differences between document IDs rather than absolute values. Frequencies are compressed with variable-byte or bit-packing schemes. These techniques can reduce index size by 70-90% while maintaining fast decompression. Less I/O means faster queries, especially when indexes exceed available memory.

Search engines like Lucene (powering Elasticsearch and Solr) and Tantivy (powering Meilisearch and Quickwit) implement these techniques and many more. They've evolved far beyond core BM25 ranking to include faceted search, geo-spatial queries, and complex aggregations. The algorithms and data structures are well-understood at this point—anyone can study the open source implementations to see exactly how high-performance text search works.

The Postgres ecosystem is seeing renewed interest in bringing these capabilities native to the database. ParadeDB has done impressive engineering work integrating Tantivy directly with Postgres at the transactional and page level, demonstrating both the demand for ranked search in Postgres and the technical feasibility of the integration. Their system offers the full breadth of Lucene-like features. However, it's licensed under AGPL and not widely available through managed Postgres providers.

We chose a different path for pg_textsearch: build a focused implementation targeting specifically BM25 ranked search.

# How pg_textsearch Works: Quick Start

Let's walk through using pg_textsearch on TigerCloud, starting from the basics.

## Enable the Extension

First, enable pg_textsearch in your database:

```sql
CREATE EXTENSION pg_textsearch;
```

That's it—no configuration files, no external services. The extension is ready to use.

## Create Your First Index

Let's say you have a table of articles:

```sql
CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT,
    published_date DATE
);

-- Insert some sample data
INSERT INTO articles (title, content, published_date) VALUES
    ('Getting Started with Postgres',
     'Postgres is a powerful relational database system known for its reliability and feature set...',
     '2024-01-15'),
    ('Understanding Indexes',
     'Database indexes are data structures that improve query performance by enabling fast lookups...',
     '2024-02-20'),
    ('Advanced Query Optimization',
     'Query optimization involves choosing the most efficient execution plan for a given query...',
     '2024-03-10');
```

Create a BM25 index on the content column:

```sql
CREATE INDEX articles_content_idx ON articles
USING pg_textsearch(content)
WITH (text_config='english');
```

The `text_config` parameter specifies which Postgres text search configuration to use for tokenization and stemming. Common options include 'english', 'simple' (no stemming), 'french', 'german', and [29 others built into Postgres](https://www.postgresql.org/docs/current/textsearch-configuration.html).

For smaller datasets (up to around 100,000 documents), expect indexing to complete in a second or two, with queries returning in just a millisecond or two. The preview release keeps everything in memory, so performance is consistently fast for workloads that fit within the configured memory limit (default 64MB per index).

## Run Your First Query

Search for articles about "database performance":

```sql
SELECT id, title,
       content <@> to_tpquery('database performance', 'articles_content_idx') AS score
FROM articles
ORDER BY score
LIMIT 5;
```

Key points:
- The `<@>` operator calculates BM25 scores between text and a query
- `to_tpquery()` creates a query object with the index name for proper IDF calculation
- Scores are negative (better matches are more negative) to work naturally with Postgres's ASC ordering
- The index provides corpus statistics even when Postgres chooses a sequential scan

## Combine with SQL Features

pg_textsearch integrates seamlessly with standard SQL:

```sql
-- Filter by date and relevance threshold
SELECT title, published_date
FROM articles
WHERE published_date > '2024-01-01'
  AND content <@> to_tpquery('query optimization', 'articles_content_idx') < -1.5
ORDER BY content <@> to_tpquery('query optimization', 'articles_content_idx');

-- Group results by month
SELECT DATE_TRUNC('month', published_date) AS month,
       COUNT(*) AS matching_articles,
       AVG(content <@> to_tpquery('indexes', 'articles_content_idx')) AS avg_score
FROM articles
WHERE content <@> to_tpquery('indexes', 'articles_content_idx') < -0.5
GROUP BY month
ORDER BY month DESC;

-- Join with other tables
SELECT a.title, u.name AS author,
       a.content <@> to_tpquery('postgres tips', 'articles_content_idx') AS score
FROM articles a
JOIN users u ON a.author_id = u.id
ORDER BY score
LIMIT 10;
```

## Tune BM25 Parameters

The default BM25 parameters (k1=1.2, b=0.75) work well for most use cases, but you can adjust them:

```sql
-- Less length normalization (b=0.5) and higher term frequency saturation (k1=1.8)
CREATE INDEX articles_custom_idx ON articles
USING pg_textsearch(content)
WITH (text_config='english', k1=1.8, b=0.5);
```

- **k1** controls term frequency saturation (higher = more weight to repeated terms)
- **b** controls document length normalization (0 = no normalization, 1 = full normalization)

## Monitor Your Indexes

Check index memory usage and statistics:

```sql
-- Debug view of index internals (terms, posting lists, documents)
SELECT tp_debug_dump_index('articles_content_idx');

-- Check how often your indexes are used
SELECT indexrelid::regclass AS index_name,
       idx_scan,
       idx_tup_read
FROM pg_stat_user_indexes
WHERE indexrelid::regclass::text LIKE '%pg_textsearch%';
```

## Hybrid Search: Combining Vectors and Keywords

Modern search systems increasingly combine semantic vector search with keyword matching for optimal results. This hybrid approach leverages the strengths of both methods: vector embeddings capture conceptual similarity while keyword search provides precision for exact terms. The technique has proven effective across major search platforms including Elasticsearch, OpenSearch, and Azure AI Search.

Here's how to build hybrid search with pgvector and pg_textsearch:

```sql
-- Create a table with both text content and vector embeddings
CREATE TABLE documents (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT,
    embedding vector(1536)  -- OpenAI ada-002 dimension
);

-- Create both indexes
CREATE INDEX documents_embedding_idx ON documents
USING hnsw (embedding vector_cosine_ops);

CREATE INDEX documents_content_idx ON documents
USING pg_textsearch(content)
WITH (text_config='english');

-- Insert sample data (embeddings would come from your embedding model)
INSERT INTO documents (title, content, embedding) VALUES
    ('Database Performance',
     'Optimizing query performance in Postgres requires understanding indexes...',
     '[0.1, 0.2, ...]'::vector),
    ('Machine Learning Systems',
     'Building scalable ML systems requires careful attention to data pipelines...',
     '[0.3, 0.4, ...]'::vector);
```

### Reciprocal Rank Fusion (RRF)

One popular method for combining results is Reciprocal Rank Fusion, introduced by Cormack et al. at SIGIR 2009. RRF provides a simple, parameter-free way to merge rankings from multiple sources. While you can implement RRF directly in SQL as shown below, it's equally valid—and sometimes preferable—to handle this logic in your application code:

```sql
WITH vector_search AS (
    SELECT id,
           ROW_NUMBER() OVER (ORDER BY embedding <=> '[0.1, 0.2, ...]'::vector) AS rank
    FROM documents
    ORDER BY embedding <=> '[0.1, 0.2, ...]'::vector
    LIMIT 20
),
keyword_search AS (
    SELECT id,
           ROW_NUMBER() OVER (ORDER BY content <@> to_tpquery('query performance', 'documents_content_idx')) AS rank
    FROM documents
    ORDER BY content <@> to_tpquery('query performance', 'documents_content_idx')
    LIMIT 20
)
SELECT
    d.id,
    d.title,
    COALESCE(1.0 / (60 + v.rank), 0.0) + COALESCE(1.0 / (60 + k.rank), 0.0) AS combined_score
FROM documents d
LEFT JOIN vector_search v ON d.id = v.id
LEFT JOIN keyword_search k ON d.id = k.id
WHERE v.id IS NOT NULL OR k.id IS NOT NULL
ORDER BY combined_score DESC
LIMIT 10;
```

This query:
1. Retrieves top-20 results from vector search
2. Retrieves top-20 results from keyword search
3. Combines them using RRF with k=60 (a common default)
4. Returns the top-10 documents by combined score

The RRF formula `1 / (k + rank)` rewards documents that appear high in both result sets while still including documents that excel in just one.

### Weighted Hybrid Search

Adjust the relative importance of vector vs keyword search:

```sql
SELECT
    d.id,
    d.title,
    0.7 * COALESCE(1.0 / (60 + v.rank), 0.0) +  -- 70% weight to vectors
    0.3 * COALESCE(1.0 / (60 + k.rank), 0.0)    -- 30% weight to keywords
    AS combined_score
FROM documents d
LEFT JOIN vector_search v ON d.id = v.id
LEFT JOIN keyword_search k ON d.id = k.id
WHERE v.id IS NOT NULL OR k.id IS NOT NULL
ORDER BY combined_score DESC
LIMIT 10;
```

### Beyond Simple Fusion

The SQL examples above implement RRF for illustration, but production systems often require more sophisticated approaches. The [paper by Bruch, Gai, and Ingber](https://arxiv.org/pdf/2210.11934) provides a good overview of fusion methods and their trade-offs. Common approaches include:

- **Linear combination** of scores with tuned weights (simpler than RRF but requires parameter tuning)
- **Learning-to-rank models** that train on relevance judgments to optimize result ordering
- **Cross-encoder reranking** where a transformer model scores query-document pairs jointly
- **Cascade architectures** that apply increasingly expensive rankers to progressively smaller result sets

The choice depends on your latency requirements, result quality needs, and available computational resources. Simple score fusion like RRF works well for many applications, while systems requiring maximum relevance might justify the computational cost of neural rerankers.

Regardless of the fusion method, hybrid search with pg_textsearch and pgvector gives you both keyword precision and semantic understanding from a single Postgres instance—particularly valuable for RAG systems and other AI applications.

# Implementation Details

The preview release focuses on supporting the full user surface but backed only by the memtable component—fast, simple, and sufficient for small workloads. The full hierarchical architecture with disk segments will be released in stages over the coming months.

## Current Architecture (Preview Release)

The implementation leverages Postgres Dynamic Shared Areas (DSA) to build an inverted index in shared memory. This allows all backend processes to share a single index structure, avoiding duplication and enabling efficient memory use. The index consists of three core components:

**A term dictionary** implemented as a DSA hash table with string interning. Each unique term maps to its posting list pointer and document frequency for IDF calculation. String interning ensures each term appears exactly once in memory, regardless of how many documents contain it.

**Posting lists** stored as dynamically-growing vectors in DSA memory. Each entry contains a document identifier (Postgres TID) and term frequency. Lists remain unsorted during insertion for write performance, then sort on-demand for query processing. This design optimizes for the common case of sequential writes followed by read queries.

**Document metadata** tracking each document's length and state. The length enables BM25's document normalization. The state supports crash recovery—on restart, the index rebuilds by rescanning documents marked as indexed. Corpus-wide statistics (document count, average length) live here too, updated incrementally as documents are added or removed.

## Memtable Structure

\[replace the following ASCII diagram w/ one made by the design team\]

```
┌─────────────────────────────────────────────────┐
│              Memtable (Shared Memory)           │
├─────────────────────────────────────────────────┤
│                                                 │
│  ┌──────────────────────────────────────┐       │
│  │  Term Dictionary (dshash)            │       │
│  │  "database" → [stats, ptr]           │       │
│  │  "search"   → [stats, ptr]           │       │
│  │  "query"    → [stats, ptr]           │       │
│  └──────────┬───────────────────────────┘       │
│             │                                   │
│             ├─→ Posting List (vector)           │
│             │   [(page1,off3, tf=2),            │
│             │    (page2,off7, tf=1),            │
│             │    (page5,off2, tf=3)]            │
│             │                                   │
│  ┌──────────────────────────────────────┐       │
│  │  Document Lengths (dshash)           │       │
│  │  (page1,off3) → 245                  │       │
│  │  (page2,off7) → 189                  │       │
│  │  (page5,off2) → 412                  │       │
│  └──────────────────────────────────────┘       │
│                                                 │
│  Corpus Stats: total_docs, avg_doc_length       │
└─────────────────────────────────────────────────┘
                    │
                    │ (crash recovery)
                    ↓
┌─────────────────────────────────────────────────┐
│         Index Metapages (on disk)               │
│         List of document IDs                    │
│    (memtable rebuilt on restart from this)      │
└─────────────────────────────────────────────────┘
```

## Roadmap to Full Architecture

The preview release implements just the memtable layer—the top of the hierarchical structure described earlier. This is intentional: we're releasing in stages to get working software into users' hands quickly while building toward the complete system.

**Next milestone:** Naive disk segments. When the memtable fills, it flushes to an immutable on-disk segment. Queries merge results from the memtable and all segments. This removes the memory limitation while keeping the implementation straightforward.

**Following milestone:** Optimized segments with compression. Delta encoding for posting lists, skip lists for faster intersection, and basic query optimization. This brings significant performance improvements for larger indexes.

**Future milestone:** Background compaction and advanced query algorithms. A background worker merges small segments into larger ones, maintaining optimal query performance. Block-Max WAND enables efficient top-k retrieval without scoring all documents.

Each release maintains backward compatibility—indexes created with earlier versions continue working as you upgrade.

## Query Processing

When you execute a BM25 query:

```sql
SELECT * FROM documents
ORDER BY content <@> to_tpquery('database performance', 'docs_idx')
LIMIT 10;
```

The extension evaluates it in several steps. First, it looks up each query term in the dictionary to retrieve posting lists and IDF values. Then, for documents appearing in any posting list, it computes the BM25 score by summing each term's contribution: IDF × normalized term frequency × length normalization. The normalization factors come from the k1 and b parameters, using the corpus statistics maintained in the index.

The `<@>` operator returns negative BM25 scores—a deliberate design choice. Postgres sorts NULL values last in ascending order, and treats positive infinity as larger than any finite value. By returning negative scores (better matches are more negative), we enable efficient index scans in ascending order without special NULL handling.

# Try It Today

pg_textsearch is available now on Tiger Cloud for all customers, including the free tier.

**\[PLACEHOLDER: Link to Tiger Cloud free tier signup\]**

To get started, create a Tiger Cloud service (or use an existing one after the next maintenance window) and enable the extension:

```sql
CREATE EXTENSION pg_textsearch;
```

This blog post serves as the primary documentation for the preview release. As the extension evolves, we'll publish additional tutorials and guides.

# Share Your Feedback

This is a preview release, and your feedback will shape the final product.

**\[PLACEHOLDER: Feedback channels \- GitHub issues? Email? Forum?\]**

Tell us about your use cases, report bugs, and request features. We're particularly interested in hearing from developers building:

* RAG systems and agentic applications
* Hybrid search combining semantic and keyword search
* Applications that need full-text search at scale in Postgres

# Learn More

* **pgvector**: [https://github.com/pgvector/pgvector](https://github.com/pgvector/pgvector) - Vector similarity search for Postgres
* **pgvectorscale**: [https://github.com/timescale/pgvectorscale](https://github.com/timescale/pgvectorscale) - High-performance vector search built on pgvector
* **Postgres text search**: [https://www.postgresql.org/docs/current/textsearch.html](https://www.postgresql.org/docs/current/textsearch.html) - Official documentation for Postgres full-text search
* **Reciprocal Rank Fusion**: Cormack et al., "Reciprocal Rank Fusion Outperforms Condorcet and Individual Rank Learning Methods" (SIGIR 2009) - The original RRF paper
