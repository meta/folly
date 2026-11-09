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

#include <algorithm>
#include <bitset>
#include <vector>

#include <glog/logging.h>
#include <folly/ConstructorCallbackList.h>
#include <folly/Function.h>
#include <folly/Optional.h>
#include <folly/ScopeGuard.h>
#include <folly/io/async/DestructorCheck.h>
#include <folly/lang/Switch.h>
#include <folly/small_vector.h>

/**
 * Tooling that makes it easier to design observable objects and observers.
 */

namespace folly {

/**
 * Interface for store of pointers to observers.
 */
template <typename Observer>
class ObserverContainerStoreBase {
 public:
  using observer_type = Observer;

  // ObserverContainerStore stores shared_ptr<Observer> objects.
  //
  // To support observer objects that are NOT managed by a shared_ptr, the
  // encapsulating ObserverContainer wraps unmanaged pointers inside of
  // shared_ptrs, but sets an empty deleter for the shared_ptr, so that the
  // pointer remains unmanaged.
  //
  // As a result, it is possible for the shared_ptrs maintained by the store to
  // be "unmanaged". The type alias `MaybeManagedObserverPointer` is used to
  // ensure that this detail is apparent in other parts of the container code.
  using MaybeManagedObserverPointer = std::shared_ptr<Observer>;

  virtual ~ObserverContainerStoreBase() = default;

  /**
   * Add an observer pointer to the store.
   *
   * @param observer     Observer to add.
   * @return             Whether observer was added (not already present).
   */
  virtual bool add(MaybeManagedObserverPointer observer) = 0;

  /**
   * Remove an observer pointer from the store.
   *
   * @param observer     Observer to remove.
   * @return             Whether observer found and removed from store.
   */
  virtual bool remove(MaybeManagedObserverPointer observer) = 0;

  /**
   * Find an observer in the store by pointer identity.
   *
   * Returns a copy of the stored pointer, retaining ownership if it is managed.
   *
   * @param observer     Observer to find.
   * @return             Stored observer pointer, or nullptr if not found.
   */
  virtual MaybeManagedObserverPointer findObserver(
      const Observer* observer) const = 0;

  /**
   * Get number of observers in store.
   *
   * If called while the store is being iterated, the returned value may not
   * reflect changes that occurred (e.g., observers added or removed) during
   * iteration.
   *
   * @return             Number of observers in store.
   */
  virtual size_t size() const = 0;

  /**
   * Policy that determines how invokeForEachObserver handles mutations.
   */
  enum class InvokeWhileIteratingPolicy {
    InvokeAdded, // if observer added, invoke fn for it
    DoNotInvokeAdded, // if observer added, do not invoke fn for it
    CheckNoChange, // observers must not be added or removed during iteration
    CheckNoAdded // observers must not be added during iteration
  };

  /**
   * Invoke function for each observer in the store.
   *
   * @param fn           Function to call for each observer in store.
   * @param policy       InvokeWhileIteratingPolicy policy.
   */
  virtual void invokeForEachObserver(
      folly::Function<void(MaybeManagedObserverPointer&)>&& fn,
      const InvokeWhileIteratingPolicy policy) = 0;

  /**
   * Invoke function for each observer in the store.
   *
   * @param fn           Function to call for each observer in store.
   * @param policy       InvokeWhileIteratingPolicy policy.
   */
  virtual void invokeForEachObserver(
      folly::Function<void(Observer*)>&& fn,
      const InvokeWhileIteratingPolicy policy) {
    // A by-value capture is 64 bytes, over the 48-byte in-situ buffer, and
    // would heap-allocate per call; the callee never retains the wrapper.
    auto fnL = std::move(fn);
    invokeForEachObserver(
        [&fnL](MaybeManagedObserverPointer& observer) { fnL(observer.get()); },
        policy);
  }
};

/**
 * Policy for ObserverContainerStore.
 *
 * Defines the udnerlying container type and the default size.
 */
template <unsigned int ReserveElements = 2>
struct ObserverContainerStorePolicyDefault {
  template <typename Observer>
  using container = std::conditional_t<
      !kIsMobile,
      folly::small_vector<Observer, ReserveElements>,
      std::vector<Observer>>;
  const static unsigned int reserve_elements = ReserveElements;
};

/**
 * Policy-based implementation of ObserverContainerStoreBase.
 */
template <
    typename Observer,
    typename Policy = ObserverContainerStorePolicyDefault<>>
class ObserverContainerStore : public ObserverContainerStoreBase<Observer> {
 public:
  using Base = ObserverContainerStoreBase<Observer>;
  using InvokeWhileIteratingPolicy = typename Base::InvokeWhileIteratingPolicy;

