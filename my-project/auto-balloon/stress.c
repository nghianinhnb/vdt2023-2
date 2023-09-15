#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

void consumeMemory() {
    int mbToAllocate = 32;  // Số MB cần cấp phát trong mỗi lần lặp
    int totalAllocated = 0;
    void *memoryBlocks[1400/32 + 1];

    int blockIndex = 0;

    while (totalAllocated < 1400) {
        void *ptr = malloc(mbToAllocate * 1024 * 1024);
        if (ptr == NULL) {
            sleep(1);
            continue;
        }
        memset(ptr, 1, mbToAllocate * 1024 * 1024);
        memoryBlocks[blockIndex++] = ptr;
        totalAllocated += mbToAllocate;
        fprintf(stdout, "Allocated %dMB\n", totalAllocated);
        sleep(1);
    }

    for (int i = 0; i < blockIndex; i++) {
        free(memoryBlocks[i]);
    }
}

int main() {
    consumeMemory();
    return 0;
}