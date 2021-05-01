/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <folly/Synchronized.h>

using namespace watchman;

namespace watchman {

namespace {
constexpr flag_map kFlags[] = {
    {W_PENDING_CRAWL_ONLY, "CRAWL_ONLY"},
    {W_PENDING_RECURSIVE, "RECURSIVE"},
    {W_PENDING_VIA_NOTIFY, "VIA_NOTIFY"},
    {W_PENDING_IS_DESYNCED, "IS_DESYNCED"},
    {0, NULL},
};
}

bool is_path_prefix(
    const char* path,
    size_t path_len,
    const char* other,
    size_t common_prefix) {
  if (common_prefix > path_len) {
    return false;
  }

  w_assert(
      memcmp(path, other, common_prefix) == 0,
      "is_path_prefix: %.*s vs %.*s should have %d common_prefix chars\n",
      (int)path_len,
      path,
      (int)common_prefix,
      other,
      (int)common_prefix);
  (void)other;

  if (common_prefix == path_len) {
    return true;
  }

  return is_slash(path[common_prefix]);
}

} // namespace watchman

// Helper to un-doubly-link a pending item.
void PendingCollectionBase::unlinkItem(
    std::shared_ptr<watchman_pending_fs>& p) {
  if (pending_ == p) {
    pending_ = p->next;
  }
  auto prev = p->prev.lock();

  if (prev) {
    prev->next = p->next;
  }

  if (p->next) {
    p->next->prev = prev;
  }

  p->next.reset();
  p->prev.reset();
}

// Helper to doubly-link a pending item to the head of a collection.
void PendingCollectionBase::linkHead(std::shared_ptr<watchman_pending_fs>&& p) {
  p->prev.reset();
  p->next = pending_;
  if (p->next) {
    p->next->prev = p;
  }
  pending_ = std::move(p);
}

/* initialize a pending_coll */
PendingCollectionBase::PendingCollectionBase(
    std::condition_variable& cond,
    std::atomic<bool>& pinged)
    : cond_(cond), pinged_(pinged) {}

/* drain and discard the content of a pending_coll, but do not destroy it */
void PendingCollectionBase::drain() {
  pending_.reset();
  tree_.clear();
}

void PendingCollectionBase::ping() {
  pinged_ = true;
  cond_.notify_all();
}

void PendingCollection::ping() {
  pinged_ = true;
  cond_.notify_all();
}

// if there are any entries that are obsoleted by a recursive insert,
// walk over them now and mark them as ignored.
void PendingCollectionBase::maybePruneObsoletedChildren(
    w_string path,
    int flags) {
  if ((flags & (W_PENDING_RECURSIVE | W_PENDING_CRAWL_ONLY)) ==
      W_PENDING_RECURSIVE) {
    uint32_t pruned = 0;

    // Since deletion invalidates the iterator, we need to repeatedly
    // call this to prune out the nodes.  It will return 0 once no
    // matching prefixes are found and deleted.
    // Deletion is a bit awkward in this radix tree implementation.
    // We can't recursively delete a given prefix as a built-in operation
    // and it is non-trivial to add that functionality right now.
    // When we lop-off a portion of a tree that we're going to analyze
    // recursively, we have to iterate each leaf and explicitly delete
    // that leaf.
    // Since deletion invalidates the iteration state we have to signal
    // to stop iteration after each deletion and then retry the prefix
    // deletion.
    //
    // We need to compare the prefix to make sure that we don't delete
    // a sibling node by mistake (see commentary on the is_path_prefix
    // function for more on that).

    auto callback = [&](const w_string& key,
                        std::shared_ptr<watchman_pending_fs>& p) -> int {
      if (!p) {
        // It was removed; update the tree to reflect this
        tree_.erase(key);
        // Stop iteration: we deleted something and invalidated the
        // iterators.
        return 1;
      }

      if ((p->flags & W_PENDING_CRAWL_ONLY) == 0 && key.size() > path.size() &&
          is_path_prefix(
              (const char*)key.data(), key.size(), path.data(), path.size()) &&
          !watchman::CookieSync::isPossiblyACookie(p->path)) {
        logf(
            DBG,
            "delete_kids: removing ({}) {} from pending because it is "
            "obsoleted by ({}) {}\n",
            p->path.size(),
            p->path,
            path.size(),
            path);

        // Unlink the child from the pending index.
        unlinkItem(p);

        // Remove it from the art tree.
        tree_.erase(key);

        // Stop iteration because we just invalidated the iterator state
        // by modifying the tree mid-iteration.
        return 1;
      }

      return 0;
    };

    while (tree_.iterPrefix(
        reinterpret_cast<const uint8_t*>(path.data()), path.size(), callback)) {
      // OK; try again
      ++pruned;
    }

    if (pruned) {
      logf(
          DBG,
          "maybePruneObsoletedChildren: pruned {} nodes under ({}) {}\n",
          pruned,
          path.size(),
          path);
    }
  }
}

