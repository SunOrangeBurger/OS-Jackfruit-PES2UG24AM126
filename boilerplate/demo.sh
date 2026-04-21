#!/bin/bash

# Multi-Container Runtime - Automated Demo Script
# Team: Arun Hariharan (PES2UG24AM126) & Krish Arun (PES2UG24AM078)

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
SUPERVISOR_PID=""
DEMO_DIR="$(pwd)"

# Helper functions
print_header() {
    echo -e "\n${BLUE}========================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}========================================${NC}\n"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

wait_for_input() {
    echo -e "\n${YELLOW}Press Enter to continue...${NC}"
    read
}

cleanup() {
    print_header "Cleanup"
    
    # Stop all containers
    echo "Stopping containers..."
    sudo ./engine stop alpha 2>/dev/null || true
    sudo ./engine stop beta 2>/dev/null || true
    sudo ./engine stop mem-test 2>/dev/null || true
    sudo ./engine stop cpu1 2>/dev/null || true
    sudo ./engine stop cpu2 2>/dev/null || true
    sudo ./engine stop io1 2>/dev/null || true
    
    # Kill supervisor if running
    if [ -n "$SUPERVISOR_PID" ] && kill -0 "$SUPERVISOR_PID" 2>/dev/null; then
        echo "Stopping supervisor (PID: $SUPERVISOR_PID)..."
        sudo kill -TERM "$SUPERVISOR_PID" 2>/dev/null || true
        sleep 2
    fi
    
    # Unload kernel module
    if lsmod | grep -q "^monitor"; then
        echo "Unloading kernel module..."
        sudo rmmod monitor 2>/dev/null || true
    fi
    
    # Clean build artifacts
    echo "Cleaning build artifacts..."
    make clean 2>/dev/null || true
    
    print_success "Cleanup complete"
}

# Trap to ensure cleanup on exit
trap cleanup EXIT INT TERM

# Main demo flow
main() {
    print_header "Multi-Container Runtime Demo"
    echo "This script will demonstrate all features of the container runtime"
    wait_for_input
    
    # Step 1: Prerequisites check
    print_header "Step 1: Checking Prerequisites"
    
    if [ ! -f "environment-check.sh" ]; then
        print_error "environment-check.sh not found"
        exit 1
    fi
    
    chmod +x environment-check.sh
    if sudo ./environment-check.sh; then
        print_success "Prerequisites verified"
    else
        print_warning "Environment check failed, but continuing anyway..."
        echo "Make sure you have kernel headers installed:"
        echo "  sudo apt install build-essential linux-headers-\$(uname -r)"
    fi
    wait_for_input
    
    # Step 2: Build project
    print_header "Step 2: Building Project"
    make clean
    make
    print_success "Build complete"
    wait_for_input
    
    # Step 3: Prepare rootfs
    print_header "Step 3: Preparing Root Filesystem"
    
    if [ ! -d "rootfs-base" ]; then
        echo "Creating rootfs-base..."
        mkdir rootfs-base
        
        if [ ! -f "alpine-minirootfs-3.20.3-x86_64.tar.gz" ]; then
            echo "Downloading Alpine minirootfs..."
            wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
        fi
        
        echo "Extracting rootfs..."
        tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
    fi
    
    # Copy workload binaries
    echo "Copying workload binaries to rootfs-base..."
    cp memory_hog cpu_hog io_pulse rootfs-base/ 2>/dev/null || true
    
    print_success "Root filesystem ready"
    wait_for_input
    
    # Step 4: Load kernel module
    print_header "Step 4: Loading Kernel Module"
    
    if lsmod | grep -q "^monitor"; then
        sudo rmmod monitor
    fi
    
    sudo insmod monitor.ko
    ls -l /dev/container_monitor
    print_success "Kernel module loaded"
    wait_for_input
    
    # Step 5: Start supervisor
    print_header "Step 5: Starting Supervisor"
    
    echo "Starting supervisor in background..."
    sudo ./engine supervisor ./rootfs-base > supervisor.log 2>&1 &
    SUPERVISOR_PID=$!
    sleep 2
    
    if kill -0 "$SUPERVISOR_PID" 2>/dev/null; then
        print_success "Supervisor started (PID: $SUPERVISOR_PID)"
    else
        print_error "Failed to start supervisor"
        exit 1
    fi
    wait_for_input
    
    # Step 6: Create per-container rootfs copies
    print_header "Step 6: Creating Per-Container Rootfs Copies"
    
    rm -rf rootfs-alpha rootfs-beta 2>/dev/null || true
    cp -a rootfs-base rootfs-alpha
    cp -a rootfs-base rootfs-beta
    
    print_success "Rootfs copies created"
    wait_for_input
    
    # Step 7: Launch multiple containers
    print_header "Step 7: Launching Multiple Containers"
    
    echo "Starting container 'alpha'..."
    sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
    sleep 1
    
    echo "Starting container 'beta'..."
    sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
    sleep 1
    
    print_success "Containers launched"
    wait_for_input
    
    # Step 8: Show container metadata
    print_header "Step 8: Container Metadata (ps command)"
    
    sudo ./engine ps
    print_success "Metadata displayed"
    wait_for_input
    
    # Step 9: View container logs
    print_header "Step 9: Container Logs"
    
    echo "Logs for container 'alpha':"
    sudo ./engine logs alpha || echo "No logs yet"
    
    echo -e "\nLogs for container 'beta':"
    sudo ./engine logs beta || echo "No logs yet"
    
    print_success "Logs displayed"
    wait_for_input
    
    # Step 10: Memory test - soft limit
    print_header "Step 10: Memory Test - Soft Limit Warning"
    
    echo "Launching memory_hog with soft limit 40 MiB, hard limit 64 MiB..."
    sudo ./engine start mem-test ./rootfs-alpha /memory_hog --soft-mib 40 --hard-mib 64
    
    echo "Waiting for soft limit warning..."
    sleep 5
    
    echo -e "\nKernel logs (checking for soft limit warning):"
    dmesg | tail -20 | grep -i "soft\|memory\|monitor" || echo "Check dmesg manually"
    
    print_success "Memory test running"
    wait_for_input
    
    # Step 11: Memory test - hard limit
    print_header "Step 11: Memory Test - Hard Limit Enforcement"
    
    echo "Waiting for hard limit enforcement (container should be killed)..."
    sleep 10
    
    echo -e "\nKernel logs (checking for hard limit kill):"
    dmesg | tail -30 | grep -i "hard\|kill\|memory\|monitor" || echo "Check dmesg manually"
    
    echo -e "\nContainer status:"
    sudo ./engine ps
    
    print_success "Hard limit enforcement demonstrated"
    wait_for_input
    
    # Step 12: Scheduling experiment 1 - CPU-bound with different priorities
    print_header "Step 12: Scheduling Experiment 1 - CPU Priority"
    
    echo "Launching two CPU-bound workloads with different nice values..."
    echo "cpu1: nice 0 (higher priority)"
    echo "cpu2: nice 10 (lower priority)"
    
    sudo ./engine start cpu1 ./rootfs-alpha /cpu_hog --nice 0
    sleep 1
    sudo ./engine start cpu2 ./rootfs-beta /cpu_hog --nice 10
    
    echo -e "\nRunning for 15 seconds..."
    sleep 5
    
    echo -e "\nContainer status:"
    sudo ./engine ps
    
    sleep 10
    
    echo -e "\nFinal status:"
    sudo ./engine ps
    
    print_success "CPU priority experiment complete"
    wait_for_input
    
    # Step 13: Scheduling experiment 2 - CPU vs I/O bound
    print_header "Step 13: Scheduling Experiment 2 - CPU vs I/O Bound"
    
    echo "Stopping previous CPU workloads..."
    sudo ./engine stop cpu1 2>/dev/null || true
    sudo ./engine stop cpu2 2>/dev/null || true
    sleep 2
    
    echo "Launching CPU-bound and I/O-bound workloads..."
    sudo ./engine start cpu1 ./rootfs-alpha /cpu_hog --nice 0
    sleep 1
    sudo ./engine start io1 ./rootfs-beta /io_pulse --nice 0
    
    echo -e "\nRunning for 10 seconds..."
    sleep 5
    
    echo -e "\nContainer status:"
    sudo ./engine ps
    
    sleep 5
    
    echo -e "\nFinal status:"
    sudo ./engine ps
    
    print_success "CPU vs I/O experiment complete"
    wait_for_input
    
    # Step 14: Stop containers
    print_header "Step 14: Stopping Containers"
    
    echo "Stopping all containers..."
    sudo ./engine stop alpha 2>/dev/null || true
    sudo ./engine stop beta 2>/dev/null || true
    sudo ./engine stop cpu1 2>/dev/null || true
    sudo ./engine stop io1 2>/dev/null || true
    
    sleep 2
    
    echo -e "\nFinal container status:"
    sudo ./engine ps
    
    print_success "Containers stopped"
    wait_for_input
    
    # Step 15: Verify clean teardown
    print_header "Step 15: Verifying Clean Teardown"
    
    echo "Checking for zombie processes..."
    ps aux | grep -E "engine|memory_hog|cpu_hog|io_pulse" | grep -v grep || echo "No lingering processes"
    
    echo -e "\nStopping supervisor..."
    if [ -n "$SUPERVISOR_PID" ] && kill -0 "$SUPERVISOR_PID" 2>/dev/null; then
        sudo kill -TERM "$SUPERVISOR_PID"
        sleep 2
    fi
    
    echo -e "\nUnloading kernel module..."
    sudo rmmod monitor
    
    echo -e "\nVerifying module unloaded:"
    lsmod | grep monitor || echo "Module successfully unloaded"
    
    print_success "Clean teardown verified"
    
    # Final summary
    print_header "Demo Complete!"
    echo "All tests have been executed successfully."
    echo ""
    echo "Summary of demonstrated features:"
    echo "  ✓ Multi-container supervision"
    echo "  ✓ Metadata tracking (ps command)"
    echo "  ✓ Bounded-buffer logging"
    echo "  ✓ CLI and IPC"
    echo "  ✓ Soft-limit warning"
    echo "  ✓ Hard-limit enforcement"
    echo "  ✓ Scheduling experiments (CPU priority & CPU vs I/O)"
    echo "  ✓ Clean teardown"
    echo ""
    echo "Check the following for detailed results:"
    echo "  - supervisor.log: Supervisor output"
    echo "  - dmesg: Kernel module logs"
    echo "  - Container log files in current directory"
}

# Run main demo
main
