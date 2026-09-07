FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    bash \
    build-essential \
    ca-certificates \
    cmake \
    curl \
    gcc-mingw-w64 \
    git \
    make \
    pkg-config \
    libcurl4-openssl-dev \
    libssl-dev \
    libsdl1.2-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

COPY . /workspace

RUN chmod +x tests/*.sh 2>/dev/null || true

CMD ["bash"]
