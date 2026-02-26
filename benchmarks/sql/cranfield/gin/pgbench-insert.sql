SELECT nextval('insert_seq') AS next_id \gset
INSERT INTO cranfield_gin_documents
    (doc_id, title, author, bibliography, content)
SELECT doc_id, title, author, bibliography, content
FROM cranfield_gin_staging WHERE staging_id = :next_id;
