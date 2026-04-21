# Multi-Container Runtime Demo Script Explanation

## Team Information
- **Arun Hariharan** (PES2UG24AM126)
- **Krish Arun** (PES2UG24AM078)

---

## Overview

This document provides a comprehensive explanation of the `demo.sh` script execution, detailing all 15 steps and how they interconnect to demonstrate the complete multi-container runtime system.

---

## Step 1: Checking Prerequisites

**What happens:**
- Runs `environment-check.sh` to verify system compatibility
- Checks for Ubuntu 22.04/24.04
- Verifies kernel headers are installed
- Validates build tools availability

**Why it matters:**
The container runtime requires specific kernel features (namespaces, cgroups) and build tools. Kernel headers are essential for compiling the kernel module (`monitor.ko`). Without proper prerequisites, subsequent steps will fail.

**Technical details:**
- Kernel headers location: `/lib/modules/$(uname -r)/build`
- Required packages: `build-essential`, `linux-headers-$(uname -r)`
- The script continues even if checks fail (with warnings) to allow manual verification

**Connection to next steps:**
Prerequisites validation ensures Step 2 (build) will succeed.

---

## Step 2: Building Project

**What happens:**
- Executes `make clean` to remove old artifacts
- Compiles all components via `make`:
  - `engine` - user-space runtime binary
  - `monitor.ko` - kernel module for memory monitoring
  - `memory_hog`, `cpu_hog`, `io_pulse` - test workload binaries

**Why it matters:**
This step creates all executable components needed for the runtime. The `engine` binary handles container lifecycle, the kernel module enforces memory limits, and workload binaries test different resource usage patterns.

**Technical details:**
- `engine.c` compiles with pthread support for multi-threading
- `monitor.ko` is built against current kernel headers
- Workload binaries are statically linked for portability inside containers

**Connection to next steps:**
Built binaries are used in Step 3 (copied to rootfs), Step 4 (kernel module loaded), and Step 5 (supervisor started).

---

## Step 3: Preparing Root Filesystem

**What happens:**
- Creates `rootfs-base` directory if it doesn't exist
- Downloads Alpine Linux minirootfs (3.20.3) if not present
- Extracts the tarball to create a minimal Linux filesystem
- Copies workload binaries (`memory_hog`, `cpu_hog`, `io_pulse`) into rootfs

**Why it matters:**
Containers need an isolated filesystem to operate. The rootfs provides a complete Linux userland (shell, utilities, libraries) that containers will use as their root directory via `chroot`. Alpine Linux is chosen for its small size (~3MB).

**Technical details:**
- Alpine minirootfs contains: `/bin`, `/etc`, `/lib`, `/usr`, `/proc`, `/sys`
- Workload binaries are copied so containers can execute them
- This is a "base" rootfs that will be copied per-container in Step 6

**Connection to next steps:**
The base rootfs is used in Step 5 (supervisor argument) and Step 6 (per-container copies).

---

## Step 4: Loading Kernel Module

**What happens:**
- Unloads existing `monitor` module if already loaded
- Loads `monitor.ko` using `insmod`
- Verifies `/dev/container_monitor` device file is created
- Displays device file permissions

**Why it matters:**
The kernel module provides memory monitoring and enforcement capabilities that cannot be implemented in user space. It creates a character device that the supervisor uses to register containers and set memory limits.

**Technical details:**
- Module creates `/dev/container_monitor` with major/minor numbers
- Supports ioctl operations: `MONITOR_REGISTER`, `MONITOR_UNREGISTER`
- Runs a timer callback every second to check RSS of monitored processes
- Enforces soft limits (warnings) and hard limits (SIGKILL)

**Connection to next steps:**
The supervisor (Step 5) opens `/dev/container_monitor` to register containers with memory limits.

---

## Step 5: Starting Supervisor

**What happens:**
- Launches `./engine supervisor ./rootfs-base` in background
- Captures supervisor PID for later management
- Waits 2 seconds for initialization
- Verifies supervisor is running

**Why it matters:**
The supervisor is the central orchestrator of the container runtime. It manages container lifecycle, handles IPC requests, coordinates logging, and reaps child processes. Without it, no containers can be started.

**Technical details:**
- Creates UNIX domain socket at `/tmp/mini_runtime.sock` for control IPC
- Opens `/dev/container_monitor` for kernel module communication
- Initializes bounded buffer for log aggregation
- Spawns logging thread to consume log entries
- Enters event loop to accept client connections and reap children

