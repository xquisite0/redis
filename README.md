[![progress-banner](https://backend.codecrafters.io/progress/redis/2413cc25-ce65-4e24-b894-57e7717ca4bf)](https://app.codecrafters.io/users/codecrafters-bot?r=2qF)

A Redis Implementation in C++ from scratch that supports the following list of commands: 

`ECHO`, `PING`, `SET`, `GET`, `CONFIG GET`, `KEYS`, `INFO replication`, `WAIT`, `TYPE`, `XADD`, `XRANGE`, `XREAD`, `INCR`, `MULTI`, `EXEC`, `DISCARD`.

Interesting points:

1) Socket Programming lies at the heart of this program. Master-Replica / Master-Client / Replica-Client connections are all built off sockets.

2) Concurrent clients are "supported" by Redis instances with threads. Concurrent clients can be processed in parallel, however race conditions may still arise as proper write guards on shared data structures have not been implemented.

3) Setting keys with expiries is supported in this implementation. For instance `SET foo bar px 100` will set the key "foo" to "bar" with an expiry of 100 milliseconds. Any future `GET` commands that are executed more than 100ms past the SET command will return NULL.

4) Persistence is supported! 