
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
TODO: List which add-ons are included in the project, and the purpose each add-on
serves in your app.

### Installation Steps
- sudo apt install git
- sudo apt install gcc
- sudo apt install libcjson-dev
- git clone https://github.com/remocom/compiler.git
- make
- cd compiler
  - ./coordinator_app
  - ./worker_app

## Functionality
TODO: Write usage instructions. Structuring it as a walkthrough can help structure
this section, and showcase your features.

## Known Problems
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
