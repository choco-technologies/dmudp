#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmudp.h"

static dmudp_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmudp_create();
}

void dmod_test_teardown(void)
{
    dmudp_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmudp_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmudp_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmudp_is_valid(g_handle));
}

DMOD_TEST_STEP(dmudp_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmudp_destroy(NULL);
}
