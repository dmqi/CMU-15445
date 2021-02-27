/**
 * lock_manager.cpp
 */

#include "concurrency/lock_manager.h"

using namespace std;

namespace cmudb {

bool LockManager::LockShared(Transaction *txn, const RID &rid) {
  return lockTemplate(txn, rid, LockType::SHARED);
}

bool LockManager::LockExclusive(Transaction *txn, const RID &rid) {
  return lockTemplate(txn, rid, LockType::EXCLUSIVE);
}

bool LockManager::LockUpgrade(Transaction *txn, const RID &rid) {
  return lockTemplate(txn, rid, LockType::UPGRADING);
}

bool LockManager::Unlock(Transaction *txn, const RID &rid) {
  if (strict_2PL_) {
    if (txn->GetState() != TransactionState::ABORTED && txn->GetState() != TransactionState::COMMITTED) {
      txn->SetState(TransactionState::ABORTED);
      return false;
    }
  } else if (txn->GetState() == TransactionState::GROWING) {
    txn->SetState(TransactionState::SHRINKING);
  }
  unique_lock<mutex> tableLatch(mutex_);
  TxnList& txnList = lockTable_[rid];
  unique_lock<mutex> txnListLatch(txnList.mutex_);
  auto it = find_if(txnList.list_.begin(), txnList.list_.end(),
                    [txn](const TxnItem& item) { return item.txnId_ == txn->GetTransactionId(); });
  assert(it != txnList.list_.end());
  auto lockSet = (it->type_ == LockType::SHARED) ? txn->GetSharedLockSet() : txn->GetExclusiveLockSet();
  assert(lockSet->erase(rid) == 1);
  txnList.list_.erase(it);
  if (txnList.list_.empty()) {
    lockTable_.erase(rid);
    return true;
  }
  tableLatch.unlock();
  for (auto& lock : txnList.list_) {
    if (lock.granted_) break;
    lock.Grant();
    if (lock.type_ == LockType::SHARED) continue;
    if (lock.type_ == LockType::UPGRADING) {
      txnList.hasUpgrading_ = false;
      lock.type_ = LockType::EXCLUSIVE;
    }
    break;
  }
  return true;
}

bool LockManager::lockTemplate(Transaction *txn, const RID &rid, LockType type) {
  if (txn->GetState() != TransactionState::GROWING) {
      txn->SetState(TransactionState::ABORTED);
      return false;
  }
  unique_lock<mutex> tableLatch(mutex_);
  TxnList& txnList = lockTable_[rid];
  unique_lock<mutex> txnListLatch(txnList.mutex_);
  tableLatch.unlock();
  if (type == LockType::UPGRADING) {
    if (txnList.hasUpgrading_) {
      txn->SetState(TransactionState::ABORTED);
      return false;
    }
    auto it = find_if(txnList.list_.begin(), txnList.list_.end(),
                      [txn](const TxnItem& item) { return item.txnId_ == txn->GetTransactionId(); });
    if (it == txnList.list_.end() || !it->granted_ || it->type_ != LockType::SHARED) {
      txn->SetState(TransactionState::ABORTED);
      return false;
    }
    txnList.list_.erase(it);
    assert(txn->GetSharedLockSet()->erase(rid) == 1);
  }
  bool granted = txnList.checkCanGrant(type);
  if (!granted && txnList.list_.back().txnId_ < txn->GetTransactionId()) {
    txn->SetState(TransactionState::ABORTED);
    return false;
  }
  txnList.insert(txn, rid, type, granted, &txnListLatch);
  return true;
}

} // namespace cmudb
