#include "program_compiler.h"

#include <QDirIterator>

#include <exception>

#include <compiler/diagnostics.h>
#include <compiler/dsl_compiler.h>

namespace noisefactor::sync::render_helper {

namespace {

[[nodiscard]] auto count_definitions(const QString& data_root) -> int {
  int count = 0;
  QDirIterator it(data_root + QStringLiteral("/effects"), {QStringLiteral("*.json")},
                  QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    ++count;
  }
  return count;
}

}  // namespace

ProgramCompiler::ProgramCompiler(const QString& data_root)
    : definitions_(count_definitions(data_root)) {
  registry_.loadAll(data_root);
}

auto ProgramCompiler::effect_count() const -> int { return definitions_; }

auto ProgramCompiler::compile(const QString& source) -> Result {
  Result result;
  try {
    result.graph = std::make_shared<nm::Graph>(nm::compileGraph(source, registry_));
  } catch (const nm::DslSyntaxError& error) {
    result.error = QString::fromUtf8(error.what());
    result.diagnostic = error.diagnostic();
  } catch (const std::exception& error) {
    // UnsupportedDsl, a validation or expansion failure, or a graph the
    // runtime cannot represent. All leave the previous program on screen.
    result.error = QString::fromUtf8(error.what());
  }
  return result;
}

}  // namespace noisefactor::sync::render_helper
