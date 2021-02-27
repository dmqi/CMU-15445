/**
 * lock_manager.h
 *
 * Tuple level lock manager, use wait-die to prevent deadlocks
 */

#pragma once

#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <cassert>

#include "common/rid.h"
#include "concurrency/transaction.h"

using namespace std;

enum class LockType { SHARED = 0, EXCLUSIVE, UPGRADING };

namespace cmudb {

class LockManager {

struct TxnItem {
  TxnItem(txn_id_t id, LockType type, bool granted)
    : txnId_(id), type_(type), granted_(granted) {}

  void Wait() {
    unique_lock<mutex> ul(mutex_);
    cv_.wait(ul, [this] { return this->granted_; });
  }

  void Grant() {
    lock_guard<mutex> lg(mutex_);
    granted_ = true;
    cv_.notify_one();
  }

  mutex mutex_;
  condition_variable cv_;
  txn_id_t txnId_;
  LockType type_;
  bool granted_;
};

struct TxnList {
  mutex mutex_;
  bool hasUpgrading_;
  list<TxnItem> list_;

  bool checkCanGrant(LockType type) {
    if (list_.empty()) return true;
    const auto& last = list_.back();
    if (type == LockType::SHARED) {
      return last.granted_ && last.type_ == LockType::SHARED;
    }
    return false;
  }

  void insert(Transaction* txn, const RID& rid, LockType type, bool granted, unique_lock<mutex>* lock) {
    bool isUpgrading = (type == LockType::UPGRADING);
    if (isUpgrading && granted) type = LockType::EXCLUSIVE;
    list_.emplace_back(txn->GetTransactionId(), type, granted);
    auto& last = list_.back();
    if (!granted) {
      hasUpgrading_ |= isUpgrading;
      lock->unlock();
      last.Wait();
    }
    if (type == LockType::SHARED) {
      txn->GetSharedLockSet()->insert(rid);
    } else {
      txn->GetExclusiveLockSet()->insert(rid);
    }
  }
};


public:
  LockManager(bool strict_2PL) : strict_2PL_(strict_2PL){};

  /*** below are APIs need to implement ***/
  // lock:
  // return false if transaction is aborted
  // it should be blocked on waiting and should return true when granted
  // note the behavior of trying to lock locked rids by same txn is undefined
  // it is transaction's job to keep track of its current locks
  bool LockShared(Transaction *txn, const RID &rid);
  bool LockExclusive(Transaction *txn, const RID &rid);
  bool LockUpgrade(Transaction *txn, const RID &rid);

  // unlock:
  // release the lock hold by the txn
  bool Unlock(Transaction *txn, const RID &rid);
  /*** END OF APIs ***/

private:
  bool lockTemplate(Transaction* txn, const RID &rid, LockType type);
  bool strict_2PL_;
  mutex mutex_;
  std::unordered_map<RID, TxnList> lockTable_;
};


} // namespace cmudb
