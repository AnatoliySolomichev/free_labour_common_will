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

# То же окно в Firefox ESR (docs/extension.md §6) — версии, что у участника на
# хосте (Debian, firefox-esr, 2026-10-24). Firefox из Ubuntu — тоже snap, поэтому
# сборка Mozilla. Ведёт его geckodriver: Playwright расширения в Firefox не грузит.
RUN apt-get update && apt-get install -y --no-install-recommends libgtk-3-0t64 xz-utils \
    && curl -fsSL https://ftp.mozilla.org/pub/firefox/releases/153.5.1esr/linux-x86_64/en-US/firefox-153.5.1esr.tar.xz \
       | tar -xJ -C /opt \
    && ln -s /opt/firefox/firefox /usr/local/bin/firefox \
    && curl -fsSL https://github.com/mozilla/geckodriver/releases/download/v0.37.1/geckodriver-v0.37.1-linux64.tar.gz \
       | tar -xz -C /usr/local/bin \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
