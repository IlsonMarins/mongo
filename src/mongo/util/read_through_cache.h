/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>

#include "mongo/bson/oid.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/invalidating_lru_cache.h"

namespace mongo {

/**
 * Serves as a container of the non-templatised parts of the ReadThroughCache class below.
 */
class ReadThroughCacheBase {
    ReadThroughCacheBase(const ReadThroughCacheBase&) = delete;
    ReadThroughCacheBase& operator=(const ReadThroughCacheBase&) = delete;

protected:
    ReadThroughCacheBase(Mutex& mutex, ServiceContext* service, ThreadPoolInterface& threadPool);

    virtual ~ReadThroughCacheBase();

    /**
     * This method is an extension of ThreadPoolInterface::schedule, with the following additions:
     *  - Creates a client and an operation context and executes the specified 'work' under that
     * environment
     *  - Returns a CancelToken, which can be used to attempt to cancel 'work'
     *
     * If the task manages to get canceled before it is executed (through a call to tryCancel),
     * 'work' will be invoked out-of-line with a non-OK status, set to error code
     * ReadThroughCacheLookupCanceled.
     */
    class CancelToken {
    public:
        struct TaskInfo;
        CancelToken(std::shared_ptr<TaskInfo> info);
        CancelToken(CancelToken&&);
        ~CancelToken();

        void tryCancel();

    private:
        std::shared_ptr<TaskInfo> _info;
    };
    using WorkWithOpContext = unique_function<void(OperationContext*, const Status&)>;
    CancelToken _asyncWork(WorkWithOpContext work) noexcept;

    Date_t _now();

    // Service context under which this cache has been instantiated (used for access to service-wide
    // functionality, such as client/operation context creation)
    ServiceContext* const _serviceContext;

    // Thread pool to be used for invoking the blocking 'lookup' calls
    ThreadPoolInterface& _threadPool;

    // Used to protect the shared state in the child ReadThroughCache template below. Has a lock
    // level of 3, meaning that while held, it is only allowed to take '_cancelTokenMutex' below and
    // the Client lock.
    Mutex& _mutex;

    // Used to protect calls to 'tryCancel' above. Has a lock level of 2, meaning what while held,
    // it is only allowed to take the Client lock.
    Mutex _cancelTokenMutex = MONGO_MAKE_LATCH("ReadThroughCacheBase::_cancelTokenMutex");
};

/**
 * Implements a generic read-through cache built on top of InvalidatingLRUCache.
 */
template <typename Key, typename Value>
class ReadThroughCache : public ReadThroughCacheBase {
    /**
     * Data structure wrapping and expanding on the values stored in the cache.
     */
    struct StoredValue {
        Value value;

        // Contains the wallclock time of when the value was fetched from the backing storage. This
        // value is not precise and should only be used for diagnostics purposes (i.e., it cannot be
        // relied on to perform any recency comparisons for example).
        Date_t updateWallClockTime;
    };
    using Cache = InvalidatingLRUCache<Key, StoredValue>;

public:
    /**
     * Common type for values returned from the cache.
     */
    class ValueHandle {
    public:
        // The two constructors below are present in order to offset the fact that the cache doesn't
        // support pinning items. Their only usage must be in the authorization mananager for the
        // internal authentication user.
        ValueHandle(Value&& value) : _valueHandle({std::move(value), Date_t::min()}) {}
        ValueHandle() = default;

        operator bool() const {
            return bool(_valueHandle);
        }

        bool isValid() const {
            return _valueHandle.isValid();
        }

        Value* get() {
            return &_valueHandle->value;
        }

        const Value* get() const {
            return &_valueHandle->value;
        }

        Value& operator*() {
            return *get();
        }

        const Value& operator*() const {
            return *get();
        }

        Value* operator->() {
            return get();
        }

        const Value* operator->() const {
            return get();
        }

        /**
         * See the comments for `StoredValue::updateWallClockTime` above.
         */
        Date_t updateWallClockTime() const {
            return _valueHandle->updateWallClockTime;
        }

    private:
        friend class ReadThroughCache;

        ValueHandle(typename Cache::ValueHandle&& valueHandle)
            : _valueHandle(std::move(valueHandle)) {}

        typename Cache::ValueHandle _valueHandle;
    };

    /**
     * Signature for a blocking function to provide the value for a key when there is a cache miss.
     *
     * The implementation must throw a uassertion to indicate an error while looking up the value,
     * return boost::none if the key is not found, or return an actual value.
     */
    struct LookupResult {
        explicit LookupResult(boost::optional<Value>&& v) : v(std::move(v)) {}
        LookupResult(LookupResult&&) = default;
        LookupResult& operator=(LookupResult&&) = default;

        // If boost::none, it means the '_lookupFn' did not find the key in the store
        boost::optional<Value> v;
    };
    using LookupFn = unique_function<LookupResult(OperationContext*, const Key&)>;

    // Exposed publicly so it can be unit-tested indepedently of the usages in this class. Must not
    // be used independently.
    class InProgressLookup;

