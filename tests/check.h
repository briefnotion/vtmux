// The few lines every test program needs.
#pragma once
#include <cstdio>
inline int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define CHECK_EQ(a, b) do { const auto va_ = (a); const auto vb_ = (b); if (!(va_ == vb_)) { std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); failures++; } } while (0)