void PendingCollectionBase::consolidateItem(watchman_pending_fs* p, int flags) {
  // Increase the strength of the pending item if either of these
  // flags are set.
  // We upgrade crawl-only as well as recursive; it indicates that
  // we've recently just performed the stat and we want to avoid
  // infinitely trying to stat-and-crawl
  p->flags |= flags &
      (W_PENDING_CRAWL_ONLY | W_PENDING_RECURSIVE | W_PENDING_IS_DESYNCED);

  maybePruneObsoletedChildren(p->path, p->flags);
}

// Check the tree to see if there is a path that is earlier/higher in the
// filesystem than the input path; if there is, and it is recursive,
// return true to indicate that there is no need to track this new path
// due to the already scheduled higher level path.
bool PendingCollectionBase::isObsoletedByContainingDir(const w_string& path) {
  auto leaf = tree_.longestMatch((const uint8_t*)path.data(), path.size());
  if (!leaf) {
    return false;
  }
  auto p = leaf->value;

  if ((p->flags & W_PENDING_RECURSIVE) &&
      is_path_prefix(
          path.data(),
          path.size(),
          (const char*)leaf->key.data(),
          leaf->key.size())) {
    if (watchman::CookieSync::isPossiblyACookie(path)) {
      return false;
    }

    // Yes: the pre-existing entry higher up in the tree obsoletes this
    // one that we would add now.
    logf(DBG, "is_obsoleted: SKIP {} is obsoleted by {}\n", path, p->path);
    return true;
  }
  return false;
}

watchman_pending_fs::watchman_pending_fs(
    const w_string& path,
    const struct timeval& now,
    int flags)
    : path(path), now(now), flags(flags) {}

void PendingCollectionBase::add(
    const w_string& path,
    struct timeval now,
    int flags) {
  char flags_label[128];

  auto existing = tree_.search(path);
  if (existing) {
    /* Entry already exists: consolidate */
    consolidateItem(existing->get(), flags);
    /* all done */
    return;
  }

  if (isObsoletedByContainingDir(path)) {
    return;
  }

  // Try to allocate the new node before we prune any children.
  auto p = std::make_shared<watchman_pending_fs>(path, now, flags);

  maybePruneObsoletedChildren(path, flags);

  w_expand_flags(kFlags, flags, flags_label, sizeof(flags_label));
  logf(DBG, "add_pending: {} {}\n", path, flags_label);

  tree_.insert(path, p);
  linkHead(std::move(p));
}

void PendingCollectionBase::add(
    struct watchman_dir* dir,
    const char* name,
    struct timeval now,
    int flags) {
  return add(dir->getFullPathToChild(name), now, flags);
}

/* Append the contents of src to target, consolidating in target.
 * src is effectively drained in the process.
 * Caller must own the lock on both src and target. */
void PendingCollectionBase::append(PendingCollectionBase* src) {
  auto p = src->stealItems();
  while (p) {
    auto target_p =
        tree_.search((const uint8_t*)p->path.data(), p->path.size());
    if (target_p) {
      /* Entry already exists: consolidate */
      consolidateItem(target_p->get(), p->flags);
      p = std::move(p->next);
      continue;
    }

    if (isObsoletedByContainingDir(p->path)) {
      p = std::move(p->next);
      continue;
    }
    maybePruneObsoletedChildren(p->path, p->flags);

    auto next = std::move(p->next);
    tree_.insert(p->path, p);
    linkHead(std::move(p));

    p = std::move(next);
  }
}

std::shared_ptr<watchman_pending_fs> PendingCollectionBase::stealItems() {
  tree_.clear();
  return std::move(pending_);
}

/* Returns the number of unique pending items in the collection */
uint32_t PendingCollectionBase::size() const {
  return tree_.size();
}

bool PendingCollectionBase::checkAndResetPinged() {
  if (pending_ || pinged_) {
    pinged_ = false;
    return true;
  }
  return false;
}

PendingCollection::PendingCollection()
    : folly::Synchronized<
          PendingCollectionBase,
          std::mutex>{folly::in_place, cond_, pinged_},
      pinged_{false} {}

PendingCollection::LockedPtr PendingCollection::lockAndWait(
    std::chrono::milliseconds timeoutms,
    bool& pinged) {
  auto lock = this->lock();

  if (lock->checkAndResetPinged()) {
    pinged = true;
    return lock;
  }

  if (timeoutms.count() == -1) {
    cond_.wait(lock.getUniqueLock());
  } else {
    cond_.wait_for(lock.getUniqueLock(), timeoutms);
  }

  pinged = lock->checkAndResetPinged();

  return lock;
}

/* vim:ts=2:sw=2:et:
 */
