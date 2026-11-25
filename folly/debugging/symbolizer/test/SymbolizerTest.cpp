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

#include <folly/debugging/symbolizer/Symbolizer.h>

#include <signal.h>
#include <array>
#include <cstdlib>

#include <glog/logging.h>
#include <folly/Demangle.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/debugging/symbolizer/ElfCache.h>
#include <folly/debugging/symbolizer/SymbolizedFrame.h>
#include <folly/debugging/symbolizer/test/SymbolizerTestUtils.h>
#include <folly/lang/Cast.h>
#include <folly/portability/Filesystem.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Unistd.h>
#include <folly/synchronization/Baton.h>
#include <folly/test/TestUtils.h>

#if FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF

namespace folly {
namespace symbolizer {
namespace test {

#define SCOPED_TRACE_FRAMES(frames) \
  SCOPED_TRACE([&] {                \
    StringSymbolizePrinter printer; \
    printer.println(frames);        \
    return printer.str();           \
  }())

void foo() {}

FOLLY_NOINLINE void bar() {
  std::array<int, 2> a = {{1, 2}};
  // Use lfind, which is in a different library
  int key = 1;
  unsigned long nmemb = 2;
  lfind(&key, a.data(), &nmemb, sizeof(int), testComparator);
}

TEST(Symbolizer, Single) {
  SKIP_IF(!Symbolizer::isAvailable());

  // It looks like we could only use .debug_aranges with "-g2", with
  // "-g1 -gdwarf-aranges", the code has to fallback to line-tables to
  // get the file name.
  Symbolizer symbolizer(LocationInfoMode::FULL);
  SymbolizedFrame a;
  ASSERT_TRUE(symbolizer.symbolize(reinterpret_cast<uintptr_t>(foo), a));
  EXPECT_EQ("folly::symbolizer::test::foo()", folly::demangle(a.name));

  auto path = a.location.file.toString();
  folly::StringPiece basename(path);
  auto pos = basename.rfind('/');
  if (pos != folly::StringPiece::npos) {
    basename.advance(pos + 1);
  }
  EXPECT_EQ("SymbolizerTest.cpp", basename.str());
}

TEST(Symbolizer, SingleCustomExePath) {
  SKIP_IF(!Symbolizer::isAvailable());

  auto pid = ::fork();
  if (pid == -1) {
    SKIP("fork failed");
  } else if (pid == 0) {
    // child process waits forever, parent will kill
    folly::Baton<> baton;
    baton.wait();
  } else {
    // parent process

    // kill the child process on cleanup
    auto guard = folly::makeGuard([pid] {
      auto error = ::kill(pid, SIGKILL);
      ASSERT_EQ(0, error);
    });

    // path to executable for child binary
    auto exePath = folly::to<std::string>("/proc/", pid, "/exe");

    // It looks like we could only use .debug_aranges with "-g2", with
    // "-g1 -gdwarf-aranges", the code has to fallback to line-tables to
    // get the file name.
    Symbolizer symbolizer(
        nullptr, LocationInfoMode::FULL, 0, std::move(exePath));
    SymbolizedFrame a;
    ASSERT_TRUE(symbolizer.symbolize(reinterpret_cast<uintptr_t>(foo), a));
    EXPECT_EQ("folly::symbolizer::test::foo()", folly::demangle(a.name));

    auto path = a.location.file.toString();
    folly::StringPiece basename(path);
    auto pos = basename.rfind('/');
    if (pos != folly::StringPiece::npos) {
      basename.advance(pos + 1);
    }
    EXPECT_EQ("SymbolizerTest.cpp", basename.str());
  }
}

// Test stack frames...

class ElfCacheTest : public testing::Test {
 protected:
  void SetUp() override;
};

// Capture "golden" stack trace with default-configured Symbolizer
FrameArray<100> goldenFrames;

void ElfCacheTest::SetUp() {
  SKIP_IF(!Symbolizer::isAvailable());

  gComparatorGetStackTraceArg = &goldenFrames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  bar();

  Symbolizer symbolizer;
  symbolizer.symbolize(goldenFrames);
  // At least 3 stack frames from us + getStackTrace()
  ASSERT_LE(4, goldenFrames.frameCount);
}

void runElfCacheTest(Symbolizer& symbolizer) {
  FrameArray<100> frames = goldenFrames;
  for (size_t i = 0; i < frames.frameCount; ++i) {
    frames.frames[i].clear();
  }
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  ASSERT_LE(4, frames.frameCount);
  for (size_t i = 1; i < 4; ++i) {
    EXPECT_STREQ(goldenFrames.frames[i].name, frames.frames[i].name);
  }
}

TEST_F(ElfCacheTest, ElfCache) {
  ElfCache cache;
  Symbolizer symbolizer(&cache);
  for (size_t i = 0; i < 2; ++i) {
    runElfCacheTest(symbolizer);
  }
}

TEST_F(ElfCacheTest, SignalSafeElfCache) {
  SignalSafeElfCache cache;
  Symbolizer symbolizer(&cache);
  for (size_t i = 0; i < 2; ++i) {
    runElfCacheTest(symbolizer);
  }
}

struct SignalSafeElfCacheTestAccess : SignalSafeElfCache {
  using SignalSafeElfCache::Path;
};

TEST(SignalSafeElfCache, PathViewExcludesNullTerminator) {
  reentrant_allocator<char> alloc{reentrant_allocator_options()};
  const std::array<char, 3> bytes = {'a', 'b', 'c'};
  SignalSafeElfCacheTestAccess::Path path(bytes.data(), bytes.size(), alloc);
  const cstring_view view = path;

  EXPECT_EQ(cstring_view("abc"), view);
  EXPECT_EQ(std::string_view("abc"), std::string_view(view));
  EXPECT_STREQ("abc", view.c_str());
}

TEST(SignalSafeElfCache, PathStringOrdering) {
  reentrant_allocator<char> alloc{reentrant_allocator_options()};
  SignalSafeElfCacheTestAccess::Path empty("", 0, alloc);
  SignalSafeElfCacheTestAccess::Path prefix("a", 1, alloc);
  SignalSafeElfCacheTestAccess::Path longer("ab", 2, alloc);
  SignalSafeElfCacheTestAccess::Path high("\x80", 1, alloc);
  SignalSafeElfCacheTestAccess::Path equal("a", 1, alloc);

  EXPECT_TRUE(empty < prefix);
  EXPECT_TRUE(prefix < longer);
  EXPECT_TRUE(longer < high);
  EXPECT_FALSE(longer < prefix);
  EXPECT_FALSE(prefix < equal);
  EXPECT_FALSE(equal < prefix);
}

TEST(SignalSafeElfCache, NonNullTerminatedLookup) {
  SignalSafeElfCache cache;
  const char path[] = "/proc/self/exe?";
  const StringPiece lookup(path, sizeof(path) - 2);
  auto first = cache.getFile(lookup);

  ASSERT_NE(nullptr, first);
  EXPECT_EQ(first, cache.getFile(lookup));
  EXPECT_EQ(first, cache.getFile("/proc/self/exe"));
}

TEST(SignalSafeElfCache, MissingPathLeavesNoEntry) {
  SignalSafeElfCache cache;
  const size_t before = cache.size();
  EXPECT_EQ(nullptr, cache.getFile("/nonexistent-elf-file-for-test"));
  EXPECT_EQ(before, cache.size());
  EXPECT_EQ(nullptr, cache.getFile("/nonexistent-elf-file-for-test"));
  EXPECT_EQ(before, cache.size());
}

TEST(SymbolizerTest, SymbolCache) {
  SKIP_IF(!Symbolizer::isAvailable());

  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL, 100);

