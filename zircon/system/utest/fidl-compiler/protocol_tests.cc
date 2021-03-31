// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/flat_ast.h>
#include <fidl/lexer.h>
#include <fidl/parser.h>
#include <fidl/source_file.h>
#include <zxtest/zxtest.h>

#include "error_test.h"
#include "test_library.h"

namespace {

TEST(ProtocolTests, ValidEmptyProtocol) {
  TestLibrary library(R"FIDL(
library example;

protocol Empty {};

)FIDL");
  ASSERT_COMPILED_AND_CONVERT(library);

  auto protocol = library.LookupProtocol("Empty");
  ASSERT_NOT_NULL(protocol);

  EXPECT_EQ(protocol->methods.size(), 0);
  EXPECT_EQ(protocol->all_methods.size(), 0);
}

TEST(ProtocolTests, ValidComposeMethod) {
  TestLibrary library(R"FIDL(
library example;

protocol HasComposeMethod1 {
    compose();
};

protocol HasComposeMethod2 {
    compose() -> ();
};

)FIDL");
  ASSERT_COMPILED_AND_CONVERT(library);

  auto protocol1 = library.LookupProtocol("HasComposeMethod1");
  ASSERT_NOT_NULL(protocol1);
  EXPECT_EQ(protocol1->methods.size(), 1);
  EXPECT_EQ(protocol1->all_methods.size(), 1);

  auto protocol2 = library.LookupProtocol("HasComposeMethod2");
  ASSERT_NOT_NULL(protocol2);
  EXPECT_EQ(protocol2->methods.size(), 1);
  EXPECT_EQ(protocol2->all_methods.size(), 1);
}

TEST(ProtocolTests, ValidProtocolComposition) {
  TestLibrary library(R"FIDL(
library example;

protocol A {
    MethodA();
};

protocol B {
    compose A;
    MethodB();
};

protocol C {
    compose A;
    MethodC();
};

protocol D {
    compose B;
    compose C;
    MethodD();
};

)FIDL");
  ASSERT_COMPILED_AND_CONVERT(library);

  auto protocol_a = library.LookupProtocol("A");
  ASSERT_NOT_NULL(protocol_a);
  EXPECT_EQ(protocol_a->methods.size(), 1);
  EXPECT_EQ(protocol_a->all_methods.size(), 1);

  auto protocol_b = library.LookupProtocol("B");
  ASSERT_NOT_NULL(protocol_b);
  EXPECT_EQ(protocol_b->methods.size(), 1);
  EXPECT_EQ(protocol_b->all_methods.size(), 2);

  auto protocol_c = library.LookupProtocol("C");
  ASSERT_NOT_NULL(protocol_c);
  EXPECT_EQ(protocol_c->methods.size(), 1);
  EXPECT_EQ(protocol_c->all_methods.size(), 2);

  auto protocol_d = library.LookupProtocol("D");
  ASSERT_NOT_NULL(protocol_d);
  EXPECT_EQ(protocol_d->methods.size(), 1);
  EXPECT_EQ(protocol_d->all_methods.size(), 4);
}

TEST(ProtocolTests, InvalidColonNotSupportedOld) {
  TestLibrary library(R"FIDL(
library example;

protocol Parent {};
protocol Child : Parent {};

)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrUnexpectedTokenOfKind);
}

TEST(ProtocolTests, InvalidColonNotSupported) {
  fidl::ExperimentalFlags experimental_flags;
  experimental_flags.SetFlag(fidl::ExperimentalFlags::Flag::kAllowNewSyntax);
  TestLibrary library(R"FIDL(
library example;

protocol Parent {};
protocol Child : Parent {};

)FIDL",
                      std::move(experimental_flags));
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrUnexpectedTokenOfKind);
}

TEST(ProtocolTests, InvalidDocCommentOutsideAttributelistOld) {
  TestLibrary library(R"FIDL(
library example;

protocol WellDocumented {
    Method();
    /// Misplaced doc comment
};

)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrExpectedProtocolMember);
}

TEST(ProtocolTests, InvalidDocCommentOutsideAttributelist) {
  fidl::ExperimentalFlags experimental_flags;
  experimental_flags.SetFlag(fidl::ExperimentalFlags::Flag::kAllowNewSyntax);
  TestLibrary library(R"FIDL(
library example;

protocol WellDocumented {
    Method();
    /// Misplaced doc comment
};

)FIDL",
                      std::move(experimental_flags));
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrExpectedProtocolMember);
}

