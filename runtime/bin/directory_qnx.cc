// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "platform/globals.h"
#if defined(DART_HOST_OS_QNX)

#include "bin/directory.h"

#include <dirent.h>     // NOLINT
#include <errno.h>      // NOLINT
#include <fcntl.h>      // NOLINT
#include <stdlib.h>     // NOLINT
#include <string.h>     // NOLINT
#include <sys/param.h>  // NOLINT
#include <sys/stat.h>   // NOLINT
#include <unistd.h>     // NOLINT

#include "bin/crypto.h"
#include "bin/dartutils.h"
#include "bin/fdutils.h"
#include "bin/file.h"
#include "bin/namespace.h"
#include "bin/platform.h"
#include "platform/signal_blocker.h"

namespace dart {
namespace bin {

PathBuffer::PathBuffer() : length_(0) {
  data_ = calloc(PATH_MAX + 1, sizeof(char));  // NOLINT
}

PathBuffer::~PathBuffer() {
  free(data_);
}

bool PathBuffer::AddW(const wchar_t* name) {
  UNREACHABLE();
  return false;
}

char* PathBuffer::AsString() const {
  return reinterpret_cast<char*>(data_);
}

wchar_t* PathBuffer::AsStringW() const {
  UNREACHABLE();
  return nullptr;
}

const char* PathBuffer::AsScopedString() const {
  return DartUtils::ScopedCopyCString(AsString());
}

bool PathBuffer::Add(const char* name) {
  char* data = AsString();
  int written = snprintf(data + length_, PATH_MAX - length_, "%s", name);
  data[PATH_MAX] = '\0';
  if ((written <= PATH_MAX - length_) && (written >= 0) &&
      (static_cast<size_t>(written) == strnlen(name, PATH_MAX + 1))) {
    length_ += written;
    return true;
  } else {
    errno = ENAMETOOLONG;
    return false;
  }
}

void PathBuffer::Reset(intptr_t new_length) {
  length_ = new_length;
  AsString()[length_] = '\0';
}

// A linked list of symbolic links, with their unique file system identifiers.
// These are scanned to detect loops while doing a recursive directory listing.
struct LinkList {
  decltype(stat64::st_dev) dev;
  ino64_t ino;
  LinkList* next;
};

enum class EntryType {
  kUnknown,
  kRegular,
  kDirectory,
  kCharacter,
  kBlock,
  kFIFO,
  kLink,
  kSocket,
};

static EntryType GetDirectoryEntryType(DIR* entry,
                                       const char* name,
                                       bool follow_symlinks) {
  auto fd = ::dirfd(entry);
  if (fd == -1) {
    return EntryType::kUnknown;
  }

  struct stat statbuf = {};
  int stat_flags = {};
  if (!follow_symlinks) {
    stat_flags |= AT_SYMLINK_NOFOLLOW;
  }
  if (::fstatat(fd, name, &statbuf, stat_flags) != 0) {
    return EntryType::kUnknown;
  }
  if (S_ISREG(statbuf.st_mode)) {
    return EntryType::kRegular;
  } else if (S_ISDIR(statbuf.st_mode)) {
    return EntryType::kDirectory;
  } else if (S_ISCHR(statbuf.st_mode)) {
    return EntryType::kCharacter;
  } else if (S_ISBLK(statbuf.st_mode)) {
    return EntryType::kBlock;
  } else if (S_ISFIFO(statbuf.st_mode)) {
    return EntryType::kFIFO;
  } else if (S_ISLNK(statbuf.st_mode)) {
    return EntryType::kLink;
  } else if (S_ISSOCK(statbuf.st_mode)) {
    return EntryType::kSocket;
  }
  return EntryType::kUnknown;
}

ListType DirectoryListingEntry::Next(DirectoryListing* listing) {
  if (done_) {
    return kListDone;
  }

  if (fd_ == -1) {
    ASSERT(lister_ == 0);
    NamespaceScope ns(listing->namespc(), listing->path_buffer().AsString());
    const int listingfd =
        TEMP_FAILURE_RETRY(openat(ns.fd(), ns.path(), O_DIRECTORY));
    if (listingfd < 0) {
      done_ = true;
      return kListError;
    }
    fd_ = listingfd;
  }

  if (lister_ == 0) {
    do {
      lister_ = reinterpret_cast<intptr_t>(fdopendir(fd_));
    } while ((lister_ == 0) && (errno == EINTR));
    if (lister_ == 0) {
      done_ = true;
      return kListError;
    }
    if (parent_ != nullptr) {
      if (!listing->path_buffer().Add(File::PathSeparator())) {
        return kListError;
      }
    }
    path_length_ = listing->path_buffer().length();
  }
  // Reset.
  listing->path_buffer().Reset(path_length_);
  ResetLink();

  // Iterate the directory and post the directories and files to the
  // ports.
  errno = 0;
  auto lister_dir = reinterpret_cast<DIR*>(lister_);
  dirent* entry = readdir(lister_dir);
  if (entry != nullptr) {
    if (!listing->path_buffer().Add(entry->d_name)) {
      done_ = true;
      return kListError;
    }
    switch (GetDirectoryEntryType(lister_dir,              //
                                  entry->d_name,           //
                                  listing->follow_links()  //
                                  )) {
      case EntryType::kDirectory:
        if ((strcmp(entry->d_name, ".") == 0) ||
            (strcmp(entry->d_name, "..") == 0)) {
          return Next(listing);
        }
        return kListDirectory;
      case EntryType::kBlock:
      case EntryType::kCharacter:
      case EntryType::kFIFO:
      case EntryType::kSocket:
        // Suspicious, but OK.
      case EntryType::kRegular:
        return kListFile;
      case EntryType::kLink:
        return kListLink;
      default:
        // We should have covered all the bases. If not, let's get an error.
        FATAL("Unexpected listing type");
        return kListError;
    }
  }
  done_ = true;

  if (errno != 0) {
    return kListError;
  }

  return kListDone;
}

DirectoryListingEntry::~DirectoryListingEntry() {
  ResetLink();
  if (lister_ != 0) {
    // This also closes fd_.
    VOID_NO_RETRY_EXPECTED(closedir(reinterpret_cast<DIR*>(lister_)));
  }
}

void DirectoryListingEntry::ResetLink() {
  if ((link_ != nullptr) &&
      ((parent_ == nullptr) || (parent_->link_ != link_))) {
    delete link_;
    link_ = nullptr;
  }
  if (parent_ != nullptr) {
    link_ = parent_->link_;
  }
}

static bool DeleteRecursively(int dirfd, PathBuffer* path);

static bool DeleteFile(int dirfd, char* file_name, PathBuffer* path) {
  return path->Add(file_name) &&
         (NO_RETRY_EXPECTED(unlinkat(dirfd, path->AsString(), 0)) == 0);
}

static bool DeleteDir(int dirfd, char* dir_name, PathBuffer* path) {
  if ((strcmp(dir_name, ".") == 0) || (strcmp(dir_name, "..") == 0)) {
    return true;
  }
  return path->Add(dir_name) && DeleteRecursively(dirfd, path);
}

static bool DeleteRecursively(int dirfd, PathBuffer* path) {
  // Do not recurse into links for deletion. Instead delete the link.
  // If it's a file, delete it.
  struct stat64 st;
  if (TEMP_FAILURE_RETRY(
          fstatat64(dirfd, path->AsString(), &st, AT_SYMLINK_NOFOLLOW)) == -1) {
    return false;
  } else if (!S_ISDIR(st.st_mode)) {
    return (NO_RETRY_EXPECTED(unlinkat(dirfd, path->AsString(), 0)) == 0);
  }

  if (!path->Add(File::PathSeparator())) {
    return false;
  }

  // Not a link. Attempt to open as a directory and recurse into the
  // directory.
  const int fd =
      TEMP_FAILURE_RETRY(openat(dirfd, path->AsString(), O_DIRECTORY));
  if (fd < 0) {
    return false;
  }
  DIR* dir_pointer;
  do {
    dir_pointer = fdopendir(fd);
  } while ((dir_pointer == nullptr) && (errno == EINTR));
  if (dir_pointer == nullptr) {
    FDUtils::SaveErrorAndClose(fd);
    return false;
  }

  // Iterate the directory and delete all files and directories.
  int path_length = path->length();
  while (true) {
    // In case `readdir()` returns `nullptr` we distinguish between
    // end-of-stream and error by looking if `errno` was updated.
    errno = 0;
    // In glibc 2.24+, readdir_r is deprecated.
    // According to the man page for readdir:
    // "readdir(3) is not required to be thread-safe. However, in modern
    // implementations (including the glibc implementation), concurrent calls to
    // readdir(3) that specify different directory streams are thread-safe."
    dirent* entry = readdir(dir_pointer);
    if (entry == nullptr) {
      // Failed to read next directory entry.
      if (errno != 0) {
        break;
      }
      // End of directory.
      int status = NO_RETRY_EXPECTED(closedir(dir_pointer));
      if (status != 0) {
        return false;
      }
      status =
          NO_RETRY_EXPECTED(unlinkat(dirfd, path->AsString(), AT_REMOVEDIR));
      return status == 0;
    }
    bool ok = false;
    switch (GetDirectoryEntryType(dir_pointer, entry->d_name, false)) {
      case EntryType::kDirectory:
        ok = DeleteDir(dirfd, entry->d_name, path);
        break;
      case EntryType::kBlock:
      case EntryType::kCharacter:
      case EntryType::kFIFO:
      case EntryType::kSocket:
      case EntryType::kRegular:
      case EntryType::kLink:
        // Treat all links as files. This will delete the link which
        // is what we want no matter if the link target is a file or a
        // directory.
        ok = DeleteFile(dirfd, entry->d_name, path);
        break;
      case EntryType::kUnknown:
      default:
        // We should have covered all the bases. If not, let's get an error.
        FATAL("Unexpected file type to delete\n");
        break;
    }
    if (!ok) {
      break;
    }
    path->Reset(path_length);
  }
  // Only happens if an error.
  ASSERT(errno != 0);
  int err = errno;
  VOID_NO_RETRY_EXPECTED(closedir(dir_pointer));
  errno = err;
  return false;
}

Directory::ExistsResult Directory::Exists(Namespace* namespc,
                                          const char* dir_name) {
  NamespaceScope ns(namespc, dir_name);
  struct stat64 entry_info;
  int success =
      TEMP_FAILURE_RETRY(fstatat64(ns.fd(), ns.path(), &entry_info, 0));
  if (success == 0) {
    if (S_ISDIR(entry_info.st_mode)) {
      return EXISTS;
    } else {
      // An OSError may be constructed based on the return value of this
      // function, so set errno to something that makes sense.
      errno = ENOTDIR;
      return DOES_NOT_EXIST;
    }
  } else {
    if ((errno == EACCES) || (errno == EBADF) || (errno == EFAULT) ||
        (errno == ENOMEM) || (errno == EOVERFLOW)) {
      // Search permissions denied for one of the directories in the
      // path or a low level error occurred. We do not know if the
      // directory exists.
      return UNKNOWN;
    }
    ASSERT((errno == ELOOP) || (errno == ENAMETOOLONG) || (errno == ENOENT) ||
           (errno == ENOTDIR));
    return DOES_NOT_EXIST;
  }
}

char* Directory::CurrentNoScope() {
  return getcwd(nullptr, 0);
}

bool Directory::Create(Namespace* namespc, const char* dir_name) {
  NamespaceScope ns(namespc, dir_name);
  // Create the directory with the permissions specified by the
  // process umask.
  const int result = NO_RETRY_EXPECTED(mkdirat(ns.fd(), ns.path(), 0777));
  // If the directory already exists, treat it as a success.
  if ((result == -1) && (errno == EEXIST)) {
    return (Exists(namespc, dir_name) == EXISTS);
  }
  return (result == 0);
}

const char* Directory::SystemTemp(Namespace* namespc) {
  if (Directory::system_temp_path_override_ != nullptr) {
    return DartUtils::ScopedCopyCString(Directory::system_temp_path_override_);
  }

  PathBuffer path;
  const char* temp_dir = getenv("TMPDIR");
  if (temp_dir == nullptr) {
    temp_dir = getenv("TMP");
  }
  if (temp_dir == nullptr) {
    temp_dir = "/tmp";
  }
  NamespaceScope ns(namespc, temp_dir);
  if (!path.Add(ns.path())) {
    return nullptr;
  }

  // Remove any trailing slash.
  char* result = path.AsString();
  int length = strlen(result);
  if ((length > 1) && (result[length - 1] == '/')) {
    result[length - 1] = '\0';
  }
  return path.AsScopedString();
}

// Returns a new, unused directory name, adding characters to the end
// of prefix.  Creates the directory with the permissions specified
// by the process umask.
// The return value is Dart_ScopeAllocated.
const char* Directory::CreateTemp(Namespace* namespc, const char* prefix) {
  PathBuffer path;
  const int firstchar = 'A';
  const int numchars = 'Z' - 'A' + 1;
  uint8_t random_bytes[7];

  // mkdtemp doesn't have an "at" variant, so we have to simulate it.
  if (!path.Add(prefix)) {
    return nullptr;
  }
  intptr_t prefix_length = path.length();
  while (true) {
    Crypto::GetRandomBytes(6, random_bytes);
    for (intptr_t i = 0; i < 6; i++) {
      random_bytes[i] = (random_bytes[i] % numchars) + firstchar;
    }
    random_bytes[6] = '\0';
    if (!path.Add(reinterpret_cast<char*>(random_bytes))) {
      return nullptr;
    }
    NamespaceScope ns(namespc, path.AsString());
    const int result = NO_RETRY_EXPECTED(mkdirat(ns.fd(), ns.path(), 0777));
    if (result == 0) {
      return path.AsScopedString();
    } else if (errno == EEXIST) {
      path.Reset(prefix_length);
    } else {
      return nullptr;
    }
  }
}

bool Directory::Delete(Namespace* namespc,
                       const char* dir_name,
                       bool recursive) {
  NamespaceScope ns(namespc, dir_name);
  if (!recursive) {
    if ((File::GetType(namespc, dir_name, false) == File::kIsLink) &&
        (File::GetType(namespc, dir_name, true) == File::kIsDirectory)) {
      return NO_RETRY_EXPECTED(unlinkat(ns.fd(), ns.path(), 0)) == 0;
    }
    return NO_RETRY_EXPECTED(unlinkat(ns.fd(), ns.path(), AT_REMOVEDIR)) == 0;
  } else {
    PathBuffer path;
    if (!path.Add(ns.path())) {
      return false;
    }
    return DeleteRecursively(ns.fd(), &path);
  }
}

bool Directory::Rename(Namespace* namespc,
                       const char* old_path,
                       const char* new_path) {
  ExistsResult exists = Exists(namespc, old_path);
  if (exists != EXISTS) {
    return false;
  }
  NamespaceScope oldns(namespc, old_path);
  NamespaceScope newns(namespc, new_path);
  return (NO_RETRY_EXPECTED(renameat(oldns.fd(), oldns.path(), newns.fd(),
                                     newns.path())) == 0);
}

}  // namespace bin
}  // namespace dart

#endif  // defined(DART_HOST_OS_QNX)
