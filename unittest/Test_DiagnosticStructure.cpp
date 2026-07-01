// Test_DiagnosticStructure.cpp â€?Phase 4-N structured diagnostics

#include "AYCompilerError.h"
#include "AYTest.h"

using namespace ayt::shader::phoskia;

TEST_SUITE(DiagnosticStructureTests)

TEST_CASE(phoskia_diagnostic_human_format_includes_location)
{
    PhoskiaDiagnostic diag;
    diag.severity = DiagnosticSeverity::Error;
    diag.message = "unexpected token";
    diag.location.file = "shader.phoskia";
    diag.location.line = 12;
    diag.location.column = 4;
    diag.hint = "did you mean ';'?";

    const std::string text = diag.toHumanString();
    CHECK(text.find("shader.phoskia:12:4") != std::string::npos);
    CHECK(text.find("unexpected token") != std::string::npos);
    CHECK(text.find("did you mean") != std::string::npos);
}

TEST_SUITE_END
