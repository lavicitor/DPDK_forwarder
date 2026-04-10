1. Instructions: Compile and run in container

The project uses a helper script `run.sh` to compile and run in a Podman container.  

`./run.sh build`          - Builds the application and forwarder container  
`./run.sh start`          - Creates two Podman networks and starts forwarder container attached to them  
`./run.sh logs`           - Connects to application output in container  
`./run.sh pdump <rx|tx>`  - Starts packet capture on either of the two ports used  
`./run.sh stop`           - Stops the application and container  
`./run.sh cleanup`        - Kills the container, cleans hugepages and removes Podman networks  
`./run.sh destroy`        - Removes container images  

2. Dependencies

- Hugepages configured on host system
- Podman for containerized execution and orchestration

3. Flow Tracking

The application extracts a 5-tuple (Source IP, Destination IP, Source Port, Destination Port, Protocol) from each IPv4 packet.  
Storage: Uses rte_hash for lookups.  
Timeout: Flows inactive for longer than the configured `--timeout` are removed during the export cycle to free table space.  
Affinity: Each worker core maintains its own local hash table. In this implementation, flow affinity is guaranteed by dedicated core-to-port mapping.  
- For bare metal code for using hardware RSS is included but not used since it is not supported in containers  


4. Export Format

Statistics are appended to logs/flow_stats_core_<id>.csv. The format is:  
Timestamp, SrcIP, DstIP, SrcPort, DstPort, Proto, RX_Packets, RX_Bytes, TX_Packets, TX_Bytes  

Metric	Description  
RX_Packets/Bytes - Data recorded upon ingress.  
TX_Packets/Bytes - Data recorded only after successful enqueue to the TX ring.  

5. Expected Output and Verification

Console Output

On a successful start, the logs will show:
```
EAL: Detected CPU lcores: 20
EAL: Detected NUMA nodes: 1
EAL: Detected shared linkage of DPDK
EAL: Multi-process socket /tmp/dpdk/rte/mp_socket
EAL: Selected IOVA mode 'VA'
DEBUG [../main.c:305]: --- DEBUG MODE ACTIVE ---Starting Forwarder (Export: 10s, Timeout: 60s, Podman: ON)
Pdump initialized successfully.
Core 1 starting TX worker...
Core 2 forwarding packets...
```

- ARP Test: The application includes an ARP responder. When traffic is sent to the interface, the app will log Valid ARP Request and reply to it.
```
DEBUG [../main.c:218]: ARP Packet received on Port 0
DEBUG [../main.c:222]: Valid ARP Request for IP: 10.89.1.21
DEBUG [../main.c:218]: ARP Packet received on Port 1
DEBUG [../main.c:222]: Valid ARP Request for IP: 10.89.2.19
DEBUG [../main.c:218]: ARP Packet received on Port 0
DEBUG [../main.c:222]: Valid ARP Request for IP: 10.89.1.12
```

- Traffic Forwarding: Send traffic into eth0. Use the included script to capture packets on the first interface `./run.sh pdump rx` or the second one `./run.sh pdump tx`.
- Logs: In the logs/ directory csv files are created with logged flow stats.
