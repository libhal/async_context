// Copyright 2024 - 2025 Khalil Estell and the libhal contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

module;

#include <cstddef>
#include <cstdint>

export module async_context:types;

export namespace async {

using u8 = std::uint8_t;
using byte = std::uint8_t;
using usize = std::size_t;

enum class blocked_by : u8
{
  /// Not blocked by anything, ready to run!
  nothing = 0,
  /// Blocked by an amount of time that must be waited before the task resumes.
  /// It is the responsibility of the scheduler to defer calling the active
  /// coroutine of an `context` until the amount of time requested has
  /// elapsed.
  ///
  /// The time duration passed to the transition handler function represents the
  /// amount of time that the coroutine must suspend for until before scheduling
  /// the task again. Timed delays in this fashion are not real time and only
  /// represent the shortest duration of time necessary to fulfil the
  /// coroutine's delay needs. To schedule the coroutine before its delay time
  /// has been awaited, is considered to be undefined behavior. It is the
  /// responsibility of the develop of the transition handler to ensure that
  /// tasks are not executed until their delay time has elapsed.
  ///
  /// A value of 0 means do not wait and suspend but set the blocked by state to
  /// `time`. This will suspend the coroutine and it later. This is equivalent
  /// to just performing `std::suspend_always` but with additional steps, thus
  /// it is not advised to perform `co_await 0ns`.
  time = 1,
  /// Blocked by an I/O operation such as a DMA transfer or a bus. It is the
  /// responsibility of an interrupt (or thread performing I/O operations) to
  /// change the state to 'nothing' when the operation has completed.
  ///
  /// The time duration passed to the transition handler represents the amount
  /// of time the task must wait until it may resume the task even before its
  /// block status has transitioned to "nothing". This would represent the
  /// coroutine providing a hint to the scheduler about polling the coroutine. A
  /// value of 0 means wait indefinitely.
  io = 2,
  /// Blocked by a synchronization primitive of some kind. It is the
  /// responsibility of - to change the state to 'nothing' when new work is
  /// available.
  ///
  /// The time duration passed to the transition handler represents... TBD
  sync = 3,
  /// Blocked by a lack of work to perform. It is the responsibility of - to
  /// change the state to 'nothing' when new work is available.
  ///
  /// The time duration passed to the transition handler represents the amount
  /// of time the task must wait until it may resume the task even before its
  /// block status has transitioned to "nothing". This would represent the
  /// coroutine providing a hint to the scheduler about polling the coroutine. A
  /// value of 0 means wait indefinitely.
  inbox_empty = 4,
  /// Blocked by congestion to a mailbox of work. For example attempting to
  /// write a message to one of 3 outgoing mailboxes over CAN but all are
  /// currently busy waiting to emit their message. It is the
  /// responsibility of - to change the state to 'nothing' when new work is
  /// available.
  ///
  /// The time duration passed to the transition handler represents the amount
  /// of time the task must wait until it may resume the task even before its
  /// block status has transitioned to "nothing". This would represent the
  /// coroutine providing a hint to the scheduler about polling the coroutine. A
  /// value of 0 means wait indefinitely.
  outbox_full = 5,
};
}  // namespace async
