#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#define STU "123"
#define FILENAME "testfile.txt"
#define BUFSIZE 256
#define STRLEN 11
#define SHM_KEY 0x12345
#define SHM_SIZE sizeof(int)

sig_atomic_t running = 1;

void write_file(const char *buf, size_t len) {
  int fd = open(FILENAME, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    perror("open");
    exit(EXIT_FAILURE);
  }

  ssize_t size = write(fd, buf, len);
  if (size < 0) {
    perror("write");
    exit(EXIT_FAILURE);
  }

  sleep(1); // 测试是否工作

  if (close(fd) < 0) {
    perror("close");
    exit(EXIT_FAILURE);
  }
}

void signal_handler(int signum) { running = 0; }

int main(void) {
  char buf[BUFSIZE] = STU;

  struct sigaction sa;
  sa.sa_handler = signal_handler;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGINT, &sa, NULL) < 0) {
    perror("sigaction");
    exit(EXIT_FAILURE);
  }

  int shmid = shmget(SHM_KEY, SHM_SIZE, 0666 | IPC_CREAT);
  if (shmid < 0) {
    perror("shmget");
    exit(EXIT_FAILURE);
  }

  int *turn = shmat(shmid, NULL, 0);
  if (turn == (int *)(-1)) {
    perror("shmat");
    exit(1);
  }
  *turn = 2; // 进程2先进临界区

  pid_t pid = -1;
  if ((pid = fork())) {
    if (pid == -1) {
      perror("fork");
      exit(EXIT_FAILURE);
    }
    // pid != 0, 父进程

    strcat(buf, " PROC1 MYFILE1\n");

    while (running) {
      while (*turn != 1)
        ;
      printf("进程1在临界区\n");
      write_file(buf, strlen(buf));

      *turn = 2;
    }

    waitpid(pid, NULL, 0);
  } else {
    strcat(buf, " PROC2 MYFILE2\n");

    while (running) {
      while (*turn != 2)
        ;
      printf("进程2在临界区\n");
      write_file(buf, strlen(buf));

      *turn = 1;
    }
  }

  if (shmdt(turn) < 0) {
    perror("shmdt");
    exit(1);
  }

  if (pid != 0) {
    if (shmctl(shmid, IPC_RMID, NULL) < 0) {
      perror("shmctl");
      exit(1);
    }
  }
  return 0;
}
