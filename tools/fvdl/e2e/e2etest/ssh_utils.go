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

// CreateSSHKeyPairFiles creates a private-public and authorized_key files. The files are written to the following locations:
//  {base}/.fx-ssh-path
//  {base}/.ssh/fuchsia_ed25519
//  {base}/.ssh/fuchsia_ed25519.pub
//  {base}/.ssh/fuchsia_authorized_keys
//
// This simulates in-tree ssh setup.
func CreateSSHKeyPairFiles(t *testing.T, base string) {
	sshBase := filepath.Join(base, ".ssh")
	if err := os.Mkdir(sshBase, 0o755); err != nil && !os.IsExist(err) {
		t.Fatal(err)
	}
	// Create private key
	privateKey := filepath.Join(sshBase, "fuchsia_ed25519")
	if err := removeIfExists(privateKey); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command("ssh-keygen", "-N", "",
		"-t", "ed25519",
		"-f", privateKey,
		"-C", "Generated by Fuchsia automated testing")
	if err := cmd.Run(); err != nil {
		t.Fatalf("ssh-keygen for private_key failed: %s", err)
	}

	// Create public key
	cmd = exec.Command("ssh-keygen", "-y", "-f", privateKey)
	output, err := cmd.Output()
	if err != nil {
		t.Fatalf("ssh-keygen for public key failed: %s", err)
	}

	// Create fuchsia_authorized_keys
	authKey := filepath.Join(sshBase, "fuchsia_authorized_keys")
	if err := removeIfExists(authKey); err != nil {
		t.Fatal(err)
	}
	if err := ioutil.WriteFile(authKey, []byte(output), 0o644); err != nil {
		t.Fatal(err)
	}

	// Create .fx-ssh-path file
	data := []byte(fmt.Sprintf("%s\n%s\n", privateKey, authKey))
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
