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

#include <folly/test/ObserverContainerTestUtil.h>

#include <folly/ObserverContainer.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

using namespace folly;
using namespace ::testing;

/**
 *
 * Test components.
 *
 */

template <typename T>
std::vector<T*> uniquePtrVecToRawPtrVec(
    const std::vector<std::unique_ptr<T>>& input) {
  std::vector<T*> output;
  for (const auto& uptr : input) {
    output.push_back(uptr.get());
  }
  return output;
}

enum class TestObserverEvents : uint8_t {
  SpecialEvent = 1,
  SuperSpecialEvent = 2
};

using TestObserverContainerPolicy =
    ObserverContainerBasePolicyDefault<TestObserverEvents, 8>;

template <typename ObservedT>
class TestObserverInterface {
 public:
  virtual ~TestObserverInterface() = default;
  virtual void special(ObservedT* /* obj */) noexcept {}
  virtual void superSpecial(ObservedT* /* obj */) noexcept {}
  virtual void broadcast(ObservedT* /* obj */) noexcept {}
};

class TestSubject {
 public:
  TestSubject() : observerCtr(this) {}
  explicit TestSubject(TestSubject&& old)
      : observerCtr(this, std::move(old.observerCtr)) {}
  using ObserverContainer = ObserverContainer<
      TestObserverInterface<TestSubject>,
      TestSubject,
      TestObserverContainerPolicy>;

  void doSomethingSpecial() {
    observerCtr.invokeInterfaceMethod<TestObserverEvents::SpecialEvent>(
        [](auto observer, auto observed) { observer->special(observed); });
  }

  void doSomethingSuperSpecial() {
    observerCtr.invokeInterfaceMethod<TestObserverEvents::SuperSpecialEvent>(
        [](auto observer, auto observed) { observer->superSpecial(observed); });
  }

  void doBroadcast() { // no event enum associated, so "always on"
    observerCtr.invokeInterfaceMethodAllObservers(
        [](auto observer, auto observed) { observer->broadcast(observed); });
  }

  ObserverContainer observerCtr;
};

template <typename ObserverContainerT>
class MockTestSubjectObserver : public MockObserver<ObserverContainerT> {
 public:
  using TestSubjectT = typename ObserverContainerT::observed_type;

  // inherit constructor
  using MockObserver<ObserverContainerT>::MockObserver;

  MOCK_METHOD1(specialMock, void(TestSubjectT*));
  MOCK_METHOD1(superSpecialMock, void(TestSubjectT*));
  MOCK_METHOD1(broadcastMock, void(TestSubjectT*));

 private:
  void special(TestSubjectT* obj) noexcept override { specialMock(obj); }
  void superSpecial(TestSubjectT* obj) noexcept override {
    superSpecialMock(obj);
  }
  void broadcast(TestSubjectT* obj) noexcept override { broadcastMock(obj); }
};

template <typename ObserverContainerT>
class MockTestSubjectManagedObserver
    : public MockManagedObserver<ObserverContainerT> {
 public:
  using TestSubjectT = typename ObserverContainerT::observed_type;

  // inherit constructor
  using MockManagedObserver<ObserverContainerT>::MockManagedObserver;

  MOCK_METHOD1(specialMock, void(TestSubjectT*));
  MOCK_METHOD1(superSpecialMock, void(TestSubjectT*));
  MOCK_METHOD1(broadcastMock, void(TestSubjectT*));

 private:
  void special(TestSubjectT* obj) noexcept override { specialMock(obj); }
  void superSpecial(TestSubjectT* obj) noexcept override {
    superSpecialMock(obj);
  }
  void broadcast(TestSubjectT* obj) noexcept override { broadcastMock(obj); }
};

template <typename ObserverContainerT>
class MockTestSubjectManagedObserverSpecialized
    : public MockTestSubjectManagedObserver<ObserverContainerT> {
  using MockTestSubjectManagedObserver<
      ObserverContainerT>::MockTestSubjectManagedObserver;
};

/**
 *
 * Tests for ObserverContainer.
 *
 */

class ObserverContainerTest : public ::testing::Test {};

struct RemovalTrackingState {
  size_t detached{0};
  size_t destroyed{0};
  size_t detachedAtDestruction{0};
  bool unobservingAtDestruction{false};
};

class RemovalTrackingObserver
    : public TestSubject::ObserverContainer::ManagedObserver {
 public:
  explicit RemovalTrackingObserver(RemovalTrackingState& state)
      : state_(state) {}

  ~RemovalTrackingObserver() override {
    state_.detachedAtDestruction = state_.detached;
    state_.unobservingAtDestruction = getObservedObject() == nullptr;
    ++state_.destroyed;
  }

 private:
  void detached(TestSubject* obj) noexcept override {
    ASSERT_EQ(0, state_.destroyed);
    ASSERT_EQ(obj, getObservedObject());
    ++state_.detached;
  }

  void broadcast(TestSubject* obj) noexcept override {
    ASSERT_TRUE(obj->observerCtr.removeObserver(this));
  }

  RemovalTrackingState& state_;
};

TEST_F(ObserverContainerTest, RawRemovalRetainsStoreOwnedObserver) {
  for (bool duringDispatch : {false, true}) {
    SCOPED_TRACE(duringDispatch);
    RemovalTrackingState state;
    TestSubject subject;
    auto observer = std::make_shared<RemovalTrackingObserver>(state);
    auto* rawObserver = observer.get();
    std::weak_ptr<RemovalTrackingObserver> weakObserver = observer;
    subject.observerCtr.addObserver(observer);
    observer.reset();

    ASSERT_FALSE(weakObserver.expired());
    if (duringDispatch) {
      subject.doBroadcast();
    } else {
      ASSERT_TRUE(subject.observerCtr.removeObserver(rawObserver));
    }

    ASSERT_TRUE(weakObserver.expired());
    ASSERT_EQ(1, state.detached);
    ASSERT_EQ(1, state.destroyed);
    ASSERT_EQ(1, state.detachedAtDestruction);
    ASSERT_TRUE(state.unobservingAtDestruction);
    ASSERT_EQ(0, subject.observerCtr.numObservers());
  }
}

TEST_F(ObserverContainerTest, SharedRemovalRetainsRawRegisteredObserver) {
  for (bool duringDispatch : {false, true}) {
    SCOPED_TRACE(duringDispatch);
    RemovalTrackingState state;
    TestSubject subject;
    auto observer = std::make_shared<RemovalTrackingObserver>(state);
    std::weak_ptr<RemovalTrackingObserver> weakObserver = observer;
    subject.observerCtr.addObserver(observer.get());

    if (duringDispatch) {
      subject.observerCtr.invokeInterfaceMethodAllObservers([&](auto*, auto*) {
        ASSERT_TRUE(subject.observerCtr.removeObserver(std::move(observer)));
      });
    } else {
      ASSERT_TRUE(subject.observerCtr.removeObserver(std::move(observer)));
    }

    ASSERT_EQ(nullptr, observer);
    ASSERT_TRUE(weakObserver.expired());
    ASSERT_EQ(1, state.detached);
    ASSERT_EQ(1, state.destroyed);
    ASSERT_EQ(1, state.detachedAtDestruction);
    ASSERT_TRUE(state.unobservingAtDestruction);
    ASSERT_EQ(0, subject.observerCtr.numObservers());
  }
}

/**
 * Wrapping class for MockObserver managed with various smart pointers.
 *
 * Used to enable TYPED_TEST with different observer configurations.
 */
class WrappedMockObserver {
 public:
  enum class MockType { Strict };
  using EventSet = TestSubject::ObserverContainer::Observer::EventSet;
  using ObserverType = TestSubject::ObserverContainer::Observer;
  using MockObserverType =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;

  WrappedMockObserver() = default;
  virtual ~WrappedMockObserver() = default;

  /**
   * Release shared_ptr or unique_ptr for MockObserver object.
   *
   * If no other smart pointer is keeping the observer alive, the observer will
   * be destroyed.
   */
  virtual void release() = 0;

  /**
   * Get a reference to the mock object for use in EXPECT_CALL operations.
   */
  virtual MockObserverType& getMock() = 0;

  /**
   * Get a pointer to the observer object for use in lookup operations.
   *
   * Do not use this for addObserver or removeObserver. Instead, rely on the
   * implicit operator conversions.
   */
  virtual ObserverType* getPtr() = 0;
};

/*
 * Wrapping class for MockObserver managed with unique_ptr.
 */
class WrappedUniquePtrMockObserver : public WrappedMockObserver {
 public:
  WrappedUniquePtrMockObserver(const MockType mockType, const EventSet eventSet)
      : mockObserver_(makeMock(mockType, eventSet)) {}
  ~WrappedUniquePtrMockObserver() override = default;

  void release() override { mockObserver_.reset(); }

  MockObserverType& getMock() override { return *mockObserver_.get(); }

  MockObserverType* getPtr() override { return mockObserver_.get(); }

  /*
   * Conversion operator for use with addObserver() and removeObserver().
   */
  operator ObserverType*() { return mockObserver_.get(); }

 private:
  std::unique_ptr<MockObserverType> makeMock(
      MockType mockType, const EventSet eventSet) {
    switch (mockType) {
      case MockType::Strict:
        return std::make_unique<StrictMock<MockObserverType>>(eventSet);
    }
  }

  std::unique_ptr<MockObserverType> mockObserver_;
};

/*
 * Wrapping class for MockObserver managed with shared_ptr.
 */
class WrappedSharedPtrMockObserver : public WrappedMockObserver {
 public:
  WrappedSharedPtrMockObserver(const MockType mockType, const EventSet eventSet)
      : mockObserver_(makeMock(mockType, eventSet)) {}
  ~WrappedSharedPtrMockObserver() override = default;

  void release() override { mockObserver_.reset(); }

  MockObserverType& getMock() override { return *mockObserver_.get(); }

  MockObserverType* getPtr() override { return mockObserver_.get(); }

  /*
   * Conversion operator for use with addObserver() and removeObserver().
   */
  operator std::shared_ptr<ObserverType>() { return mockObserver_; }

 private:
  std::shared_ptr<MockObserverType> makeMock(
      MockType mockType, const EventSet eventSet) {
    switch (mockType) {
      case MockType::Strict:
        return std::make_shared<StrictMock<MockObserverType>>(eventSet);
    }
  }

  std::shared_ptr<MockObserverType> mockObserver_;
};

/*
 * Wrapping class for MockObserver managed with shared_ptr.
 *
 * The wrapper transfers ownership of the shared_ptr to the caller the first
 * time it is accessed, and relies on a weak_ptr to get a copy of the shared_ptr
 * from then onwards. This enables us to test scenarios where the only handle
 * keeping the observer alive is the one held by the container.
 */
class WrappedSharedPtrSingleHandleMockObserver : public WrappedMockObserver {
 public:
  WrappedSharedPtrSingleHandleMockObserver(
      const MockType mockType, const EventSet eventSet)
      : mockObserver_(makeMock(mockType, eventSet)),
        weakMockObserver_(mockObserver_) {}
  ~WrappedSharedPtrSingleHandleMockObserver() override = default;

  void release() override { mockObserver_.reset(); }

  MockObserverType& getMock() override {
    std::shared_ptr<MockObserverType> mockObserver(weakMockObserver_); // throws
    return *mockObserver.get();
  }

  MockObserverType* getPtr() override {
    std::shared_ptr<MockObserverType> mockObserver(weakMockObserver_); // throws
    return mockObserver.get();
  }

  /*
   * Conversion operator for use with addObserver() and removeObserver().
   *
   * The first call will transfer ownership of the shared_ptr to the caller.
   * Subsequent calls use the weak_ptr to get a shared_ptr.
   */
  operator std::shared_ptr<ObserverType>() {
    // first call, we transfer out the handle for the mock observer
    if (mockObserver_) {
      auto mockObserver = std::move(mockObserver_);
      mockObserver_.reset();
      return mockObserver;
    }

    // subsequent calls use the weak
    // this should work as long as the observer hasn't been destroyed
    std::shared_ptr<MockObserverType> mockObserver(weakMockObserver_); // throws
    return mockObserver;
  }

 private:
  std::shared_ptr<MockObserverType> makeMock(
      MockType mockType, const EventSet eventSet) {
    switch (mockType) {
      case MockType::Strict:
        return std::make_shared<StrictMock<MockObserverType>>(eventSet);
    }
  }

  std::shared_ptr<MockObserverType> mockObserver_;
  std::weak_ptr<MockObserverType> weakMockObserver_;
};

TEST(WrappedSharedPtrMockObserver, CheckBehavior) {
  using WrapperType = WrappedSharedPtrMockObserver;
  WrapperType wrappedObserver(
      WrapperType::MockType::Strict,
      WrapperType::ObserverType::EventSetBuilder().enableAllEvents().build());

  // verify that the handle is NOT moved to caller
  {
    std::shared_ptr<WrapperType::ObserverType> sptr = wrappedObserver;
    EXPECT_EQ(2, sptr.use_count()); // two handles
  }

  // test getMock
  {
    auto ptr = &wrappedObserver.getMock();
    std::shared_ptr<WrapperType::ObserverType> sptr = wrappedObserver;
    EXPECT_EQ(sptr.get(), ptr);
  }

  // test getPtr
  {
    auto ptr = wrappedObserver.getPtr();
    std::shared_ptr<WrapperType::ObserverType> sptr = wrappedObserver;
    EXPECT_EQ(sptr.get(), ptr);
  }

  // test release
  {
    std::shared_ptr<WrapperType::ObserverType> sptr = wrappedObserver;
    EXPECT_EQ(2, sptr.use_count()); // two handles
    wrappedObserver.release();
    EXPECT_EQ(1, sptr.use_count()); // one handle
  }
}

