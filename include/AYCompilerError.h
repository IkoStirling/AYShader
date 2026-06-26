#pragma once
// AYCompilerError.h - Compiler error definitions for Phoskia

#include <string>
#include <vector>

namespace ayt::shader::phoskia
{

enum class ErrorCode : uint8_t {
    UnexpectedToken,
    InvalidExpression,
    InvalidStatement,
    UnterminatedString,
    InvalidNumber,
    UnexpectedEndOfFile,
    TypeMismatch,
    UnknownIdentifier,
    InvalidOperation
};

struct CompilerError {
    ErrorCode code = ErrorCode::UnexpectedToken;
    std::string message;
    int line = 0;
    int column = 0;

    CompilerError() = default;
    CompilerError(ErrorCode c, std::string m, int l, int col)
        : code(c), message(std::move(m)), line(l), column(col) {}

    std::string toString() const;
};

class CompilerErrorReporter {
public:
    void error(ErrorCode code, const std::string& message, int line, int column);
    bool hasErrors() const { return !_errors.empty(); }
    const std::vector<CompilerError>& errors() const { return _errors; }
    void clear() { _errors.clear(); }

private:
    std::vector<CompilerError> _errors;
};

} // namespace ayt::shader::phoskia