  FrameArray<100> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  bar();
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  FrameArray<100> frames2;
  gComparatorGetStackTraceArg = &frames2;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  bar();
  symbolizer.symbolize(frames2);
  SCOPED_TRACE_FRAMES(frames2);

  for (size_t i = 0; i < frames.frameCount; i++) {
    EXPECT_STREQ(frames.frames[i].name, frames2.frames[i].name);
  }
}

namespace {

void expectFrameEq(
    SymbolizedFrame frame,
    const std::string& shortName,
    const std::string& fullName,
    const std::string& file,
    size_t lineno) {
  auto normalizePath = [](std::string path) {
    path = fs::lexically_normal(path).native();
    return (path.find("./", 0) != std::string::npos) ? path.substr(2) : path;
  };
  auto demangled = folly::demangle(frame.name);
  EXPECT_TRUE(demangled == shortName || demangled == fullName)
      << "Found: demangled=" << demangled << " expecting shortName="
      << shortName << " or fullName=" << fullName << " address: " << frame.addr
      << " hex(address): " << std::hex << frame.addr;
  // Use endsWith in case the build system adds extra paths in front.
  EXPECT_TRUE(
      folly::StringPiece(normalizePath(frame.location.file.toString()))
          .endsWith(normalizePath(file)))
      << ' ' << fullName << " address: " << frame.addr
      << " hex(address): " << std::hex << frame.addr
      << " frame.location.file.toString(): " << frame.location.file.toString()
      << " normalizePath(frame.location.file.toString()): "
      << normalizePath(frame.location.file.toString()) //
      << " file: " << file //
      << " normalizePath(file): " << normalizePath(file);
  EXPECT_EQ(frame.location.line, lineno) << ' ' << fullName;
}

template <size_t kNumFrames = 100>
void expectFramesEq(
    const FrameArray<kNumFrames>& frames1,
    const FrameArray<kNumFrames>& frames2) {
  EXPECT_EQ(frames1.frameCount, frames2.frameCount);
  for (size_t i = 0; i < frames1.frameCount; i++) {
    EXPECT_STREQ(frames1.frames[i].name, frames2.frames[i].name);
  }
}

template <size_t kNumFrames = 100>
void printFrames(const FrameArray<kNumFrames>& frames) {
  for (size_t i = 0; i < frames.frameCount; i++) {
    auto& frame = frames.frames[i];
    LOG(INFO) << std::hex << frame.addr << ": " << frame.name << " in "
              << frame.location.file.toString() << ":" << std::dec
              << frame.location.line;
  }
}

} // namespace

TEST(SymbolizerTest, InlineFunctionBasic) {
  SKIP_IF(!Symbolizer::isAvailable());

  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL_WITH_INLINE, 0);