TEST(WrappedSharedPtrSingleHandleMockObserver, CheckBehavior) {
  using WrapperType = WrappedSharedPtrSingleHandleMockObserver;
  WrapperType wrappedObserver(
      WrapperType::MockType::Strict,
      WrapperType::ObserverType::EventSetBuilder().enableAllEvents().build());

  // verify that the handle is moved to caller
  std::shared_ptr<WrapperType::ObserverType> sptr = wrappedObserver;
  EXPECT_EQ(1, sptr.use_count()); // one handle

  // verify that wrapper returns shared_ptr via weak_ptr after first time
  {
    std::shared_ptr<WrapperType::ObserverType> sptr2 = wrappedObserver;
    EXPECT_EQ(sptr, sptr2);
    EXPECT_EQ(2, sptr.use_count()); // two handles
  }
  EXPECT_EQ(1, sptr.use_count()); // one handle

  // test getMock
  {
    auto ptr = &wrappedObserver.getMock();
    EXPECT_EQ(sptr.get(), ptr);
  }

  // test getPtr
  {
    auto ptr = wrappedObserver.getPtr();
    EXPECT_EQ(sptr.get(), ptr);
  }

  // test release
  {
    wrappedObserver.release();
    EXPECT_EQ(1, sptr.use_count()); // one handle

    // since we hold on to weak ptr, can still shared_ptr
    std::shared_ptr<WrapperType::ObserverType> sptr2 = wrappedObserver;
    EXPECT_EQ(sptr, sptr2);
    EXPECT_EQ(2, sptr.use_count()); // two handles
  }

  // destroy sptr, verify all functions fail
  sptr = nullptr;
  EXPECT_THROW(wrappedObserver.getMock(), std::bad_weak_ptr);
  EXPECT_THROW(wrappedObserver.getPtr(), std::bad_weak_ptr);
  EXPECT_THROW(
      std::shared_ptr<WrapperType::ObserverType> sptr2 = wrappedObserver,
      std::bad_weak_ptr);
}

template <class T>
class ObserverContainerTypedTest : public ObserverContainerTest {};
using ObserverContainerTypedTestTypes = testing::Types<
    WrappedUniquePtrMockObserver,
    WrappedSharedPtrMockObserver,
    WrappedSharedPtrSingleHandleMockObserver>;
TYPED_TEST_SUITE(ObserverContainerTypedTest, ObserverContainerTypedTestTypes);

/**
 * Ensure no issue if container is never used.
 */
TYPED_TEST(ObserverContainerTypedTest, CtrObserverNeverAttached) {
  auto obj1 = std::make_unique<TestSubject>();
}

/**
 * Ensure no issue if container is never used.
 */
