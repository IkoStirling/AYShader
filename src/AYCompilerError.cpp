// AYCompilerError.cpp - Compiler error implementation

#include "AYShader/CompilerError.h"
#include <sstream>

namespace ayt::shader::phoskia
{

std::string PhoskiaDiagnostic::toHumanString() const
{
    std::ostringstream oss;
    if (!location.file.empty()) {
        oss << location.file << ':';
    }
    if (location.line > 0) {
        oss << location.line << ':' << location.column << ": ";
    }
    oss << message;
    if (!hint.empty()) {
        oss << " (" << hint << ')';
    }
    return oss.str();
}

std::string CompilerError::toString() const {
    std::ostringstream oss;
    oss << "Error [" << static_cast<int>(code) << "] at line " << line
        << ", column " << column << ": " << message;
    return oss.str();
}

void CompilerErrorReporter::error(ErrorCode code, const std::string& message, int line, int column) {
    CompilerError err;
    err.code = code;
    err.message = message;
    err.line = line;
    err.column = column;
    _errors.push_back(err);
}

} // namespace ayt::shader::phoskia
