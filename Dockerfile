# Build Stage
FROM --platform=linux/amd64 ubuntu:20.04 as builder

## Install build dependencies.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y cmake clang

## Add source code to the build stage.
ADD . /mayhem-nullc
WORKDIR /mayhem-nullc

RUN mkdir /mayhem-nullc/build
WORKDIR /mayhem-nullc/build
RUN cmake ..
RUN make
WORKDIR /mayhem-nullc/build/nullcl

# Package Stage
FROM --platform=linux/amd64 ubuntu:20.04

## TODO: Change <Path in Builder Stage>
COPY --from=builder /mayhem-nullc/build/nullcl/nullcl /