TYPED_TEST(ObserverContainerTypedTest, CtrObserverNeverAttachedWithChecks) {
  auto obj1 = std::make_unique<TestSubject>();
  EXPECT_EQ(0, obj1->observerCtr.numObservers());
  EXPECT_THAT(obj1->observerCtr.getObservers(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      IsEmpty());
}

/**
 * Test add / remove.
 */
TYPED_TEST(ObserverContainerTypedTest, CtrObserverAddRemove) {
  using WrappedMockObserverT = TypeParam;
  using EventSetBuilder =
      TestSubject::ObserverContainer::Observer::EventSetBuilder;
  using MockType = typename WrappedMockObserverT::MockType;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  EXPECT_EQ(0, obj1->observerCtr.numObservers());
  EXPECT_THAT(obj1->observerCtr.getObservers(), IsEmpty());
  EXPECT_THAT(obj1->observerCtr.findObservers(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      IsEmpty());

  WrappedMockObserverT observer1(
      MockType::Strict, EventSetBuilder().enableAllEvents().build());
  EXPECT_CALL(
      observer1.getMock(), addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(observer1.getMock(), attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1); // implicit cast per type

  EXPECT_EQ(1, obj1->observerCtr.numObservers());
  EXPECT_THAT(
      obj1->observerCtr.getObservers(),
      UnorderedElementsAre(observer1.getPtr()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers(),
      UnorderedElementsAre(observer1.getPtr()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(observer1.getPtr()));

  EXPECT_CALL(observer1.getMock(), detachedMock(obj1.get()));
  EXPECT_CALL(
      observer1.getMock(),
      removedFromObserverContainerMock(&obj1->observerCtr));
  obj1->observerCtr.removeObserver(observer1); // implicit cast per type

  EXPECT_EQ(0, obj1->observerCtr.numObservers());
  EXPECT_THAT(obj1->observerCtr.getObservers(), IsEmpty());
  EXPECT_THAT(obj1->observerCtr.findObservers(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      IsEmpty());
}

/**
 * Test moving an ObserverContainer.
 *
 * Post move action: Observer removed and destroyed prior to subject destroy.
 */
TYPED_TEST(ObserverContainerTypedTest, CtrMoveThenRemoveAndDestroyObserver) {
  using WrappedMockObserverT = TypeParam;
  using EventSetBuilder =
      TestSubject::ObserverContainer::Observer::EventSetBuilder;
  using MockType = typename WrappedMockObserverT::MockType;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  EXPECT_EQ(0, obj1->observerCtr.numObservers());
  EXPECT_THAT(obj1->observerCtr.getObservers(), IsEmpty());
  EXPECT_THAT(obj1->observerCtr.findObservers(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      IsEmpty());

  WrappedMockObserverT observer1(
      MockType::Strict, EventSetBuilder().enableAllEvents().build());
  typename WrappedMockObserverT::ObserverType::Safety dc(observer1.getMock());
  EXPECT_CALL(
      observer1.getMock(), addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(observer1.getMock(), attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1); // implicit cast per type

  EXPECT_EQ(1, obj1->observerCtr.numObservers());
  EXPECT_THAT(
      obj1->observerCtr.getObservers(),
      UnorderedElementsAre(observer1.getPtr()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers(),
      UnorderedElementsAre(observer1.getPtr()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(observer1.getPtr()));

  EXPECT_CALL(
      observer1.getMock(), movedToObserverContainerMock(&obj1->observerCtr, _));
  EXPECT_CALL(observer1.getMock(), movedMock(obj1.get(), _, _));

  auto obj2 = std::make_unique<TestSubject>(std::move(*obj1));

  EXPECT_EQ(obj2->observerCtr.numObservers(), 1);
  EXPECT_EQ(obj1->observerCtr.numObservers(), 0);

  EXPECT_THAT(obj1->observerCtr.getObservers(), IsEmpty());
  EXPECT_THAT(obj1->observerCtr.findObservers(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      IsEmpty());

  EXPECT_THAT(
      obj2->observerCtr.getObservers(),
      UnorderedElementsAre(observer1.getPtr()));
  EXPECT_THAT(
      obj2->observerCtr.findObservers(),
      UnorderedElementsAre(observer1.getPtr()));
  EXPECT_THAT(
      obj2->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(observer1.getPtr()));

  EXPECT_CALL(observer1.getMock(), detachedMock(obj2.get()));
  EXPECT_CALL(
      observer1.getMock(),
      removedFromObserverContainerMock(&obj2->observerCtr));
  obj2->observerCtr.removeObserver(observer1); // implicit cast per type

  observer1.release(); // release the observer from wrapper; should destroy

  EXPECT_EQ(obj2->observerCtr.numObservers(), 0);
  EXPECT_EQ(obj1->observerCtr.numObservers(), 0);
}

/**
 * Test moving an ObserverContainer.
 *
 * Post move action: Observed object destroyed.
 */
TYPED_TEST(ObserverContainerTypedTest, CtrMoveThenDestroyObserved) {
  using WrappedMockObserverT = TypeParam;
  using EventSetBuilder =
      TestSubject::ObserverContainer::Observer::EventSetBuilder;
  using MockType = typename WrappedMockObserverT::MockType;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  EXPECT_EQ(0, obj1->observerCtr.numObservers());
  EXPECT_THAT(obj1->observerCtr.getObservers(), IsEmpty());
  EXPECT_THAT(obj1->observerCtr.findObservers(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      IsEmpty());

  WrappedMockObserverT observer1(
      MockType::Strict, EventSetBuilder().enableAllEvents().build());
  typename WrappedMockObserverT::ObserverType::Safety dc(observer1.getMock());
  EXPECT_CALL(
      observer1.getMock(), addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(observer1.getMock(), attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1); // implicit cast per type

  EXPECT_EQ(1, obj1->observerCtr.numObservers());
  EXPECT_THAT(
      obj1->observerCtr.getObservers(),
      UnorderedElementsAre(observer1.getPtr()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers(),
      UnorderedElementsAre(observer1.getPtr()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(observer1.getPtr()));

  EXPECT_CALL(
      observer1.getMock(), movedToObserverContainerMock(&obj1->observerCtr, _));
  EXPECT_CALL(observer1.getMock(), movedMock(obj1.get(), _, _));

  auto obj2 = std::make_unique<TestSubject>(std::move(*obj1));

  EXPECT_EQ(obj2->observerCtr.numObservers(), 1);
  EXPECT_EQ(obj1->observerCtr.numObservers(), 0);

  EXPECT_THAT(obj1->observerCtr.getObservers(), IsEmpty());
  EXPECT_THAT(obj1->observerCtr.findObservers(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      IsEmpty());

  EXPECT_THAT(
      obj2->observerCtr.getObservers(),
      UnorderedElementsAre(observer1.getPtr()));
  EXPECT_THAT(
      obj2->observerCtr.findObservers(),
      UnorderedElementsAre(observer1.getPtr()));
  EXPECT_THAT(
      obj2->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(observer1.getPtr()));

  obj1 = nullptr; // nothing should occur

  EXPECT_CALL(observer1.getMock(), destroyedMock(obj2.get(), _));
  EXPECT_CALL(
      observer1.getMock(),
      removedFromObserverContainerMock(&obj2->observerCtr));
  obj2 = nullptr;
}

/**
 * Test moving a ObserverContainer with single handle shared_ptr observers.
 *
 * Verifies that moves work as expected when the shared_ptr within the container
 * is the only thing keeping the observer alive. `CtrMoveThenDestroyObserved`
 * typed test with type `WrappedSharedPtrSingleHandleMockObserver` also tests
 * this functionality; this is an additional explicit test.
 *
 * Post move action: Observed object destroyed.
 */
TEST_F(ObserverContainerTest, CtrWithSharedPtrMoveThenDestroyObserved) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  EXPECT_EQ(0, obj1->observerCtr.numObservers());
  EXPECT_THAT(obj1->observerCtr.getObservers(), IsEmpty());
  EXPECT_THAT(obj1->observerCtr.findObservers(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      IsEmpty());

  auto observer1 = std::make_shared<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  typename MockTestSubjectObserver::Safety dc(*observer1);
  EXPECT_CALL(*observer1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1);

  EXPECT_EQ(1, obj1->observerCtr.numObservers());
  EXPECT_THAT(
      obj1->observerCtr.getObservers(), UnorderedElementsAre(observer1.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers(), UnorderedElementsAre(observer1.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(observer1.get()));

  // now that observer1 is attached, we release shared_ptr but keep weak & raw
  // since the container holds shared_ptr too, observer should not be destroyed
  auto observer1Raw = observer1.get();
  std::weak_ptr<MockTestSubjectObserver> observer1Weak = observer1;
  observer1 = nullptr;
  ASSERT_FALSE(dc.destroyed()); // should still exist

  EXPECT_CALL(
      *observer1Raw, movedToObserverContainerMock(&obj1->observerCtr, _));
  EXPECT_CALL(*observer1Raw, movedMock(obj1.get(), _, _));

  auto obj2 = std::make_unique<TestSubject>(std::move(*obj1));

  EXPECT_EQ(obj2->observerCtr.numObservers(), 1);
  EXPECT_EQ(obj1->observerCtr.numObservers(), 0);

  EXPECT_THAT(obj1->observerCtr.getObservers(), IsEmpty());
  EXPECT_THAT(obj1->observerCtr.findObservers(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      IsEmpty());

  EXPECT_THAT(
      obj2->observerCtr.getObservers(), UnorderedElementsAre(observer1Raw));
  EXPECT_THAT(
      obj2->observerCtr.findObservers(), UnorderedElementsAre(observer1Raw));
  EXPECT_THAT(
      obj2->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(observer1Raw));

  obj1 = nullptr; // nothing should occur

  EXPECT_CALL(*observer1Raw, destroyedMock(obj2.get(), _));
  EXPECT_CALL(
      *observer1Raw, removedFromObserverContainerMock(&obj2->observerCtr));
  obj2 = nullptr;
}

/**
 * Ensure correct behavior for invokeInterfaceMethod.
 */
TYPED_TEST(ObserverContainerTypedTest, CtrInvoke) {
  using WrappedMockObserverT = TypeParam;
  using EventSetBuilder =
      TestSubject::ObserverContainer::Observer::EventSetBuilder;
  using MockType = typename WrappedMockObserverT::MockType;
  InSequence s;

  // first invoke with no observers
  auto obj1 = std::make_unique<TestSubject>();
  obj1->doSomethingSpecial();
  obj1->doSomethingSuperSpecial();

  // now add an observer and hit the events again to ensure it works
  WrappedMockObserverT observer1(
      MockType::Strict, EventSetBuilder().enableAllEvents().build());
  observer1.getMock().useDefaultInvokeMockHandler();
  observer1.getMock().useDefaultPostInvokeMockHandler();
  EXPECT_CALL(
      observer1.getMock(), addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(observer1.getMock(), attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1); // implicit cast per type

  EXPECT_CALL(observer1.getMock(), specialMock(obj1.get()));
  EXPECT_CALL(observer1.getMock(), superSpecialMock(obj1.get()));
  obj1->doSomethingSpecial();
  obj1->doSomethingSuperSpecial();

  // remove and destroy the observer
  EXPECT_CALL(observer1.getMock(), detachedMock(obj1.get()));
  EXPECT_CALL(
      observer1.getMock(),
      removedFromObserverContainerMock(&obj1->observerCtr));
  obj1->observerCtr.removeObserver(observer1); // implicit cast per type
  observer1.release();

  // once more, with no observers
  obj1->doSomethingSpecial();
  obj1->doSomethingSuperSpecial();

  obj1 = nullptr;
}

/**
 * Ensure correct behavior for invokeInterfaceMethod after moving observers.
 */
TYPED_TEST(ObserverContainerTypedTest, CtrInvokeAfterMove) {
  using WrappedMockObserverT = TypeParam;
  using EventSetBuilder =
      TestSubject::ObserverContainer::Observer::EventSetBuilder;
  using MockType = typename WrappedMockObserverT::MockType;
  InSequence s;

  // first invoke with no observers
  auto obj1 = std::make_unique<TestSubject>();
  obj1->doSomethingSpecial();
  obj1->doSomethingSuperSpecial();

  // now add an observer and hit the events again to ensure it works
  WrappedMockObserverT observer1(
      MockType::Strict, EventSetBuilder().enableAllEvents().build());
  observer1.getMock().useDefaultInvokeMockHandler();
  observer1.getMock().useDefaultPostInvokeMockHandler();
  EXPECT_CALL(
      observer1.getMock(), addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(observer1.getMock(), attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1); // implicit cast per type

  EXPECT_CALL(observer1.getMock(), specialMock(obj1.get()));
  EXPECT_CALL(observer1.getMock(), superSpecialMock(obj1.get()));
  obj1->doSomethingSpecial();
  obj1->doSomethingSuperSpecial();

  // move the observer
  EXPECT_CALL(
      observer1.getMock(), movedToObserverContainerMock(&obj1->observerCtr, _));
  EXPECT_CALL(observer1.getMock(), movedMock(obj1.get(), _, _));

  auto obj2 = std::make_unique<TestSubject>(std::move(*obj1));

  EXPECT_EQ(obj2->observerCtr.numObservers(), 1);
  EXPECT_EQ(obj1->observerCtr.numObservers(), 0);

  // hit the events again after the move
  EXPECT_CALL(observer1.getMock(), specialMock(obj2.get()));
  EXPECT_CALL(observer1.getMock(), superSpecialMock(obj2.get()));
  obj2->doSomethingSpecial();
  obj2->doSomethingSuperSpecial();

  // hit the events again on obj1, nothing should happen
  EXPECT_CALL(observer1.getMock(), specialMock(obj1.get())).Times(0);
  EXPECT_CALL(observer1.getMock(), superSpecialMock(obj1.get())).Times(0);
  obj1->doSomethingSpecial();
  obj1->doSomethingSuperSpecial();

  EXPECT_CALL(observer1.getMock(), destroyedMock(obj2.get(), _));
  EXPECT_CALL(
      observer1.getMock(),
      removedFromObserverContainerMock(&obj2->observerCtr));
}

/**
 * Ensure that invokeInterfaceMethod handles EventSets properly.
 */
TEST_F(ObserverContainerTest, CtrInvokeMultipleObserversWithEventSets) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectObserver>> observers;

  // observer 1 is subscribed to nothing
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>());
  }

  // observer 2 is subscribed to SpecialEvent and SuperSpecialEvent explicitly
  // subscription is performed in two separate calls to enable()
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enable(TestObserverEvents::SpecialEvent);
    eventSet.enable(TestObserverEvents::SuperSpecialEvent);
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // observer 3 is subscribed to SpecialEvent and SuperSpecialEvent explicitly
  // subscription is performed in a single call to enable()
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enable(
        TestObserverEvents::SpecialEvent,
        TestObserverEvents::SuperSpecialEvent);
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // observer 4 is subscribed to all events via enableAllEvents()
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enableAllEvents();
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // observer 5 is subscribed to just SpecialEvent
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enable(TestObserverEvents::SpecialEvent);
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // observer 6 is subscribed to just SuperSpecialEvent
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enable(TestObserverEvents::SuperSpecialEvent);
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // add the observers
  for (const auto& observer : observers) {
    observer->useDefaultInvokeMockHandler();
    observer->useDefaultPostInvokeMockHandler();
    ;
    EXPECT_CALL(*observer, addedToObserverContainerMock(&obj1->observerCtr));
    EXPECT_CALL(*observer, attachedMock(obj1.get()));
    obj1->observerCtr.addObserver(observer.get());
  }
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(),
      UnorderedElementsAreArray(uniquePtrVecToRawPtrVec(observers)));

  // trigger multiple times
  for (auto i = 0; i < 3; i++) {
    // set up expectations, then trigger events
    //
    // everyone gets doBroadcast and...
    //// observer 1 should get no other events
    //// observer 2 -> 4 should get SpecialEvent + SuperSpecialEvent
    //// observer 5 should get SpecialEvent
    //// observer 6 should get SuperSpecialEvent
    EXPECT_CALL(*observers[0], broadcastMock(obj1.get()));
    EXPECT_CALL(*observers[1], broadcastMock(obj1.get()));
    EXPECT_CALL(*observers[2], broadcastMock(obj1.get()));
    EXPECT_CALL(*observers[3], broadcastMock(obj1.get()));
    EXPECT_CALL(*observers[4], broadcastMock(obj1.get()));
    EXPECT_CALL(*observers[5], broadcastMock(obj1.get()));
    obj1->doBroadcast();

    EXPECT_CALL(*observers[0], specialMock(obj1.get())).Times(0);
    EXPECT_CALL(*observers[1], specialMock(obj1.get()));
    EXPECT_CALL(*observers[2], specialMock(obj1.get()));
    EXPECT_CALL(*observers[3], specialMock(obj1.get()));
    EXPECT_CALL(*observers[4], specialMock(obj1.get()));
    EXPECT_CALL(*observers[5], specialMock(obj1.get())).Times(0);
    obj1->doSomethingSpecial();

    EXPECT_CALL(*observers[0], superSpecialMock(obj1.get())).Times(0);
    EXPECT_CALL(*observers[1], superSpecialMock(obj1.get()));
    EXPECT_CALL(*observers[2], superSpecialMock(obj1.get()));
    EXPECT_CALL(*observers[3], superSpecialMock(obj1.get()));
    EXPECT_CALL(*observers[4], superSpecialMock(obj1.get())).Times(0);
    EXPECT_CALL(*observers[5], superSpecialMock(obj1.get()));
    obj1->doSomethingSuperSpecial();
  }

  // destroy object
  for (const auto& observer : observers) {
    EXPECT_CALL(*observer, destroyedMock(obj1.get(), _));
    EXPECT_CALL(
        *observer, removedFromObserverContainerMock(&obj1->observerCtr));
  }
  obj1 = nullptr;
}

/**
 * Ensure that invokeInterfaceMethod and postInvokeInterfaceMethod are called.
 */
TEST_F(
    ObserverContainerTest,
    CtrInvokeMultipleObserversWithEventSetsOverrideInvoke) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectObserver>> observers;

  // observer 1 is subscribed to nothing
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>());
  }

  // observer 2 is subscribed to SpecialEvent and SuperSpecialEvent explicitly
  // subscription is performed in two separate calls to enable()
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enable(TestObserverEvents::SpecialEvent);
    eventSet.enable(TestObserverEvents::SuperSpecialEvent);
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // observer 3 is subscribed to SpecialEvent and SuperSpecialEvent explicitly
  // subscription is performed in a single call to enable()
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enable(
        TestObserverEvents::SpecialEvent,
        TestObserverEvents::SuperSpecialEvent);
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // observer 4 is subscribed to all events via enableAllEvents()
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enableAllEvents();
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // observer 5 is subscribed to just SpecialEvent
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enable(TestObserverEvents::SpecialEvent);
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // observer 6 is subscribed to just SuperSpecialEvent
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enable(TestObserverEvents::SuperSpecialEvent);
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // add the observers
  for (const auto& observer : observers) {
    EXPECT_CALL(*observer, addedToObserverContainerMock(&obj1->observerCtr));
    EXPECT_CALL(*observer, attachedMock(obj1.get()));
    obj1->observerCtr.addObserver(observer.get());
  }
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(),
      UnorderedElementsAreArray(uniquePtrVecToRawPtrVec(observers)));

  // trigger multiple times
  for (auto i = 0; i < 3; i++) {
    // set up expectations, then trigger events
    //
    // everyone gets doBroadcast and...
    //// observer 1 should get no other events
    //// observer 2 -> 4 should get SpecialEvent + SuperSpecialEvent
    //// observer 5 should get SpecialEvent
    //// observer 6 should get SuperSpecialEvent
    //
    // since we're not using the default handlers for invoke and postInvoke,
    // we should see the mocked invoke and postInvoke being called

    // broadcast
    EXPECT_CALL(
        *observers[0],
        invokeInterfaceMethodMock(
            obj1.get(), _, folly::Optional<TestObserverEvents>()));
    EXPECT_CALL(
        *observers[1],
        invokeInterfaceMethodMock(
            obj1.get(), _, folly::Optional<TestObserverEvents>()));
    EXPECT_CALL(
        *observers[2],
        invokeInterfaceMethodMock(
            obj1.get(), _, folly::Optional<TestObserverEvents>()));
    EXPECT_CALL(
        *observers[3],
        invokeInterfaceMethodMock(
            obj1.get(), _, folly::Optional<TestObserverEvents>()));
    EXPECT_CALL(
        *observers[4],
        invokeInterfaceMethodMock(
            obj1.get(), _, folly::Optional<TestObserverEvents>()));
    EXPECT_CALL(
        *observers[5],
        invokeInterfaceMethodMock(
            obj1.get(), _, folly::Optional<TestObserverEvents>()));
    EXPECT_CALL(*observers[0], postInvokeInterfaceMethodMock(obj1.get()));
    EXPECT_CALL(*observers[1], postInvokeInterfaceMethodMock(obj1.get()));
    EXPECT_CALL(*observers[2], postInvokeInterfaceMethodMock(obj1.get()));
    EXPECT_CALL(*observers[3], postInvokeInterfaceMethodMock(obj1.get()));
    EXPECT_CALL(*observers[4], postInvokeInterfaceMethodMock(obj1.get()));
    EXPECT_CALL(*observers[5], postInvokeInterfaceMethodMock(obj1.get()));
    obj1->doBroadcast();

    // special event
    EXPECT_CALL(
        *observers[0],
        invokeInterfaceMethodMock(
            obj1.get(),
            _,
            folly::Optional<TestObserverEvents>(
                TestObserverEvents::SpecialEvent)))
        .Times(0);
    EXPECT_CALL(
        *observers[1],
        invokeInterfaceMethodMock(
            obj1.get(),
            _,
            folly::Optional<TestObserverEvents>(
                TestObserverEvents::SpecialEvent)));
    EXPECT_CALL(
        *observers[2],
        invokeInterfaceMethodMock(
            obj1.get(),
            _,
            folly::Optional<TestObserverEvents>(
                TestObserverEvents::SpecialEvent)));
    EXPECT_CALL(
        *observers[3],
        invokeInterfaceMethodMock(
            obj1.get(),
            _,
            folly::Optional<TestObserverEvents>(
                TestObserverEvents::SpecialEvent)));
    EXPECT_CALL(
        *observers[4],
        invokeInterfaceMethodMock(
            obj1.get(),
            _,
            folly::Optional<TestObserverEvents>(
                TestObserverEvents::SpecialEvent)));
    EXPECT_CALL(
        *observers[5],
        invokeInterfaceMethodMock(
            obj1.get(),
            _,
            folly::Optional<TestObserverEvents>(
                TestObserverEvents::SpecialEvent)))
        .Times(0);
    EXPECT_CALL(*observers[0], postInvokeInterfaceMethodMock(obj1.get()))
        .Times(0);
    EXPECT_CALL(*observers[1], postInvokeInterfaceMethodMock(obj1.get()));
    EXPECT_CALL(*observers[2], postInvokeInterfaceMethodMock(obj1.get()));
    EXPECT_CALL(*observers[3], postInvokeInterfaceMethodMock(obj1.get()));
    EXPECT_CALL(*observers[4], postInvokeInterfaceMethodMock(obj1.get()));
    EXPECT_CALL(*observers[5], postInvokeInterfaceMethodMock(obj1.get()))
        .Times(0);
    obj1->doSomethingSpecial();

    // super special event
    EXPECT_CALL(
        *observers[0],
        invokeInterfaceMethodMock(
            obj1.get(),
            _,
            folly::Optional<TestObserverEvents>(
                TestObserverEvents::SuperSpecialEvent)))
        .Times(0);
    EXPECT_CALL(
        *observers[1],
        invokeInterfaceMethodMock(
            obj1.get(),
            _,
            folly::Optional<TestObserverEvents>(
                TestObserverEvents::SuperSpecialEvent)));
    EXPECT_CALL(
        *observers[2],
        invokeInterfaceMethodMock(
            obj1.get(),
            _,
            folly::Optional<TestObserverEvents>(
                TestObserverEvents::SuperSpecialEvent)));
    EXPECT_CALL(
        *observers[3],
        invokeInterfaceMethodMock(
            obj1.get(),
            _,
            folly::Optional<TestObserverEvents>(
                TestObserverEvents::SuperSpecialEvent)));
    EXPECT_CALL(
        *observers[4],
        invokeInterfaceMethodMock(
            obj1.get(),
            _,
            folly::Optional<TestObserverEvents>(
                TestObserverEvents::SuperSpecialEvent)))
        .Times(0);
    EXPECT_CALL(
        *observers[5],
        invokeInterfaceMethodMock(
            obj1.get(),
            _,
            folly::Optional<TestObserverEvents>(
                TestObserverEvents::SuperSpecialEvent)));
    EXPECT_CALL(*observers[0], postInvokeInterfaceMethodMock(obj1.get()))
        .Times(0);
    EXPECT_CALL(*observers[1], postInvokeInterfaceMethodMock(obj1.get()));
    EXPECT_CALL(*observers[2], postInvokeInterfaceMethodMock(obj1.get()));
    EXPECT_CALL(*observers[3], postInvokeInterfaceMethodMock(obj1.get()));
    EXPECT_CALL(*observers[4], postInvokeInterfaceMethodMock(obj1.get()))
        .Times(0);
    EXPECT_CALL(*observers[5], postInvokeInterfaceMethodMock(obj1.get()));
    obj1->doSomethingSuperSpecial();
  }

  // destroy object
  for (const auto& observer : observers) {
    EXPECT_CALL(*observer, destroyedMock(obj1.get(), _));
    EXPECT_CALL(
        *observer, removedFromObserverContainerMock(&obj1->observerCtr));
  }
  obj1 = nullptr;
}

/**
 * Add observer during event processing.
 */
TEST_F(ObserverContainerTest, CtrInvokeAddObserverOnInvoke) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectObserver>> observers;

  auto obs1 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs2 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs3 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  obs1->useDefaultInvokeMockHandler();
  obs2->useDefaultInvokeMockHandler();
  obs3->useDefaultInvokeMockHandler();

  // add observers 1 and 2
  EXPECT_CALL(*obs1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs1.get());

  EXPECT_CALL(*obs2, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs2.get());

  EXPECT_CALL(*obs1, specialMock(obj1.get()))
      .Times(1)
      .WillOnce(InvokeWithoutArgs([&obj1, &obs3]() {
        obj1->observerCtr.addObserver(obs3.get());
      }));
  EXPECT_CALL(*obs3, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs3, attachedMock(obj1.get()));

  EXPECT_CALL(*obs2, specialMock(obj1.get()));
  EXPECT_CALL(*obs3, specialMock(obj1.get()));
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*obs1, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs2, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs3, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get()));
  obj1->doSomethingSuperSpecial();

  EXPECT_CALL(*obs1, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs1, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs2, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs3, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs3, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1 = nullptr;
}

/**
 * Add two observers during event processing.
 */
TEST_F(ObserverContainerTest, CtrInvokeAddTwoObserversOnInvokeFirst) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectObserver>> observers;

  auto obs1 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs2 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs3 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs4 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  obs1->useDefaultInvokeMockHandler();
  obs2->useDefaultInvokeMockHandler();
  obs3->useDefaultInvokeMockHandler();
  obs4->useDefaultInvokeMockHandler();

  // add observers 1 and 2
  EXPECT_CALL(*obs1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs1.get());

  EXPECT_CALL(*obs2, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs2.get());

  // observer 1 will add observers 3 and 4
  EXPECT_CALL(*obs1, specialMock(obj1.get()))
      .Times(1)
      .WillOnce(InvokeWithoutArgs([&obj1, &obs3, &obs4]() {
        obj1->observerCtr.addObserver(obs3.get());
        obj1->observerCtr.addObserver(obs4.get());
      }));
  EXPECT_CALL(*obs3, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs3, attachedMock(obj1.get()));
  EXPECT_CALL(*obs4, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs4, attachedMock(obj1.get()));

  EXPECT_CALL(*obs2, specialMock(obj1.get()));
  EXPECT_CALL(*obs3, specialMock(obj1.get()));
  EXPECT_CALL(*obs4, specialMock(obj1.get()));
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs4, postInvokeInterfaceMethodMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*obs1, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs2, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs3, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs4, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs4, postInvokeInterfaceMethodMock(obj1.get()));
  obj1->doSomethingSuperSpecial();

  EXPECT_CALL(*obs1, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs1, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs2, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs3, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs3, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs4, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs4, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1 = nullptr;
}

/**
 * Add two observers during event processing at two different points.
 */
TEST_F(ObserverContainerTest, CtrInvokeAddTwoObserversOnInvokeFirstSecond) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectObserver>> observers;

  auto obs1 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs2 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs3 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs4 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  obs1->useDefaultInvokeMockHandler();
  obs2->useDefaultInvokeMockHandler();
  obs3->useDefaultInvokeMockHandler();
  obs4->useDefaultInvokeMockHandler();

  // add observers 1 and 2
  EXPECT_CALL(*obs1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs1.get());

  EXPECT_CALL(*obs2, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs2.get());

  // observer 3 and 4 will be added by observers 1 and 3 respectively
  EXPECT_CALL(*obs1, specialMock(obj1.get()))
      .Times(1)
      .WillOnce(InvokeWithoutArgs([&obj1, &obs3]() {
        obj1->observerCtr.addObserver(obs3.get());
      }));
  EXPECT_CALL(*obs3, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs3, attachedMock(obj1.get()));

  EXPECT_CALL(*obs2, specialMock(obj1.get()));
  EXPECT_CALL(*obs3, specialMock(obj1.get()))
      .Times(1)
      .WillOnce(InvokeWithoutArgs([&obj1, &obs4]() {
        obj1->observerCtr.addObserver(obs4.get());
      }));
  EXPECT_CALL(*obs4, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs4, attachedMock(obj1.get()));
  EXPECT_CALL(*obs4, specialMock(obj1.get()));

  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs4, postInvokeInterfaceMethodMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*obs1, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs2, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs3, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs4, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs4, postInvokeInterfaceMethodMock(obj1.get()));
  obj1->doSomethingSuperSpecial();

  EXPECT_CALL(*obs1, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs1, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs2, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs3, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs3, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs4, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs4, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1 = nullptr;
}

/**
 * Add observer during post event processing.
 */
TEST_F(ObserverContainerTest, CtrInvokeAddObserverOnPostInvoke) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectObserver>> observers;

  auto obs1 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs2 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs3 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  obs1->useDefaultInvokeMockHandler();
  obs2->useDefaultInvokeMockHandler();
  obs3->useDefaultInvokeMockHandler();

  // add observers 1 and 2
  EXPECT_CALL(*obs1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs1.get());

  EXPECT_CALL(*obs2, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs2.get());

  EXPECT_CALL(*obs1, specialMock(obj1.get()));
  EXPECT_CALL(*obs2, specialMock(obj1.get()));
  EXPECT_CALL(*obs3, specialMock(obj1.get())).Times(0); // not added yet

  // observer 3 will be added during post processing by observer 1
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()))
      .Times(1)
      .WillOnce(InvokeWithoutArgs([&obj1, &obs3]() {
        obj1->observerCtr.addObserver(obs3.get());
      }));
  EXPECT_CALL(*obs3, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs3, attachedMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  // post invoke won't be called on observer 3 since it was newly added
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get())).Times(0);
  obj1->doSomethingSpecial();

  EXPECT_CALL(*obs1, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs2, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs3, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get()));
  obj1->doSomethingSuperSpecial();

  EXPECT_CALL(*obs1, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs1, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs2, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs3, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs3, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1 = nullptr;
}

/**
 * Remove observer during event processing.
 */
TEST_F(ObserverContainerTest, CtrInvokeRemoveObserverOnInvoke) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectObserver>> observers;

  auto obs1 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs2 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs3 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  obs1->useDefaultInvokeMockHandler();
  obs2->useDefaultInvokeMockHandler();
  obs3->useDefaultInvokeMockHandler();

  // add observers 1, 2, and 3
  EXPECT_CALL(*obs1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs1.get());

  EXPECT_CALL(*obs2, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs2.get());

  EXPECT_CALL(*obs3, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs3, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs3.get());

  // observer 3 will be removed during processing of the event by observer 1
  EXPECT_CALL(*obs1, specialMock(obj1.get()))
      .Times(1)
      .WillOnce(InvokeWithoutArgs([&obj1, &obs3]() {
        obj1->observerCtr.removeObserver(obs3.get());
      }));
  EXPECT_CALL(*obs3, detachedMock(obj1.get()));
  EXPECT_CALL(*obs3, removedFromObserverContainerMock(&obj1->observerCtr));

  EXPECT_CALL(*obs2, specialMock(obj1.get()));
  EXPECT_CALL(*obs3, specialMock(obj1.get())).Times(0);
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get())).Times(0);
  obj1->doSomethingSpecial();

  EXPECT_CALL(*obs1, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs2, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs3, superSpecialMock(obj1.get())).Times(0);
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get())).Times(0);
  obj1->doSomethingSuperSpecial();

  EXPECT_CALL(*obs1, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs1, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs2, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1 = nullptr;
}

/**
 * Remove two observers during event processing.
 */
TEST_F(ObserverContainerTest, CtrInvokeRemoveTwoObserversOnInvoke) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectObserver>> observers;

  auto obs1 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs2 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs3 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs4 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  obs1->useDefaultInvokeMockHandler();
  obs2->useDefaultInvokeMockHandler();
  obs3->useDefaultInvokeMockHandler();
  obs4->useDefaultInvokeMockHandler();

  // add observers 1 - 4
  EXPECT_CALL(*obs1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs1.get());

  EXPECT_CALL(*obs2, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs2.get());

  EXPECT_CALL(*obs3, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs3, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs3.get());

  EXPECT_CALL(*obs4, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs4, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs4.get());

  // observers 3 and 4 removed during processing of the event by observer 1
  EXPECT_CALL(*obs1, specialMock(obj1.get()))
      .Times(1)
      .WillOnce(InvokeWithoutArgs([&obj1, &obs3, &obs4]() {
        obj1->observerCtr.removeObserver(obs3.get());
        obj1->observerCtr.removeObserver(obs4.get());
      }));
  EXPECT_CALL(*obs3, detachedMock(obj1.get()));
  EXPECT_CALL(*obs3, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs4, detachedMock(obj1.get()));
  EXPECT_CALL(*obs4, removedFromObserverContainerMock(&obj1->observerCtr));

  EXPECT_CALL(*obs2, specialMock(obj1.get()));
  EXPECT_CALL(*obs3, specialMock(obj1.get())).Times(0);
  EXPECT_CALL(*obs4, specialMock(obj1.get())).Times(0);
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get())).Times(0);
  EXPECT_CALL(*obs4, postInvokeInterfaceMethodMock(obj1.get())).Times(0);
  obj1->doSomethingSpecial();

  EXPECT_CALL(*obs1, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs2, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs3, superSpecialMock(obj1.get())).Times(0);
  EXPECT_CALL(*obs4, superSpecialMock(obj1.get())).Times(0);
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs4, postInvokeInterfaceMethodMock(obj1.get())).Times(0);
  obj1->doSomethingSuperSpecial();

  EXPECT_CALL(*obs1, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs1, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs2, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1 = nullptr;
}

