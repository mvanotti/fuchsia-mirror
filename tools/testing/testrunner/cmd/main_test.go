// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/google/go-cmp/cmp"
	"github.com/google/go-cmp/cmp/cmpopts"

	"go.fuchsia.dev/fuchsia/tools/build"
	"go.fuchsia.dev/fuchsia/tools/integration/testsharder"
	"go.fuchsia.dev/fuchsia/tools/lib/clock"
	"go.fuchsia.dev/fuchsia/tools/testing/runtests"
	"go.fuchsia.dev/fuchsia/tools/testing/tap"
	"go.fuchsia.dev/fuchsia/tools/testing/testrunner"
)

const (
	testFunc        = "Test"
	copySinksFunc   = "EnsureSinks"
	runSnapshotFunc = "RunSnapshot"
)

// When returned by Test(), errFatal should cause testrunner to stop running any
// more tests.
var errFatal = fatalError{errors.New("fatal error occurred")}

type fakeTester struct {
	testErr   error
	runTest   func(testsharder.Test, io.Writer, io.Writer)
	funcCalls []string
	outDirs   map[string]bool
}

func (t *fakeTester) Test(_ context.Context, test testsharder.Test, stdout, stderr io.Writer, outDir string) (runtests.DataSinkReference, error) {
	t.funcCalls = append(t.funcCalls, testFunc)
	if t.outDirs == nil {
		t.outDirs = make(map[string]bool)
	}
	t.outDirs[outDir] = true
	if t.runTest != nil {
		t.runTest(test, stdout, stderr)
	}
	return runtests.DataSinkReference{}, t.testErr
}

func (t *fakeTester) Close() error {
	return nil
}

func (t *fakeTester) EnsureSinks(_ context.Context, _ []runtests.DataSinkReference, _ *testOutputs) error {
	t.funcCalls = append(t.funcCalls, copySinksFunc)
	return nil
}

func (t *fakeTester) RunSnapshot(_ context.Context, _ string) error {
	t.funcCalls = append(t.funcCalls, runSnapshotFunc)
	return nil
}

func TestValidateTest(t *testing.T) {
	cases := []struct {
		name      string
		test      testsharder.Test
		expectErr bool
	}{
		{
			name: "missing name",
			test: testsharder.Test{
				Test: build.Test{
					OS:   "linux",
					Path: "/foo/bar",
				},
				Runs: 1,
			},
			expectErr: true,
		},
		{
			name: "missing OS",
			test: testsharder.Test{
				Test: build.Test{
					Name: "test1",
					Path: "/foo/bar",
				},
				Runs: 1,
			},
			expectErr: true,
		},
		{
			name: "spurious package URL",
			test: testsharder.Test{
				Test: build.Test{
					Name:       "test1",
					OS:         "linux",
					PackageURL: "fuchsia-pkg://test1",
				},
				Runs: 1,
			},
			expectErr: true,
		},
		{
			name: "missing required path",
			test: testsharder.Test{
				Test: build.Test{
					Name: "test1",
					OS:   "linux",
				},
				Runs: 1,
			},
			expectErr: true,
		},
		{
			name: "missing required package_url or path",
			test: testsharder.Test{
				Test: build.Test{
					Name: "test1",
					OS:   "fuchsia",
				},
				Runs: 1,
			},
			expectErr: true,
		},
		{
			name: "missing runs",
			test: testsharder.Test{
				Test: build.Test{
					Name: "test1",
					OS:   "linux",
					Path: "/foo/bar",
				},
			},
			expectErr: true,
		},
		{
			name: "missing run algorithm",
			test: testsharder.Test{
				Test: build.Test{
					Name: "test1",
					OS:   "linux",
					Path: "/foo/bar",
				},
				Runs: 2,
			},
			expectErr: true,
		},
		{
			name: "valid test with path",
			test: testsharder.Test{
				Test: build.Test{
					Name: "test1",
					OS:   "linux",
					Path: "/foo/bar",
				},
				Runs: 1,
			},
			expectErr: false,
		},
		{
			name: "valid test with packageurl",
			test: testsharder.Test{
				Test: build.Test{
					Name:       "test1",
					OS:         "fuchsia",
					PackageURL: "fuchsia-pkg://test1",
				},
				Runs:         5,
				RunAlgorithm: testsharder.KeepGoing,
			},
			expectErr: false,
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			err := validateTest(c.test)
			if c.expectErr != (err != nil) {
				t.Errorf("got error: %q, expectErr: %t", err, c.expectErr)
			}
		})
	}
}