  /**
   * Construct a new store, reserving as configured.
   */
  ObserverContainerStore() { observers_.reserve(Policy::reserve_elements); }

  /**
   * Add an observer pointer to the store.
   *
   * @param observer     Observer to add.
   * @return             Whether observer was added (not already present).
   */
  bool add(std::shared_ptr<Observer> observer) override {
    // attempts to add the same observer multiple times are rejected
    if (std::find(observers_.begin(), observers_.end(), observer) !=
        observers_.end()) {
      return false;
    }

    if (iterating_) {
      CHECK(maybeCurrentIterationPolicy_.has_value());
      const auto& policy = maybeCurrentIterationPolicy_.value();
      FOLLY_EXHAUSTIVE_SWITCH({
        switch (policy) {
          case InvokeWhileIteratingPolicy::InvokeAdded:
          case InvokeWhileIteratingPolicy::DoNotInvokeAdded:
            break;
          case InvokeWhileIteratingPolicy::CheckNoChange:
            folly::terminate_with<std::runtime_error>(
                "Cannot add observers while iterating "
                "per current iteration policy (CheckNoChange)");
            break;
          case InvokeWhileIteratingPolicy::CheckNoAdded:
            folly::terminate_with<std::runtime_error>(
                "Cannot add observers while iterating "
                "per current iteration policy (CheckNoAdded)");
            break;
          default:
            folly::assume_unreachable();
        }
      });
    }
    observers_.insert(observers_.end(), observer);
    return true;
  }

  /**
   * Remove an observer pointer from the store.
   *
   * @param observer     Observer to remove.
   * @return             Whether observer found and removed from store.
   */
  bool remove(std::shared_ptr<Observer> observer) override {
    const auto it = std::find(observers_.begin(), observers_.end(), observer);
    if (it == observers_.end()) {
      return false;
    }

    // if store is currently being iterated, set this element to nullptr and it
    // will be cleaned up after iteration is completed, else erase immediately.
    if (iterating_) {
      CHECK(maybeCurrentIterationPolicy_.has_value());
      const auto& policy = maybeCurrentIterationPolicy_.value();
      FOLLY_EXHAUSTIVE_SWITCH({
        switch (policy) {
          case InvokeWhileIteratingPolicy::InvokeAdded:
          case InvokeWhileIteratingPolicy::DoNotInvokeAdded:
            break;
          case InvokeWhileIteratingPolicy::CheckNoChange:
            folly::terminate_with<std::runtime_error>(
                "Cannot remove observers while iterating "
                "per current iteration policy (CheckNoChange)");
            break;
          case InvokeWhileIteratingPolicy::CheckNoAdded:
            break;
          default:
            folly::assume_unreachable();
        }
      });

      *it = nullptr;
      removalDuringIteration_ = true;
    } else {
      observers_.erase(it);
    }

    return true;
  }

  /**
   * Get number of observers in store.
   *
   * If called while the store is being iterated, the returned value may not
   * reflect changes that occurred (e.g., observers added or removed) during
   * iteration.
   *
   * @return             Number of observers in store.
   */
  size_t size() const override { return observers_.size(); }

  /**
   * Find an observer in the store by pointer identity.
   *
   * Returns a copy of the stored pointer, retaining ownership if it is managed.
   *
   * @param observer     Observer to find.
   * @return             Stored observer pointer, or nullptr if not found.
   */
  std::shared_ptr<Observer> findObserver(
      const Observer* observer) const override {
    if (!observer) {
      return nullptr;
    }
    const auto it = std::find_if(
        observers_.begin(), observers_.end(), [observer](const auto& item) {
          return item.get() == observer;
        });
    return it == observers_.end() ? nullptr : *it;
  }

