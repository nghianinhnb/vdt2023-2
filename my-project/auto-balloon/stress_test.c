#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MEMORY_TO_CONSUME 100 * 1024 * 1024

void consume_ram() {
    // Allocate memory
    char *memory = (char *)malloc(MEMORY_TO_CONSUME);

    if (memory != NULL) {
        // Fill the allocated memory with data
        memset(memory, 1, MEMORY_TO_CONSUME);

        printf("Consuming %dMB RAM...\n", MEMORY_TO_CONSUME >> 20);
        sleep(20);

        // Free the allocated memory before exiting
        free(memory);
    } else {
        printf("Memory allocation failed. Exiting...\n");
    }
}

int main() {
    consume_ram();
    printf("Process completed!\n");
    return 0;
}