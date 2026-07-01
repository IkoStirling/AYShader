#pragma once
// detail/AYShaderDiskCache.h — CompiledShaderProgram disk tier (Phase 4-I)

#include "AYShaderProgram.h"

#include <string>

namespace ayt::shader::detail
{

std::string diskCacheFilePath(const std::string& cacheDirectory,
                              const std::string& digestHex);

bool loadCompiledProgramFromDisk(const std::string& path,
                                 CompiledShaderProgram& out);
bool saveCompiledProgramToDisk(const std::string& path,
                               const CompiledShaderProgram& prog);

} // namespace ayt::shader::detail
