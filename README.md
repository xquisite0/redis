[![progress-banner](https://backend.codecrafters.io/progress/redis/2413cc25-ce65-4e24-b894-57e7717ca4bf)](https://app.codecrafters.io/users/codecrafters-bot?r=2qF)

A Redis Implementation in C++ from scratch that supports the following list of commands: 

`ECHO`, `PING`, `SET`, `GET`, `CONFIG GET`, `KEYS`, `INFO replication`, `WAIT`, `TYPE`, `XADD`, `XRANGE`, `XREAD`, `INCR`, `MULTI`, `EXEC`, `DISCARD`.

Interesting points:

1) Socket Programming lies at the heart of this program. Master-Replica / Master-Client / Replica-Client connections are all built off sockets.

2) Concurrent clients are "supported" by Redis instances with threads. Concurrent clients can be processed in parallel, however race conditions may still arise as proper write guards on shared data structures have not been implemented.

3) Setting keys with expiries is supported in this implementation. For instance `SET foo bar px 100` will set the key "foo" to "bar" with an expiry of 100 milliseconds. Any future `GET` commands that are executed more than 100ms past the SET command will return NULL.

4) Persistence is supported! Servers use the `parseRDB()` function in `util.cpp` to process the Redis Database (RDB) file that stores the previous state of the database and update their key-value store state correspondingly.

5) Newly instantiated replicas execute a "Handshake" process with its master server (involves a specific protocol of back-and-forth messages). This helps to establish a connection between the replica and master servers. 

6) Using the connection mentioned in 5), master servers can then propagate state-changing commands that it receives from clients to its replica servers. Bear in mind that these connections are all concurrently running at the same time, so none of the replicas get neglected!

7) The `WAIT` command is likely the command that I faced the most trouble implementing and spent a few weeks on it. For a brief summary, the `WAIT numreplicas timeout` command is used by a client on a master server. `WAIT` then blocks, and awaits for a specified number of replicas to be fully synchronised with the master's state, before finally unblocking. It can be used to improve "real world data safety" (as the Redis docs says) and allows for a greater likelihood of a replica being able to promote to a master server in case of any server failures. 

7.1) The biggest issue with `WAIT` came about from the timeout. The command flow is as such: check for the `syncedReplicas` count, if it is not `< numreplicas`, pause command execution until the timeout is reached. However, the updating of the `syncedReplicas` required the master server to send out a `REPLCONF GETACK *` command to all clients, then await a `REPLCONF ACK {replica_offset}` from each replica. Then, do an equality check with `master_repl_offset`.

7.2) To clarify some terms, `replica_offset` - The total number of bytes of propagated commands (see point 6) that this individual replica has PROCESSED (processed being updated its own internal state accordingly). `master_repl_offset` - The total number of bytes of commands propagated by the master to its replicas. (the master propagates the same commands to all replicas). 

7.3) So, if `replica_offset == master_repl_offset` for a specific replica's offset, it means that replica if fully synchronised! It has processed all propagated commands from the master and updated its internal replica_offset. 

7.4) Okay, but back to the annoying part of this command. Since this process of sending and awaiting the replica's offset is so time-consuming, (sometimes the replica doesn't even respond with the its `replica_offset` because it is busy handling a prior propagated command! adds to the time-consuming part of things), hence the given `timeout` in `WAIT numreplicas timeout` is simply not enough to accurately return the proper number of synced replicas.

7.5) After a few days of thinking, the only way to ensure that the `WAIT` command can be processed in time accurately before the timeout, is to process each replica **asynchronously**! Each replica is now given `timeout` amount of time to be processed, rather than `timeout \ replicaSockets.size()` amount of time to be processed!! How this is done is that the updating of the `syncedReplicas` count is done on the concurrent Master-Replica connections, rather than the sole Master-client connection (the one where the client passed the `WAIT` function). On the Master-Replica connections, upon the receipt of `REPLCONF ACK {replica_offset}`, the code can then update the `syncedReplicas` count based on the given `replica_offset` for that particular replica, with `replica_offset == master_repl_offset` check. 

7.6) To conclude this section on `WAIT`, a shift from linear programming to parallel programming allowed the command run within the tight `timeout` accurately. 

8) Streams are supported in this Redis instance. They can be used almost like an append-only log, but the true benefit of streams arises from the ability to record (`XADD`) & simultaneously track events that happen in real time (`XREAD`). Use-cases include: Event sourcing (clicks/user actions, etc.), Sensor monitoring (e.g. readings from actions in the field) & notifications (e.g. storing a record of each user's notifications in a separate stream). Source: Redis Docs. 

8.1) IDs are generated for each stream entry in the format `milliSeconds-sequenceNumber`. `XADD` allows for the addition of new stream entries and there are multiple consumption strategies. 

8.2) `XRANGE` fetches the entries within a specified start and end ID. 

8.3) `XREAD` fetches stream entries from a specified start ID onwards. `BLOCK` is also supported. In its usual form, `XREAD` can fetch the stream entries that are present. But in some cases, stream entries may not be available at this current point in time. To avoid the need to write code that tries to fetch data at a fixed interval, `BLOCK` can be used (with a specified timeout) to block XREAD until one of its streams accepts data. 

9) Transactions are now supported as well! Transactions start with a `MULTI` command. After this command is given, any future commands are queued and not processed. Until a `EXEC` command is given, which batch executes the commands in the transaction queue. Or if `DISCARD` is given, which discards all commands in the current transaction and kills it.

9.1) The true beauty in transactions in the actual Redis implementation actually lies in its atomic nature. That all commands in a transaction are run either sequentially all at once, or none of the commands are run at all. 

9.2) Transactions should actually be run sequentially in the actual implementation, i.e. a request sent by another client will never be served in the middle of the execution of a Redis Transaction. This allows things like bank transfers to be carried out (A -= $50, B += $50) without any issues. 

9.3) However, my implementation leaves transaction processing to be concurrent without any sort of serializable scheduling of commands in transactions, giving rise to possible race conditions in processing a transaction.

9.4) Support for serialized transactions that are executed sequentially may come in the future. 


Thanks for viewing my project! See you in the next one :)