  FrameArray<100> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  call_inlineB_inlineA_lfind();
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  printFrames(frames);

  expectFrameEq(
      frames.frames[4],
      "inlineA_lfind",
      "folly::symbolizer::test::inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_lfind);

  expectFrameEq(
      frames.frames[5],
      "inlineB_inlineA_lfind",
      "folly::symbolizer::test::inlineB_inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_inlineA_lfind);

  FrameArray<100> frames2;
  gComparatorGetStackTraceArg = &frames2;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  call_inlineB_inlineA_lfind();
  symbolizer.symbolize(frames2);

  expectFramesEq(frames, frames2);
}

FOLLY_NOINLINE void call_B_A_lfind() {
  kLineno_inlineB_inlineA_lfind = __LINE__ + 1;
  inlineB_inlineA_lfind();
}

TEST(SymbolizerTest, InlineFunctionWithoutEnoughFrames) {
  SKIP_IF(!Symbolizer::isAvailable());

  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL_WITH_INLINE, 0);

  FrameArray<100> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  call_B_A_lfind();
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  std::array<SymbolizedFrame, 2> limitedFrames;

  // The address of the line where call_B_A_lfind calls inlineB_inlineA_lfind.
  symbolizer.symbolize(
      folly::Range<const uintptr_t*>(&frames.addresses[4], 1),
      folly::range(limitedFrames));

  expectFrameEq(
      limitedFrames[0],
      "inlineB_inlineA_lfind",
      "folly::symbolizer::test::inlineB_inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_inlineA_lfind);
  expectFrameEq(
      limitedFrames[1],
      "call_B_A_lfind",
      "folly::symbolizer::test::call_B_A_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTest.cpp",
      kLineno_inlineB_inlineA_lfind);
}

TEST(SymbolizerTest, InlineFunctionInLexicalBlock) {
  SKIP_IF(!Symbolizer::isAvailable());

  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL_WITH_INLINE, 0);

  FrameArray<100> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  call_lexicalBlock_inlineB_inlineA_lfind();
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  expectFrameEq(
      frames.frames[4],
      "inlineA_lfind",
      "folly::symbolizer::test::inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_lfind);

  expectFrameEq(
      frames.frames[5],
      "inlineB_inlineA_lfind",
      "folly::symbolizer::test::inlineB_inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_inlineA_lfind);

  expectFrameEq(
      frames.frames[6],
      "lexicalBlock_inlineB_inlineA_lfind",
      "folly::symbolizer::test::lexicalBlock_inlineB_inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils.cpp",
      kLineno_inlineB_inlineA_lfind);

  printFrames(frames);
}

TEST(SymbolizerTest, InlineFunctionInDifferentCompilationUnit) {
  SKIP_IF(!Symbolizer::isAvailable());

  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL_WITH_INLINE, 0);

  FrameArray<100> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  // NOTE: inlineLTO_inlineA_lfind is only inlined with LTO/ThinLTO.
  call_inlineLTO_inlineA_lfind();
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  expectFrameEq(
      frames.frames[5],
      "inlineLTO_inlineA_lfind",
      "folly::symbolizer::test::inlineLTO_inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils.cpp",
      kLineno_inlineA_lfind);
}

TEST(SymbolizerTest, InlineClassMemberFunctionSameFile) {
  SKIP_IF(!Symbolizer::isAvailable());

  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL_WITH_INLINE, 0);

  FrameArray<100> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  call_same_file_memberInline_inlineA_lfind();
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  expectFrameEq(
      frames.frames[4],
      "inlineA_lfind",
      "folly::symbolizer::test::inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_lfind);

  expectFrameEq(
      frames.frames[5],
      "memberInline_inlineA_lfind",
      "folly::symbolizer::test::ClassSameFile::memberInline_inlineA_lfind() const",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils.cpp",
      kLineno_inlineA_lfind);
}

TEST(SymbolizerTest, StaticInlineClassMemberFunctionSameFile) {
  SKIP_IF(!Symbolizer::isAvailable());

  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL_WITH_INLINE, 0);

  FrameArray<100> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  call_same_file_staticMemberInline_inlineA_lfind();
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  expectFrameEq(
      frames.frames[4],
      "inlineA_lfind",
      "folly::symbolizer::test::inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_lfind);

  expectFrameEq(
      frames.frames[5],
      "staticMemberInline_inlineA_lfind",
      "folly::symbolizer::test::ClassSameFile::staticMemberInline_inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils.cpp",
      kLineno_inlineA_lfind);
}

TEST(SymbolizerTest, InlineClassMemberFunctionInDifferentFile) {
  SKIP_IF(!Symbolizer::isAvailable());

  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL_WITH_INLINE, 0);

  FrameArray<100> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  call_different_file_memberInline_inlineA_lfind();
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  expectFrameEq(
      frames.frames[4],
      "inlineA_lfind",
      "folly::symbolizer::test::inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_lfind);

  expectFrameEq(
      frames.frames[5],
      "memberInline_inlineA_lfind",
      "folly::symbolizer::test::ClassDifferentFile::memberInline_inlineA_lfind() const",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_inlineA_lfind);
}

TEST(SymbolizerTest, StaticInlineClassMemberFunctionInDifferentFile) {
  SKIP_IF(!Symbolizer::isAvailable());

  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL_WITH_INLINE, 0);

  FrameArray<100> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  call_different_file_staticMemberInline_inlineA_lfind();
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  expectFrameEq(
      frames.frames[4],
      "inlineA_lfind",
      "folly::symbolizer::test::inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_lfind);

  expectFrameEq(
      frames.frames[5],
      "staticMemberInline_inlineA_lfind",
      "folly::symbolizer::test::ClassDifferentFile::staticMemberInline_inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_inlineA_lfind);
}

// No inline frames should be filled because of no extra frames.
TEST(SymbolizerTest, InlineFunctionNoExtraFrames) {
  SKIP_IF(!Symbolizer::isAvailable());

  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL_WITH_INLINE, 100);
  FrameArray<9> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<9>);
  call_inlineB_inlineA_lfind();
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  Symbolizer symbolizer2(nullptr, LocationInfoMode::FULL, 100);
  FrameArray<9> frames2;
  gComparatorGetStackTraceArg = &frames2;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<9>);
  call_inlineB_inlineA_lfind();
  symbolizer2.symbolize(frames2);
  SCOPED_TRACE_FRAMES(frames2);

