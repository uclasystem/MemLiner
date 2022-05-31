# MemLiner README


MemLiner  is  a  managed  runtime  built  for  a  memory-disaggregated cluster where each managed application runson one server and uses both local memory and remote memory located on another server. When launched on MemLiner,the process fetches data from the remote server via the paging system.  MemLiner reduces the local-memory working set and improves the remote-memory prefetching by lining upthe memory accesses from application and GC threads. MemLiner is transparent to applications and can be integrated in any existing GC algorithms, such as G1 and Shenandoah. Please refer to our OSDI'22 paper, **[MemLiner](http://web.cs.ucla.edu/~harryxu/papers/memliner-osdi22.pdf)** for more details. 




# 1. Build & Install MemLiner

## 1.1 Environments

Two sets of environment have been tested for MemLiner:

```bash
Ubuntu 18.04
Linux 5.4
OpenJDK 12.04
GCC 5.5 
MLNX_OFED driver 4.9-2.2.4.0

or

CentOS 7.5 - 7.7
Linux 5.4
OpenJDK 12.04
GCC 5.5
MLNX_OFED driver 4.9-2.2.4.0 
```

Among the requirements, the Linux version, OpenJDK version and MLNX-OFED driver version are guaranteed during the build & installation process below. Just make sure that the Linux distribution version and gcc version are correct.

## 1.2 Install Kernel

Next we will use Ubuntu 18.04 as an example to show how to build and install the kernel. Please change the kernel on both CPU and memory server.

（1）Change the grub parameters

```bash
sudo vim /etc/default/grub

# Choose the bootup kernel version as 5.4.0
GRUB_DEFAULT="Advanced options for Ubuntu>Ubuntu, with Linux 5.4.0"

# Change the value of GRUB_CMDLINE_LINUX:
GRUB_CMDLINE_LINUX="nokaslr transparent_hugepage=madvise intel_pstate=disable idle=poll processor.max_cstate=0 intel_idle.max_cstate=0"

```

（2）Build the Kernel source code && install it. In case new kernel options are prompted during `sudo ./build_kernel.sh build`, press enter to use the default options.

```bash
# Change to the kernel folder:
cd MemLiner/Kernel

# In case new kernel options are prompted, press enter to use the default options.
sudo ./build_kernel.sh build
sudo ./build_kernel.sh install
sudo reboot
```

## 1.3 Install MLNX OFED Driver

**Preparations:**

MemLiner is only tested on `MLNX_OFED-4.9-2.2.4.0`. Download and unzip the package according to your system version, on both CPU and memory server.

Take Ubuntu 18.04 as an example:

### 1.3.1 Download & Install the MLNX_OFED driver

```bash
# Download the MLNX OFED driver for the Ubuntu 18.04
wget https://content.mellanox.com/ofed/MLNX_OFED-4.9-2.2.4.0/MLNX_OFED_LINUX-4.9-2.2.4.0-ubuntu18.04-x86_64.tgz
tar xzf MLNX_OFED_LINUX-4.9-2.2.4.0-ubuntu18.04-x86_64.tgz
cd MLNX_OFED_LINUX-4.9-2.2.4.0-ubuntu18.04-x86_64

# Remove the incompatible libraries
sudo apt remove ibverbs-providers:amd64 librdmacm1:amd64 librdmacm-dev:amd64 libibverbs-dev:amd64 libopensm5a libosmvendor4 libosmcomp3 -y

# Install the MLNX OFED driver against the kernel 5.4.0
sudo ./mlnxofedinstall --add-kernel-support
```

### 1.3.2 Enable the *opensm* and *openibd* services

(1) Enable and start the ***openibd*** service

```bash
sudo systemctl enable openibd
sudo systemctl start  openibd

# confirm the service is running and enabled:
sudo systemctl status openibd

# the log shown as:
● openibd.service - openibd - configure Mellanox devices
   Loaded: loaded (/lib/systemd/system/openibd.service; enabled; vendor preset: enabled)
   Active: active (exited) since Mon 2022-05-02 14:40:53 CST; 1min 24s ago
    
```

(2) Enable and start the ***opensmd*** service:

