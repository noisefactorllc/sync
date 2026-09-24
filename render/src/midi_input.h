#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <memory>
#include <vector>

class RtMidiIn;

namespace nm {
class MidiState;
}

namespace noisefactor::sync::render_helper {

// A MIDI input port as the engine's MidiState registers it: a stable id and a
// display name. RtMidi identifies ports only by name, so the id is the name,
// made unique by occurrence when two devices report the same one.
struct MidiPort {
  QString id;
  QString name;
};

// Port ids for a list of reported names, in order: "IAC Bus 1", and a second
// port of the same name "IAC Bus 1 #2", so ids stay stable across rescans as
// long as the devices stay in the same order.
[[nodiscard]] auto midi_port_ids(const QStringList& names) -> std::vector<MidiPort>;

// Whether a port is wanted: every port when the list is empty, else a port
// whose name contains any entry (case-insensitive).
[[nodiscard]] auto midi_port_wanted(const QString& name, const QStringList& wanted) -> bool;

// Opens the machine's MIDI inputs and collects their raw messages. RtMidi
// delivers on its own threads; drain() hands the backlog to the render
// thread in arrival order. Ports come and go, so the port list is rescanned
// every two seconds and changes are reported.
class MidiInput final : public QObject {
  Q_OBJECT

 public:
  struct Message {
    std::vector<std::uint8_t> bytes;
    QString port_id;
    QString port_name;
  };

  explicit MidiInput(QStringList wanted, QObject* parent = nullptr);
  ~MidiInput() override;

  // Opens the wanted ports that exist now and starts watching for more.
  void start();
  [[nodiscard]] auto drain() -> std::vector<Message>;
  [[nodiscard]] auto open_ports() const -> std::vector<MidiPort>;

 signals:
  void ports_changed(const QStringList& open_port_names);

 private:
  struct Port;
  void rescan();
  void push(const Port& port, const std::vector<unsigned char>& bytes);

  QStringList wanted_;
  std::vector<std::unique_ptr<Port>> ports_;
  QTimer rescan_timer_;
  bool scanned_ = false;
  mutable QMutex mutex_;
  std::vector<Message> pending_;
};

// Feeds drained messages to the engine's MidiState, each attributed to its
// port. Returns whether anything was fed, i.e. whether the backend needs a
// fresh snapshot.
[[nodiscard]] auto feed_midi(nm::MidiState& state, const std::vector<MidiInput::Message>& messages)
    -> bool;

// Mirrors the open ports into the MidiState's port registry, so midi() steps
// that name a port see it connect and disconnect: removed ports are
// disconnected, open ones registered, and the inventory replaced.
void sync_midi_ports(nm::MidiState& state, const std::vector<MidiPort>& open);

}  // namespace noisefactor::sync::render_helper
