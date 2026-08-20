// Sync.exe -- the Windows tray companion, mirroring native/src/platform/
// macos/app_main.mm's menu-bar app: it supervises syncd.exe (via
// CompanionProcess) and drives the shared CompanionModel exactly the way
// the macOS app does, presenting the result as a Shell_NotifyIconW tray
// icon instead of an NSStatusItem.
//
// Built as the WIN32-subsystem `sync_menu` target so launching it from
// Explorer or at sign-in never flashes a console window. CMake links
// user32, shell32, and advapi32; the #pragma comment(lib, ...) directives
// below duplicate that for MSVC so the file also builds standalone.

#include "companion_process.hpp"
#include "resource.h"

#include <sync/companion_model.hpp>
#include <sync/server.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace companion = noisefactor::sync::companion;

namespace {

// ---------------------------------------------------------------------
// Small Win32 helpers
// ---------------------------------------------------------------------

constexpr UINT kMessageTrayIcon = WM_APP + 1;
constexpr UINT kMessageDispatch = WM_APP + 2;
constexpr UINT_PTR kPollTimerId = 1;
constexpr UINT_PTR kRecoveryTimerId = 2;
constexpr UINT kTrayIconId = 1;

constexpr UINT kCommandRestart = 1001;
constexpr UINT kCommandCopyDiagnostics = 1002;
constexpr UINT kCommandQuit = 1003;
constexpr UINT kCommandPairingsRefresh = 1004;
// Dynamic pairing-revoke items get ids in [kCommandPairingsBase,
// kCommandPairingsBase + kMaximumPairingMenuEntries), one per cached
// pairing, indexed positionally -- mirroring macOS's use of representedObject
// to carry the origin string on each revoke menu item.
constexpr UINT kCommandPairingsBase = 2000;
constexpr std::size_t kMaximumPairingMenuEntries = 64;

std::wstring to_wide(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int needed = ::MultiByteToWideChar(
      CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), needed);
  return wide;
}

std::string to_utf8(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int needed = ::WideCharToMultiByte(
      CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
      nullptr, nullptr);
  if (needed <= 0) return {};
  std::string utf8(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), needed, nullptr, nullptr);
  return utf8;
}

std::uint64_t monotonic_milliseconds() noexcept {
  return static_cast<std::uint64_t>(::GetTickCount64());
}

// HKCU\Software\Noise Factor\Sync -- the Windows analogue of the macOS
// app's NSUserDefaults "SyncPreviewNoticeShown" bool, gating the same
// one-time first-run notice.
bool preview_notice_shown() {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Noise Factor\\Sync", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD value = 0;
  DWORD size = sizeof(value);
  DWORD type = 0;
  const bool shown =
      ::RegQueryValueExW(key, L"PreviewNoticeShown", nullptr, &type,
                        reinterpret_cast<BYTE*>(&value), &size) ==
          ERROR_SUCCESS &&
      type == REG_DWORD && value != 0;
  ::RegCloseKey(key);
  return shown;
}

void mark_preview_notice_shown() {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Noise Factor\\Sync", 0,
                        nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
    return;
  }
  const DWORD value = 1;
  ::RegSetValueExW(key, L"PreviewNoticeShown", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
  ::RegCloseKey(key);
}

void copy_to_clipboard(HWND owner, const std::wstring& text) {
  if (!::OpenClipboard(owner)) return;
  ::EmptyClipboard();
  const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory != nullptr) {
    void* locked = ::GlobalLock(memory);
    if (locked != nullptr) {
      std::memcpy(locked, text.c_str(), bytes);
      ::GlobalUnlock(memory);
      // Ownership of `memory` passes to the clipboard on success; it must
      // not be freed here.
      if (::SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        ::GlobalFree(memory);
      }
    } else {
      ::GlobalFree(memory);
    }
  }
  ::CloseClipboard();
}

// ---------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------

