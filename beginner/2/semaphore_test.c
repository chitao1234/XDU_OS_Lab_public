#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <unistd.h>

#define STU "123"
#define FILENAME "testfile.txt"
#define BUFSIZE 256
#define STRLEN 11
#define SEM_KEY 0x34567

volatile sig_atomic_t running = 1;

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

void P(int semid) {
  struct sembuf op = {0, -1, 0};
  if (semop(semid, &op, 1) < 0) {
    perror("semop in P");
    exit(EXIT_FAILURE);
  }
}

void V(int semid) {
  struct sembuf op = {0, 1, 0};
  if (semop(semid, &op, 1) < 0) {
    perror("semop in V");
    exit(EXIT_FAILURE);
  }
}

union semun {
  int val;
  struct semid_ds *buf;
  unsigned short *array;
  struct seminfo *__buf;
};

int create_sem(key_t key, int init_val) {
  int semid;
  union semun arg;

  semid = semget(key, 1, IPC_CREAT | 0666);
  if (semid == -1) {
    perror("semget");
    return -1;
  }

  arg.val = init_val;
  if (semctl(semid, 0, SETVAL, arg) == -1) {
    perror("semctl");
    return -1;
  }

  return semid;
}

void remove_sem(int semid) {
  if (semctl(semid, 0, IPC_RMID) == -1) {
    perror("semctl in remove_sem");
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

  int sem = create_sem(SEM_KEY, 1);
  if (sem < 0) {
    exit(EXIT_FAILURE);
  }

  pid_t pid = -1;
  if ((pid = fork())) {
    if (pid == -1) {
      perror("fork");
      exit(EXIT_FAILURE);
    }
    // pid != 0, 父进程

    strcat(buf, " PROC1 MYFILE1\n");

    while (running) {
      P(sem);
      printf("进程1在临界区\n");
      write_file(buf, strlen(buf));
      V(sem);
    }

    waitpid(pid, NULL, 0);
    remove_sem(sem);
  } else {
    strcat(buf, " PROC2 MYFILE2\n");

    while (running) {
      P(sem);
      printf("进程2在临界区\n");
      write_file(buf, strlen(buf));
      V(sem);
    }
  }
  return 0;
}
