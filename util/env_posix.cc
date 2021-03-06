//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <deque>
#include <set>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#ifdef OS_LINUX
#include <sys/statfs.h>
#endif
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if defined(OS_LINUX)
#include <linux/fs.h>
#include <fcntl.h>
#endif
#if defined(LEVELDB_PLATFORM_ANDROID)
#include <sys/stat.h>
#endif
#include "rocksdb/env.h"
#include "rocksdb/slice.h"
#include "port/port.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/posix_logger.h"
#include "util/random.h"
#include <signal.h>

// Get nano time for mach systems
#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#if !defined(TMPFS_MAGIC)
#define TMPFS_MAGIC 0x01021994
#endif
#if !defined(XFS_SUPER_MAGIC)
#define XFS_SUPER_MAGIC 0x58465342
#endif
#if !defined(EXT4_SUPER_MAGIC)
#define EXT4_SUPER_MAGIC 0xEF53
#endif

// For non linux platform, the following macros are used only as place
// holder.
#ifndef OS_LINUX
#define POSIX_FADV_NORMAL 0 /* [MC1] no further special treatment */
#define POSIX_FADV_RANDOM 1 /* [MC1] expect random page refs */
#define POSIX_FADV_SEQUENTIAL 2 /* [MC1] expect sequential page refs */
#define POSIX_FADV_WILLNEED 3 /* [MC1] will need these pages */
#define POSIX_FADV_DONTNEED 4 /* [MC1] dont need these pages */
#endif

// This is only set from db_stress.cc and for testing only.
// If non-zero, kill at various points in source code with probability 1/this
int rocksdb_kill_odds = 0;

namespace rocksdb {

namespace {

// A wrapper for fadvise, if the platform doesn't support fadvise,
// it will simply return Status::NotSupport.
int Fadvise(int fd, off_t offset, size_t len, int advice) {
#ifdef OS_LINUX
  return posix_fadvise(fd, offset, len, advice);
#else
  return 0;  // simply do nothing.
#endif
}

// list of pathnames that are locked
static std::set<std::string> lockedFiles;
static port::Mutex mutex_lockedFiles;

static Status IOError(const std::string& context, int err_number) {
  return Status::IOError(context, strerror(err_number));
}

#ifdef NDEBUG
// empty in release build
#define TEST_KILL_RANDOM(rocksdb_kill_odds)
#else

// Kill the process with probablity 1/odds for testing.
static void TestKillRandom(int odds, const std::string& srcfile,
                           int srcline) {
  time_t curtime = time(nullptr);
  Random r((uint32_t)curtime);

  assert(odds > 0);
  bool crash = r.OneIn(odds);
  if (crash) {
    fprintf(stdout, "Crashing at %s:%d\n", srcfile.c_str(), srcline);
    fflush(stdout);
    kill(getpid(), SIGTERM);
  }
}

// To avoid crashing always at some frequently executed codepaths (during
// kill random test), use this factor to reduce odds
#define REDUCE_ODDS 2
#define REDUCE_ODDS2 4

#define TEST_KILL_RANDOM(rocksdb_kill_odds) {   \
  if (rocksdb_kill_odds > 0) { \
    TestKillRandom(rocksdb_kill_odds, __FILE__, __LINE__);     \
  } \
}

#endif

#if defined(OS_LINUX)
namespace {
  static size_t GetUniqueIdFromFile(int fd, char* id, size_t max_size) {
    if (max_size < kMaxVarint64Length*3) {
      return 0;
    }

    struct stat buf;
    int result = fstat(fd, &buf);
    if (result == -1) {
      return 0;
    }

    long version = 0;
    result = ioctl(fd, FS_IOC_GETVERSION, &version);
    if (result == -1) {
      return 0;
    }
    uint64_t uversion = (uint64_t)version;

    char* rid = id;
    rid = EncodeVarint64(rid, buf.st_dev);
    rid = EncodeVarint64(rid, buf.st_ino);
    rid = EncodeVarint64(rid, uversion);
    assert(rid >= id);
    return static_cast<size_t>(rid-id);
  }
}
#endif

class PosixSequentialFile: public SequentialFile {
 private:
  std::string filename_;
  FILE* file_;
  int fd_;
  bool use_os_buffer_;

 public:
  PosixSequentialFile(const std::string& fname, FILE* f,
      const EnvOptions& options)
      : filename_(fname), file_(f), fd_(fileno(f)),
        use_os_buffer_(options.use_os_buffer) {
  }
  virtual ~PosixSequentialFile() { fclose(file_); }

  virtual Status Read(size_t n, Slice* result, char* scratch) {
    Status s;
    size_t r = fread_unlocked(scratch, 1, n, file_);
    *result = Slice(scratch, r);
    if (r < n) {
      if (feof(file_)) {
        // We leave status as ok if we hit the end of the file
      } else {
        // A partial read with an error: return a non-ok status
        s = IOError(filename_, errno);
      }
    }
    if (!use_os_buffer_) {
      // we need to fadvise away the entire range of pages because
      // we do not want readahead pages to be cached.
      Fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED); // free OS pages
    }
    return s;
  }

  virtual Status Skip(uint64_t n) {
    if (fseek(file_, n, SEEK_CUR)) {
      return IOError(filename_, errno);
    }
    return Status::OK();
  }

  virtual Status InvalidateCache(size_t offset, size_t length) {
#ifndef OS_LINUX
    return Status::OK();
#else
    // free OS pages
    int ret = Fadvise(fd_, offset, length, POSIX_FADV_DONTNEED);
    if (ret == 0) {
      return Status::OK();
    }
    return IOError(filename_, errno);
#endif
  }
};

// pread() based random-access
class PosixRandomAccessFile: public RandomAccessFile {
 private:
  std::string filename_;
  int fd_;
  bool use_os_buffer_;

