CREATE EXTENSION IF NOT EXISTS pg_textsearch;
ALTER DATABASE postgres SET default_text_search_config = 'english';
SET default_text_search_config = 'english';
DROP TABLE IF EXISTS documents CASCADE;
CREATE TABLE documents (
    id BIGINT PRIMARY KEY,
    source_id TEXT NOT NULL,
    body TEXT NOT NULL
);
