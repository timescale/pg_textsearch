SELECT nextval('insert_seq') AS next_id \gset
INSERT INTO wikipedia_gin_articles
    (article_id, title, content)
SELECT article_id, title, content
FROM wikipedia_gin_staging WHERE staging_id = :next_id;
