#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

sem_t sem;
void *worker(void *arg) {
    for (int i = 0; i < 5; i++) {
        sem_wait(&sem);
        printf("Worker %ld is working...\n", pthread_self());
        sleep(1); // simulate work
        sem_post(&sem);
        sleep(1); // allow other worker to run
    }
    return NULL;
}

int main(int argc, char **argv) {

//basic mmap
    const char *file = "test.txt";

    int fd = open(file, O_RDWR | O_CREAT, 0666);
    ftruncate(fd, 4096); // ensure file size

    char *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    strcpy(map, "Hello via mmap!");

    printf("File contains: %s\n", map);

    munmap(map, 4096);
    close(fd);


//basic shared memory
    const char *name = "/my_shm";

    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    ftruncate(fd, 4096);

    char *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    sprintf(ptr, "Shared memory says hello!");

    printf("%s\n", ptr);

    munmap(ptr, 4096);
    close(fd);

    // optional cleanup:
    // shm_unlink(name);

}



//basic semaphore
typedef struct {
    int data;
    sem_t sem;   // semaphore inside shared memory
} shm_t;

int main() {
    // 1. Create shared memory
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(fd, sizeof(shm_t));

    shm_t *shm = mmap(NULL, sizeof(shm_t),
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);

    // 2. Initialize semaphore (shared between processes)
    sem_init(&shm->sem, 1, 0); // pshared=1, initial value=0

    pid_t pid = fork();

    if (pid == 0) {
        // ================= CHILD (consumer) =================
        printf("[Child] Waiting for data...\n");

        sem_wait(&shm->sem);   // wait until parent signals

        printf("[Child] Got data: %d\n", shm->data);

        exit(0);
    }

    // ================= PARENT (producer) =================
    sleep(1); // just to show ordering

    shm->data = 42;
    printf("[Parent] Produced data: %d\n", shm->data);

    sem_post(&shm->sem);  // notify child

    wait(NULL);

    // Cleanup
    sem_destroy(&shm->sem);
    munmap(shm, sizeof(shm_t));
    close(fd);
    shm_unlink(SHM_NAME);

    return 0;
}