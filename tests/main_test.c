
#include "greatest.h"
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"


// Forward declaration for test suites
void suite_synthetic(void);
void suite_image(void);


// Define the test runner's main() and greatest internals.
GREATEST_MAIN_DEFS();


// ----------------------------------------------------------------
// Main entry point for the test runner.
// ----------------------------------------------------------------

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(suite_synthetic);
    RUN_SUITE(suite_image);
    GREATEST_MAIN_END();
    return EXIT_SUCCESS;
}

