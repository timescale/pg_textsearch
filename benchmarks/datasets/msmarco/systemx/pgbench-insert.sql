SELECT nextval('insert_seq') AS next_id \gset
INSERT INTO msmarco_passages_systemx (passage_id, passage_text)
SELECT passage_id, passage_text
FROM msmarco_systemx_staging WHERE staging_id = :next_id;
