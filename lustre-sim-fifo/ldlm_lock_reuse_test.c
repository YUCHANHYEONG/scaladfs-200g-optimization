#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>

#define NUM_THREADS 128
#define NUM_WRITES  10
#define BLOCK_SIZE  4096

#define TEST_DIR "/mnt/client"

static pthread_barrier_t barrier;

static void *writer(void *arg)
{
    long id = (long)arg;
    char path[256];
    char *buf;
    int fd;
    int i;

    snprintf(path, sizeof(path),
             TEST_DIR "/locktest_%03ld", id);

    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        return NULL;
    }

    buf = malloc(BLOCK_SIZE);
    if (!buf) {
        perror("malloc");
        close(fd);
        return NULL;
    }

    memset(buf, 'A' + (id % 26), BLOCK_SIZE);

    /*
     * 모든 thread가 파일 open까지 끝낸 뒤
     * write를 거의 동시에 시작.
     */
    pthread_barrier_wait(&barrier);

    for (i = 0; i < NUM_WRITES; i++) {
        ssize_t ret;

        ret = pwrite(fd, buf, BLOCK_SIZE, 0);

        if (ret != BLOCK_SIZE) {
            fprintf(stderr,
                    "thread %ld write %d failed: ret=%zd errno=%d\n",
                    id, i, ret, errno);
            break;
        }

        printf("thread=%ld write=%d offset=0 size=4096\n",
               id, i + 1);
    }

    close(fd);
    free(buf);

    return NULL;
}

int main(void)
{
    pthread_t threads[NUM_THREADS];
    long i;

    pthread_barrier_init(&barrier, NULL, NUM_THREADS);

    for (i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL,
                           writer, (void *)i) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    pthread_barrier_destroy(&barrier);

    return 0;
}