  /**
   * Invoke function for each observer in the store.
   *
   * @param fn           Function to call for each observer in store.
   * @param policy       InvokeWhileIteratingPolicy policy.
   */
  void invokeForEachObserver(
      folly::Function<void(typename Base::MaybeManagedObserverPointer&)>&& fn,
      const typename Base::InvokeWhileIteratingPolicy policy) noexcept
      override {
    CHECK(!iterating_)
        << "Nested iteration of ObserverContainer is prohibited.";
    CHECK(!maybeCurrentIterationPolicy_.has_value())
        << "Nested iteration of ObserverContainer is prohibited.";
    iterating_ = true;
    maybeCurrentIterationPolicy_ = policy;
    SCOPE_EXIT {
      if (removalDuringIteration_) {
        // observers removed while we were iterating through container;
        // remove elements for which the element value is null
        observers_.erase(
            std::remove_if(
                observers_.begin(),
                observers_.end(),
                [](const auto& elem) { return elem == nullptr; }),
            observers_.end());
      }
      iterating_ = false;
      maybeCurrentIterationPolicy_ = folly::none;
      removalDuringIteration_ = false;
    };

    const auto numObserversAtStart = observers_.size();

    // iterate through the list using indexes, not iterators, so that the list
    // can mutate during iteration...
    for (typename container_type::size_type idx = 0;
         // observers_.size() cannot decrease during iteration, so it should be
         // insignificantly faster to check the single size in the common case.
         idx < numObserversAtStart ||
         (idx < observers_.size() &&
          policy == InvokeWhileIteratingPolicy::InvokeAdded);
         idx++) {
      auto& observer = observers_.at(idx);
      if (!observer) { // empty space in list caused by incomplete removal
        continue;
      }

      fn(observer);
    }
  }

  using Base::invokeForEachObserver;

 private:
  using container_type =
      typename Policy::template container<std::shared_ptr<Observer>>;

  // The actual list of observers.
  container_type observers_;

  // Whether we are actively iterating through the list of observers.
  bool iterating_{false};

  // If we are actively iterating, the corresponding InvokeWhileIteratingPolicy.
  folly::Optional<InvokeWhileIteratingPolicy> maybeCurrentIterationPolicy_;

  // Whether a removal or addition occurred while we iterating through the list.
  bool removalDuringIteration_{false};
};

/**
 * Policy for ObserverContainerBase.
 *
 * @tparam EventEnum    Enum of events that observers can subscribe to.
 *                      Each event must have a unique integer value greater
 *                      than zero.
 *
 * @tparam BitsetSize   Size of bitset, must be greater than or equal to the
 *                      number of events in EventEnum.
 */
template <typename EventEnum, size_t BitsetSize>
struct ObserverContainerBasePolicyDefault {
  static constexpr size_t bitset_size() { return BitsetSize; }
  using event_enum = EventEnum;
};

/**
 * Base ObserverContainer and definition of Observers.
 */
template <
    typename ObserverInterface,
    typename Observed,
    typename ContainerPolicy>
class ObserverContainerBase {
 public:
  using interface_type = ObserverInterface;
  using observed_type = Observed;
  using policy_type = ContainerPolicy;
  using EventEnum = typename ContainerPolicy::event_enum;
  using EventEnumIntT = std::underlying_type_t<EventEnum>;

  virtual ~ObserverContainerBase() = default;

  /**
   * EventSet is used to keep track of the observer events that are enabled.
   */
  class ObserverEventSet {
   public:
    ObserverEventSet() : bitset_(0) {}

    /**
     * Enables all events.
     */
    void enableAllEvents() { bitset_.set(); }

    /**
     * Enables the events passed in the initializer list.
     *
     * @param eventsEnums  Events to enable.

     */
    template <typename... EventEnums>
    void enable(EventEnums... eventEnums) {
      for (auto&& event : {eventEnums...}) {
        const auto eventAsInt = static_cast<EventEnumIntT>(event);
        bitset_.set(eventAsInt);
      }
    }

