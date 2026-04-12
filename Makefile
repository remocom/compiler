CC = gcc
CFLAGS = -Wall -Wextra -g -pthread

all: coordinatorMake workerMake

coordinatorMake: coordinator/coordinator.c
	$(CC) $(CFLAGS) -o coordinator_app coordinator/coordinator.c -lcjson

workerMake: worker/worker.c
	$(CC) $(CFLAGS) -o worker_app worker/worker.c -lcjson

clean:
	rm -f coordinator_app worker_app