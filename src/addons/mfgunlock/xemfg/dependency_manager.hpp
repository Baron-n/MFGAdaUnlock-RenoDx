// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <vector>
namespace mfgunlock::dependencies {
enum class Health { Missing, Valid, UpdateOptional, Unknown, Error };
struct File { std::string name,path,version,sha256; Health health{Health::Missing}; bool mapped{},required{}; };
struct Status { bool checked{},busy{},helperRunning{}; std::string message; std::vector<File> files; };
void Inspect();
void Repair();
Status Snapshot();
void Draw();
}
