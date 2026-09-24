#include "midi_input.h"

#include <QHash>
#include <QMutexLocker>

#include <RtMidi.h>

#include <runtime/midi_state.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace noisefactor::sync::render_helper {

namespace {

// A stalled render thread must not let a busy controller grow the backlog
// without bound; the oldest messages go first, as a real MIDI buffer would.
constexpr std::size_t kMaximumPending = 8192;

}  // namespace

struct MidiInput::Port {
  MidiInput* owner = nullptr;
  MidiPort identity;
  std::unique_ptr<RtMidiIn> input;
};

auto midi_port_ids(const QStringList& names) -> std::vector<MidiPort> {
  std::vector<MidiPort> ports;
  QHash<QString, int> seen;
  for (const QString& name : names) {
    const int occurrence = ++seen[name];
    ports.push_back({occurrence == 1 ? name : QStringLiteral("%1 #%2").arg(name).arg(occurrence), name});
  }
  return ports;
}

auto midi_port_wanted(const QString& name, const QStringList& wanted) -> bool {
  if (wanted.isEmpty()) return true;
  return std::any_of(wanted.begin(), wanted.end(), [&](const QString& entry) {
    return name.contains(entry, Qt::CaseInsensitive);
  });
}

MidiInput::MidiInput(QStringList wanted, QObject* parent)
    : QObject(parent), wanted_(std::move(wanted)) {
  rescan_timer_.setInterval(2000);
  connect(&rescan_timer_, &QTimer::timeout, this, &MidiInput::rescan);
}

MidiInput::~MidiInput() {
  for (auto& port : ports_) {
    if (port->input) port->input->closePort();
  }
}

void MidiInput::start() {
  rescan();
  rescan_timer_.start();
}

void MidiInput::rescan() {
  QStringList names;
  try {
    RtMidiIn probe;
    const unsigned count = probe.getPortCount();
    for (unsigned i = 0; i < count; ++i) {
      names << QString::fromStdString(probe.getPortName(i));
    }
  } catch (const RtMidiError& error) {
    std::fprintf(stderr, "sync-render: MIDI unavailable: %s\n", error.getMessage().c_str());
    return;
  }

  const std::vector<MidiPort> present = midi_port_ids(names);
  // The first scan always reports, so the log says which ports were open
  // from the start, including none.
  bool changed = !scanned_;
  scanned_ = true;

  // Close ports that went away.
  const auto gone = std::remove_if(ports_.begin(), ports_.end(), [&](const auto& port) {
    return std::none_of(present.begin(), present.end(),
                        [&](const MidiPort& p) { return p.id == port->identity.id; });
  });
  if (gone != ports_.end()) changed = true;
  ports_.erase(gone, ports_.end());

  // Open wanted ports that are new. Port numbers are positions in this scan.
  for (std::size_t index = 0; index < present.size(); ++index) {
    const MidiPort& candidate = present[index];
    if (!midi_port_wanted(candidate.name, wanted_)) continue;
    if (std::any_of(ports_.begin(), ports_.end(),
                    [&](const auto& port) { return port->identity.id == candidate.id; })) {
      continue;
    }
    auto port = std::make_unique<Port>();
    port->owner = this;
    port->identity = candidate;
    try {
      port->input = std::make_unique<RtMidiIn>();
      // Keep timing clock (the reference counts 0xF8); drop sysex and active
      // sensing, which the engine does not read.
      port->input->ignoreTypes(true, false, true);
      Port* raw = port.get();
      port->input->setCallback(
          [](double, std::vector<unsigned char>* message, void* context) {
            auto* p = static_cast<Port*>(context);
            if (message != nullptr && !message->empty()) p->owner->push(*p, *message);
          },
          raw);
      port->input->openPort(static_cast<unsigned>(index), "sync-render");
    } catch (const RtMidiError& error) {
      std::fprintf(stderr, "sync-render: MIDI port %s: %s\n", qPrintable(candidate.name),
                   error.getMessage().c_str());
      continue;
    }
    ports_.push_back(std::move(port));
    changed = true;
  }

  if (changed) {
    QStringList open;
    for (const auto& port : ports_) open << port->identity.name;
    emit ports_changed(open);
  }
}

void MidiInput::push(const Port& port, const std::vector<unsigned char>& bytes) {
  QMutexLocker lock(&mutex_);
  if (pending_.size() >= kMaximumPending) pending_.erase(pending_.begin());
  pending_.push_back({std::vector<std::uint8_t>(bytes.begin(), bytes.end()), port.identity.id,
                      port.identity.name});
}

auto MidiInput::drain() -> std::vector<Message> {
  QMutexLocker lock(&mutex_);
  std::vector<Message> out;
  out.swap(pending_);
  return out;
}

auto MidiInput::open_ports() const -> std::vector<MidiPort> {
  std::vector<MidiPort> open;
  for (const auto& port : ports_) open.push_back(port->identity);
  return open;
}

auto feed_midi(nm::MidiState& state, const std::vector<MidiInput::Message>& messages) -> bool {
  for (const MidiInput::Message& message : messages) {
    const nm::MidiPort port{message.port_id, message.port_name, true};
    state.handleMessage(reinterpret_cast<const quint8*>(message.bytes.data()),
                        static_cast<qsizetype>(message.bytes.size()), &port);
  }
  return !messages.empty();
}

void sync_midi_ports(nm::MidiState& state, const std::vector<MidiPort>& open) {
  // The reference host's order (external-input.js MidiInputManager): ports
  // that went away are disconnected, open ones registered, and then the
  // inventory is replaced with what is open now.
  for (const nm::MidiPort& known : state.ports()) {
    if (!known.connected) continue;
    const bool still_open = std::any_of(open.begin(), open.end(),
                                        [&](const MidiPort& port) { return port.id == known.id; });
    if (!still_open) state.disconnectPort(known.id);
  }
  QVector<nm::MidiPort> inventory;
  for (const MidiPort& port : open) {
    const nm::MidiPort entry{port.id, port.name, true};
    state.registerPort(entry);
    inventory.push_back(entry);
  }
  state.setPortInventory(inventory);
}

}  // namespace noisefactor::sync::render_helper
