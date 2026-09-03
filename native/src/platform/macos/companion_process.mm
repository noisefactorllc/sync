#include "companion_process.hpp"

#import <Foundation/Foundation.h>

#include <sync/origin.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <mutex>
#include <set>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

namespace noisefactor::sync::companion {
namespace {

NSString* ns_string(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data()
                                  length:value.size()
                                encoding:NSUTF8StringEncoding];
}

std::string cpp_string(NSString* value) {
  if (value == nil) return {};
  const char* bytes = value.UTF8String;
  return bytes == nullptr ? std::string{} : std::string(bytes);
}

void on_main(std::function<void()> callback) {
  dispatch_async(dispatch_get_main_queue(), ^{
    callback();
  });
}

bool exact_keys(NSDictionary* dictionary, NSSet<NSString*>* expected) {
  if (dictionary == nil || dictionary.count != expected.count) return false;
  for (id key in dictionary) {
    if (![key isKindOfClass:NSString.class] || ![expected containsObject:key]) {
      return false;
    }
  }
  return true;
}

std::optional<HealthSnapshot> parse_health(NSData* data,
                                           bool require_sender_count) {
  if (data == nil || data.length == 0 || data.length > 65'536) return std::nullopt;
  NSError* error = nil;
  id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
  if (error != nil || ![object isKindOfClass:NSDictionary.class]) {
    return std::nullopt;
  }
  NSDictionary* dictionary = object;
  NSString* product = dictionary[@"product"];
  NSString* status = dictionary[@"status"];
  NSString* version = dictionary[@"version"];
  NSArray* protocols = dictionary[@"protocolVersions"];
  NSDictionary* capabilities = dictionary[@"capabilities"];
  if (![product isEqualToString:@"Sync"] || ![status isEqualToString:@"ok"] ||
      ![version isKindOfClass:NSString.class] ||
      ![protocols isKindOfClass:NSArray.class] ||
      ![capabilities isKindOfClass:NSDictionary.class]) {
    return std::nullopt;
  }
  bool protocol_v1 = false;
  for (id value in protocols) {
    if ([value isKindOfClass:NSNumber.class] && [value integerValue] == 1) {
      protocol_v1 = true;
    }
  }
  NSArray* providers = capabilities[@"providers"];
  if (!protocol_v1 || ![providers isKindOfClass:NSArray.class]) {
    return std::nullopt;
  }
  // Sync publishes through every available send provider at once, so the
  // companion reports the whole set rather than probing for one id.
  AvailableProviders available_providers;
  for (id value in providers) {
    if (![value isKindOfClass:NSDictionary.class]) continue;
    NSDictionary* provider = value;
    NSString* identifier = provider[@"id"];
    // available AND selected: the companion reports the providers that are
    // actually publishing, which is the set the operator can go looking for
    // in a receiving application.
    if (![identifier isKindOfClass:NSString.class] ||
        ![provider[@"direction"] isEqualToString:@"send"] ||
        ![provider[@"available"] isKindOfClass:NSNumber.class] ||
        ![provider[@"available"] boolValue] ||
        ![provider[@"selected"] isKindOfClass:NSNumber.class] ||
        ![provider[@"selected"] boolValue]) {
      continue;
    }
    const std::string identifier_bytes = cpp_string(identifier);
    if (!available_providers.add(identifier_bytes)) {
      // A helper advertising more providers than this build bounds for is
      // reporting something this companion cannot represent truthfully.
      return std::nullopt;
    }
  }

  std::optional<std::size_t> active_senders;
  id count = dictionary[@"activeSenders"];
  if (count != nil) {
    if (![count isKindOfClass:NSNumber.class]) return std::nullopt;
    const double numeric = [count doubleValue];
    if (!std::isfinite(numeric) || numeric < 0 || numeric > 64 ||
        std::floor(numeric) != numeric) {
      return std::nullopt;
    }
    active_senders = static_cast<std::size_t>(numeric);
  } else if (require_sender_count) {
    return std::nullopt;
  }

  return HealthSnapshot{
      .reachable = true,
      .compatible = true,
      .product = cpp_string(product),
      .version = cpp_string(version),
      .providers = available_providers,
      .active_senders = active_senders,
  };
}

std::optional<HealthSnapshot> classify_http_health(NSHTTPURLResponse* response,
                                                   NSData* data,
                                                   bool require_sender_count) {
  if (response == nil) return std::nullopt;
  if (response.statusCode == 200) {
    if (auto health = parse_health(data, require_sender_count);
        health.has_value()) {
      return health;
    }
  }
  return HealthSnapshot{
      .reachable = true,
      .compatible = false,
  };
}

bool tcp_listener_reachable(std::string_view endpoint,
                            double timeout_seconds) {
  NSURLComponents* components =
      [NSURLComponents componentsWithString:ns_string(endpoint)];
  if (components == nil || components.host == nil) return false;
  const std::string host = cpp_string(components.host);
  const std::string service =
      std::to_string(components.port == nil ? 80 : components.port.intValue);
  if (host.empty()) return false;

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* addresses = nullptr;
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0) {
    return false;
  }
  const int timeout_ms = std::clamp(
      static_cast<int>(std::ceil(timeout_seconds * 1000.0)), 1, 250);
  bool reachable = false;
  for (addrinfo* address = addresses; address != nullptr;
       address = address->ai_next) {
    const int descriptor =
        ::socket(address->ai_family, address->ai_socktype,
                 address->ai_protocol);
    if (descriptor < 0) continue;
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags >= 0) (void)::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
    const int connected =
        ::connect(descriptor, address->ai_addr, address->ai_addrlen);
    if (connected == 0) {
      reachable = true;
    } else if (errno == EINPROGRESS) {
      pollfd poll_descriptor{
          .fd = descriptor,
          .events = POLLOUT,
          .revents = 0,
      };
      const int polled = ::poll(&poll_descriptor, 1, timeout_ms);
      if (polled > 0) {
        int socket_error = 0;
        socklen_t error_size = sizeof(socket_error);
        reachable =
            ::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error,
                         &error_size) == 0 &&
            socket_error == 0;
      }
    }
    ::close(descriptor);
    if (reachable) break;
  }
  ::freeaddrinfo(addresses);
  return reachable;
}

