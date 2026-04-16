#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <sys/wait.h>

#define SHM_NAME "/no_cond_shm"
#define BUFFER_SIZE 8
#define NUM_CONSUMERS 3
#define ITEMS 20

typedef struct {
    int data[BUFFER_SIZE];

    int write_idx;
    int read_idx[NUM_CONSUMERS];

    pthread_mutex_t mutex;
} shm_struct;

void init_mutex(shm_struct *shm) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);

    pthread_mutex_init(&shm->mutex, &attr);

    pthread_mutexattr_destroy(&attr);
}

void producer(shm_struct *shm) {
    for (int i = 0; i < ITEMS; i++) {

        int written = 0;

        while (!written) {
            pthread_mutex_lock(&shm->mutex);

            int next = (shm->write_idx + 1) % BUFFER_SIZE;

            // check if buffer slot is safe (not overwritten)
            int can_write = 1;
            for (int c = 0; c < NUM_CONSUMERS; c++) {
                if (next == shm->read_idx[c]) {
                    can_write = 0;
                    break;
                }
            }

            if (can_write) {
                shm->data[shm->write_idx] = i;
                printf("[PROD] wrote %d at %d\n", i, shm->write_idx);

                shm->write_idx = next;
                written = 1;
            }

            pthread_mutex_unlock(&shm->mutex);

            if (!written)
                usleep(1000); // avoid 100% CPU spin
        }

        usleep(200000);
    }

    printf("[PROD] done\n");
}

void consumer(shm_struct *shm, int id) {
    int local = shm->read_idx[id];

    while (1) {

        int consumed = 0;

        pthread_mutex_lock(&shm->mutex);

        if (local != shm->write_idx) {
            int value = shm->data[local];

            printf("    [C%d] got %d from %d\n", id, value, local);

            local = (local + 1) % BUFFER_SIZE;
            shm->read_idx[id] = local;

            consumed = value;
        }

        pthread_mutex_unlock(&shm->mutex);

        if (!consumed) {
            usleep(1000); // polling delay
            continue;
        }

        if (consumed == ITEMS - 1)
            break;
    }

    printf("    [C%d] done\n", id);
}

int main() {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(fd, sizeof(shm_struct));

    shm_struct *shm = mmap(NULL, sizeof(shm_struct),
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);

    memset(shm, 0, sizeof(shm_struct));
    init_mutex(shm);

    for (int i = 0; i < NUM_CONSUMERS; i++)
        shm->read_idx[i] = 0;

    pid_t pids[NUM_CONSUMERS];

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        pid_t pid = fork();

        if (pid == 0) {
            consumer(shm, i);
            exit(0);
        }

        pids[i] = pid;
    }

    producer(shm);

    for (int i = 0; i < NUM_CONSUMERS; i++)
        waitpid(pids[i], NULL, 0);

    munmap(shm, sizeof(shm_struct));
    close(fd);
    shm_unlink(SHM_NAME);

    return 0;
}