    /**
     * Returns whether the event passed in is enabled.
     *
     * @param event        Event to check.
     * @return             Whether the passed event is enabled.
     */
    bool isEnabled(const EventEnum event) const {
      const auto eventAsInt = static_cast<EventEnumIntT>(event);
      return bitset_.test(eventAsInt);
    }

    /**
     * Builder that makes it easier to pass EventSet to Observer constructor.
     */
    class Builder {
     public:
      explicit Builder() = default;

      /**
       * Enables all events.
       */
      Builder&& enableAllEvents() {
        set_.enableAllEvents();
        return std::move(*this);
      }

      /**
       * Enables the events passed in the intiailizer list.
       *
       * @param events       Events to enable.
       */
      template <typename... EventEnums>
      Builder&& enable(EventEnums... eventEnums) {
        set_.enable(eventEnums...);
        return std::move(*this);
      }

      /**
       * Returns the EventSet that has been built.
       */
      ObserverEventSet build() && { return set_; }

     private:
      ObserverEventSet set_;
    };

   private:
    std::bitset<ContainerPolicy::bitset_size()> bitset_{0};
  };

  /**
   * Observer base interface.
   *
   * This interface includes the events exposed by the subject's observer
   * interface and the set of events that are provided by the ObserverContainer
   * (attached/detached/moved/destoyed). It also defines how observers subscribe
   * to specific events made available by the subject.
   */
  class ObserverBase : public ObserverInterface, public DestructorCheck {
   public:
    using observed_type = Observed;
    using interface_type = ObserverInterface;

    using EventSet = ObserverEventSet;
    using EventSetBuilder = typename ObserverEventSet::Builder;

    ~ObserverBase() override = default;

    /**
     * Construct a new observer with no event subscriptions.
     */
    ObserverBase() {}

    /**
     * Construct a new observer subscribed to events in the passed EventSet.
     */
    explicit ObserverBase(EventSet eventSet) : eventSet_(eventSet) {}

    /**
     * Base class that can be used to pass context about move operation.
     */
    class MoveContext {};

    /**
     * Base class that can be used to pass context about why object destroyed.
     */
    class DestroyContext {};

    /**
     * Invoked when this observer is attached to an object.
     *
     * @param obj   Object that observer is now attached to.
     */
    virtual void attached(Observed* /* obj */) noexcept {}

    /**
     * Invoked if this observer is detached from an object.
     *
     * @param obj   Object that observer is no longer attached to.
     */
    virtual void detached(Observed* /* obj */) noexcept {}

    /**
     * Invoked when an observed object's destructor is invoked.
     *
     * Destruction of the observed object implicitly implies detached, and thus
     * detached will not be called if an object is destroyed.
     *
     * @param obj           Object being destroyed.
     * @param ctx           Additional info about what triggered destruction.
     *                      Not available unless provided by the implementation;
     *                      if not supported it is a nullptr.
     */
    virtual void destroyed(
        Observed* /* obj */, DestroyContext* /* ctx */) noexcept {}

    /**
     * Invoked when object being observed changes due to move construction.
     *
     * @param oldObj        Object previously being observed.
     * @param newObj        Object now being observed.
     * @param ctx           Additional info about what triggered the move.
     *                      Not available unless provided by the implementation;
     *                      if not supported it is a nullptr.
     */
    virtual void moved(
        Observed* /* oldObj */,
        Observed* /* newObj */,
        MoveContext* /* ctx */) noexcept {}

    /**
     * Proxy function used to invoke a method defined in the observer interface.
     *
     * Can be overridden to enable composition of observers, including event bus
     * architectures in which multiple handlers act on an event.
     *
     * Implementations can remove themselves and add/remove other observers from
     * the container when handling this call. If new observers are added to the
     * container, invokeInterfaceMethod will be called on those new observers
     * as well. If you want to avoid this in your observer implementation, delay
     * mutation of the container until postInvokeInterfaceMethod is called.
     *
     * @param obj           Object associated with observer event.
     * @param fn            Function that will invoke the method associated with
     *                      an observer event, passing any event context.
     * @param maybeEvent    The event enum associated with the invocation.
     */
    virtual void invokeInterfaceMethod(
        Observed* obj,
        folly::Function<void(ObserverBase*, Observed*)>& fn,
        folly::Optional<EventEnum> /* maybeEvent */) noexcept {
      fn(this, obj);
    }

