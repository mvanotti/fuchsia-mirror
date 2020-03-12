// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package pkgfs

import (
	"fmt"
	"log"
	"path/filepath"
	"strings"
	"thinfs/fs"
	"time"

	"fuchsia.googlesource.com/pmd/allowlist"
)

type packagesRoot struct {
	unsupportedDirectory

	fs                        *Filesystem
	allowedNonStaticPackages  *allowlist.Allowlist
	enforceNonStaticAllowlist bool
}

func (pr *packagesRoot) isAllowedNonStaticPackage(packageName string) bool {
	if pr.allowedNonStaticPackages == nil {
		return false
	}

	return pr.allowedNonStaticPackages.Contains(packageName)
}

func (pr *packagesRoot) loadAllowedNonStaticPackages(blob string) (*allowlist.Allowlist, error) {
	allowListFile, err := pr.fs.blobfs.Open(blob)
	if err != nil {
		return nil, fmt.Errorf("could not load non_static_pkgs allowlist from blob %s: %v", blob, err)
	}
	defer allowListFile.Close()

	return allowlist.LoadFrom(allowListFile)
}

func (pr *packagesRoot) setAllowedNonStaticPackages(blob string) error {
	allowedNonStaticPackages, err := pr.loadAllowedNonStaticPackages(blob)
	if err != nil {
		log.Printf("pkgfs: couldn't load allowed non-static packages list: %v", err)
		return err
	}

	pr.allowedNonStaticPackages = allowedNonStaticPackages

	log.Printf("pkgfs: parsed non-static pkgs allowlist: %v", allowedNonStaticPackages)

	return nil
}

func (pr *packagesRoot) Dup() (fs.Directory, error) {
	return pr, nil
}

func (pr *packagesRoot) Close() error { return nil }

func (pr *packagesRoot) Open(name string, flags fs.OpenFlags) (fs.File, fs.Directory, *fs.Remote, error) {
	name = clean(name)

	if name == "" {
		return nil, pr, nil, nil
	}

	parts := strings.Split(name, "/")

	packageName := parts[0]

	if (pr.fs.static == nil || (pr.fs.static != nil && !pr.fs.static.HasName(packageName))) && !pr.isAllowedNonStaticPackage(packageName) {
		log.Printf("pkgfs: attempted open of non-static package %s, see go/pkgfs-base-packages. Full path: %s", packageName, name)

		if pr.enforceNonStaticAllowlist {
			return nil, nil, nil, fs.ErrNotFound
		}
	}

	pld, err := newPackageListDir(packageName, pr.fs)
	if err != nil {
		// If the package isn't on the system at all, we'll return an error here.
		return nil, nil, nil, err
	}

	if len(parts) > 1 {
		// We were asked for a specific variant, so return a package directory itself, not just the list of variants
		file, directory, remote, err := pld.Open(filepath.Join(parts[1:]...), flags)
		return file, directory, remote, err
	}

	// Return the list of variants
	return nil, pld, nil, nil
}

func (pr *packagesRoot) Read() ([]fs.Dirent, error) {
	var names = map[string]struct{}{}
	if pr.fs.static != nil {
		pkgs, err := pr.fs.static.List()
		if err != nil {
			return nil, err
		}
		for _, p := range pkgs {
			names[p.Name] = struct{}{}
		}
	}

	pkgs := pr.fs.index.List()
	for _, p := range pkgs {
		if pr.enforceNonStaticAllowlist && !pr.isAllowedNonStaticPackage(p.Name) {
			continue
		}
		names[p.Name] = struct{}{}
	}

	dirents := make([]fs.Dirent, 0, len(names))
	for name := range names {
		dirents = append(dirents, dirDirEnt(name))
	}
	return dirents, nil
}

func (pr *packagesRoot) Stat() (int64, time.Time, time.Time, error) {
	// TODO(raggi): stat the index directory and pass on info
	return 0, pr.fs.mountTime, pr.fs.mountTime, nil
}

// packageListDir is a directory in the pkgfs packages directory for an
// individual package that lists all versions of packages
type packageListDir struct {
	unsupportedDirectory
	fs          *Filesystem
	packageName string
}

func newPackageListDir(name string, f *Filesystem) (*packageListDir, error) {
	if !f.static.HasName(name) {

		pkgs := f.index.List()
		found := false
		for _, p := range pkgs {
			if p.Name == name {
				found = true
				break
			}
		}
		if !found {
			return nil, fs.ErrNotFound
		}
	}

	pld := packageListDir{
		unsupportedDirectory: unsupportedDirectory(filepath.Join("/packages", name)),
		fs:                   f,
		packageName:          name,
	}
	return &pld, nil
}

func (pld *packageListDir) Dup() (fs.Directory, error) {
	return pld, nil
}

func (pld *packageListDir) Close() error {
	return nil
}

func (pld *packageListDir) Open(name string, flags fs.OpenFlags) (fs.File, fs.Directory, *fs.Remote, error) {
	name = clean(name)

	if name == "" {
		return nil, pld, nil, nil
	}

	parts := strings.Split(name, "/")

	d, err := newPackageDir(pld.packageName, parts[0], pld.fs)
	if err != nil {
		return nil, nil, nil, err
	}

	if len(parts) > 1 {
		return d.Open(filepath.Join(parts[1:]...), flags)
	}
	return nil, d, nil, nil
}

func (pld *packageListDir) Read() ([]fs.Dirent, error) {
	if pld.fs.static != nil && pld.fs.static.HasName(pld.packageName) {
		versions := pld.fs.static.ListVersions(pld.packageName)
		dirents := make([]fs.Dirent, len(versions))
		for i := range versions {
			dirents[i] = dirDirEnt(versions[i])
		}
		return dirents, nil
	}

	var dirents []fs.Dirent
	pkgs := pld.fs.index.List()
	for _, p := range pkgs {
		if p.Name == pld.packageName {
			dirents = append(dirents, dirDirEnt(p.Version))
		}
	}

	return dirents, nil
}

func (pld *packageListDir) Stat() (int64, time.Time, time.Time, error) {
	// TODO(raggi): stat the index directory and pass on info
	return 0, pld.fs.mountTime, pld.fs.mountTime, nil
}
