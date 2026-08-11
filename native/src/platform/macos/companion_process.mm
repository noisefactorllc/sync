#include "companion_process.hpp"

#import <Foundation/Foundation.h>

#include <sync/origin.hpp>

#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <utility>

#include <signal.h>

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
  bool syphon_available = false;
  for (id value in providers) {
    if (![value isKindOfClass:NSDictionary.class]) continue;
    NSDictionary* provider = value;
    if ([provider[@"id"] isEqualToString:@"syphon"] &&
        [provider[@"direction"] isEqualToString:@"send"] &&
        [provider[@"available"] isKindOfClass:NSNumber.class] &&
        [provider[@"available"] boolValue]) {
      syphon_available = true;
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
      .syphon_available = syphon_available,
      .active_senders = active_senders,
  };
}

struct ManagementState {
  std::atomic_bool completed{false};
  __strong NSTask* task = nil;
};

} // namespace

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

struct CompanionProcess::Impl {
  explicit Impl(CompanionProcessOptions value) : options(std::move(value)) {}

  CompanionProcessOptions options;
  __strong NSTask* owned_task = nil;
  __strong NSPipe* stderr_pipe = nil;
  __strong NSPipe* stdout_pipe = nil;
  StderrCallback stderr_callback;
  ExitCallback exit_callback;

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

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      [task waitUntilExit];
      NSData* stdout_data = [output.fileHandleForReading readDataToEndOfFile];
      NSData* stderr_data = [error.fileHandleForReading readDataToEndOfFile];
      if (state->completed.exchange(true)) return;
      const int status = task.terminationStatus;
      std::string stdout_text(
          static_cast<const char*>(stdout_data.bytes), stdout_data.length);
      std::string stderr_text(
          static_cast<const char*>(stderr_data.bytes), stderr_data.length);
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
          if (task.running) {
            ::kill(task.processIdentifier, SIGKILL);
          }
          on_main([completion] { completion(-1, {}, {}, true); });
        });
  }
};

