// Copyright 2022 The gRPC Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/core/lib/event_engine/posix_engine/ev_poll_posix.h"

#include <grpc/event_engine/event_engine.h>
#include <grpc/status.h>
#include <grpc/support/port_platform.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>
#include <stdint.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "absl/container/inlined_vector.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "src/core/lib/event_engine/poller.h"
#include "src/core/lib/event_engine/posix_engine/event_poller.h"
#include "src/core/lib/event_engine/posix_engine/posix_engine_closure.h"
#include "src/core/lib/event_engine/posix_engine/posix_interface.h"
#include "src/core/lib/iomgr/port.h"
#include "src/core/util/crash.h"

#ifdef GRPC_POSIX_SOCKET_EV_POLL

#include <errno.h>
#include <grpc/support/alloc.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "src/core/lib/event_engine/common_closures.h"
#include "src/core/lib/event_engine/posix_engine/wakeup_fd_posix.h"
#include "src/core/lib/event_engine/posix_engine/wakeup_fd_posix_default.h"
#include "src/core/lib/event_engine/time_util.h"
#include "src/core/util/status_helper.h"
#include "src/core/util/strerror.h"
#include "src/core/util/sync.h"
#include "src/core/util/time.h"

static const intptr_t kClosureNotReady = 0;
static const intptr_t kClosureReady = 1;
static const int kPollinCheck = POLLIN | POLLHUP | POLLERR;
static const int kPolloutCheck = POLLOUT | POLLHUP | POLLERR;

namespace grpc_event_engine::experimental {

using Events = absl::InlinedVector<PollEventHandle*, 5>;

class PollEventHandle : public EventHandle {
 public:
  PollEventHandle(FileDescriptor fd, std::shared_ptr<PollPoller> poller)
      : fd_(fd),
        pending_actions_(0),
        poller_handles_list_(this),
        scheduler_(poller->GetScheduler()),
        poller_(std::move(poller)),
        is_orphaned_(false),
        is_shutdown_(false),
        closed_(false),
        released_(false),
        pollhup_(false),
        watch_mask_(-1),
        shutdown_error_(absl::OkStatus()),
        exec_actions_closure_([this]() { ExecutePendingActions(); }),
        on_done_(nullptr),
        read_closure_(reinterpret_cast<PosixEngineClosure*>(kClosureNotReady)),
        write_closure_(
            reinterpret_cast<PosixEngineClosure*>(kClosureNotReady)) {
    grpc_core::MutexLock lock(&poller_->mu_);
    poller_->PollerHandlesListAddHandle(this);
  }
  PollPoller* Poller() override { return poller_.get(); }
  bool SetPendingActions(bool pending_read, bool pending_write) {
    pending_actions_ |= pending_read;
    if (pending_write) {
      pending_actions_ |= (1 << 2);
    }
    if (pending_read || pending_write) {
      // The closure is going to be executed. We'll Unref this handle in
      // ExecutePendingActions.
      Ref();
      return true;
    }
    return false;
  }
  void ForceRemoveHandleFromPoller() {
    grpc_core::MutexLock lock(&poller_->mu_);
    poller_->PollerHandlesListRemoveHandle(this);
  }
  FileDescriptor WrappedFd() override { return fd_; }
  bool IsOrphaned() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    return is_orphaned_;
  }
  void CloseFd() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    if (!released_ && !closed_) {
      closed_ = true;
      poller_->posix_interface().Close(fd_);
    }
  }
  bool IsPollhup() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) { return pollhup_; }
  void SetPollhup(bool pollhup) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    pollhup_ = pollhup;
  }
  bool IsWatched(int& watch_mask) const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    watch_mask = watch_mask_;
    return watch_mask_ != -1;
  }
  bool IsWatched() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    return watch_mask_ != -1;
  }
  void SetWatched(int watch_mask) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    watch_mask_ = watch_mask;
  }
  void OrphanHandle(PosixEngineClosure* on_done, FileDescriptor* release_fd,
                    absl::string_view reason) override;
  void ShutdownHandle(absl::Status why) override;
  void NotifyOnRead(PosixEngineClosure* on_read) override;
  void NotifyOnWrite(PosixEngineClosure* on_write) override;
  void NotifyOnError(PosixEngineClosure* on_error) override;
  void SetReadable() override;
  void SetWritable() override;
  void SetHasError() override;
  bool IsHandleShutdown() override {
    grpc_core::MutexLock lock(&mu_);
    return is_shutdown_;
  };
  inline void ExecutePendingActions() {
    int kick = 0;
    {
      grpc_core::MutexLock lock(&mu_);
      if ((pending_actions_ & 1UL)) {
        if (SetReadyLocked(&read_closure_)) {
          kick = 1;
        }
      }
      if (((pending_actions_ >> 2) & 1UL)) {
        if (SetReadyLocked(&write_closure_)) {
          kick = 1;
        }
      }
      pending_actions_ = 0;
    }
    if (kick) {
      // SetReadyLocked immediately scheduled some closure. It would have set
      // the closure state to NOT_READY. We need to wakeup the Work(...)
      // thread to start polling on this fd. If this call is not made, it is
      // possible that the poller will reach a state where all the fds under
      // the poller's control are not polled for POLLIN/POLLOUT events thus
      // leading to an indefinitely blocked Work(..) method.
      poller_->KickExternal(false);
    }
    Unref();
  }
  void Ref() { ref_count_.fetch_add(1, std::memory_order_relaxed); }
  void Unref() {
    if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (on_done_ != nullptr) {
        scheduler_->Run(on_done_);
      }
      delete this;
    }
  }
  ~PollEventHandle() override = default;
  grpc_core::Mutex* mu() ABSL_LOCK_RETURNED(mu_) { return &mu_; }
  PollPoller::HandlesList& PollerHandlesListPos() {
    return poller_handles_list_;
  }
  uint32_t BeginPollLocked(uint32_t read_mask, uint32_t write_mask)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  bool EndPollLocked(bool got_read, bool got_write)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

 private:
  int SetReadyLocked(PosixEngineClosure** st);
  int NotifyOnLocked(PosixEngineClosure** st, PosixEngineClosure* closure);
  // See Epoll1Poller::ShutdownHandle for explanation on why a mutex is
  // required.
  grpc_core::Mutex mu_;
  std::atomic<int> ref_count_{1};
  FileDescriptor fd_;
  int pending_actions_;
  PollPoller::HandlesList poller_handles_list_;
  Scheduler* scheduler_;
  std::shared_ptr<PollPoller> poller_;
  bool is_orphaned_;
  bool is_shutdown_;
  bool closed_;
  bool released_;
  bool pollhup_;
  int watch_mask_;
  absl::Status shutdown_error_;
  AnyInvocableClosure exec_actions_closure_;
  PosixEngineClosure* on_done_;
  PosixEngineClosure* read_closure_;
  PosixEngineClosure* write_closure_;
};