/**
 * Remove observer during post event processing.
 */
TEST_F(ObserverContainerTest, CtrInvokeRemoveObserverOnPostInvoke) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectObserver>> observers;

  auto obs1 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs2 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs3 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  obs1->useDefaultInvokeMockHandler();
  obs2->useDefaultInvokeMockHandler();
  obs3->useDefaultInvokeMockHandler();

  // add observers 1, 2, and 3
  EXPECT_CALL(*obs1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs1.get());

  EXPECT_CALL(*obs2, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs2.get());

  EXPECT_CALL(*obs3, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs3, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs3.get());

  EXPECT_CALL(*obs1, specialMock(obj1.get()));
  EXPECT_CALL(*obs2, specialMock(obj1.get()));
  EXPECT_CALL(*obs3, specialMock(obj1.get()));

  // observer 3 will be removed during post processing by observer 1
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()))
      .Times(1)
      .WillOnce(InvokeWithoutArgs([&obj1, &obs3]() {
        obj1->observerCtr.removeObserver(obs3.get());
      }));
  EXPECT_CALL(*obs3, detachedMock(obj1.get()));
  EXPECT_CALL(*obs3, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get())).Times(0);
  obj1->doSomethingSpecial();

  EXPECT_CALL(*obs1, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs2, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs3, superSpecialMock(obj1.get())).Times(0);
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get())).Times(0);
  obj1->doSomethingSuperSpecial();

  EXPECT_CALL(*obs1, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs1, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs2, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1 = nullptr;
}

/**
 * Add and remove observer during event processing.
 */
TEST_F(ObserverContainerTest, CtrInvokeAddRemoveObserverOnInvoke) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectObserver>> observers;

  auto obs1 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs2 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs3 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  auto obs4 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  obs1->useDefaultInvokeMockHandler();
  obs2->useDefaultInvokeMockHandler();
  obs3->useDefaultInvokeMockHandler();
  obs4->useDefaultInvokeMockHandler();

  // add observers 1 - 3
  EXPECT_CALL(*obs1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs1.get());

  EXPECT_CALL(*obs2, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs2.get());

  EXPECT_CALL(*obs3, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs3, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(obs3.get());

  // remove observer 3, add observer 4 during event processing for observer 1
  EXPECT_CALL(*obs1, specialMock(obj1.get()))
      .Times(1)
      .WillOnce(InvokeWithoutArgs([&obj1, &obs3, &obs4]() {
        obj1->observerCtr.removeObserver(obs3.get());
        obj1->observerCtr.addObserver(obs4.get());
      }));
  EXPECT_CALL(*obs3, detachedMock(obj1.get()));
  EXPECT_CALL(*obs3, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs4, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs4, attachedMock(obj1.get()));

  EXPECT_CALL(*obs2, specialMock(obj1.get()));
  EXPECT_CALL(*obs3, specialMock(obj1.get())).Times(0); // removed
  EXPECT_CALL(*obs4, specialMock(obj1.get())); // added
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get()))
      .Times(0); // removed
  EXPECT_CALL(*obs4, postInvokeInterfaceMethodMock(obj1.get())); // added
  obj1->doSomethingSpecial();

  EXPECT_CALL(*obs1, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs2, superSpecialMock(obj1.get()));
  EXPECT_CALL(*obs3, superSpecialMock(obj1.get())).Times(0); // removed
  EXPECT_CALL(*obs4, superSpecialMock(obj1.get())); // added
  EXPECT_CALL(*obs1, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs2, postInvokeInterfaceMethodMock(obj1.get()));
  EXPECT_CALL(*obs3, postInvokeInterfaceMethodMock(obj1.get()))
      .Times(0); // removed
  EXPECT_CALL(*obs4, postInvokeInterfaceMethodMock(obj1.get())); // added
  obj1->doSomethingSuperSpecial();

  EXPECT_CALL(*obs1, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs1, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs2, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs2, removedFromObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*obs4, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs4, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1 = nullptr;
}

