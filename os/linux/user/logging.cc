/* Copyright 2014 Peter Goodman, all rights reserved. */

// TODO(pag): Issue #1: Refactor this code to use an output stream.

#define GRANARY_INTERNAL

#include "granary/base/base.h"
#include "granary/base/lock.h"
#include "granary/base/option.h"
#include "granary/base/string.h"

#include "os/logging.h"
#include "os/lock.h"

GRANARY_DEFINE_string(output_log_file, "/dev/stdout",
    "The log file used by Granary for otuputting messages to "
    "`os::LogLevel::LogOutput`. The default value is `/dev/stdout`.");

GRANARY_DEFINE_string(debug_log_file, "/dev/stderr",
    "The log file used by Granary for outputting messages to "
    "`os::LogLevel::LogDebug`. The default value is `/dev/stderr`.");

extern "C" {

enum {
  kLogBufferSize = 32768 << 5,
  kLogBufferSafeSize = kLogBufferSize - 4096
};

char granary_log_buffer[kLogBufferSize] = {'\0'};
unsigned long granary_log_buffer_index = 0;

extern int open(const char * __file, int __oflag, ...);
extern ssize_t write(int __fd, const void * __buf, size_t __n);

#define O_WRONLY 01
#define O_CREAT 0100
#define O_APPEND 02000

}  // extern C
namespace granary {
namespace os {
namespace {

static int OUTPUT_FD[] = {
  -1,  // LogOutput; goes to `/dev/stdout`.
  -1,  // LogDebug; goes to `/dev/stderr`.
  -1
};

static os::Lock log_buffer_lock;
static int log_buffer_fd = -1;

}  // namespace

// Initialize the logging mechanism.
void InitLog(void) {
  OUTPUT_FD[LogLevel::LogOutput] = open(
      FLAG_output_log_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
  OUTPUT_FD[LogLevel::LogDebug] = open(
      FLAG_debug_log_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

// Exit the log.
void ExitLog(void) {
  os::LockedRegion locker(&log_buffer_lock);
  if (granary_log_buffer_index && -1 != log_buffer_fd) {
    write(log_buffer_fd, granary_log_buffer, granary_log_buffer_index);
    granary_log_buffer_index = 0;
    log_buffer_fd = -1;
  }
}

// Log something.
size_t Log(LogLevel level, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const auto fd = OUTPUT_FD[level];

  os::LockedRegion locker(&log_buffer_lock);

  // Flush the buffer.
  if (granary_log_buffer_index &&
      (granary_log_buffer_index >= kLogBufferSafeSize || log_buffer_fd != fd)) {
    write(fd, granary_log_buffer, granary_log_buffer_index);
    granary_log_buffer_index = 0;
    granary_log_buffer[0] = '\0';
  }

  // Fill the buffer.
  auto ret = VFormat(&(granary_log_buffer[granary_log_buffer_index]),
                     sizeof granary_log_buffer - granary_log_buffer_index - 1,
                     format, args);

  granary_log_buffer_index += ret;
  log_buffer_fd = fd;

  va_end(args);
  return ret;
}

}  // namespace os
}  // namespace granary
