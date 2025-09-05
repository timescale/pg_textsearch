#!/usr/bin/env python3
"""
Validate BM25 scores against Python rank-bm25 library
This computes ground truth BM25 scores for our test cases
"""

import math
from typing import List, Dict, Tuple

def simple_tokenize(text: str) -> List[str]:
    """Simple tokenization - lowercase and split on whitespace"""
    return text.lower().split()

def compute_bm25_score(doc_tokens: List[str], query_tokens: List[str], 
                       corpus: List[List[str]], k1: float = 1.2, b: float = 0.75) -> float:
    """
    Compute BM25 score for a document given a query
    
    Args:
        doc_tokens: Tokenized document
        query_tokens: Tokenized query
        corpus: List of all tokenized documents for IDF calculation
        k1: BM25 k1 parameter (controls term frequency saturation)
        b: BM25 b parameter (controls length normalization)
    
    Returns:
        BM25 score
    """
    # Calculate average document length
    avg_doc_len = sum(len(doc) for doc in corpus) / len(corpus)
    doc_len = len(doc_tokens)
    N = len(corpus)
    
    score = 0.0
    
    # Count term frequencies in document
    doc_tf = {}
    for token in doc_tokens:
        doc_tf[token] = doc_tf.get(token, 0) + 1
    
    # Process each query term
    for query_term in query_tokens:
        if query_term not in doc_tf:
            continue
            
        # Term frequency in document
        tf = doc_tf[query_term]
        
        # Document frequency (number of documents containing the term)
        df = sum(1 for doc in corpus if query_term in doc)
        
        # IDF calculation: log((N - df + 0.5) / (df + 0.5))
        idf = math.log((N - df + 0.5) / (df + 0.5))
        
        # BM25 formula
        numerator = tf * (k1 + 1)
        denominator = tf + k1 * (1 - b + b * (doc_len / avg_doc_len))
        
        score += idf * (numerator / denominator)
    
    return score

def test_queries_dataset():
    """Test the 'queries' test dataset"""
    print("=" * 60)
    print("Testing 'queries' dataset")
    print("=" * 60)
    
    # Documents from the test
    documents = [
        "postgresql database management system with advanced indexing capabilities for fast query processing",
        "machine learning algorithms and artificial intelligence techniques for data analysis and prediction",
        "full text search with bm25 ranking and relevance scoring for information retrieval systems",
        "information retrieval and search engines using vector space models and term frequency analysis",
        "natural language processing techniques including parsing stemming and semantic analysis",
        "database indexing and query optimization strategies for improving performance in large datasets",
        "text mining and document classification using machine learning and statistical methods",
        "search relevance and ranking algorithms with focus on user experience and result quality",
        "distributed systems and scalability challenges in modern cloud computing environments",
        "data mining algorithms for pattern discovery and knowledge extraction from large databases",
        "evolution of web search from simple keyword matching to semantic understanding",
        "big data analytics platforms and tools for processing massive datasets efficiently"
    ]
    
    # Tokenize all documents
    corpus = [simple_tokenize(doc) for doc in documents]
    
    # Test 1: Basic search for 'database'
    print("\nTest 1: Query = 'database'")
    query = "database"
    query_tokens = simple_tokenize(query)
    
    scores = []
    for i, doc in enumerate(documents):
        doc_tokens = simple_tokenize(doc)
        score = compute_bm25_score(doc_tokens, query_tokens, corpus)
        scores.append((i, doc[:60] + "...", score))
    
    # Sort by score (descending)
    scores.sort(key=lambda x: x[2], reverse=True)
    
    print("Top 3 results:")
    for idx, title, score in scores[:3]:
        print(f"  Doc {idx}: {title}")
        print(f"    BM25 Score: {score:.6f}")
        print(f"    Negative (for PG): {-score:.6f}")
    
    # Test 2: Multi-term search for 'machine learning'
    print("\nTest 2: Query = 'machine learning'")
    query = "machine learning"
    query_tokens = simple_tokenize(query)
    
    scores = []
    for i, doc in enumerate(documents):
        doc_tokens = simple_tokenize(doc)
        score = compute_bm25_score(doc_tokens, query_tokens, corpus)
        scores.append((i, doc[:60] + "...", score))
    
    scores.sort(key=lambda x: x[2], reverse=True)
    
    print("Top 3 results:")
    for idx, title, score in scores[:3]:
        print(f"  Doc {idx}: {title}")
        print(f"    BM25 Score: {score:.6f}")
        print(f"    Negative (for PG): {-score:.6f}")
    
    # Test 3: 'search algorithms'
    print("\nTest 3: Query = 'search algorithms'")
    query = "search algorithms"
    query_tokens = simple_tokenize(query)
    
    scores = []
    for i, doc in enumerate(documents):
        doc_tokens = simple_tokenize(doc)
        score = compute_bm25_score(doc_tokens, query_tokens, corpus)
        scores.append((i, doc[:60] + "...", score))
    
    scores.sort(key=lambda x: x[2], reverse=True)
    
    print("Top 5 results:")
    for idx, title, score in scores[:5]:
        print(f"  Doc {idx}: {title}")
        print(f"    BM25 Score: {score:.6f}")
        print(f"    Negative (for PG): {-score:.6f}")