TEST_F(ObserverContainerTest, CtrGetFindObserversWithDetach) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  using MockTestSubjectManagedObserverSpecialized =
      MockTestSubjectManagedObserverSpecialized<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();

  // should be no observers
  EXPECT_EQ(0, obj1->observerCtr.numObservers());
  EXPECT_THAT(obj1->observerCtr.getObservers(), IsEmpty());
  EXPECT_THAT(obj1->observerCtr.findObservers<>(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::ManagedObserver>(),
      IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectManagedObserver>(),
      IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<MockTestSubjectManagedObserverSpecialized>(),
      IsEmpty());

  // lambda for adding observer
  auto addObserver = [&obj1](auto& observer) {
    observer->useDefaultInvokeMockHandler();
    observer->useDefaultPostInvokeMockHandler();
    if constexpr (std::is_same_v<
                      decltype(*observer),
                      StrictMock<MockTestSubjectObserver>&>) {
      EXPECT_CALL(*observer, addedToObserverContainerMock(&obj1->observerCtr));
    }
    EXPECT_CALL(*observer, attachedMock(obj1.get()));
    obj1->observerCtr.addObserver(observer.get());
  };

  // lambda for removing observer
  auto removeObserver = [&obj1](auto& observer) {
    EXPECT_CALL(*observer, detachedMock(obj1.get()));
    if constexpr (std::is_same_v<
                      decltype(*observer),
                      StrictMock<MockTestSubjectObserver>&>) {
      EXPECT_CALL(
          *observer, removedFromObserverContainerMock(&obj1->observerCtr));
    }
    obj1->observerCtr.removeObserver(observer.get());
  };

  // observer 1 is a MockTestSubjectObserver
  auto obs1 = std::make_unique<StrictMock<MockTestSubjectObserver>>();
  addObserver(obs1);
  EXPECT_EQ(1, obj1->observerCtr.numObservers());
  EXPECT_THAT(
      obj1->observerCtr.getObservers(), UnorderedElementsAre(obs1.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<>(), UnorderedElementsAre(obs1.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(obs1.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::ManagedObserver>(),
      IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(),
      UnorderedElementsAre(obs1.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectManagedObserver>(),
      IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<MockTestSubjectManagedObserverSpecialized>(),
      IsEmpty());

  // observer 2 is a MockTestSubjectManagedObserver
  auto obs2 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>();
  addObserver(obs2);
  EXPECT_EQ(2, obj1->observerCtr.numObservers());
  EXPECT_THAT(
      obj1->observerCtr.getObservers(),
      UnorderedElementsAre(obs1.get(), obs2.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<>(),
      UnorderedElementsAre(obs1.get(), obs2.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(obs1.get(), obs2.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::ManagedObserver>(),
      UnorderedElementsAre(obs2.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(),
      UnorderedElementsAre(obs1.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectManagedObserver>(),
      UnorderedElementsAre(obs2.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<MockTestSubjectManagedObserverSpecialized>(),
      IsEmpty());

  // observer 3 is another MockTestSubjectManagedObserver
  auto obs3 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>();
  addObserver(obs3);
  EXPECT_EQ(3, obj1->observerCtr.numObservers());
  EXPECT_THAT(
      obj1->observerCtr.getObservers(),
      UnorderedElementsAre(obs1.get(), obs2.get(), obs3.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<>(),
      UnorderedElementsAre(obs1.get(), obs2.get(), obs3.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(obs1.get(), obs2.get(), obs3.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::ManagedObserver>(),
      UnorderedElementsAre(obs2.get(), obs3.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(),
      UnorderedElementsAre(obs1.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectManagedObserver>(),
      UnorderedElementsAre(obs2.get(), obs3.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<MockTestSubjectManagedObserverSpecialized>(),
      IsEmpty());

  // observer 4 is a MockTestSubjectManagedObserverSpecialized
  auto obs4 =
      std::make_unique<StrictMock<MockTestSubjectManagedObserverSpecialized>>();
  addObserver(obs4);
  EXPECT_EQ(4, obj1->observerCtr.numObservers());
  EXPECT_THAT(
      obj1->observerCtr.getObservers(),
      UnorderedElementsAre(obs1.get(), obs2.get(), obs3.get(), obs4.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<>(),
      UnorderedElementsAre(obs1.get(), obs2.get(), obs3.get(), obs4.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(obs1.get(), obs2.get(), obs3.get(), obs4.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::ManagedObserver>(),
      UnorderedElementsAre(obs2.get(), obs3.get(), obs4.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(),
      UnorderedElementsAre(obs1.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectManagedObserver>(),
      UnorderedElementsAre(obs2.get(), obs3.get(), obs4.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<MockTestSubjectManagedObserverSpecialized>(),
      UnorderedElementsAre(obs4.get()));

  // observer 5 is another MockTestSubjectManagedObserver
  auto obs5 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>();
  addObserver(obs5);
  EXPECT_EQ(5, obj1->observerCtr.numObservers());
  EXPECT_THAT(
      obj1->observerCtr.getObservers(),
      UnorderedElementsAre(
          obs1.get(), obs2.get(), obs3.get(), obs4.get(), obs5.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<>(),
      UnorderedElementsAre(
          obs1.get(), obs2.get(), obs3.get(), obs4.get(), obs5.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(
          obs1.get(), obs2.get(), obs3.get(), obs4.get(), obs5.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::ManagedObserver>(),
      UnorderedElementsAre(obs2.get(), obs3.get(), obs4.get(), obs5.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(),
      UnorderedElementsAre(obs1.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectManagedObserver>(),
      UnorderedElementsAre(obs2.get(), obs3.get(), obs4.get(), obs5.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<MockTestSubjectManagedObserverSpecialized>(),
      UnorderedElementsAre(obs4.get()));

  // observer 6 is another MockTestSubjectManagedObserverSpecialized
  auto obs6 =
      std::make_unique<StrictMock<MockTestSubjectManagedObserverSpecialized>>();
  addObserver(obs6);
  EXPECT_EQ(6, obj1->observerCtr.numObservers());
  EXPECT_THAT(
      obj1->observerCtr.getObservers(),
      UnorderedElementsAre(
          obs1.get(),
          obs2.get(),
          obs3.get(),
          obs4.get(),
          obs5.get(),
          obs6.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<>(),
      UnorderedElementsAre(
          obs1.get(),
          obs2.get(),
          obs3.get(),
          obs4.get(),
          obs5.get(),
          obs6.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(
          obs1.get(),
          obs2.get(),
          obs3.get(),
          obs4.get(),
          obs5.get(),
          obs6.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::ManagedObserver>(),
      UnorderedElementsAre(
          obs2.get(), obs3.get(), obs4.get(), obs5.get(), obs6.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(),
      UnorderedElementsAre(obs1.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectManagedObserver>(),
      UnorderedElementsAre(
          obs2.get(), obs3.get(), obs4.get(), obs5.get(), obs6.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<MockTestSubjectManagedObserverSpecialized>(),
      UnorderedElementsAre(obs4.get(), obs6.get()));

  // remove observers 3 and 4
  removeObserver(obs3);
  removeObserver(obs4);
  EXPECT_EQ(4, obj1->observerCtr.numObservers());
  EXPECT_THAT(
      obj1->observerCtr.getObservers(),
      UnorderedElementsAre(obs1.get(), obs2.get(), obs5.get(), obs6.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<>(),
      UnorderedElementsAre(obs1.get(), obs2.get(), obs5.get(), obs6.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      UnorderedElementsAre(obs1.get(), obs2.get(), obs5.get(), obs6.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::ManagedObserver>(),
      UnorderedElementsAre(obs2.get(), obs5.get(), obs6.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(),
      UnorderedElementsAre(obs1.get()));
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectManagedObserver>(),
      UnorderedElementsAre(obs2.get(), obs5.get(), obs6.get()));
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<MockTestSubjectManagedObserverSpecialized>(),
      UnorderedElementsAre(obs6.get()));

  // remove the rest of the observers
  removeObserver(obs1);
  removeObserver(obs2);
  removeObserver(obs5);
  removeObserver(obs6);

  // should be no observers
  EXPECT_EQ(0, obj1->observerCtr.numObservers());
  EXPECT_THAT(obj1->observerCtr.getObservers(), IsEmpty());
  EXPECT_THAT(obj1->observerCtr.findObservers<>(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::Observer>(),
      IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<TestSubject::ObserverContainer::ManagedObserver>(),
      IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(), IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectManagedObserver>(),
      IsEmpty());
  EXPECT_THAT(
      obj1->observerCtr
          .findObservers<MockTestSubjectManagedObserverSpecialized>(),
      IsEmpty());

  obj1 = nullptr;
}

TEST_F(ObserverContainerTest, CtrHasObserversForEvent) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();

  // should be no observers
  EXPECT_EQ(0, obj1->observerCtr.numObservers());
  EXPECT_FALSE(obj1->observerCtr
                   .hasObserversForEvent<TestObserverEvents::SpecialEvent>());
  EXPECT_FALSE(
      obj1->observerCtr
          .hasObserversForEvent<TestObserverEvents::SuperSpecialEvent>());

  // lambda for adding observer
  auto addObserver = [&obj1](auto& observer) {
    observer->useDefaultInvokeMockHandler();
    observer->useDefaultPostInvokeMockHandler();
    if constexpr (std::is_same_v<
                      decltype(*observer),
                      StrictMock<MockTestSubjectObserver>&>) {
      EXPECT_CALL(*observer, addedToObserverContainerMock(&obj1->observerCtr));
    }
    EXPECT_CALL(*observer, attachedMock(obj1.get()));
    obj1->observerCtr.addObserver(observer.get());
  };

  // lambda for removing observer
  auto removeObserver = [&obj1](auto& observer) {
    EXPECT_CALL(*observer, detachedMock(obj1.get()));
    if constexpr (std::is_same_v<
                      decltype(*observer),
                      StrictMock<MockTestSubjectObserver>&>) {
      EXPECT_CALL(
          *observer, removedFromObserverContainerMock(&obj1->observerCtr));
    }
    obj1->observerCtr.removeObserver(observer.get());
  };

  // observer 1 has SpecialEvent
  auto obs1 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder()
          .enable(TestObserverEvents::SpecialEvent)
          .build());
  addObserver(obs1);
  EXPECT_EQ(1, obj1->observerCtr.numObservers());
  EXPECT_TRUE(obj1->observerCtr
                  .hasObserversForEvent<TestObserverEvents::SpecialEvent>());
  EXPECT_FALSE(
      obj1->observerCtr
          .hasObserversForEvent<TestObserverEvents::SuperSpecialEvent>());

  // observer 2 has SuperSpecialEvent
  auto obs2 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder()
          .enable(TestObserverEvents::SuperSpecialEvent)
          .build());
  addObserver(obs2);
  EXPECT_EQ(2, obj1->observerCtr.numObservers());
  EXPECT_TRUE(obj1->observerCtr
                  .hasObserversForEvent<TestObserverEvents::SpecialEvent>());
  EXPECT_TRUE(
      obj1->observerCtr
          .hasObserversForEvent<TestObserverEvents::SuperSpecialEvent>());

  // observer 3 has SpecialEvent
  auto obs3 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder()
          .enable(TestObserverEvents::SpecialEvent)
          .build());
  addObserver(obs3);
  EXPECT_EQ(3, obj1->observerCtr.numObservers());
  EXPECT_TRUE(obj1->observerCtr
                  .hasObserversForEvent<TestObserverEvents::SpecialEvent>());
  EXPECT_TRUE(
      obj1->observerCtr
          .hasObserversForEvent<TestObserverEvents::SuperSpecialEvent>());

  // observer 4 has SuperSpecialEvent
  auto obs4 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder()
          .enable(TestObserverEvents::SuperSpecialEvent)
          .build());
  addObserver(obs4);
  EXPECT_EQ(4, obj1->observerCtr.numObservers());
  EXPECT_TRUE(obj1->observerCtr
                  .hasObserversForEvent<TestObserverEvents::SpecialEvent>());
  EXPECT_TRUE(
      obj1->observerCtr
          .hasObserversForEvent<TestObserverEvents::SuperSpecialEvent>());

  // remove observers 3 and 4
  removeObserver(obs3);
  removeObserver(obs4);
  EXPECT_EQ(2, obj1->observerCtr.numObservers());
  EXPECT_TRUE(obj1->observerCtr
                  .hasObserversForEvent<TestObserverEvents::SpecialEvent>());
  EXPECT_TRUE(
      obj1->observerCtr
          .hasObserversForEvent<TestObserverEvents::SuperSpecialEvent>());

  // remove the rest of the observers
  removeObserver(obs1);
  removeObserver(obs2);

  // should be no observers
  EXPECT_EQ(0, obj1->observerCtr.numObservers());
  EXPECT_FALSE(obj1->observerCtr
                   .hasObserversForEvent<TestObserverEvents::SpecialEvent>());
  EXPECT_FALSE(
      obj1->observerCtr
          .hasObserversForEvent<TestObserverEvents::SuperSpecialEvent>());

  obj1 = nullptr;
}

/**
 *
 * Tests for (raw) Observer interactions with ObserverContainer.
 *
 */

TEST_F(ObserverContainerTest, ObserverNeverAttachedToCtr) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
}

TEST_F(ObserverContainerTest, ObserverNeverAttachedToCtrNoEvents) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectObserver>>();
}

TEST_F(ObserverContainerTest, ObserverAttachedThenObjectDestroyed) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  MockTestSubjectObserver::Safety dc(*observer1.get());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_CALL(*observer1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());
  ASSERT_FALSE(dc.destroyed());

  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observer1, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*observer1, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1 = nullptr;
  ASSERT_FALSE(dc.destroyed());
}

TEST_F(ObserverContainerTest, SharedPtrObserverAttachedThenObjectDestroyed) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_shared<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  MockTestSubjectObserver::Safety dc(*observer1.get());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_CALL(*observer1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1);
  EXPECT_FALSE(dc.destroyed());

  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observer1, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*observer1, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1 = nullptr;
  EXPECT_FALSE(dc.destroyed());
}

TEST_F(
    ObserverContainerTest,
    SharedPtrObserverAttachedSingleHandleThenObjectDestroyed) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_shared<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  MockTestSubjectObserver::Safety dc(*observer1.get());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_CALL(*observer1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1);
  EXPECT_FALSE(dc.destroyed());

  // now that observer1 is attached, we release shared_ptr but keep raw ptr
  // since the container holds shared_ptr too, observer should not be destroyed
  auto observer1Raw = observer1.get();
  observer1 = nullptr;
  ASSERT_FALSE(dc.destroyed()); // should still exist

  EXPECT_CALL(*observer1Raw, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observer1Raw, destroyedMock(obj1.get(), _));
  EXPECT_CALL(
      *observer1Raw, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1 = nullptr;
  EXPECT_TRUE(dc.destroyed()); // destroyed when observer destroyed
}

TEST_F(ObserverContainerTest, ObserverAttachedThenObserverDetached) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_CALL(*observer1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observer1, detachedMock(obj1.get()));
  EXPECT_CALL(*observer1, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1->observerCtr.removeObserver(observer1.get());
}

TEST_F(ObserverContainerTest, SharedPtrObserverAttachedThenObserverDetached) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_shared<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_CALL(*observer1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1);

  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observer1, detachedMock(obj1.get()));
  EXPECT_CALL(*observer1, removedFromObserverContainerMock(&obj1->observerCtr));
  obj1->observerCtr.removeObserver(observer1);
}

TEST_F(
    ObserverContainerTest,
    SharedPtrObserverAttachedSingleHandleThenObserverDetached) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_shared<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  MockTestSubjectObserver::Safety dc(*observer1.get());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_CALL(*observer1, addedToObserverContainerMock(&obj1->observerCtr));
  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1);

  // now that observer1 is attached, we release shared_ptr but keep weak & raw
  // since the container holds shared_ptr too, observer should not be destroyed
  auto observer1Raw = observer1.get();
  std::weak_ptr<MockTestSubjectObserver> observer1Weak = observer1;
  observer1 = nullptr;
  ASSERT_FALSE(dc.destroyed()); // should still exist

  EXPECT_CALL(*observer1Raw, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observer1Raw, detachedMock(obj1.get()));
  EXPECT_CALL(
      *observer1Raw, removedFromObserverContainerMock(&obj1->observerCtr));
  auto observer1Locked = observer1Weak.lock(); // lock again for removal
  ASSERT_THAT(observer1Locked, Not(IsNull()));
  EXPECT_TRUE(obj1->observerCtr.removeObserver(observer1Locked));
}

TEST_F(ObserverContainerTest, ObserverAttachedEvents) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectObserver>> observers;

  // observer 1 is subscribed to nothing
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>());
  }

  // observer 2 is subscribed to both events explicitly
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enable(TestObserverEvents::SpecialEvent);
    eventSet.enable(TestObserverEvents::SuperSpecialEvent);
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // observer 3 is subscribed to both events explicitly at once
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enable(
        TestObserverEvents::SpecialEvent,
        TestObserverEvents::SuperSpecialEvent);
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // observer 4 is subscribed to all events via enableAllEvents()
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enableAllEvents();
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // observer 5 is subscribed to just SpecialEvent
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enable(TestObserverEvents::SpecialEvent);
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // observer 6 is subscribed to just SuperSpecialEvent
  {
    MockTestSubjectObserver::EventSet eventSet;
    eventSet.enable(TestObserverEvents::SuperSpecialEvent);
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(eventSet));
  }

  // add the observers
  for (const auto& observer : observers) {
    observer->useDefaultInvokeMockHandler();
    observer->useDefaultPostInvokeMockHandler();
    EXPECT_CALL(*observer, addedToObserverContainerMock(&obj1->observerCtr));
    EXPECT_CALL(*observer, attachedMock(obj1.get()));
    obj1->observerCtr.addObserver(observer.get());
  }
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(),
      UnorderedElementsAreArray(uniquePtrVecToRawPtrVec(observers)));

  // set up expectations, then trigger events
  //// observer 1 should get no events
  //// observer 2 -> 4 should get both events
  //// observer 5 should get SpecialEvent
  //// observer 6 should get SuperSpecialEvent
  EXPECT_CALL(*observers[0], specialMock(obj1.get())).Times(0);
  EXPECT_CALL(*observers[1], specialMock(obj1.get()));
  EXPECT_CALL(*observers[2], specialMock(obj1.get()));
  EXPECT_CALL(*observers[3], specialMock(obj1.get()));
  EXPECT_CALL(*observers[4], specialMock(obj1.get()));
  EXPECT_CALL(*observers[5], specialMock(obj1.get())).Times(0);
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observers[0], superSpecialMock(obj1.get())).Times(0);
  EXPECT_CALL(*observers[1], superSpecialMock(obj1.get()));
  EXPECT_CALL(*observers[2], superSpecialMock(obj1.get()));
  EXPECT_CALL(*observers[3], superSpecialMock(obj1.get()));
  EXPECT_CALL(*observers[4], superSpecialMock(obj1.get())).Times(0);
  EXPECT_CALL(*observers[5], superSpecialMock(obj1.get()));
  obj1->doSomethingSuperSpecial();

  // destroy object
  for (const auto& observer : observers) {
    EXPECT_CALL(*observer, destroyedMock(obj1.get(), _));
    EXPECT_CALL(
        *observer, removedFromObserverContainerMock(&obj1->observerCtr));
  }
  obj1 = nullptr;
}

TEST_F(ObserverContainerTest, ObserverAttachedEventsUseBuilder) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectObserver>> observers;

  // observer 1 is subscribed to nothing
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(
            MockTestSubjectObserver::EventSetBuilder().build()));
  }

  // observer 2 is subscribed to both events explicitly
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(
            MockTestSubjectObserver::EventSetBuilder()
                .enable(TestObserverEvents::SpecialEvent)
                .enable(TestObserverEvents::SuperSpecialEvent)
                .build()));
  }

  // observer 3 is subscribed to both events explicitly at once
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(
            MockTestSubjectObserver::EventSetBuilder()
                .enable(
                    TestObserverEvents::SpecialEvent,
                    TestObserverEvents::SuperSpecialEvent)
                .build()));
  }

  // observer 4 is subscribed to all events via enableAllEvents()
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(
            MockTestSubjectObserver::EventSetBuilder()
                .enableAllEvents()
                .build()));
  }

  // observer 5 is subscribed to just SpecialEvent
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(
            MockTestSubjectObserver::EventSetBuilder()
                .enable(TestObserverEvents::SpecialEvent)
                .build()));
  }

  // observer 6 is subscribed to just SuperSpecialEvent
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectObserver>>(
            MockTestSubjectObserver::EventSetBuilder()
                .enable(TestObserverEvents::SuperSpecialEvent)
                .build()));
  }

  // add the observers
  for (const auto& observer : observers) {
    observer->useDefaultInvokeMockHandler();
    observer->useDefaultPostInvokeMockHandler();
    EXPECT_CALL(*observer, addedToObserverContainerMock(&obj1->observerCtr));
    EXPECT_CALL(*observer, attachedMock(obj1.get()));
    obj1->observerCtr.addObserver(observer.get());
  }
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectObserver>(),
      UnorderedElementsAreArray(uniquePtrVecToRawPtrVec(observers)));

  // set up expectations, then trigger events
  //// observer 1 should get no events
  //// observer 2 -> 4 should get both events
  //// observer 5 should get SpecialEvent
  //// observer 6 should get SuperSpecialEvent
  EXPECT_CALL(*observers[0], specialMock(obj1.get())).Times(0);
  EXPECT_CALL(*observers[1], specialMock(obj1.get()));
  EXPECT_CALL(*observers[2], specialMock(obj1.get()));
  EXPECT_CALL(*observers[3], specialMock(obj1.get()));
  EXPECT_CALL(*observers[4], specialMock(obj1.get()));
  EXPECT_CALL(*observers[5], specialMock(obj1.get())).Times(0);
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observers[0], superSpecialMock(obj1.get())).Times(0);
  EXPECT_CALL(*observers[1], superSpecialMock(obj1.get()));
  EXPECT_CALL(*observers[2], superSpecialMock(obj1.get()));
  EXPECT_CALL(*observers[3], superSpecialMock(obj1.get()));
  EXPECT_CALL(*observers[4], superSpecialMock(obj1.get())).Times(0);
  EXPECT_CALL(*observers[5], superSpecialMock(obj1.get()));
  obj1->doSomethingSuperSpecial();

  // destroy object
  for (const auto& observer : observers) {
    EXPECT_CALL(*observer, destroyedMock(obj1.get(), _));
    EXPECT_CALL(
        *observer, removedFromObserverContainerMock(&obj1->observerCtr));
  }
  obj1 = nullptr;
}

/**
 * Tests for ManagedObserver interactions with ObserverContainer.
 */

TEST_F(ObserverContainerTest, ManagedObserverNeverAttached) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  auto observer1 =
      std::make_unique<StrictMock<MockTestSubjectManagedObserver>>();
}

TEST_F(ObserverContainerTest, ManagedObserverNeverAttachedWithChecks) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  auto observer1 =
      std::make_unique<StrictMock<MockTestSubjectManagedObserver>>();
  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());
}

