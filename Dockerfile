FROM debian:bookworm-slim AS c-build

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential libmosquitto-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY Makefile ./
COPY include ./include
COPY src ./src
RUN make

FROM python:3.12-slim-bookworm

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

RUN apt-get update \
    && apt-get install -y --no-install-recommends libmosquitto1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=c-build /app/logistica /app/robo ./
COPY config ./config
COPY interface ./interface
RUN python -m pip install --no-cache-dir -r interface/requirements.txt

EXPOSE 8080