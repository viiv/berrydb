// Copyright 2017 The BerryDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "./page_pool.h"

#include <cstring>
#include <random>
#include <string>

#include "gtest/gtest.h"

#include "berrydb/options.h"
#include "berrydb/store.h"
#include "berrydb/vfs.h"
#include "./page_pool.h"
#include "./pool_impl.h"
#include "./store_impl.h"
#include "./test/block_access_file_wrapper.h"
#include "./test/file_deleter.h"
#include "./util/unique_ptr.h"

namespace berrydb {

class PagePoolTest : public ::testing::Test {
 protected:
  PagePoolTest()
      : vfs_(DefaultVfs()), data_file1_deleter_(kStoreFileName1),
        log_file1_deleter_(StoreImpl::LogFilePath(kStoreFileName1)) { }

  void SetUp() override {
    BlockAccessFile* raw_data_file1;
    ASSERT_EQ(Status::kSuccess, vfs_->OpenForBlockAccess(
        data_file1_deleter_.path(), kStorePageShift, true, false,
        &raw_data_file1, &data_file1_size_));
    data_file1_.reset(raw_data_file1);
    RandomAccessFile* raw_log_file1;
    ASSERT_EQ(Status::kSuccess, vfs_->OpenForRandomAccess(
        log_file1_deleter_.path(), true, false, &raw_log_file1,
        &log_file1_size_));
    log_file1_.reset(raw_log_file1);
  }

  void CreatePool(int page_shift, int page_capacity) {
    PoolOptions options;
    options.page_shift = page_shift;
    options.page_pool_size = page_capacity;
    pool_.reset(PoolImpl::Create(options));
  }

  void WriteStorePage(StoreImpl* store, size_t page_id, const uint8_t* data) {
    ASSERT_TRUE(pool_.get() != nullptr);
    PagePool* page_pool = pool_->page_pool();
    Page* page = page_pool->AllocPage();
    ASSERT_TRUE(page != nullptr);

    ASSERT_EQ(Status::kSuccess, page_pool->AssignPageToStore(
        page, store, page_id, PagePool::kIgnorePageData));
    page->MarkDirty();
    std::memcpy(page->data(), data, 1 << kStorePageShift);
    ASSERT_EQ(Status::kSuccess, store->WritePage(page));
    page->MarkDirty(false);
    page_pool->UnassignPageFromStore(page);
    page_pool->UnpinUnassignedPage(page);
  }

  const std::string kStoreFileName1 = "test_page_pool_1.berry";
  constexpr static size_t kStorePageShift = 12;

  Vfs* vfs_;
  // Must precede UniquePtr members, because on Windows all file handles must be
  // closed before the files can be deleted.
  FileDeleter data_file1_deleter_, log_file1_deleter_;

