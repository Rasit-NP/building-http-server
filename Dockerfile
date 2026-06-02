# Stage 0: Dev
FROM ubuntu:22.04 AS dev

RUN apt-get update && apt-get install -y build-essential cmake git gdb rsync openssh-server
RUN rm -rf /var/lib/apt/lists/*

RUN useradd -m user && echo "user:password" | chpasswd
RUN mkdir /var/run/sshd

WORKDIR /src

# Stage 1: Build
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y build-essential cmake git
RUN rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src/ ./src/
COPY tests/ ./tests/
COPY include/ ./include/

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
RUN cmake --build build -j

# Stage 2: Runtime
FROM ubuntu:22.04 AS runtime

RUN apt-get update && apt-get install -y libstdc++6
RUN rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build/src/http_server ./

CMD ["./http_server"]