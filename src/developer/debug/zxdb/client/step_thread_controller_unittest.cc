// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/client/step_thread_controller.h"

#include "src/developer/debug/ipc/protocol.h"
#include "src/developer/debug/zxdb/client/inline_thread_controller_test.h"
#include "src/developer/debug/zxdb/client/process.h"
#include "src/developer/debug/zxdb/client/thread.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/line_details.h"
#include "src/developer/debug/zxdb/symbols/mock_module_symbols.h"

namespace zxdb {

class StepThreadControllerTest : public InlineThreadControllerTest {
 public:
  // Shared code for the shared lib thunk tests. There are two variants of this test, one where we
  // want to skip the thunks, and one where we don't. The parameter controls which variant of the
  // test to run.
  void DoSharedLibThunkTest(bool stop_on_no_symbols);

  // Backend that runs a test for stepping into an unsymbolized function, both for when we want it
  // to stop (param = true) and continue (param = false).
  void DoUnsymbolizedFunctionTest(bool stop_on_no_symbols);

  // Does two tests that are very similar based on the flag.
  //
  // Steps with some instructions on a line, followed by an inline function.
  //
  // When |separate_line| is true, the inline function call will be on the following line. A step
  // into command should step over the first line, leaving the stack about to call into the inline
  // frame.
  //
  // When |separate_line| is false, the inline call will be on the same line being stepped and
  // a step into command should stop at the first instruction inside the inline function. This
  // will be the same physical instruction as the first case, but the stacks will be different.
  void DoIntoInlineFunctionTest(bool separate_line);
};

// Software exceptions should always stop execution. These might be from something like a hardcoded
// breakpoint instruction in the code. Doing "step" shouldn't skip over these.
TEST_F(StepThreadControllerTest, SofwareException) {
  // Step as long as we're in this range. Using the "code range" for stepping allows us to avoid
  // dependencies on the symbol subsystem.
  constexpr uint64_t kBeginAddr = 0x1000;
  constexpr uint64_t kEndAddr = 0x1010;

  // Set up the thread to be stopped at the beginning of our range.
  debug_ipc::NotifyException exception;
  exception.type = debug_ipc::ExceptionType::kSingleStep;
  exception.thread.process_koid = process()->GetKoid();
  exception.thread.thread_koid = thread()->GetKoid();
  exception.thread.state = debug_ipc::ThreadRecord::State::kBlocked;
  exception.thread.frames.emplace_back(kBeginAddr, 0x5000);
  InjectException(exception);

  // Continue the thread with the controller stepping in range.
  auto step_into =
      std::make_unique<StepThreadController>(AddressRanges(AddressRange(kBeginAddr, kEndAddr)));
  bool continued = false;
  thread()->ContinueWith(std::move(step_into), [&continued](const Err& err) {
    if (!err.has_error())
      continued = true;
  });

  // It should have been able to step without doing any further async work.
  EXPECT_TRUE(continued);
  EXPECT_EQ(1, mock_remote_api()->GetAndResetResumeCount());

  // Issue a software exception in the range.
  exception.type = debug_ipc::ExceptionType::kSoftware;
  exception.thread.frames[0].ip += 4;
  InjectException(exception);

  // It should have stayed stopped despite being in range.
  EXPECT_EQ(0, mock_remote_api()->GetAndResetResumeCount());  // Stopped
  EXPECT_EQ(debug_ipc::ThreadRecord::State::kBlocked, thread()->GetState());
}

// Some entries in the line table may have their line number set to zero. These indicate code
// generated by the compiler not associated with any line number. These should be transparently
// stepped over when stepping by line.
//
// This test tests the case where the line table has 10, 0, 10 11. Stepping from the first "10" line
// should end up on "11".
TEST_F(StepThreadControllerTest, Line0) {
  FileLine line0("/path/file.cc", 0);
  FileLine line10("/path/file.cc", 10);
  FileLine line11("/path/file.cc", 11);

  const uint64_t kAddr1 = kSymbolizedModuleAddress + 0x100;  // Line 10
  const uint64_t kAddr2 = kAddr1 + 4;                        // Line 0
  const uint64_t kAddr3 = kAddr2 + 4;                        // Line 10
  const uint64_t kAddr4 = kAddr3 + 4;                        // Line 11

  LineDetails line_details1(line10);
  line_details1.entries().push_back({21, AddressRange(kAddr1, kAddr2)});

  LineDetails line_details2(line0);
  // Column 0 indicates the "whole line".
  line_details2.entries().push_back({0, AddressRange(kAddr2, kAddr3)});

  // Same line 10 as above but starts at a different column (this time 7).
  LineDetails line_details3(line10);
  line_details3.entries().push_back({7, AddressRange(kAddr3, kAddr4)});

  LineDetails line_details4(line11);
  line_details4.entries().push_back({0, AddressRange(kAddr4, kAddr4 + 4)});

  module_symbols()->AddLineDetails(kAddr1, line_details1);
  module_symbols()->AddLineDetails(kAddr2, line_details2);
  module_symbols()->AddLineDetails(kAddr3, line_details3);
  module_symbols()->AddLineDetails(kAddr4, line_details4);

  // Set up the thread to be stopped at the beginning of our range.
  std::vector<std::unique_ptr<MockFrame>> mock_frames;
  mock_frames.push_back(GetTopFrame(kAddr1));
  mock_frames[0]->SetFileLine(line10);
  InjectExceptionWithStack(process()->GetKoid(), thread()->GetKoid(),
                           debug_ipc::ExceptionType::kSingleStep,
                           MockFrameVectorToFrameVector(std::move(mock_frames)), true);

  // Continue the thread with the controller stepping in range.
  auto step_into = std::make_unique<StepThreadController>(StepMode::kSourceLine);
  bool continued = false;
  thread()->ContinueWith(std::move(step_into), [&continued](const Err& err) {
    if (!err.has_error())
      continued = true;
  });

  // It should have been able to step without doing any further async work.
  EXPECT_TRUE(continued);
  EXPECT_EQ(1, mock_remote_api()->GetAndResetResumeCount());

  // Stop on 2nd instruction (line 0). This should be automatically resumed.
  mock_frames.push_back(GetTopFrame(kAddr2));
  mock_frames[0]->SetFileLine(line0);
  InjectExceptionWithStack(process()->GetKoid(), thread()->GetKoid(),
                           debug_ipc::ExceptionType::kSingleStep,
                           MockFrameVectorToFrameVector(std::move(mock_frames)), true);
  EXPECT_EQ(1, mock_remote_api()->GetAndResetResumeCount());

  // Stop on 3rd instruction (line 10). Since this matches the original line, it should be
  // automatically resumed.
  mock_frames.push_back(GetTopFrame(kAddr3));
  mock_frames[0]->SetFileLine(line10);
  InjectExceptionWithStack(process()->GetKoid(), thread()->GetKoid(),
                           debug_ipc::ExceptionType::kSingleStep,
                           MockFrameVectorToFrameVector(std::move(mock_frames)), true);
  EXPECT_EQ(1, mock_remote_api()->GetAndResetResumeCount());

  // Stop on 4th instruction. Since this is line 11, we should stay stopped.
  mock_frames.push_back(GetTopFrame(kAddr4));
  mock_frames[0]->SetFileLine(line11);
  InjectExceptionWithStack(process()->GetKoid(), thread()->GetKoid(),
                           debug_ipc::ExceptionType::kSingleStep,
                           MockFrameVectorToFrameVector(std::move(mock_frames)), true);
  EXPECT_EQ(0, mock_remote_api()->GetAndResetResumeCount());  // Stopped
  EXPECT_EQ(debug_ipc::ThreadRecord::State::kBlocked, thread()->GetState());
}

// Tests shared library thunks which have no symbol information in a module which otherwise has
// symbols.
//
// A cross module function call looks like
//  1. A call to an address in the same module.
//  2. That is an indirect jump to an address (the dynamic loader fills in the destination address
//      when imports are resolved). This jump has no symbol information since it's generated by the
//      linker.
//  3. Normal code in another module.
void StepThreadControllerTest::DoSharedLibThunkTest(bool stop_on_no_symbols) {
  FileLine src_line("/path/src.cc", 1);
  FileLine dest_line("/path/dest.cc", 2);

  const uint64_t kAddrSrc = kSymbolizedModuleAddress + 0x100;      // Line 1
  const uint64_t kAddrThunk = kSymbolizedModuleAddress + 0x10000;  // No symbols.
  // This is technically in the same module (normally it would be in a different one) but it doesn't
  // matter for this test and it simplifies things.
  const uint64_t kAddrDest = kSymbolizedModuleAddress + 0x200;

  const uint64_t kSrcSP = 0x5000;
  const uint64_t kThunkSP = 0x4ff8;

  LineDetails src_details(src_line);
  src_details.entries().push_back({0, AddressRange(kAddrSrc, kAddrSrc + 1)});
  module_symbols()->AddLineDetails(kAddrSrc, src_details);

  LineDetails dest_details(dest_line);
  dest_details.entries().push_back({0, AddressRange(kAddrDest, kAddrDest + 1)});
  module_symbols()->AddLineDetails(kAddrDest, dest_details);

  // Set up the thread to be stopped at the beginning of our range.
  debug_ipc::NotifyException exception;
  exception.type = debug_ipc::ExceptionType::kSingleStep;
  exception.thread.process_koid = process()->GetKoid();
  exception.thread.thread_koid = thread()->GetKoid();
  exception.thread.state = debug_ipc::ThreadRecord::State::kBlocked;
  exception.thread.frames.emplace_back(kAddrSrc, kSrcSP, 0);
  InjectException(exception);

  // Continue the thread with the controller stepping in range.
  auto step_into = std::make_unique<StepThreadController>(StepMode::kSourceLine);
  step_into->set_stop_on_no_symbols(stop_on_no_symbols);
  bool continued = false;
  thread()->ContinueWith(std::move(step_into), [&continued](const Err& err) {
    if (!err.has_error())
      continued = true;
  });

  // It should have been able to step without doing any further async work.
  EXPECT_TRUE(continued);
  EXPECT_EQ(1, mock_remote_api()->GetAndResetResumeCount());

  // Stop on the thunk instruction with no line info. This is a separate function so we push an
  // entry on the stack.
  exception.thread.frames.emplace(exception.thread.frames.begin(), kAddrThunk, kThunkSP, kSrcSP);
  InjectException(exception);
  if (stop_on_no_symbols) {
    // For this variant of the test, the unsymbolized thunk should have stopped stepping.
    EXPECT_EQ(0, mock_remote_api()->GetAndResetResumeCount());  // Stopped.
    EXPECT_EQ(debug_ipc::ThreadRecord::State::kBlocked, thread()->GetState());
    return;
  }

  // The rest of this test is the "step over unsymbolized thunks" case. It should have automatically
  // resumed from the previous exception.
  EXPECT_EQ(1, mock_remote_api()->GetAndResetResumeCount());

  // Stop on dest instruction. Since it's a different line, we should now stop.
  exception.thread.frames[0].ip = kAddrDest;
  InjectException(exception);
  EXPECT_EQ(0, mock_remote_api()->GetAndResetResumeCount());  // Stopped
  EXPECT_EQ(debug_ipc::ThreadRecord::State::kBlocked, thread()->GetState());
}

TEST_F(StepThreadControllerTest, SharedLibThunksStepOver) { DoSharedLibThunkTest(false); }

TEST_F(StepThreadControllerTest, SharedLibThunksStepInto) { DoSharedLibThunkTest(true); }

void StepThreadControllerTest::DoUnsymbolizedFunctionTest(bool stop_on_no_symbols) {
  FileLine src_line("/path/src.cc", 1);

  // Jump from src to dest and return, then to kOutOfRange.
  const uint64_t kAddrSrc = kSymbolizedModuleAddress + 0x100;
  const uint64_t kAddrDest = kUnsymbolizedModuleAddress + 0x200;
  const uint64_t kAddrReturn = kAddrSrc + 4;
  const uint64_t kAddrOutOfRange = kAddrReturn + 4;

  const uint64_t kSrcSP = 0x5000;
  const uint64_t kDestSP = 0x4ff0;

  LineDetails src_details(src_line);
  src_details.entries().push_back({0, AddressRange(kAddrSrc, kAddrOutOfRange)});
  module_symbols()->AddLineDetails(kAddrSrc, src_details);

  // Set up the thread to be stopped at the beginning of our range.
  debug_ipc::NotifyException src_exception;
  src_exception.type = debug_ipc::ExceptionType::kSingleStep;
  src_exception.thread.process_koid = process()->GetKoid();
  src_exception.thread.thread_koid = thread()->GetKoid();
  src_exception.thread.state = debug_ipc::ThreadRecord::State::kBlocked;
  src_exception.thread.frames.emplace_back(kAddrSrc, kSrcSP, 0x5008);
  src_exception.thread.frames.emplace_back(0x10, 0x5008, 0);
  InjectException(src_exception);

  // Continue the thread with the controller stepping in range.
  auto step_into = std::make_unique<StepThreadController>(StepMode::kSourceLine);
  step_into->set_stop_on_no_symbols(stop_on_no_symbols);
  bool continued = false;
  thread()->ContinueWith(std::move(step_into), [&continued](const Err& err) {
    if (!err.has_error())
      continued = true;
  });

  // It should have been able to step without doing any further async work.
  EXPECT_TRUE(continued);
  EXPECT_EQ(1, mock_remote_api()->GetAndResetResumeCount());

  // Stop on the destination unsymbolized address.
  debug_ipc::NotifyException dest_exception(src_exception);
  dest_exception.thread.frames.clear();
  dest_exception.thread.frames.emplace_back(kAddrDest, kDestSP, kSrcSP);
  dest_exception.thread.frames.emplace_back(kAddrReturn, kSrcSP, 0);
  InjectException(dest_exception);
  if (stop_on_no_symbols) {
    // For this variant of the test, the unsymbolized thunk should have stopped
    // stepping.
    EXPECT_EQ(0, mock_remote_api()->GetAndResetResumeCount());  // Stopped
    EXPECT_EQ(debug_ipc::ThreadRecord::State::kBlocked, thread()->GetState());
    return;
  }

  // The rest of this test is the "step over unsymbolized thunks" case. It should have automatically
  // resumed from the previous exception.
  EXPECT_EQ(1, mock_remote_api()->GetAndResetResumeCount());

  // Send a breakpoint completion notification at the previous stack frame. Breakpoint exceptions
  // are "software".
  src_exception.type = debug_ipc::ExceptionType::kSoftware;
  src_exception.hit_breakpoints.resize(1);
  src_exception.hit_breakpoints[0].id = mock_remote_api()->last_breakpoint_id();
  src_exception.hit_breakpoints[0].hit_count = 1;
  src_exception.thread.frames[0].ip = kAddrReturn;
  InjectException(src_exception);

  // This should have continued since the return address is still in the original address range.
  EXPECT_EQ(1, mock_remote_api()->GetAndResetResumeCount());

  // Stop on dest instruction, this is still in range so we should continue.
  src_exception.thread.frames[0].ip = kAddrOutOfRange;
  InjectException(src_exception);
  EXPECT_EQ(0, mock_remote_api()->GetAndResetResumeCount());  // Stopped
  EXPECT_EQ(debug_ipc::ThreadRecord::State::kBlocked, thread()->GetState());
}

TEST_F(StepThreadControllerTest, UnsymbolizedCallStepOver) { DoUnsymbolizedFunctionTest(false); }

TEST_F(StepThreadControllerTest, UnsymbolizedCallStepInto) { DoUnsymbolizedFunctionTest(true); }

// Tests doing a "step" when the current instruction is about to execute the first instruction of
// an inline, with the current function being the caller. This location is ambiguous so the step
// operation should go into the inline without actually executing any code.
TEST_F(StepThreadControllerTest, AmbiguousInline) {
  // Recall the top frame from GetStack() is inline.
  auto mock_frames = GetStack();

  // Stepping into the 0th frame from the first. These are the source locations.
  FileLine file_line = mock_frames[1]->GetLocation().file_line();

  InjectExceptionWithStack(process()->GetKoid(), thread()->GetKoid(),
                           debug_ipc::ExceptionType::kSingleStep,
                           MockFrameVectorToFrameVector(std::move(mock_frames)), true);

  // Hide the inline frame at the top so we're about to step into it.
  Stack& stack = thread()->GetStack();
  stack.SetHideAmbiguousInlineFrameCount(1);

  // Provide a line mapping for this address so the symbolic stepping doesn't think we're in code
  // with no symbols. This maps the first address of the inline function to its code. This is
  // specifically not the file/line above because the line table maps generated code which will be
  // the inlined function.
  module_symbols()->AddLineDetails(
      kTopInlineFunctionRange.begin(),
      LineDetails(kTopInlineFileLine, {LineDetails::LineEntry(kTopInlineFunctionRange)}));

  // Do the "step into".
  auto step_into_controller = std::make_unique<StepThreadController>(StepMode::kSourceLine);
  bool continued = false;
  thread()->ContinueWith(std::move(step_into_controller), [&continued](const Err& err) {
    if (!err.has_error())
      continued = true;
  });
  EXPECT_TRUE(continued);

  // That should have requested a synthetic exception which will be sent out asynchronously. The
  // Resume() call will cause the MockRemoteAPI to exit the message loop.
  EXPECT_EQ(0, mock_remote_api()->GetAndResetResumeCount());  // Nothing yet.
  loop().RunUntilNoTasks();

  // The operation should have unhidden the inline stack frame rather than actually affecting the
  // backend.
  EXPECT_EQ(0, mock_remote_api()->GetAndResetResumeCount());
  EXPECT_EQ(0u, stack.hide_ambiguous_inline_frame_count());

  // Now that we're at the top of the inline stack, do a subsequent "step into" which this time
  // should resume the backend.
  step_into_controller = std::make_unique<StepThreadController>(StepMode::kSourceLine);
  continued = false;
  thread()->ContinueWith(std::move(step_into_controller), [&continued](const Err& err) {
    if (!err.has_error())
      continued = true;
  });
  EXPECT_TRUE(continued);
  EXPECT_EQ(1, mock_remote_api()->GetAndResetResumeCount());
  EXPECT_EQ(0u, stack.hide_ambiguous_inline_frame_count());
}

void StepThreadControllerTest::DoIntoInlineFunctionTest(bool separate_line) {
  // Recall the top frame from GetStack() is inline. We delete that and set the instruction pointer
  // back a few so that it looks like it's stepping in the function before it gets to the inline.
  auto mock_frames = GetStack();
  mock_frames.erase(mock_frames.begin());
  uint64_t kStepBegin = mock_frames[0]->GetAddress() - 4;
  mock_frames[0]->SetAddress(kStepBegin);

  FileLine file_line = mock_frames[0]->GetLocation().file_line();

  InjectExceptionWithStack(process()->GetKoid(), thread()->GetKoid(),
                           debug_ipc::ExceptionType::kSingleStep,
                           MockFrameVectorToFrameVector(std::move(mock_frames)), true);

  // Provide a line mapping for the code we step immediately before the inline call.
  AddressRange before_inline_range(kStepBegin, kTopInlineFunctionRange.begin());
  module_symbols()->AddLineDetails(
      kStepBegin, LineDetails(file_line, {LineDetails::LineEntry(before_inline_range)}));

  // Line information for the inline function (otherwise it will try to step through the
  // unsymbolized function).
  module_symbols()->AddLineDetails(
      kTopInlineFunctionRange.begin(),
      LineDetails(kTopInlineFileLine, {LineDetails::LineEntry(kTopInlineFunctionRange)}));

  // Do the "step into".
  auto step_into_controller = std::make_unique<StepThreadController>(StepMode::kSourceLine);
  bool continued = false;
  thread()->ContinueWith(std::move(step_into_controller), [&continued](const Err& err) {
    if (!err.has_error())
      continued = true;
  });
  EXPECT_TRUE(continued);
  EXPECT_EQ(1, mock_remote_api()->GetAndResetResumeCount());  // Continued.

  // Construct a stop at the beginning of the inline function. The reported stack will include all
  // inline frames and the step controller will have to figure out what to do.
  mock_frames = GetStack();

  // Fix up the inline call location based on the type of test we're doing.
  Location loc = mock_frames[0]->GetLocation();
  Function* function = const_cast<Function*>(loc.symbol().Get()->AsFunction());
  ASSERT_TRUE(function);

  FileLine call_line;
  if (separate_line)
    call_line = FileLine(file_line.file(), file_line.line() + 1);
  else
    call_line = file_line;
  function->set_call_line(call_line);
  mock_frames[0]->set_location(
      Location(loc.address(), loc.file_line(), loc.column(), loc.symbol_context(), function));

  // The previous frame's location should match the inline call line (this will always be the
  // case for inlines -- the Stack does this computation).
  loc = mock_frames[1]->GetLocation();
  mock_frames[1]->set_location(
      Location(loc.address(), call_line, loc.column(), loc.symbol_context(), loc.symbol()));

  InjectExceptionWithStack(process()->GetKoid(), thread()->GetKoid(),
                           debug_ipc::ExceptionType::kSingleStep,
                           MockFrameVectorToFrameVector(std::move(mock_frames)), true);

  // The controller should have reported stop and left the stack at the first instruction of the
  // inline, but whether it counts as being inside the inline or not depends on whether the inline
  // was called on the line being stepped or not.
  EXPECT_EQ(0, mock_remote_api()->GetAndResetResumeCount());  // Stopped.
  Stack& stack = thread()->GetStack();
  if (separate_line)
    EXPECT_EQ(1u, stack.hide_ambiguous_inline_frame_count());
  else
    EXPECT_EQ(0u, stack.hide_ambiguous_inline_frame_count());
}

TEST_F(StepThreadControllerTest, IntoInlineSameLine) { DoIntoInlineFunctionTest(false); }

TEST_F(StepThreadControllerTest, IntoInlineNextLine) { DoIntoInlineFunctionTest(true); }

}  // namespace zxdb
