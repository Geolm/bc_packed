#include "greatest.h"

#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include "Metal.hpp"

TEST metal_device_create_destroy(void)
{
    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    ASSERT(device != nullptr);
    device->release();
    PASS();
}

extern "C" {

SUITE(suite_gpu)
{
    RUN_TEST(metal_device_create_destroy);
}

}
