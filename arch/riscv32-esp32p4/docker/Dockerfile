FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && apt-get install -y -qq \
    build-essential \
    gcc-riscv64-unknown-elf \
    binutils-riscv64-unknown-elf \
    python3 \
    python3-pip \
    git \
    make \
    && pip install --break-system-packages esptool \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /aros
