SELECT nextval('insert_seq') AS next_id \gset
INSERT INTO msmarco_passages_paradedb (passage_id, passage_text)
SELECT passage_id, passage_text
FROM msmarco_paradedb_staging WHERE staging_id = :next_id;
