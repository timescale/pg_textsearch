#!/usr/bin/env python3
"""
Generate a large Wikipedia dataset by fetching articles in batches
This is more reliable than trying to download full dumps
"""

import json
import sys
import urllib.request
import urllib.parse
import time
import random

def fetch_batch(count, output_file):
    """Fetch Wikipedia articles in batches and save to TSV"""

    api_url = "https://en.wikipedia.org/w/api.php"
    articles_written = 0

    # Categories to fetch from for variety
    categories = [
        "Technology", "Science", "History", "Geography",
        "Mathematics", "Physics", "Biology", "Medicine",
        "Computer_science", "Engineering", "Chemistry",
        "Philosophy", "Economics", "Politics", "Literature"
    ]

    with open(output_file, 'w', encoding='utf-8') as out:
        while articles_written < count:
            # Randomly select category for variety
            category = random.choice(categories)
            batch_size = min(50, count - articles_written)

            # Get articles from category
            params = {
                'action': 'query',
                'format': 'json',
                'generator': 'categorymembers',
                'gcmtitle': f'Category:{category}',
                'gcmlimit': batch_size,
                'prop': 'extracts',
                'exintro': True,
                'explaintext': True,
                'exlimit': batch_size
            }

            # Add User-Agent header
            url = api_url + '?' + urllib.parse.urlencode(params)
            req = urllib.request.Request(url)
            req.add_header('User-Agent', 'pg_textsearch-benchmark/1.0')

            try:
                with urllib.request.urlopen(req) as response:
                    data = json.loads(response.read().decode())

                    if 'query' in data and 'pages' in data['query']:
                        for page_id, page in data['query']['pages'].items():
                            if articles_written >= count:
                                break

                            title = page.get('title', '').replace('\t', ' ').replace('\n', ' ')
                            extract = page.get('extract', '').replace('\t', ' ').replace('\n', ' ')

                            if title and extract and len(extract) > 100:
                                # Limit extract to reasonable size
                                if len(extract) > 5000:
                                    extract = extract[:5000]

                                out.write(f"{title}\t{extract}\n")
                                articles_written += 1

                                if articles_written % 100 == 0:
                                    print(f"Fetched {articles_written}/{count} articles...")

                    # Be nice to Wikipedia's servers
                    time.sleep(0.5)

            except Exception as e:
                print(f"Error fetching batch: {e}")
                time.sleep(2)  # Wait longer on error

    return articles_written

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: generate_wikipedia_batch.py <count> <output_file>")
        sys.exit(1)

    count = int(sys.argv[1])
    output_file = sys.argv[2]

    print(f"Fetching {count} Wikipedia articles...")
    fetched = fetch_batch(count, output_file)
    print(f"Successfully fetched {fetched} articles to {output_file}")

    # Show file size
    import os
    size = os.path.getsize(output_file)
    print(f"File size: {size/1024/1024:.1f} MB")