namespace {

// Returns the number of milliseconds elapsed between now and start timestamp.
int PollElapsedTimeToMillis(grpc_core::Timestamp start) {
  if (start == grpc_core::Timestamp::InfFuture()) return -1;
  grpc_core::Timestamp now =
      grpc_core::Timestamp::FromTimespecRoundDown(gpr_now(GPR_CLOCK_MONOTONIC));
  int64_t delta = (now - start).millis();
  if (delta > INT_MAX) {
    return INT_MAX;
  } else if (delta < 0) {
    return 0;
  } else {
    return static_cast<int>(delta);
  }
}

}  // namespace

EventHandle* PollPoller::CreateHandle(FileDescriptor fd,
                                      absl::string_view /*name*/,
                                      bool track_err) {
  // Avoid unused-parameter warning for debug-only parameter
  (void)track_err;
  DCHECK(track_err == false);
  PollEventHandle* handle = new PollEventHandle(fd, shared_from_this());
  // We need to send a kick to the thread executing Work(..) so that it can
  // add this new Fd into the list of Fds to poll.
  KickExternal(false);
  return handle;
}

void PollEventHandle::OrphanHandle(PosixEngineClosure* on_done,
                                   FileDescriptor* release_fd,
                                   absl::string_view /*reason*/) {
  ForceRemoveHandleFromPoller();
  {
    grpc_core::ReleasableMutexLock lock(&mu_);
    on_done_ = on_done;
    released_ = release_fd != nullptr;
    if (release_fd != nullptr) {
      *release_fd = fd_;
    }
    CHECK(!is_orphaned_);
    is_orphaned_ = true;
    // Perform shutdown operations if not already done so.
    if (!is_shutdown_) {
      is_shutdown_ = true;
      shutdown_error_ =
          absl::Status(absl::StatusCode::kInternal, "FD Orphaned");
      grpc_core::StatusSetInt(&shutdown_error_,
                              grpc_core::StatusIntProperty::kRpcStatus,
                              GRPC_STATUS_UNAVAILABLE);
      SetReadyLocked(&read_closure_);
      SetReadyLocked(&write_closure_);
    }
    // signal read/write closed to OS so that future operations fail.
    if (!released_) {
      poller_->posix_interface().Shutdown(fd_, SHUT_RDWR);
    }
    if (!IsWatched()) {
      CloseFd();
    } else {
      // It is watched i.e we cannot take action without breaking from the
      // blocking poll. Mark it as Unwatched and kick the thread executing
      // Work(...). That thread should proceed with the cleanup.
      SetWatched(-1);
      lock.Release();
      poller_->KickExternal(false);
    }
  }
  Unref();
}