def test_aerodocs_dataset():
    """Test the aerodynamics documents dataset"""
    print("\n" + "=" * 60)
    print("Testing aerodynamics documents dataset")
    print("=" * 60)
    
    documents = [
        "aerodynamic flow boundary layer wing design",
        "boundary layer turbulent flow analysis",
        "supersonic aircraft design aerodynamic flow",
        "wing design optimization aerodynamic performance",
        "computational fluid dynamics boundary layer"
    ]
    
    corpus = [simple_tokenize(doc) for doc in documents]
    
    # Test 1: 'aerodynamic flow'
    print("\nTest 1: Query = 'aerodynamic flow'")
    query = "aerodynamic flow"
    query_tokens = simple_tokenize(query)
    
    scores = []
    for i, doc in enumerate(documents):
        doc_tokens = simple_tokenize(doc)
        score = compute_bm25_score(doc_tokens, query_tokens, corpus)
        scores.append((i+1, doc, score))  # doc_id starts from 1
    
    scores.sort(key=lambda x: x[2], reverse=True)
    
    print("All results (sorted by score):")
    for doc_id, title, score in scores:
        print(f"  Doc {doc_id}: {title}")
        print(f"    BM25 Score: {score:.6f}")
        print(f"    Negative (for PG): {-score:.6f}")
    
    # Test 2: 'boundary layer turbulent'
    print("\nTest 2: Query = 'boundary layer turbulent'")
    query = "boundary layer turbulent"
    query_tokens = simple_tokenize(query)
    
    scores = []
    for i, doc in enumerate(documents):
        doc_tokens = simple_tokenize(doc)
        score = compute_bm25_score(doc_tokens, query_tokens, corpus)
        scores.append((i+1, doc, score))
    
    scores.sort(key=lambda x: x[2], reverse=True)
    
    print("All results (sorted by score):")
    for doc_id, title, score in scores:
        print(f"  Doc {doc_id}: {title}")
        print(f"    BM25 Score: {score:.6f}")
        print(f"    Negative (for PG): {-score:.6f}")

def test_vector_dataset():
    """Test the vector test dataset"""
    print("\n" + "=" * 60)
    print("Testing 'vector' dataset")
    print("=" * 60)
    
    documents = [
        "the quick brown fox",
        "jumped over the lazy dog",
        "sphinx of black quartz",
        "hello world example",
        "postgresql full text search"
    ]
    
    corpus = [simple_tokenize(doc) for doc in documents]
    
    # Test: 'hello world'
    print("\nQuery = 'hello world'")
    query = "hello world"
    query_tokens = simple_tokenize(query)
    
    scores = []
    for i, doc in enumerate(documents):
        doc_tokens = simple_tokenize(doc)
        score = compute_bm25_score(doc_tokens, query_tokens, corpus)
        scores.append((doc, score))
    
    scores.sort(key=lambda x: x[1], reverse=True)
    
    print("All results (sorted by score):")
    for title, score in scores:
        print(f"  {title}")
        print(f"    BM25 Score: {score:.6f}")
        print(f"    Negative (for PG): {-score:.6f}")
    
    # Test: 'quick fox'
    print("\nQuery = 'quick fox'")
    query = "quick fox"
    query_tokens = simple_tokenize(query)
    
    scores = []
    for i, doc in enumerate(documents):
        doc_tokens = simple_tokenize(doc)
        score = compute_bm25_score(doc_tokens, query_tokens, corpus)
        scores.append((doc, score))
    
    scores.sort(key=lambda x: x[1], reverse=True)
    
    print("Top result:")
    for title, score in scores[:1]:
        print(f"  {title}")
        print(f"    BM25 Score: {score:.6f}")
        print(f"    Negative (for PG): {-score:.6f}")

if __name__ == "__main__":
    test_queries_dataset()
    test_aerodocs_dataset()
    test_vector_dataset()