FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    git \
    gdb \
    python3 \
    python3-pip \
    python3-venv \
    libsodium-dev \
    liblmdb-dev \
    libgtest-dev \
    libgmock-dev \
    libcbor-dev \
    libssl-dev \
    ca-certificates \
    curl \
    faketime \
    nodejs \
    npm \
    xvfb \
    xauth \
    && rm -rf /var/lib/apt/lists/*

# Расширение браузера (docs/extension.md): проверка в настоящем Chromium —
# Playwright со своим Chromium (chromium из Ubuntu ставится через snap, а snap в
# Docker не работает); окно браузера — виртуальное, под xvfb-run.
RUN apt-get update \
    && npm install -g playwright@1.47.2 \
    && playwright install --with-deps chromium \
    && rm -rf /var/lib/apt/lists/*
ENV NODE_PATH=/usr/local/lib/node_modules

WORKDIR /workspace
