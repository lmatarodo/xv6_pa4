// xv6/user/forkmmap.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE    4096
#define NPAGES    1024

int
main(void)
{
  char *buf;
  int pid;

  printf("forkmmap: allocating %d pages\n", NPAGES);
  buf = sbrk(NPAGES * PGSIZE);
  if (buf == (char*)-1) {
    printf("forkmmap: sbrk failed\n");
    exit(1);
  }

  // 초기 데이터 쓰기
  for (int i = 0; i < NPAGES; i++) {
    buf[i*PGSIZE] = (char)i;
  }

  // fork
  pid = fork();
  if (pid < 0) {
    printf("forkmmap: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    // 자식 프로세스
    for (int i = 0; i < NPAGES; i++) {
      if (buf[i*PGSIZE] != (char)i) {
        printf("forkmmap child: initial mismatch at %d\n", i);
        exit(1);
      }
      buf[i*PGSIZE] = (char)(i + 100);
    }
    printf("forkmmap child: modified its copy, exiting\n");
    exit(0);
  } else {
    // 부모 프로세스
    if (wait(0) != pid) {
      printf("forkmmap: wait failed\n");
      exit(1);
    }
    // 부모 메모리 값이 변경되지 않았는지 확인
    for (int i = 0; i < NPAGES; i++) {
      if (buf[i*PGSIZE] != (char)i) {
        printf("forkmmap parent: data corrupted at %d: got %d\n",
               i, buf[i*PGSIZE]);
        exit(1);
      }
    }
    printf("forkmmap: PASS\n");
    exit(0);
  }
}
