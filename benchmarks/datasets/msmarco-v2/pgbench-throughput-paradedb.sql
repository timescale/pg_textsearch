\set qid random(1, 1000)
SELECT passage_id FROM msmarco_v2_passages
WHERE passage_text @@@ paradedb.match('passage_text',
    (SELECT query_text FROM weighted_query_pool WHERE id = :qid))
ORDER BY paradedb.score(passage_id) DESC
LIMIT 10;
