// xv6/user/swaploop.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define NUM_PAGES 128    // 2MB
#define TOUCH_STRIDE 128 // 128 bytes 간격으로 건드리기

int
main(int argc, char *argv[])
{
  printf("swaploop: allocating %d pages (~%d KB)\n", NUM_PAGES, NUM_PAGES * PGSIZE / 1024);
  char *buf = sbrk(NUM_PAGES * PGSIZE);
  if (buf == (char*)-1) {
    printf("swaploop: sbrk failed\n");
    exit(1);
  }

  // 1) 연속 쓰기: 각 페이지 첫 바이트에 페이지 번호 기록
  for (int i = 0; i < NUM_PAGES; i++) {
    buf[i * PGSIZE + 0] = i;
  }

  // 2) 스트라이드 읽기: 페이지마다 여러 위치를 건드려서 액세스 비율 조정
  printf("swaploop: strided reads to induce swapping\n");
  for (int pass = 0; pass < 10; pass++) {
    for (int i = 0; i < NUM_PAGES; i++) {
      char *p = buf + i * PGSIZE;
      // 페이지 당 TOUCH_STRIDE 간격으로 몇 번씩 읽기
      for (int off = 0; off < PGSIZE; off += TOUCH_STRIDE) {
        volatile char x = p[off];
        (void)x;
      }
    }
  }

  // 3) 잠시 대기해서 더 많은 페이지 폴트/스왑 트리거
  printf("swaploop: sleeping to let OS swap pages\n");
  sleep(50);

  // 4) 데이터 무결성 확인
  printf("swaploop: verifying data integrity\n");
  for (int i = 0; i < NUM_PAGES; i++) {
    if (buf[i * PGSIZE + 0] != i) {
      printf("swaploop: data corrupt at page %d (got %d)\n", i, buf[i * PGSIZE + 0]);
      exit(1);
    }
  }

  printf("swaploop: all pages verified\n");
  exit(0);
}