    /**
     * Invoked after invokeInterfaceMethod has completed for all observers.
     *
     * Can be used to delay mutation of the container after processing of an
     * event has completed. Implementations can remove themselves and add/remove
     * other observers from the container when handling this call. However, this
     * function will only be called for the set of observers in the container
     * when the preceding call to invokeInterfaceMethod finished.
     *
     * @param obj           Object associated with observer event.
     */
    virtual void postInvokeInterfaceMethod(Observed* /* obj */) noexcept {}

    /**
     * Returns the EventSet containing the events the observer wants.
     */
    const EventSet& getEventSet() const noexcept { return eventSet_; }

   private:
    const EventSet eventSet_;
  };

  /**
   * Observer interface.
   *
   * The interface between an observer container and observers in the container.
   *
   * This interface includes methods that are called upon relevant changes to
   * the observer's status in a container (added/removedFromObserverContainer).
   *
   * An observer must not be destroyed while it is in a container. This can be
   * accomplished by removing the observer from the container on its destruction
   * or delaying destruction.
   *
   * Typical use cases should not attempt to implement this interface and should
   * instead use a specialization such as ManagedObserver.
   */
  class Observer : public ObserverBase {
   public:
    using observed_type = Observed;
    using interface_type = ObserverInterface;

    using EventSet = ObserverEventSet;
    using EventSetBuilder = typename ObserverEventSet::Builder;

    ~Observer() override = default;

    /**
     * Construct a new observer with no event subscriptions.
     */
    Observer() : ObserverBase() {}

    /**
     * Construct a new observer subscribed to events in the passed EventSet.
     */
    explicit Observer(EventSet eventSet) : ObserverBase(eventSet) {}

    /**
     * Invoked when this observer has been added to an observer container.
     *
     * For the typical observer container implementation a call to `attached`
     * will proceed a call to this method.
     *
     * The observer implementation must ensure that it remains alive as long as
     * it is in this container.
     *
     * @param ctr           Container observer has been added to.
     */
    virtual void addedToObserverContainer(
        ObserverContainerBase* ctr) noexcept = 0;

    /**
     * Invoked when this observer has been removed from an observer container.
     *
     * For the typical observer container implementation a call to `detached`
     * will have occurred before this method is called.
     *
     * @param ctr           Container observer has been removed from.
     */
    virtual void removedFromObserverContainer(
        ObserverContainerBase* ctr) noexcept = 0;

    /**
     * Invoked when this observer is moved from one container to another.
     *
     * Occurs in the case of move construction of a new object during which the
     * observers in the observer container are shifted from the old object to
     * the new object.
     *
     * @param oldCtr        Container observer has been removed from.
     * @param newCtr        Container observer has been added to.
     */
    virtual void movedToObserverContainer(
        ObserverContainerBase* oldCtr,
        ObserverContainerBase* newCtr) noexcept = 0;
  };

  /**
   * Returns the object associated with the container (e.g., observed object).
   *
   * @return             Return object associated with container or nullptr.
   */
  virtual Observed* getObject() const = 0;

  /**
   * Adds an observer to the container.
   *
   * If the observer is already in the container, this is a no-op.
   *
   * @param observer     Observer to add.
   */
  virtual void addObserver(std::shared_ptr<Observer> observer) = 0;

  /**
   * Adds an observer to the container.
   *
   * If the observer is already in the container, this is a no-op.
   *
   * @param observer     Observer to add.
   */
  virtual void addObserver(Observer* observer) {
    // create a shared_ptr holding an unmanaged ptr
    // this does not trigger control block allocation
    return addObserver(
        std::shared_ptr<Observer>(std::shared_ptr<void>(), observer));
  }

  /**
   * Removes an observer from the container.
   *
   * @param observer     Observer to remove.
   * @return             Whether the observer was found and removed.
   */
  virtual bool removeObserver(std::shared_ptr<Observer> observer) = 0;

