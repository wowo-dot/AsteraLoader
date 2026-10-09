/* The plugin header must build as plain C, with the same layout as C++. */
#include "abi_layout.h"

static const char* const after[] = {"other", NULL};

static const al_plugin_info_t info = {
    sizeof(al_plugin_info_t), AL_API_VERSION, "sample", NULL, "1.0", 0, 0, after, NULL, NULL, NULL,
};

int main(void) {
    return info.api_version == AL_MAKE_VERSION(1, 6) && AL_VERSION_MAJOR(info.api_version) == 1 &&
                   AL_VERSION_MINOR(info.api_version) == 6
               ? 0
               : 1;
}