TEST_F(ObserverContainerTest, ManagedObserverAttachedThenObserverDestroyed) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj1.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  observer1 = nullptr;
}

TEST_F(
    ObserverContainerTest, ManagedObserverAttachedThenObserverDetachedViaCtr) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj1.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observer1, detachedMock(obj1.get()));
  obj1->observerCtr.removeObserver(observer1.get());
  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());
}

TEST_F(ObserverContainerTest, ManagedObserverAttachViaListCalledTwice) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  // try calling a second time... nothing should happen
  obj1->observerCtr.addObserver(observer1.get());
}

TEST_F(
    ObserverContainerTest, ManagedObserverAttachCalledTwiceDifferentObjects) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  // call attach again, but different object, should die
  auto obj2 = std::make_unique<TestSubject>();
  EXPECT_DEATH(obj2->observerCtr.addObserver(observer1.get()), ".*");
}

TEST_F(ObserverContainerTest, ManagedObserverDetachCalledTwice) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  EXPECT_CALL(*observer1, detachedMock(obj1.get()));
  EXPECT_TRUE(observer1->detach());
  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  // try calling a second time... nothing should happen
  EXPECT_FALSE(observer1->detach());
}

TEST_F(ObserverContainerTest, ManagedObserverDetachCalledNeverAttached) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();
  EXPECT_FALSE(observer1->detach());
}

TEST_F(ObserverContainerTest, ManagedObserverMovesBetweenObjectsDetach) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj1.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observer1, detachedMock(obj1.get()));
  observer1->detach();
  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  // now bring up obj2

  auto obj2 = std::make_unique<TestSubject>();

  EXPECT_CALL(*observer1, attachedMock(obj2.get()));
  obj2->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj2.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj2.get()));
  obj2->doSomethingSpecial();

  EXPECT_CALL(*observer1, detachedMock(obj2.get()));
  observer1->detach();
  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());
}

TEST_F(ObserverContainerTest, ManagedObserverMovesBetweenObjectsDetachViaCtr) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj1.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observer1, detachedMock(obj1.get()));
  obj1->observerCtr.removeObserver(observer1.get());
  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  // now bring up obj2

  auto obj2 = std::make_unique<TestSubject>();

  EXPECT_CALL(*observer1, attachedMock(obj2.get()));
  obj2->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj2.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj2.get()));
  obj2->doSomethingSpecial();

  EXPECT_CALL(*observer1, detachedMock(obj2.get()));
  obj2->observerCtr.removeObserver(observer1.get());
  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());
}

TEST_F(ObserverContainerTest, ManagedObserverMovesBetweenObjectsDestroyObject) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj1.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observer1, destroyedMock(obj1.get(), _));
  obj1 = nullptr;

  // now bring up obj2

  auto obj2 = std::make_unique<TestSubject>();

  EXPECT_CALL(*observer1, attachedMock(obj2.get()));
  obj2->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj2.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj2.get()));
  obj2->doSomethingSpecial();

  EXPECT_CALL(*observer1, destroyedMock(obj2.get(), _));
  obj2 = nullptr;
}

