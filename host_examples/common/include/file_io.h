// Copyright 2026 Kevin Ahrendt
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Output file helpers shared by the host examples (POSIX)

#pragma once

#include <sys/stat.h>

#include <cstddef>
#include <cstdio>

// Whether `output_path` names the regular file at `input_path`, through any
// alias (the same path, a symlink, or a hard link). Opening it for writing with
// "wb" would truncate the input before it is read. A path that does not exist
// yet is never the input, and a device such as /dev/null may be both.
inline bool is_same_file(const char* output_path, const char* input_path) {
    struct stat output_stat {};
    struct stat input_stat {};
    if (stat(output_path, &output_stat) != 0 || stat(input_path, &input_stat) != 0) {
        return false;
    }
    return S_ISREG(output_stat.st_mode) && output_stat.st_dev == input_stat.st_dev &&
           output_stat.st_ino == input_stat.st_ino;
}

// Writes all `size` bytes, returning false on a short write (a full disk, for
// example)
inline bool write_all(FILE* file, const void* data, size_t size) {
    return std::fwrite(data, 1, size, file) == size;
}

// Closes `file`, returning false if flushing its buffered writes or the close
// itself failed. The file is closed either way.
inline bool close_checked(FILE* file) {
    return std::fclose(file) == 0;
}
