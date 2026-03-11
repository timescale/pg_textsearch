\set qid random(1, 691)
SELECT passage_id
FROM msmarco_v2_passages
ORDER BY passage_text <@> to_bm25query(
    (SELECT query_text FROM benchmark_queries_power WHERE id = :qid),
    'msmarco_v2_bm25_idx')
LIMIT 10;