TEST(ProtocolTests, invalid_cannot_attach_attributes_to_compose) {
  TestLibrary library(R"FIDL(
library example;

protocol Child {
    [NoCantDo] compose Parent;
};

)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrCannotAttachAttributesToCompose);
}

TEST(ProtocolTests, InvalidCannotComposeYourselfOld) {
  TestLibrary library(R"FIDL(
library example;

protocol Narcisse {
    compose Narcisse;
};

)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrIncludeCycle);
}

TEST(ProtocolTests, InvalidCannotComposeYourself) {
  fidl::ExperimentalFlags experimental_flags;
  experimental_flags.SetFlag(fidl::ExperimentalFlags::Flag::kAllowNewSyntax);
  TestLibrary library(R"FIDL(
library example;

protocol Narcisse {
    compose Narcisse;
};

)FIDL",
                      std::move(experimental_flags));
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrIncludeCycle);
}

TEST(ProtocolTests, InvalidCannotComposeSameProtocolTwiceOld) {
  TestLibrary library(R"FIDL(
library example;

protocol Parent {
    Method();
};

protocol Child {
    compose Parent;
    compose Parent;
};

)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrProtocolComposedMultipleTimes);
}

TEST(ProtocolTests, InvalidCannotComposeSameProtocolTwice) {
  fidl::ExperimentalFlags experimental_flags;
  experimental_flags.SetFlag(fidl::ExperimentalFlags::Flag::kAllowNewSyntax);
  TestLibrary library(R"FIDL(
library example;

protocol Parent {
    Method();
};

protocol Child {
    compose Parent;
    compose Parent;
};

)FIDL",
                      std::move(experimental_flags));
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrProtocolComposedMultipleTimes);
}

TEST(ProtocolTests, InvalidCannotComposeMissingProtocolOld) {
  TestLibrary library(R"FIDL(
library example;

protocol Child {
    compose MissingParent;
};

)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrUnknownType);
  ASSERT_SUBSTR(errors[0]->msg.c_str(), "MissingParent");
}

TEST(ProtocolTests, InvalidCannotComposeMissingProtocol) {
  fidl::ExperimentalFlags experimental_flags;
  experimental_flags.SetFlag(fidl::ExperimentalFlags::Flag::kAllowNewSyntax);
  TestLibrary library(R"FIDL(
library example;

protocol Child {
    compose MissingParent;
};

)FIDL",
                      std::move(experimental_flags));
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrUnknownType);
  ASSERT_SUBSTR(errors[0]->msg.c_str(), "MissingParent");
}

TEST(ProtocolTests, InvalidCannotComposeNonProtocolOld) {
  TestLibrary library(R"FIDL(
library example;

struct S {};
protocol P {
    compose S;
};
)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrComposingNonProtocol);
}

TEST(ProtocolTests, InvalidCannotComposeNonProtocol) {
  fidl::ExperimentalFlags experimental_flags;
  experimental_flags.SetFlag(fidl::ExperimentalFlags::Flag::kAllowNewSyntax);
  TestLibrary library(R"FIDL(
library example;

type S = struct {};
protocol P {
    compose S;
};
)FIDL",
                      std::move(experimental_flags));
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrComposingNonProtocol);
}

TEST(ProtocolTests, InvalidCannotUseOrdinalsInProtocolDeclarationOld) {
  TestLibrary library(R"FIDL(
library example;

protocol NoMoreOrdinals {
    42: NiceTry();
};

)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrExpectedProtocolMember);
}

TEST(ProtocolTests, InvalidCannotUseOrdinalsInProtocolDeclaration) {
  fidl::ExperimentalFlags experimental_flags;
  experimental_flags.SetFlag(fidl::ExperimentalFlags::Flag::kAllowNewSyntax);
  TestLibrary library(R"FIDL(
library example;

protocol NoMoreOrdinals {
    42: NiceTry();
};

)FIDL",
                      std::move(experimental_flags));
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrExpectedProtocolMember);
}

TEST(ProtocolTests, InvalidNoOtherPragmaThanComposeOld) {
  TestLibrary library(R"FIDL(
library example;

protocol Wrong {
    not_compose Something;
};

)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrUnrecognizedProtocolMember);
}

TEST(ProtocolTests, InvalidNoOtherPragmaThanCompose) {
  fidl::ExperimentalFlags experimental_flags;
  experimental_flags.SetFlag(fidl::ExperimentalFlags::Flag::kAllowNewSyntax);
  TestLibrary library(R"FIDL(
library example;

protocol Wrong {
    not_compose Something;
};

)FIDL",
                      std::move(experimental_flags));
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrUnrecognizedProtocolMember);
}

