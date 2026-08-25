#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define FILE_PATH "/mnt/client/a.txt"
#define WRITE_SIZE 1 * 1024 * 1024

int main(void)
{
        char buffer[WRITE_SIZE];
        ssize_t written;
        int fd;

        memset(buffer, 'A', sizeof(buffer));

        fd = open(FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
                perror("open");
                return EXIT_FAILURE;
        }

        written = write(fd, buffer, sizeof(buffer));
        if (written < 0) {
                perror("write");
                close(fd);
                return EXIT_FAILURE;
        }

        if (written != WRITE_SIZE) {
                fprintf(stderr, "Short write: %zd bytes\n", written);
                close(fd);
                return EXIT_FAILURE;
        }

        if (close(fd) < 0) {
                perror("close");
                return EXIT_FAILURE;
        }

        printf("Wrote %d bytes to %s\n", WRITE_SIZE, FILE_PATH);
        return EXIT_SUCCESS;
}
