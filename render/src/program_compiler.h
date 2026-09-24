#pragma once

#include <QJsonObject>
#include <QString>

#include <memory>

#include <compiler/effect_registry.h>
#include <runtime/graph.h>

namespace noisefactor::sync::render_helper {

// Compiles program text to a render graph with the effect catalogue loaded
// once at startup. A failed compile is data, not an exception: the caller
// keeps rendering the last program that compiled and reports the error.
class ProgramCompiler {
 public:
  struct Result {
    std::shared_ptr<nm::Graph> graph;  // null on failure
    QString error;
    QJsonObject diagnostic;  // structured lexer/parser diagnostic, when there is one
  };

  explicit ProgramCompiler(const QString& data_root);

  // Definition files found under the data root. Zero means the helper was
  // pointed at the wrong tree, which every compile would otherwise report
  // one unknown effect at a time.
  [[nodiscard]] auto effect_count() const -> int;
  [[nodiscard]] auto compile(const QString& source) -> Result;
  // The catalogue live parameter updates resolve against.
  [[nodiscard]] auto registry() const -> const nm::EffectRegistry& { return registry_; }

 private:
  int definitions_ = 0;
  nm::EffectRegistry registry_;
};

}  // namespace noisefactor::sync::render_helper