    /**
     * If 'key' is found in the cache, returns a set ValueHandle (its operator bool will be true).
     * Otherwise, either causes the blocking 'lookup' below to be asynchronously invoked to fetch
     * 'key' from the backing store (or joins an already scheduled invocation) and returns a future
     * which will be signaled when the lookup completes.
     *
     * If the lookup is successful and 'key' is found in the store, it will be cached (so subsequent
     * lookups won't have to re-fetch it) and the future will be set. If 'key' is not found in the
     * backing store, returns a not-set ValueHandle (it's bool operator will be false). If 'lookup'
     * fails, the future will be set to the appropriate exception and nothing will be cached,
     * meaning that subsequent calls to 'acquireAsync' will kick-off 'lookup' again.
     *
     * NOTES:
     *  The returned value may be invalid by the time the caller gets to access it, if 'invalidate'
     *  is called for 'key'.
     */
    SharedSemiFuture<ValueHandle> acquireAsync(const Key& key) {
        // Fast path
        if (auto cachedValue = _cache.get(key))
            return {std::move(cachedValue)};

        stdx::unique_lock ul(_mutex);

        // Re-check the cache under a mutex, before kicking-off the asynchronous lookup
        if (auto cachedValue = _cache.get(key))
            return {std::move(cachedValue)};

        // Join an in-progress lookup if one has already been scheduled
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            return it->second->addWaiter(ul);

        // Schedule an asynchronous lookup for the key
        auto [it, emplaced] =
            _inProgressLookups.emplace(key, std::make_unique<InProgressLookup>(*this, key));
        invariant(emplaced);
        auto& inProgressLookup = *it->second;
        auto sharedFutureToReturn = inProgressLookup.addWaiter(ul);

        ul.unlock();

        _doLookupWhileNotValid(key, Status(ErrorCodes::Error(461540), "")).getAsync([](auto) {});

        return sharedFutureToReturn;
    }

    /**
     * A blocking variant of 'acquireAsync' above - refer to it for more details.
     *
     * NOTES:
     *  This is a potentially blocking method.
     */
    ValueHandle acquire(OperationContext* opCtx, const Key& key) {
        return acquireAsync(key).get(opCtx);
    }

    /**
     * Invalidates the given 'key' and immediately replaces it with a new value.
     */
    ValueHandle insertOrAssignAndGet(const Key& key, Value&& newValue, Date_t updateWallClockTime) {
        stdx::lock_guard lg(_mutex);
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            it->second->invalidateAndCancelCurrentLookupRound(lg);
        return _cache.insertOrAssignAndGet(key, {std::move(newValue), updateWallClockTime});
    }

    /**
     * The invalidate methods below guarantee the following:
     *  - All affected keys already in the cache (or returned to callers) will be invalidated and
     *    removed from the cache
     *  - All affected keys, which are in the process of being loaded (i.e., acquireAsync has not
     *    yet completed) will be internally interrupted and rescheduled again, as if 'acquireAsync'
     *    was called *after* the call to invalidate
     *
     * In essence, the invalidate calls serve as a "barrier" for the affected keys.
     */
    void invalidate(const Key& key) {
        stdx::lock_guard lg(_mutex);
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            it->second->invalidateAndCancelCurrentLookupRound(lg);
        _cache.invalidate(key);
    }

    template <typename Pred>
    void invalidateIf(const Pred& predicate) {
        stdx::lock_guard lg(_mutex);
        for (auto& entry : _inProgressLookups) {
            if (predicate(entry.first))
                entry.second->invalidateAndCancelCurrentLookupRound(lg);
        }
        _cache.invalidateIf([&](const Key& key, const StoredValue*) { return predicate(key); });
    }

    void invalidateAll() {
        invalidateIf([](const Key&) { return true; });
    }

    /**
     * Returns statistics information about the cache for reporting purposes.
     */
    std::vector<typename Cache::CachedItemInfo> getCacheInfo() const {
        return _cache.getCacheInfo();
    }

protected:
    /**
     * ReadThroughCache constructor, to be called by sub-classes, which implement 'lookup'.
     *
     * The 'mutex' is for the exclusive usage of the ReadThroughCache and must not be used in any
     * way by the implementing class. Having the mutex stored by the sub-class allows latch
     * diagnostics to be correctly associated with the sub-class (not the generic ReadThroughCache
     * class).
     *
     * The 'threadPool' can be used for other purposes, but it is mandatory that by the time this
     * object is destructed that it is shut down and joined so that there are no more asynchronous
     * loading activities going on.
     *
     * The 'cacheSize' parameter specifies the maximum size of the cache before the least recently
     * used entries start getting evicted. It is allowed to be zero, in which case no entries will
     * actually be cached, but it doesn't guarantee that every `acquire` call will result in an
     * invocation of `lookup`. Specifically, several concurrent invocations of `acquire` for the
     * same key may group together for a single `lookup`.
     */
    ReadThroughCache(Mutex& mutex,
                     ServiceContext* service,
                     ThreadPoolInterface& threadPool,
                     LookupFn lookupFn,
                     int cacheSize)
        : ReadThroughCacheBase(mutex, service, threadPool),
          _lookupFn(std::move(lookupFn)),
          _cache(cacheSize) {}

