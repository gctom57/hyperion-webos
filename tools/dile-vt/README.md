# DILE_VT tools

Two small programs for diagnosing video capture through `libdile_vt` (webOS 3.x and 4.x), found while debugging a webOS 3.5 TV.

## Why video capture can get stuck

`DILE_VT_Create` (via `HAL_VT_Init`) registers the TV's capture unit with the kernel display driver under the **calling process's PID**. Only `DILE_VT_Destroy` (via `HAL_VT_Finalize`) unregisters it, again by PID.

If a process dies between those two calls, the unit stays registered to a PID that no longer exists. That happens after a crash, a `kill -9`, or reinstalling the app while capturing. A `DILE_VT_Create` that fails part way leaves its own registration behind too. From then on, every `DILE_VT_Create` fails with `HAL_VT_Init error` until the TV reboots. Powering off with the remote doesn't count: with Quick Start+ that's only standby.

hyperion-webos repairs this by itself for its own processes. It records the owner in `/tmp/piccap-dilevt.pid` and, on the next start, unregisters an owner that has died. These tools cover everything else.

`DILE_VT_Start` / `DILE_VT_Stop` are unrelated to this. According to LG's header, they're a cross-process lock (a SysV semaphore with `SEM_UNDO`, so it's released automatically when a process dies).

## Tools

**`dvtest <steps>`** runs the DILE_VT API one step at a time:

| Step | Meaning |
|---|---|
| `c` | create |
| `s` | start (take the lock) |
| `t` | stop (release the lock) |
| `d` | destroy |
| `w` | wait 1 s |
| `k` | exit right away without cleanup, like `kill -9` |

- `dvtest cstd` checks that capture can be opened.
- `dvtest ck` reproduces the stuck state on purpose: create, then die.

**`vtreset <pid>`** releases the registration left by the dead process `<pid>`. It sends the same request `HAL_VT_Finalize` would have sent, and refuses if that process is still running.

## Build

These targets aren't part of the normal build. With the webOS toolchain set up as for hyperion-webos:

```sh
cmake -DCMAKE_TOOLCHAIN_FILE=<toolchain>/share/buildroot/toolchainfile.cmake ..
make dvtest vtreset
```

## Example (on the TV, as root)

```sh
./dvtest cstd        # create: OK ... -> capture works
./dvtest ck & wait   # dies holding the capture unit; note its pid (e.g. 22770)
./dvtest cstd        # create: FAILED
./vtreset 22770      # KADP_DE_SetVTM(enable=0, pid=22770) -> 0
./dvtest cstd        # create: OK again
```