 public:
  PosixRandomAccessFile(const std::string& fname, int fd,
                        const EnvOptions& options)
      : filename_(fname), fd_(fd), use_os_buffer_(options.use_os_buffer) {
    assert(!options.use_mmap_reads);
  }
  virtual ~PosixRandomAccessFile() { close(fd_); }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    Status s;
    ssize_t r = pread(fd_, scratch, n, static_cast<off_t>(offset));
    *result = Slice(scratch, (r < 0) ? 0 : r);
    if (r < 0) {
      // An error: return a non-ok status
      s = IOError(filename_, errno);
    }
    if (!use_os_buffer_) {
      // we need to fadvise away the entire range of pages because
      // we do not want readahead pages to be cached.
      Fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED); // free OS pages
    }
    return s;
  }

#ifdef OS_LINUX
  virtual size_t GetUniqueId(char* id, size_t max_size) const {
    return GetUniqueIdFromFile(fd_, id, max_size);
  }
#endif

  virtual void Hint(AccessPattern pattern) {
    switch(pattern) {
      case NORMAL:
        Fadvise(fd_, 0, 0, POSIX_FADV_NORMAL);
        break;
      case RANDOM:
        Fadvise(fd_, 0, 0, POSIX_FADV_RANDOM);
        break;
      case SEQUENTIAL:
        Fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
        break;
      case WILLNEED:
        Fadvise(fd_, 0, 0, POSIX_FADV_WILLNEED);
        break;
      case DONTNEED:
        Fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
        break;
      default:
        assert(false);
        break;
    }
  }

  virtual Status InvalidateCache(size_t offset, size_t length) {
#ifndef OS_LINUX
    return Status::OK();
#else
    // free OS pages
    int ret = Fadvise(fd_, offset, length, POSIX_FADV_DONTNEED);
    if (ret == 0) {
      return Status::OK();
    }
    return IOError(filename_, errno);
#endif
  }
};

// mmap() based random-access
class PosixMmapReadableFile: public RandomAccessFile {
 private:
  int fd_;
  std::string filename_;
  void* mmapped_region_;
  size_t length_;

 public:
  // base[0,length-1] contains the mmapped contents of the file.
  PosixMmapReadableFile(const int fd, const std::string& fname,
                        void* base, size_t length,
                        const EnvOptions& options)
      : fd_(fd), filename_(fname), mmapped_region_(base), length_(length) {
    fd_ = fd_ + 0;  // suppress the warning for used variables
    assert(options.use_mmap_reads);
    assert(options.use_os_buffer);
  }
  virtual ~PosixMmapReadableFile() {
    int ret = munmap(mmapped_region_, length_);
    if (ret != 0) {
      fprintf(stdout, "failed to munmap %p length %zu \n",
              mmapped_region_, length_);
    }
  }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    Status s;
    if (offset + n > length_) {
      *result = Slice();
      s = IOError(filename_, EINVAL);
    } else {
      *result = Slice(reinterpret_cast<char*>(mmapped_region_) + offset, n);
    }
    return s;
  }
  virtual Status InvalidateCache(size_t offset, size_t length) {
#ifndef OS_LINUX
    return Status::OK();
#else
    // free OS pages
    int ret = Fadvise(fd_, offset, length, POSIX_FADV_DONTNEED);
    if (ret == 0) {
      return Status::OK();
    }
    return IOError(filename_, errno);
#endif
  }
};

// We preallocate up to an extra megabyte and use memcpy to append new
// data to the file.  This is safe since we either properly close the
// file before reading from it, or for log files, the reading code
// knows enough to skip zero suffixes.
class PosixMmapFile : public WritableFile {
 private:
  std::string filename_;
  int fd_;
  size_t page_size_;
  size_t map_size_;       // How much extra memory to map at a time
  char* base_;            // The mapped region
  char* limit_;           // Limit of the mapped region
  char* dst_;             // Where to write next  (in range [base_,limit_])
  char* last_sync_;       // Where have we synced up to
  uint64_t file_offset_;  // Offset of base_ in file
  // Have we done an munmap of unsynced data?
  bool pending_sync_;
  bool fallocate_with_keep_size_;

  // Roundup x to a multiple of y
  static size_t Roundup(size_t x, size_t y) {
    return ((x + y - 1) / y) * y;
  }

  size_t TruncateToPageBoundary(size_t s) {
    s -= (s & (page_size_ - 1));
    assert((s % page_size_) == 0);
    return s;
  }

  bool UnmapCurrentRegion() {
    bool result = true;
    TEST_KILL_RANDOM(rocksdb_kill_odds);
    if (base_ != nullptr) {
      if (last_sync_ < limit_) {
        // Defer syncing this data until next Sync() call, if any
        pending_sync_ = true;
      }
      if (munmap(base_, limit_ - base_) != 0) {
        result = false;
      }
      file_offset_ += limit_ - base_;
      base_ = nullptr;
      limit_ = nullptr;
      last_sync_ = nullptr;
      dst_ = nullptr;

      // Increase the amount we map the next time, but capped at 1MB
      if (map_size_ < (1<<20)) {
        map_size_ *= 2;
      }
    }
    return result;
  }

  Status MapNewRegion() {
#ifdef ROCKSDB_FALLOCATE_PRESENT
    assert(base_ == nullptr);

    TEST_KILL_RANDOM(rocksdb_kill_odds);
    // we can't fallocate with FALLOC_FL_KEEP_SIZE here
    int alloc_status = fallocate(fd_, 0, file_offset_, map_size_);
    if (alloc_status != 0) {
      // fallback to posix_fallocate
      alloc_status = posix_fallocate(fd_, file_offset_, map_size_);
    }
    if (alloc_status != 0) {
      return Status::IOError("Error allocating space to file : " + filename_ +
        "Error : " + strerror(alloc_status));
    }

    TEST_KILL_RANDOM(rocksdb_kill_odds);
    void* ptr = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd_, file_offset_);
    if (ptr == MAP_FAILED) {
      return Status::IOError("MMap failed on " + filename_);
    }

