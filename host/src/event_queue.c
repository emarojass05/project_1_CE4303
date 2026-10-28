#include "event_queue.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Queue: sem_wait that retries after a signal */
static bool WaitSemaphore(sem_t *semaphore)
{
    while (sem_wait(semaphore) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

/* Queue: starts the three shared semaphores */
static bool InitSemaphores(EventQueue *queue)
{
    if (sem_init(&queue->mutex, 1, 1) != 0) {
        return false;
    }
    if (sem_init(&queue->freeSlots, 1, EventQueueCapacity) != 0) {
        sem_destroy(&queue->mutex);
        return false;
    }
    if (sem_init(&queue->usedSlots, 1, 0) != 0) {
        sem_destroy(&queue->mutex);
        sem_destroy(&queue->freeSlots);
        return false;
    }
    return true;
}

/* Queue: maps the shared memory */
static EventQueue *MapQueue(int fd)
{
    void *memory = mmap(NULL, sizeof(EventQueue), PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);

    if (memory == MAP_FAILED) {
        return NULL;
    }
    return (EventQueue *)memory;
}

/* Queue: creates the shared memory and semaphores */
EventQueue *EventQueueCreate(const char *name)
{
    EventQueue *queue;
    int fd;

    if (name == NULL) {
        return NULL;
    }

    /* A leftover from a crashed run would block O_EXCL */
    (void)shm_unlink(name);
    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        return NULL;
    }
    if (ftruncate(fd, (off_t)sizeof(EventQueue)) != 0) {
        close(fd);
        shm_unlink(name);
        return NULL;
    }
    queue = MapQueue(fd);
    close(fd);
    if (queue == NULL) {
        shm_unlink(name);
        return NULL;
    }

    queue->head = 0;
    queue->tail = 0;
    queue->closed = 0;
    if (!InitSemaphores(queue)) {
        munmap(queue, sizeof(EventQueue));
        shm_unlink(name);
        return NULL;
    }
    return queue;
}

/* Queue: maps an existing shared queue */
EventQueue *EventQueueOpen(const char *name)
{
    EventQueue *queue;
    struct stat info;
    int fd;

    if (name == NULL) {
        return NULL;
    }
    fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        return NULL;
    }
    if (fstat(fd, &info) != 0 || (size_t)info.st_size != sizeof(EventQueue)) {
        close(fd);
        return NULL;
    }
    queue = MapQueue(fd);
    close(fd);
    return queue;
}

/* Queue: unmaps it in this process */
void EventQueueClose(EventQueue *queue)
{
    if (queue != NULL) {
        munmap(queue, sizeof(EventQueue));
    }
}

/* Queue: frees semaphores and removes shared memory */
void EventQueueDestroy(EventQueue *queue, const char *name)
{
    if (queue != NULL) {
        sem_destroy(&queue->mutex);
        sem_destroy(&queue->freeSlots);
        sem_destroy(&queue->usedSlots);
        munmap(queue, sizeof(EventQueue));
    }
    if (name != NULL) {
        shm_unlink(name);
    }
}

/* Queue: waits for room, then appends an event */
bool EventQueuePush(EventQueue *queue, const HostEvent *event)
{
    if (queue == NULL || event == NULL) {
        return false;
    }
    if (!WaitSemaphore(&queue->freeSlots)) {
        return false;
    }
    if (!WaitSemaphore(&queue->mutex)) {
        sem_post(&queue->freeSlots);
        return false;
    }

    if (queue->closed) {
        /* Pass the wake-up on to the next waiting producer */
        sem_post(&queue->mutex);
        sem_post(&queue->freeSlots);
        return false;
    }
    queue->events[queue->head % EventQueueCapacity] = *event;
    queue->head++;

    sem_post(&queue->mutex);
    sem_post(&queue->usedSlots);
    return true;
}

/* Queue: waits for an event, then removes the oldest */
bool EventQueuePop(EventQueue *queue, HostEvent *outEvent)
{
    if (queue == NULL || outEvent == NULL) {
        return false;
    }
    if (!WaitSemaphore(&queue->usedSlots)) {
        return false;
    }
    if (!WaitSemaphore(&queue->mutex)) {
        sem_post(&queue->usedSlots);
        return false;
    }

    if (queue->head == queue->tail) {
        /* Only a shutdown wakes us with nothing to read */
        sem_post(&queue->mutex);
        sem_post(&queue->usedSlots);
        return false;
    }
    *outEvent = queue->events[queue->tail % EventQueueCapacity];
    queue->tail++;

    sem_post(&queue->mutex);
    sem_post(&queue->freeSlots);
    return true;
}

/* Queue: closes it and wakes every sleeper */
void EventQueueShutdown(EventQueue *queue)
{
    if (queue == NULL) {
        return;
    }
    if (!WaitSemaphore(&queue->mutex)) {
        return;
    }
    queue->closed = 1;
    sem_post(&queue->mutex);

    /* One extra token for each side wakes the first sleeper, who wakes the next */
    sem_post(&queue->usedSlots);
    sem_post(&queue->freeSlots);
}

/* Queue: number of stored events */
size_t EventQueueCount(EventQueue *queue)
{
    size_t count;

    if (queue == NULL || !WaitSemaphore(&queue->mutex)) {
        return 0;
    }
    count = (size_t)(queue->head - queue->tail);
    sem_post(&queue->mutex);
    return count;
}

/* Queue: monotonic clock in microseconds */
uint64_t HostClockUs(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}