std::optional<HealthSnapshot> classify_probe_result(
    NSHTTPURLResponse* response, NSData* data, bool require_sender_count,
    std::string_view endpoint, double timeout_seconds) {
  if (auto health =
          classify_http_health(response, data, require_sender_count);
      health.has_value()) {
    return health;
  }
  if (!tcp_listener_reachable(endpoint, timeout_seconds)) return std::nullopt;
  return HealthSnapshot{
      .reachable = true,
      .compatible = false,
  };
}

struct ManagementState {
  std::atomic_bool completed{false};
  __strong NSTask* task = nil;
};

constexpr std::size_t kManagementOutputLimit = 65'536;

std::string drain_bounded(NSFileHandle* handle, std::size_t limit) {
  std::string output;
  output.reserve(limit);
  while (true) {
    @autoreleasepool {
      NSData* data = [handle readDataOfLength:8192];
      if (data.length == 0) break;
      const std::size_t available = limit - output.size();
      const std::size_t copied = std::min<std::size_t>(available, data.length);
      output.append(static_cast<const char*>(data.bytes), copied);
    }
  }
  return output;
}

struct OwnedTaskState {
  __strong NSTask* task = nil;
  __strong NSPipe* stderr_pipe = nil;
  __strong NSPipe* stdout_pipe = nil;
  CompanionProcess::StderrCallback stderr_callback;
  CompanionProcess::ExitCallback exit_callback;
  std::vector<std::function<void()>> termination_completions;
  bool termination_requested = false;
};

} // namespace