    TEST_KILL_RANDOM(rocksdb_kill_odds);

    base_ = reinterpret_cast<char*>(ptr);
    limit_ = base_ + map_size_;
    dst_ = base_;
    last_sync_ = base_;
    return Status::OK();
#else
    return Status::NotSupported("This platform doesn't support fallocate()");
#endif
  }

 public:
  PosixMmapFile(const std::string& fname, int fd, size_t page_size,
                const EnvOptions& options)
      : filename_(fname),
        fd_(fd),
        page_size_(page_size),
        map_size_(Roundup(65536, page_size)),
        base_(nullptr),
        limit_(nullptr),
        dst_(nullptr),
        last_sync_(nullptr),
        file_offset_(0),
        pending_sync_(false),
        fallocate_with_keep_size_(options.fallocate_with_keep_size) {
    assert((page_size & (page_size - 1)) == 0);
    assert(options.use_mmap_writes);
  }


  ~PosixMmapFile() {
    if (fd_ >= 0) {
      PosixMmapFile::Close();
    }
  }

  virtual Status Append(const Slice& data) {
    const char* src = data.data();
    size_t left = data.size();
    TEST_KILL_RANDOM(rocksdb_kill_odds * REDUCE_ODDS);
    PrepareWrite(GetFileSize(), left);
    while (left > 0) {
      assert(base_ <= dst_);
      assert(dst_ <= limit_);
      size_t avail = limit_ - dst_;
      if (avail == 0) {
        if (UnmapCurrentRegion()) {
          Status s = MapNewRegion();
          if (!s.ok()) {
            return s;
          }
          TEST_KILL_RANDOM(rocksdb_kill_odds);
        }
      }

      size_t n = (left <= avail) ? left : avail;
      memcpy(dst_, src, n);
      dst_ += n;
      src += n;
      left -= n;
    }
    TEST_KILL_RANDOM(rocksdb_kill_odds);
    return Status::OK();
  }

  virtual Status Close() {
    Status s;
    size_t unused = limit_ - dst_;

    TEST_KILL_RANDOM(rocksdb_kill_odds);

    if (!UnmapCurrentRegion()) {
      s = IOError(filename_, errno);
    } else if (unused > 0) {
      // Trim the extra space at the end of the file
      if (ftruncate(fd_, file_offset_ - unused) < 0) {
        s = IOError(filename_, errno);
      }
    }

    TEST_KILL_RANDOM(rocksdb_kill_odds);

    if (close(fd_) < 0) {
      if (s.ok()) {
        s = IOError(filename_, errno);
      }
    }

    fd_ = -1;
    base_ = nullptr;
    limit_ = nullptr;
    return s;
  }

  virtual Status Flush() {
    TEST_KILL_RANDOM(rocksdb_kill_odds);
    return Status::OK();
  }

  virtual Status Sync() {
    Status s;

    if (pending_sync_) {
      // Some unmapped data was not synced
      TEST_KILL_RANDOM(rocksdb_kill_odds);
      pending_sync_ = false;
      if (fdatasync(fd_) < 0) {
        s = IOError(filename_, errno);
      }
      TEST_KILL_RANDOM(rocksdb_kill_odds * REDUCE_ODDS);
    }

    if (dst_ > last_sync_) {
      // Find the beginnings of the pages that contain the first and last
      // bytes to be synced.
      size_t p1 = TruncateToPageBoundary(last_sync_ - base_);
      size_t p2 = TruncateToPageBoundary(dst_ - base_ - 1);
      last_sync_ = dst_;
      TEST_KILL_RANDOM(rocksdb_kill_odds);
      if (msync(base_ + p1, p2 - p1 + page_size_, MS_SYNC) < 0) {
        s = IOError(filename_, errno);
      }
      TEST_KILL_RANDOM(rocksdb_kill_odds);
    }

    return s;
  }

  /**
   * Flush data as well as metadata to stable storage.
   */
  virtual Status Fsync() {
    if (pending_sync_) {
      // Some unmapped data was not synced
      TEST_KILL_RANDOM(rocksdb_kill_odds);
      pending_sync_ = false;
      if (fsync(fd_) < 0) {
        return IOError(filename_, errno);
      }
      TEST_KILL_RANDOM(rocksdb_kill_odds);
    }
    // This invocation to Sync will not issue the call to
    // fdatasync because pending_sync_ has already been cleared.
    return Sync();
  }

  /**
   * Get the size of valid data in the file. This will not match the
   * size that is returned from the filesystem because we use mmap
   * to extend file by map_size every time.
   */
  virtual uint64_t GetFileSize() {
    size_t used = dst_ - base_;
    return file_offset_ + used;
  }

  virtual Status InvalidateCache(size_t offset, size_t length) {
#ifndef OS_LINUX
    return Status::OK();
#else
    // free OS pages
    int ret = Fadvise(fd_, offset, length, POSIX_FADV_DONTNEED);
    if (ret == 0) {
      return Status::OK();
    }
    return IOError(filename_, errno);
#endif
  }

#ifdef ROCKSDB_FALLOCATE_PRESENT
  virtual Status Allocate(off_t offset, off_t len) {
    TEST_KILL_RANDOM(rocksdb_kill_odds);
    int alloc_status = fallocate(
        fd_, fallocate_with_keep_size_ ? FALLOC_FL_KEEP_SIZE : 0, offset, len);
    if (alloc_status == 0) {
      return Status::OK();
    } else {
      return IOError(filename_, errno);
    }
  }
#endif
};

