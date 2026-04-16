CC = gcc
CFLAGS = -Wall -Wextra -g -pthread -Icommon

all: coordinatorMake workerMake

coordinatorMake: coordinator/coordinator.c
	$(CC) $(CFLAGS) -o coordinator_app coordinator/coordinator.c common/common.c -lcjson

workerMake: worker/worker.c
	$(CC) $(CFLAGS) -o worker_app worker/worker.c common/common.c -lcjson

clean:
	rm -f coordinator_app worker_app