TEST(ProtocolTests, InvalidComposedProtocolsHaveClashingNamesOld) {
  TestLibrary library(R"FIDL(
library example;

protocol A {
    MethodA();
};

protocol B {
    compose A;
    MethodB();
};

protocol C {
    compose A;
    MethodC();
};

protocol D {
    compose B;
    compose C;
    MethodD();
    MethodA();
};
)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrDuplicateMethodName);
}

TEST(ProtocolTests, InvalidComposedProtocolsHaveClashingNames) {
  fidl::ExperimentalFlags experimental_flags;
  experimental_flags.SetFlag(fidl::ExperimentalFlags::Flag::kAllowNewSyntax);
  TestLibrary library(R"FIDL(
library example;

protocol A {
    MethodA();
};

protocol B {
    compose A;
    MethodB();
};

protocol C {
    compose A;
    MethodC();
};

protocol D {
    compose B;
    compose C;
    MethodD();
    MethodA();
};
)FIDL",
                      std::move(experimental_flags));
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrDuplicateMethodName);
}

// See GetGeneratedOrdinal64ForTesting in test_library.h
TEST(ProtocolTests, InvalidComposedProtocolsHaveClashingOrdinalsOld) {
  TestLibrary library(R"FIDL(
library methodhasher;

protocol SpecialComposed {
   ClashOne();
};

protocol Special {
    compose SpecialComposed;
    ClashTwo();
};
)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrDuplicateMethodOrdinal);
  ASSERT_SUBSTR(errors[0]->msg.c_str(), "ClashTwo_");
}

// See GetGeneratedOrdinal64ForTesting in test_library.h
TEST(ProtocolTests, InvalidComposedProtocolsHaveClashingOrdinals) {
  fidl::ExperimentalFlags experimental_flags;
  experimental_flags.SetFlag(fidl::ExperimentalFlags::Flag::kAllowNewSyntax);
  TestLibrary library(R"FIDL(
library methodhasher;

protocol SpecialComposed {
   ClashOne();
};

protocol Special {
    compose SpecialComposed;
    ClashTwo();
};
)FIDL",
                      std::move(experimental_flags));
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrDuplicateMethodOrdinal);
  ASSERT_SUBSTR(errors[0]->msg.c_str(), "ClashTwo_");
}

// TODO(fxbug.dev/68792), TODO(fxbug.dev/72924): support attributes in the new syntax
TEST(ProtocolTests, InvalidSimpleConstraintAppliesToComposedMethodsTooOld) {
  TestLibrary library(R"FIDL(
library example;

protocol NotSimple {
    Complex(vector<uint64> arg);
};

[ForDeprecatedCBindings]
protocol YearningForSimplicity {
    compose NotSimple;
    Simple();
};
)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrMemberMustBeSimple);
  ASSERT_SUBSTR(errors[0]->msg.c_str(), "arg");
}

// TODO(fxbug.dev/71536): implement client/server end in the new syntax
TEST(ProtocolTests, InvalidRequestMustBeProtocolOld) {
  TestLibrary library(R"FIDL(
library example;

struct S {};
protocol P {
    Method(request<S> r);
};
)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrMustBeAProtocol);
}

// TODO(fxbug.dev/71536): implement client/server end in the new syntax
TEST(ProtocolTests, InvalidRequestMustBeParameterized) {
  TestLibrary library(R"FIDL(
library example;

protocol P {
    Method(request r);
};
)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrMustBeParameterized);
}

// TODO(fxbug.dev/71536): implement client/server end in the new syntax
TEST(ProtocolTests, InvalidRequestCannotHaveSize) {
  TestLibrary library(R"FIDL(
library example;

protocol P {};
struct S {
    request<P>:0 p;
};
)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrCannotHaveSize);
}

TEST(ProtocolTests, InvalidDuplicateParameterNameOld) {
  TestLibrary library(R"FIDL(
library example;

protocol P {
  MethodWithDuplicateParams(uint8 foo, uint8 foo);
};
)FIDL");
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrDuplicateMethodParameterName);
}

TEST(ProtocolTests, InvalidDuplicateParameterName) {
  fidl::ExperimentalFlags experimental_flags;
  experimental_flags.SetFlag(fidl::ExperimentalFlags::Flag::kAllowNewSyntax);
  TestLibrary library(R"FIDL(
library example;

protocol P {
  MethodWithDuplicateParams(foo uint8, foo uint8);
};
)FIDL",
                      std::move(experimental_flags));
  ASSERT_FALSE(library.Compile());
  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_ERR(errors[0], fidl::ErrDuplicateMethodParameterName);
}

}  // namespace
