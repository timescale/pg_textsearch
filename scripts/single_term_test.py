#!/usr/bin/env python3
"""
Test single term BM25 calculation
"""

import math

# BM25 parameters (same as Tapir)
k1 = 1.2
b = 0.75

# Corpus stats (from our tests)
total_docs = 5
avg_doc_len = 4.6

# Term stats for 'aerodynam'
doc_freq = 4  # appears in 4 out of 5 documents
average_idf = 0.834868

# Calculate IDF with epsilon flooring
idf_numerator = total_docs - doc_freq + 0.5
idf_denominator = doc_freq + 0.5
raw_idf = math.log(idf_numerator / idf_denominator)

print(f"Raw IDF calculation: log(({total_docs}-{doc_freq}+0.5)/({doc_freq}+0.5)) = log({idf_numerator}/{idf_denominator}) = {raw_idf}")

if raw_idf <= 0.0:
    idf = 0.25 * average_idf
    print(f"Epsilon applied: {idf}")
else:
    idf = raw_idf
    print(f"No epsilon: {idf}")

# Document 1 analysis: contains 'aerodynam' with tf=1, doc_len=4
tf = 1.0
doc_len = 4.0
query_freq = 1

# BM25 calculation
numerator = tf * (k1 + 1.0)
denominator = tf + k1 * (1.0 - b + b * (doc_len / avg_doc_len))
term_score = idf * (numerator / denominator) * query_freq

print(f"\nBM25 term calculation for document 1:")
print(f"tf={tf}, doc_len={doc_len}, query_freq={query_freq}")
print(f"numerator = {tf} * ({k1} + 1) = {numerator}")
print(f"denominator = {tf} + {k1} * (1 - {b} + {b} * ({doc_len}/{avg_doc_len})) = {denominator}")
print(f"term_score = {idf} * ({numerator}/{denominator}) * {query_freq} = {term_score}")
print(f"Tapir reports: 0.307008")
print(f"Match: {abs(term_score - 0.307008) < 0.001}")