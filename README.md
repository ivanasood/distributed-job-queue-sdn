# Distributed Job Queue and Worker Management using SDN

A distributed job processing system built using C socket programming, TCP/UDP communication, multithreading, Mininet, Open vSwitch, and an SDN controller.

The system distributes computational tasks across multiple workers while the SDN controller manages network forwarding and worker availability.

## Features

- TCP-based client-manager communication.
- TCP-based worker registration and job distribution.
- UDP heartbeat monitoring.
- Concurrent job handling using POSIX threads.
- Job queue and round-robin worker scheduling.
- Worker failure detection and availability monitoring.
- SDN-controlled forwarding using OpenFlow 1.3.
- Dynamic route discovery through an HTTP API.
- Worker isolation using SDN flow rules.
- Mininet-based network emulation.

## Architecture

| Component | Responsibility |
|---|---|
| Client | Submits computational jobs |
| Manager | Maintains the job queue, registers workers and schedules jobs |
| Workers | Execute assigned jobs and send heartbeats |
| SDN Controller | Controls forwarding, routes and worker availability |
| Mininet | Emulates the network using Open vSwitch |

### Communication

- Client ↔ Manager: TCP
- Worker ↔ Manager: TCP
- Worker → Manager heartbeat: UDP
- Manager ↔ SDN Controller: HTTP REST API
- Controller ↔ Switches: OpenFlow 1.3

## Network Topology

The project uses a diamond-shaped topology consisting of:

- Four Open vSwitch switches: s1, s2, s3 and s4.
- Two worker hosts: h1 and h2.
- One manager node connected to s1.
- Two alternative paths between the worker-side switches.

The controller installs forwarding rules and supports alternate-path forwarding when a link fails.

## Technology Stack

- C and POSIX sockets
- POSIX threads (pthreads)
- Python and OS-Ken
- OpenFlow 1.3
- Mininet and Open vSwitch
- Linux / Ubuntu
- GCC and Make

## Project Structure

```text
distributed-job-system/
├── client/
│   └── client.c
├── common/
│   ├── protocol.c
│   └── protocol.h
├── manager/
│   ├── server.c
│   ├── job_queue.c
│   ├── scheduler.c
│   └── worker_registry.c
├── worker/
│   └── worker.c
├── sdn/
│   ├── controller.py
│   └── topology.py
├── tests/
│   ├── benchmark.sh
│   ├── benchmark_results.txt
│   ├── test_plan.md
│   └── results/
├── Makefile
├── README.md
└── .gitignore
```

## Build

Run inside the Ubuntu environment:

```bash
make -B all
```

This generates `manager_program`, `client_program` and `worker_program`.

To remove compiled binaries:

```bash
make clean
```

## Running the Project

Start each component in a separate Ubuntu terminal.

### 1. Start the SDN Controller

Activate the OS-Ken environment:

```bash
source ~/osken311-venv/bin/activate
```

Start the controller:

```bash
osken-manager --ofp-tcp-listen-port 6633 sdn/controller.py os_ken.controller.ofp_handler
```

### 2. Start Mininet

```bash
sudo mn -c
sudo python3 sdn/topology.py
```

### 3. Start the Manager

```bash
stdbuf -oL -eL ./manager_program
```

### 4. Start Workers

From the Mininet CLI:

```bash
h1 sh -c 'MANAGER_IP=10.0.0.254 /mnt/utm/worker_program 1 > /tmp/worker1.log 2>&1 &'
h2 sh -c 'MANAGER_IP=10.0.0.254 /mnt/utm/worker_program 2 > /tmp/worker2.log 2>&1 &'
```

### 5. Submit Jobs

From the Mininet CLI:

```bash
h1 sh -c 'MANAGER_IP=10.0.0.254 /mnt/utm/client_program'
```

## Supported Job Types

| Job | Description | Example result |
|---|---|---|
| SUM | Calculates the sum of a range | SUM 1–100 = 5050 |
| FACTORIAL | Calculates factorial | FACTORIAL 5 = 120 |
| PRIME | Checks whether a number is prime | PRIME 17 = 1 |

## Worker Monitoring and SDN Integration

Workers periodically send UDP heartbeat messages to the manager.

- Heartbeat interval: 3 seconds.
- Worker timeout: 10 seconds.
- The manager detects workers that stop responding.
- The manager notifies the SDN controller through its HTTP API.
- The controller installs a high-priority drop rule for an unavailable worker.
- When a worker registers again, the manager marks it available and notifies the controller.

Controller API endpoints:

- `GET /route?worker=worker1`
- `GET /route?worker=worker2`
- `POST /worker-status`

## Testing and Results

Tested scenarios include:

- Worker registration and job submission.
- SUM, FACTORIAL and PRIME execution.
- Round-robin distribution across two workers.
- UDP heartbeat acknowledgements.
- Worker failure detection and SDN blocking.
- Worker re-registration.
- Mininet connectivity and link failure/recovery.
- OpenFlow rule inspection.

### Performance Comparison

A 40-job benchmark was executed using static forwarding and SDN-controlled forwarding.

| Configuration | Jobs completed | Elapsed time |
|---|---:|---:|
| Static forwarding | 40/40 | 0.05 seconds |
| SDN-controlled forwarding | 40/40 | 0.04 seconds |

These are single-run measurements in a virtualized Mininet/UTM environment. The difference is too small to establish a meaningful performance advantage.

## Limitations

- Performance measurements use small workloads in a virtualized environment.
- A controller restart can leave stale OpenFlow rules in switches. Manual flow cleanup may be required before a worker reconnects.
- The benchmark timings should not be interpreted as physical network throughput.

## Future Improvements

- Persistent job and worker state.
- Load-aware scheduling.
- Automated stale-flow cleanup after controller restart.
- Larger benchmark workloads and repeated measurements.
- Improved fault recovery and monitoring.

## Authors

Developed as an academic mini project for the SDN and Socket Programming course.