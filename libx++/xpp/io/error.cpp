/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error.cpp - Out-of-line bits of xpp::io::Error.
 *
 * Hides <errno.h> and x/base/error.h from the public header so
 * downstream TUs don't pull in the full POSIX errno surface every
 * time they include <xpp/io/error.h>.
 */

#include <xpp/io/error.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <x/base/error.h>
}

namespace xpp {
namespace io {

/* ── kind_name ──────────────────────────────────────────────────────── */

const char *kind_name(ErrorKind k) noexcept {
  switch (k) {
  case ErrorKind::_Niche:             return "_Niche"; // never user-visible
  case ErrorKind::NotFound:           return "NotFound";
  case ErrorKind::PermissionDenied:   return "PermissionDenied";
  case ErrorKind::ConnectionRefused:  return "ConnectionRefused";
  case ErrorKind::ConnectionReset:    return "ConnectionReset";
  case ErrorKind::ConnectionAborted:  return "ConnectionAborted";
  case ErrorKind::NotConnected:       return "NotConnected";
  case ErrorKind::AddrInUse:          return "AddrInUse";
  case ErrorKind::AddrNotAvailable:   return "AddrNotAvailable";
  case ErrorKind::NetworkDown:        return "NetworkDown";
  case ErrorKind::NetworkUnreachable: return "NetworkUnreachable";
  case ErrorKind::HostUnreachable:    return "HostUnreachable";
  case ErrorKind::BrokenPipe:         return "BrokenPipe";
  case ErrorKind::AlreadyExists:      return "AlreadyExists";
  case ErrorKind::WouldBlock:         return "WouldBlock";
  case ErrorKind::InvalidInput:       return "InvalidInput";
  case ErrorKind::InvalidData:        return "InvalidData";
  case ErrorKind::TimedOut:           return "TimedOut";
  case ErrorKind::WriteZero:          return "WriteZero";
  case ErrorKind::Interrupted:        return "Interrupted";
  case ErrorKind::Unsupported:        return "Unsupported";
  case ErrorKind::UnexpectedEof:      return "UnexpectedEof";
  case ErrorKind::OutOfMemory:        return "OutOfMemory";
  case ErrorKind::ResourceBusy:       return "ResourceBusy";
  case ErrorKind::ResolveFailed:      return "ResolveFailed";
  case ErrorKind::NoAddress:          return "NoAddress";
  case ErrorKind::Other:              return "Other";
  default:                            return "Unknown";
  }
}

/* ── errno → ErrorKind ─────────────────────────────────────────────── */

ErrorKind errno_to_kind(int e) noexcept {
  switch (e) {
  case ENOENT:        return ErrorKind::NotFound;
  case EACCES:
  case EPERM:         return ErrorKind::PermissionDenied;
  case ECONNREFUSED:  return ErrorKind::ConnectionRefused;
  case ECONNRESET:    return ErrorKind::ConnectionReset;
  case ECONNABORTED:  return ErrorKind::ConnectionAborted;
  case ENOTCONN:      return ErrorKind::NotConnected;
  case EADDRINUSE:    return ErrorKind::AddrInUse;
  case EADDRNOTAVAIL: return ErrorKind::AddrNotAvailable;
  case ENETDOWN:      return ErrorKind::NetworkDown;
  case ENETUNREACH:   return ErrorKind::NetworkUnreachable;
  case EHOSTUNREACH:  return ErrorKind::HostUnreachable;
  case EPIPE:         return ErrorKind::BrokenPipe;
  case EEXIST:        return ErrorKind::AlreadyExists;
#if defined(EAGAIN) && defined(EWOULDBLOCK) && EAGAIN != EWOULDBLOCK
  case EAGAIN:
  case EWOULDBLOCK: return ErrorKind::WouldBlock;
#else
  case EAGAIN: return ErrorKind::WouldBlock;
#endif
  case EINVAL:    return ErrorKind::InvalidInput;
  case ETIMEDOUT: return ErrorKind::TimedOut;
  case EINTR:     return ErrorKind::Interrupted;
  case ENOTSUP:   return ErrorKind::Unsupported;
  case ENOMEM:    return ErrorKind::OutOfMemory;
  case EBUSY:     return ErrorKind::ResourceBusy;
  default:        return ErrorKind::Other;
  }
}

/* ── xErrno → ErrorKind (internal helper, file-scope) ─────────────── */

/// Defined for future use by code that wants to bridge libx errors;
/// not currently called from this TU.  Kept static so unused-function
/// warnings can be suppressed cleanly if needed.
__attribute__((unused)) static ErrorKind xerrno_to_kind(xErrno e) noexcept {
  switch (e) {
  case xErrno_Ok:            return ErrorKind::Other; // shouldn't happen
  case xErrno_NotFound:      return ErrorKind::NotFound;
  case xErrno_AlreadyExists: return ErrorKind::AlreadyExists;
  case xErrno_InvalidArg:    return ErrorKind::InvalidInput;
  case xErrno_NoMemory:      return ErrorKind::OutOfMemory;
  case xErrno_InvalidState:  return ErrorKind::Other;
  case xErrno_Cancelled:     return ErrorKind::Interrupted;
  case xErrno_NotSupported:  return ErrorKind::Unsupported;
  case xErrno_DnsNotFound:   return ErrorKind::ResolveFailed;
  case xErrno_DnsTempFail:   return ErrorKind::ResolveFailed;
  case xErrno_DnsError:      return ErrorKind::ResolveFailed;
  case xErrno_Timeout:       return ErrorKind::TimedOut;
  case xErrno_Again:         return ErrorKind::WouldBlock;
  case xErrno_Busy:          return ErrorKind::ResourceBusy;
  case xErrno_Pending:       return ErrorKind::WouldBlock;
  case xErrno_PromptTooLong: return ErrorKind::InvalidInput;
  case xErrno_SysError:      return ErrorKind::Other;
  case xErrno_Unknown:
  default:                   return ErrorKind::Other;
  }
}

/* ── Custom payload allocator ──────────────────────────────────────── */

namespace _ {

Custom *custom_alloc(ErrorKind kind, const char *msg, size_t len) {
  // Layout: [Custom header (with 1-byte data[1])][len-1 more bytes][NUL]
  // Total = sizeof(Custom) - 1 + len + 1 = sizeof(Custom) + len.
  void   *raw = ::operator new(sizeof(Custom) + len);
  Custom *c   = static_cast<Custom *>(raw);
  c->kind     = kind;
  // Saturate len at uint32_t::max — error messages this long are
  // pathological; truncate rather than fail loudly.
  c->len = static_cast<uint32_t>(len > UINT32_MAX ? UINT32_MAX : len);
  if (msg && c->len > 0) {
    std::memcpy(c->data, msg, c->len);
  }
  c->data[c->len] = '\0';
  return c;
}

void custom_free(Custom *c) noexcept {
  ::operator delete(static_cast<void *>(c));
}

} // namespace _

/* ── to_string ─────────────────────────────────────────────────────── */

String Error::to_string() const {
  String    out;
  ErrorKind k = kind();
  out.append(kind_name(k));

  // Drive the formatting from the public accessors so this works on
  // both the 64-bit packed and the 32-bit tagged-union layouts.
  if (auto os = raw_os_error()) {
    char buf[40];
    int  n = std::snprintf(buf, sizeof(buf), " (os error %d)",
                          std::move(os).unwrap());
    if (n > 0) out.append(buf, static_cast<size_t>(n));
    return out;
  }
  if (const char *m = message()) {
    if (*m != '\0') {
      out.append(": ");
      out.append(m);
    }
  }
  return out;
}

} // namespace io
} // namespace xpp
