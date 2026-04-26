FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    git \
    gdb \
    libsodium-dev \
    liblmdb-dev \
    libgtest-dev \
    libgmock-dev \
    libcbor-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