**Connection to next steps:**
All subsequent container operations (Steps 6-14) communicate with this supervisor via the control socket.

---

## Step 6: Creating Per-Container Rootfs Copies

**What happens:**
- Removes old `rootfs-alpha` and `rootfs-beta` directories
- Creates fresh copies from `rootfs-base` using `cp -a`
- Each container gets its own isolated filesystem

**Why it matters:**
Containers must have separate rootfs copies to maintain isolation. If they shared the same rootfs, file modifications in one container would affect others. Each copy provides a clean, independent filesystem.

**Technical details:**
- `cp -a` preserves permissions, ownership, and timestamps
- Each rootfs is ~3MB (Alpine minirootfs size)
- Containers will `chroot` into their respective directories

**Connection to next steps:**
These rootfs copies are used as arguments in Step 7 when starting containers.

---

## Step 7: Launching Multiple Containers

**What happens:**
- Starts container `alpha` with `/bin/sh`, soft limit 48 MiB, hard limit 80 MiB
- Starts container `beta` with `/bin/sh`, soft limit 64 MiB, hard limit 96 MiB
- Both containers run concurrently under supervisor management

**Why it matters:**
This demonstrates multi-container supervision - the core feature of the runtime. Multiple isolated containers run simultaneously, each with independent namespaces, filesystems, and resource limits.

**Technical details:**
- CLI sends `CMD_START` request to supervisor via UNIX socket
- Supervisor creates pipe for logging (stdout/stderr capture)
- Supervisor calls `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS`
- Child process executes `child_fn()`: sets hostname, chroots, mounts /proc, execs command
- Supervisor registers container with kernel module for memory monitoring
- Supervisor spawns pipe reader thread to capture container output
- Supervisor adds container record to metadata list

**Connection to next steps:**
These running containers are queried in Step 8 (ps), logged in Step 9 (logs), and managed in Step 14 (stop).

---

## Step 8: Container Metadata (ps command)

**What happens:**
- Executes `./engine ps` to query supervisor
- Displays table with: ID, PID, STATE, SOFT_MiB, HARD_MiB
- Shows metadata for all tracked containers

**Why it matters:**
This demonstrates the control-plane IPC and metadata tracking. The supervisor maintains state for all containers and responds to queries, similar to `docker ps`.

**Technical details:**
- CLI sends `CMD_PS` request via UNIX socket
- Supervisor locks `metadata_lock` mutex
- Iterates through `container_record_t` linked list
- Formats output with container ID, host PID, state, and limits
- Returns formatted string to client

**Connection to next steps:**
Metadata tracking is continuously updated as containers change state (Steps 10-14).

---

## Step 9: Container Logs

**What happens:**
- Executes `./engine logs alpha` and `./engine logs beta`
- Displays captured stdout/stderr from each container
- Shows "No logs yet" if containers haven't produced output

**Why it matters:**
This demonstrates the bounded-buffer logging system - a producer-consumer pattern with thread synchronization. Container output is captured, buffered, and written to per-container log files.

**Technical details:**
- Each container's stdout/stderr redirected to pipe write end
- Pipe reader thread reads chunks, pushes to bounded buffer
- Logging thread pops from buffer, writes to `logs/<container-id>.log`
- Bounded buffer uses mutex and condition variables for synchronization
- CLI reads log file and returns contents

**Connection to next steps:**
Logs accumulate as containers run workloads (Steps 10-13).

---

## Step 10: Memory Test - Soft Limit Warning

**What happens:**
- Starts `mem-test` container running `/memory_hog`
- Sets soft limit to 40 MiB, hard limit to 64 MiB
- `memory_hog` allocates memory incrementally
- Waits 5 seconds for soft limit to be exceeded
- Checks `dmesg` for kernel warning messages

**Why it matters:**
This demonstrates soft-limit monitoring - a non-disruptive warning mechanism. The kernel module detects when RSS exceeds the soft limit and logs a warning without killing the process.

**Technical details:**
- Supervisor registers container with kernel module via ioctl
- Kernel module's timer callback checks RSS every second
- When RSS > soft_limit and warning not yet issued:
  - Logs message to kernel ring buffer (dmesg)
  - Sets `soft_warned` flag to avoid spam
- Process continues running normally

