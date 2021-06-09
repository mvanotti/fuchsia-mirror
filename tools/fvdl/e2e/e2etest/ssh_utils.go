// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package e2etest

import (
	"fmt"
	"io/ioutil"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// CreateSSHKeyPairFiles creates a private-public key files and write their location to
// base/.fx-ssh-path. This simulates in-tree ssh setup.
func CreateSSHKeyPairFiles(t *testing.T, base string) {
	// Create private key
	private_key := filepath.Join(base, "fuchsia_ed25519")
	if err := removeIfExists(private_key); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command("ssh-keygen", "-N", "",
		"-t", "ed25519",
		"-f", private_key,
		"-C", "Generated by Fuchsia automated testing")
	if err := cmd.Run(); err != nil {
		t.Fatalf("ssh-keygen for private_key failed: %s", err)
	}

	// Create public key
	cmd = exec.Command("ssh-keygen", "-y", "-f", private_key)
	output, err := cmd.Output()
	if err != nil {
		t.Fatalf("ssh-keygen for auth_key failed: %s", err)
	}
	auth_key := filepath.Join(base, "fuchsia_authorized_keys")
	if err := removeIfExists(auth_key); err != nil {
		t.Fatal(err)
	}
	if err := ioutil.WriteFile(auth_key, []byte(output), 0o644); err != nil {
		t.Fatal(err)
	}

	// Create .fx-ssh-path file
	data := []byte(fmt.Sprintf("%s\n%s\n", private_key, auth_key))
	if err := ioutil.WriteFile(filepath.Join(base, ".fx-ssh-path"), data, 0o644); err != nil {
		t.Fatal(err)
	}
	t.Logf("[test info] wrote file %s\n with content\n%s\n", filepath.Join(base, ".fx-ssh-path"), data)
}

func removeIfExists(f string) error {
	if err := os.Remove(f); err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}