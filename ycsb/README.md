# Built-in YCSB

YCSB is part of `redis-server`. It requires no shared library or `loadmodule` directive. The adapter uses the Redis module API inside the server.

## Build and run

From the repository root:

```sh
make -C src -j4 redis-server redis-cli
src/redis-server redis.conf
```

In another terminal:

```sh
src/redis-cli ycsb.run ycsb/workloads/workloada.spec
```

The command loads records, runs the workload, prints metrics to the server output, and returns `OK`. It blocks normal command processing until the workload ends. Use a dedicated benchmark server. The command writes to the selected database and can replace existing keys.

Workloads A, B, C, D, and F use supported operations. Workload E requires scans and is not supported. Record count must be at least 2. Operation count must be non-negative.

For cross-compilation, select matching C and C++ compilers:

```sh
make -C src CC=riscv64-unknown-linux-gnu-gcc CXX=riscv64-unknown-linux-gnu-g++ redis-server
```

Yukon hooks still use `dlsym`. They remain optional and work through `LD_PRELOAD`; they do not load the YCSB code.

## Run once at startup

```sh
src/redis-server redis.conf --ycsb-run ycsb/workloads/workloada.spec
```

This mode runs `ycsb.run` on database 0 and exits. It starts with an empty database, skips network listeners and disk loading, and disables AOF, snapshots, supervision, and daemon mode. Cluster, replica, and Sentinel configurations are rejected. Other settings, such as the allocator, CPU affinity, and memory limit, still apply. Relative workload paths resolve from the launch directory, before Redis applies `dir`.

Exit status is 0 on success and 1 on a command error. Metrics go to standard output. `perf stat` covers the full process, including initialization and record loading. The `YUKON_YCSB_*` metrics cover the workload phases.

`test.sh` uses this mode for three benchmark runs:

```sh
bash test.sh ycsb/workloads/workloada.spec
```

The Yukon runs require the two stub libraries in the repository root.

## Tests

```sh
./runtest --single unit/ycsb
```
