
manager_program: manager/server.c manager/job_queue.c manager/scheduler.c manager/worker_registry.c common/protocol.c common/protocol.h
	gcc -pthread -o manager_program manager/server.c manager/job_queue.c manager/scheduler.c manager/worker_registry.c common/protocol.c

client_program: client/client.c common/protocol.c common/protocol.h
	gcc -o client_program client/client.c common/protocol.c

worker_program: worker/worker.c manager/scheduler.c common/protocol.c common/protocol.h
	gcc -pthread -o worker_program worker/worker.c manager/scheduler.c common/protocol.c

all: manager_program client_program worker_program

clean:
	rm -f manager_program client_program worker_program
