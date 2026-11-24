CREATE INDEX documents_tin ON documents USING tin (body)
WITH (stemmer = 'en');
VACUUM (ANALYZE) documents;
