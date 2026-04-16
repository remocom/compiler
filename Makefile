CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -g -pthread -Icommon
CXXFLAGS = -Wall -Wextra -g -pthread -std=c++17 -Icommon -Icoordinator

all: coordinatorMake workerMake

coordinatorMake: coordinator/coordinator.c coordinator/manifest_loader.cpp coordinator/manifest_loader.h
	$(CC) $(CFLAGS) -c -o coordinator/coordinator.o coordinator/coordinator.c
	$(CC) $(CFLAGS) -c -o common/common.o common/common.c
	$(CXX) $(CXXFLAGS) -c -o coordinator/manifest_loader.o coordinator/manifest_loader.cpp
	$(CXX) -o coordinator_app coordinator/coordinator.o common/common.o coordinator/manifest_loader.o -lcjson

workerMake: worker/worker.c
	$(CC) $(CFLAGS) -o worker_app worker/worker.c common/common.c -lcjson

clean:
	rm -f coordinator_app worker_app coordinator/coordinator.o coordinator/manifest_loader.o common/common.o