TEST_F(
    ObserverContainerTest, ManagedObserverMovesBetweenObjectsDestroyObserver) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj1.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observer1, destroyedMock(obj1.get(), _));
  obj1 = nullptr;

  // now bring up obj2
  auto obj2 = std::make_unique<TestSubject>();

  EXPECT_CALL(*observer1, attachedMock(obj2.get()));
  obj2->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj2.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj2.get()));
  obj2->doSomethingSpecial();

  EXPECT_EQ(1, obj2->observerCtr.numObservers());
  observer1 = nullptr;
  EXPECT_EQ(0, obj2->observerCtr.numObservers());
}

TEST_F(
    ObserverContainerTest,
    ManagedObserverMovesBetweenObjectsViaConstructorThenDetach) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj1.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  // now bring up obj2
  EXPECT_CALL(*observer1, movedMock(obj1.get(), _, _));
  auto obj2 = std::make_unique<TestSubject>(std::move(*obj1));

  EXPECT_EQ(obj2.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj2.get()));
  obj2->doSomethingSpecial();

  EXPECT_CALL(*observer1, detachedMock(obj2.get()));
  observer1->detach();
  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());
}

TEST_F(
    ObserverContainerTest,
    ManagedObserverMovesBetweenObjectsViaConstructorThenDetachViaCtr) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj1.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  // now bring up obj2
  EXPECT_CALL(*observer1, movedMock(obj1.get(), _, _));
  auto obj2 = std::make_unique<TestSubject>(std::move(*obj1));

  EXPECT_EQ(obj2.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj2.get()));
  obj2->doSomethingSpecial();

  EXPECT_CALL(*observer1, detachedMock(obj2.get()));
  obj2->observerCtr.removeObserver(observer1.get());
  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());
}

TEST_F(
    ObserverContainerTest,
    ManagedObserverMovesBetweenObjectsViaConstructorThenDestroyObject) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj1.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  // now bring up obj2
  EXPECT_CALL(*observer1, movedMock(obj1.get(), _, _));
  auto obj2 = std::make_unique<TestSubject>(std::move(*obj1));

  EXPECT_EQ(obj2.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj2.get()));
  obj2->doSomethingSpecial();

  EXPECT_CALL(*observer1, destroyedMock(obj2.get(), _));
  obj2 = nullptr;
}

TEST_F(
    ObserverContainerTest,
    ManagedObserverMovesBetweenObjectsViaConstructorThenDestroyObserver) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  auto observer1 = std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
      MockTestSubjectManagedObserver::EventSetBuilder()
          .enableAllEvents()
          .build());
  observer1->useDefaultInvokeMockHandler();
  observer1->useDefaultPostInvokeMockHandler();

  EXPECT_EQ(nullptr, observer1->getObservedObject());
  EXPECT_FALSE(observer1->isObserving());

  EXPECT_CALL(*observer1, attachedMock(obj1.get()));
  obj1->observerCtr.addObserver(observer1.get());

  EXPECT_EQ(obj1.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj1.get()));
  obj1->doSomethingSpecial();

  // now bring up obj2
  EXPECT_CALL(*observer1, movedMock(obj1.get(), _, _));
  auto obj2 = std::make_unique<TestSubject>(std::move(*obj1));

  EXPECT_EQ(obj2.get(), observer1->getObservedObject());
  EXPECT_TRUE(observer1->isObserving());
  EXPECT_CALL(*observer1, specialMock(obj2.get()));
  obj2->doSomethingSpecial();

  EXPECT_EQ(1, obj2->observerCtr.numObservers());
  observer1 = nullptr;
  EXPECT_EQ(0, obj2->observerCtr.numObservers());
}

TEST_F(ObserverContainerTest, ManagedObserverAttachedEventsUseBuilder) {
  using MockTestSubjectManagedObserver =
      MockTestSubjectManagedObserver<TestSubject::ObserverContainer>;
  InSequence s;

  auto obj1 = std::make_unique<TestSubject>();
  std::vector<std::unique_ptr<MockTestSubjectManagedObserver>> observers;

  // observer 1 is subscribed to nothing
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
            MockTestSubjectManagedObserver::EventSetBuilder().build()));
  }

  // observer 2 is subscribed to both events explicitly
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
            MockTestSubjectManagedObserver::EventSetBuilder()
                .enable(TestObserverEvents::SpecialEvent)
                .enable(TestObserverEvents::SuperSpecialEvent)
                .build()));
  }

  // observer 3 is subscribed to just SpecialEvent
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
            MockTestSubjectManagedObserver::EventSetBuilder()
                .enable(TestObserverEvents::SpecialEvent)
                .build()));
  }

  // observer 4 is subscribed to just SuperSpecialEvent
  {
    observers.emplace_back(
        std::make_unique<StrictMock<MockTestSubjectManagedObserver>>(
            MockTestSubjectManagedObserver::EventSetBuilder()
                .enable(TestObserverEvents::SuperSpecialEvent)
                .build()));
  }

  // add the observers
  for (const auto& observer : observers) {
    observer->useDefaultInvokeMockHandler();
    observer->useDefaultPostInvokeMockHandler();
    EXPECT_CALL(*observer, attachedMock(obj1.get()));
    obj1->observerCtr.addObserver(observer.get());
  }
  EXPECT_THAT(
      obj1->observerCtr.findObservers<MockTestSubjectManagedObserver>(),
      UnorderedElementsAreArray(uniquePtrVecToRawPtrVec(observers)));

  // set up expectations, then trigger events
  //// observer 1 should get no events
  //// observer 2 should get both events
  //// observer 3 should get SpecialEvent
  //// observer 4 should get SuperSpecialEvent
  EXPECT_CALL(*observers[0], specialMock(obj1.get())).Times(0);
  EXPECT_CALL(*observers[1], specialMock(obj1.get()));
  EXPECT_CALL(*observers[2], specialMock(obj1.get()));
  EXPECT_CALL(*observers[3], specialMock(obj1.get())).Times(0);
  obj1->doSomethingSpecial();

  EXPECT_CALL(*observers[0], superSpecialMock(obj1.get())).Times(0);
  EXPECT_CALL(*observers[1], superSpecialMock(obj1.get()));
  EXPECT_CALL(*observers[2], superSpecialMock(obj1.get())).Times(0);
  EXPECT_CALL(*observers[3], superSpecialMock(obj1.get()));
  obj1->doSomethingSuperSpecial();

  // destroy object
  for (const auto& observer : observers) {
    EXPECT_CALL(*observer, destroyedMock(obj1.get(), _));
  }
  obj1 = nullptr;
}

TEST_F(ObserverContainerTest, AddConstructorCallback) {
  uint64_t numSubjectsCreated = 0;
  std::vector<TestSubject*> subjectsCreated;
  auto callbackF = [&](TestSubject::ObserverContainer* ctr) {
    subjectsCreated.push_back(ctr->getObject());
    numSubjectsCreated++;
  };

  TestSubject::ObserverContainer::addConstructorCallback(callbackF);

  EXPECT_EQ(0, numSubjectsCreated);
  auto obj1 = std::make_unique<TestSubject>();
  EXPECT_EQ(1, numSubjectsCreated);
  auto obj2 = std::make_unique<TestSubject>();
  EXPECT_EQ(2, numSubjectsCreated);
  EXPECT_THAT(subjectsCreated, ElementsAre(obj1.get(), obj2.get()));

  obj1 = nullptr;
  obj2 = nullptr;
  EXPECT_EQ(2, numSubjectsCreated);
}

TEST_F(ObserverContainerTest, AddConstructorCallbackMulti) {
  uint64_t numSubjectsCreated1 = 0;
  std::vector<TestSubject*> subjectsCreated1;
  auto callbackF1 = [&](TestSubject::ObserverContainer* ctr) {
    subjectsCreated1.push_back(ctr->getObject());
    numSubjectsCreated1++;
  };

  uint64_t numSubjectsCreated2 = 0;
  std::vector<TestSubject*> subjectsCreated2;
  auto callbackF2 = [&](TestSubject::ObserverContainer* ctr) {
    subjectsCreated2.push_back(ctr->getObject());
    numSubjectsCreated2++;
  };

  TestSubject::ObserverContainer::addConstructorCallback(callbackF1);
  TestSubject::ObserverContainer::addConstructorCallback(callbackF2);

  EXPECT_EQ(0, numSubjectsCreated1);
  EXPECT_EQ(0, numSubjectsCreated2);
  EXPECT_THAT(subjectsCreated1, IsEmpty());
  EXPECT_THAT(subjectsCreated2, IsEmpty());
  auto obj1 = std::make_unique<TestSubject>();
  EXPECT_EQ(1, numSubjectsCreated1);
  EXPECT_EQ(1, numSubjectsCreated2);
  EXPECT_THAT(subjectsCreated1, ElementsAre(obj1.get()));
  EXPECT_THAT(subjectsCreated2, ElementsAre(obj1.get()));
  auto obj2 = std::make_unique<TestSubject>();
  EXPECT_EQ(2, numSubjectsCreated1);
  EXPECT_EQ(2, numSubjectsCreated2);
  EXPECT_THAT(subjectsCreated1, ElementsAre(obj1.get(), obj2.get()));
  EXPECT_THAT(subjectsCreated2, ElementsAre(obj1.get(), obj2.get()));

  obj1 = nullptr;
  obj2 = nullptr;
  EXPECT_EQ(2, numSubjectsCreated1);
  EXPECT_EQ(2, numSubjectsCreated2);
}

TEST_F(ObserverContainerTest, AddConstructorCallbackAttachObserver) {
  using MockTestSubjectObserver =
      MockTestSubjectObserver<TestSubject::ObserverContainer>;

  auto obs = std::make_unique<StrictMock<MockTestSubjectObserver>>(
      MockTestSubjectObserver::EventSetBuilder().enableAllEvents().build());
  obs->useDefaultInvokeMockHandler();
  obs->useDefaultPostInvokeMockHandler();

  uint64_t numSubjectsCreated = 0;
  auto callbackF = [&](TestSubject::ObserverContainer* ctr) {
    numSubjectsCreated++;
    ctr->addObserver(obs.get());
  };

  TestSubject::ObserverContainer::addConstructorCallback(callbackF);

  TestSubject::ObserverContainer::ContainerBase* ctrPtr;
  TestSubject* objPtr;
  EXPECT_CALL(*obs, addedToObserverContainerMock(_))
      .WillOnce(testing::SaveArg<0>(&ctrPtr));
  EXPECT_CALL(*obs, attachedMock(_)).WillOnce(testing::SaveArg<0>(&objPtr));

  auto obj1 = std::make_unique<TestSubject>();
  EXPECT_EQ(&obj1->observerCtr, ctrPtr);
  EXPECT_EQ(obj1.get(), objPtr);
  EXPECT_EQ(1, numSubjectsCreated);

  EXPECT_CALL(*obs, specialMock(obj1.get()));
  EXPECT_CALL(*obs, superSpecialMock(obj1.get()));
  obj1->doSomethingSpecial();
  obj1->doSomethingSuperSpecial();

  EXPECT_CALL(*obs, destroyedMock(obj1.get(), _));
  EXPECT_CALL(*obs, removedFromObserverContainerMock(ctrPtr));
  obj1 = nullptr;
}

/**
 *
 * Tests for ObserverContainerStore.
 *
 */

class ObserverContainerStoreTest : public ::testing::Test {
 protected:
  class TestStoreObserver {
   public:
    void incrementCount() { count_++; }

    uint64_t getCount() const { return count_; }

   private:
    uint64_t count_{0};
  };

  using Store = ObserverContainerStore<TestStoreObserver>;
};

TEST_F(ObserverContainerStoreTest, SizeInvokeEmpty) {
  Store store;
  EXPECT_EQ(0, store.size());
  store.invokeForEachObserver(
      [](auto* observer) { observer->incrementCount(); },
      Store::InvokeWhileIteratingPolicy::InvokeAdded);
}

TEST_F(ObserverContainerStoreTest, AddRemoveSize) {
  Store store;

  // add observer 1, then remove
  auto obs1 = std::make_shared<TestStoreObserver>();
  EXPECT_EQ(0, store.size());
  EXPECT_TRUE(store.add(obs1));
  EXPECT_EQ(1, store.size());
  EXPECT_TRUE(store.remove(obs1));
  EXPECT_EQ(0, store.size());

  // add observer 2, then remove
  auto obs2 = std::make_shared<TestStoreObserver>();
  EXPECT_EQ(0, store.size());
  EXPECT_TRUE(store.add(obs2));
  EXPECT_EQ(1, store.size());
  EXPECT_TRUE(store.remove(obs2));
  EXPECT_EQ(0, store.size());

  // add observer 1 and 2, then remove
  EXPECT_EQ(0, store.size());
  EXPECT_TRUE(store.add(obs1));
  EXPECT_EQ(1, store.size());
  EXPECT_TRUE(store.add(obs2));
  EXPECT_EQ(2, store.size());
  EXPECT_TRUE(store.remove(obs2));
  EXPECT_EQ(1, store.size());
  EXPECT_TRUE(store.remove(obs1));
  EXPECT_EQ(0, store.size());
}