  /**
   * Removes an observer from the container.
   *
   * @param observer     Observer to remove.
   * @return             Whether the observer was found and removed.
   */
  virtual bool removeObserver(Observer* observer) {
    // create a shared_ptr holding an unmanaged ptr
    // this does not trigger control block allocation
    return removeObserver(
        std::shared_ptr<Observer>(std::shared_ptr<void>(), observer));
  }

  /**
   * Get number of observers in container.
   *
   * @return             Number of observers in container.
   */
  size_t numObservers() const { return getStoreConst().size(); }

  /**
   * Get a list of observers in the container of type T.
   *
   * @tparam T           Type of observer to find.
   * @return             List of observers in the container of type T.
   */
  template <typename T = Observer>
  std::vector<T*> findObservers() {
    static_assert(
        std::is_base_of<Observer, T>::value,
        "T must derive from ObserverContainer::Observer");

    std::vector<T*> matchingObservers;
    using InvokeWhileIteratingPolicy = typename ObserverContainerStoreBase<
        Observer>::InvokeWhileIteratingPolicy;
    getStore().invokeForEachObserver(
        [&matchingObservers](Observer* observer) {
          auto castPtr = dynamic_cast<T*>(observer);
          if (castPtr) {
            matchingObservers.emplace_back(castPtr);
          }
        },
        InvokeWhileIteratingPolicy::CheckNoChange);

    return matchingObservers;
  }

  /**
   * Get all observers.
   *
   * @return             List of observers in the container.
   */
  std::vector<Observer*> getObservers() { return findObservers<Observer>(); }

  /**
   * Returns if any observer in the container is subscribed to a given event.
   *
   * TODO(bschlinker): The current implementation scans the entire container to
   * search for an observer subscribed to the requested event; we should cache
   * this information instead and update the cache on observer add / remove.
   *
   * @tparam event       Event in EventEnum.
   * @return             If there are observers subscribed to the given event.
   */
  template <EventEnum event>
  bool hasObserversForEvent() {
    bool foundObserverWithEvent = false;
    using InvokeWhileIteratingPolicy = typename ObserverContainerStoreBase<
        Observer>::InvokeWhileIteratingPolicy;
    getStore().invokeForEachObserver(
        [&foundObserverWithEvent](Observer* observer) {
          foundObserverWithEvent |= observer->getEventSet().isEnabled(event);
        },
        InvokeWhileIteratingPolicy::CheckNoChange);
    return foundObserverWithEvent;
  }

  /**
   * Helper class for observers that attach to single object / container.
   *
   * Does not have any thread safety, and thus can only be used if the observer
   * is driven exclusively by the same thread as the thread that controls the
   * object being observed.
   *
   * Tracks the container the observer is in (if any). If the observer's
   * destructor is triggered while it is in an container, it will be removed
   * from the container during the destruction process.
   */
  class ManagedObserver : public Observer {
   public:
    using Observer::Observer;
    using EventSet = typename Observer::EventSet;
    using EventSetBuilder = typename Observer::EventSetBuilder;

    ~ManagedObserver() override { detach(); }

    /**
     * Detach the observer (if currently attached).
     *
     * If the observer is not currently attached, this is a no-op.
     *
     * @return       If successfully detached.
     */
    bool detach() {
      if (!ctr_) {
        return false;
      }
      return ctr_->removeObserver(this);
    }

    /**
     * Return if the observer is observing an object.
     *
     * @return       If observer is observing an object.
     */
    bool isObserving() const {
      return ctr_ != nullptr && ctr_->getObject() != nullptr;
    }

    /**
     * Get the object that is being observed or nullptr.
     *
     * @return       Object being observed.
     */
    Observed* getObservedObject() const {
      if (!ctr_) {
        return nullptr;
      }
      return ctr_->getObject();
    }

   private:
    void addedToObserverContainer(
        ObserverContainerBase* ctr) noexcept override {
      CHECK(!ctr_);
      ctr_ = ctr;
    }

    void removedFromObserverContainer(
        ObserverContainerBase* ctr) noexcept override {
      CHECK_EQ(ctr_, ctr);
      ctr_ = nullptr;
    }

