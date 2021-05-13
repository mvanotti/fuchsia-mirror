// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        errors::FxfsError,
        lsm_tree::{
            merge::{Merger, MergerIterator},
            types::{ItemRef, LayerIterator},
        },
        object_handle::{ObjectHandle, ObjectProperties},
        object_store::{
            current_time,
            record::{
                ObjectAttributes, ObjectItem, ObjectKey, ObjectKeyData, ObjectKind, ObjectValue,
                Timestamp,
            },
            transaction::{Mutation, Transaction},
            HandleOptions, ObjectStore, ObjectStoreMutation, StoreObjectHandle,
        },
    },
    anyhow::{anyhow, bail, Error},
    std::{fmt, ops::Bound, sync::Arc},
};

// ObjectDescriptor is exposed in Directory::lookup.
pub use crate::object_store::record::ObjectDescriptor;

/// A directory stores name to child object mappings.
pub struct Directory<S> {
    owner: Arc<S>,
    object_id: u64,
}

impl<S: AsRef<ObjectStore> + Send + Sync + 'static> Directory<S> {
    pub fn new(owner: Arc<S>, object_id: u64) -> Self {
        Directory { owner, object_id }
    }

    pub fn object_id(&self) -> u64 {
        return self.object_id;
    }

    pub fn owner(&self) -> &Arc<S> {
        &self.owner
    }

    pub fn store(&self) -> &ObjectStore {
        self.owner.as_ref().as_ref()
    }

    pub async fn create(
        transaction: &mut Transaction<'_>,
        owner: &Arc<S>,
    ) -> Result<Directory<S>, Error> {
        let store = owner.as_ref().as_ref();
        store.ensure_open().await?;
        let object_id = store.get_next_object_id();
        let now = current_time();
        transaction.add(
            store.store_object_id,
            Mutation::insert_object(
                ObjectKey::object(object_id),
                ObjectValue::Object {
                    kind: ObjectKind::Directory,
                    attributes: ObjectAttributes {
                        creation_time: now.clone(),
                        modification_time: now,
                    },
                },
            ),
        );
        Ok(Directory::new(owner.clone(), object_id))
    }

    pub async fn open(owner: &Arc<S>, object_id: u64) -> Result<Directory<S>, Error> {
        let store = owner.as_ref().as_ref();
        store.ensure_open().await?;
        if let ObjectItem {
            value: ObjectValue::Object { kind: ObjectKind::Directory, .. }, ..
        } = store.tree.find(&ObjectKey::object(object_id)).await?.ok_or(FxfsError::NotFound)?
        {
            Ok(Directory::new(owner.clone(), object_id))
        } else {
            bail!(FxfsError::NotDir);
        }
    }

    pub async fn has_children(&self) -> Result<bool, Error> {
        let layer_set = self.store().tree().layer_set();
        let mut merger = layer_set.merger();
        Ok(self.iter(&mut merger).await?.get().is_some())
    }

    /// Returns the object ID and descriptor for the given child, or None if not found.
    pub async fn lookup(&self, name: &str) -> Result<Option<(u64, ObjectDescriptor)>, Error> {
        match self.store().tree().find(&ObjectKey::child(self.object_id, name)).await? {
            None | Some(ObjectItem { value: ObjectValue::None, .. }) => Ok(None),
            Some(ObjectItem {
                value: ObjectValue::Child { object_id, object_descriptor }, ..
            }) => Ok(Some((object_id, object_descriptor))),
            Some(item) => Err(anyhow!(FxfsError::Inconsistent)
                .context(format!("Unexpected item in lookup: {:?}", item))),
        }
    }

    pub async fn create_child_dir<'a>(
        &self,
        transaction: &mut Transaction<'a>,
        name: &str,
    ) -> Result<Directory<S>, Error> {
        let handle = Directory::create(transaction, &self.owner).await?;
        transaction.add(
            self.store().store_object_id(),
            Mutation::replace_or_insert_object(
                ObjectKey::child(self.object_id, name),
                ObjectValue::child(handle.object_id(), ObjectDescriptor::Directory),
            ),
        );
        self.update_timestamps(transaction, None, Some(current_time())).await?;
        Ok(handle)
    }

    pub async fn create_child_file<'a>(
        &self,
        transaction: &mut Transaction<'a>,
        name: &str,
    ) -> Result<StoreObjectHandle<S>, Error> {
        let handle =
            ObjectStore::create_object(&self.owner, transaction, HandleOptions::default()).await?;
        transaction.add(
            self.store().store_object_id(),
            Mutation::replace_or_insert_object(
                ObjectKey::child(self.object_id, name),
                ObjectValue::child(handle.object_id(), ObjectDescriptor::File),
            ),
        );
        self.update_timestamps(transaction, None, Some(current_time())).await?;
        Ok(handle)
    }

    pub async fn add_child_volume<'a>(
        &self,
        transaction: &mut Transaction<'a>,
        volume_name: &str,
        store_object_id: u64,
    ) -> Result<(), Error> {
        transaction.add(
            self.store().store_object_id(),
            Mutation::replace_or_insert_object(
                ObjectKey::child(self.object_id, volume_name),
                ObjectValue::child(store_object_id, ObjectDescriptor::Volume),
            ),
        );
        self.update_timestamps(transaction, None, Some(current_time())).await
    }

    /// Inserts a child into the directory.
    ///
    /// Requires transaction locks on |self|.
    pub async fn insert_child<'a>(
        &self,
        transaction: &mut Transaction<'a>,
        name: &str,
        object_id: u64,
        descriptor: ObjectDescriptor,
    ) -> Result<(), Error> {
        transaction.add(
            self.store().store_object_id(),
            Mutation::replace_or_insert_object(
                ObjectKey::child(self.object_id, name),
                ObjectValue::child(object_id, descriptor),
            ),
        );
        self.update_timestamps(transaction, None, Some(current_time())).await
    }

    /// Updates the timestamps for the object.  If either argument is None, that timestamp is not
    /// modified.
    pub async fn update_timestamps<'a>(
        &self,
        transaction: &mut Transaction<'a>,
        crtime: Option<Timestamp>,
        mtime: Option<Timestamp>,
    ) -> Result<(), Error> {
        if let (None, None) = (crtime.as_ref(), mtime.as_ref()) {
            return Ok(());
        }
        let mut item = if let Some(ObjectStoreMutation { item, .. }) = transaction
            .get_object_mutation(self.store().store_object_id(), ObjectKey::object(self.object_id))
        {
            item.clone()
        } else {
            self.store()
                .tree()
                .find(&ObjectKey::object(self.object_id))
                .await?
                .ok_or(anyhow!(FxfsError::NotFound))?
        };
        if let ObjectValue::Object { ref mut attributes, .. } = item.value {
            if let Some(time) = crtime {
                attributes.creation_time = time;
            }
            if let Some(time) = mtime {
                attributes.modification_time = time;
            }
        } else {
            bail!(FxfsError::Inconsistent);
        };
        transaction.add(
            self.store().store_object_id(),
            Mutation::replace_or_insert_object(item.key, item.value),
        );
        Ok(())
    }

    pub async fn get_properties(&self) -> Result<ObjectProperties, Error> {
        let item = self
            .store()
            .tree()
            .find(&ObjectKey::object(self.object_id))
            .await?
            .ok_or(anyhow!(FxfsError::NotFound))?;
        match item.value {
            ObjectValue::Object {
                kind: ObjectKind::Directory,
                attributes: ObjectAttributes { creation_time, modification_time },
            } => Ok(ObjectProperties {
                refs: 0,
                allocated_size: 0,
                data_attribute_size: 0,
                creation_time,
                modification_time,
            }),
            _ => bail!(FxfsError::Inconsistent),
        }
    }

    /// Returns an iterator that will return directory entries skipping deleted ones.  Example
    /// usage:
    ///
    ///   let layer_set = dir.store().tree().layer_set();
    ///   let mut merger = layer_set.merger();
    ///   let mut iter = dir.iter(&mut merger).await?;
    ///
    pub async fn iter<'a, 'b>(
        &self,
        merger: &'a mut Merger<'b, ObjectKey, ObjectValue>,
    ) -> Result<DirectoryIterator<'a, 'b>, Error> {
        self.iter_from(merger, "").await
    }

    /// Like "iter", but seeks from a specific filename (inclusive).  Example usage:
    ///
    ///   let layer_set = dir.store().tree().layer_set();
    ///   let mut merger = layer_set.merger();
    ///   let mut iter = dir.iter_from(&mut merger, "foo").await?;
    ///
    pub async fn iter_from<'a, 'b>(
        &self,
        merger: &'a mut Merger<'b, ObjectKey, ObjectValue>,
        from: &str,
    ) -> Result<DirectoryIterator<'a, 'b>, Error> {
        let mut iter =
            merger.seek(Bound::Included(&ObjectKey::child(self.object_id, from))).await?;
        // Skip deleted entries.
        // TODO(csuter): Remove this once we've developed a filtering iterator.
        loop {
            match iter.get() {
                Some(ItemRef { key: ObjectKey { object_id, .. }, value: ObjectValue::None })
                    if *object_id == self.object_id => {}
                _ => break,
            }
            iter.advance().await?;
        }
        Ok(DirectoryIterator { object_id: self.object_id, iter })
    }
}

