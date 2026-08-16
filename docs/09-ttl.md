# 09 — TTL / Key Expiration

> **Stage:** 10  
> **Status:** 🔲 Planned. This document describes the design before implementation begins.

---

## What is TTL?

TTL stands for **Time to Live**. It is an optional property that can be attached to a key at insertion time, specifying how long the key should remain accessible before it automatically expires.

A key with a TTL becomes invisible (effectively deleted) once its expiration time passes, even if no explicit DELETE was ever issued.

This is a fundamental feature of any cache or session store. It is how Redis implements session expiration, cache invalidation, rate limiting windows, and one-time tokens.

---

## Use Cases

```
SET session:abc123   "user:vishnu"   TTL=3600    ← expires in 1 hour
SET rate_limit:ip    "42"            TTL=60       ← resets every 60 seconds
SET otp:phone:9999   "837291"        TTL=300      ← OTP valid for 5 minutes
```

Without TTL, these use cases require the client to manage expiration manually — a fragile and error-prone approach. With TTL, the storage engine enforces expiration automatically.

---

## How TTL Works Internally

When a key is stored with a TTL, ForgeKV records the **absolute expiration timestamp** alongside the value:

```
expiry_time = current_time + TTL_seconds
```

```
In-memory store entry (conceptual):
  Key:    "session:abc123"
  Value:  "user:vishnu"
  Expiry: 1755289200   ← Unix timestamp (seconds since epoch)
```

Every read operation (GET, EXISTS) checks whether the current time has passed the expiry timestamp. If it has, the key is treated as if it does not exist.

```
GET "session:abc123"

  current_time = now()
  entry = store.find("session:abc123")

  if entry not found:
    return not_found

  if entry.expiry != 0 AND current_time > entry.expiry:
    return not_found   ← key has expired

  return entry.value
```

---

## Data Structure Change

Adding TTL requires extending the in-memory store. A value can no longer be a bare `std::string` — it needs to carry an optional expiration timestamp.

The backing store changes from:

```cpp
std::unordered_map<std::string, std::string>
```

To something like:

```cpp
struct Entry {
    std::string value;
    std::chrono::time_point<std::chrono::steady_clock> expires_at;
    bool has_ttl;
};

std::unordered_map<std::string, Entry>
```

The exact representation will be finalized during Stage 10 implementation. This is one reason Stage 2 (Storage Abstraction) is introduced early — the data structure can change without breaking the API.

---

## Lazy Expiration

The simplest expiration strategy is **lazy expiration**: a key is not removed when it expires — it is only removed when it is next accessed.

```
GET "session:abc123"
  → current_time > expiry_time
  → return not_found
  → (optionally) remove the entry from the map at this point
```

Advantages:
- No background work required
- Simple to implement correctly
- No impact on write throughput

Disadvantages:
- Expired keys remain in memory until accessed
- For keys that are never read again after expiry, they accumulate as dead weight

Lazy expiration is the first strategy implemented. It is correct and safe.

---

## Active Expiration (Background Cleanup)

To reclaim memory for keys that expire but are never accessed again, a background cleanup mechanism periodically scans the store and removes expired entries.

```
Background thread (periodic sweep):
  for each key in store:
    if current_time > key.expiry_time:
      remove key from store
      write DELETE record to WAL
```

The WAL must record the expiration-driven DELETE so that a recovery after restart does not re-add the expired key from a snapshot.

Background cleanup must hold appropriate locks while modifying the store. Long sweeps on large stores must not starve readers. The sweep interval and batch size will be configurable.

---

## TTL and the WAL

TTL-related information must be persisted to the WAL for correct recovery.

When a key is SET with a TTL, the WAL record must include:
- The key
- The value
- The expiration timestamp (absolute time, not the original TTL duration)

Storing the absolute timestamp (not the TTL offset) ensures that replay after a long downtime does not resurrect keys that should already have expired.

For example:

```
SET "otp" "837291" with TTL=300 at time T=1000
→ WAL record stores expiry = 1300

Recovery at time T=1500:
  Load record: expiry = 1300
  current_time = 1500 > 1300
  → Key is already expired, skip it
```

This is correct behavior. A key that expired during downtime should not appear in the recovered store.

---

## TTL Without TTL

Keys stored without a TTL have no expiration. They remain in the store indefinitely until explicitly deleted. This is the default behavior, and it is preserved exactly — no TTL field means no expiration check.

---

## Summary

| Mechanism               | Description                                              |
|-------------------------|----------------------------------------------------------|
| TTL value               | Duration in seconds provided at SET time                 |
| Expiry timestamp        | Absolute time stored in the entry and WAL                |
| Lazy expiration         | Check on every read; return not_found if expired         |
| Active expiration       | Background thread sweeps and removes expired entries     |
| WAL interaction         | Expiry timestamp stored in WAL; replayed on recovery     |
| Keys without TTL        | No expiration, persist indefinitely                      |

---

*Previous: [08-snapshots.md](08-snapshots.md)*  
*Next: [10-benchmarking.md](10-benchmarking.md)*