    void movedToObserverContainer(
        ObserverContainerBase* oldCtr,
        ObserverContainerBase* newCtr) noexcept override {
      CHECK_EQ(ctr_, oldCtr);
      CHECK_NE(ctr_, newCtr);
      ctr_ = newCtr;
    }

    // Container the observer is in (or nullptr).
    ObserverContainerBase* ctr_{nullptr};
  };

 protected:
  virtual ObserverContainerStoreBase<Observer>& getStore() = 0;

  virtual const ObserverContainerStoreBase<Observer>& getStoreConst() const = 0;

  void invokeInterfaceMethodImpl(
      Observed* observed,
      folly::Function<void(ObserverBase*, Observed*)>&& fn,
      const folly::Optional<EventEnum> maybeEvent = folly::none) noexcept {
    using InvokeWhileIteratingPolicy = typename ObserverContainerStoreBase<
        Observer>::InvokeWhileIteratingPolicy;
    getStore().invokeForEachObserver(
        [observed, maybeEvent, &fn](Observer* observer) {
          if (!maybeEvent.has_value() ||
              observer->getEventSet().isEnabled(maybeEvent.value())) {
            observer->invokeInterfaceMethod(observed, fn, maybeEvent);
          }
        },
        InvokeWhileIteratingPolicy::InvokeAdded);
    getStore().invokeForEachObserver(
        [observed, maybeEvent](ObserverBase* observer) {
          if (!maybeEvent.has_value() ||
              observer->getEventSet().isEnabled(maybeEvent.value())) {
            observer->postInvokeInterfaceMethod(observed);
          }
        },
        InvokeWhileIteratingPolicy::DoNotInvokeAdded);
  }
};

/**
 * Policy-based implementation of ObserverContainerBase.
 */
template <
    typename ObserverInterface,
    typename Observed,
    typename ContainerPolicy,
    typename StorePolicy = ObserverContainerStorePolicyDefault<>,
    std::size_t MaxConstructorCallbacks = 4>
class ObserverContainer
    : public ObserverContainerBase<
          ObserverInterface,
          Observed,
          ContainerPolicy> {
 public:
  using ContainerBase =
      ObserverContainerBase<ObserverInterface, Observed, ContainerPolicy>;
  using Observer = typename ContainerBase::Observer;
  using EventEnum = typename ContainerBase::EventEnum;
  using StoreBase = ObserverContainerStoreBase<Observer>;
  using ContainerConstructorCallbackList =
      ConstructorCallbackList<ObserverContainer, MaxConstructorCallbacks>;

  explicit ObserverContainer(Observed* obj)
      : obj_(CHECK_NOTNULL(obj)), constructorCallbackList_(this) {}

  ObserverContainer(Observed* obj, ObserverContainer&& observerContainer)
      : obj_(CHECK_NOTNULL(obj)), constructorCallbackList_(this) {
    using InvokeWhileIteratingPolicy =
        typename StoreBase::InvokeWhileIteratingPolicy;
    observerContainer.getStore().invokeForEachObserver(
        [this, &observerContainer](
            typename StoreBase::MaybeManagedObserverPointer& observer) {
          // observer may be a managed pointer (e.g., a shared_ptr), and
          // invokeForEachObserver passes a reference, so we need to copy the
          // observer object before calling remove so that it will not be
          // destroyed upon removal from the old ObserverContainer
          auto observerCopy = observer;
          CHECK_NOTNULL(observerCopy.get());

          // remove
          const bool removed = observerContainer.getStore().remove(observer);
          CHECK(removed);

          // add to new, operating solely on observerCopy
          const bool added = getStore().add(observerCopy);
          CHECK(added);
          observerCopy->movedToObserverContainer(&observerContainer, this);
          observerCopy->moved(
              observerContainer.getObject(), obj_, nullptr /* ctx */);
        },
        InvokeWhileIteratingPolicy::CheckNoAdded);
  }

  ~ObserverContainer() override {
    using InvokeWhileIteratingPolicy =
        typename StoreBase::InvokeWhileIteratingPolicy;
    getStore().invokeForEachObserver(
        [this](Observer* observer) {
          DestructorCheck::Safety dc(*observer);
          observer->destroyed(obj_, nullptr /* ctx */);
          if (!dc.destroyed()) {
            observer->removedFromObserverContainer(this);
          }
        },
        InvokeWhileIteratingPolicy::CheckNoAdded);
  }

  /**
   * Returns the object associated with the container (e.g., observed object).
   *
   * @return             Return object associated with container or nullptr.
   */
  Observed* getObject() const override { return obj_; }

  using ContainerBase::addObserver;
  using ContainerBase::removeObserver;

  /**
   * Adds an observer to the container.
   *
   * If the observer is already in the container, this is a no-op.
   *
   * @param observer     Observer to add.
   */
  void addObserver(std::shared_ptr<Observer> observer) override {
    CHECK_NOTNULL(observer.get());
    if (getStore().add(observer)) {
      DestructorCheck::Safety dc(*observer);
      observer->addedToObserverContainer(this);
      if (!dc.destroyed()) {
        observer->attached(obj_);
      }
    }
  }

  /**
   * Removes an observer from the container.
   *
   * @param observer     Observer to remove.
   * @return             Whether the observer was found and removed.
   */
  bool removeObserver(std::shared_ptr<Observer> observer) override {
    CHECK_NOTNULL(observer.get());
    auto retainedObserver = getStore().findObserver(observer.get());
    if (retainedObserver && getStore().remove(retainedObserver)) {
      DestructorCheck::Safety dc(*retainedObserver);
      retainedObserver->detached(obj_);
      if (!dc.destroyed()) {
        retainedObserver->removedFromObserverContainer(this);
      }
      return true;
    }
    return false;
  }

  /**
   * Add a callback fired each time obj of the observed type is constructed.
   *
   * Uses ConstructorCallbackList. Can be used to attach observers to all
   * objects of a given type.
   *
   * @throw std::length_error() if installing callback would exceed max allowed
   */
  static void addConstructorCallback(
      typename ContainerConstructorCallbackList::Callback cb) {
    ContainerConstructorCallbackList::addCallback(std::move(cb));
  }

  /**
   * Invokes an observer interface method on observers subscribed to an event.
   *
   * See instead `invokeInterfaceMethodAllObservers` to invoke an interface
   * method on all observers without filtering based on observer event
   * subscription.
   *
   * @tparam event       Associated event in EventEnum. The passed function will
   *                     only be called for observers subscribed to this event.
   * @param  fn          Function to call for each observer that takes a pointer
   *                     to the observer and invokes the interface method.
   */
  template <EventEnum event>
  void invokeInterfaceMethod(
      folly::Function<void(ObserverInterface*, Observed*)>&& fn) noexcept {
    this->invokeInterfaceMethodImpl(obj_, std::move(fn), event);
  }

  /**
   * Invokes an observer interface method on all observers.
   *
   * @param  fn          Function to call for each observer that takes a pointer
   *                     to the observer and invokes the interface method.
   */
  void invokeInterfaceMethodAllObservers(
      folly::Function<void(ObserverInterface*, Observed*)>&& fn) noexcept {
    this->invokeInterfaceMethodImpl(obj_, std::move(fn), folly::none);
  }

  ObserverContainer(const ObserverContainer&) = delete;
  ObserverContainer(ObserverContainer&&) = delete;
  ObserverContainer& operator=(const ObserverContainer&) = delete;
  ObserverContainer& operator=(ObserverContainer&& rhs) = delete;

 private:
  StoreBase& getStore() override { return store_; }
  const StoreBase& getStoreConst() const override { return store_; }

  // Object being observed.
  Observed* obj_{nullptr};

  // Store that contains the observers in the container.
  ObserverContainerStore<Observer, StorePolicy> store_;

  // Enables objects to register constructor callbacks.
  //
  // This can be used to enable observers to be attached to all objects of a
  // given type immediately upon object construction.
  //
  // Initialized last and in ObserverContainer instead of ObserverContainerBase
  // to ensure that the container has completed initialization and is ready to
  // add observers when any constructor callbacks are called.
  ContainerConstructorCallbackList constructorCallbackList_{this};
};

} // namespace folly
