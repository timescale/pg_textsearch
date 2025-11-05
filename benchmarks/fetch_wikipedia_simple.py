#!/usr/bin/env python3
"""
Fetch real Wikipedia data using only standard library
"""

import json
import sys
import urllib.request
import urllib.parse
import time
import re
from typing import List, Tuple


def fetch_wikipedia_articles(count: int, output_file: str):
    """Fetch real Wikipedia articles using the API"""

    api_url = "https://en.wikipedia.org/w/api.php"
    articles = []

    print(f"Fetching {count} Wikipedia articles...")

    # We'll fetch in batches of 10
    batch_size = 10
    fetched = 0

    while fetched < count:
        current_batch = min(batch_size, count - fetched)

        # Get random articles
        params = {
            'action': 'query',
            'format': 'json',
            'generator': 'random',
            'grnnamespace': 0,
            'grnlimit': current_batch,
            'prop': 'extracts',
            'exintro': True,
            'explaintext': True,
            'exlimit': current_batch
        }

        url = api_url + '?' + urllib.parse.urlencode(params)

        # Wikipedia requires a User-Agent header
        req = urllib.request.Request(url)
        req.add_header('User-Agent', 'pg_textsearch-benchmark/1.0 (https://github.com/user/pg_textsearch)')

        try:
            with urllib.request.urlopen(req) as response:
                data = json.loads(response.read().decode())

                if 'query' in data and 'pages' in data['query']:
                    for page_id, page in data['query']['pages'].items():
                        title = page.get('title', 'Untitled')
                        extract = page.get('extract', '')

                        if extract:
                            # Clean the text
                            extract = re.sub(r'\s+', ' ', extract)
                            extract = extract[:2000]  # Limit to 2000 chars

                            articles.append((title, extract))
                            fetched += 1

                            print(f"  Fetched {fetched}/{count}: {title[:50]}...")

        except Exception as e:
            print(f"Error fetching articles: {e}")
            break

        # Be nice to Wikipedia
        time.sleep(0.5)

    # Save to TSV
    print(f"\nSaving {len(articles)} articles to {output_file}...")

    with open(output_file, 'w', encoding='utf-8') as f:
        for title, content in articles:
            # Escape tabs and newlines for TSV format
            title = title.replace('\t', ' ').replace('\n', ' ').replace('\r', '')
            content = content.replace('\t', ' ').replace('\n', ' ').replace('\r', '')
            f.write(f"{title}\t{content}\n")

    # Calculate stats
    total_chars = sum(len(content) for _, content in articles)
    avg_chars = total_chars / len(articles) if articles else 0

    print(f"Done! Fetched {len(articles)} articles")
    print(f"Total size: {total_chars:,} characters ({total_chars/1024:.1f} KB)")
    print(f"Average article: {avg_chars:.0f} characters")

    return len(articles)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: fetch_wikipedia_simple.py <count> <output_file>")
        print("Example: fetch_wikipedia_simple.py 1000 wikipedia_articles.tsv")
        sys.exit(1)

    try:
        count = int(sys.argv[1])
        output_file = sys.argv[2]
    except ValueError:
        print(f"Error: Count must be a number")
        sys.exit(1)

    result = fetch_wikipedia_articles(count, output_file)
    sys.exit(0 if result > 0 else 1)
