#pragma once

#include <initializer_list>
#include <vstl/config.h>
#include <vstl/MetaLib.h>

namespace vstd {

LUISA_DLL void vstl_log(char const *chunk);
LUISA_DLL void vstl_log(char const *const *chunk, size_t chunkCount);
LUISA_DLL void vstl_log(std::initializer_list<char const *> initList);
LUISA_DLL void vstl_log_error_pure_virtual(Type tarType);

}// namespace vstd
