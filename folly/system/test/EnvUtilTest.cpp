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

#include <folly/system/EnvUtil.h>

#include <spawn.h>

#include <folly/Subprocess.h>
#include <folly/container/Array.h>
#include <folly/portability/Fcntl.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Stdlib.h>

#include <glog/logging.h>

using namespace folly;
using folly::experimental::EnvironmentState;
using folly::experimental::MalformedEnvironment;
using folly::test::EnvVarSaver;

static std::map<std::string, std::string> parseEnvVars(std::string_view text) {
  std::map<std::string, std::string> ret;
  std::vector<std::string_view> lines;
  split('\n', text, lines);
  for (auto line : lines) {
    if (line.empty()) {
      continue;
    }
    std::vector<std::string> pieces;
    split('=', line, pieces);
    ret[std::move(pieces.at(0))] = std::move(pieces.at(1));
  }
  return ret;
}

TEST(EnvVarSaverTest, ExampleNew) {
  auto key = "hahahahaha";
  EXPECT_EQ(nullptr, getenv(key));

  PCHECK(0 == setenv(key, "", true));
  EXPECT_STREQ("", getenv(key));
  PCHECK(0 == unsetenv(key));
  EXPECT_EQ(nullptr, getenv(key));

  auto saver = std::make_unique<EnvVarSaver>();
  PCHECK(0 == setenv(key, "blah", true));
  EXPECT_STREQ("blah", getenv(key));
  saver = nullptr;
  EXPECT_EQ(nullptr, getenv(key));
}

TEST(EnvVarSaverTest, ExampleExisting) {
  auto key = "PATH";
  EXPECT_NE(nullptr, getenv(key));
  auto value = std::string{getenv(key)};

  auto saver = std::make_unique<EnvVarSaver>();
  PCHECK(0 == setenv(key, "blah", true));
  EXPECT_STREQ("blah", getenv(key));
  saver = nullptr;
  EXPECT_EQ(value, getenv(key));
}

TEST(EnvVarSaverTest, Movable) {
  Optional<EnvVarSaver> pSaver1;
  pSaver1.emplace();
  auto key = "PATH";
  EXPECT_NE(nullptr, getenv(key));
  auto value = std::string{getenv(key)};
  Optional<EnvVarSaver> pSaver2;
  pSaver2.emplace(std::move(*pSaver1));
  pSaver1.reset();
  PCHECK(0 == setenv(key, "blah", true));
  EXPECT_STREQ("blah", getenv(key));
  pSaver2.reset();
  EXPECT_EQ(value, getenv(key));
}

TEST(EnvironmentStateTest, FailOnEmptyString) {
  EnvVarSaver saver{};
  char test[4] = "A=B";
  PCHECK(0 == putenv(test));
  auto okState = EnvironmentState::fromCurrentEnvironment();
  test[0] = 0;
  SCOPE_EXIT {
    test[0] = 'A';
  };
  EXPECT_THROW(
      EnvironmentState::fromCurrentEnvironment(), MalformedEnvironment);
}

TEST(EnvironmentStateTest, MovableAndCopyable) {
  auto initialState = EnvironmentState::fromCurrentEnvironment();
  auto copiedState1 = EnvironmentState::empty();
  copiedState1.operator=(initialState);
  EnvironmentState copiedState2{initialState};
  EXPECT_EQ(*initialState, *copiedState1);
  EXPECT_EQ(*initialState, *copiedState2);
  (*initialState)["foo"] = "bar";
  EXPECT_EQ(0, copiedState1->count("foo"));
  EXPECT_EQ(0, copiedState2->count("foo"));
  auto movedState1 = EnvironmentState::empty();
  movedState1.operator=(std::move(copiedState1));
  EnvironmentState movedState2{std::move(copiedState2)};
  EXPECT_EQ(0, movedState1->count("foo"));
  EXPECT_EQ(0, movedState2->count("foo"));
  initialState->erase("foo");
  EXPECT_EQ(*initialState, *movedState1);
  EXPECT_EQ(*initialState, *movedState2);
}

TEST(EnvironmentStateTest, FailOnDuplicate) {
  EnvVarSaver saver{};
  char test[7] = "PATG=B";
  PCHECK(0 == putenv(test));
  auto okState = EnvironmentState::fromCurrentEnvironment();
  test[3] = 'H';
  SCOPE_EXIT {
    test[3] = 'G';
  };
  EXPECT_THROW(
      EnvironmentState::fromCurrentEnvironment(), MalformedEnvironment);
}

