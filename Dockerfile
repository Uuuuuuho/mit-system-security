# shd-labs: one image for every lab. Built natively for the host architecture
# (arm64 on Apple silicon, amd64 on Intel/AMD) - do NOT force linux/amd64: timing
# labs are meaningless under instruction emulation.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    PYTHONDONTWRITEBYTECODE=1

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential gdb valgrind make git less vim-tiny file procps util-linux ca-certificates \
        python3 python3-numpy python3-matplotlib python3-sklearn python3-z3 \
        clang lld llvm \
        yosys iverilog \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /home/student/shd-labs
WORKDIR /home/student/shd-labs
CMD ["/bin/bash"]