impl<S: AsRef<ObjectStore> + Send + Sync + 'static> fmt::Debug for Directory<S> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Directory")
            .field("store_id", &self.store().store_object_id())
            .field("object_id", &self.object_id)
            .finish()
    }
}

pub struct DirectoryIterator<'a, 'b> {
    object_id: u64,
    iter: MergerIterator<'a, 'b, ObjectKey, ObjectValue>,
}

impl DirectoryIterator<'_, '_> {
    pub fn get(&self) -> Option<(&str, u64, &ObjectDescriptor)> {
        match self.iter.get() {
            Some(ItemRef {
                key: ObjectKey { object_id: oid, data: ObjectKeyData::Child { name } },
                value: ObjectValue::Child { object_id, object_descriptor },
            }) if *oid == self.object_id => Some((name, *object_id, object_descriptor)),
            _ => None,
        }
    }

    pub async fn advance(&mut self) -> Result<(), Error> {
        loop {
            self.iter.advance().await?;
            // Skip deleted entries.
            match self.iter.get() {
                Some(ItemRef { key: ObjectKey { object_id, .. }, value: ObjectValue::None })
                    if *object_id == self.object_id => {}
                _ => return Ok(()),
            }
        }
    }
}

/// Moves src.0/src.1 to dst.0/dst.1.
///
/// If |dst.0| already has a child |dst.1|, it is removed. If that child was a directory, it must
/// be empty.
///
/// If |src| is None, this is effectively the same as unlink(dst.0/dst.1).
///
/// If there is an existing entry, it is returned and the caller is responsible for adjusting the
/// reference count or moving into the graveyard as appropriate.
#[must_use]
pub async fn replace_child<'a, S: AsRef<ObjectStore> + Send + Sync + 'static>(
    transaction: &mut Transaction<'a>,
    src: Option<(&'a Directory<S>, &str)>,
    dst: (&'a Directory<S>, &str),
) -> Result<Option<(u64, ObjectDescriptor)>, Error> {
    let deleted_id_and_descriptor = dst.0.lookup(dst.1).await?;
    match deleted_id_and_descriptor {
        Some((_, ObjectDescriptor::File)) => {}
        Some((old_id, ObjectDescriptor::Directory)) => {
            let dir = Directory::open(&dst.0.owner(), old_id).await?;
            if dir.has_children().await? {
                bail!(FxfsError::NotEmpty);
            }
        }
        Some((_, ObjectDescriptor::Volume)) => bail!(FxfsError::Inconsistent),
        None => {
            if src.is_none() {
                // Neither src nor dst exist
                bail!(FxfsError::NotFound);
            }
        }
    };
    let now = current_time();
    let new_value = if let Some((src_dir, src_name)) = src {
        transaction.add(
            src_dir.store().store_object_id(),
            Mutation::replace_or_insert_object(
                ObjectKey::child(src_dir.object_id, src_name),
                ObjectValue::None,
            ),
        );
        src_dir.update_timestamps(transaction, None, Some(now.clone())).await?;
        let (id, descriptor) = src_dir.lookup(src_name).await?.ok_or(FxfsError::NotFound)?;
        ObjectValue::child(id, descriptor)
    } else {
        ObjectValue::None
    };
    transaction.add(
        dst.0.store().store_object_id(),
        Mutation::replace_or_insert_object(ObjectKey::child(dst.0.object_id, dst.1), new_value),
    );
    dst.0.update_timestamps(transaction, None, Some(now)).await?;
    Ok(deleted_id_and_descriptor)
}

pub fn remove(transaction: &mut Transaction<'_>, store: &ObjectStore, object_id: u64) {
    transaction.add(
        store.store_object_id(),
        Mutation::merge_object(ObjectKey::tombstone(object_id), ObjectValue::None),
    );
}

#[cfg(test)]
mod tests {
    use {
        crate::{
            device::DeviceHolder,
            errors::FxfsError,
            object_store::{
                directory::{replace_child, Directory},
                filesystem::{FxFilesystem, SyncOptions},
                transaction::TransactionHandler,
                HandleOptions, ObjectDescriptor, ObjectHandle, ObjectHandleExt, ObjectStore,
            },
            testing::fake_device::FakeDevice,
        },
        fuchsia_async as fasync,
    };

    const TEST_DEVICE_BLOCK_SIZE: u32 = 512;

    #[fasync::run_singlethreaded(test)]
    async fn test_create_directory() {
        let device = DeviceHolder::new(FakeDevice::new(2048, TEST_DEVICE_BLOCK_SIZE));
        let fs = FxFilesystem::new_empty(device).await.expect("new_empty failed");
        let object_id = {
            let mut transaction =
                fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
            let dir =
                Directory::create(&mut transaction, &fs.root_store()).await.expect("create failed");

            let child_dir = dir
                .create_child_dir(&mut transaction, "foo")
                .await
                .expect("create_child_dir failed");
            let _child_dir_file = child_dir
                .create_child_file(&mut transaction, "bar")
                .await
                .expect("create_child_file failed");
            let _child_file = dir
                .create_child_file(&mut transaction, "baz")
                .await
                .expect("create_child_file failed");
            dir.add_child_volume(&mut transaction, "corge", 100)
                .await
                .expect("add_child_volume failed");
            transaction.commit().await;
            fs.sync(SyncOptions::default()).await.expect("sync failed");
            dir.object_id()
        };
        let fs = FxFilesystem::open(fs.take_device().await).await.expect("open failed");
        {
            let dir = Directory::open(&fs.root_store(), object_id).await.expect("open failed");
            let (object_id, object_descriptor) =
                dir.lookup("foo").await.expect("lookup failed").expect("not found");
            assert_eq!(object_descriptor, ObjectDescriptor::Directory);
            let child_dir =
                Directory::open(&fs.root_store(), object_id).await.expect("open failed");
            let (object_id, object_descriptor) =
                child_dir.lookup("bar").await.expect("lookup failed").expect("not found");
            assert_eq!(object_descriptor, ObjectDescriptor::File);
            let _child_dir_file =
                ObjectStore::open_object(&fs.root_store(), object_id, HandleOptions::default())
                    .await
                    .expect("open object failed");
            let (object_id, object_descriptor) =
                dir.lookup("baz").await.expect("lookup failed").expect("not found");
            assert_eq!(object_descriptor, ObjectDescriptor::File);
            let _child_file =
                ObjectStore::open_object(&fs.root_store(), object_id, HandleOptions::default())
                    .await
                    .expect("open object failed");
            let (object_id, object_descriptor) =
                dir.lookup("corge").await.expect("lookup failed").expect("not found");
            assert_eq!(object_id, 100);
            if let ObjectDescriptor::Volume = object_descriptor {
            } else {
                panic!("wrong ObjectDescriptor");
            }

            assert_eq!(dir.lookup("qux").await.expect("lookup failed"), None);
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_delete_child() {
        let device = DeviceHolder::new(FakeDevice::new(2048, TEST_DEVICE_BLOCK_SIZE));
        let fs = FxFilesystem::new_empty(device).await.expect("new_empty failed");
        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        let dir =
            Directory::create(&mut transaction, &fs.root_store()).await.expect("create failed");

        dir.create_child_file(&mut transaction, "foo").await.expect("create_child_file failed");
        transaction.commit().await;

        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        assert!(replace_child(&mut transaction, None, (&dir, "foo"))
            .await
            .expect("replace_child failed")
            .is_some());
        transaction.commit().await;

        assert_eq!(dir.lookup("foo").await.expect("lookup failed"), None);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_delete_child_with_children_fails() {
        let device = DeviceHolder::new(FakeDevice::new(2048, TEST_DEVICE_BLOCK_SIZE));
        let fs = FxFilesystem::new_empty(device).await.expect("new_empty failed");
        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        let dir =
            Directory::create(&mut transaction, &fs.root_store()).await.expect("create failed");

        let child =
            dir.create_child_dir(&mut transaction, "foo").await.expect("create_child_dir failed");
        child.create_child_file(&mut transaction, "bar").await.expect("create_child_file failed");
        transaction.commit().await;

        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        assert_eq!(
            replace_child(&mut transaction, None, (&dir, "foo"))
                .await
                .expect_err("replace_child succeeded")
                .downcast::<FxfsError>()
                .expect("wrong error"),
            FxfsError::NotEmpty
        );

        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        assert!(replace_child(&mut transaction, None, (&child, "bar"))
            .await
            .expect("replace_child failed")
            .is_some());
        transaction.commit().await;

        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        assert!(replace_child(&mut transaction, None, (&dir, "foo"))
            .await
            .expect("replace_child failed")
            .is_some());
        transaction.commit().await;

        assert_eq!(dir.lookup("foo").await.expect("lookup failed"), None);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_delete_and_reinsert_child() {
        let device = DeviceHolder::new(FakeDevice::new(2048, TEST_DEVICE_BLOCK_SIZE));
        let fs = FxFilesystem::new_empty(device).await.expect("new_empty failed");
        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        let dir =
            Directory::create(&mut transaction, &fs.root_store()).await.expect("create failed");

        dir.create_child_file(&mut transaction, "foo").await.expect("create_child_file failed");
        transaction.commit().await;

        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        assert!(replace_child(&mut transaction, None, (&dir, "foo"))
            .await
            .expect("replace_child failed")
            .is_some());
        transaction.commit().await;

        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        dir.create_child_file(&mut transaction, "foo").await.expect("create_child_file failed");
        transaction.commit().await;

        dir.lookup("foo").await.expect("lookup failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_delete_child_persists() {
        let device = DeviceHolder::new(FakeDevice::new(2048, TEST_DEVICE_BLOCK_SIZE));
        let fs = FxFilesystem::new_empty(device).await.expect("new_empty failed");
        let object_id = {
            let mut transaction =
                fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
            let dir =
                Directory::create(&mut transaction, &fs.root_store()).await.expect("create failed");

            dir.create_child_file(&mut transaction, "foo").await.expect("create_child_file failed");
            transaction.commit().await;
            dir.lookup("foo").await.expect("lookup failed");

            let mut transaction =
                fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
            assert!(replace_child(&mut transaction, None, (&dir, "foo"))
                .await
                .expect("replace_child failed")
                .is_some());
            transaction.commit().await;

            fs.sync(SyncOptions::default()).await.expect("sync failed");
            dir.object_id()
        };

        let fs = FxFilesystem::open(fs.take_device().await).await.expect("new_empty failed");
        let dir = Directory::open(&fs.root_store(), object_id).await.expect("open failed");
        assert_eq!(dir.lookup("foo").await.expect("lookup failed"), None);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_replace_child() {
        let device = DeviceHolder::new(FakeDevice::new(2048, TEST_DEVICE_BLOCK_SIZE));
        let fs = FxFilesystem::new_empty(device).await.expect("new_empty failed");
        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        let dir =
            Directory::create(&mut transaction, &fs.root_store()).await.expect("create failed");

        let child_dir1 =
            dir.create_child_dir(&mut transaction, "dir1").await.expect("create_child_dir failed");
        let child_dir2 =
            dir.create_child_dir(&mut transaction, "dir2").await.expect("create_child_dir failed");
        child_dir1
            .create_child_file(&mut transaction, "foo")
            .await
            .expect("create_child_file failed");
        transaction.commit().await;

        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        replace_child(&mut transaction, Some((&child_dir1, "foo")), (&child_dir2, "bar"))
            .await
            .expect("replace_child failed");
        transaction.commit().await;

        assert_eq!(child_dir1.lookup("foo").await.expect("lookup failed"), None);
        child_dir2.lookup("bar").await.expect("lookup failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_replace_child_overwrites_dst() {
        let device = DeviceHolder::new(FakeDevice::new(2048, TEST_DEVICE_BLOCK_SIZE));
        let fs = FxFilesystem::new_empty(device).await.expect("new_empty failed");
        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        let dir =
            Directory::create(&mut transaction, &fs.root_store()).await.expect("create failed");

        let child_dir1 =
            dir.create_child_dir(&mut transaction, "dir1").await.expect("create_child_dir failed");
        let child_dir2 =
            dir.create_child_dir(&mut transaction, "dir2").await.expect("create_child_dir failed");
        let foo = child_dir1
            .create_child_file(&mut transaction, "foo")
            .await
            .expect("create_child_file failed");
        let bar = child_dir2
            .create_child_file(&mut transaction, "bar")
            .await
            .expect("create_child_file failed");
        transaction.commit().await;

        {
            let mut buf = foo.allocate_buffer(TEST_DEVICE_BLOCK_SIZE as usize);
            buf.as_mut_slice().fill(0xaa);
            foo.write(0, buf.as_ref()).await.expect("write failed");
            buf.as_mut_slice().fill(0xbb);
            bar.write(0, buf.as_ref()).await.expect("write failed");
        }
        std::mem::drop(bar);
        std::mem::drop(foo);

        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        replace_child(&mut transaction, Some((&child_dir1, "foo")), (&child_dir2, "bar"))
            .await
            .expect("replace_child failed");
        transaction.commit().await;

        assert_eq!(child_dir1.lookup("foo").await.expect("lookup failed"), None);

        // Check the contents to ensure that the file was replaced.
        let (oid, object_descriptor) =
            child_dir2.lookup("bar").await.expect("lookup failed").expect("not found");
        assert_eq!(object_descriptor, ObjectDescriptor::File);
        let bar = ObjectStore::open_object(&child_dir2.owner, oid, HandleOptions::default())
            .await
            .expect("Open failed");
        let mut buf = bar.allocate_buffer(TEST_DEVICE_BLOCK_SIZE as usize);
        bar.read(0, buf.as_mut()).await.expect("read failed");
        assert_eq!(buf.as_slice(), vec![0xaa; TEST_DEVICE_BLOCK_SIZE as usize]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_replace_child_fails_if_would_overwrite_nonempty_dir() {
        let device = DeviceHolder::new(FakeDevice::new(2048, TEST_DEVICE_BLOCK_SIZE));
        let fs = FxFilesystem::new_empty(device).await.expect("new_empty failed");
        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        let dir =
            Directory::create(&mut transaction, &fs.root_store()).await.expect("create failed");

        let child_dir1 =
            dir.create_child_dir(&mut transaction, "dir1").await.expect("create_child_dir failed");
        let child_dir2 =
            dir.create_child_dir(&mut transaction, "dir2").await.expect("create_child_dir failed");
        child_dir1
            .create_child_file(&mut transaction, "foo")
            .await
            .expect("create_child_file failed");
        let nested_child = child_dir2
            .create_child_dir(&mut transaction, "bar")
            .await
            .expect("create_child_file failed");
        nested_child
            .create_child_file(&mut transaction, "baz")
            .await
            .expect("create_child_file failed");
        transaction.commit().await;

        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        assert_eq!(
            replace_child(&mut transaction, Some((&child_dir1, "foo")), (&child_dir2, "bar"))
                .await
                .expect_err("replace_child succeeded")
                .downcast::<FxfsError>()
                .expect("wrong error"),
            FxfsError::NotEmpty
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_replace_child_within_dir() {
        let device = DeviceHolder::new(FakeDevice::new(2048, TEST_DEVICE_BLOCK_SIZE));
        let fs = FxFilesystem::new_empty(device).await.expect("new_empty failed");
        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        let dir =
            Directory::create(&mut transaction, &fs.root_store()).await.expect("create failed");
        dir.create_child_file(&mut transaction, "foo").await.expect("create_child_file failed");
        transaction.commit().await;

        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        replace_child(&mut transaction, Some((&dir, "foo")), (&dir, "bar"))
            .await
            .expect("replace_child failed");
        transaction.commit().await;

        assert_eq!(dir.lookup("foo").await.expect("lookup failed"), None);
        dir.lookup("bar").await.expect("lookup new name failed");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_iterate() {
        let device = DeviceHolder::new(FakeDevice::new(2048, TEST_DEVICE_BLOCK_SIZE));
        let fs = FxFilesystem::new_empty(device).await.expect("new_empty failed");
        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        let dir =
            Directory::create(&mut transaction, &fs.root_store()).await.expect("create failed");
        let _cat =
            dir.create_child_file(&mut transaction, "cat").await.expect("create_child_file failed");
        let _ball = dir
            .create_child_file(&mut transaction, "ball")
            .await
            .expect("create_child_file failed");
        let _apple = dir
            .create_child_file(&mut transaction, "apple")
            .await
            .expect("create_child_file failed");
        let _dog =
            dir.create_child_file(&mut transaction, "dog").await.expect("create_child_file failed");
        transaction.commit().await;
        let mut transaction =
            fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        replace_child(&mut transaction, None, (&dir, "apple"))
            .await
            .expect("rereplace_child failed");
        transaction.commit().await;
        let layer_set = dir.store().tree().layer_set();
        let mut merger = layer_set.merger();
        let mut iter = dir.iter(&mut merger).await.expect("iter failed");
        let mut entries = Vec::new();
        while let Some((name, _, _)) = iter.get() {
            entries.push(name.to_string());
            iter.advance().await.expect("advance failed");
        }
        assert_eq!(&entries, &["ball", "cat", "dog"]);
    }
}
