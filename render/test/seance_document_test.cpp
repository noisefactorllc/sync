#include "test_harness.hpp"

#include <QJsonArray>
#include <QJsonObject>

#include "seance_client.h"
#include "seance_document.h"

namespace {

using noisefactor::sync::render_helper::parse_session_id;
using noisefactor::sync::render_helper::SeanceDocument;
using noisefactor::sync::render_helper::session_socket_url;

[[nodiscard]] auto doc(const QString& id, const QString& text, int rev,
                       const QString& kind = QStringLiteral("noisemaker-dsl"),
                       bool is_default = false) -> QJsonObject {
  return {{QStringLiteral("id"), id},
          {QStringLiteral("title"), QStringLiteral("Program")},
          {QStringLiteral("kind"), kind},
          {QStringLiteral("rev"), rev},
          {QStringLiteral("text"), text},
          {QStringLiteral("default"), is_default}};
}

[[nodiscard]] auto edit(const QString& doc_id, int rev, int start, int end, const QString& text)
    -> QJsonObject {
  return {{QStringLiteral("type"), QStringLiteral("doc-edit")},
          {QStringLiteral("docId"), doc_id},
          {QStringLiteral("rev"), rev},
          {QStringLiteral("authorSeq"), 1},
          {QStringLiteral("edit"), QJsonObject{{QStringLiteral("start"), start},
                                               {QStringLiteral("end"), end},
                                               {QStringLiteral("text"), text}}}};
}

SYNC_TEST(the_controllers_main_document_wins) {
  SeanceDocument document;
  SYNC_REQUIRE(document.adopt(QJsonArray{doc(QStringLiteral("notes"), QStringLiteral("x"), 0,
                                             QStringLiteral("noisemaker-dsl"), true),
                                         doc(QStringLiteral("main"), QStringLiteral("y"), 4)}));
  SYNC_REQUIRE(document.doc_id() == QStringLiteral("main"));
  SYNC_REQUIRE(document.text() == QStringLiteral("y"));
  SYNC_REQUIRE(document.rev() == 4);
}

SYNC_TEST(without_main_the_default_program_then_any_program_is_used) {
  SeanceDocument document;
  document.adopt(QJsonArray{doc(QStringLiteral("a"), QStringLiteral("1"), 0),
                            doc(QStringLiteral("b"), QStringLiteral("2"), 0,
                                QStringLiteral("noisemaker-dsl"), true)});
  SYNC_REQUIRE(document.doc_id() == QStringLiteral("b"));
  document.adopt(QJsonArray{doc(QStringLiteral("deck:A"), QStringLiteral("x"), 0,
                                QStringLiteral("layers"), true),
                            doc(QStringLiteral("c"), QStringLiteral("3"), 0)});
  SYNC_REQUIRE(document.doc_id() == QStringLiteral("c"));
  SYNC_REQUIRE(document.adopt(QJsonArray{doc(QStringLiteral("deck:A"), QStringLiteral("x"), 0,
                                             QStringLiteral("layers"), true)}));
  SYNC_REQUIRE(!document.has_document());
}

SYNC_TEST(edits_apply_in_utf16_units) {
  SeanceDocument document;
  // U+1F600 is two UTF-16 units, the unit seance counts in. An offset after
  // it lands after both halves, not in the middle of the character.
  document.adopt(QJsonArray{doc(QStringLiteral("main"), QString::fromUtf8("a\xF0\x9F\x98\x80z"), 0)});
  SYNC_REQUIRE(document.apply(edit(QStringLiteral("main"), 1, 3, 3, QStringLiteral("b"))) ==
               SeanceDocument::Edit::Applied);
  SYNC_REQUIRE(document.text() == QString::fromUtf8("a\xF0\x9F\x98\x80" "bz"));
  SYNC_REQUIRE(document.apply(edit(QStringLiteral("main"), 2, 1, 3, QStringLiteral(""))) ==
               SeanceDocument::Edit::Applied);
  SYNC_REQUIRE(document.text() == QStringLiteral("abz"));
  SYNC_REQUIRE(document.rev() == 2);
}

SYNC_TEST(a_revision_gap_asks_for_a_resync_and_a_repeat_is_ignored) {
  SeanceDocument document;
  document.adopt(QJsonArray{doc(QStringLiteral("main"), QStringLiteral("abc"), 5)});
  SYNC_REQUIRE(document.apply(edit(QStringLiteral("main"), 5, 0, 0, QStringLiteral("x"))) ==
               SeanceDocument::Edit::Ignored);
  SYNC_REQUIRE(document.apply(edit(QStringLiteral("main"), 7, 0, 0, QStringLiteral("x"))) ==
               SeanceDocument::Edit::NeedsResync);
  SYNC_REQUIRE(document.text() == QStringLiteral("abc"));
  SYNC_REQUIRE(document.rev() == 5);
}

SYNC_TEST(an_edit_that_does_not_fit_asks_for_a_resync) {
  SeanceDocument document;
  document.adopt(QJsonArray{doc(QStringLiteral("main"), QStringLiteral("abc"), 0)});
  SYNC_REQUIRE(document.apply(edit(QStringLiteral("main"), 1, 2, 9, QStringLiteral(""))) ==
               SeanceDocument::Edit::NeedsResync);
  SYNC_REQUIRE(document.apply(edit(QStringLiteral("main"), 1, 2, 1, QStringLiteral(""))) ==
               SeanceDocument::Edit::NeedsResync);
  QJsonObject bad = edit(QStringLiteral("main"), 1, 0, 0, QStringLiteral(""));
  bad.insert(QStringLiteral("rev"), 1.5);
  SYNC_REQUIRE(document.apply(bad) == SeanceDocument::Edit::NeedsResync);
  SYNC_REQUIRE(document.text() == QStringLiteral("abc"));
}

SYNC_TEST(edits_to_other_documents_are_ignored) {
  SeanceDocument document;
  document.adopt(QJsonArray{doc(QStringLiteral("main"), QStringLiteral("abc"), 0)});
  SYNC_REQUIRE(document.apply(edit(QStringLiteral("deck:B"), 1, 0, 0, QStringLiteral("x"))) ==
               SeanceDocument::Edit::Ignored);
  SYNC_REQUIRE(document.text() == QStringLiteral("abc"));
}

SYNC_TEST(session_ids_come_bare_or_in_a_share_link) {
  SYNC_REQUIRE(parse_session_id(QStringLiteral("Ab12Cd")) == QStringLiteral("Ab12Cd"));
  SYNC_REQUIRE(parse_session_id(QStringLiteral(" Ab12Cd\n")) == QStringLiteral("Ab12Cd"));
  SYNC_REQUIRE(parse_session_id(QStringLiteral("https://noisedeck.app/?mode=freeform&seance=Zz9")) ==
               QStringLiteral("Zz9"));
  SYNC_REQUIRE(!parse_session_id(QStringLiteral("https://noisedeck.app/")).has_value());
  SYNC_REQUIRE(!parse_session_id(QStringLiteral("../etc")).has_value());
  SYNC_REQUIRE(!parse_session_id(QStringLiteral("https://x/?seance=a%2Fb")).has_value());
}

SYNC_TEST(the_socket_url_follows_the_server_scheme) {
  SYNC_REQUIRE(session_socket_url(QUrl(QStringLiteral("https://seance.noisefactor.io")),
                                  QStringLiteral("Ab12Cd")) ==
               QUrl(QStringLiteral("wss://seance.noisefactor.io/v1/sessions/Ab12Cd/ws")));
  SYNC_REQUIRE(session_socket_url(QUrl(QStringLiteral("http://127.0.0.1:8123/ignored?q=1")),
                                  QStringLiteral("x")) ==
               QUrl(QStringLiteral("ws://127.0.0.1:8123/v1/sessions/x/ws")));
}

}  // namespace