struct AppState {
  HWND hwnd = nullptr;
  HICON icon = nullptr;
  std::unique_ptr<companion::CompanionModel> model;
  std::unique_ptr<companion::CompanionProcess> process;
  bool expected_exit = false;
  bool quitting = false;

  std::vector<std::string> pairings;
  std::string pairing_error;
  bool pairings_loading = false;
  bool pairings_fetched_ever = false;
  std::chrono::steady_clock::time_point pairings_fetched_at{};

  // The recovery attempt currently armed on kRecoveryTimerId, if any. A
  // single-instance app only ever has one recovery outstanding at a time
  // (CompanionModel::recovery_generation() enforces that), so one slot
  // suffices.
  std::optional<companion::RecoverySchedule> pending_recovery;
};

void probe_then_start(AppState& app);
void start_helper(AppState& app);
void schedule_recovery(AppState& app, companion::RecoverySchedule schedule);
void preflight_recovery(AppState& app, std::uint64_t generation);
void start_recovery_helper(AppState& app, std::uint64_t generation);
void refresh_pairings(AppState& app);
void show_context_menu(AppState& app);

void probe_then_start(AppState& app) {
  const std::uint64_t generation = app.model->recovery_generation();
  AppState* self = &app;
  app.process->probe([self, generation](
                         std::optional<companion::HealthSnapshot> health,
                         std::string) {
    if (self->quitting || self->model->recovery_generation() != generation) {
      return;
    }
    if (health.has_value()) {
      self->model->observe_health(std::move(*health), monotonic_milliseconds());
      return;
    }
    start_helper(*self);
  });
}

void start_helper(AppState& app) {
  if (app.quitting || app.process->owned_pid().has_value()) return;
  app.model->begin_start();
  AppState* self = &app;
  std::string error;
  const bool started = app.process->start(
      [self](std::string_view bytes) { self->model->append_stderr(bytes); },
      [self](int status) {
        const bool expected = self->expected_exit || self->quitting;
        self->expected_exit = false;
        const auto recovery =
            self->model->helper_exited(status, expected, monotonic_milliseconds());
        if (recovery.has_value()) schedule_recovery(*self, *recovery);
      },
      error);
  if (!started) {
    const auto recovery =
        app.model->helper_exited(-1, false, monotonic_milliseconds());
    app.model->append_stderr(error);
    if (recovery.has_value()) schedule_recovery(app, *recovery);
    return;
  }
  if (const auto pid = app.process->owned_pid(); pid.has_value()) {
    app.model->helper_started(*pid);
  }
}

void schedule_recovery(AppState& app, companion::RecoverySchedule schedule) {
  if (app.quitting || !app.model->recovery_active() ||
      app.model->recovery_generation() != schedule.generation) {
    return;
  }
  const std::uint64_t now = monotonic_milliseconds();
  const std::uint64_t delay_ms =
      schedule.due_ms > now ? schedule.due_ms - now : 0;
  // One-shot timer: the WM_TIMER handler kills kRecoveryTimerId immediately
  // on receipt, so this never repeats. Re-scheduling (a later call with a
  // new due_ms) simply replaces whatever was pending, which SetTimer does
  // for free when reusing the same id.
  app.pending_recovery = schedule;
  ::SetTimer(app.hwnd, kRecoveryTimerId,
            static_cast<UINT>(std::min<std::uint64_t>(delay_ms, UINT_MAX)),
            nullptr);
}

void preflight_recovery(AppState& app, std::uint64_t generation) {
  if (app.quitting || app.model->recovery_generation() != generation) return;
  AppState* self = &app;
  app.process->probe([self, generation](
                         std::optional<companion::HealthSnapshot> health,
                         std::string) {
    if (self->quitting || self->model->recovery_generation() != generation) {
      return;
    }
    if (health.has_value()) {
      if (!health->compatible) {
        const auto retry = self->model->recovery_preflight_conflict(
            std::move(*health), monotonic_milliseconds());
        if (retry.has_value()) schedule_recovery(*self, *retry);
        return;
      }
      self->model->observe_health(std::move(*health), monotonic_milliseconds());
      return;
    }
    start_recovery_helper(*self, generation);
  });
}

