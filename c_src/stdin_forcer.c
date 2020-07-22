#include <sys/wait.h>
#include <sys/types.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define PARENT_READ readpipe[0]
#define CHILD_WRITE readpipe[1]
#define CHILD_READ writepipe[0]
#define PARENT_WRITE writepipe[1]
#define PARENT_ERROR errorpipe[0]
#define CHILD_ERROR errorpipe[1]

/* Status bytes */
enum {
    /* ASCII & UTF-8 control character: 145 | 0x91 | PU1 | for private use. */
    SUCCESS_BYTE = 0x91,
    /* ASCII & UTF-8 control character: 146 | 0x92 | PU2 | for private use. */
    ERROR_BYTE = 0x92,
};

int dup2close(int oldfd, int newfd) {
    int dupResult;
    do {
        dupResult = dup2(oldfd, newfd);
    } while ((dupResult == -1) && (errno == EINTR));

    return close(oldfd);
}

void toSTDOUT(FILE* fin, const char firstByte) {
    char buf[BUFSIZ];
    ssize_t count;

    do {
        count = fread(buf, 1, BUFSIZ, fin);
    } while (count == -1 && errno == EINTR);

    if (count == -1) {
        perror("read");
        exit(1);
    } else if (count > 0) {
        if (firstByte)
            fwrite(&firstByte, 1, 1, stdout); /* Write first byte */

        do {
            fwrite(buf, 1, count, stdout); /* write buffer to STDOUT */
            count = fread(buf, 1, BUFSIZ, fin);
        } while (count > 0);
    }
    fclose(fin);
}

int main(int argc, char *argv[]) {
    int readpipe[2], writepipe[2], errorpipe[2];
    int unused __attribute__((unused));
    pid_t cpid;

    assert(1 < argc && argc < 64);

    if (pipe(readpipe) == -1 || pipe(writepipe) == -1 ||
        pipe(errorpipe) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    cpid = fork();
    if (cpid == -1) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (cpid == 0) {
        /* Forked Child with STDIN forwarding */
        char *cmd = argv[1];
        char *exec_args[64] = {0};
        int i;

        for (i = 0; i < argc - 1; i++) {
            /* args to stdin_forcer are the program and optional args to exec.
               Here we copy pointers pointing to strings of cmd/args.
               exec_args is indexed one lower than argv. */
            exec_args[i] = argv[i + 1];
        }

        close(PARENT_READ); /* We aren't the parent. Decrement fd refcounts. */
        close(PARENT_WRITE);
        close(PARENT_ERROR);

        /* CHILD_READ  = STDIN  to the exec'd process.
           CHILD_WRITE = STDOUT to the exec'd process.
           CHILD_ERROR = STDERR to the exec'd process. */
        if (dup2close(CHILD_READ, STDIN_FILENO) ||
            dup2close(CHILD_WRITE, STDOUT_FILENO) ||
            dup2close(CHILD_ERROR, STDERR_FILENO)) {
            perror("dup2 or close");
            _exit(EXIT_FAILURE);
        }

        /* At this point, the execv'd program's STDIN and STDOUT are the pipe */
        if (execv(cmd, exec_args) == -1) {
            perror("execve");
        }
        _exit(EXIT_FAILURE); /* Silence a warning */
    } else {
        /* Original Parent Process */
        char buf[4];
        uint32_t len = 0;

        close(CHILD_READ); /* We aren't the child.  Close its read/write. */
        close(CHILD_WRITE);
        close(CHILD_ERROR);

        FILE* write_fd = fdopen(PARENT_WRITE, "wb");

        /* Read length prefix info */
        if (fread(buf, 1, 1, stdin)<1)
          goto NOINPUT;
        switch (buf[0]) {
        case 0:
          goto NOINPUT;
        case 1:
          if (fread(buf, 1, 1, stdin)<1)
            exit(EXIT_FAILURE);
          len = (uint32_t)buf[0];
          break;
        case 4:
          for (int i=0; i<4; i++)
            if (fread(&buf[i], 1, 1, stdin)<1)
              goto NOINPUT;
          /*
          while(4-len)
            len+=read(STDIN_FILENO, &buf[len], 4-len);
          */
          len = (uint32_t)buf[0] << 24 |
            (uint32_t)buf[1] << 16 |
            (uint32_t)buf[2] << 8  |
            (uint32_t)buf[3];
          break;
        default:
          unused = fwrite(buf, 1, 1, write_fd);
          break;
        }
        /* Read until len or 0 byte */
        while (fread(buf, 1, 1, stdin) > 0 && (len!=0 || buf[0] != 0x0)) {
          while(fwrite(buf, 1, 1, write_fd)==0)
            usleep(100);
          if (len!=0 && --len==0)
            break;
        }

    NOINPUT: // When not input, continue here
        fclose(write_fd); /* closing PARENT_WRITE sends EOF to CHILD_READ */

        toSTDOUT(fdopen(PARENT_READ, "r"), (uint8_t)SUCCESS_BYTE);
        toSTDOUT(fdopen(PARENT_ERROR, "r"), (uint8_t)ERROR_BYTE);

        //close(PARENT_READ);   /* done reading from writepipe */
        //close(PARENT_ERROR);  /* done reading from errorpipe */
        //close(STDOUT_FILENO); /* done writing to stdout */
        fclose(stdin);
        fclose(stdout);

        wait(NULL); /* Wait for child to exit */

        exit(EXIT_SUCCESS); /* This was a triumph */
    }
}
