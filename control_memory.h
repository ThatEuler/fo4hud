#pragma once
#include "control_json.h"
#include <vector>
namespace fo4control {
bool read(uintptr_t address, void* buffer, size_t size);
template<class T> bool read_value(uintptr_t address, T& value) { return read(address,&value,sizeof(value)); }
Json memory_maps();
Json read_memory(json_object* args);
Json write_memory(json_object* args);
}