int PollEventHandle::NotifyOnLocked(PosixEngineClosure** st,
                                    PosixEngineClosure* closure) {
  if (is_shutdown_ || pollhup_) {
    closure->SetStatus(shutdown_error_);
    scheduler_->Run(closure);
  } else if (*st == reinterpret_cast<PosixEngineClosure*>(kClosureNotReady)) {
    // not ready ==> switch to a waiting state by setting the closure
    *st = closure;
    return 0;
  } else if (*st == reinterpret_cast<PosixEngineClosure*>(kClosureReady)) {
    // already ready ==> queue the closure to run immediately
    *st = reinterpret_cast<PosixEngineClosure*>(kClosureNotReady);
    closure->SetStatus(shutdown_error_);
    scheduler_->Run(closure);
    return 1;
  } else {
    // upcallptr was set to a different closure.  This is an error!
    grpc_core::Crash(
        "User called a notify_on function with a previous callback still "
        "pending");
  }
  return 0;
}

// returns 1 if state becomes not ready
int PollEventHandle::SetReadyLocked(PosixEngineClosure** st) {
  if (*st == reinterpret_cast<PosixEngineClosure*>(kClosureReady)) {
    // duplicate ready ==> ignore
    return 0;
  } else if (*st == reinterpret_cast<PosixEngineClosure*>(kClosureNotReady)) {
    // not ready, and not waiting ==> flag ready
    *st = reinterpret_cast<PosixEngineClosure*>(kClosureReady);
    return 0;
  } else {
    // waiting ==> queue closure
    PosixEngineClosure* closure = *st;
    *st = reinterpret_cast<PosixEngineClosure*>(kClosureNotReady);
    closure->SetStatus(shutdown_error_);
    scheduler_->Run(closure);
    return 1;
  }
}

void PollEventHandle::ShutdownHandle(absl::Status why) {
  // We need to take a Ref here because SetReadyLocked may trigger execution
  // of a closure which calls OrphanHandle or poller->Shutdown() prematurely.
  Ref();
  {
    grpc_core::MutexLock lock(&mu_);
    // only shutdown once
    if (!is_shutdown_) {
      is_shutdown_ = true;
      shutdown_error_ = std::move(why);
      grpc_core::StatusSetInt(
          &shutdown_error_, grpc_core::StatusIntProperty::kRpcStatus,
          absl::IsCancelled(shutdown_error_) ? GRPC_STATUS_CANCELLED
                                             : GRPC_STATUS_UNAVAILABLE);
      SetReadyLocked(&read_closure_);
      SetReadyLocked(&write_closure_);
    }
  }
  // For the Ref() taken at the beginning of this function.
  Unref();
}

void PollEventHandle::NotifyOnRead(PosixEngineClosure* on_read) {
  // We need to take a Ref here because NotifyOnLocked may trigger execution
  // of a closure which calls OrphanHandle that may delete this object or call
  // poller->Shutdown() prematurely.
  Ref();
  {
    grpc_core::ReleasableMutexLock lock(&mu_);
    if (NotifyOnLocked(&read_closure_, on_read)) {
      lock.Release();
      // NotifyOnLocked immediately scheduled some closure. It would have set
      // the closure state to NOT_READY. We need to wakeup the Work(...) thread
      // to start polling on this fd. If this call is not made, it is possible
      // that the poller will reach a state where all the fds under the
      // poller's control are not polled for POLLIN/POLLOUT events thus leading
      // to an indefinitely blocked Work(..) method.
      poller_->KickExternal(false);
    }
  }
  // For the Ref() taken at the beginning of this function.
  Unref();
}

