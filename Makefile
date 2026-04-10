CC = gcc
CFLAGS = -Wall -Wextra -g

all: coordinator worker

coordinator: coordinator/coordinator.c
	$(CC) $(CFLAGS) -o coordinator_app coordinator/coordinator.c

worker: worker/worker.c
	$(CC) $(CFLAGS) -o worker_app worker/worker.c

clean:
	rm -f coordinator_app worker_app