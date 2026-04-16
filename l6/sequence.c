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

main(){
    create_shared_memory_and_truncate();
    map_shared_memory();
    initialize_shared_data_structure();//mutex inside
    initialize_synchronization_primitives();
    loop(N times){
        fork();
        if child{
            loop(depending on task){
                wait_for_synchronization_permits();
                lock();
                child_task();
                unlock();
            }
            exit();
        }
        
    }
    loop(depending on task){
        lock();
        parent_task();
        unlock();
    }
    wait_for_children();
    cleanup();
}


// semaphore example
Producer: write data
Producer: sem_post(items)

Consumer: sem_wait(items) → wakes up
Consumer: read data