#pragma once

#include <cstddef>

inline bool parse_volume_command(const char* data, size_t length, int* volume) {
    if (data == nullptr || volume == nullptr || length == 0) return false;

    size_t begin = 0;
    while (begin < length && (data[begin] == ' ' || data[begin] == '\t' || data[begin] == '\r' || data[begin] == '\n')) begin++;
    if (begin == length) return false;

    int value = 0;
    size_t pos = begin;
    while (pos < length && data[pos] >= '0' && data[pos] <= '9') {
        value = value * 10 + (data[pos] - '0');
        if (value > 10) return false;
        pos++;
    }
    if (pos == begin) return false;
    while (pos < length && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r' || data[pos] == '\n')) pos++;
    if (pos != length || value < 1 || value > 10) return false;

    *volume = value;
    return true;
}