func TestRunAndOutputTest(t *testing.T) {
	cases := []struct {
		name           string
		test           build.Test
		runs           int
		runAlgorithm   testsharder.RunAlgorithm
		testErr        error
		runTestFunc    func(testsharder.Test, io.Writer, io.Writer)
		expectedErr    error
		expectedResult []*testrunner.TestResult
	}{
		{
			name: "host test pass",
			test: build.Test{
				Name: "bar",
				Path: "/foo/bar",
				OS:   "linux",
			},
			expectedResult: []*testrunner.TestResult{{
				Name:   "bar",
				Result: runtests.TestSuccess,
			}},
		},
		{
			name: "fuchsia test pass",
			test: build.Test{
				Name:       "bar",
				Path:       "/foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			expectedResult: []*testrunner.TestResult{{
				Name:   "bar",
				Result: runtests.TestSuccess,
			}},
		},
		{
			name: "fuchsia test fail",
			test: build.Test{
				Name:       "bar",
				Path:       "/foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			testErr: fmt.Errorf("test failed"),
			expectedResult: []*testrunner.TestResult{{
				Name:   "bar",
				Result: runtests.TestFailure,
			}},
		},
		{
			name: "fatal error",
			test: build.Test{
				Name:       "bar",
				Path:       "/foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			testErr:     errFatal,
			expectedErr: errFatal,
		},
		{
			name: "multiplier test gets unique index",
			test: build.Test{
				Name:       "bar (2)",
				Path:       "/foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			runs:         2,
			runAlgorithm: testsharder.KeepGoing,
			expectedResult: []*testrunner.TestResult{{
				Name:   "bar (2)",
				Result: runtests.TestSuccess,
			}, {
				Name:     "bar (2)",
				Result:   runtests.TestSuccess,
				RunIndex: 1,
			}},
		},
		{
			name: "combines stdio and stdout in chronological order",
			test: build.Test{
				Name:       "fuchsia-pkg://foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			expectedResult: []*testrunner.TestResult{{
				Name:   "fuchsia-pkg://foo/bar",
				Result: runtests.TestSuccess,
				Stdio:  []byte("stdout stderr stdout"),
			}},
			runTestFunc: func(t testsharder.Test, stdout, stderr io.Writer) {
				stdout.Write([]byte("stdout "))
				stderr.Write([]byte("stderr "))
				stdout.Write([]byte("stdout"))
			},
		},
		{
			name: "retries test",
			test: build.Test{
				Name:       "fuchsia-pkg://foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			runs:         6,
			runAlgorithm: testsharder.StopOnSuccess,
			testErr:      fmt.Errorf("test failed"),
			expectedResult: []*testrunner.TestResult{
				{
					Name:   "fuchsia-pkg://foo/bar",
					Result: runtests.TestFailure,
					Stdio:  []byte("stdio"),
				},
				{
					Name:     "fuchsia-pkg://foo/bar",
					Result:   runtests.TestFailure,
					RunIndex: 1,
					Stdio:    []byte("stdio"),
				},
				{
					Name:     "fuchsia-pkg://foo/bar",
					Result:   runtests.TestFailure,
					RunIndex: 2,
					Stdio:    []byte("stdio"),
				},
				{
					Name:     "fuchsia-pkg://foo/bar",
					Result:   runtests.TestFailure,
					RunIndex: 3,
					Stdio:    []byte("stdio"),
				},
				{
					Name:     "fuchsia-pkg://foo/bar",
					Result:   runtests.TestFailure,
					RunIndex: 4,
					Stdio:    []byte("stdio"),
				},
				{
					Name:     "fuchsia-pkg://foo/bar",
					Result:   runtests.TestFailure,
					RunIndex: 5,
					Stdio:    []byte("stdio"),
				},
			},
			runTestFunc: func(t testsharder.Test, stdout, stderr io.Writer) {
				stdout.Write([]byte("stdio"))
			},
		},
		{
			name: "retries test after timeout",
			test: build.Test{
				Name:       "bar",
				Path:       "/foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			runAlgorithm: testsharder.StopOnSuccess,
			runs:         2,
			testErr:      &timeoutError{timeout: time.Minute},
			expectedResult: []*testrunner.TestResult{
				{
					Name:     "bar",
					Result:   runtests.TestFailure,
					RunIndex: 0,
				},
				{
					Name:     "bar",
					Result:   runtests.TestFailure,
					RunIndex: 1,
				},
			},
		},
		{
			name: "returns on first success even if max attempts > 1",
			test: build.Test{
				Name:       "fuchsia-pkg://foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			runs:         5,
			runAlgorithm: testsharder.StopOnSuccess,
			expectedResult: []*testrunner.TestResult{{
				Name:   "fuchsia-pkg://foo/bar",
				Result: runtests.TestSuccess,
			}},
		},
		{
			name: "returns on first failure even if max attempts > 1",
			test: build.Test{
				Name:       "fuchsia-pkg://foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			runs:         5,
			runAlgorithm: testsharder.StopOnFailure,
			testErr:      fmt.Errorf("test failed"),
			expectedResult: []*testrunner.TestResult{{
				Name:   "fuchsia-pkg://foo/bar",
				Result: runtests.TestFailure,
			}},
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			tester := &fakeTester{
				testErr: c.testErr,
				runTest: c.runTestFunc,
			}
			var buf bytes.Buffer
			producer := tap.NewProducer(&buf)
			o, err := createTestOutputs(producer, "")
			if err != nil {
				t.Fatalf("failed to create a test outputs object: %s", err)
			}
			defer o.Close()
			if c.runs == 0 {
				c.runs = 1
			}
			results, err := runAndOutputTest(context.Background(), testsharder.Test{Test: c.test, Runs: c.runs, RunAlgorithm: c.runAlgorithm}, tester, o, &buf, &buf, "out-dir")

			if err != c.expectedErr {
				t.Fatalf("got error: %q, expected: %q", err, c.expectedErr)
			}

			opts := []cmp.Option{
				cmpopts.IgnoreFields(testrunner.TestResult{}, "StartTime", "EndTime"),
				cmpopts.EquateEmpty(),
			}
			if diff := cmp.Diff(results, c.expectedResult, opts...); diff != "" {
				t.Errorf("test results mismatch (-want +got):\n%s", diff)
			}

			if c.runs > 1 {
				funcCalls := strings.Join(tester.funcCalls, ",")
				testCount := strings.Count(funcCalls, testFunc)
				expectedTries := 1
				if (c.runAlgorithm == testsharder.StopOnSuccess && c.testErr != nil) || c.runAlgorithm == testsharder.KeepGoing {
					expectedTries = c.runs
				}
				if testCount != expectedTries {
					t.Errorf("ran test %d times, expected: %d", testCount, expectedTries)
				}
				// Each try should have a unique outDir
				if len(tester.outDirs) != expectedTries {
					t.Errorf("got %d unique outDirs, expected %d", len(tester.outDirs), expectedTries)
				}
			}
			expectedOutput := ""
			for i, result := range c.expectedResult {
				statusString := "ok"
				if result.Result != runtests.TestSuccess {
					statusString = "not ok"
				}
				expectedOutput += fmt.Sprintf("%s%s %d %s (.*)\n", result.Stdio, statusString, i+1, result.Name)
			}
			actualOutput := buf.String()
			expectedOutputRegex := regexp.MustCompile(strings.ReplaceAll(strings.ReplaceAll(expectedOutput, "(", "\\("), ")", "\\)"))
			submatches := expectedOutputRegex.FindStringSubmatch(actualOutput)
			if len(submatches) != 1 {
				t.Errorf("unexpected output:\nexpected: %q\nactual: %q\n", expectedOutput, actualOutput)
			}
		})
	}

	// Tests that `runAndOutputTest` doesn't return an error if running in
	// StopOnFailure mode with a deadline, as would be the case with
	// `StopRepeatingAfterSecs`.
	t.Run("multiplied shard hitting time limit", func(t *testing.T) {
		test := testsharder.Test{
			Test: build.Test{
				Name:       "fuchsia-pkg://foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			RunAlgorithm:           testsharder.StopOnFailure,
			Runs:                   100,
			StopRepeatingAfterSecs: 5,
		}

		o, err := createTestOutputs(tap.NewProducer(io.Discard), "")
		if err != nil {
			t.Fatal(err)
		}
		defer o.Close()

		fakeClock := clock.NewFakeClock()
		ctx := clock.NewContext(context.Background(), fakeClock)

		// If each test run takes 1 second, we should be able to run the test
		// exactly `StopRepeatingAfterSecs` times.
		tester := fakeTester{
			runTest: func(t testsharder.Test, w1, w2 io.Writer) {
				fakeClock.Advance(time.Second)
			},
		}

		results, err := runAndOutputTest(ctx, test, &tester, o, io.Discard, io.Discard, t.TempDir())
		if err != nil {
			t.Fatal(err)
		}
		if len(results) != test.StopRepeatingAfterSecs {
			t.Fatalf("Expected %d test results but got %d", test.StopRepeatingAfterSecs, len(results))
		}
	})
}