void PollEventHandle::NotifyOnWrite(PosixEngineClosure* on_write) {
  // We need to take a Ref here because NotifyOnLocked may trigger execution
  // of a closure which calls OrphanHandle that may delete this object or call
  // poller->Shutdown() prematurely.
  Ref();
  {
    grpc_core::ReleasableMutexLock lock(&mu_);
    if (NotifyOnLocked(&write_closure_, on_write)) {
      lock.Release();
      // NotifyOnLocked immediately scheduled some closure. It would have set
      // the closure state to NOT_READY. We need to wakeup the Work(...) thread
      // to start polling on this fd. If this call is not made, it is possible
      // that the poller will reach a state where all the fds under the
      // poller's control are not polled for POLLIN/POLLOUT events thus leading
      // to an indefinitely blocked Work(..) method.
      poller_->KickExternal(false);
    }
  }
  // For the Ref() taken at the beginning of this function.
  Unref();
}

void PollEventHandle::NotifyOnError(PosixEngineClosure* on_error) {
  on_error->SetStatus(
      absl::Status(absl::StatusCode::kCancelled,
                   "Polling engine does not support tracking errors"));
  scheduler_->Run(on_error);
}

void PollEventHandle::SetReadable() {
  Ref();
  {
    grpc_core::MutexLock lock(&mu_);
    SetReadyLocked(&read_closure_);
  }
  Unref();
}

void PollEventHandle::SetWritable() {
  Ref();
  {
    grpc_core::MutexLock lock(&mu_);
    SetReadyLocked(&write_closure_);
  }
  Unref();
}

void PollEventHandle::SetHasError() {}

uint32_t PollEventHandle::BeginPollLocked(uint32_t read_mask,
                                          uint32_t write_mask) {
  uint32_t mask = 0;
  bool read_ready = (pending_actions_ & 1UL);
  bool write_ready = ((pending_actions_ >> 2) & 1UL);
  Ref();
  // If we are shutdown, then no need to poll this fd. Set watch_mask to 0.
  if (is_shutdown_) {
    SetWatched(0);
    return 0;
  }
  // If there is nobody polling for read, but we need to, then start doing so.
  if (read_mask && !read_ready &&
      read_closure_ != reinterpret_cast<PosixEngineClosure*>(kClosureReady)) {
    mask |= read_mask;
  }

  // If there is nobody polling for write, but we need to, then start doing so
  if (write_mask && !write_ready &&
      write_closure_ != reinterpret_cast<PosixEngineClosure*>(kClosureReady)) {
    mask |= write_mask;
  }
  SetWatched(mask);
  return mask;
}

bool PollEventHandle::EndPollLocked(bool got_read, bool got_write) {
  if (is_orphaned_ && !IsWatched()) {
    CloseFd();
  } else if (!is_orphaned_) {
    return SetPendingActions(got_read, got_write);
  }
  return false;
}

void PollPoller::KickExternal(bool ext) {
  grpc_core::MutexLock lock(&mu_);
  if (closed_) {
    return;
  }
  if (was_kicked_) {
    if (ext) {
      was_kicked_ext_ = true;
    }
    return;
  }
  was_kicked_ = true;
  was_kicked_ext_ = ext;
  CHECK(wakeup_fd_->Wakeup().ok());
}

void PollPoller::Kick() { KickExternal(true); }

void PollPoller::PollerHandlesListAddHandle(PollEventHandle* handle) {
  handle->PollerHandlesListPos().next = poll_handles_list_head_;
  handle->PollerHandlesListPos().prev = nullptr;
  if (poll_handles_list_head_ != nullptr) {
    poll_handles_list_head_->PollerHandlesListPos().prev = handle;
  }
  poll_handles_list_head_ = handle;
  ++num_poll_handles_;
}

