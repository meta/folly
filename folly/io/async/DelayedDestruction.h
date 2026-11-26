/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cassert>
#include <cstdlib>
#include <memory>

#include <folly/io/async/DelayedDestructionBase.h>

namespace folly {

/**
 * DelayedDestruction is a helper class to ensure objects are not deleted
 * while they still have functions executing in a higher stack frame.
 *
 * This is useful for objects that invoke callback functions, to ensure that a
 * callback does not destroy the calling object.
 *
 * Classes needing this functionality should:
 * - derive from DelayedDestruction
 * - make their destructor private or protected, so it cannot be called
 *   directly
 * - create a DestructorGuard object on the stack in each public method that
 *   may invoke a callback
 *
 * DelayedDestruction does not perform any locking.  It is intended to be used
 * only from a single thread.
 */
class DelayedDestruction : public DelayedDestructionBase {
 public:
  /**
   * destroy() requests destruction of the object.
   *
   * This method will destroy the object after it has no more functions running
   * higher up on the stack.  (i.e., No more DestructorGuard objects exist for
   * this object.)  This method must be used instead of the destructor.
   */
  virtual void destroy() {
    // If guardCount_ is not 0, just set destroyPending_ to delay
    // actual destruction.
    if (getDestructorGuardCount() != 0) {
      destroyPending_ = true;
    } else {
      onDelayedDestroy(false);
    }
  }

  /**
   * Helper class to allow DelayedDestruction classes to be used with
   * std::shared_ptr.
   *
   * This class can be specified as the destructor argument when creating the
   * shared_ptr, and it will destroy the guarded class properly when all
   * shared_ptr references are released.
   */
  class Destructor {
   public:
    void operator()(DelayedDestruction* dd) const { dd->destroy(); }
  };

  bool getDestroyPending() const { return destroyPending_; }

 protected:
  /**
   * Protected destructor.
   *
   * Making this protected ensures that users cannot delete DelayedDestruction
   * objects directly, and that everyone must use destroy() instead.
   * Subclasses of DelayedDestruction must also define their destructors as
   * protected or private in order for this to work.
   *
   * This also means that DelayedDestruction objects cannot be created
   * directly on the stack; they must always be dynamically allocated on the
   * heap.
   *
   * In order to use a DelayedDestruction object with a shared_ptr, create the
   * shared_ptr using a DelayedDestruction::Destructor as the second argument
   * to the shared_ptr constructor.
   */
  ~DelayedDestruction() override;

  DelayedDestruction() : destroyPending_(false) {}

 private:
  /**
   * destroyPending_ is set to true if destoy() is called while guardCount_ is
   * non-zero. It is set to false before the object is deleted.
   *
   * If destroyPending_ is true, the object will be destroyed the next time
   * guardCount_ drops to 0.
   */
  bool destroyPending_;

  void onDelayedDestroy(bool delayed) override {
    // check if it is ok to destroy now
    if (delayed && !destroyPending_) {
      return;
    }
    destroyPending_ = false;
    delete this;
  }
};

template <typename T>
using DelayedDestructionUniquePtr =
    std::unique_ptr<T, DelayedDestruction::Destructor>;

template <typename T, typename... A>
DelayedDestructionUniquePtr<T> makeDelayedDestructionUniquePtr(A&&... a) {
  return DelayedDestructionUniquePtr<T>{new T(static_cast<A&&>(a)...)};
}

/**
 * A helper class to allow a DelayedDestruction object to be instantiated on
 * the stack.
 *
 * This class derives from an existing DelayedDestruction type and makes the
 * destructor public again.  This allows objects of this type to be declared on
 * the stack or directly inside another class.  Normally DelayedDestruction
 * objects must be dynamically allocated on the heap.
 *
 * However, the trade-off is that you lose some of the protections provided by
 * DelayedDestruction::destroy().  DelayedDestruction::destroy() will
 * automatically delay destruction of the object until it is safe to do so.
 * If you use UndelayedDestruction, you become responsible for ensuring that
 * you only destroy the object where it is safe to do so.  Attempting to
 * destroy a UndelayedDestruction object while it has a non-zero destructor
 * guard count will abort the program.
 */
template <typename TDD>
class UndelayedDestruction : public TDD {
 public:
  using TDD::TDD;

  /**
   * Public destructor.
   *
   * The caller is responsible for ensuring that the object is only destroyed
   * where it is safe to do so.  (i.e., when the destructor guard count is 0).
   *
   * The exact conditions for meeting this may be dependent upon your class
   * semantics.  Typically you are only guaranteed that it is safe to destroy
   * the object directly from the event loop (e.g., directly from a
   * EventBase::LoopCallback), or when the event loop is stopped.
   */
  ~UndelayedDestruction() override {
    // Crash if the caller is destroying us with outstanding destructor guards.
    if (this->getDestructorGuardCount() != 0) {
      abort();
    }
    // Invoke destroy.  This is necessary since our base class may have
    // implemented custom behavior in destroy().
    this->destroy();
  }

  // Defaulted (and implicitly deleted while the base is unmovable) to
  // satisfy special-member-function checks.
  UndelayedDestruction(UndelayedDestruction&&) = default;
  UndelayedDestruction& operator=(UndelayedDestruction&&) = default;

  void onDelayedDestroy(bool delayed) override {
    if (delayed && !this->TDD::getDestroyPending()) {
      return;
    }
    // Do nothing.  This will always be invoked from the call to destroy
    // inside our destructor.
    assert(!delayed);
    // prevent unused variable warnings when asserts are compiled out.
    (void)delayed;
  }

 protected:
  /**
   * Override our parent's destroy() method to make it protected.
   * Callers should use the normal destructor instead of destroy
   */
  void destroy() override { this->TDD::destroy(); }

 private:
  // Forbidden copy constructor and assignment operator
  UndelayedDestruction(UndelayedDestruction const&) = delete;
  UndelayedDestruction& operator=(UndelayedDestruction const&) = delete;
};

} // namespace folly
