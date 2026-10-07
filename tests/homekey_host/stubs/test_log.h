#pragma once
#include <string>
// Every log line of the code under test is captured so tests can assert that
// no key material is ever logged.
void test_log(const char *level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
const std::string &test_log_buffer();
void test_log_clear();