std::optional<HealthSnapshot> parse_health_json(std::string_view json,
                                                bool require_sender_count) {
  if (json.size() > 65'536) return std::nullopt;
  @autoreleasepool {
    NSData* data = [NSData dataWithBytes:json.data() length:json.size()];
    return parse_health(data, require_sender_count);
  }
}

PairingsResult parse_pairings_json(std::string_view json) {
  PairingsResult result;
  NSData* data = [NSData dataWithBytes:json.data() length:json.size()];
  NSError* error = nil;
  id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
  if (error != nil || ![object isKindOfClass:NSDictionary.class]) {
    result.error = "Sync returned malformed pairing data.";
    return result;
  }
  NSDictionary* dictionary = object;
  NSSet<NSString*>* keys = [NSSet setWithObjects:@"type", @"origins", nil];
  NSArray* origins = dictionary[@"origins"];
  if (!exact_keys(dictionary, keys) ||
      ![dictionary[@"type"] isEqualToString:@"pairings"] ||
      ![origins isKindOfClass:NSArray.class] || origins.count > 64) {
    result.error = "Sync returned malformed pairing data.";
    return result;
  }

  std::set<std::string> unique;
  for (id value in origins) {
    if (![value isKindOfClass:NSString.class]) {
      result.error = "Sync returned malformed pairing data.";
      result.origins.clear();
      return result;
    }
    const std::string origin = cpp_string(value);
    const auto normalized = normalize_origin(origin);
    if (!normalized.ok() || normalized.origin.view() != origin ||
        !unique.insert(origin).second) {
      result.error = "Sync returned noncanonical pairing data.";
      result.origins.clear();
      return result;
    }
    result.origins.push_back(origin);
  }
  return result;
}

RevocationResult classify_revocation(int exit_status, std::string_view json) {
  RevocationResult result;
  if (exit_status != 0 && exit_status != 3) {
    result.error = "Could not revoke Sync pairing.";
    return result;
  }
  NSError* error = nil;
  NSData* data = [NSData dataWithBytes:json.data() length:json.size()];
  id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
  NSSet<NSString*>* keys =
      [NSSet setWithObjects:@"type", @"origin", @"status", nil];
  NSString* status = [object isKindOfClass:NSDictionary.class]
                         ? object[@"status"]
                         : nil;
  if (error != nil || ![object isKindOfClass:NSDictionary.class] ||
      !exact_keys(object, keys) ||
      ![object[@"type"] isEqualToString:@"revocation"] ||
      ![object[@"origin"] isKindOfClass:NSString.class] ||
      ![status isKindOfClass:NSString.class]) {
    result.error = "Sync returned malformed revocation data.";
    return result;
  }
  if ([status isEqualToString:@"revoked_durability_uncertain"]) {
    result.error =
        "Sync removed the pairing but could not confirm it was written to "
        "disk. Revoke again after checking free disk space.";
    return result;
  }
  if (![status isEqualToString:@"revoked"] &&
      ![status isEqualToString:@"not_found"]) {
    result.error = "Sync returned malformed revocation data.";
    return result;
  }
  // Exit 0 with a well-formed record is the only durable outcome. `revoked`
  // means the origin is confirmed absent, which "not_found" also satisfies.
  if (exit_status != 0) {
    result.error = "Sync could not confirm the pairing was revoked.";
    return result;
  }
  result.revoked = true;
  return result;
}

struct CompanionProcess::Impl {
  explicit Impl(CompanionProcessOptions value) : options(std::move(value)) {}

  CompanionProcessOptions options;
  std::shared_ptr<OwnedTaskState> owned_state;
  // One session per endpoint for the life of the supervisor. The status poll
  // runs every second for as long as the menu app is open; a session, its
  // ephemeral stores, and its delegate queue are meant to be built once, not
  // eighty-six thousand times a day.
  __strong NSURLSession* status_session = nil;
  __strong NSURLSession* health_session = nil;

  NSURLSession* session_for(__strong NSURLSession*& slot, bool cache_policy) {
    if (slot != nil) return slot;
    NSURLSessionConfiguration* configuration =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.timeoutIntervalForRequest = options.health_timeout_seconds;
    configuration.timeoutIntervalForResource = options.health_timeout_seconds;
    if (cache_policy) {
      configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    }
    slot = [NSURLSession sessionWithConfiguration:configuration];
    return slot;
  }

  void run_management(std::vector<std::string> arguments,
                      std::function<void(int, std::string, std::string,
                                         bool)> completion) {
    auto state = std::make_shared<ManagementState>();
    NSTask* task = [[NSTask alloc] init];
    state->task = task;
    task.executableURL = [NSURL fileURLWithPath:ns_string(options.helper_path)];
    NSMutableArray<NSString*>* native_arguments = [NSMutableArray array];
    for (const std::string& argument : arguments) {
      [native_arguments addObject:ns_string(argument)];
    }
    task.arguments = native_arguments;
    NSPipe* output = [NSPipe pipe];
    NSPipe* error = [NSPipe pipe];
    task.standardOutput = output;
    task.standardError = error;

    NSError* launch_error = nil;
    if (![task launchAndReturnError:&launch_error]) {
      on_main([completion = std::move(completion),
               message = cpp_string(launch_error.localizedDescription)] {
        completion(-1, {}, message, false);
      });
      return;
    }

    auto stdout_capture = std::make_shared<std::string>();
    auto stderr_capture = std::make_shared<std::string>();
    const double termination_grace_seconds = options.termination_timeout_seconds;
    dispatch_queue_t queue =
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    dispatch_group_t group = dispatch_group_create();
    dispatch_group_async(group, queue, ^{
      *stdout_capture =
          drain_bounded(output.fileHandleForReading, kManagementOutputLimit);
    });
    dispatch_group_async(group, queue, ^{
      *stderr_capture =
          drain_bounded(error.fileHandleForReading, kManagementOutputLimit);
    });
    dispatch_group_async(group, queue, ^{
      [task waitUntilExit];
    });
    dispatch_group_notify(group, queue, ^{
      if (state->completed.exchange(true)) return;
      const int status = task.terminationStatus;
      std::string stdout_text = std::move(*stdout_capture);
      std::string stderr_text = std::move(*stderr_capture);
      on_main([completion, status, stdout_text = std::move(stdout_text),
               stderr_text = std::move(stderr_text)]() mutable {
        completion(status, std::move(stdout_text), std::move(stderr_text), false);
      });
    });

    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW,
                      static_cast<int64_t>(options.management_timeout_seconds *
                                           NSEC_PER_SEC)),
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
          if (state->completed.exchange(true)) return;
          // NSTask reaps the child itself, after which processIdentifier names
          // a pid the kernel may have reused. Ask NSTask to signal the process
          // it still owns rather than racing a raw kill against reuse.
          if (task.running) {
            [task terminate];
            // A helper that ignores SIGTERM would otherwise park the three
            // reader and waiter blocks above for good, and every later menu
            // open would park three more. The kill ends the child, which
            // closes its pipes, which lets the readers and the waiter return.
            const pid_t pid = task.processIdentifier;
            dispatch_after(
                dispatch_time(DISPATCH_TIME_NOW,
                              static_cast<int64_t>(termination_grace_seconds *
                                                   NSEC_PER_SEC)),
                dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                  if (task.running && task.processIdentifier == pid) {
                    ::kill(pid, SIGKILL);
                  }
                });
          }
          on_main([completion] { completion(-1, {}, {}, true); });
        });
  }
};

