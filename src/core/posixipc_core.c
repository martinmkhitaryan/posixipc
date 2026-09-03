#include "posixipc_config.h"
#include "posixipc_result.h"

unsigned int posixipc_abi_tag_seed(void);

unsigned int posixipc_abi_tag_seed(void)
{
    return (unsigned int)POSIXIPC_ABI_TAG_SEED;
}