**Connection to next steps:**
If memory continues growing, Step 11 demonstrates hard limit enforcement.

---

## Step 11: Memory Test - Hard Limit Enforcement

**What happens:**
- Waits 10 more seconds for `memory_hog` to exceed hard limit
- Kernel module detects RSS > hard limit
- Sends SIGKILL to container process
- Container is terminated immediately
- Checks `dmesg` for kill message
- Verifies container state changed to "killed" via `ps`

**Why it matters:**
This demonstrates hard-limit enforcement - critical for preventing memory exhaustion. The kernel module forcibly terminates processes that exceed their hard limit, protecting system stability.

**Technical details:**
- Kernel module's timer callback detects RSS > hard_limit
- Calls `send_sig(SIGKILL, task, 1)` to terminate process
- Logs kill event to dmesg
- Supervisor receives SIGCHLD when child dies
- Supervisor calls `waitpid()` to reap zombie
- Updates container state to `CONTAINER_KILLED`
- Unregisters container from kernel module

**Connection to next steps:**
Demonstrates why kernel-space enforcement is necessary (user-space can be bypassed).

---

## Step 12: Scheduling Experiment 1 - CPU Priority

**What happens:**
- Launches `cpu1` running `/cpu_hog` with nice value 0 (higher priority)
- Launches `cpu2` running `/cpu_hog` with nice value 10 (lower priority)
- Both consume 100% CPU in infinite loop
- Runs for 15 seconds while monitoring with `ps`
- Observes CPU time distribution

**Why it matters:**
This demonstrates how the Linux CFS (Completely Fair Scheduler) allocates CPU time based on process priority. Nice values affect scheduling weight, giving higher-priority processes more CPU time.

**Technical details:**
- `nice()` system call adjusts process priority (-20 to 19)
- Lower nice value = higher priority = more CPU time
- CFS calculates time slices based on weight
- Expected result: cpu1 gets ~65% CPU, cpu2 gets ~35%
- Both processes make progress (no starvation)

**Connection to next steps:**
Step 13 contrasts CPU-bound vs I/O-bound scheduling behavior.

---

## Step 13: Scheduling Experiment 2 - CPU vs I/O Bound

**What happens:**
- Stops previous CPU workloads
- Launches `cpu1` running `/cpu_hog` (CPU-bound)
- Launches `io1` running `/io_pulse` (I/O-bound)
- Both have same nice value (0)
- Runs for 10 seconds while monitoring
- Observes responsiveness differences

**Why it matters:**
This demonstrates how CFS treats workloads with different characteristics. I/O-bound processes get better responsiveness because they frequently yield the CPU, while CPU-bound processes maximize throughput.

**Technical details:**
- `cpu_hog`: infinite loop, never blocks, consumes CPU continuously
- `io_pulse`: writes to file, calls fsync(), sleeps - frequently blocks
- When I/O-bound process blocks, CPU-bound process runs
- When I/O-bound process wakes up, CFS schedules it quickly (low latency)
- Result: I/O-bound appears more responsive, completes faster

**Connection to next steps:**
After experiments, Step 14 cleans up all running containers.

---

## Step 14: Stopping Containers

**What happens:**
- Executes `./engine stop` for each running container
- Supervisor sends SIGTERM to container processes
- Waits for graceful shutdown
- Updates container state to "stopped"
- Displays final `ps` output showing all containers stopped

**Why it matters:**
This demonstrates graceful container shutdown and state management. The supervisor can selectively stop containers while keeping others running.

**Technical details:**
- CLI sends `CMD_STOP` request with container ID
- Supervisor locks metadata, finds container by ID
- Calls `kill(pid, SIGTERM)` to request graceful shutdown
- Updates state to `CONTAINER_STOPPED`
- Container process receives signal, exits
- Supervisor reaps child via `waitpid()` in event loop

**Connection to next steps:**
Step 15 performs final cleanup of supervisor and kernel module.

---

## Step 15: Verifying Clean Teardown

**What happens:**
- Checks for zombie processes (none should exist)
- Stops supervisor process with SIGTERM
- Unloads kernel module with `rmmod monitor`
- Verifies module is unloaded via `lsmod`
- Confirms clean system state

**Why it matters:**
This demonstrates proper resource cleanup - essential for production systems. All processes are reaped, kernel resources are freed, and the system returns to pre-demo state.

