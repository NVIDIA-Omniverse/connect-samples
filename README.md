# Using the Omniverse Client Library

The **Omniverse Client Library** is NVIDIA's C/C++ library (with Python bindings) for
interacting with Nucleus servers and other content providers — listing folders, reading
and writing files, managing ACLs and checkpoints, subscribing to changes, and exchanging
messages over channels.

This document explains how to **pull the Client Library into your own application** and
gives a **brief overview of how to use it**. It is intentionally not an exhaustive API
reference — for the exact behavior of every function, consult the official documentation:

- **Client Library docs:** <https://docs.omniverse.nvidia.com/kit/docs/client_library/latest/index.html>

The current reference version of the library is **`omni_client_library` 2.72.1**.

---

## 1. Pulling the Client Library

The library is distributed as a versioned, per-platform archive on a CloudFront remote.
Each package is simply a download from:

```
https://d4i3qtqj3r0z5.cloudfront.net/${name}@${version}.7z
```

where:

- `${name}` is `omni_client_library.${platform}`
- `${platform}` identifies the OS/architecture. The supported platforms are:
  - `windows-x86_64`
  - `manylinux_2_35_x86_64`
  - `manylinux_2_35_aarch64`
- `${version}` is the library version, e.g. `2.72.1`

The package is a **7-Zip (`.7z`) archive** — note the `.7z` suffix on the URL. So, for
example, the Linux aarch64 build of 2.72.1 lives at:

```
https://d4i3qtqj3r0z5.cloudfront.net/omni_client_library.manylinux_2_35_aarch64@2.72.1.7z
```

Download the archive for your platform and extract it somewhere in your project (this
document assumes `deps/omni_client_library`). Extracting a `.7z` requires a 7-Zip tool —
for example `7z` (from `p7zip-full`) or Python's `py7zr`:

```bash
curl -sSL -o omni_client_library.7z \
  "https://d4i3qtqj3r0z5.cloudfront.net/omni_client_library.manylinux_2_35_x86_64@2.72.1.7z"
7z x omni_client_library.7z -odeps/omni_client_library
```

The archive contains:

| Path | Contents |
|------|----------|
| `include/` | C/C++ headers (`OmniClient.h`, …) |
| `release/` | Release shared libraries (`libomniclient.so` and `libomniverse_connection.so`, or the `.dll` equivalents) |
| `debug/` | Debug shared libraries |
| `release/bindings-python/` | Python bindings (`omni.client`) |

(The archive also includes `PACKAGE-LICENSES/` and some packman metadata, which your
application can ignore.)

> Windows (x86_64) and Linux (x86_64 and aarch64) are supported. macOS is not supported.

---

## 2. Using the Client Library from C++

### Build configuration (CMake)

Point your build at the package's `include/` directory and link against the
`omniclient` library found under `release/` (or `debug/`):

```cmake
cmake_minimum_required(VERSION 3.12)
project(MyOmniApp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to the unpacked package, e.g. set with -DOMNI_CLIENT_DIR=.../deps/omni_client_library
find_path(OMNICLIENT_INCLUDE NAMES OmniClient.h
    PATHS ${OMNI_CLIENT_DIR}/include REQUIRED)

find_library(OMNICLIENT_LIB NAMES omniclient
    PATHS ${OMNI_CLIENT_DIR}/release NO_DEFAULT_PATH)

add_executable(myapp main.cpp)
target_include_directories(myapp PRIVATE ${OMNICLIENT_INCLUDE})
target_link_libraries(myapp PRIVATE ${OMNICLIENT_LIB})

if(WIN32)
    target_link_libraries(myapp PRIVATE shlwapi)
elseif(UNIX)
    target_link_libraries(myapp PRIVATE pthread stdc++fs)
endif()
```

At runtime, the loader must be able to find the shared libraries. With the CMake setup
above, CMake embeds the library directory as an RPATH/RUNPATH in the executable, so it
typically runs without any extra environment setup. If you move the executable, strip its
RPATH, or load the libraries some other way, point the loader at the `release/` directory:

- **Linux:** add `deps/omni_client_library/release` to `LD_LIBRARY_PATH`
- **Windows:** add `deps\omni_client_library\release` to `PATH`

### API overview (C++)

The C API is built around asynchronous functions that return a request handle. You start
a call, optionally pass a completion callback, and use `omniClientWait()` to block until it
finishes. Initialize once at startup and shut down at exit.

