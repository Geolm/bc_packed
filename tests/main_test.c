
#include "greatest.h"
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"


// Forward declaration for test suites
void suite_synthetic(void);
#ifdef HAS_IMAGE_TESTS
void suite_image(void);
#endif
void suite_benchmark(void);

#ifdef HAS_GPU_TESTS
void suite_gpu(void);
#endif


// Define the test runner's main() and greatest internals.
GREATEST_MAIN_DEFS();


// ----------------------------------------------------------------
// Main entry point for the test runner.
// ----------------------------------------------------------------

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(suite_synthetic);
#ifdef HAS_IMAGE_TESTS
    RUN_SUITE(suite_image);
#endif
    RUN_SUITE(suite_benchmark);
#ifdef HAS_GPU_TESTS
    RUN_SUITE(suite_gpu);
#endif
    GREATEST_MAIN_END();
    return EXIT_SUCCESS;
}