// Use posix write to write data to a file.
class PosixWritableFile : public WritableFile {
 private:
  const std::string filename_;
  int fd_;
  size_t cursize_;      // current size of cached data in buf_
  size_t capacity_;     // max size of buf_
  unique_ptr<char[]> buf_;           // a buffer to cache writes
  uint64_t filesize_;
  bool pending_sync_;
  bool pending_fsync_;
  uint64_t last_sync_size_;
  uint64_t bytes_per_sync_;
  bool fallocate_with_keep_size_;

 public:
  PosixWritableFile(const std::string& fname, int fd, size_t capacity,
                    const EnvOptions& options)
      : filename_(fname),
        fd_(fd),
        cursize_(0),
        capacity_(capacity),
        buf_(new char[capacity]),
        filesize_(0),
        pending_sync_(false),
        pending_fsync_(false),
        last_sync_size_(0),
        bytes_per_sync_(options.bytes_per_sync),
        fallocate_with_keep_size_(options.fallocate_with_keep_size) {
    assert(!options.use_mmap_writes);
  }

  ~PosixWritableFile() {
    if (fd_ >= 0) {
      PosixWritableFile::Close();
    }
  }

  virtual Status Append(const Slice& data) {
    const char* src = data.data();
    size_t left = data.size();
    Status s;
    pending_sync_ = true;
    pending_fsync_ = true;

    TEST_KILL_RANDOM(rocksdb_kill_odds * REDUCE_ODDS2);

    PrepareWrite(GetFileSize(), left);
    // if there is no space in the cache, then flush
    if (cursize_ + left > capacity_) {
      s = Flush();
      if (!s.ok()) {
        return s;
      }
      // Increase the buffer size, but capped at 1MB
      if (capacity_ < (1<<20)) {
        capacity_ *= 2;
        buf_.reset(new char[capacity_]);
      }
      assert(cursize_ == 0);
    }

    // if the write fits into the cache, then write to cache
    // otherwise do a write() syscall to write to OS buffers.
    if (cursize_ + left <= capacity_) {
      memcpy(buf_.get()+cursize_, src, left);
      cursize_ += left;
    } else {
      while (left != 0) {
        ssize_t done = write(fd_, src, left);
        if (done < 0) {
          return IOError(filename_, errno);
        }
        TEST_KILL_RANDOM(rocksdb_kill_odds);

        left -= done;
        src += done;
      }
    }
    filesize_ += data.size();
    return Status::OK();
  }

  virtual Status Close() {
    Status s;
    s = Flush(); // flush cache to OS
    if (!s.ok()) {
      return s;
    }

    TEST_KILL_RANDOM(rocksdb_kill_odds);

    size_t block_size;
    size_t last_allocated_block;
    GetPreallocationStatus(&block_size, &last_allocated_block);
    if (last_allocated_block > 0) {
      // trim the extra space preallocated at the end of the file
      int dummy __attribute__((unused));
      dummy = ftruncate(fd_, filesize_);  // ignore errors
    }

    if (close(fd_) < 0) {
      if (s.ok()) {
        s = IOError(filename_, errno);
      }
    }
    fd_ = -1;
    return s;
  }

  // write out the cached data to the OS cache
  virtual Status Flush() {
    TEST_KILL_RANDOM(rocksdb_kill_odds * REDUCE_ODDS2);
    size_t left = cursize_;
    char* src = buf_.get();
    while (left != 0) {
      ssize_t done = write(fd_, src, left);
      if (done < 0) {
        return IOError(filename_, errno);
      }
      TEST_KILL_RANDOM(rocksdb_kill_odds * REDUCE_ODDS2);
      left -= done;
      src += done;
    }
    cursize_ = 0;

    // sync OS cache to disk for every bytes_per_sync_
    // TODO: give log file and sst file different options (log
    // files could be potentially cached in OS for their whole
    // life time, thus we might not want to flush at all).
    if (bytes_per_sync_ &&
        filesize_ - last_sync_size_ >= bytes_per_sync_) {
      RangeSync(last_sync_size_, filesize_ - last_sync_size_);
      last_sync_size_ = filesize_;
    }

    return Status::OK();
  }

  virtual Status Sync() {
    TEST_KILL_RANDOM(rocksdb_kill_odds);
    if (pending_sync_ && fdatasync(fd_) < 0) {
      return IOError(filename_, errno);
    }
    TEST_KILL_RANDOM(rocksdb_kill_odds);
    pending_sync_ = false;
    return Status::OK();
  }

  virtual Status Fsync() {
    TEST_KILL_RANDOM(rocksdb_kill_odds);
    if (pending_fsync_ && fsync(fd_) < 0) {
      return IOError(filename_, errno);
    }
    TEST_KILL_RANDOM(rocksdb_kill_odds);
    pending_fsync_ = false;
    pending_sync_ = false;
    return Status::OK();
  }

  virtual uint64_t GetFileSize() {
    return filesize_;
  }

  virtual Status InvalidateCache(size_t offset, size_t length) {
#ifndef OS_LINUX
    return Status::OK();
#else
    // free OS pages
    int ret = Fadvise(fd_, offset, length, POSIX_FADV_DONTNEED);
    if (ret == 0) {
      return Status::OK();
    }
    return IOError(filename_, errno);
#endif
  }

#ifdef ROCKSDB_FALLOCATE_PRESENT
  virtual Status Allocate(off_t offset, off_t len) {
    TEST_KILL_RANDOM(rocksdb_kill_odds);
    int alloc_status = fallocate(
        fd_, fallocate_with_keep_size_ ? FALLOC_FL_KEEP_SIZE : 0, offset, len);
    if (alloc_status == 0) {
      return Status::OK();
    } else {
      return IOError(filename_, errno);
    }
  }

  virtual Status RangeSync(off64_t offset, off64_t nbytes) {
    if (sync_file_range(fd_, offset, nbytes, SYNC_FILE_RANGE_WRITE) == 0) {
      return Status::OK();
    } else {
      return IOError(filename_, errno);
    }
  }
  virtual size_t GetUniqueId(char* id, size_t max_size) const {
    return GetUniqueIdFromFile(fd_, id, max_size);
  }
#endif
};