void PollPoller::PollerHandlesListRemoveHandle(PollEventHandle* handle) {
  if (poll_handles_list_head_ == handle) {
    poll_handles_list_head_ = handle->PollerHandlesListPos().next;
  }
  if (handle->PollerHandlesListPos().prev != nullptr) {
    handle->PollerHandlesListPos().prev->PollerHandlesListPos().next =
        handle->PollerHandlesListPos().next;
  }
  if (handle->PollerHandlesListPos().next != nullptr) {
    handle->PollerHandlesListPos().next->PollerHandlesListPos().prev =
        handle->PollerHandlesListPos().prev;
  }
  --num_poll_handles_;
}

PollPoller::PollPoller(Scheduler* scheduler, bool use_phony_poll)
    : scheduler_(scheduler),
      use_phony_poll_(use_phony_poll),
      was_kicked_(false),
      was_kicked_ext_(false),
      num_poll_handles_(0),
      poll_handles_list_head_(nullptr),
      closed_(false) {
  wakeup_fd_ = *CreateWakeupFd(&posix_interface());
  CHECK(wakeup_fd_ != nullptr);
}

PollPoller::~PollPoller() {
  // Assert that no active handles are present at the time of destruction.
  // They should have been orphaned before reaching this state.
  CHECK_EQ(num_poll_handles_, 0);
  CHECK_EQ(poll_handles_list_head_, nullptr);
}

