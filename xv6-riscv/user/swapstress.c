// xv6/user/swapstress.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define NUM_PAGES 256  // 4MB (1024 * 4KB)

int
main(int argc, char *argv[])
{
  printf("Starting swap stress test...\n");
  char *pages[NUM_PAGES];
  int i;

  // Allocate pages
  for(i = 0; i < NUM_PAGES; i++) {
    printf("Attempting to allocate page %d...\n", i);
    if((pages[i] = sbrk(PGSIZE)) == (char*)-1) {
      printf("sbrk failed at page %d\n", i);
      exit(1);
    }
    printf("Successfully allocated page %d at %p\n", i, pages[i]);

    // Write data to page
    printf("Writing data to page %d...\n", i);
    for(int j = 0; j < PGSIZE; j += 1024) {
      printf("  Writing byte %d/%d to page %d\n", j, PGSIZE, i);
      pages[i][j] = i;  // Write page number to first byte of each 1KB block
    }
    printf("Finished writing data to page %d\n", i);
  }

  // Verify data
  printf("Verifying data...\n");
  for(i = 0; i < NUM_PAGES; i++) {
    printf("Verifying page %d...\n", i);
    for(int j = 0; j < PGSIZE; j += 1024) {
      if(pages[i][j] != i) {
        printf("Data verification failed at page %d, offset %d\n", i, j);
        exit(1);
      }
    }
  }
  printf("All data verified successfully\n");

  // Sleep to induce swapping
  printf("Sleeping to induce swapping...\n");
  sleep(100);
  printf("Woke up from sleep\n");

  // Test page access
  printf("Testing page access...\n");
  for(i = 0; i < NUM_PAGES; i++) {
    printf("Accessing page %d...\n", i);
    if(pages[i][0] != i) {
      printf("Page %d data corrupted\n", i);
      exit(1);
    }
  }
  printf("All pages accessed successfully\n");

  exit(0);
}