  UniquePtr<PoolImpl> pool_;
  UniquePtr<BlockAccessFile> data_file1_;
  size_t data_file1_size_;
  UniquePtr<RandomAccessFile> log_file1_;
  size_t log_file1_size_;
  std::mt19937 rnd_;
};

TEST_F(PagePoolTest, Constructor) {
  CreatePool(16, 42);
  PagePool page_pool(pool_.get(), 16, 42);
  EXPECT_EQ(16U, page_pool.page_shift());
  EXPECT_EQ(65536U, page_pool.page_size());
  EXPECT_EQ(42U, page_pool.page_capacity());

  EXPECT_EQ(0U, page_pool.allocated_pages());
  EXPECT_EQ(0U, page_pool.unused_pages());
  EXPECT_EQ(0U, page_pool.pinned_pages());
}

TEST_F(PagePoolTest, AllocPageState) {
  CreatePool(12, 1);
  PagePool page_pool(pool_.get(), 12, 1);

  Page* page = page_pool.AllocPage();
  ASSERT_NE(nullptr, page);
  EXPECT_NE(nullptr, page->data());
  EXPECT_EQ(1U, page_pool.allocated_pages());
  EXPECT_EQ(0U, page_pool.unused_pages());
  EXPECT_EQ(1U, page_pool.pinned_pages());

  EXPECT_FALSE(page->is_dirty());
  EXPECT_FALSE(page->IsUnpinned());
#if DCHECK_IS_ON()
  EXPECT_EQ(nullptr, page->transaction());
#endif  // DCHECK_IS_ON()

  page_pool.UnpinUnassignedPage(page);
}

TEST_F(PagePoolTest, AllocRespectsCapacity) {
  CreatePool(12, 1);
  PagePool page_pool(pool_.get(), 12, 1);

  Page* page = page_pool.AllocPage();
  ASSERT_NE(nullptr, page);
  EXPECT_EQ(1U, page_pool.allocated_pages());
  EXPECT_EQ(0U, page_pool.unused_pages());
  EXPECT_EQ(1U, page_pool.pinned_pages());

  ASSERT_EQ(nullptr, page_pool.AllocPage());
  EXPECT_EQ(1U, page_pool.allocated_pages());
  EXPECT_EQ(0U, page_pool.unused_pages());
  EXPECT_EQ(1U, page_pool.pinned_pages());

  page_pool.UnpinUnassignedPage(page);
  EXPECT_EQ(1U, page_pool.allocated_pages());
  EXPECT_EQ(1U, page_pool.unused_pages());
  EXPECT_EQ(0U, page_pool.pinned_pages());
}

TEST_F(PagePoolTest, AllocUsesFreeList) {
  CreatePool(12, 1);
  PagePool* page_pool = pool_->page_pool();

  Page* page = page_pool->AllocPage();
  ASSERT_NE(nullptr, page);

  page_pool->UnpinUnassignedPage(page);
  EXPECT_EQ(1U, page_pool->allocated_pages());
  EXPECT_EQ(1U, page_pool->unused_pages());
  EXPECT_EQ(0U, page_pool->pinned_pages());

  Page* page2 = page_pool->AllocPage();
  EXPECT_EQ(page, page2);
  EXPECT_EQ(1U, page_pool->allocated_pages());
  EXPECT_EQ(0U, page_pool->unused_pages());
  EXPECT_EQ(1U, page_pool->pinned_pages());

  page_pool->UnpinUnassignedPage(page2);
}

TEST_F(PagePoolTest, AllocUsesLruList) {
  CreatePool(kStorePageShift, 1);
  PagePool* page_pool = pool_->page_pool();

  EXPECT_EQ(0U, data_file1_size_);
  UniquePtr<StoreImpl> store(StoreImpl::Create(
      data_file1_.release(), data_file1_size_, log_file1_.release(),
      log_file1_size_, page_pool, StoreOptions()));

  Page* page = page_pool->AllocPage();
  ASSERT_NE(nullptr, page);

  ASSERT_EQ(Status::kSuccess, page_pool->AssignPageToStore(
      page, store.get(), 0, PagePool::kIgnorePageData));
  EXPECT_EQ(store->init_transaction(), page->transaction());

  // Unset the page's dirty bit to avoid having the page written to the store
  // when it is evicted from the LRU list.
  page->MarkDirty(false);
  page_pool->UnpinStorePage(page);
  EXPECT_EQ(1U, page_pool->allocated_pages());
  EXPECT_EQ(0U, page_pool->unused_pages());
  EXPECT_EQ(0U, page_pool->pinned_pages());

  Page* page2 = page_pool->AllocPage();
  EXPECT_EQ(page, page2);
  EXPECT_EQ(1U, page_pool->allocated_pages());
  EXPECT_EQ(0U, page_pool->unused_pages());
  EXPECT_EQ(1U, page_pool->pinned_pages());
#if DCHECK_IS_ON()
  EXPECT_EQ(nullptr, page2->transaction());
#endif  // DCHECK_IS_ON()

  page_pool->UnpinUnassignedPage(page2);
}

TEST_F(PagePoolTest, AllocPrefersFreeListToLruList) {
  CreatePool(kStorePageShift, 2);
  PagePool* page_pool = pool_->page_pool();

  EXPECT_EQ(0U, data_file1_size_);
  UniquePtr<StoreImpl> store(StoreImpl::Create(
      data_file1_.release(), data_file1_size_, log_file1_.release(),
      log_file1_size_, page_pool, StoreOptions()));

  Page* page = page_pool->AllocPage();
  ASSERT_NE(nullptr, page);

  ASSERT_EQ(Status::kSuccess, page_pool->AssignPageToStore(
      page, store.get(), 0, PagePool::kIgnorePageData));
  EXPECT_EQ(store->init_transaction(), page->transaction());
#if DCHECK_IS_ON()
  EXPECT_EQ(1U, store->AssignedPageCount());
#endif  // DCHECK_IS_ON()

  // Unset the page's dirty bit to avoid having the page written to the store
  // if it is evicted from the LRU list.
  page->MarkDirty(false);
  page_pool->UnpinStorePage(page);
  ASSERT_EQ(1U, page_pool->allocated_pages());
  ASSERT_EQ(0U, page_pool->unused_pages());
  ASSERT_EQ(0U, page_pool->pinned_pages());

  Page* page2 = page_pool->AllocPage();
  EXPECT_NE(page, page2);
  EXPECT_EQ(2U, page_pool->allocated_pages());
  EXPECT_EQ(0U, page_pool->unused_pages());
  EXPECT_EQ(1U, page_pool->pinned_pages());
#if DCHECK_IS_ON()
  EXPECT_EQ(nullptr, page2->transaction());
  EXPECT_EQ(1U, store->AssignedPageCount());
#endif  // DCHECK_IS_ON()

  page_pool->UnpinUnassignedPage(page2);
}

TEST_F(PagePoolTest, UnassignPageFromStoreState) {
  CreatePool(kStorePageShift, 1);
  PagePool* page_pool = pool_->page_pool();

  EXPECT_EQ(0U, data_file1_size_);
  UniquePtr<StoreImpl> store(StoreImpl::Create(
      data_file1_.release(), data_file1_size_, log_file1_.release(),
      log_file1_size_, page_pool, StoreOptions()));

  Page* page = page_pool->AllocPage();
  ASSERT_NE(nullptr, page);
  ASSERT_EQ(Status::kSuccess, page_pool->AssignPageToStore(
      page, store.get(), 0, PagePool::kIgnorePageData));
  EXPECT_EQ(store->init_transaction(), page->transaction());

  page->MarkDirty(true);
  uint8_t* data = page->data();
  ASSERT_NE(nullptr, data);
  std::memset(data, 0, 1 << kStorePageShift);

  page_pool->UnassignPageFromStore(page);
  EXPECT_FALSE(page->is_dirty());
  EXPECT_FALSE(page->IsUnpinned());
#if DCHECK_IS_ON()
  EXPECT_EQ(nullptr, page->transaction());
#endif  // DCHECK_IS_ON()
  EXPECT_FALSE(store->IsClosed());

  page_pool->UnpinUnassignedPage(page);
}

TEST_F(PagePoolTest, UnassignPageFromStoreIoError) {
  CreatePool(kStorePageShift, 1);
  PagePool* page_pool = pool_->page_pool();

  EXPECT_EQ(0U, data_file1_size_);
  BlockAccessFileWrapper data_file_wrapper(data_file1_.release());
  UniquePtr<StoreImpl> store(StoreImpl::Create(
      &data_file_wrapper, data_file1_size_, log_file1_.release(),
      log_file1_size_, page_pool, StoreOptions()));

  Page* page = page_pool->AllocPage();
  ASSERT_NE(nullptr, page);
  ASSERT_EQ(Status::kSuccess, page_pool->AssignPageToStore(
      page, store.get(), 0, PagePool::kIgnorePageData));
  EXPECT_EQ(store->init_transaction(), page->transaction());

  page->MarkDirty(true);
  uint8_t* data = page->data();
  ASSERT_NE(nullptr, data);
  std::memset(data, 0, 1 << kStorePageShift);

  data_file_wrapper.SetAccessError(Status::kIoError);
  page_pool->UnassignPageFromStore(page);
  EXPECT_FALSE(page->is_dirty());
  EXPECT_FALSE(page->IsUnpinned());
#if DCHECK_IS_ON()
  EXPECT_EQ(nullptr, page->transaction());
#endif  // DCHECK_IS_ON()
  EXPECT_EQ(true, store->IsClosed());

  page_pool->UnpinUnassignedPage(page);
}

TEST_F(PagePoolTest, AssignPageToStoreSuccess) {
  uint8_t buffer[4 << kStorePageShift];
  for(size_t i = 0; i < sizeof(buffer); ++i)
    buffer[i] = static_cast<uint8_t>(rnd_());

  CreatePool(kStorePageShift, 1);
  PagePool* page_pool = pool_->page_pool();
  UniquePtr<StoreImpl> store(StoreImpl::Create(
      data_file1_.release(), data_file1_size_, log_file1_.release(),
      log_file1_size_, page_pool, StoreOptions()));

  for (size_t i = 0; i < 4; ++i)
    WriteStorePage(store.get(), i, buffer + (i << kStorePageShift));

  for (size_t i = 0; i < 4; ++i) {
    Page* page = page_pool->AllocPage();
    ASSERT_NE(nullptr, page);
    EXPECT_EQ(Status::kSuccess, page_pool->AssignPageToStore(
        page, store.get(), i, PagePool::kFetchPageData));
    EXPECT_FALSE(page->is_dirty());
    EXPECT_FALSE(page->IsUnpinned());
    EXPECT_EQ(store->init_transaction(), page->transaction());
    EXPECT_EQ(i, page->page_id());
    EXPECT_EQ(0, std::memcmp(
        page->data(), buffer + (i << kStorePageShift), 1 << kStorePageShift));
    page_pool->UnpinStorePage(page);
  }
}

TEST_F(PagePoolTest, AssignPageToStoreIoError) {
  uint8_t buffer[2 << kStorePageShift];
  for(size_t i = 0; i < sizeof(buffer); ++i)
    buffer[i] = static_cast<uint8_t>(rnd_());

  CreatePool(kStorePageShift, 1);
  PagePool* page_pool = pool_->page_pool();
  BlockAccessFileWrapper data_file_wrapper(data_file1_.release());
  UniquePtr<StoreImpl> store(StoreImpl::Create(
      &data_file_wrapper, data_file1_size_, log_file1_.release(),
      log_file1_size_, page_pool, StoreOptions()));

  for (size_t i = 0; i < 2; ++i)
    WriteStorePage(store.get(), i, buffer + (i << kStorePageShift));

  Page* page = page_pool->AllocPage();
  ASSERT_NE(nullptr, page);
  EXPECT_EQ(Status::kSuccess, page_pool->AssignPageToStore(
      page, store.get(), 1, PagePool::kFetchPageData));
  page_pool->UnpinStorePage(page);

  data_file_wrapper.SetAccessError(Status::kIoError);
  page = page_pool->AllocPage();
  ASSERT_NE(nullptr, page);
  EXPECT_EQ(Status::kIoError, page_pool->AssignPageToStore(
      page, store.get(), 1, PagePool::kFetchPageData));
  EXPECT_FALSE(page->is_dirty());
  EXPECT_FALSE(page->IsUnpinned());
#if DCHECK_IS_ON()
  EXPECT_EQ(nullptr, page->transaction());
#endif  // DCHECK_IS_ON()

  page_pool->UnpinUnassignedPage(page);
}

TEST_F(PagePoolTest, UnpinUnassignedPageState) {
  CreatePool(12, 1);
  PagePool page_pool(pool_.get(), 12, 1);

  Page* page = page_pool.AllocPage();
  ASSERT_NE(nullptr, page);
  EXPECT_EQ(1U, page_pool.allocated_pages());
  EXPECT_EQ(0U, page_pool.unused_pages());
  EXPECT_EQ(1U, page_pool.pinned_pages());

  page_pool.UnpinUnassignedPage(page);
  EXPECT_EQ(1U, page_pool.allocated_pages());
  EXPECT_EQ(1U, page_pool.unused_pages());
  EXPECT_EQ(0U, page_pool.pinned_pages());

  EXPECT_FALSE(page->is_dirty());
  EXPECT_TRUE(page->IsUnpinned());
#if DCHECK_IS_ON()
  EXPECT_EQ(nullptr, page->transaction());
#endif  // DCHECK_IS_ON()
}

TEST_F(PagePoolTest, PinUnpinStorePage) {
  uint8_t buffer[4 << kStorePageShift];
  for(size_t i = 0; i < sizeof(buffer); ++i)
    buffer[i] = static_cast<uint8_t>(rnd_());

  CreatePool(kStorePageShift, 1);
  PagePool* page_pool = pool_->page_pool();
  BlockAccessFileWrapper data_file_wrapper(data_file1_.release());
  UniquePtr<StoreImpl> store(StoreImpl::Create(
      &data_file_wrapper, data_file1_size_, log_file1_.release(),
      log_file1_size_, page_pool, StoreOptions()));

  Page* page = page_pool->AllocPage();
  ASSERT_NE(nullptr, page);
  ASSERT_EQ(Status::kSuccess, page_pool->AssignPageToStore(
      page, store.get(), 42, PagePool::kIgnorePageData));
  EXPECT_EQ(store->init_transaction(), page->transaction());

  page->MarkDirty(false);

  // Should add a pin to the page.
  page_pool->PinStorePage(page);
  EXPECT_FALSE(page->is_dirty());
  EXPECT_FALSE(page->IsUnpinned());
  EXPECT_EQ(42U, page->page_id());
  EXPECT_EQ(store->init_transaction(), page->transaction());
  EXPECT_EQ(1U, page_pool->allocated_pages());
  EXPECT_EQ(0U, page_pool->unused_pages());
  EXPECT_EQ(1U, page_pool->pinned_pages());

  // Should remove one of the pins from the page.
  page_pool->UnpinStorePage(page);
  EXPECT_FALSE(page->is_dirty());
  EXPECT_FALSE(page->IsUnpinned());
  EXPECT_EQ(store->init_transaction(), page->transaction());
  EXPECT_EQ(42U, page->page_id());
  EXPECT_EQ(1U, page_pool->allocated_pages());
  EXPECT_EQ(0U, page_pool->unused_pages());
  EXPECT_EQ(1U, page_pool->pinned_pages());

  // Should remove the last pin and add the page to the LRU list.
  page_pool->UnpinStorePage(page);
  EXPECT_FALSE(page->is_dirty());
  EXPECT_TRUE(page->IsUnpinned());
  EXPECT_EQ(store->init_transaction(), page->transaction());
  EXPECT_EQ(42U, page->page_id());
  EXPECT_EQ(1U, page_pool->allocated_pages());
  EXPECT_EQ(0U, page_pool->unused_pages());
  EXPECT_EQ(0U, page_pool->pinned_pages());

  // Should remove the page from the LRU list.
  page_pool->PinStorePage(page);
  EXPECT_FALSE(page->is_dirty());
  EXPECT_FALSE(page->IsUnpinned());
  EXPECT_EQ(store->init_transaction(), page->transaction());
  EXPECT_EQ(42U, page->page_id());
  EXPECT_EQ(1U, page_pool->allocated_pages());
  EXPECT_EQ(0U, page_pool->unused_pages());
  EXPECT_EQ(1U, page_pool->pinned_pages());

  page_pool->UnpinStorePage(page);
}

TEST_F(PagePoolTest, StorePage) {
  uint8_t buffer[4 << kStorePageShift];
  for(size_t i = 0; i < sizeof(buffer); ++i)
    buffer[i] = static_cast<uint8_t>(rnd_());

  CreatePool(kStorePageShift, 2);
  PagePool* page_pool = pool_->page_pool();
  BlockAccessFileWrapper data_file_wrapper(data_file1_.release());
  UniquePtr<StoreImpl> store(StoreImpl::Create(
      &data_file_wrapper, data_file1_size_, log_file1_.release(),
      log_file1_size_, page_pool, StoreOptions()));

  for (size_t i = 0; i < 4; ++i)
    WriteStorePage(store.get(), i, buffer + (i << kStorePageShift));

  Page* page;
  ASSERT_EQ(Status::kSuccess, page_pool->StorePage(
      store.get(), 2, PagePool::kFetchPageData, &page));

  ASSERT_TRUE(page != nullptr);
  EXPECT_FALSE(page->is_dirty());
  EXPECT_FALSE(page->IsUnpinned());
  EXPECT_EQ(store->init_transaction(), page->transaction());
  EXPECT_EQ(2U, page->page_id());
  EXPECT_EQ(0, std::memcmp(
      page->data(), buffer + (2 << kStorePageShift), 1 << kStorePageShift));

  Page* page2;
  ASSERT_EQ(Status::kSuccess, page_pool->StorePage(
      store.get(), 2, PagePool::kFetchPageData, &page2));
  EXPECT_EQ(page, page2);
  EXPECT_FALSE(page2->is_dirty());
  EXPECT_FALSE(page2->IsUnpinned());
  EXPECT_EQ(store->init_transaction(), page->transaction());
  EXPECT_EQ(2U, page2->page_id());
  EXPECT_EQ(0, std::memcmp(
      page2->data(), buffer + (2 << kStorePageShift), 1 << kStorePageShift));
  EXPECT_EQ(1U, page_pool->allocated_pages());
  EXPECT_EQ(0U, page_pool->unused_pages());
  EXPECT_EQ(1U, page_pool->pinned_pages());

  page_pool->UnpinStorePage(page2);
  EXPECT_FALSE(page->is_dirty());
  EXPECT_FALSE(page->IsUnpinned());
  EXPECT_EQ(store->init_transaction(), page->transaction());
  EXPECT_EQ(2U, page->page_id());
  EXPECT_EQ(1U, page_pool->allocated_pages());
  EXPECT_EQ(0U, page_pool->unused_pages());
  EXPECT_EQ(1U, page_pool->pinned_pages());

  page_pool->UnassignPageFromStore(page);
  page_pool->UnpinUnassignedPage(page);
  EXPECT_FALSE(page->is_dirty());
  EXPECT_TRUE(page->IsUnpinned());
#if DCHECK_IS_ON()
  EXPECT_EQ(nullptr, page->transaction());
#endif  // DCHECK_IS_ON()
  EXPECT_EQ(1U, page_pool->allocated_pages());
  EXPECT_EQ(1U, page_pool->unused_pages());
  EXPECT_EQ(0U, page_pool->pinned_pages());

  data_file_wrapper.SetAccessError(Status::kIoError);
  page = nullptr;
  EXPECT_EQ(Status::kIoError, page_pool->StorePage(
      store.get(), 2, PagePool::kFetchPageData, &page));
  EXPECT_EQ(nullptr, page);
  EXPECT_EQ(1U, page_pool->allocated_pages());
  EXPECT_EQ(1U, page_pool->unused_pages());
  EXPECT_EQ(0U, page_pool->pinned_pages());
}

}  // namespace berrydb
