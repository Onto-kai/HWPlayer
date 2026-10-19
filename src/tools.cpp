#include "tools.h"
#include <string>
#include <cstring>
#include <libavutil/error.h>

std::string avError(int ret) {
    std::string ebuff(AV_ERROR_MAX_STRING_SIZE, '\0');

    av_strerror(
        ret,
        &ebuff[0],
        static_cast<size_t>(AV_ERROR_MAX_STRING_SIZE)
    );

    ebuff.resize(std::strlen(ebuff.c_str()));
    return ebuff;
}