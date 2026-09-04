# OSS-Fuzz build image for NX Warp.  Skeleton: not yet submitted.
FROM gcr.io/oss-fuzz-base/base-builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build zip python3 && \
    rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 https://github.com/nx-warp/nx-warp.git nx-warp
WORKDIR nx-warp
COPY build.sh $SRC/