  expectFramesEq<9>(frames, frames2);
}

TEST(SymbolizerTest, InlineFunctionWithCache) {
  SKIP_IF(!Symbolizer::isAvailable());

  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL_WITH_INLINE, 100);

  FrameArray<100> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  call_inlineB_inlineA_lfind();
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  expectFrameEq(
      frames.frames[4],
      "inlineA_lfind",
      "folly::symbolizer::test::inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_lfind);

  expectFrameEq(
      frames.frames[5],
      "inlineB_inlineA_lfind",
      "folly::symbolizer::test::inlineB_inlineA_lfind()",
      "folly/debugging/symbolizer/test/SymbolizerTestUtils-inl.h",
      kLineno_inlineA_lfind);

  FrameArray<100> frames2;
  gComparatorGetStackTraceArg = &frames2;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  call_inlineB_inlineA_lfind();
  symbolizer.symbolize(frames2);
  expectFramesEq(frames, frames2);
}

// A cached entry carrying inline frames must still symbolize the non-inlined
// call when there is no room left to expand the inline frames into.
TEST(SymbolizerTest, InlineFunctionCacheHitNoExtraFrames) {
  SKIP_IF(!Symbolizer::isAvailable());

  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL_WITH_INLINE, 100);

  // Populate the cache with entries that have inline frames.
  FrameArray<100> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  call_inlineB_inlineA_lfind();
  symbolizer.symbolize(frames);
  SCOPED_TRACE_FRAMES(frames);