class PosixRandomRWFile : public RandomRWFile {
 private:
  const std::string filename_;
  int fd_;
  bool pending_sync_;
  bool pending_fsync_;
  bool fallocate_with_keep_size_;

 public:
  PosixRandomRWFile(const std::string& fname, int fd, const EnvOptions& options)
      : filename_(fname),
        fd_(fd),
        pending_sync_(false),
        pending_fsync_(false),
        fallocate_with_keep_size_(options.fallocate_with_keep_size) {
    assert(!options.use_mmap_writes && !options.use_mmap_reads);
  }

  ~PosixRandomRWFile() {
    if (fd_ >= 0) {
      Close();
    }
  }

  virtual Status Write(uint64_t offset, const Slice& data) {
    const char* src = data.data();
    size_t left = data.size();
    Status s;
    pending_sync_ = true;
    pending_fsync_ = true;

    while (left != 0) {
      ssize_t done = pwrite(fd_, src, left, offset);
      if (done < 0) {
        return IOError(filename_, errno);
      }

      left -= done;
      src += done;
      offset += done;
    }

    return Status::OK();
  }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    Status s;
    ssize_t r = pread(fd_, scratch, n, static_cast<off_t>(offset));
    *result = Slice(scratch, (r < 0) ? 0 : r);
    if (r < 0) {
      s = IOError(filename_, errno);
    }
    return s;
  }

  virtual Status Close() {
    Status s = Status::OK();
    if (fd_ >= 0 && close(fd_) < 0) {
      s = IOError(filename_, errno);
    }
    fd_ = -1;
    return s;
  }

  virtual Status Sync() {
    if (pending_sync_ && fdatasync(fd_) < 0) {
      return IOError(filename_, errno);
    }
    pending_sync_ = false;
    return Status::OK();
  }

  virtual Status Fsync() {
    if (pending_fsync_ && fsync(fd_) < 0) {
      return IOError(filename_, errno);
    }
    pending_fsync_ = false;
    pending_sync_ = false;
    return Status::OK();
  }

#ifdef ROCKSDB_FALLOCATE_PRESENT
  virtual Status Allocate(off_t offset, off_t len) {
    TEST_KILL_RANDOM(rocksdb_kill_odds);
    int alloc_status = fallocate(
        fd_, fallocate_with_keep_size_ ? FALLOC_FL_KEEP_SIZE : 0, offset, len);
    if (alloc_status == 0) {
      return Status::OK();
    } else {
      return IOError(filename_, errno);
    }
  }
#endif
};

class PosixDirectory : public Directory {
 public:
  explicit PosixDirectory(int fd) : fd_(fd) {}
  ~PosixDirectory() {
    close(fd_);
  }

  virtual Status Fsync() {
    if (fsync(fd_) == -1) {
      return IOError("directory", errno);
    }
    return Status::OK();
  }

 private:
  int fd_;
};

static int LockOrUnlock(const std::string& fname, int fd, bool lock) {
  mutex_lockedFiles.Lock();
  if (lock) {
    // If it already exists in the lockedFiles set, then it is already locked,
    // and fail this lock attempt. Otherwise, insert it into lockedFiles.
    // This check is needed because fcntl() does not detect lock conflict
    // if the fcntl is issued by the same thread that earlier acquired
    // this lock.
    if (lockedFiles.insert(fname).second == false) {
      mutex_lockedFiles.Unlock();
      errno = ENOLCK;
      return -1;
    }
  } else {
    // If we are unlocking, then verify that we had locked it earlier,
    // it should already exist in lockedFiles. Remove it from lockedFiles.
    if (lockedFiles.erase(fname) != 1) {
      mutex_lockedFiles.Unlock();
      errno = ENOLCK;
      return -1;
    }
  }
  errno = 0;
  struct flock f;
  memset(&f, 0, sizeof(f));
  f.l_type = (lock ? F_WRLCK : F_UNLCK);
  f.l_whence = SEEK_SET;
  f.l_start = 0;
  f.l_len = 0;        // Lock/unlock entire file
  int value = fcntl(fd, F_SETLK, &f);
  if (value == -1 && lock) {
    // if there is an error in locking, then remove the pathname from lockedfiles
    lockedFiles.erase(fname);
  }
  mutex_lockedFiles.Unlock();
  return value;
}

class PosixFileLock : public FileLock {
 public:
  int fd_;
  std::string filename;
};


namespace {
void PthreadCall(const char* label, int result) {
  if (result != 0) {
    fprintf(stderr, "pthread %s: %s\n", label, strerror(result));
    exit(1);
  }
}
}

class PosixEnv : public Env {
 public:
  PosixEnv();

  virtual ~PosixEnv(){
    for (const auto tid : threads_to_join_) {
      pthread_join(tid, nullptr);
    }
  }

  void SetFD_CLOEXEC(int fd, const EnvOptions* options) {
    if ((options == nullptr || options->set_fd_cloexec) && fd > 0) {
      fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }
  }

  virtual Status NewSequentialFile(const std::string& fname,
                                   unique_ptr<SequentialFile>* result,
                                   const EnvOptions& options) {
    result->reset();
    FILE* f = fopen(fname.c_str(), "r");
    if (f == nullptr) {
      *result = nullptr;
      return IOError(fname, errno);
    } else {
      int fd = fileno(f);
      SetFD_CLOEXEC(fd, &options);
      result->reset(new PosixSequentialFile(fname, f, options));
      return Status::OK();
    }
  }

