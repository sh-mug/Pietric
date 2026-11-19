# Pietric with LLVM 20. Compile a program in the current directory:
#   docker build -t pietric .
#   docker run --rm -v "$PWD:/work" -u "$(id -u):$(id -g)" pietric program.png -o program
# or compile and run it in one go (stdin is passed on with -i):
#   docker run --rm -i -v "$PWD:/work" pietric run program.png

FROM ubuntu:24.04 AS llvm
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
 && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
    | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
 && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" \
    > /etc/apt/sources.list.d/llvm.list \
 && apt-get purge -y --auto-remove curl gnupg \
 && rm -rf /var/lib/apt/lists/*

FROM llvm AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends llvm-20-dev cmake make g++ zlib1g-dev libzstd-dev \
 && rm -rf /var/lib/apt/lists/*
COPY . /src
RUN cmake -S /src -B /build -DLLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm \
 && cmake --build /build -j"$(nproc)"

# Pietric runs LLVM's opt and llc and links with cc.
FROM llvm
RUN apt-get update \
 && apt-get install -y --no-install-recommends llvm-20 gcc libc6-dev \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/Pietric /usr/local/bin/Pietric
# "run <program> [options]" compiles the program and runs it; anything else goes to Pietric.
RUN printf '%s\n' '#!/bin/sh' 'set -e' \
      'if [ "$1" = run ]; then' \
      '    shift' \
      '    exe=$(mktemp /tmp/piet.XXXXXX)' \
      '    Pietric "$@" -o "$exe"' \
      '    exec "$exe"' \
      'fi' \
      'exec Pietric "$@"' > /usr/local/bin/pietric-docker \
 && chmod +x /usr/local/bin/pietric-docker
WORKDIR /work
ENTRYPOINT ["pietric-docker"]