CompanionProcess::CompanionProcess(CompanionProcessOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

CompanionProcess::~CompanionProcess() {
  NSTask* task = impl_->owned_task;
  task.terminationHandler = nil;
  impl_->stderr_pipe.fileHandleForReading.readabilityHandler = nil;
  impl_->stdout_pipe.fileHandleForReading.readabilityHandler = nil;
  if (task != nil && task.running) {
    ::kill(task.processIdentifier, SIGKILL);
  }
  task.standardError = nil;
  task.standardOutput = nil;
}

std::vector<std::string> CompanionProcess::launch_arguments() const {
  return {"--publisher", "syphon", "--syphon-framework",
          impl_->options.framework_path};
}

std::optional<int> CompanionProcess::owned_pid() const noexcept {
  NSTask* task = impl_->owned_task;
  if (task == nil || !task.running) return std::nullopt;
  return static_cast<int>(task.processIdentifier);
}

bool CompanionProcess::start(StderrCallback stderr_callback,
                             ExitCallback exit_callback,
                             std::string& error) {
  if (owned_pid().has_value()) {
    error = "Sync helper is already running.";
    return false;
  }
  NSTask* task = [[NSTask alloc] init];
  task.executableURL = [NSURL fileURLWithPath:ns_string(impl_->options.helper_path)];
  NSMutableArray<NSString*>* arguments = [NSMutableArray array];
  for (const std::string& argument : launch_arguments()) {
    [arguments addObject:ns_string(argument)];
  }
  task.arguments = arguments;
  impl_->stderr_pipe = [NSPipe pipe];
  impl_->stdout_pipe = [NSPipe pipe];
  task.standardError = impl_->stderr_pipe;
  task.standardOutput = impl_->stdout_pipe;
  impl_->stderr_callback = std::move(stderr_callback);
  impl_->exit_callback = std::move(exit_callback);

  Impl* process = impl_.get();
  impl_->stderr_pipe.fileHandleForReading.readabilityHandler =
      ^(NSFileHandle* handle) {
        NSData* data = handle.availableData;
        if (data.length == 0) return;
        std::string bytes(static_cast<const char*>(data.bytes), data.length);
        on_main([process, bytes = std::move(bytes)] {
          if (process->stderr_callback) process->stderr_callback(bytes);
        });
      };
  impl_->stdout_pipe.fileHandleForReading.readabilityHandler =
      ^(NSFileHandle* handle) {
        (void)handle.availableData;
      };
  task.terminationHandler = ^(NSTask* terminated) {
    process->stderr_pipe.fileHandleForReading.readabilityHandler = nil;
    process->stdout_pipe.fileHandleForReading.readabilityHandler = nil;
    const int status = terminated.terminationStatus;
    on_main([process, terminated, status] {
      if (process->owned_task == terminated) process->owned_task = nil;
      if (process->exit_callback) process->exit_callback(status);
    });
  };

  NSError* launch_error = nil;
  if (![task launchAndReturnError:&launch_error]) {
    impl_->stderr_pipe.fileHandleForReading.readabilityHandler = nil;
    impl_->stdout_pipe.fileHandleForReading.readabilityHandler = nil;
    error = cpp_string(launch_error.localizedDescription);
    return false;
  }
  impl_->owned_task = task;
  error.clear();
  return true;
}

void CompanionProcess::probe(ProbeCallback completion) {
  const std::string endpoint_base = impl_->options.endpoint;
  const double timeout_seconds = impl_->options.health_timeout_seconds;
  // Older daemons do not have /status. Probe it directly here and retry
  // /health only for the precise old-endpoint response.
  NSURLSessionConfiguration* configuration =
      [NSURLSessionConfiguration ephemeralSessionConfiguration];
  configuration.timeoutIntervalForRequest = timeout_seconds;
  configuration.timeoutIntervalForResource = timeout_seconds;
  configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
  NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration];
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
      [session finishTasksAndInvalidate];
      NSURLSessionConfiguration* fallback_configuration =
          [NSURLSessionConfiguration ephemeralSessionConfiguration];
      fallback_configuration.timeoutIntervalForRequest = timeout_seconds;
      fallback_configuration.timeoutIntervalForResource = timeout_seconds;
      NSURLSession* fallback_session =
          [NSURLSession sessionWithConfiguration:fallback_configuration];
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
        const auto health = fallback_error == nil && fallback_http.statusCode == 200
                                ? parse_health(fallback_data, false)
                                : std::nullopt;
        const std::string message =
            fallback_error != nil
                ? cpp_string(fallback_error.localizedDescription)
                : (health.has_value()
                       ? std::string{}
                       : "Sync health was unavailable or invalid.");
        [fallback_session finishTasksAndInvalidate];
        on_main([completion, health, message] { completion(health, message); });
      }] resume];
      return;
    }
    const auto health = error == nil && http.statusCode == 200
                            ? parse_health(data, true)
                            : std::nullopt;
    const std::string message = error != nil
                                    ? cpp_string(error.localizedDescription)
                                    : (health.has_value()
                                           ? std::string{}
                                           : "Sync status was unavailable or invalid.");
    [session finishTasksAndInvalidate];
    on_main([completion, health, message] { completion(health, message); });
  }] resume];
}

void CompanionProcess::terminate(Completion completion) {
  NSTask* task = impl_->owned_task;
  if (task == nil || !task.running) {
    impl_->owned_task = nil;
    on_main(std::move(completion));
    return;
  }
  const pid_t pid = task.processIdentifier;
  [task terminate];
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW,
                    static_cast<int64_t>(impl_->options.termination_timeout_seconds *
                                         NSEC_PER_SEC)),
      dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        if (task.running && task.processIdentifier == pid) ::kill(pid, SIGKILL);
      });
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    [task waitUntilExit];
    on_main([this, task, completion = std::move(completion)] {
      if (impl_->owned_task == task) impl_->owned_task = nil;
      completion();
    });
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
        if ((status != 0 && status != 3) || output.size() > 65'536) {
          completion(false, error.empty() ? "Could not revoke Sync pairing."
                                          : error);
          return;
        }
        NSError* json_error = nil;
        NSData* data = [NSData dataWithBytes:output.data() length:output.size()];
        id object = [NSJSONSerialization JSONObjectWithData:data
                                                    options:0
                                                      error:&json_error];
        const bool valid = json_error == nil &&
                           [object isKindOfClass:NSDictionary.class] &&
                           [object[@"type"] isEqualToString:@"revocation"] &&
                           [object[@"origin"] isKindOfClass:NSString.class] &&
                           [object[@"status"] isKindOfClass:NSString.class];
        completion(valid, valid ? std::string{}
                                : "Sync returned malformed revocation data.");
      });
}

} // namespace noisefactor::sync::companion