  virtual Status NewRandomAccessFile(const std::string& fname,
                                     unique_ptr<RandomAccessFile>* result,
                                     const EnvOptions& options) {
    result->reset();
    Status s;
    int fd = open(fname.c_str(), O_RDONLY);
    SetFD_CLOEXEC(fd, &options);
    if (fd < 0) {
      s = IOError(fname, errno);
    } else if (options.use_mmap_reads && sizeof(void*) >= 8) {
      // Use of mmap for random reads has been removed because it
      // kills performance when storage is fast.
      // Use mmap when virtual address-space is plentiful.
      uint64_t size;
      s = GetFileSize(fname, &size);
      if (s.ok()) {
        void* base = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (base != MAP_FAILED) {
          result->reset(new PosixMmapReadableFile(fd, fname, base,
                                                  size, options));
        } else {
          s = IOError(fname, errno);
        }
      }
      close(fd);
    } else {
      result->reset(new PosixRandomAccessFile(fname, fd, options));
    }
    return s;
  }

  virtual Status NewWritableFile(const std::string& fname,
                                 unique_ptr<WritableFile>* result,
                                 const EnvOptions& options) {
    result->reset();
    Status s;
    const int fd = open(fname.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
      s = IOError(fname, errno);
    } else {
      SetFD_CLOEXEC(fd, &options);
      if (options.use_mmap_writes) {
        if (!checkedDiskForMmap_) {
          // this will be executed once in the program's lifetime.
          // do not use mmapWrite on non ext-3/xfs/tmpfs systems.
          if (!SupportsFastAllocate(fname)) {
            forceMmapOff = true;
          }
          checkedDiskForMmap_ = true;
        }
      }
      if (options.use_mmap_writes && !forceMmapOff) {
        result->reset(new PosixMmapFile(fname, fd, page_size_, options));
      } else {
        // disable mmap writes
        EnvOptions no_mmap_writes_options = options;
        no_mmap_writes_options.use_mmap_writes = false;

        result->reset(
            new PosixWritableFile(fname, fd, 65536, no_mmap_writes_options)
        );
      }
    }
    return s;
  }

  virtual Status NewRandomRWFile(const std::string& fname,
                                 unique_ptr<RandomRWFile>* result,
                                 const EnvOptions& options) {
    result->reset();
    // no support for mmap yet
    if (options.use_mmap_writes || options.use_mmap_reads) {
      return Status::NotSupported("No support for mmap read/write yet");
    }
    Status s;
    const int fd = open(fname.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
      s = IOError(fname, errno);
    } else {
      SetFD_CLOEXEC(fd, &options);
      result->reset(new PosixRandomRWFile(fname, fd, options));
    }
    return s;
  }

  virtual Status NewDirectory(const std::string& name,
                              unique_ptr<Directory>* result) {
    result->reset();
    const int fd = open(name.c_str(), 0);
    if (fd < 0) {
      return IOError(name, errno);
    } else {
      result->reset(new PosixDirectory(fd));
    }
    return Status::OK();
  }

  virtual bool FileExists(const std::string& fname) {
    return access(fname.c_str(), F_OK) == 0;
  }

  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) {
    result->clear();
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
      return IOError(dir, errno);
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
      result->push_back(entry->d_name);
    }
    closedir(d);
    return Status::OK();
  }

  virtual Status DeleteFile(const std::string& fname) {
    Status result;
    if (unlink(fname.c_str()) != 0) {
      result = IOError(fname, errno);
    }
    return result;
  };

  virtual Status CreateDir(const std::string& name) {
    Status result;
    if (mkdir(name.c_str(), 0755) != 0) {
      result = IOError(name, errno);
    }
    return result;
  };

  virtual Status CreateDirIfMissing(const std::string& name) {
    Status result;
    if (mkdir(name.c_str(), 0755) != 0) {
      if (errno != EEXIST) {
        result = IOError(name, errno);
      } else if (!DirExists(name)) { // Check that name is actually a
                                     // directory.
        // Message is taken from mkdir
        result = Status::IOError("`"+name+"' exists but is not a directory");
      }
    }
    return result;
  };

  virtual Status DeleteDir(const std::string& name) {
    Status result;
    if (rmdir(name.c_str()) != 0) {
      result = IOError(name, errno);
    }
    return result;
  };

  virtual Status GetFileSize(const std::string& fname, uint64_t* size) {
    Status s;
    struct stat sbuf;
    if (stat(fname.c_str(), &sbuf) != 0) {
      *size = 0;
      s = IOError(fname, errno);
    } else {
      *size = sbuf.st_size;
    }
    return s;
  }

  virtual Status GetFileModificationTime(const std::string& fname,
                                         uint64_t* file_mtime) {
    struct stat s;
    if (stat(fname.c_str(), &s) !=0) {
      return IOError(fname, errno);
    }
    *file_mtime = static_cast<uint64_t>(s.st_mtime);
    return Status::OK();
  }
  virtual Status RenameFile(const std::string& src, const std::string& target) {
    Status result;
    if (rename(src.c_str(), target.c_str()) != 0) {
      result = IOError(src, errno);
    }
    return result;
  }

  virtual Status LockFile(const std::string& fname, FileLock** lock) {
    *lock = nullptr;
    Status result;
    int fd = open(fname.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
      result = IOError(fname, errno);
    } else if (LockOrUnlock(fname, fd, true) == -1) {
      result = IOError("lock " + fname, errno);
      close(fd);
    } else {
      SetFD_CLOEXEC(fd, nullptr);
      PosixFileLock* my_lock = new PosixFileLock;
      my_lock->fd_ = fd;
      my_lock->filename = fname;
      *lock = my_lock;
    }
    return result;
  }

  virtual Status UnlockFile(FileLock* lock) {
    PosixFileLock* my_lock = reinterpret_cast<PosixFileLock*>(lock);
    Status result;
    if (LockOrUnlock(my_lock->filename, my_lock->fd_, false) == -1) {
      result = IOError("unlock", errno);
    }
    close(my_lock->fd_);
    delete my_lock;
    return result;
  }

  virtual void Schedule(void (*function)(void*), void* arg, Priority pri = LOW);

  virtual void StartThread(void (*function)(void* arg), void* arg);

  virtual void WaitForJoin();

  virtual unsigned int GetThreadPoolQueueLen(Priority pri = LOW) const override;

  virtual Status GetTestDirectory(std::string* result) {
    const char* env = getenv("TEST_TMPDIR");
    if (env && env[0] != '\0') {
      *result = env;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "/tmp/rocksdbtest-%d", int(geteuid()));
      *result = buf;
    }
    // Directory may already exist
    CreateDir(*result);
    return Status::OK();
  }

  static uint64_t gettid() {
    pthread_t tid = pthread_self();
    uint64_t thread_id = 0;
    memcpy(&thread_id, &tid, std::min(sizeof(thread_id), sizeof(tid)));
    return thread_id;
  }

  virtual Status NewLogger(const std::string& fname,
                           shared_ptr<Logger>* result) {
    FILE* f = fopen(fname.c_str(), "w");
    if (f == nullptr) {
      result->reset();
      return IOError(fname, errno);
    } else {
      int fd = fileno(f);
      SetFD_CLOEXEC(fd, nullptr);
      result->reset(new PosixLogger(f, &PosixEnv::gettid, this));
      return Status::OK();
    }
  }

  virtual uint64_t NowMicros() {
    struct timeval tv;
    // TODO(kailiu) MAC DON'T HAVE THIS
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  }

  virtual uint64_t NowNanos() {
#ifdef OS_LINUX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#elif __MACH__
    clock_serv_t cclock;
    mach_timespec_t ts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &ts);
    mach_port_deallocate(mach_task_self(), cclock);
