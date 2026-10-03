# Fluxer++
**Fluxer++** is a native **C++ SDK** for building fast, reliable Fluxer bots. It provides a REST client, real‑time gateway handling, type‑safe models, and an event‑driven architecture on top of modern C++20.

The original repo will no longer be maintained as I have lost that account. This will be the main repo.

Fluxer++ is a young, in-progress SDK — this README describes what actually exists and compiles today, not the eventual feature set. See [Bottlenecks](#-bottlenecks) for known gaps.

---
## Quick links

[Project Structure](#-project-structure) · [Installation](#-installation) · [Getting Started](#-getting-started) · [REST API Usage](#-rest-api-usage) · [Gateway & Events](#-gateway--events) · [Models](#-models) · [Logging](#-logging) · [Error Handling](#-error-handling) · [Bottlenecks](#-bottlenecks) · [Contributing](#-contributing) · [License](#-license) · [Installation](#-installation)

---
## Coming real soon

- Linux/MacOS build

- Proper guild caching

---
## ✨ Features
- **C++20 core**, built and tested against MSYS2 UCRT64 + libcurl + WinHTTP
- **Event‑driven gateway** — register callbacks for ready, message creation, guild creation, and heartbeat latency
- **REST client** — JSON‑based request/response over libcurl, with automatic one-time retry on HTTP 429
- **Type‑safe models** — Message, User, Channel, Guild, Role, Member, Embed (with author/footer/image/thumbnail/video sub-objects), Attachment, File, PartialEmoji, Reaction
- **Modular architecture** — gateway, REST, event dispatch, and models are separate, independently testable pieces
- **CMake target interface** — `target_link_libraries(your_target fluxerpp)` is all a consumer needs; no manual include-path wiring
- **Windows-only for now** — see [Bottlenecks](#-bottlenecks)

---

## 📁 Project Structure
```
fluxerpp/
│
├── include/
│   └── fluxerpp/
│       ├── FluxerClient.h
│       ├── FluxerConfig.h
│       ├── RestClient.h
│       ├── GatewayClient.h
│       ├── EventDispatcher.h
│       ├── env.h
│       ├── models/
│       │   ├─ Message.h
│       │   ├─ User.h
│       │   ├─ Attachment.h
│       │   ├─ Embed.h
│       │   ├─ EmbedAuthor.h
│       │   ├─ EmbedField.h
│       │   ├─ EmbedFooter.h
│       │   ├─ EmbedImage.h
│       │   ├─ EmbedThumbnail.h
│       │   ├─ EmbedVideo.h
│       │   ├─ Reaction.h
│       │   ├─ PartialEmoji.h
│       │   ├─ File.h
│       │   ├─ Channel.h
│       │   ├─ Guild.h
│       │   ├─ Role.h
│       │   └─ Member.h
│       └── util/
│           ├── Json.h
│           └── Logger.h
│
├── src/
│   ├── FluxerClient.cpp
│   ├── RestClient.cpp
│   ├── GatewayClient.cpp
│   ├── EventDispatcher.cpp
│   └── models/
│       ├─ Message.cpp
│       ├─ User.cpp
│       ├─ Attachment.cpp
│       ├─ Embed.cpp
│       ├─ EmbedAuthor.cpp
│       ├─ EmbedField.cpp
│       ├─ EmbedFooter.cpp
│       ├─ EmbedImage.cpp
│       ├─ EmbedThumbnail.cpp
│       ├─ EmbedVideo.cpp
│       ├─ Reaction.cpp
│       ├─ File.cpp
│       ├─ Channel.cpp
│       ├─ Guild.cpp
│       ├─ Role.cpp
│       └─ Member.cpp
│
├── examples/
│   └── ping_bot.cpp
│
├── tests/
│   └── consumer_smoke_test.cpp
│
└── CMakeLists.txt
```
`PartialEmoji` is header-only (no `.cpp`) — every method is small enough to stay inline.

> **Note:** if your checked-in repo still has a root-level `include/EventDispatcher.h` alongside `include/fluxerpp/EventDispatcher.h`, delete the root one — only the `fluxerpp/`-prefixed path is real. A `builders/` directory (starting with `EmbedVideoBuilder`) also exists in-progress and isn't reflected in this tree yet.

---

## 🔧 Installation
Fluxer++ currently builds on **UCRT64** (MSYS2) with **libcurl** and **WinHTTP**. Precompiled binaries are now available for Windows.

### Clone the repository
```bash
git clone https://github.com/Jw4Lk3r16/Fluxer.cpp
```

---

### 🧩 MSYS2 UCRT64 Package (recommended)
Download the latest `.pkg.tar.zst` from the **Releases** tab and install it directly with **pacman**:

If you are not able to download it you can try one of these commands I recommend running them from downloads so they appear there and not in the abyss

**wget**
```
wget  curl -LO https://github.com/Jw4Lk3r16/Fluxerpp/releases/download/Release/mingw-w64-ucrt-x86_64-fluxerpp
```
**curl**
```
curl -LO  curl -LO https://github.com/Jw4Lk3r16/Fluxerpp/releases/download/Release/mingw-w64-ucrt-x86_64-fluxerpp
```
**Install**
```
pacman -U --noconfirm mingw-w64-ucrt-x86_64-fluxerpp-0.1.0-1-any.pkg.tar.zst
```

This automatically places Fluxer++ into your MSYS2 environment under:
```
/ucrt64/bin/fluxerpp.dll
/ucrt64/lib/libfluxerpp.dll.a
/ucrt64/include/fluxerpp/
```

---

### 📦 Manual file installation (alternative)
If you prefer manual setup, download the **Windows DLL package** from the Release page and extract it anywhere:

```bash
curl -LO https://github.com/Jw4Lk3r16/Fluxerpp/releases/download/Release/mingw-w64-ucrt-x86_64-fluxerpp
```

After extracting the archive, copy the contents into your MSYS2 UCRT64 directories:

```bash
cp fluxerpp.dll /ucrt64/bin/
cp libfluxerpp.dll.a /ucrt64/lib/
cp -r include/fluxerpp /ucrt64/include/
```

This installs Fluxer++ locally without requiring pacman signatures and produces the same environment as the package install.

---

### Build with CMake
```
mkdir build
cd build
cmake ..
cmake --build .
```
This also builds `fluxerpp_consumer_smoke_test`, a small executable that proves the `fluxerpp` CMake target actually links for code outside the library (set `-DFLUXERPP_BUILD_TESTS=OFF` to skip it).

### Using Fluxer++ in your own CMake project
```cmake
add_subdirectory(path/to/Fluxer.cpp)
target_link_libraries(your_target PRIVATE fluxerpp)
```
That's the whole integration — `target_include_directories` on the `fluxerpp` target handles the include path for you.

---

## 🚀 Getting Started
### Minimal Bot Example
```cpp
#include "fluxerpp/FluxerClient.h"
#include <iostream>

int main() {
    fluxerpp::FluxerConfig cfg;
    cfg.token = "YOUR_TOKEN";

    fluxerpp::FluxerClient client(cfg);

    client.gateway().on_ready([]() {
        std::cout << "Bot is online!\n";
    });

    client.login(); // blocks — runs the gateway connect/heartbeat/reconnect loop
}
```
`client.gate` from earlier drafts of this README was never public — the accessor is `client.gateway()`.

---

## 🌐 REST API Usage
```cpp
client.api().send_message(channel_id, nlohmann::json{
    {"content", "Hello from Fluxer++!"}
});
```

Currently implemented:
- Sending, editing, and deleting messages
- Adding/removing a reaction, clearing one emoji's reactions, or clearing all reactions on a message
- Pinning / unpinning messages
- Fetching the bot's own user ID (`RestClient::user_id()` — cached after the first call)
- Any other authenticated route via the generic `get()` / `post()` / `patch()` / `del()` methods

**Not implemented yet** (earlier drafts of this README claimed these — they don't exist): dedicated channel management, guild management, or webhook endpoints. `File::to_dict()` also can't actually carry binary bytes over JSON yet — real attachment upload needs multipart/form-data support in `RestClient`, which isn't built.

---

## 🔌 Gateway & Events
```cpp
client.gateway().on_message_create([](const nlohmann::json& msg) {
    std::cout << "Received: " << msg.value("content", "") << "\n";
});

client.gateway().on_guild_create([](const fluxerpp::models::Guild& guild) {
    std::cout << "Guild ready: " << guild.name << " (" << guild.channels.size() << " channels)\n";
});

client.gateway().on_latency([](int ms) {
    std::cout << "Heartbeat latency: " << ms << "ms\n";
});
```

**Currently implemented events:**
- `on_ready()`
- `on_message_create(const nlohmann::json&)` — hands back the raw gateway payload; it is **not** hydrated into a `fluxerpp::models::Message` yet, despite that model existing
- `on_guild_create(const fluxerpp::models::Guild&)` — this one *is* fully parsed into the model
- `on_latency(int ms)` — fires on every heartbeat ACK with the measured round-trip
- `on_heartbeat_ack()` — fires on every heartbeat ACK with no payload, for callers that just want a liveness signal

**Not implemented yet:** `on_message_update`, `on_message_delete`, `on_reaction_add`, `on_reaction_remove`, `on_member_join`, `on_member_leave`, `on_typing_start`, and anything else beyond the five above. `GatewayClient` also has no persistent guild/channel cache — `on_guild_create` hands you a `Guild` for that one event only; nothing stores it for later lookup.

---

## 🧱 Models
- **Message** — content, embeds, attachments, mentions, reactions (see the `on_message_create` caveat above — not yet produced by the gateway)
- **User** — id, username, discriminator, global_name, avatar, bot
- **Channel** — id, type, position, permission overwrites, voice-specific fields
- **Guild** — id, name, owner, features, roles, channels, members
- **Role** — id, name, color, position, permissions bitfield
- **Member** — a `User` plus guild-specific fields (nick, role ids, joined_at, deaf/mute)
- **Embed** — title, description, color, fields, plus `EmbedAuthor`, `EmbedFooter`, `EmbedImage`, `EmbedThumbnail`, and `EmbedVideo` sub-objects
- **Attachment** — filename, content type, size, URL
- **File** — local file wrapper for `Message::send()`'s file parameter; see the REST section above for its current limitation
- **PartialEmoji** / **Reaction** — reaction-related types, including custom vs. unicode emoji handling

Each model has a `from_data(const util::Json&)` static factory, and the ones used in outgoing requests (`Embed`, `File`) have a matching `to_dict()`.

---
## Future packaging

| Packaging Method | Best For |
| --- | --- |
| Precompiled DLL + headers | Most users |
| Static library | Performance bots |
| CMake package | Serious C++ devs |
| vcpkg port | Windows/Linux/Mac |
| Conan package | Enterprise users |
| MSYS2 package | UCRT64 users |
| Single‑header | Beginners |
| NuGet | Visual Studio users |

None of these exist yet — today, `add_subdirectory` + `target_link_libraries` (see [Installation](#-installation)) is the only supported integration path.

---

## 📜 Logging
```cpp
#include "fluxerpp/util/Logger.h"

using fluxerpp::util::Logger;
using fluxerpp::util::LogLevel;

Logger::instance().set_level(LogLevel::Debug); // default level is Info
Logger::instance().info("Gateway connected");
Logger::instance().error("Failed to parse JSON");
```
`Logger` is a singleton (`Logger::instance()`), not static free functions. Levels, in order: `Debug`, `Info`, `Warn`, `Error`, `Silent`. `GatewayClient::set_debug_logging(true)` turns on verbose per-frame `RAW FRAME` / `DISPATCH` tracing at the `Debug` level. Secrets (bot token, Authorization headers) are redacted before anything is logged, even at `Debug`.

---

## ⚠️ Error Handling
There's no `Result`/`ok()`/`error()` wrapper — `RestClient` methods return `nlohmann::json` directly on success and throw `std::runtime_error` on failure:

```cpp
try {
    client.api().send_message(channel_id, nlohmann::json{{"content", "hi"}});
} catch (const std::exception& ex) {
    std::cerr << "Send failed: " << ex.what() << "\n";
}
```
A `429` response is retried once automatically (honoring `Retry-After`) before this throws — you only see the exception if the retry also fails or the response was some other non‑2xx status.

---
## ⚠️ Bottlenecks

**This SDK only supports Windows** at the moment — the gateway transport is WinHTTP-specific. A cross-platform migration (likely IXWebSocket or Boost.Beast for the gateway, keeping libcurl for REST, which is already cross-platform) is planned but not started.

Other known gaps, tracked so they don't get lost:
- No persistent guild/channel cache on `FluxerClient`
- `on_message_create` doesn't hydrate a `Message` model
- No multipart file upload support
- Test coverage is one consumer smoke test — no unit tests for gateway protocol transitions or REST edge cases yet

---

## 🛠️ Contributing
Contributions are welcome! You can help by:

- Improving documentation
- Adding more examples
- Implementing additional gateway events
- Enhancing model coverage
- Adding real test coverage
- Working toward cross-platform support

Open a pull request or create an issue on GitHub.

---

## 📄 License
Fluxer++ is licensed under MIT.

---

[Back to top](#fluxer)
