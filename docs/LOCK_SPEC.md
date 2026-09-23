# ZenthrilDB Lock Manager Specification

## Scope

The Lock Manager Foundation provides local in-memory coordination for future
transaction isolation, MVCC, page access, and catalog changes.

It does not implement:

- deadlock detection
- distributed locks
- lock escalation
- intention locks
- predicate locks
- SQL isolation levels

## Lock Modes

| Mode | Meaning |
|---|---|
| `Shared` | multiple transactions may hold the same resource for read-compatible access |
| `Exclusive` | only one transaction may hold the resource |

Compatibility matrix:

| Requested / Existing | Shared | Exclusive |
|---|---:|---:|
| Shared | yes | no |
| Exclusive | no | no |

The same transaction may reacquire a lock it already owns. A transaction may
upgrade its own single shared lock to exclusive when no other transaction holds
the resource.

## Resource Model

`LockResourceId` supports:

- database
- metadata
- table
- page

The lock manager stores resource identifiers independently from storage classes
to keep coupling low.

## API

`LockManager` supports:

- `tryAcquire(transactionId, resource, mode)`
- `acquire(transactionId, resource, mode, timeout)`
- `acquireGuard(transactionId, resource, mode, timeout)`
- `release(transactionId, resource)`
- `releaseAll(transactionId)`
- `holds(transactionId, resource, mode)`
- `activeLockCount()`
- `activeLockCount(resource)`

`LockGuard` is move-only and releases its lock in the destructor.

## Concurrency Model

All lock table mutations are protected by a mutex. Waiting uses a condition
variable and timeout-based policy. Timeout-based waiting is a deliberate MVP
choice until deadlock detection is introduced.

## Security and Race-Condition Requirements

The lock manager must prevent:

- exclusive lock overrun while shared locks are active
- multiple exclusive owners on one resource
- forgotten release through RAII usage
- invalid transaction id acquisition
- indefinite blocking in MVP APIs

## Future Work

- deadlock detector
- wait-for graph
- lock conversion queue fairness
- intention lock modes
- page latch integration
- MVCC snapshot integration
