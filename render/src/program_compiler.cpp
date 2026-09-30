#include "program_compiler.h"

#include <QDirIterator>
#include <QJsonArray>
#include <QRegularExpression>

#include <exception>
#include <stdexcept>
#include <vector>

#include <compiler/diagnostics.h>
#include <compiler/dsl_compiler.h>
#include <compiler/lexer.h>
#include <compiler/parser.h>
#include <compiler/validator.h>

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

[[nodiscard]] auto image_id(const QString& url) -> QString {
  static const QRegularExpression pattern(QStringLiteral("^image:([a-f0-9]{64})$"));
  const auto match = pattern.match(url);
  if (!match.hasMatch()) throw std::runtime_error("media url must be a persisted image:<sha256> reference");
  return match.captured(1);
}

// The full validator assigns temp IDs after expanding inline chains and
// ordering dependencies. Lexical occurrence order is not the render step ID.
void collect_images(const QJsonValue& value, QHash<int, QString>& images) {
  if (value.isArray()) {
    for (const auto& item : value.toArray()) collect_images(item, images);
  } else if (value.isObject()) {
    const auto object = value.toObject();
    if (object.value(QStringLiteral("op")) == QStringLiteral("synth.media")) {
      const auto kwargs = object.value(QStringLiteral("rawKwargs")).toObject();
      if (kwargs.contains(QStringLiteral("url"))) {
        const auto url = kwargs.value(QStringLiteral("url")).toObject();
        if (url.value(QStringLiteral("type")) != QStringLiteral("String")) {
          throw std::runtime_error("media url must be a literal image reference");
        }
        images.insert(object.value(QStringLiteral("temp")).toInt(-1),
                      image_id(url.value(QStringLiteral("value")).toString()));
      }
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) collect_images(it.value(), images);
  }
}

[[nodiscard]] auto runtime_source(const QString& source, const QJsonArray& all_tokens,
                                   bool default_media_is_shared) -> QString {
  QList<QJsonObject> tokens;
  for (const auto& value : all_tokens) {
    const auto token = value.toObject();
    if (token.value(QStringLiteral("type")) != QStringLiteral("COMMENT")) tokens.push_back(token);
  }
  QList<qsizetype> line_starts{0};
  for (qsizetype i = 0; i < source.size(); ++i) {
    if (source.at(i) == QLatin1Char('\n')) line_starts.push_back(i + 1);
  }
  const auto position = [&](qsizetype i) {
    const auto& token = tokens.at(i);
    return line_starts.at(token.value(QStringLiteral("line")).toInt() - 1)
        + token.value(QStringLiteral("col")).toInt() - 1;
  };
  const auto lexeme = [&](qsizetype i) { return tokens.at(i).value(QStringLiteral("lexeme")).toString(); };
  QString stripped = source;
  struct Context { bool media = false; bool from = false; bool group = false; QString name_space; };
  std::vector<Context> call_stack;
  for (qsizetype i = 0; i < tokens.size(); ++i) {
    const QString type = tokens.at(i).value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("LPAREN") || type == QStringLiteral("LBRACKET") || type == QStringLiteral("LBRACE")) {
      Context context;
      if (type == QStringLiteral("LPAREN")) {
        const QString name = i > 0 && tokens.at(i - 1).value(QStringLiteral("type")) == QStringLiteral("IDENT")
            ? lexeme(i - 1) : QString();
        context.group = name.isEmpty();
        if (!call_stack.empty() && (call_stack.back().from || call_stack.back().group)) {
          context.name_space = call_stack.back().name_space;
        }
        context.from = name == QStringLiteral("from");
        if (context.from && context.name_space.isEmpty()) {
          for (qsizetype j = i + 1; j < tokens.size() && lexeme(j) != QStringLiteral(","); ++j) {
            context.name_space += lexeme(j);
          }
        }
        context.media = name == QStringLiteral("media") && (context.name_space.isEmpty()
            ? default_media_is_shared : context.name_space == QStringLiteral("synth"));
      }
      call_stack.push_back(context);
    } else if (type == QStringLiteral("RPAREN") || type == QStringLiteral("RBRACKET") || type == QStringLiteral("RBRACE")) {
      if (!call_stack.empty()) call_stack.pop_back();
    } else if (!call_stack.empty() && call_stack.back().media && type == QStringLiteral("IDENT")
               && lexeme(i) == QStringLiteral("url") && i + 3 < tokens.size()
               && lexeme(i + 1) == QStringLiteral(":")) {
      if (tokens.at(i + 2).value(QStringLiteral("type")) != QStringLiteral("STRING")) {
        throw std::runtime_error("media url must be a literal image reference");
      }
      (void)image_id(lexeme(i + 2));
      const QString next = lexeme(i + 3);
      if (next != QStringLiteral(",") && next != QStringLiteral(")")) {
        throw std::runtime_error("media url must be a literal image reference");
      }
      qsizetype start = position(i), end = position(i + 3);
      if (next == QStringLiteral(",")) ++end;
      else if (i > 0 && lexeme(i - 1) == QStringLiteral(",")) start = position(i - 1);
      // Keep diagnostic line/column locations stable for every other argument.
      for (qsizetype at = start; at < end; ++at) {
        if (stripped.at(at) != QLatin1Char('\n')) stripped[at] = QLatin1Char(' ');
      }
    }
  }
  return stripped;
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
    if (!source.contains(QStringLiteral("url"))) {
      result.graph = std::make_shared<nm::Graph>(nm::compileGraph(source, registry_));
    } else {
      const auto tokens = nm::lex(source);
      const auto ast = nm::parse(tokens);
      const auto validated = nm::validate(ast, registry_);
      collect_images(validated.value(QStringLiteral("plans")), result.shared_images);
      bool default_media_is_shared = false;
      for (const auto& item : ast.value(QStringLiteral("namespace")).toObject().value(QStringLiteral("searchOrder")).toArray()) {
        const QString name_space = item.toString();
        if (!registry_.getOp(name_space + QStringLiteral(".media"))) continue;
        default_media_is_shared = name_space == QStringLiteral("synth");
        break;
      }
      const QString runtime = result.shared_images.isEmpty() ? source
          : runtime_source(source, tokens, default_media_is_shared);
      result.graph = std::make_shared<nm::Graph>(nm::compileGraph(runtime, registry_));
    }
  } catch (const nm::DslSyntaxError& error) {
    result.error = QString::fromUtf8(error.what());
    result.diagnostic = error.diagnostic();
  } catch (const std::exception& error) {
    // UnsupportedDsl, a validation or expansion failure, or a graph the
    // runtime cannot represent. All leave the previous program on screen.
    result.error = QString::fromUtf8(error.what());
  }
  if (!result.graph) result.shared_images.clear();
  return result;
}

}  // namespace noisefactor::sync::render_helper