Poller::WorkResult PollPoller::Work(
    EventEngine::Duration timeout,
    absl::FunctionRef<void()> schedule_poll_again) {
  // Avoid malloc for small number of elements.
  enum { inline_elements = 96 };
  struct pollfd pollfd_space[inline_elements];
  bool was_kicked_ext = false;
  PollEventHandle* watcher_space[inline_elements];
  Events pending_events;
  pending_events.clear();
  int timeout_ms =
      static_cast<int>(grpc_event_engine::experimental::Milliseconds(timeout));
  mu_.Lock();
  // Start polling, and keep doing so while we're being asked to
  // re-evaluate our pollers (this allows poll() based pollers to
  // ensure they don't miss wakeups).
  while (pending_events.empty() && timeout_ms >= 0) {
    int r = 0;
    size_t i;
    nfds_t pfd_count;
    struct pollfd* pfds;
    PollEventHandle** watchers;
    // Estimate start time for a poll iteration.
    grpc_core::Timestamp start = grpc_core::Timestamp::FromTimespecRoundDown(
        gpr_now(GPR_CLOCK_MONOTONIC));
    if (num_poll_handles_ + 2 <= inline_elements) {
      pfds = pollfd_space;
      watchers = watcher_space;
    } else {
      const size_t pfd_size = sizeof(*pfds) * (num_poll_handles_ + 2);
      const size_t watch_size = sizeof(*watchers) * (num_poll_handles_ + 2);
      void* buf = gpr_malloc(pfd_size + watch_size);
      pfds = static_cast<struct pollfd*>(buf);
      watchers = static_cast<PollEventHandle**>(
          static_cast<void*>((static_cast<char*>(buf) + pfd_size)));
      pfds = static_cast<struct pollfd*>(buf);
    }

    pfd_count = 1;
    auto wakeup_fd = posix_interface().GetFd(wakeup_fd_->ReadFd());
    CHECK(wakeup_fd.ok()) << wakeup_fd.StrError();
    pfds[0].fd = *wakeup_fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    // Event handles from before fork, need to be notified
    PollEventHandle* head = poll_handles_list_head_;
    while (head != nullptr) {
      {
        grpc_core::MutexLock lock(head->mu());
        // There shouldn't be any orphaned fds at this point. This is because
        // prior to marking a handle as orphaned it is first removed from
        // poll handle list for the poller under the poller lock.
        CHECK(!head->IsOrphaned());
        if (!head->IsPollhup()) {
          if (auto file_descriptor = posix_interface().GetFd(head->WrappedFd());
              file_descriptor.ok()) {
            pfds[pfd_count].fd = *file_descriptor;
            watchers[pfd_count] = head;
            // BeginPollLocked takes a ref of the handle. It also marks the
            // fd as Watched with an appropriate watch_mask. The watch_mask
            // is 0 if the fd is shutdown or if the fd is already ready (i.e
            // both read and write events are already available) and doesn't
            // need to be polled again. The watch_mask is > 0 otherwise
            // indicating the fd needs to be polled.
            pfds[pfd_count].events = head->BeginPollLocked(POLLIN, POLLOUT);
            pfd_count++;
          } else {
            LOG(ERROR) << "Polling FD from a wrong generation: "
                       << head->WrappedFd();
          }
        }
      }
      head = head->PollerHandlesListPos().next;
    }
    mu_.Unlock();
    if (!use_phony_poll_ || timeout_ms == 0 || pfd_count == 1) {
      // If use_phony_poll is true and pfd_count == 1, it implies only the
      // wakeup_fd is present. Allow the call to get blocked in this case as
      // well instead of crashing. This is because the poller::Work is called
      // right after an event engine is constructed. Even if phony poll is
      // expected to be used, we dont want to check for it until some actual
      // event handles are registered. Otherwise the EventEngine construction
      // may crash.
      r = poll(pfds, pfd_count, timeout_ms);
    } else {
      grpc_core::Crash("Attempted a blocking poll when declared non-polling.");
    }

    if (r <= 0) {
      if (r < 0 && errno != EINTR) {
        // Abort fail here.
        grpc_core::Crash(absl::StrFormat(
            "(event_engine) PollPoller:%p encountered poll error: %s", this,
            grpc_core::StrError(errno).c_str()));
      }

      for (i = 1; i < pfd_count; i++) {
        PollEventHandle* head = watchers[i];
        int watch_mask;
        grpc_core::ReleasableMutexLock lock(head->mu());
        if (head->IsWatched(watch_mask)) {
          head->SetWatched(-1);
          // This fd was Watched with a watch mask > 0.
          if (watch_mask > 0 && r < 0) {
            // This case implies the fd was polled (since watch_mask > 0 and
            // the poll returned an error. Mark the fds as both readable and
            // writable.
            if (head->EndPollLocked(true, true)) {
              // Its safe to add to list of pending events because
              // EndPollLocked returns true only when the handle is
              // not orphaned. But an orphan might be initiated on the handle
              // after this Work() method returns and before the next Work()
              // method is invoked.
              pending_events.push_back(head);
            }
          } else {
            // In this case, (1) watch_mask > 0 && r == 0 or (2) watch_mask ==
            // 0 and r < 0 or (3) watch_mask == 0 and r == 0. For case-1, no
            // events are pending on the fd even though the fd was polled. For
            // case-2 and 3, the fd was not polled
            head->EndPollLocked(false, false);
          }
        } else {
          // It can enter this case if an orphan was invoked on the handle
          // while it was being polled.
          head->EndPollLocked(false, false);
        }
        lock.Release();
        // Unref the ref taken at BeginPollLocked.
        head->Unref();
      }
    } else {
      if (pfds[0].revents & kPollinCheck) {
        CHECK(wakeup_fd_->ConsumeWakeup().ok());
      }
      for (i = 1; i < pfd_count; i++) {
        PollEventHandle* head = watchers[i];
        int watch_mask;
        grpc_core::ReleasableMutexLock lock(head->mu());
        if (!head->IsWatched(watch_mask) || watch_mask == 0) {
          // IsWatched will be false if an orphan was invoked on the
          // handle while it was being polled. If watch_mask is 0, then the fd
          // was not polled.
          head->SetWatched(-1);
          head->EndPollLocked(false, false);
        } else {
          // Watched is true and watch_mask > 0
          if (pfds[i].revents & POLLHUP) {
            head->SetPollhup(true);
          }
          head->SetWatched(-1);
          if (head->EndPollLocked(pfds[i].revents & kPollinCheck,
                                  pfds[i].revents & kPolloutCheck)) {
            // Its safe to add to list of pending events because EndPollLocked
            // returns true only when the handle is not orphaned.
            // But an orphan might be initiated on the handle after this
            // Work() method returns and before the next Work() method is
            // invoked.
            pending_events.push_back(head);
          }
        }
        lock.Release();
        // Unref the ref taken at BeginPollLocked.
        head->Unref();
      }
    }

    if (pfds != pollfd_space) {
      gpr_free(pfds);
    }

    // End of poll iteration. Update how much time is remaining.
    timeout_ms -= PollElapsedTimeToMillis(start);
    mu_.Lock();
    if (std::exchange(was_kicked_, false) &&
        std::exchange(was_kicked_ext_, false)) {
      // External kick. Need to break out.
      was_kicked_ext = true;
      break;
    }
  }
  mu_.Unlock();
  if (pending_events.empty()) {
    if (was_kicked_ext) {
      return Poller::WorkResult::kKicked;
    }
    return Poller::WorkResult::kDeadlineExceeded;
  }
  // Run the provided callback synchronously.
  schedule_poll_again();
  // Process all pending events inline.
  for (auto& it : pending_events) {
    it->ExecutePendingActions();
  }
  return was_kicked_ext ? Poller::WorkResult::kKicked : Poller::WorkResult::kOk;
}

