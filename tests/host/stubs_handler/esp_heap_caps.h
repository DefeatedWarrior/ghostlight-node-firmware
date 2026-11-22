/* Host stub */
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM (1 << 10)
#define heap_caps_calloc(n, size, caps) calloc(n, size)
