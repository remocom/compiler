
# Remocom Compiler™
## Authors
- Ryan Davis [contact](mailto:ryan.w.davis@wsu.edu)
- Jaden Bryant [contact](mailto:jaden.bryant@wsu.edu)
- Mitchell Milander [contact](mailto:mitchell.milander@wsu.edu)

## Project summary
A remote compiler that seeks to reduce program compilation time by coordinating with other machines in a distributed system.

### Additional Information
1) A central machine (“the coordinator”)
   - Initiates a build using a TOML build manifest, turns each source file into a compile task, and dispatches tasks to connected workers
   - Receives object files from workers and performs the final link step
   - Reassigns an active task if the worker handling it disconnects or stops sending heartbeats
2) Compile tasks are assigned from a shared queue:
   - Workers request work from the coordinator after registering
   - The coordinator sends the next available task, along with the source and header files needed to compile it
   - If no tasks are available, the worker stops requesting work and continues sending heartbeat messages
3) When joining the compilation network, nodes engage in a handshake with the coordinator to ensure that they’re capable of compiling the file in such a way that it could be integrated within the larger solution. During this handshake process, the compiler confirms:
   - Architecture
   - Compiler version
   - Operating system
   - RPC protocol version
4) The remote compiler has a command-line interface and uses remote procedure calls to conduct jobs such as:
   - File discovery
   - Compilation
   - Heartbeat checks
   - Job scheduling
5) Communications occur via TCP, with nodes taking advantage of it for file transfer and messaging.

## Installation
### Prerequisites
- A Linux distro
- GCC, G++, and Make
- cJSON
- toml11

On Debian/Ubuntu-based systems:

```bash
sudo apt install git make gcc g++ libcjson-dev libtoml11-dev
```

### Installation Steps
```bash
git clone https://github.com/remocom/compiler.git
cd compiler
make
```

This builds two executables in the repository root:

- `coordinator_app`
- `worker_app`

To remove generated binaries and object files:

```bash
make clean
```

## Functionality
1) Create a TOML build manifest (example: `build.toml`):

```toml
[build]
output = "myprogram"
flags = ["-O2", "-std=c11", "-Icommon"]
sources = ["main.c", "common/common.c"]
headers = ["common/common.h"]
```

Required fields:

- `output`: final executable name produced by the coordinator's link step.
- `sources`: source files to compile remotely.

Optional fields:

- `flags`: compiler and linker flags passed to each compile task and the final link command.
- `headers`: additional project headers to transfer to workers. The coordinator also scans dependencies with `gcc -MM` or `g++ -MM` and transfers discovered headers.

2) Start the coordinator with the manifest:

```bash
./coordinator_app --manifest build.toml
```

By default, the coordinator listens only on `127.0.0.1` for local workers. To accept workers from other machines on your network, start it with:

```bash
./coordinator_app --manifest build.toml --allow-external-workers
```

The coordinator accepts the same manifest with `-m`:

```bash
./coordinator_app -m build.toml
```

By default, intermediate object files are removed after a successful link. To keep them:

```bash
./coordinator_app --manifest build.toml --keep-objects
```

3) Start one or more workers on the same machine:

```bash
./worker_app
```

To join from another machine, point the worker at the coordinator's LAN IP or hostname:

```bash
./worker_app --coordinator 192.168.1.25
```

4) Workers request compilation tasks from the coordinator, receive the needed source/header files, run `gcc` or `g++` based on the source extension, and send object files plus compiler output back to the coordinator.

5) When all objects are ready, the coordinator links the final executable. The linker uses `g++` if any manifest source is a C++ file; otherwise it uses `gcc`.

### Runtime Notes
- Coordinator and worker communication uses TCP port `5000`.
- The coordinator writes `coordinator.log` in the manifest directory.
- The link step writes `linker.log` in the manifest directory.
- The coordinator changes into the manifest's directory before dispatching tasks, so relative manifest paths are resolved from the manifest location.
- Workers create temporary task directories under `/tmp` and remove them after each compile task.
- Workers send heartbeats every 5 seconds after completing assigned tasks.

### Themes Used
- Network Programming
  - Project allows multiple devices to communicate over a network to accomplish the shared goal of compiling a program.
  - Coordinator and worker nodes communicate over TCP sockets.