void start_recovery_helper(AppState& app, std::uint64_t generation) {
  if (app.quitting || app.model->recovery_generation() != generation ||
      app.process->owned_pid().has_value()) {
    return;
  }
  AppState* self = &app;
  std::string error;
  const bool started = app.process->start(
      [self](std::string_view bytes) { self->model->append_stderr(bytes); },
      [self](int status) {
        const bool expected = self->expected_exit || self->quitting;
        self->expected_exit = false;
        const auto recovery =
            self->model->helper_exited(status, expected, monotonic_milliseconds());
        if (recovery.has_value()) schedule_recovery(*self, *recovery);
      },
      error);
  if (!started) {
    app.model->append_stderr(error);
    const auto recovery =
        app.model->recovery_launch_failed(-1, monotonic_milliseconds());
    if (recovery.has_value()) schedule_recovery(app, *recovery);
    return;
  }
  if (const auto pid = app.process->owned_pid(); pid.has_value()) {
    app.model->helper_started(*pid, monotonic_milliseconds());
  } else {
    const auto recovery =
        app.model->recovery_launch_failed(-1, monotonic_milliseconds());
    if (recovery.has_value()) schedule_recovery(app, *recovery);
  }
}

void poll_status(AppState& app) {
  if (app.quitting) return;
  // While no replacement process exists, the generation-bound recovery
  // preflight is the sole owner of discovery -- this keeps the periodic
  // poll from consuming or cancelling the same attempt concurrently.
  if (app.model->recovery_active() && !app.process->owned_pid().has_value()) {
    return;
  }
  const std::uint64_t generation = app.model->recovery_generation();
  AppState* self = &app;
  app.process->probe([self, generation](
                         std::optional<companion::HealthSnapshot> health,
                         std::string) {
    if (self->quitting || self->model->recovery_generation() != generation) {
      return;
    }
    if (health.has_value()) {
      if (self->model->recovery_active() &&
          self->process->owned_pid().has_value() && !health->compatible) {
        const bool terminate_owned =
            self->model->observe_health_failure(monotonic_milliseconds());
        if (terminate_owned) self->process->terminate([] {});
        return;
      }
      self->model->observe_health(std::move(*health), monotonic_milliseconds());
    } else {
      const bool terminate_owned =
          self->model->observe_health_failure(monotonic_milliseconds());
      if (terminate_owned) self->process->terminate([] {});
    }
  });
}

void refresh_pairings(AppState& app) {
  if (app.pairings_loading) return;
  app.pairings_loading = true;
  AppState* self = &app;
  app.process->list_pairings(
      [self](std::vector<std::string> origins, std::string error) {
        self->pairings = std::move(origins);
        self->pairing_error = std::move(error);
        self->pairings_loading = false;
        self->pairings_fetched_ever = true;
        self->pairings_fetched_at = std::chrono::steady_clock::now();
      });
}

void refresh_pairings_if_stale(AppState& app) {
  constexpr std::chrono::seconds kPairingCache{5};
  if (app.pairings_fetched_ever &&
      std::chrono::steady_clock::now() - app.pairings_fetched_at <
          kPairingCache) {
    return;
  }
  refresh_pairings(app);
}

void restart_sync(AppState& app) {
  app.model->manual_restart();
  if (app.process->owned_pid().has_value()) {
    app.expected_exit = true;
    AppState* self = &app;
    app.process->terminate([self] {
      if (self->quitting) return;
      start_helper(*self);
    });
  } else {
    probe_then_start(app);
  }
}

