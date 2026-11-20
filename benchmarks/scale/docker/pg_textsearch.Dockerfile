ARG POSTGRES_BASE
FROM ${POSTGRES_BASE}
ARG PG_TEXTSEARCH_SHA
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates clang git libicu-dev postgresql-server-dev-18 \
    && git clone https://github.com/timescale/pg_textsearch.git /build/pg_textsearch \
    && cd /build/pg_textsearch \
    && git checkout --detach "$PG_TEXTSEARCH_SHA" \
    && make -j"$(nproc)" \
    && make install \
    && rm -rf /build /var/lib/apt/lists/*
CMD ["postgres", "-c", "shared_preload_libraries=pg_textsearch"]
