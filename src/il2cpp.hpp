#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "process.hpp"

// Minimal reader for the IL2CPP runtime structures of a running Unity game:
// classes (Il2CppClass), their fields (FieldInfo) and managed objects.
namespace ms::il2cpp {

struct Field {
    std::string name;
    int32_t offset = 0;      // instance fields: from the object start; statics: from static_fields
    bool is_static = false;
};

struct Class {
    uintptr_t klass = 0;           // Il2CppClass*
    std::string name;
    uintptr_t static_fields = 0;   // base address of the class's static data
    std::vector<Field> fields;

    // Offset of a field by name, or -1.
    int32_t offset_of(const std::string& field) const;
    bool has_fields(std::initializer_list<const char*> names) const;
};

// Reads one candidate Il2CppClass*. False if it does not look like a class with fields.
bool read_class(const Process& process, uintptr_t klass, Class& out);

std::string read_cstring(const Process& process, uintptr_t address, size_t max_length = 96);

// System.String object -> UTF-8.
std::string read_string(const Process& process, uintptr_t string_object);

// All classes with each of these names (several may exist; callers pick by fields).
std::map<std::string, std::vector<Class>> find_classes(const Process& process,
                                                       const std::vector<std::string>& names);

// Addresses whose first qword (object header) equals one of the class pointers.
std::map<uintptr_t, std::vector<uintptr_t>> find_instances(const Process& process,
                                                           const std::vector<uintptr_t>& klasses);

}  // namespace ms::il2cpp
