FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc make libc6-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /repo

# Build context is the repo root (FastCSV-C/).
COPY Makefile ./
COPY arena.c arena.h ./
COPY csv_config.c csv_config.h ./
COPY csv_parser.c csv_parser.h ./
COPY csv_reader.c csv_reader.h ./
COPY csv_utils.c csv_utils.h ./
COPY csv_writer.c csv_writer.h ./
COPY query/ query/

RUN make csvql

WORKDIR /app
COPY bench.sh /app/bench.sh
RUN chmod +x /app/bench.sh

ENTRYPOINT ["/app/bench.sh"]