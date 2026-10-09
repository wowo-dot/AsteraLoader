/* The plugin header must build as plain C, with the same layout as C++. */
#include "abi_layout.h"

static const char* const after[] = {"other", NULL};

static const cmfm_plugin_info_t info = {
    sizeof(cmfm_plugin_info_t), CMFM_API_VERSION, "sample", NULL, "1.0", 0, 0, after, NULL, NULL, NULL,
};

int main(void) {
    return info.api_version == CMFM_MAKE_VERSION(1, 3) && CMFM_VERSION_MAJOR(info.api_version) == 1 &&
                   CMFM_VERSION_MINOR(info.api_version) == 3
               ? 0
               : 1;
}