TEST_F(ObserverContainerStoreTest, AddRemoveInvokeSize) {
  Store store;
  auto obs1 = std::make_shared<TestStoreObserver>();
  auto obs2 = std::make_shared<TestStoreObserver>();
  EXPECT_EQ(0, obs1->getCount());
  EXPECT_EQ(0, obs2->getCount());
  EXPECT_EQ(0, obs1->getCount()); // sanity check no side effects on getCount()
  EXPECT_EQ(0, obs2->getCount()); // sanity check no side effects on getCount()

  // add observer 1, invoke, then remove
  EXPECT_EQ(0, store.size());
  EXPECT_TRUE(store.add(obs1));
  EXPECT_EQ(1, store.size());
  store.invokeForEachObserver(
      [](auto* observer) { observer->incrementCount(); },
      Store::InvokeWhileIteratingPolicy::InvokeAdded);
  EXPECT_EQ(1, obs1->getCount());
  EXPECT_EQ(0, obs2->getCount());

  EXPECT_TRUE(store.remove(obs1));
  store.invokeForEachObserver(
      [](auto* observer) { observer->incrementCount(); },
      Store::InvokeWhileIteratingPolicy::InvokeAdded);
  EXPECT_EQ(0, store.size());

  // add observer 2, invoke, then remove
  EXPECT_EQ(0, store.size());
  EXPECT_TRUE(store.add(obs2));
  EXPECT_EQ(1, store.size());
  store.invokeForEachObserver(
      [](auto* observer) { observer->incrementCount(); },
      Store::InvokeWhileIteratingPolicy::InvokeAdded);
  EXPECT_EQ(1, obs1->getCount());
  EXPECT_EQ(1, obs2->getCount());

  EXPECT_TRUE(store.remove(obs2));
  EXPECT_EQ(0, store.size());
  store.invokeForEachObserver(
      [](auto* observer) { observer->incrementCount(); },
      Store::InvokeWhileIteratingPolicy::InvokeAdded);
  EXPECT_EQ(0, store.size());

  // add each observer and invoke, and then do the reverse
  EXPECT_TRUE(store.add(obs1));
  EXPECT_EQ(1, store.size());
  store.invokeForEachObserver(
      [](auto* observer) { observer->incrementCount(); },
      Store::InvokeWhileIteratingPolicy::InvokeAdded);
  EXPECT_EQ(2, obs1->getCount());
  EXPECT_EQ(1, obs2->getCount());

  EXPECT_TRUE(store.add(obs2));
  EXPECT_EQ(2, store.size());
  store.invokeForEachObserver(
      [](auto* observer) { observer->incrementCount(); },
      Store::InvokeWhileIteratingPolicy::InvokeAdded);
  EXPECT_EQ(3, obs1->getCount());
  EXPECT_EQ(2, obs2->getCount());

  EXPECT_TRUE(store.remove(obs2));
  EXPECT_EQ(1, store.size());
  store.invokeForEachObserver(
      [](auto* observer) { observer->incrementCount(); },
      Store::InvokeWhileIteratingPolicy::InvokeAdded);
  EXPECT_EQ(4, obs1->getCount());
  EXPECT_EQ(2, obs2->getCount());

  EXPECT_TRUE(store.remove(obs1));
  EXPECT_EQ(0, store.size());
  store.invokeForEachObserver(
      [](auto* observer) { observer->incrementCount(); },
      Store::InvokeWhileIteratingPolicy::InvokeAdded);
}

TEST_F(ObserverContainerStoreTest, AddRemoveDuplicateAdd) {
  Store store;
  auto obs1 = std::make_shared<TestStoreObserver>();

  EXPECT_EQ(0, store.size());
  EXPECT_TRUE(store.add(obs1));
  EXPECT_EQ(1, store.size());
  EXPECT_FALSE(store.add(obs1));
  EXPECT_EQ(1, store.size());
  EXPECT_TRUE(store.remove(obs1));
  EXPECT_EQ(0, store.size());
}

TEST_F(ObserverContainerStoreTest, AddRemoveDuplicateRemove) {
  Store store;
  auto obs1 = std::make_shared<TestStoreObserver>();

  EXPECT_EQ(0, store.size());
  EXPECT_TRUE(store.add(obs1));
  EXPECT_EQ(1, store.size());
  EXPECT_TRUE(store.remove(obs1));
  EXPECT_EQ(0, store.size());
  EXPECT_FALSE(store.remove(obs1));
  EXPECT_EQ(0, store.size());
}

TEST_F(ObserverContainerStoreTest, AddWhileIteratingPolicyInvokeAdded) {
  Store store;
  auto obs1 = std::make_shared<TestStoreObserver>();
  auto obs2 = std::make_shared<TestStoreObserver>();
  EXPECT_EQ(0, obs1->getCount());
  EXPECT_EQ(0, obs2->getCount());
  EXPECT_EQ(0, obs1->getCount()); // sanity check no side effects on getCount()
  EXPECT_EQ(0, obs2->getCount()); // sanity check no side effects on getCount()

  // add observer1, then during invoke, try to add observer2
  EXPECT_TRUE(store.add(obs1));
  EXPECT_EQ(1, store.size());
  store.invokeForEachObserver(
      [&obs1, &obs2, &store](auto* observer) {
        if (observer == obs1.get()) {
          EXPECT_TRUE(store.add(obs2)); // add observer 2
        }
        observer->incrementCount();
      },
      Store::InvokeWhileIteratingPolicy::InvokeAdded);
  EXPECT_EQ(2, store.size());

  // both observers should have a count of 1
  EXPECT_EQ(1, obs1->getCount());
  EXPECT_EQ(1, obs2->getCount());

  // remove both observers
  EXPECT_TRUE(store.remove(obs1));
  EXPECT_EQ(1, store.size());
  EXPECT_TRUE(store.remove(obs2));
  EXPECT_EQ(0, store.size());
}

TEST_F(ObserverContainerStoreTest, AddWhileIteratingPolicyDoNotInvokeAdded) {
  Store store;
  auto obs1 = std::make_shared<TestStoreObserver>();
  auto obs2 = std::make_shared<TestStoreObserver>();
  EXPECT_EQ(0, obs1->getCount());
  EXPECT_EQ(0, obs2->getCount());
  EXPECT_EQ(0, obs1->getCount()); // sanity check no side effects on getCount()
  EXPECT_EQ(0, obs2->getCount()); // sanity check no side effects on getCount()

  // add observer1, then during invoke, try to add observer2
  EXPECT_TRUE(store.add(obs1));
  EXPECT_EQ(1, store.size());
  store.invokeForEachObserver(
      [&obs1, &obs2, &store](auto* observer) {
        if (observer == obs1.get()) {
          EXPECT_TRUE(store.add(obs2)); // add observer 2
        }
        observer->incrementCount();
      },
      Store::InvokeWhileIteratingPolicy::DoNotInvokeAdded);
  EXPECT_EQ(2, store.size());

  // incrementCount should have only been invoked for observer1
  EXPECT_EQ(1, obs1->getCount());
  EXPECT_EQ(0, obs2->getCount());

  // remove both observers
  EXPECT_TRUE(store.remove(obs1));
  EXPECT_EQ(1, store.size());
  EXPECT_TRUE(store.remove(obs2));
  EXPECT_EQ(0, store.size());
}

TEST_F(ObserverContainerStoreTest, AddWhileIteratingPolicyCheckNoChange) {
  Store store;
  auto obs1 = std::make_shared<TestStoreObserver>();
  auto obs2 = std::make_shared<TestStoreObserver>();

  // add observer1, then during invoke, try to add observer2
  EXPECT_TRUE(store.add(obs1));
  EXPECT_EQ(1, store.size());
  store.invokeForEachObserver(
      [&obs1, &obs2, &store](auto* observer) {
        if (observer == obs1.get()) {
          // adding observers during iteration isn't allowed; expect exit
          EXPECT_EXIT(store.add(obs2), testing::KilledBySignal(SIGABRT), ".*");
        }
        observer->incrementCount();
      },
      Store::InvokeWhileIteratingPolicy::CheckNoChange);
}

TEST_F(ObserverContainerStoreTest, AddWhileIteratingPolicyCheckNoAdded) {
  Store store;
  auto obs1 = std::make_shared<TestStoreObserver>();
  auto obs2 = std::make_shared<TestStoreObserver>();

  // add observer1, then during invoke, try to add observer2
  EXPECT_TRUE(store.add(obs1));
  EXPECT_EQ(1, store.size());
  store.invokeForEachObserver(
      [&obs1, &obs2, &store](auto* observer) {
        if (observer == obs1.get()) {
          // adding observers during iteration isn't allowed; expect exit
          EXPECT_EXIT(store.add(obs2), testing::KilledBySignal(SIGABRT), ".*");
        }
        observer->incrementCount();
      },
      Store::InvokeWhileIteratingPolicy::CheckNoAdded);
}

TEST_F(ObserverContainerStoreTest, RemoveWhileIteratingPolicyInvokeAdded) {
  Store store;
  auto obs1 = std::make_shared<TestStoreObserver>();
  auto obs2 = std::make_shared<TestStoreObserver>();
  EXPECT_EQ(0, obs1->getCount());
  EXPECT_EQ(0, obs2->getCount());
  EXPECT_EQ(0, obs1->getCount()); // sanity check no side effects on getCount()
  EXPECT_EQ(0, obs2->getCount()); // sanity check no side effects on getCount()

  // add observers 1 and 2, then during invoke, remove observer2
  EXPECT_TRUE(store.add(obs1));
  EXPECT_TRUE(store.add(obs2));
  EXPECT_EQ(2, store.size());
  store.invokeForEachObserver(
      [&obs1, &obs2, &store](auto* observer) {
        if (observer == obs1.get()) {
          EXPECT_TRUE(store.remove(obs2)); // remove observer 2
        }
        observer->incrementCount();
      },
      Store::InvokeWhileIteratingPolicy::InvokeAdded);
  EXPECT_EQ(1, store.size());

  // incrementCount should have only been invoked for observer1
  // observer2 would have been removed already
  EXPECT_EQ(1, obs1->getCount());
  EXPECT_EQ(0, obs2->getCount());

  // remove observer 1
  EXPECT_TRUE(store.remove(obs1));
  EXPECT_EQ(0, store.size());
}

TEST_F(ObserverContainerStoreTest, RemoveWhileIteratingPolicyDoNotInvokeAdded) {
  Store store;
  auto obs1 = std::make_shared<TestStoreObserver>();
  auto obs2 = std::make_shared<TestStoreObserver>();
  EXPECT_EQ(0, obs1->getCount());
  EXPECT_EQ(0, obs2->getCount());
  EXPECT_EQ(0, obs1->getCount()); // sanity check no side effects on getCount()
  EXPECT_EQ(0, obs2->getCount()); // sanity check no side effects on getCount()

  // add observers 1 and 2, then during invoke, remove observer2
  EXPECT_TRUE(store.add(obs1));
  EXPECT_TRUE(store.add(obs2));
  EXPECT_EQ(2, store.size());
  store.invokeForEachObserver(
      [&obs1, &obs2, &store](auto* observer) {
        if (observer == obs1.get()) {
          EXPECT_TRUE(store.remove(obs2)); // remove observer 2
        }
        observer->incrementCount();
      },
      Store::InvokeWhileIteratingPolicy::DoNotInvokeAdded);
  EXPECT_EQ(1, store.size());

  // incrementCount should have only been invoked for observer1
  // observer2 would have been removed already
  EXPECT_EQ(1, obs1->getCount());
  EXPECT_EQ(0, obs2->getCount());

  // remove observer 1
  EXPECT_TRUE(store.remove(obs1));
  EXPECT_EQ(0, store.size());
}

TEST_F(ObserverContainerStoreTest, RemoveWhileIteratingPolicyCheckNoChange) {
  Store store;
  auto obs1 = std::make_shared<TestStoreObserver>();
  auto obs2 = std::make_shared<TestStoreObserver>();
  EXPECT_EQ(0, obs1->getCount());
  EXPECT_EQ(0, obs2->getCount());
  EXPECT_EQ(0, obs1->getCount()); // sanity check no side effects on getCount()
  EXPECT_EQ(0, obs2->getCount()); // sanity check no side effects on getCount()

  // add observers 1 and 2, then during invoke, remove observer2
  EXPECT_TRUE(store.add(obs1));
  EXPECT_TRUE(store.add(obs2));
  EXPECT_EQ(2, store.size());
  store.invokeForEachObserver(
      [&obs1, &obs2, &store](auto* observer) {
        if (observer == obs1.get()) {
          // adding observers during iteration isn't allowed; expect exit
          EXPECT_EXIT(
              store.remove(obs2), testing::KilledBySignal(SIGABRT), ".*");
        }
        observer->incrementCount();
      },
      Store::InvokeWhileIteratingPolicy::CheckNoChange);
}

TEST_F(ObserverContainerStoreTest, RemoveWhileIteratingPolicyCheckNoAdded) {
  Store store;
  auto obs1 = std::make_shared<TestStoreObserver>();
  auto obs2 = std::make_shared<TestStoreObserver>();
  EXPECT_EQ(0, obs1->getCount());
  EXPECT_EQ(0, obs2->getCount());
  EXPECT_EQ(0, obs1->getCount()); // sanity check no side effects on getCount()
  EXPECT_EQ(0, obs2->getCount()); // sanity check no side effects on getCount()

  // add observers 1 and 2, then during invoke, remove observer2
  EXPECT_TRUE(store.add(obs1));
  EXPECT_TRUE(store.add(obs2));
  EXPECT_EQ(2, store.size());
  store.invokeForEachObserver(
      [&obs1, &obs2, &store](auto* observer) {
        if (observer == obs1.get()) {
          EXPECT_TRUE(store.remove(obs2)); // remove observer 2
        }
        observer->incrementCount();
      },
      Store::InvokeWhileIteratingPolicy::CheckNoAdded);
  EXPECT_EQ(1, store.size());

  // incrementCount should have only been invoked for observer1
  // observer2 would have been removed already
  EXPECT_EQ(1, obs1->getCount());
  EXPECT_EQ(0, obs2->getCount());

  // remove observer 1
  EXPECT_TRUE(store.remove(obs1));
  EXPECT_EQ(0, store.size());
}
