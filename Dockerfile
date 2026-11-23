# Build Mothlight anywhere Docker runs (macOS, Windows, Linux):
#   docker build -t mothlight .
#   docker run --rm -v "$PWD":/src -w /src mothlight sh -c "scripts/fetch.sh && scripts/build.sh"
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -q && apt-get install -y -q --no-install-recommends \
      git make gcc libc6-dev python3 python3-docutils python3-pil zip unzip ca-certificates \
      gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi ffmpeg \
    && rm -rf /var/lib/apt/lists/*
RUN git config --global --add safe.directory '*'