CompanionProcess::CompanionProcess(CompanionProcessOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

CompanionProcess::~CompanionProcess() {
  const auto state = impl_->owned_state;
  NSTask* task = state == nullptr ? nil : state->task;
  task.terminationHandler = nil;
  if (state != nullptr) {
    state->stderr_pipe.fileHandleForReading.readabilityHandler = nil;
    state->stdout_pipe.fileHandleForReading.readabilityHandler = nil;
  }
  if (task != nil && task.running) {
    ::kill(task.processIdentifier, SIGKILL);
  }
  task.standardError = nil;
  task.standardOutput = nil;
  // A probe still in flight completes with an error and posts to main; the
  // completion captures no supervisor state.
  [impl_->status_session invalidateAndCancel];
  [impl_->health_session invalidateAndCancel];
  impl_->status_session = nil;
  impl_->health_session = nil;
}

std::vector<std::string> CompanionProcess::launch_arguments() const {
  return {"--publisher", "syphon", "--publisher", "camera", "--syphon-framework",
          impl_->options.framework_path};
}

std::optional<int> CompanionProcess::owned_pid() const noexcept {
  const auto state = impl_->owned_state;
  NSTask* task = state == nullptr ? nil : state->task;
  if (task == nil || !task.running) return std::nullopt;
  return static_cast<int>(task.processIdentifier);
}

bool CompanionProcess::start(StderrCallback stderr_callback,
                             ExitCallback exit_callback,
                             std::string& error) {
  if (impl_->owned_state != nullptr) {
    error = "Sync helper is already running.";
    return false;
  }
  auto state = std::make_shared<OwnedTaskState>();
  NSTask* task = [[NSTask alloc] init];
  state->task = task;
  task.executableURL = [NSURL fileURLWithPath:ns_string(impl_->options.helper_path)];
  NSMutableArray<NSString*>* arguments = [NSMutableArray array];
  for (const std::string& argument : launch_arguments()) {
    [arguments addObject:ns_string(argument)];
  }
  task.arguments = arguments;
  state->stderr_pipe = [NSPipe pipe];
  state->stdout_pipe = [NSPipe pipe];
  task.standardError = state->stderr_pipe;
  task.standardOutput = state->stdout_pipe;
  state->stderr_callback = std::move(stderr_callback);
  state->exit_callback = std::move(exit_callback);

  Impl* process = impl_.get();
  // At end of file the read source stays readable forever, so Foundation
  // would call a handler that stays installed in a tight loop until the
  // termination handler removes it. Release it here as well.
  state->stderr_pipe.fileHandleForReading.readabilityHandler =
      ^(NSFileHandle* handle) {
        NSData* data = handle.availableData;
        if (data.length == 0) {
          handle.readabilityHandler = nil;
          return;
        }
        std::string bytes(static_cast<const char*>(data.bytes), data.length);
        on_main([state, bytes = std::move(bytes)] {
          if (state->stderr_callback) state->stderr_callback(bytes);
        });
      };
  state->stdout_pipe.fileHandleForReading.readabilityHandler =
      ^(NSFileHandle* handle) {
        if (handle.availableData.length == 0) handle.readabilityHandler = nil;
      };
  task.terminationHandler = ^(NSTask* terminated) {
    state->stderr_pipe.fileHandleForReading.readabilityHandler = nil;
    state->stdout_pipe.fileHandleForReading.readabilityHandler = nil;
    terminated.terminationHandler = nil;
    const int status = terminated.terminationStatus;
    on_main([process, state, status] {
      if (process->owned_state == state) process->owned_state.reset();
      if (state->exit_callback) {
        auto callback = std::move(state->exit_callback);
        callback(status);
      }
      auto completions = std::move(state->termination_completions);
      for (auto& completion : completions) completion();
    });
  };

  NSError* launch_error = nil;
  if (![task launchAndReturnError:&launch_error]) {
    state->stderr_pipe.fileHandleForReading.readabilityHandler = nil;
    state->stdout_pipe.fileHandleForReading.readabilityHandler = nil;
    task.terminationHandler = nil;
    error = cpp_string(launch_error.localizedDescription);
    return false;
  }
  impl_->owned_state = std::move(state);
  error.clear();
  return true;
}

void CompanionProcess::probe(ProbeCallback completion) {
  const std::string endpoint_base = impl_->options.endpoint;
  const double timeout_seconds = impl_->options.health_timeout_seconds;
  // Older daemons do not have /status. Probe it directly here and retry
  // /health only for the precise old-endpoint response.
  NSURLSession* session = impl_->session_for(impl_->status_session, true);
  NSURLSession* fallback_session = impl_->session_for(impl_->health_session, false);
  NSString* endpoint = ns_string(endpoint_base + "/status");
  NSMutableURLRequest* status_request =
      [NSMutableURLRequest requestWithURL:[NSURL URLWithString:endpoint]];
  status_request.HTTPMethod = @"GET";
  [[session dataTaskWithRequest:status_request
             completionHandler:^(NSData* data, NSURLResponse* response,
                                 NSError* error) {
    NSHTTPURLResponse* http =
        [response isKindOfClass:NSHTTPURLResponse.class]
            ? static_cast<NSHTTPURLResponse*>(response)
            : nil;
    if (error == nil && (http.statusCode == 400 || http.statusCode == 404)) {
      NSString* fallback_endpoint = ns_string(endpoint_base + "/health");
      NSMutableURLRequest* health_request = [NSMutableURLRequest
          requestWithURL:[NSURL URLWithString:fallback_endpoint]];
      health_request.HTTPMethod = @"GET";
      [[fallback_session dataTaskWithRequest:health_request
                           completionHandler:^(NSData* fallback_data,
                                               NSURLResponse* fallback_response,
                                               NSError* fallback_error) {
        NSHTTPURLResponse* fallback_http =
            [fallback_response isKindOfClass:NSHTTPURLResponse.class]
                ? static_cast<NSHTTPURLResponse*>(fallback_response)
                : nil;
        const auto health = classify_probe_result(
            fallback_http, fallback_data, false, endpoint_base,
            timeout_seconds);
        const std::string message =
            health.has_value() && !health->compatible
                ? "TCP 53979 is occupied by an incompatible service."
                : (fallback_error != nil
                       ? cpp_string(fallback_error.localizedDescription)
                       : (!health.has_value() ? "Sync health was unavailable."
                                              : std::string{}));
        on_main([completion, health, message] { completion(health, message); });
      }] resume];
      return;
    }
    const auto health = classify_probe_result(
        http, data, true, endpoint_base, timeout_seconds);
    const std::string message =
        health.has_value() && !health->compatible
            ? "TCP 53979 is occupied by an incompatible service."
            : (error != nil
                   ? cpp_string(error.localizedDescription)
                   : (!health.has_value() ? "Sync status was unavailable."
                                          : std::string{}));
    on_main([completion, health, message] { completion(health, message); });
  }] resume];
}

void CompanionProcess::terminate(Completion completion) {
  const auto state = impl_->owned_state;
  if (state == nullptr) {
    on_main(std::move(completion));
    return;
  }
  state->termination_completions.push_back(std::move(completion));
  if (state->termination_requested) return;
  state->termination_requested = true;
  NSTask* task = state->task;
  if (task == nil || !task.running) return;
  const pid_t pid = task.processIdentifier;
  [task terminate];
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW,
                    static_cast<int64_t>(impl_->options.termination_timeout_seconds *
                                         NSEC_PER_SEC)),
      dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        if (task.running && task.processIdentifier == pid) ::kill(pid, SIGKILL);
      });
}

