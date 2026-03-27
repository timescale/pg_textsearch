\set qid random(1, 691)
SELECT passage_id
FROM msmarco_v2_passages
WHERE passage_text @@@ paradedb.match(
    'passage_text',
    (SELECT query_text FROM benchmark_queries_power WHERE id = :qid))
ORDER BY paradedb.score(passage_id) DESC
LIMIT 10;