  FrameArray<9> frames2;
  gComparatorGetStackTraceArg = &frames2;
  gComparatorGetStackTrace =
      reinterpret_function_cast<bool(void*)>(getStackTrace<9>);
  call_inlineB_inlineA_lfind();
  symbolizer.symbolize(frames2);
  SCOPED_TRACE_FRAMES(frames2);

  // Every slot is taken by an address, so there is nowhere to put the cached
  // inline frames.
  ASSERT_EQ(9, frames2.frameCount);
  for (size_t i = 0; i < frames2.frameCount; ++i) {
    EXPECT_TRUE(frames2.frames[i].found) << "frame " << i << " not symbolized";
  }
}

int64_t functionWithTwoParameters(size_t a, int32_t b) {
  return a + b;
}

TEST(Dwarf, FindParameterNames) {
  SKIP_IF(!Symbolizer::isAvailable());

  ElfCache elfCache;

  auto address = reinterpret_cast<uintptr_t>(functionWithTwoParameters);
  Symbolizer symbolizer;
  SymbolizedFrame frame;
  ASSERT_TRUE(symbolizer.symbolize(address, frame));

  std::vector<folly::StringPiece> names;
  Dwarf dwarf(&elfCache, frame.file.get());

  folly::Range<SymbolizedFrame*> extraInlineFrames = {};
  dwarf.findAddress(
      frame.addr,
      LocationInfoMode::FAST,
      frame,
      extraInlineFrames,
      [&](const folly::StringPiece name) { names.push_back(name); });

  if (names.empty()) {
    // When using -fsplit-dwarf-inlining info about parameters will not be
    // emitted for the inlined debug info.
    return;
  }

  ASSERT_EQ(2, names.size());
  ASSERT_EQ("a", names[0]);
  ASSERT_EQ("b", names[1]);
}

#undef SCOPED_TRACE_FRAMES

} // namespace test
} // namespace symbolizer
} // namespace folly

#endif // FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF

// Can't use initFacebookLight since that would install its own signal handlers
// Can't use initFacebookNoSignals since we cannot depend on common
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
