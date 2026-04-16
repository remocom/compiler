
# Remocom Compiler™
## Authors
- Ryan Davis [contact](mailto:ryan.w.davis@wsu.edu)
- Jaden Bryant [contact](mailto:jaden.bryant@wsu.edu)
- Mitchell Milander [contact](mailto:mitchell.milander@wsu.edu)

## Project summary
A remote compiler that seeks to reduce program compilation time by coordinating with other machines in a distributed system.

### Additional Information
1) A central machine (“the coordinator”)
   - Initiates a build via the use of a build manifest, using it as a guide when determining what machines will be responsible for what
   - Responsible for performing linking and having a set of procedures in place for handling issues like the disconnection of a node during a job, creating system resilience
2) Responsibilities are assigned using calculations from a cost function that takes into account a number of factors, including but not limited to:
   - The amount of time it would take for the node to transfer a file
   - How many jobs are already queued and their expected completion times
   - A reliability metric
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
- Linux Distribution

### Add-ons
- cJSON (`libcjson-dev`): JSON message serialization/parsing between coordinator and workers.
- toml11 (`libtoml11-dev`): system TOML parser used by the coordinator to load build manifests.

### Installation Steps
- `sudo apt install git`
- `sudo apt install gcc`
- `sudo apt install g++`
- `sudo apt install libcjson-dev`
- `sudo apt install libtoml11-dev`
- `git clone https://github.com/remocom/compiler.git`
- `make`
- `cd compiler`
   - `./coordinator_app --manifest build.toml`
  - `./worker_app`

## Functionality
1) Create a TOML build manifest (example: `build.toml`):

```toml
[build]
output = "myprogram"
flags = ["-O2", "-std=c11", "-Icommon"]
sources = ["main.c", "common/common.c"]
```

2) Start the coordinator with the manifest:

```bash
./coordinator_app --manifest build.toml
```

3) Start one or more workers:

```bash
./worker_app
```

4) Workers request compilation tasks from the coordinator, run GCC with the manifest-provided flags, and report task results back to the coordinator.

### 4 Themes Used
- Network Programming 
- Remote Procedure Calls
- Distributed Systems
- Running Programs / Linking

### Design Decisions/Trade-Offs 

## Known Problems / Lessons Learned
TODO: Describe any known issues, bugs, odd behaviors or code smells.
Provide steps to reproduce the problem and/or name a file or a function where the
problem lives.

## Additional Documentation
TODO: Provide links to additional documentation that may exist in the repo, e.g.,
* Sprint reports
* User links

## License
If you haven't already, add a file called `LICENSE.txt` with the text of the
appropriate license.
We recommend using the MIT license: <https://choosealicense.com/licenses/mit/>