    ~ReadThroughCache() {
        invariant(_inProgressLookups.empty());
    }

private:
    using InProgressLookupsMap = stdx::unordered_map<Key, std::unique_ptr<InProgressLookup>>;

    /**
     * This method implements an asynchronous "while (!valid)" loop over 'key', which must be on the
     * in-progress map.
     */
    Future<LookupResult> _doLookupWhileNotValid(Key key, StatusWith<LookupResult> sw) {
        stdx::unique_lock ul(_mutex);
        auto it = _inProgressLookups.find(key);
        invariant(it != _inProgressLookups.end());
        if (!ErrorCodes::isCancelationError(sw.getStatus()) && !it->second->valid(ul)) {
            ul.unlock();  // asyncLookupRound also acquires the mutex
            return it->second->asyncLookupRound().onCompletion(
                [this, key](auto sw) { return _doLookupWhileNotValid(key, std::move(sw)); });
        }

        // The detachment of the currently active lookup and the placement of the result on the
        // '_cache' has to be atomic with respect to a concurrent call to 'invalidate'
        auto inProgressLookup(std::move(it->second));
        _inProgressLookups.erase(it);

        StatusWith<ValueHandle> swValueHandle(ErrorCodes::Error(461541), "");
        if (sw.isOK()) {
            auto result = std::move(sw.getValue());
            swValueHandle = result.v
                ? ValueHandle(_cache.insertOrAssignAndGet(key, {std::move(*result.v), _now()}))
                : ValueHandle();
        } else {
            swValueHandle = sw.getStatus();
        }
        ul.unlock();

        inProgressLookup->signalWaiters(std::move(swValueHandle));

        return Future<LookupResult>::makeReady(Status(ErrorCodes::Error(461542), ""));
    }

    // Blocking function which will be invoked to retrieve entries from the backing store
    const LookupFn _lookupFn;

    // Contains all the currently cached keys. This structure is self-synchronising and doesn't
    // require a mutex. However, on cache miss it is accessed under '_mutex', which is safe, because
    // _cache's mutex itself is at level 0.
    //
    // NOTE: From destruction order point of view, because keys first "start" in
    // '_inProgressLookups' and then move on to '_cache' the order of these two fields is important.
    Cache _cache;

    // Keeps track of all the keys, which were attempted to be 'acquireAsync'-ed, weren't found in
    // the cache and are currently in the process of being looked up from the backing store. A
    // single key may only be on this map or in '_cache', but never in both.
    //
    // This map is protected by '_mutex'.
    InProgressLookupsMap _inProgressLookups;
};

/**
 * This class represents an in-progress lookup for a specific key and implements the guarantees of
 * the invalidation logic as described in the comments of 'ReadThroughCache::invalidate'.
 *
 * It is intended to be used in conjunction with the 'ReadThroughCache', which operates on it under
 * its '_mutex' and ensures there is always at most a single active instance at a time active for
 * each 'key'.
 *
 * The methods of this class are not thread-safe, unless indicated in the comments.
 *
 * Its lifecycle is intended to be like this:
 *
 * inProgressLookups.emplace(inProgress);
 * while (true) {
 *      result = inProgress.asyncLookupRound();
 *      if (!inProgress.valid()) {
 *          continue;
 *      }
 *
 *      inProgressLookups.remove(inProgress)
 *      cachedValues.insert(result);
 *      inProgress.signalWaiters(result);
 * }
 */
template <typename Key, typename Value>
class ReadThroughCache<Key, Value>::InProgressLookup {
public:
    InProgressLookup(ReadThroughCache& cache, Key key) : _cache(cache), _key(std::move(key)) {}

    Future<LookupResult> asyncLookupRound() {
        auto [promise, future] = makePromiseFuture<LookupResult>();

        stdx::lock_guard lg(_cache._mutex);
        _valid = true;
        _cancelToken.emplace(_cache._asyncWork([ this, promise = std::move(promise) ](
            OperationContext * opCtx, const Status& status) mutable noexcept {
            promise.setWith([&] {
                uassertStatusOK(status);
                return _cache._lookupFn(opCtx, _key);
            });
        }));

        return std::move(future);
    }

    SharedSemiFuture<ValueHandle> addWaiter(WithLock) {
        return _sharedPromise.getFuture();
    }

    bool valid(WithLock) const {
        return _valid;
    }

    void invalidateAndCancelCurrentLookupRound(WithLock) {
        _valid = false;
        if (_cancelToken)
            _cancelToken->tryCancel();
    }

    void signalWaiters(StatusWith<ValueHandle> swValueHandle) {
        invariant(_valid);
        _sharedPromise.setFromStatusWith(std::move(swValueHandle));
    }

private:
    // The owning cache, from which mutex, lookupFn, async task scheduling, etc. will be used. It is
    // the responsibility of the owning cache to join all outstanding lookups at destruction time.
    ReadThroughCache& _cache;

    const Key _key;

    bool _valid{false};
    boost::optional<CancelToken> _cancelToken;

    SharedPromise<ValueHandle> _sharedPromise;
};

}  // namespace mongo