- Remote Procedure Calls
  -  When the Coordinator receives a compile job, it forwards a JSON-protocol message to an available worker node that performs the compilation and returns the result.

- Distributed Systems
  -  Fault Tolerance - The coordinator detects worker failure two ways: (1) when a worker stops sending heartbeats and (2) when a node socket is closed off. In both cases, the coordinator recognizes the worker as dead and cleans up appropriately, being sure to reassign the task to another worker node.
  -  Concurrent Programming - When a worker connects, the coordinator’s main accept() loop hands the connection off to a dedicated handler thread, so the main loop can immediately go back to accepting new workers. This lets workers join at any time without being blocked by other workers being served at the moment.
  -  The coordinator utilizes multithreading to handle incoming responses from Worker nodes.
  -  Race conditions are handled using mutex locks.
- Control Processes
  -  For each worker node, fork() is called to create a child process that runs gcc using execvp(). Once the child process exit status is returned using waitpid(), the status code is analyzed to determine if compilation was successful.
-  System I/O
   - A pipe is created before forking, and the child uses dup2() to redirect STDOUT_FILENO and STDERR_FILENO into the pipe so the parent can capture all of gcc's output. It's worth noting that the parent drains the pipe before calling waitpid() to avoid a deadlock if gcc produces more output than the pipe buffer can hold.
- Running Programs / Linking
  -  The worker nodes compile the source code and return the object files to the client for the client to run.

### Design Decisions/Trade-Offs 

### Multi-threaded worker handling
We decided to give each worker its own helper thread so the coordinator can handle multiple workers at the same time. This makes the system more realistic because workers can connect, send heartbeats, and request tasks independently.

**Trade-off:** This adds more complexity because multiple threads may access shared data, such as the worker list or log file, at the same time. Because of this, we need to be careful with synchronization and mutexes.

### Centralized coordinator model
We used one coordinator to manage worker connections, assign tasks, and track worker status. This made the system easier to understand because the coordinator acts as the “brain” of the system.

**Trade-off:** The coordinator can become a bottleneck if too many workers connect at once. It is also a single point of failure because if the coordinator crashes, the whole system stops working.

### JSON message format
We used JSON messages for communication between the coordinator and workers. This makes messages easier to read, debug, and extend later.

**Trade-off:** JSON is easier to work with, but it has more overhead than a smaller binary protocol. It also requires parsing, which can add extra code and possible parsing errors.

### Heartbeat monitoring
We added heartbeat messages so the coordinator can check if workers are still alive. This helps detect workers that disconnect, crash, or stop responding.

**Trade-off:** Heartbeat monitoring depends on timing thresholds. In a real network, delays or slow machines could make a worker look dead even if it is still running.

### Manifest-based task system
We used a manifest-based approach so the coordinator can read build information and decide what tasks need to be assigned to workers.

**Trade-off:** The coordinator has to parse through the manifest correctly. If the manifest is formatted wrong or missing information, task assignment can fail or become harder to debug.

## Known Problems / Lessons Learned

   One known issue in the current version of RemoCom is that the coordinator depends on simple timing thresholds for heartbeat monitoring. If a worker does not send a heartbeat within the expected time, the coordinator may mark it as dead. This works for testing, but in a real network, delays from network traffic, OS scheduling, or slow machines could cause a worker to be marked as dead even if it is still running.

**Where it happens:** heartbeat checking logic in the coordinator.

**How to reproduce:** 
1. Start the coordinator.
2. Start one or more worker nodes.
3. Stop or delay a worker from sending heartbeat messages.
4. After the timeout threshold passes, the coordinator logs the worker as timed out.

Another limitation is that the coordinator is centralized. This makes the system easier to design and understand because one machine handles scheduling and worker communication, but it also means the coordinator could become a bottleneck or single point of failure as more workers are added.

A lesson learned from this project is that distributed systems require more than just sending messages between machines. We also had to think about worker state, timing, task assignment, logging, and how to safely handle multiple workers at the same time. Adding threads helped workers run independently, but it also added more complexity because shared data must be handled carefully.

## Additional Documentation
Additional project documentation can be found in the `docs/` folder:

- `docs/LICENSE.txt` → license statement
- `docs/ProjectProposalTemplate (1).pdf` → contains the original project proposal, including goals, design ideas, and planned system architecture
- `docs/Testing` → contains screenshots of testing throughout the development process
