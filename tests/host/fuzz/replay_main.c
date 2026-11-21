/* main() for the fuzz harnesses without libFuzzer: runs each file named on
 * the command line through LLVMFuzzerTestOneInput() once. `make test` uses
 * it to replay the seed corpus under ASan + UBSan with any C compiler. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv)
{
    int n = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            perror(argv[i]);
            return 2;
        }
        uint8_t *buf = malloc(1 << 16);
        size_t len = fread(buf, 1, 1 << 16, f);
        fclose(f);
        LLVMFuzzerTestOneInput(buf, len);
        free(buf);
        n++;
    }
    printf("replayed %d inputs\n", n);
    return 0;
}
