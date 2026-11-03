# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# pyre-strict

import asyncio
import itertools
import random
from contextvars import copy_context
from functools import partial
from sys import platform
from unittest import IsolatedAsyncioTestCase

import folly.python.test.request_context_helper as frc_helper
from folly import request_context


async def get_Context(
    pass_ctx: request_context.Context,
) -> request_context.Context | None:
    ctx = request_context.get_from_contextvar()
    assert pass_ctx == ctx, f"Expected {pass_ctx} but got {ctx}"
    for _ in range(25):
        await asyncio.sleep(0)
        assert (
            ctx
            == request_context.get_from_contextvar()
            == pass_ctx
            == request_context.save()
        )
    return request_context.get_from_contextvar()


class RequestContextTest(IsolatedAsyncioTestCase):
    def test_ensure(self) -> None:
        # Even though this does nothing its a more nature way to ensure that
        # the module is loaded, its not empty
        self.assertIsNone(request_context.ensure())

    # A context set from another native module is invisible on Windows, where
    # each extension links its own RequestContext state, so these tests only
    # exist on other platforms instead of skipping there.
    if not platform.startswith("win"):

        def test_use_count(self) -> None:
            ctx = request_context.Context()
            # ctx is a nullptr capsule, its use count is 0
            self.assertEqual(ctx.use_count(), 0)
            self.assertFalse(ctx)
            del ctx
            # Set the frc to something.
            frc_helper.setContext()
            # Fetch that ctx, from the PyContext
            ctx = request_context.get_from_contextvar()
            # ctx, and folly threadlocal
            self.assertEqual(ctx.use_count(), 2)
            new_ctx = request_context.save()
            self.assertEqual(new_ctx.use_count(), 3)
            del new_ctx
            self.assertEqual(ctx.use_count(), 2)
            # Set the frc to something else
            frc_helper.setContext()
            # Just ctx still exists.
            self.assertEqual(ctx.use_count(), 1)

    async def test_memory_leaks(self) -> None:
        with request_context.active() as ctx:
            # Owners
            # 1. ctx ( also the same as the PyContext capsule )
            # 2. folly::RequestContext:: threadlocal
            self.assertEqual(ctx.use_count(), 2)
            self.assertEqual(await get_Context(ctx), ctx)
            self.assertEqual(ctx.use_count(), 2)
            # Copy_context shouldn't leak
            context = copy_context()
            # Still only one capsule.
            self.assertEqual(ctx.use_count(), 2)
        # Owner 2 goes away because the FRC is not set
        self.assertEqual(ctx.use_count(), 1)
        del context
        self.assertEqual(ctx.use_count(), 1)

    # Task switches never propagate to the folly RequestContext on Windows,
    # where Python has no interpreter context watcher, so these tests only
    # exist on other platforms instead of skipping there.
    if not platform.startswith("win"):

        async def test_multiple_ctx_copies_memory_leak(self) -> None:
            tasks = []
            ctx1: request_context.Context
            with request_context.active() as ctx1:
                for _ in range(20):
                    tasks.append(asyncio.create_task(get_Context(ctx1)))

            ctx2: request_context.Context
            with request_context.active() as ctx2:
                for _ in range(28):
                    tasks.append(asyncio.create_task(get_Context(ctx2)))
            # Single PyCapsule and ctx object
            self.assertEqual(ctx1.use_count(), 1)
            self.assertEqual(ctx2.use_count(), 1)

            # Now there are 20 PyContext objects

            async def test() -> None:
                await asyncio.sleep(0)
                self.assertEqual(ctx1.use_count(), 1)
                await asyncio.sleep(0)
                self.assertEqual(ctx2.use_count(), 1)

            tasks.append(asyncio.create_task(test()))

            random.shuffle(tasks)

            await asyncio.gather(*tasks)

            self.assertEqual(ctx1.use_count(), 1)
            self.assertEqual(ctx2.use_count(), 1)

            del tasks

            self.assertEqual(ctx1.use_count(), 1)
            self.assertEqual(ctx2.use_count(), 1)

        async def test_request_context(self) -> None:
            with request_context.active() as ctx1:
                # Create task used copy_context() during its creation
                # So as soon as the task is created we can "Restore" the
                # Previous folly context

                # Will active, save and get_from_contextvar() should equal
                self.assertEqual(
                    request_context.get_from_contextvar(), request_context.save()
                )
                task1 = asyncio.create_task(get_Context(ctx1))
                self.assertEqual(ctx1.use_count(), 2)  # New Context but same PyCapsule

            with request_context.active() as ctx2:
                task2 = asyncio.create_task(get_Context(ctx2))

            with request_context.active() as ctx3:
                task3 = asyncio.create_task(get_Context(ctx3))

            # Lets have a task with the NULL RequestContext
            task4 = asyncio.create_task(
                get_Context(request_context.get_from_contextvar())
            )

            await asyncio.wait([task1, task2, task3, task4])

            self.assertEqual(await task1, ctx1)
            self.assertEqual(await task2, ctx2)
            self.assertEqual(await task3, ctx3)
            # Insure that the NULL ctx is not in any of the saved RequestContexts
            self.assertNotIn(request_context.get_from_contextvar(), [ctx1, ctx2, ctx3])
            self.assertEqual(await task4, request_context.get_from_contextvar())
            # Insure that all the ctx are not actually the same ctx in disguise
            for ctxa, ctxb in itertools.permutations(
                (ctx1, ctx2, ctx3, request_context.get_from_contextvar()), 2
            ):
                self.assertNotEqual(ctxa, ctxb)

        async def test_callbacks(self) -> None:
            fut1 = asyncio.Future()
            fut2 = asyncio.Future()
            fut3 = asyncio.Future()

            def callback(
                ctx: request_context.Context, fut: asyncio.Future, task: asyncio.Task
            ) -> None:
                try:
                    current_ctx = request_context.save()
                    self.assertEqual(ctx, current_ctx)
                    fut.set_result(current_ctx)
                except Exception as e:
                    fut.set_exception(e)

            ctx = request_context.get_from_contextvar()
            with request_context.active() as ctx1:
                task1 = asyncio.create_task(get_Context(ctx1))
                # Callbacks execut with the context at the time they are added, or the context passed in.
                task1.add_done_callback(partial(callback, ctx1, fut1))
                context = copy_context()

            expected = request_context.get_from_contextvar()
            self.assertNotEqual(expected, ctx1)
            self.assertEqual(expected, ctx)
            # Since this one was added out here it should execute with this context
            task1.add_done_callback(partial(callback, ctx, fut2))
            # We want it to execute with the context that we had active above
            task1.add_done_callback(partial(callback, ctx1, fut3), context=context)
            self.assertEqual(await task1, ctx1)
            self.assertEqual(await fut1, ctx1)
            self.assertEqual(await fut2, ctx)
            self.assertEqual(await fut3, ctx1)

    # This test needs both directions: a context set from another native module
    # must be visible here, and task switches must propagate back to folly.
    # Neither holds on Windows, so it only exists on other platforms instead
    # of skipping there.
    if not platform.startswith("win"):

        def test_fRC_setContext_is_observed(self) -> None:
            # This is simulating some code setting the context in C++ land
            frc_helper.setContext()
            # Lets get a Context wrapper object to represent this var
            set_ctx: request_context.Context = request_context.save()

            # Without observing setContext calls we can't automatically update the contextvar
            expected = request_context.get_from_contextvar()
            self.assertEqual(expected, set_ctx)

            touched = 0

            async def test_context() -> None:
                await asyncio.sleep(0)
                nonlocal touched
                ctx = request_context.save()
                self.assertEqual(ctx, set_ctx)
                touched += 1

            async def set_context() -> None:
                nonlocal touched
                frc_helper.setContext()
                # Moved this to the most problematic spot if something was going to change it during task switching
                await asyncio.sleep(0)
                ctx = request_context.save()
                self.assertNotEqual(ctx, set_ctx)
                touched += 1

            loop = asyncio.new_event_loop()
            # We never set the PyContext explicitly, only the SetContextwatcher can make sure its set
            # its expected that it will inherit set_ctx
            task1 = loop.create_task(test_context())
            task2 = loop.create_task(set_context())
            task3 = loop.create_task(test_context())
            task4 = loop.create_task(set_context())

            loop.run_until_complete(asyncio.wait((task1, task2, task3, task4)))
            loop.close()

            self.assertEqual(touched, 4)

            # Even though one of the tasks set the context, it should revert to set_ctx
            self.assertEqual(request_context.get_from_contextvar(), set_ctx)
            self.assertEqual(request_context.save(), set_ctx)

    async def test_on_thread(self) -> None:
        # This is simulating some code setting the context in C++ land
        frc_helper.setContext()

        # Without observing setContext calls we can't automatically update the contextvar
        expected = request_context.get_from_contextvar()
        self.assertEqual(expected, request_context.save())

        def on_thread() -> request_context.Context:
            return request_context.get_from_contextvar()

        # to_thread() copies the current context, so the thread uses it so it should have the same FRC
        self.assertEqual(await asyncio.to_thread(on_thread), expected)

        empty_ctx = request_context.Context()

        # For now the standard loop.run_in_executor does not propogate context
        loop = asyncio.get_running_loop()
        self.assertEqual(await loop.run_in_executor(None, on_thread), empty_ctx)