```bash
sudo systemctl enable opensmd
sudo systemctl start opensmd

# confirm the service status
sudo systemctl status opensmd

# the log shown as:
opensmd.service - LSB: Manage OpenSM
   Loaded: loaded (/etc/init.d/opensmd; generated)
   Active: active (running) since Mon 2022-05-02 14:53:39 CST; 10s ago

#
# Warning: you may encounter the problem:
#
opensmd.service is not a native service, redirecting to systemd-sysv-install.
Executing: /lib/systemd/systemd-sysv-install enable opensmd
update-rc.d: error: no runlevel symlinks to modify, aborting!

#
# Please refer to the **Question #2** in FAQ for how to solve this problem
#
```

### 1.3.3 Confirm the InfiniBand is available

Check the InfiniBand information

```bash
# Get the InfiniBand information
ibstat

# the log shown as:
# Adapter's stat should be Active.

	Port 1:
		State: Active
		Physical state: LinkUp
		Rate: 100
		Base lid: 3
		LMC: 0
		SM lid: 3
		Capability mask: 0x2651e84a
		Port GUID: 0x0c42a10300605e88
		Link layer: InfiniBand
```

### 1.3.4 For other OS distributions

### (1) CentOS 7.7

Install the necessary libraries

```bash
sudo yum install -y gtk2 atk cairo tcl tk
```

Download the MLNX OFED driver for CentOS 7.7

```bash
wget https://content.mellanox.com/ofed/MLNX_OFED-4.9-2.2.4.0/MLNX_OFED_LINUX-4.9-2.2.4.0-rhel7.7-x86_64.tgz
```

And then, repeat the steps from 3.3.1 to 3.3.3.

## 1.4 Build the RemoteSwap data path

The user needs to build the RemoteSwap on both the CPU server and memory servers.

### 1.4.1  Configuration

(1) IP configuration

Assign memory server’s ip address to both the CPU server and memory servers. Take `guest@zion-1.cs.ucla.edu`(CPU server) and `guest@zion-4.cs.ucla.edu`(Memory server) as an example. Memory server’s InfiniBand IP address is 10.0.0.4: (InfiniBand IP of zion-12 is 10.0.0.12. IPs of IB on other servers can be printed with `ifconfig ib0 | grep inet`)

```cpp
// (1) CPU server
// Replace the ${HOME}/Memliner/scripts/client/rswap_rdma.c:783:	char ip[] = "memory.server.ib.ip";
// to:
char ip[] = "10.0.0.4";

// (2) Memory server
// Replace the ${HOME}/Memliner/scripts/server/rswap_server.cpp:61:	const char *ip_str = "memory.server.ib.ip";
// to:
const char *ip_str = "10.0.0.4";
```

(2) Available cores configuration for RemoteSwap server (memory server).

Replace the macro, `ONLINE_CORES`, defined in `MemLiner/scripts/server/rswap_server.hpp` to the number of cores of the CPU server (which can be printed by command line, `nproc` , on the CPU server.)

```cpp
// ${HOME}/MemLiner/scripts/server/rswap_server.hpp:38:
#define ONLINE_CORES 16
```

### 1.4.2 Build the RemoteSwap datapath

(1) Build the client end on the CPU server, e.g., `guest@zion-1.cs.ucla.edu`

```bash
cd ${HOME}/MemLiner/scripts/client
make clean && make
```

(2) Build the server end on the memory server, e.g., `guest@zion-4.cs.ucla.edu`

```bash
cd ${HOME}/MemLiner/scripts/server
make clean && make
```

And then, please refer to **Section 1.1** for how to connect the CPU server and the memory server.

## 1.5 Build MemLiner (OpenJDK) on CPU server

Build MemLiner JDK. Please download and install jdk-12.0.2 and other dependent libraries to build the MemLiner (OpenJDK)

```bash
cd ${HOME}/MemLiner/JDK
./configure --with-boot-jdk=$HOME/jdk-12.0.2 --with-debug-level=release
make JOBS=32
```

# 2. Configuration

## 2.1 Connect the CPU server with the memory server

**Warning**: The CPU server and the memory server only need to be connected once, and the connection will persist until reboot or memory server process is killed. Before trying to connect, check whether they are already connected. See Question#1 in FAQ for instructions. If it is connected, directly go to section 1.2.

