CREATE INDEX documents_bm25 ON documents
USING bm25 (id, body)
WITH (
    key_field = 'id',
    text_fields = '{"body":{"tokenizer":{
      "type":"default","stopwords_language":"English",
      "stemmer":"English","k1":1.2,"b":0.75
    }}}'
);
VACUUM (ANALYZE) documents;