void CompanionProcess::list_pairings(PairingsCallback completion) {
  impl_->run_management(
      {"--list-pairings"},
      [completion = std::move(completion)](int status, std::string output,
                                           std::string error, bool timed_out) {
        if (timed_out) {
          completion({}, "Sync management command timed out.");
          return;
        }
        if (status != 0 || output.size() > 65'536) {
          completion({}, error.empty() ? "Could not list Sync pairings." : error);
          return;
        }
        PairingsResult parsed = parse_pairings_json(output);
        completion(std::move(parsed.origins), std::move(parsed.error));
      });
}

void CompanionProcess::revoke_pairing(std::string origin,
                                      RevokeCallback completion) {
  const auto normalized = normalize_origin(origin);
  if (!normalized.ok() || normalized.origin.view() != origin) {
    on_main([completion = std::move(completion)] {
      completion(false, "Pairing origin is invalid.");
    });
    return;
  }
  impl_->run_management(
      {"--revoke-origin", std::move(origin)},
      [completion = std::move(completion)](int status, std::string output,
                                           std::string error, bool timed_out) {
        if (timed_out) {
          completion(false, "Sync management command timed out.");
          return;
        }
        if (status != 0 && status != 3 && !error.empty()) {
          completion(false, error);
          return;
        }
        RevocationResult parsed = classify_revocation(status, output);
        completion(parsed.revoked, std::move(parsed.error));
      });
}

} // namespace noisefactor::sync::companion
