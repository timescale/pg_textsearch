SELECT nextval('insert_seq') AS next_id \gset
INSERT INTO wikipedia_articles_paradedb
    (article_id, title, content)
SELECT article_id, title, content
FROM wikipedia_paradedb_staging WHERE staging_id = :next_id;
