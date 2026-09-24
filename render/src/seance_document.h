#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace noisefactor::sync::render_helper {

// The one seance document the helper renders: the program text a controller
// such as Noisedeck publishes as kind "noisemaker-dsl" (Noisedeck's id is
// "main"). The helper only ever reads, so this holds the server's text and
// applies the server's canonical edits; there is no local text to rebase.
//
// Offsets are UTF-16 code units on the wire (seance protocol.md, top), which
// is QString's own unit, so an edit applies with no conversion.
class SeanceDocument {
 public:
  enum class Edit {
    Applied,       // the text changed
    Ignored,       // another document, or a revision already applied
    NeedsResync,   // a gap or an edit that does not fit; ask for a snapshot
  };

  explicit SeanceDocument(QString preferred_id = QStringLiteral("main"));

  // Adopts the docs array of a session-snapshot or doc-snapshot. Returns true
  // when the selected document's text or identity changed.
  auto adopt(const QJsonArray& docs) -> bool;
  auto apply(const QJsonObject& doc_edit) -> Edit;
  void clear();

  [[nodiscard]] auto has_document() const -> bool { return !doc_id_.isEmpty(); }
  [[nodiscard]] auto doc_id() const -> const QString& { return doc_id_; }
  [[nodiscard]] auto text() const -> const QString& { return text_; }
  [[nodiscard]] auto rev() const -> qint64 { return rev_; }

 private:
  QString preferred_id_;
  QString doc_id_;
  QString text_;
  qint64 rev_ = -1;
};

}  // namespace noisefactor::sync::render_helper