**Technical details:**
- Supervisor receives SIGTERM, begins shutdown
- Calls `bounded_buffer_begin_shutdown()` to wake threads
- Logging thread drains remaining buffer entries, exits
- Supervisor joins logging thread with `pthread_join()`
- Closes control socket and monitor device
- Destroys mutexes and condition variables
- Kernel module cleanup: removes device file, frees monitored list, cancels timer

**Final state:**
- No lingering processes
- No kernel module loaded
- No device files
- Clean logs directory with per-container log files

---

## System Architecture Summary

### Component Interactions

```
┌─────────────────────────────────────────────────────────────┐
│                         CLI Commands                         │
│              (start, stop, ps, logs)                        │
└────────────────────────┬────────────────────────────────────┘
                         │ UNIX Socket IPC
                         │ (Control Plane)
┌────────────────────────▼────────────────────────────────────┐
│                      Supervisor Process                      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │
│  │  Event Loop  │  │ Metadata List│  │ Bounded Buf  │     │
│  │  (select)    │  │  (mutex)     │  │  (cond vars) │     │
│  └──────────────┘  └──────────────┘  └──────────────┘     │
│         │                  │                  │             │
│         │                  │                  │             │
│    ┌────▼─────┐      ┌────▼─────┐      ┌────▼─────┐      │
│    │ Accept   │      │ Reaper   │      │ Logger   │      │
│    │ Clients  │      │ (SIGCHLD)│      │ Thread   │      │
│    └──────────┘      └──────────┘      └──────────┘      │
└────────────┬────────────────────────────────────┬──────────┘
             │                                    │
             │ clone() + namespaces               │ Pipe (logs)
             │                                    │
┌────────────▼────────────────────────────────────▼──────────┐
│                    Container Processes                      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │   alpha      │  │    beta      │  │  mem-test    │    │
│  │ (PID NS)     │  │  (PID NS)    │  │  (PID NS)    │    │
│  │ (chroot)     │  │  (chroot)    │  │  (chroot)    │    │
│  └──────────────┘  └──────────────┘  └──────────────┘    │
└────────────────────────┬────────────────────────────────────┘
                         │ ioctl (register/unregister)
┌────────────────────────▼────────────────────────────────────┐
│                    Kernel Module (monitor.ko)               │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  Timer Callback (every 1 second)                     │  │
│  │  - Check RSS of monitored processes                  │  │
│  │  - Warn on soft limit breach                         │  │
│  │  - Kill on hard limit breach                         │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### Data Flow

1. **Control Flow (CLI → Supervisor):**
   - CLI connects to UNIX socket
   - Sends control_request_t struct
   - Supervisor processes request
   - Returns control_response_t struct

2. **Logging Flow (Container → Supervisor → File):**
   - Container writes to stdout/stderr
   - Pipe captures output
   - Pipe reader thread pushes to bounded buffer
   - Logger thread pops from buffer
   - Writes to per-container log file

3. **Memory Monitoring Flow (Kernel → Process):**
   - Supervisor registers container via ioctl
   - Kernel module adds to monitored list
   - Timer callback checks RSS periodically
   - Logs warnings or sends SIGKILL
   - Supervisor reaps killed processes

### Key Synchronization Points

1. **Bounded Buffer:**
   - Mutex protects head/tail/count
   - `not_empty` condition: consumers wait when empty
   - `not_full` condition: producers wait when full
   - Shutdown flag: graceful termination

2. **Metadata List:**
   - Mutex protects container linked list
   - Prevents races between: add (start), read (ps), update (reaper)

3. **Kernel Module:**
   - Mutex protects monitored process list
   - Prevents races between: register, unregister, timer callback

---

## Learning Outcomes

This demo comprehensively demonstrates:

1. **Process Isolation:** Namespaces (PID, UTS, mount) + chroot
2. **Multi-Container Management:** Supervisor pattern with IPC
3. **Thread Synchronization:** Bounded buffer with mutex/condvars
4. **Kernel-User Interaction:** Character device + ioctl
5. **Memory Management:** RSS monitoring + soft/hard limits
6. **Process Scheduling:** Nice values + CPU vs I/O workloads
7. **Signal Handling:** SIGCHLD reaping + SIGTERM shutdown
8. **Resource Cleanup:** Graceful teardown of all components

Each step builds on previous steps, creating a complete container runtime system from scratch.