```cpp
#include <OmniClient.h>

int main()
{
    // Optional: route library logs into your application.
    // Note the log callback signature has no userData argument.
    omniClientSetLogCallback(
        [](char const* threadName, char const* component, OmniClientLogLevel level,
           char const* message) noexcept {
            printf("%c: %s\n", omniClientGetLogLevelChar(level), message);
        });

    // Initialize — must succeed before any other call
    if (!omniClientInitialize(kOmniClientVersion))
        return 1;
    omniClientSetLogLevel(eOmniClientLogLevel_Warning);

    // Example: list a folder. The call is async; omniClientWait() blocks until done.
    omniClientWait(omniClientList("omniverse://localhost/Projects", nullptr,
        [](void* userData, OmniClientResult result, uint32_t numEntries,
           OmniClientListEntry const* entries) noexcept {
            (void)userData;
            if (result != eOmniClientResult_Ok) return;
            for (uint32_t i = 0; i < numEntries; ++i)
                printf("%s\n", entries[i].relativePath);
        }));

    omniClientShutdown();
    return 0;
}
```

Other operations follow the same pattern: `omniClientStat`, `omniClientCopy`,
`omniClientMove`, `omniClientDelete`, `omniClientCreateFolder`, `omniClientReadFile`,
`omniClientWriteFile`, `omniClientGetServerInfo`, `omniClientGetAcls` / `omniClientSetAcls`,
`omniClientLock` / `omniClientUnlock`, `omniClientCreateCheckpoint` /
`omniClientListCheckpoints`, and channel messaging. See the documentation for each
function's parameters and result codes.

---

## 3. Using the Client Library from Python

No build step is required — just make the bindings importable and the shared library
loadable:

- **Linux:**
  ```bash
  export LD_LIBRARY_PATH="deps/omni_client_library/release:$LD_LIBRARY_PATH"
  export PYTHONPATH="deps/omni_client_library/release/bindings-python:$PYTHONPATH"
  ```
- **Windows:**
  ```batch
  set PATH=deps\omni_client_library\release;%PATH%
  set PYTHONPATH=deps\omni_client_library\release\bindings-python;%PYTHONPATH%
  ```

The bindings ship as compiled extension modules for **Python 3.10, 3.11, and 3.12** —
use one of those interpreter versions.

### API overview (Python)

The Python bindings (`omni.client`) wrap the same functionality with synchronous calls
that return a result code plus any data:

```python
import omni.client

# Optional: route library logs into Python's logging
omni.client.set_log_callback(lambda thread, comp, level, msg: print(msg))
omni.client.set_log_level(omni.client.LogLevel.WARNING)

# Initialize — must succeed before any other call
if not omni.client.initialize():
    raise RuntimeError("omni.client.initialize() failed")

# Example: list a folder
result, entries = omni.client.list("omniverse://localhost/Projects")
if result == omni.client.Result.OK:
    for entry in entries:
        print(entry.relative_path)

# Other examples:
result, entry = omni.client.stat(url)
result, version, content = omni.client.read_file(url)
result = omni.client.copy(src, dst, omni.client.CopyBehavior.OVERWRITE)
result = omni.client.create_folder(url)
print(omni.client.get_version())
```

Additional functions mirror the C API: `move`, `delete`, `get_server_info`,
`get_acls` / `set_acls`, `lock` / `unlock`, `create_checkpoint` / `list_checkpoints`,
`combine_urls`, and channel messaging. Consult the documentation for full signatures.

---

## 4. URLs

The library addresses content with `omniverse://` URLs, for example:

```
omniverse://localhost/Projects/myfile.usd
```

It also handles local paths and other provider schemes (HTTP/S3) where supported. Use
`omniClientCombineUrls` / `omni.client.combine_urls` to compose relative paths against a
base URL.

---

## License

Copyright (c) 2020-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

Released under the MIT License. Permission is hereby granted, free of charge, to any
person obtaining a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so, subject to
including the above copyright notice and this permission notice in all copies or
substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
ANY KIND, EXPRESS OR IMPLIED.

## Security

To report a potential security vulnerability in any NVIDIA product:

- **Web:** <https://www.nvidia.com/object/submit-security-vulnerability.html>
- **E-Mail:** psirt@nvidia.com ([PGP key](https://www.nvidia.com/en-us/security/pgp-key))

Please **do not** report security vulnerabilities through public issue trackers. More
information: <https://www.nvidia.com/en-us/security>
