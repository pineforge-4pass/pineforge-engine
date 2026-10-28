# Run sandbox {#run_sandbox}

Linux hosts can call `pf_run_sandbox_enter_v1` after setup and before the first
strategy call. The native engine also enters at run begin if the host omitted
the call. Entry is irreversible and idempotent: subsequent calls reassert the
SIGSYS handler without adding another filter or widening filesystem roots.
Nothing is installed at library load.

The engine writes one mode receipt to stderr:

```text
sandbox=seccomp+landlock
```

Other modes are `seccomp`, `none`, and `unsupported`. An unavailable layer has
an additional diagnostic, such as `sandbox: landlock unavailable (ABI=0)` or
`sandbox: seccomp unavailable (errno=N)`. Non-Linux platforms log
`sandbox: not implemented on this platform; sandbox=unsupported` once and
return success, including macOS. The report/trades ABI is unchanged.

## Syscall policy

`no_new_privs` prevents privilege gains across execution. A seccomp **deny
list** rejects capabilities strategy code does not need:

- Non-`AF_UNIX` socket/socketpair creation and all `connect`, `bind`, `listen`,
  `accept`, and `accept4` calls.
- Process creation (`fork`, `vfork`, and `clone` without `CLONE_THREAD`) and
  execution (`execve`, `execveat`). `clone3` returns `ENOSYS`, allowing glibc's
  thread-creation fallback to `clone`.
- Tracing and cross-process memory access; mount and namespace operations;
  kernel/module loading, BPF, reboot, swap and accounting; nonzero
  `personality`, userfaultfd, perf events, kernel keys, x86 privilege controls,
  io_uring, and filesystem handle-based opens.

The filter allows other syscalls. Threads, `AF_UNIX` socket creation,
pre-opened channel data transfer and fd passing, memory mappings, scratch
I/O, sink writes, clocks and futexes remain available. It does not change fd
numbers, users/groups, cgroups, resource limits, or the environment, and adds
no time, size, or count cap.

Denied calls return `EPERM`. SIGSYS records the first syscall number and
category in a lock-free slot and raises the cooperative abort ingress. At
the next input boundary or run end, the engine publishes `sandbox_violation`
with `class` (`network`, `process`, `exec`, `ptrace`, `privileged`, or
`filesystem`) and integer `syscall`. It is class `resource`, retryable
`false`. Completed partial rows remain available. `clone3`'s intentional
`ENOSYS` fallback does not latch a violation. Only an unexpected syscall
architecture is killed; an x32 syscall traps.

Seccomp uses TSYNC to cover existing threads. If synchronization fails, it
logs the fallback and protects the calling thread and its future children.
Landlock restricts the calling thread and threads subsequently created from
it; it cannot retrospectively restrict other existing threads. Hosts should
enter before starting strategy workers.

## Filesystem policy and host configuration

Landlock is best effort. Default mode handles write/create/remove, execution,
and, when supported by the kernel ABI, refer and truncate rights. **Reads
remain unrestricted by default**, preserving Python lazy imports. Write
roots are the scratch directory and sink directory the host supplies.
The native fallback also includes the independently selected command-scratch
directory (`PINEFORGE_COMMAND_SCRATCH_DIR`, or the existing temporary-directory
default).
`/dev/null` is writable; `/dev/shm` is writable only when a configured scratch
root or the arena fd is there. Execution rights are granted nowhere.
Inherited descriptors retain their existing access.

Initialize `pf_run_sandbox_config_v1` with `struct_size = sizeof(config)`,
unused fds set to `-1`, and other fields zero. Supply `write_roots` and
`write_root_count`; paths must already exist. `sink_fd` and `arena_fd` are
borrowed hints and remain unchanged. The native fallback resolves the
existing scratch default and the sink path's parent directory.

Set `strict_read = 1` only in a host that stages its inputs and dependencies.
Provide `read_roots` and `read_root_count`. Strict mode also allows reads
under write roots, directories of currently mapped files, `/proc/self`,
`/proc/sys/kernel/random`, `/etc/localtime`, and `/usr/share/zoneinfo`.
Outside opens return `EACCES`; Landlock does not generate SIGSYS. Such a
refusal becomes a typed run failure only if the caller propagates the error.

Set `required = 1` to refuse unavailable seccomp or `no_new_privs` with return
value `-74` (`sandbox_violation`). Landlock remains best effort even then.
An optional failure logs the missing layer and continues. A null config has
no additional writable roots; the native fallback supplies its known paths.

`docker/run_json.py` calls before strategy creation in ordinary, bench,
selected-window, and jailed profiles. It derives roots from its scratch,
sink, input and work paths; old libraries without the export remain
compatible. Hosts can select strict reads with
`PINEFORGE_SANDBOX_STRICT_READ=1` and require seccomp with
`PINEFORGE_SANDBOX_REQUIRED=1`. These variables are read, never changed.

An outer host seccomp policy continues to apply. Linux action precedence
means our `TRAP` takes precedence over an outer `ERRNO`; an outer kill action
still takes precedence over `TRAP`. Cloud Run gen2 and GKE must be probed for
layer availability rather than inferred from the hosting product.
