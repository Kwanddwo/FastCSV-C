FROM python:3.12-alpine

RUN apk add --no-cache sqlite

WORKDIR /app
COPY sqlite/bench.py /app/bench.py

ENTRYPOINT ["python3", "/app/bench.py"]