#endif
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
  }

  virtual void SleepForMicroseconds(int micros) {
    usleep(micros);
  }

  virtual Status GetHostName(char* name, uint64_t len) {
    int ret = gethostname(name, len);
    if (ret < 0) {
      if (errno == EFAULT || errno == EINVAL)
        return Status::InvalidArgument(strerror(errno));
      else
        return IOError("GetHostName", errno);
    }
    return Status::OK();
  }

  virtual Status GetCurrentTime(int64_t* unix_time) {
    time_t ret = time(nullptr);
    if (ret == (time_t) -1) {
      return IOError("GetCurrentTime", errno);
    }
    *unix_time = (int64_t) ret;
    return Status::OK();
  }

  virtual Status GetAbsolutePath(const std::string& db_path,
      std::string* output_path) {
    if (db_path.find('/') == 0) {
      *output_path = db_path;
      return Status::OK();
    }

    char the_path[256];
    char* ret = getcwd(the_path, 256);
    if (ret == nullptr) {
      return Status::IOError(strerror(errno));
    }

    *output_path = ret;
    return Status::OK();
  }

  // Allow increasing the number of worker threads.
  virtual void SetBackgroundThreads(int num, Priority pri) {
    assert(pri >= Priority::LOW && pri <= Priority::HIGH);
    thread_pools_[pri].SetBackgroundThreads(num);
  }

  virtual std::string TimeToString(uint64_t secondsSince1970) {
    const time_t seconds = (time_t)secondsSince1970;
    struct tm t;
    int maxsize = 64;
    std::string dummy;
    dummy.reserve(maxsize);
    dummy.resize(maxsize);
    char* p = &dummy[0];
    localtime_r(&seconds, &t);
    snprintf(p, maxsize,
             "%04d/%02d/%02d-%02d:%02d:%02d ",
             t.tm_year + 1900,
             t.tm_mon + 1,
             t.tm_mday,
             t.tm_hour,
             t.tm_min,
             t.tm_sec);
    return dummy;
  }

  EnvOptions OptimizeForLogWrite(const EnvOptions& env_options) const {
    EnvOptions optimized = env_options;
    optimized.use_mmap_writes = false;
    optimized.fallocate_with_keep_size = true;
    return optimized;
  }

  EnvOptions OptimizeForManifestWrite(const EnvOptions& env_options) const {
    EnvOptions optimized = env_options;
    optimized.use_mmap_writes = false;
    optimized.fallocate_with_keep_size = true;
    return optimized;
  }

 private:
  bool checkedDiskForMmap_;
  bool forceMmapOff; // do we override Env options?


  // Returns true iff the named directory exists and is a directory.
  virtual bool DirExists(const std::string& dname) {
    struct stat statbuf;
    if (stat(dname.c_str(), &statbuf) == 0) {
      return S_ISDIR(statbuf.st_mode);
    }
    return false; // stat() failed return false
  }

  bool SupportsFastAllocate(const std::string& path) {
#ifdef ROCKSDB_FALLOCATE_PRESENT
    struct statfs s;
    if (statfs(path.c_str(), &s)){
      return false;
    }
    switch (s.f_type) {
      case EXT4_SUPER_MAGIC:
        return true;
      case XFS_SUPER_MAGIC:
        return true;
      case TMPFS_MAGIC:
        return true;
      default:
        return false;
    }
#else
    return false;
#endif
  }

  size_t page_size_;


  class ThreadPool {
   public:
    ThreadPool()
        : total_threads_limit_(1),
          bgthreads_(0),
          queue_(),
          queue_len_(0),
          exit_all_threads_(false) {
      PthreadCall("mutex_init", pthread_mutex_init(&mu_, nullptr));
      PthreadCall("cvar_init", pthread_cond_init(&bgsignal_, nullptr));
    }

    ~ThreadPool() {
      PthreadCall("lock", pthread_mutex_lock(&mu_));
      assert(!exit_all_threads_);
      exit_all_threads_ = true;
      PthreadCall("signalall", pthread_cond_broadcast(&bgsignal_));
      PthreadCall("unlock", pthread_mutex_unlock(&mu_));
      for (const auto tid : bgthreads_) {
        pthread_join(tid, nullptr);
      }
    }

    void BGThread() {
      while (true) {
        // Wait until there is an item that is ready to run
        PthreadCall("lock", pthread_mutex_lock(&mu_));
        while (queue_.empty() && !exit_all_threads_) {
          PthreadCall("wait", pthread_cond_wait(&bgsignal_, &mu_));
        }
        if (exit_all_threads_) { // mechanism to let BG threads exit safely
          PthreadCall("unlock", pthread_mutex_unlock(&mu_));
          break;
        }
        void (*function)(void*) = queue_.front().function;
        void* arg = queue_.front().arg;
        queue_.pop_front();
        queue_len_.store(queue_.size(), std::memory_order_relaxed);

        PthreadCall("unlock", pthread_mutex_unlock(&mu_));
        (*function)(arg);
      }
    }

    static void* BGThreadWrapper(void* arg) {
      reinterpret_cast<ThreadPool*>(arg)->BGThread();
      return nullptr;
    }

    void SetBackgroundThreads(int num) {
      PthreadCall("lock", pthread_mutex_lock(&mu_));
      if (num > total_threads_limit_) {
        total_threads_limit_ = num;
      }
      assert(total_threads_limit_ > 0);
      PthreadCall("unlock", pthread_mutex_unlock(&mu_));
    }

    void Schedule(void (*function)(void*), void* arg) {
      PthreadCall("lock", pthread_mutex_lock(&mu_));

      if (exit_all_threads_) {
        PthreadCall("unlock", pthread_mutex_unlock(&mu_));
        return;
      }
      // Start background thread if necessary
      while ((int)bgthreads_.size() < total_threads_limit_) {
        pthread_t t;
        PthreadCall(
          "create thread",
          pthread_create(&t,
                         nullptr,
                         &ThreadPool::BGThreadWrapper,
                         this));

        // Set the thread name to aid debugging
#if defined(_GNU_SOURCE) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 12)
        char name_buf[16];
        snprintf(name_buf, sizeof name_buf, "rocksdb:bg%zu", bgthreads_.size());
        name_buf[sizeof name_buf - 1] = '\0';
        pthread_setname_np(t, name_buf);
#endif
#endif

        bgthreads_.push_back(t);
      }

      // Add to priority queue
      queue_.push_back(BGItem());
      queue_.back().function = function;
      queue_.back().arg = arg;
      queue_len_.store(queue_.size(), std::memory_order_relaxed);

      // always wake up at least one waiting thread.
      PthreadCall("signal", pthread_cond_signal(&bgsignal_));

      PthreadCall("unlock", pthread_mutex_unlock(&mu_));
    }

    unsigned int GetQueueLen() const {
      return queue_len_.load(std::memory_order_relaxed);
    }

   private:
    // Entry per Schedule() call
    struct BGItem { void* arg; void (*function)(void*); };
    typedef std::deque<BGItem> BGQueue;

    pthread_mutex_t mu_;
    pthread_cond_t bgsignal_;
    int total_threads_limit_;
    std::vector<pthread_t> bgthreads_;
    BGQueue queue_;
    std::atomic_uint queue_len_;  // Queue length. Used for stats reporting
    bool exit_all_threads_;
  };

  std::vector<ThreadPool> thread_pools_;

  pthread_mutex_t mu_;
  std::vector<pthread_t> threads_to_join_;

};

