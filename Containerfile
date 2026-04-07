# Builder image
FROM fedora:43 AS dpdk-builder
LABEL project="dpdk-recruitment"
RUN dnf install -y gcc meson ninja pkgconf-pkg-config dpdk-devel libibverbs-devel

WORKDIR /app
COPY . .
RUN meson setup build --buildtype=debug && ninja -C build

# Forwarder image
FROM fedora:43 AS dpdk-forwarder
LABEL project="dpdk-recruitment"
RUN dnf install -y dpdk dpdk-tools libibverbs tcpdump iproute && dnf clean all

WORKDIR /app
COPY --from=dpdk-builder /app/build/forwarder /app/forwarder

