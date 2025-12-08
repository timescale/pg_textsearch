-- Upgrade from 0.0.5 to 0.0.6-dev

-- Update function cost to help planner prefer index scans over seq scan + sort.
-- Standalone BM25 scoring is expensive (~14μs per doc for tokenization alone).
ALTER FUNCTION text_bm25query_score(text, bm25query) COST 1000;