PosixEnv::PosixEnv() : checkedDiskForMmap_(false),
                       forceMmapOff(false),
                       page_size_(getpagesize()),
                       thread_pools_(Priority::TOTAL) {
  PthreadCall("mutex_init", pthread_mutex_init(&mu_, nullptr));
}

void PosixEnv::Schedule(void (*function)(void*), void* arg, Priority pri) {
  assert(pri >= Priority::LOW && pri <= Priority::HIGH);
  thread_pools_[pri].Schedule(function, arg);
}

unsigned int PosixEnv::GetThreadPoolQueueLen(Priority pri) const {
  assert(pri >= Priority::LOW && pri <= Priority::HIGH);
  return thread_pools_[pri].GetQueueLen();
}

namespace {
struct StartThreadState {
  void (*user_function)(void*);
  void* arg;
};
}
static void* StartThreadWrapper(void* arg) {
  StartThreadState* state = reinterpret_cast<StartThreadState*>(arg);
  state->user_function(state->arg);
  delete state;
  return nullptr;
}

void PosixEnv::StartThread(void (*function)(void* arg), void* arg) {
  pthread_t t;
  StartThreadState* state = new StartThreadState;
  state->user_function = function;
  state->arg = arg;
  PthreadCall("start thread",
              pthread_create(&t, nullptr,  &StartThreadWrapper, state));
  PthreadCall("lock", pthread_mutex_lock(&mu_));
  threads_to_join_.push_back(t);
  PthreadCall("unlock", pthread_mutex_unlock(&mu_));
}

void PosixEnv::WaitForJoin() {
  for (const auto tid : threads_to_join_) {
    pthread_join(tid, nullptr);
  }
  threads_to_join_.clear();
}

}  // namespace

std::string Env::GenerateUniqueId() {
  std::string uuid_file = "/proc/sys/kernel/random/uuid";
  if (FileExists(uuid_file)) {
    std::string uuid;
    Status s = ReadFileToString(this, uuid_file, &uuid);
    if (s.ok()) {
      return uuid;
    }
  }
  // Could not read uuid_file - generate uuid using "nanos-random"
  Random64 r(time(nullptr));
  uint64_t random_uuid_portion =
    r.Uniform(std::numeric_limits<uint64_t>::max());
  uint64_t nanos_uuid_portion = NowNanos();
  char uuid2[200];
  snprintf(uuid2,
           200,
           "%lx-%lx",
           (unsigned long)nanos_uuid_portion,
           (unsigned long)random_uuid_portion);
  return uuid2;
}

Env* Env::Default() {
  static PosixEnv default_env;
  return &default_env;
}

}  // namespace rocksdb