void PollPoller::Close() {
  grpc_core::MutexLock lock(&mu_);
  closed_ = true;
}

#ifdef GRPC_ENABLE_FORK_SUPPORT
void PollPoller::HandleForkInChild() {
  if (grpc_core::IsEventEngineForkEnabled()) {
    posix_interface().AdvanceGeneration();
  }
  PollEventHandle* handle;
  {
    grpc_core::MutexLock lock(&mu_);
    handle = poll_handles_list_head_;
  }
  while (handle != nullptr) {
    handle->ShutdownHandle(absl::CancelledError("Closed on fork"));
    handle = handle->PollerHandlesListPos().next;
  }
}
#endif  // GRPC_ENABLE_FORK_SUPPORT

void PollPoller::ResetKickState() {
  wakeup_fd_ = *CreateWakeupFd(&posix_interface());
  // Sometimes there's "kick" signalled on the wakeup FD. We need to redo it on
  // new fd
  // TODO (eostroukhov): Need to consider merging kicked/kicked_ext
  // with the wakeup_fd so there's no duplicate state.
  grpc_core::MutexLock lock(&mu_);
  was_kicked_ = false;
  was_kicked_ext_ = false;
}

std::shared_ptr<PollPoller> MakePollPoller(Scheduler* scheduler,
                                           bool use_phony_poll) {
  static bool kPollPollerSupported =
      grpc_event_engine::experimental::SupportsWakeupFd();
  if (kPollPollerSupported) {
    return std::make_shared<PollPoller>(scheduler, use_phony_poll);
  }
  return nullptr;
}

}  // namespace grpc_event_engine::experimental

#else  // GRPC_POSIX_SOCKET_EV_POLL

#include "src/core/util/crash.h"

namespace grpc_event_engine::experimental {

PollPoller::~PollPoller() { grpc_core::Crash("unimplemented"); }

EventHandle* PollPoller::CreateHandle(FileDescriptor /*fd*/,
                                      absl::string_view /*name*/,
                                      bool /*track_err*/) {
  grpc_core::Crash("unimplemented");
}

Poller::WorkResult PollPoller::Work(
    EventEngine::Duration /*timeout*/,
    absl::FunctionRef<void()> /*schedule_poll_again*/) {
  grpc_core::Crash("unimplemented");
}

void PollPoller::Kick() { grpc_core::Crash("unimplemented"); }

// If GRPC_LINUX_EPOLL is not defined, it means epoll is not available. Return
// nullptr.
std::shared_ptr<PollPoller> MakePollPoller(Scheduler* /*scheduler*/,
                                           bool /* use_phony_poll */) {
  return nullptr;
}

#ifdef GRPC_ENABLE_FORK_SUPPORT
void PollPoller::HandleForkInChild() { grpc_core::Crash("unimplemented"); }
#endif  // GRPC_ENABLE_FORK_SUPPORT

void PollPoller::ResetKickState() { grpc_core::Crash("unimplemented"); }
void PollPoller::Close() { grpc_core::Crash("unimplemented"); }

void PollPoller::KickExternal(bool /*ext*/) {
  grpc_core::Crash("unimplemented");
}

void PollPoller::PollerHandlesListAddHandle(PollEventHandle* /*handle*/) {
  grpc_core::Crash("unimplemented");
}

void PollPoller::PollerHandlesListRemoveHandle(PollEventHandle* /*handle*/) {
  grpc_core::Crash("unimplemented");
}

}  // namespace grpc_event_engine::experimental

#endif  // GRPC_POSIX_SOCKET_EV_POLL
