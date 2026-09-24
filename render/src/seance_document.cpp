#include "seance_document.h"

#include <utility>

namespace noisefactor::sync::render_helper {

namespace {

const QString kDialectKind = QStringLiteral("noisemaker-dsl");

[[nodiscard]] auto integer(const QJsonValue& value, qint64& out) -> bool {
  if (!value.isDouble()) return false;
  const double number = value.toDouble();
  const auto whole = static_cast<qint64>(number);
  if (static_cast<double>(whole) != number || whole < 0) return false;
  out = whole;
  return true;
}

}  // namespace

SeanceDocument::SeanceDocument(QString preferred_id) : preferred_id_(std::move(preferred_id)) {}

auto SeanceDocument::adopt(const QJsonArray& docs) -> bool {
  // Preference order: the controller's known id, then the session's default
  // program, then any program. A session always has at most one of each id.
  QJsonObject chosen;
  QJsonObject fallback_default;
  QJsonObject fallback_any;
  for (const QJsonValue& value : docs) {
    const QJsonObject doc = value.toObject();
    if (!doc.value(QStringLiteral("text")).isString()) continue;
    const QString id = doc.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) continue;
    if (id == preferred_id_) {
      chosen = doc;
      break;
    }
    const bool program = doc.value(QStringLiteral("kind")).toString() == kDialectKind;
    if (program && doc.value(QStringLiteral("default")).toBool() && fallback_default.isEmpty()) {
      fallback_default = doc;
    }
    if (program && fallback_any.isEmpty()) fallback_any = doc;
  }
  if (chosen.isEmpty()) chosen = !fallback_default.isEmpty() ? fallback_default : fallback_any;

  if (chosen.isEmpty()) {
    const bool changed = has_document();
    clear();
    return changed;
  }
  qint64 rev = 0;
  if (!integer(chosen.value(QStringLiteral("rev")), rev)) rev = 0;
  const QString id = chosen.value(QStringLiteral("id")).toString();
  const QString text = chosen.value(QStringLiteral("text")).toString();
  const bool changed = id != doc_id_ || text != text_;
  doc_id_ = id;
  text_ = text;
  rev_ = rev;
  return changed;
}

auto SeanceDocument::apply(const QJsonObject& doc_edit) -> Edit {
  if (!has_document()) return Edit::NeedsResync;
  if (doc_edit.value(QStringLiteral("docId")).toString() != doc_id_) return Edit::Ignored;

  qint64 rev = 0;
  if (!integer(doc_edit.value(QStringLiteral("rev")), rev)) return Edit::NeedsResync;
  // The relayed rev is the revision after this edit, and the edit is
  // canonical against the text at rev - 1 (seance textdoc.py). Anything we
  // already hold is a repeat; anything further ahead means we missed one.
  if (rev <= rev_) return Edit::Ignored;
  if (rev != rev_ + 1) return Edit::NeedsResync;

  const QJsonObject edit = doc_edit.value(QStringLiteral("edit")).toObject();
  qint64 start = 0;
  qint64 end = 0;
  if (!integer(edit.value(QStringLiteral("start")), start) ||
      !integer(edit.value(QStringLiteral("end")), end) ||
      !edit.value(QStringLiteral("text")).isString()) {
    return Edit::NeedsResync;
  }
  if (start > end || end > text_.size()) return Edit::NeedsResync;

  text_.replace(static_cast<qsizetype>(start), static_cast<qsizetype>(end - start),
                edit.value(QStringLiteral("text")).toString());
  rev_ = rev;
  return Edit::Applied;
}

void SeanceDocument::clear() {
  doc_id_.clear();
  text_.clear();
  rev_ = -1;
}

}  // namespace noisefactor::sync::render_helper
