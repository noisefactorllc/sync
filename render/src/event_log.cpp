#include "event_log.h"

#include <QJsonDocument>

#include <sync/render/render_ring.hpp>

namespace noisefactor::sync::render_helper {

void EventLog::write(const QString& event, QJsonObject fields) {
  fields.insert(QStringLiteral("event"), event);
  fields.insert(QStringLiteral("time_us"),
                static_cast<double>(render::render_clock_us()));
  QByteArray line = QJsonDocument(fields).toJson(QJsonDocument::Compact);
  line.append('\n');
  std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), out_);
  std::fflush(out_);
}

}  // namespace noisefactor::sync::render_helper
