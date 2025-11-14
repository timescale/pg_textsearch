#!/usr/bin/env python3
"""
Custom BM25 implementation using Tantivy's IDF formula.
This subclasses rank-bm25's BM25Okapi to use the formula: ln(1 + (N - n + 0.5) / (n + 0.5))
"""
import math
import numpy as np
from rank_bm25 import BM25Okapi

class BM25Tantivy(BM25Okapi):
    """
    BM25 with Tantivy's IDF calculation that never produces negative values.
    """

    def _calc_idf(self, nd):
        """
        Calculate IDF using Tantivy's formula: ln(1 + (N - n + 0.5) / (n + 0.5))
        This formula never produces negative values.

        Args:
            nd: dictionary mapping terms to document frequencies
        """
        # Calculate IDF using Tantivy's formula
        idf_dict = {}
        for word, freq in nd.items():
            # Tantivy's formula: ln(1 + (N - n + 0.5) / (n + 0.5))
            idf_dict[word] = math.log(1.0 + (self.corpus_size - freq + 0.5) / (freq + 0.5))

        self.idf = idf_dict

def test_tantivy_bm25():
    """Test our custom BM25 implementation against expected Tantivy scores."""

    # Test documents
    documents = [
        "original document one",
        "original document two",
        "new document three",
        "new document four"
    ]

    # Tokenize (simple split)
    tokenized_docs = [doc.split() for doc in documents]

    # Create custom BM25 instance
    bm25 = BM25Tantivy(tokenized_docs)

    # Query
    query = "document"
    query_tokens = query.split()

    # Get scores
    scores = bm25.get_scores(query_tokens)

    print("=== BM25 with Tantivy IDF Formula ===")
    print("Documents:")
    for i, doc in enumerate(documents):
        print(f"  {i+1}. '{doc}'")

    print(f"\nQuery: '{query}'")
    print("\nScores (expecting 0.105361 for all):")
    for i, (doc, score) in enumerate(zip(documents, scores)):
        print(f"  {i+1}. '{doc}': {score:.6f}")

    # Verify IDF calculation
    print("\n=== IDF Verification ===")
    word_idf = bm25.idf.get("document", 0)
    print(f"IDF for 'document': {word_idf:.6f}")
    print(f"Expected (Tantivy): 0.105361")
    print(f"Match: {abs(word_idf - 0.105361) < 0.000001}")

    # Check score calculation details
    print("\n=== Score Calculation Details ===")
    print(f"k1 parameter: {bm25.k1}")
    print(f"b parameter: {bm25.b}")
    print(f"Average document length: {bm25.avgdl}")

    return scores

if __name__ == "__main__":
    scores = test_tantivy_bm25()

    # Additional test with different query
    print("\n" + "="*50)
    print("Testing with multi-term query:")

    documents = [
        "original document one",
        "original document two",
        "new document three",
        "new document four"
    ]

    tokenized_docs = [doc.split() for doc in documents]
    bm25 = BM25Tantivy(tokenized_docs)

    query = "original new"
    query_tokens = query.split()
    scores = bm25.get_scores(query_tokens)

    print(f"Query: '{query}'")
    print("Scores:")
    for i, (doc, score) in enumerate(zip(documents, scores)):
        print(f"  {i+1}. '{doc}': {score:.6f}")