Launch the memory server:

```bash
# Log into memory server, e.g., guest@zion-4.cs.ucla.edu or guest@zion-12.cs.ucla.edu

# Let memory server run in background
tmux

# Launch the memory server and wait the connection from CPU serever
cd ${HOME}/MemLiner/scripts/server
./rswap-server
```

Launch the CPU server:

```bash
# Log into the CPU server, e.g., guest@zion-1.cs.ucla.edu or guest@zion-3.cs.ucla.edu
cd ${HOME}/MemLiner/scripts/client
./manage_rswap_client.sh install

# Confirm the success of the RDMA connection between the CPU server and the memory server.
# Print the kernel log by:
dmesg | grep "frontswap module loaded"

# the output should be:
[Some timestamp] frontswap module loaded
```

## 2.2 Limit the memory resources of applications

MemLiner relies on *cgroup* to limit the memory usage of applications running on the CPU server. we are using cgroup version 1 here. 

Create cgroup on the CPU server:

```bash
# Create a memory cgroup, memctl, for the applications running on the CPU server.
# $USER is the username of the account. It's "guest" here.
sudo cgcreate -t $USER -a $USER -g memory:/memctl

# Please confirm that a memory cgroup directory is built as:
/sys/fs/cgroup/memory/memctl
```

Limit local memory size on the CPU server. E.g., set local memory to 9GB:

```bash
# Please adjust the local memory size for different applications.
# e.g., for Spark with a 32GB Java heap. 25% local memory ratio means that we limit its CPU server memory size to 9GB.
# Application's working set includeds both Java heap and off-heap data, which is around 36GB in total. 
echo 9g > /sys/fs/cgroup/memory/memctl/memory.limit_in_bytes
```

# FAQ

## Question#1 Confirm the connection status of the CPU server and the memory server

Step 1: ensure that connection is already established.

```bash
# On CPU server
# Check RDMA connection between the CPU server and the memory server.
# search for connection built log by:
dmesg | grep "frontswap module loaded"

# If the output is:
[Some timestamp] frontswap module loaded
# Then the connection has been established.
```

If there is no connection, connect according to instructions on section 1.1.

Step 2: ensure that connection is still working.

**Warning**: Press `ctrl+b`, then press `d` to leave a tmux session. Don’t kill the process running.

```bash
# On memory server
# try attaching all tmux sessions. E.g., attach to the most recent session
tmux a

# If a command is still running and there's output like below, the connection is still active.
[...]
handle_cqe, 2-sided RDMA message sent done ?
handle_cqe, REQUEST_CHUNKS, Send available Regions to CPU server.
send_regions , Send registered Java heap to CPU server, 12 chunks.
send_message, message size = 248
handle_cqe, 2-sided RDMA message sent done ?
```

If the connection is not active, reboot both servers. After the reboot, connect according to instructions on section 1.1.

## Question#2  Enable *opensmd* service in Ubuntu 18.04

### Error message:

```bash
opensmd.service is not a native service, redirecting to systemd-sysv-install.
Executing: /lib/systemd/systemd-sysv-install enable opensmd
update-rc.d: error: no runlevel symlinks to modify, aborting!
```

### 2.1 Update the service start level in /etc/init.d/opensmd

The original /etc/init.d/opensmd 

```bash
  8 ### BEGIN INIT INFO
  9 # Provides: opensm
 10 # Required-Start: $syslog openibd
 11 # Required-Stop: $syslog openibd
 12 # Default-Start: null
 13 # Default-Stop: 0 1 6
 14 # Description:  Manage OpenSM
 15 ### END INIT INFO
```

Change the content in the line `Default-start` /etc/init.d/opensmd to :

```bash
12 # Default-Start: 2 3 4 5
```

### 2.2 Enable && Start the *opensmd* service

```bash
# Enable and strart the opensmd service
sudo update-rc.d opensmd remove -f
sudo systemctl enable opensmd
sudo systemctl start opensmd

# confirm the service status
sudo systemctl status opensmd

# The log shown as:
opensmd.service - LSB: Manage OpenSM
   Loaded: loaded (/etc/init.d/opensmd; generated)
   Active: active (running) since Mon 2022-05-02 14:53:39 CST; 10s ago
    
```
