#!/bin/bash

NET_A="dpdk-test-net-a"
NET_B="dpdk-test-net-b"

cleanup() {
    podman kill --signal SIGINT forwarder >/dev/null 2>&1 || true
    rm -rf /dev/hugepages/*
    podman network rm $NET_A $NET_B  || true
}

build() {
    echo "Building images and compiling forwarder..."
    podman build --target dpdk-builder -t dpdk-builder:latest .
    podman build --target dpdk-forwarder -t dpdk-forwarder:latest .
    podman image prune -f --filter "label=project=dpdk-recruitment"
}

start() {
    LOG_DIR="$(pwd)/logs"
    mkdir -p "$LOG_DIR"
    podman network create $NET_A >/dev/null 2>&1 || true
    podman network create $NET_B >/dev/null 2>&1 || true
    
    rm -rf /dev/hugepages/*
    
    echo "Starting forwarder..."
    podman run -d -t --rm --name forwarder \
        --network $NET_A \
        --network $NET_B \
	--userns=keep-id \
	--ipc=host \
	--cap-add=net_raw,net_admin \
	--ulimit memlock=-1:-1 \
	--security-opt label=disable \
	-v /dev/hugepages:/dev/hugepages:rw \
	-v "$LOG_DIR":/app/logs:Z \
	dpdk-forwarder:latest \
	/app/forwarder \
	-l 0-2 -n 4 \
	--file-prefix=rte \
	--vdev=net_af_packet0,iface=eth0 \
	--vdev=net_af_packet1,iface=eth1 \
	-- \
	--export-interval 10 --timeout 60 --hash-entries 1024 \
	--podman-mode

    podman exec -u 0 forwarder ip addr flush dev eth0
    podman exec -u 0 forwarder ip addr flush dev eth1
    
    podman exec -u 0 forwarder ip link set eth0 promisc on
    podman exec -u 0 forwarder ip link set eth1 promisc on
}

pdump() {
    # Ensure capture directory exists
    mkdir -p ./logs/captures

    case "$1" in
        rx)
            echo "Capturing RX traffic on Port 0 (eth0)..."
            podman exec -it forwarder /usr/bin/dpdk-pdump \
                --file-prefix=rte \
                --lcore 4 \
                -- \
                --pdump 'port=1,queue=*,rx-dev=/app/logs/captures/rx.pcap'
            ;;
        tx)
            echo "Capturing TX traffic on Port 1 (eth1)..."
            podman exec -it forwarder /usr/bin/dpdk-pdump \
                --file-prefix=rte \
                --lcore 4 \
                -- \
                --pdump 'port=0,queue=*,tx-dev=/app/logs/captures/tx.pcap'
            ;;
        *)
            echo "Usage: $0 tcpdump {rx,tx}"
            exit 1
            ;;
    esac
}

logs() {
    if podman ps --format "{{.Names}}" | grep -q "forwarder"; then
        podman logs -f forwarder
    else
        echo "Forwarder container is not running."
        exit 1
    fi
}

stop() {
    if podman ps --format "{{.Names}}" | grep -q "forwarder"; then
        echo "Stopping forwarder..."
        podman kill --signal SIGINT forwarder >/dev/null 2>&1 || true
        sleep 2
        podman stop forwarder >/dev/null 2>&1 || true
    else
        echo "Forwarder is not running."
    fi
}

destroy() {
    cleanup
    echo "Removing podman images..."
    podman image rm dpdk-forwarder
    podman image rm dpdk-builder
    podman builder prune -f --filter "label=project=dpdk-recruitment" >/dev/null 2>&1 || true
}

usage() {
    echo "Usage: $0 {build|start|stop|cleanup|destroy}"
    echo "  build:   Compile the DPDK app and build the Podman images"
    echo "  start:   Setup networks and run the forwarder"
    echo "  logs:    Connects to forwarder application output"
    echo "  pdump:   Monitor traffic on RX or TX side of the forwarder"
    echo "  stop:    Gracefully signal the forwarder to stop and save stats"
    echo "  cleanup: Stop containers and remove virtual networks"
    echo "  destroy: Wipe everything, including the built images"
    exit 1
}

case "$1" in
    build) build ;;
    start) start ;;
    stop) stop ;;
    logs) logs ;;
    pdump) pdump "$2" ;;
    cleanup) cleanup ;;
    destroy) destroy ;;
    *) usage ;;
esac