HMENU build_pairings_submenu(AppState& app) {
  HMENU menu = ::CreatePopupMenu();
  if (app.pairings_loading && !app.pairings_fetched_ever) {
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Loading…");
  } else if (!app.pairing_error.empty()) {
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                 to_wide(app.pairing_error).c_str());
  } else if (app.pairings.empty()) {
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No paired apps");
  } else {
    const std::size_t count =
        std::min(app.pairings.size(), kMaximumPairingMenuEntries);
    for (std::size_t index = 0; index < count; ++index) {
      ::AppendMenuW(menu, MF_STRING,
                   kCommandPairingsBase + static_cast<UINT>(index),
                   to_wide(app.pairings[index]).c_str());
    }
  }
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCommandPairingsRefresh, L"Refresh");
  return menu;
}

void show_context_menu(AppState& app) {
  refresh_pairings_if_stale(app);

  HMENU menu = ::CreatePopupMenu();
  const std::wstring status_title =
      L"Sync — " +
      to_wide(companion::state_name(app.model->state()));
  ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status_title.c_str());

  const companion::HealthSnapshot& health = app.model->health();
  std::wstring provider_summary = L"Unknown";
  if (health.reachable) {
    const std::string summary = health.providers.summary();
    provider_summary = summary.empty() ? L"None available" : to_wide(summary);
  }
  ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
               (L"Providers: " + provider_summary).c_str());

  const std::wstring senders =
      health.active_senders.has_value()
          ? std::to_wstring(*health.active_senders)
          : L"Unavailable";
  ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
               (L"Active senders: " + senders).c_str());
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

  UINT restart_flags = MF_STRING;
  if (app.model->state() == companion::CompanionState::External) {
    restart_flags |= MF_GRAYED;
  }
  ::AppendMenuW(menu, restart_flags, kCommandRestart, L"Restart Sync");

  HMENU pairings_menu = build_pairings_submenu(app);
  ::AppendMenuW(menu, MF_POPUP,
               reinterpret_cast<UINT_PTR>(pairings_menu), L"Pairings");

  ::AppendMenuW(menu, MF_STRING, kCommandCopyDiagnostics, L"Copy Diagnostics");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCommandQuit, L"Quit Sync");

  POINT cursor{};
  ::GetCursorPos(&cursor);
  // Required so the popup menu dismisses correctly when the user clicks
  // away from it -- the standard Shell_NotifyIconW tray menu incantation.
  ::SetForegroundWindow(app.hwnd);
  ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y,
                   0, app.hwnd, nullptr);
  ::PostMessageW(app.hwnd, WM_NULL, 0, 0);
  ::DestroyMenu(menu);  // also destroys the attached pairings submenu
}