TEST(EnvironmentStateTest, Separation) {
  EnvVarSaver saver{};
  auto initialState = EnvironmentState::fromCurrentEnvironment();
  PCHECK(0 == setenv("spork", "foon", true));
  auto updatedState = EnvironmentState::fromCurrentEnvironment();
  EXPECT_EQ(0, initialState->count("spork"));
  EXPECT_EQ(1, updatedState->count("spork"));
  EXPECT_EQ("foon", (*updatedState)["spork"]);
  updatedState->erase("spork");
  EXPECT_EQ(0, updatedState->count("spork"));
  EXPECT_STREQ("foon", getenv("spork"));
}

TEST(EnvironmentStateTest, Update) {
  EnvVarSaver saver{};
  auto env = EnvironmentState::fromCurrentEnvironment();
  EXPECT_EQ(nullptr, getenv("spork"));
  (*env)["spork"] = "foon";
  EXPECT_EQ(nullptr, getenv("spork"));
  env.setAsCurrentEnvironment();
  EXPECT_STREQ("foon", getenv("spork"));
}

TEST(EnvironmentStateTest, SetAsCurrentEnvironmentFailsOnEmptyKey) {
  EnvVarSaver saver{};
  auto env = EnvironmentState::empty();
  (*env)[""] = "value";
  EXPECT_THROW(env.setAsCurrentEnvironment(), MalformedEnvironment);
}

TEST(EnvironmentStateTest, SetAsCurrentEnvironmentFailsOnKeyContainingEquals) {
  EnvVarSaver saver{};
  auto env = EnvironmentState::empty();
  (*env)["FOO=BAR"] = "value";
  EXPECT_THROW(env.setAsCurrentEnvironment(), MalformedEnvironment);
}

TEST(EnvironmentStateTest, forSubprocess) {
  auto env = EnvironmentState::empty();
  (*env)["spork"] = "foon";
  std::vector<std::string> expected = {"spork=foon"};
  auto vec = env.toVector();
  EXPECT_EQ(expected, vec);
  auto args = std::vector<std::string>{"/usr/bin/env"};
  auto opts = Subprocess::Options().pipeStdout();
  Subprocess subProcess{args, opts, args[0].c_str(), &vec};
  std::string out;
  ASSERT_TRUE(readFile(subProcess.stdoutFd(), out));
  ASSERT_EQ(0, subProcess.wait().exitStatus());
  EXPECT_EQ("foon", parseEnvVars(out).at("spork"));
}

TEST(EnvironmentStateTest, forC) {
  auto env = EnvironmentState::empty();
  (*env)["spork"] = "foon";
  EXPECT_STREQ("spork=foon", env.toPointerArray().get()[0]);
  EXPECT_EQ(nullptr, env.toPointerArray().get()[1]);
  pid_t pid;
  char program[] = "/usr/bin/env";
  auto argV = folly::make_array(program, nullptr);
  int pipefd[2];
  pipe(pipefd);
  posix_spawn_file_actions_t file_actions;
  posix_spawn_file_actions_init(&file_actions);
  posix_spawn_file_actions_adddup2(&file_actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&file_actions, pipefd[0]);
  posix_spawn_file_actions_addclose(&file_actions, pipefd[1]);
  PCHECK(
      0 ==
      posix_spawn(
          &pid,
          program,
          &file_actions,
          nullptr,
          argV.data(),
          env.toPointerArray().get()));
  posix_spawn_file_actions_destroy(&file_actions);
  close(pipefd[1]);
  std::string out;
  ASSERT_TRUE(readFile(pipefd[0], out));
  int result;
  PCHECK(pid == waitpid(pid, &result, 0));
  close(pipefd[0]);
  EXPECT_EQ(0, result);
  EXPECT_EQ("foon", parseEnvVars(out).at("spork"));
}

TEST(EnvVarSaverTest, ExampleDeleting) {
  auto key = "PATH";
  EXPECT_NE(nullptr, getenv(key));
  auto value = std::string{getenv(key)};

  auto saver = std::make_unique<EnvVarSaver>();
  PCHECK(0 == unsetenv(key));
  EXPECT_EQ(nullptr, getenv(key));
  saver = nullptr;
  EXPECT_TRUE(value == getenv(key));
}
