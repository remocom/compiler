CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -g -pthread -Icommon
CXXFLAGS = -Wall -Wextra -g -pthread -std=c++17 -Icommon -Icoordinator

all: coordinatorMake workerMake

coordinatorMake: coordinator/coordinator.c coordinator/task_dispatch.c coordinator/task_dispatch.h coordinator/worker_registry.c coordinator/worker_registry.h coordinator/linker.c coordinator/linker.h coordinator/build_state.c coordinator/build_state.h coordinator/coordinator_types.h coordinator/manifest_loader.cpp coordinator/manifest_loader.h
	$(CC) $(CFLAGS) -c -o coordinator/coordinator.o coordinator/coordinator.c
	$(CC) $(CFLAGS) -c -o coordinator/task_dispatch.o coordinator/task_dispatch.c
	$(CC) $(CFLAGS) -c -o coordinator/worker_registry.o coordinator/worker_registry.c
	$(CC) $(CFLAGS) -c -o coordinator/linker.o coordinator/linker.c
	$(CC) $(CFLAGS) -c -o coordinator/build_state.o coordinator/build_state.c
	$(CC) $(CFLAGS) -c -o common/common.o common/common.c
	$(CXX) $(CXXFLAGS) -c -o coordinator/manifest_loader.o coordinator/manifest_loader.cpp
	$(CXX) -o coordinator_app coordinator/coordinator.o coordinator/task_dispatch.o coordinator/worker_registry.o coordinator/linker.o coordinator/build_state.o common/common.o coordinator/manifest_loader.o -lcjson

workerMake: worker/worker.c
	$(CC) $(CFLAGS) -o worker_app worker/worker.c common/common.c -lcjson

clean:
	rm -f coordinator_app worker_app coordinator/coordinator.o coordinator/task_dispatch.o coordinator/worker_registry.o coordinator/linker.o coordinator/build_state.o coordinator/manifest_loader.o common/common.o