void handle_command(AppState& app, UINT command) {
  if (command == kCommandRestart) {
    restart_sync(app);
  } else if (command == kCommandCopyDiagnostics) {
    copy_to_clipboard(app.hwnd, to_wide(app.model->diagnostics()));
  } else if (command == kCommandPairingsRefresh) {
    app.pairings_fetched_ever = false;  // force refresh even if "fresh"
    refresh_pairings(app);
  } else if (command == kCommandQuit) {
    app.quitting = true;
    app.model->cancel_recovery();
    ::KillTimer(app.hwnd, kPollTimerId);
    ::KillTimer(app.hwnd, kRecoveryTimerId);
    if (!app.process->owned_pid().has_value()) {
      ::PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
      return;
    }
    app.expected_exit = true;
    HWND hwnd = app.hwnd;
    app.process->terminate([hwnd] { ::PostMessageW(hwnd, WM_CLOSE, 0, 0); });
  } else if (command >= kCommandPairingsBase &&
            command < kCommandPairingsBase + kMaximumPairingMenuEntries) {
    const std::size_t index = command - kCommandPairingsBase;
    if (index >= app.pairings.size()) return;
    const std::string origin = app.pairings[index];
    const std::wstring prompt =
        to_wide(origin) + L" will need your approval before it can use "
                          L"Sync again.";
    if (::MessageBoxW(app.hwnd, prompt.c_str(), L"Revoke this pairing?",
                      MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
      return;
    }
    AppState* self = &app;
    app.process->revoke_pairing(
        origin, [self](bool revoked, std::string error) {
          if (!revoked) {
            ::MessageBoxW(self->hwnd, to_wide(error).c_str(),
                         L"Could not revoke pairing", MB_OK | MB_ICONERROR);
          }
          self->pairings_fetched_ever = false;
          refresh_pairings(*self);
        });
  }
}

void show_preview_notice_if_needed(HWND owner) {
  if (preview_notice_shown()) return;
  mark_preview_notice_shown();
  // Wording matches the macOS app's first-run notice (see showPreviewNotice
  // IfNeeded in native/src/platform/macos/app_main.mm).
  ::MessageBoxW(
      owner,
      L"Sync is not ready for general use. Expect rough edges, dropped "
      L"frames, and protocol changes while we validate the bridge.",
      L"Sync is a preview", MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK window_procedure(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
  auto* app =
      reinterpret_cast<AppState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (message) {
  case kMessageDispatch: {
    // CompanionProcess::CompanionProcessOptions::dispatch_to_owner target:
    // a heap-allocated std::function<void()> posted from a background
    // thread, executed here on the thread that owns this window, then
    // freed. See the comment on that option for the correctness argument
    // this depends on.
    auto* callback = reinterpret_cast<std::function<void()>*>(lparam);
    if (callback != nullptr) {
      (*callback)();
      delete callback;
    }
    return 0;
  }
  case kMessageTrayIcon:
    if (app != nullptr &&
        (lparam == WM_RBUTTONUP || lparam == WM_LBUTTONUP ||
         lparam == WM_CONTEXTMENU)) {
      show_context_menu(*app);
    }
    return 0;
  case WM_COMMAND:
    if (app != nullptr) handle_command(*app, LOWORD(wparam));
    return 0;
  case WM_TIMER:
    if (app == nullptr) return 0;
    if (wparam == kPollTimerId) {
      poll_status(*app);
    } else if (wparam == kRecoveryTimerId) {
      ::KillTimer(hwnd, kRecoveryTimerId);
      if (!app->pending_recovery.has_value()) return 0;
      const companion::RecoverySchedule pending = *app->pending_recovery;
      app->pending_recovery.reset();
      const std::uint64_t observed_at = monotonic_milliseconds();
      if (observed_at < pending.due_ms) {
        schedule_recovery(*app, pending);
        return 0;
      }
      if (!app->model->begin_recovery_attempt(pending, observed_at)) return 0;
      preflight_recovery(*app, pending.generation);
    }
    return 0;
  case WM_CLOSE: {
    NOTIFYICONDATAW icon_data{};
    icon_data.cbSize = sizeof(icon_data);
    icon_data.hWnd = hwnd;
    icon_data.uID = kTrayIconId;
    ::Shell_NotifyIconW(NIM_DELETE, &icon_data);
    ::DestroyWindow(hwnd);
    return 0;
  }
  case WM_DESTROY:
    ::PostQuitMessage(0);
    return 0;
  default:
    break;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

// The real body. The entry point that reaches it is toolchain-specific --
// see the bottom of this file.
int run_tray_application() {
  // Single instance: a named mutex, so a second launch can detect the
  // existing instance and surface it instead of starting a second syncd.exe
  // that would fight the first one for port 53979.
  HANDLE single_instance =
      ::CreateMutexW(nullptr, TRUE, L"NoiseFactorSync.SingleInstance");
  const bool already_running =
      single_instance != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS;
  if (already_running) {
    ::MessageBoxW(nullptr, L"Sync is already running.", L"Sync",
                 MB_OK | MB_ICONINFORMATION);
    if (single_instance != nullptr) ::CloseHandle(single_instance);
    return 0;
  }

  const HINSTANCE instance = ::GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &window_procedure;
  window_class.hInstance = instance;
  window_class.lpszClassName = L"NoiseFactorSyncTrayWindow";
  ::RegisterClassExW(&window_class);

  // An ordinary top-level window that is simply never shown (ShowWindow is
  // never called on it): this is the classic, maximally-compatible way to
  // own a Shell_NotifyIconW tray icon. It exists solely to receive the tray
  // icon's callback message, menu commands, timer ticks, and the
  // dispatch_to_owner marshaling messages posted from CompanionProcess's
  // background threads.
  HWND hwnd = ::CreateWindowExW(
      0, window_class.lpszClassName, L"Sync", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0,
      nullptr, nullptr, instance, nullptr);
  if (hwnd == nullptr) {
    if (single_instance != nullptr) ::CloseHandle(single_instance);
    return 1;
  }

  AppState app;
  app.hwnd = hwnd;
  ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));

  app.model = std::make_unique<companion::CompanionModel>(
      std::string(noisefactor::sync::kProductVersion));

  wchar_t module_path_buffer[MAX_PATH]{};
  ::GetModuleFileNameW(nullptr, module_path_buffer, MAX_PATH);
  const std::string helper_path =
      companion::resolve_helper_path(to_utf8(module_path_buffer));
  const std::wstring wide_helper_path = to_wide(helper_path);
  const std::wstring spout_library_path =
      wide_helper_path.substr(0, wide_helper_path.find_last_of(L"\\/") + 1) +
      L"SpoutLibrary.dll";

  companion::CompanionProcessOptions options;
  options.helper_path = wide_helper_path;
  options.spout_library_path = spout_library_path;
  options.dispatch_to_owner = [hwnd](std::function<void()> callback) {
    auto* heap_callback = new std::function<void()>(std::move(callback));
    if (::PostMessageW(hwnd, kMessageDispatch, 0,
                       reinterpret_cast<LPARAM>(heap_callback)) == 0) {
      // The window is already gone (e.g. this fires during/after
      // teardown): run inline rather than silently dropping the callback.
      // CompanionProcess's destructor waits for every dispatch() to
      // eventually execute (see its drain_operations()), and this is what
      // makes that wait bounded instead of a potential hang.
      (*heap_callback)();
      delete heap_callback;
    }
  };
  app.process = std::make_unique<companion::CompanionProcess>(options);

  // The icon resource is embedded only when the build machine had ImageMagick
  // to rasterise packaging/Sync.svg, so a developer build without it still
  // runs -- with the stock application icon rather than no tray icon at all.
  app.icon = ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_SYNC_APP));
  if (app.icon == nullptr) {
    app.icon = ::LoadIconW(nullptr, IDI_APPLICATION);
  }
  NOTIFYICONDATAW icon_data{};
  icon_data.cbSize = sizeof(icon_data);
  icon_data.hWnd = hwnd;
  icon_data.uID = kTrayIconId;
  icon_data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  icon_data.uCallbackMessage = kMessageTrayIcon;
  icon_data.hIcon = app.icon;
  ::wcsncpy_s(icon_data.szTip, L"Sync Preview", _TRUNCATE);
  ::Shell_NotifyIconW(NIM_ADD, &icon_data);

  probe_then_start(app);
  ::SetTimer(hwnd, kPollTimerId, 1000, nullptr);
  refresh_pairings(app);
  show_preview_notice_if_needed(hwnd);

  MSG message{};
  while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }

  // The window (and therefore any further dispatch_to_owner posting) is
  // already gone by the time we get here, so CompanionProcess's destructor
  // below -- which may still be waiting on background work via
  // drain_operations() -- resolves through that dispatcher's inline
  // fallback rather than through this (now-defunct) message loop.
  app.process.reset();
  app.model.reset();
  if (single_instance != nullptr) ::CloseHandle(single_instance);
  return static_cast<int>(message.wParam);
}

// add_executable(sync_menu WIN32 ...) selects the WINDOWS subsystem so no
// console window ever appears. The two toolchains disagree about which symbol
// starts a program there: the MSVC CRT looks for wWinMain and fails to link
// with an unresolved WinMain if only main() exists, while MinGW supplies a
// shim that calls plain main(). Define whichever this toolchain needs; both
// do nothing but call the same body.
#if defined(_MSC_VER)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  return run_tray_application();
}
#else
int main() { return run_tray_application(); }
#endif
