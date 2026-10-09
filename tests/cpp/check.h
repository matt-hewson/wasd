// Minimal test framework for the C++ tests (stdlib only, like the rest of
// the project). Each TEST registers itself; run_tests.cpp runs them all,
// or those whose name contains a filter given on the command line.
//
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); CHECK_NEAR(a, b, eps); }
//
// A failed check logs file:line with both values and marks the test failed;
// the test carries on, so one run shows every failure.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace check {

struct Test {
  const char* name;
  std::function<void()> body;
};

inline std::vector<Test>& Registry() {
  static std::vector<Test> tests;
  return tests;
}

inline int& Failures() {  // failed checks in the running test
  static int n = 0;
  return n;
}

struct Register {
  Register(const char* name, std::function<void()> body) { Registry().push_back({name, std::move(body)}); }
};

template <class T>
std::string Show(const T& v) {
  std::ostringstream out;
  out << v;
  return out.str();
}
inline std::string Show(const std::string& v) { return "\"" + v + "\""; }
inline std::string Show(const char* v) { return v ? "\"" + std::string(v) + "\"" : "(null)"; }
inline std::string Show(bool v) { return v ? "true" : "false"; }
inline std::string Show(const std::wstring& v) {
  std::string out = "L\"";
  for (wchar_t c : v) out += c < 128 ? static_cast<char>(c) : '?';
  return out + "\"";
}

inline void Fail(const char* file, int line, const std::string& what) {
  ++Failures();
  std::printf("    FAIL %s:%d: %s\n", file, line, what.c_str());
}

}  // namespace check

#define CHECK_CONCAT2(a, b) a##b
#define CHECK_CONCAT(a, b) CHECK_CONCAT2(a, b)

#define TEST(name)                                                                   \
  static void CHECK_CONCAT(test_, name)();                                           \
  static check::Register CHECK_CONCAT(register_, name)(#name, CHECK_CONCAT(test_, name)); \
  static void CHECK_CONCAT(test_, name)()

#define CHECK(cond) \
  do {              \
    if (!(cond)) check::Fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(actual, expected)                                                                    \
  do {                                                                                                \
    const auto& check_a_ = (actual);                                                                  \
    const auto& check_e_ = (expected);                                                                \
    if (!(check_a_ == check_e_))                                                                      \
      check::Fail(__FILE__, __LINE__,                                                                 \
                  #actual " == " #expected ": got " + check::Show(check_a_) + ", want " + check::Show(check_e_)); \
  } while (0)

#define CHECK_NEAR(actual, expected, eps)                                                             \
  do {                                                                                                \
    const double check_a_ = (actual), check_e_ = (expected);                                          \
    if (!(std::fabs(check_a_ - check_e_) <= (eps)))                                                   \
      check::Fail(__FILE__, __LINE__,                                                                 \
                  #actual " ~= " #expected ": got " + check::Show(check_a_) + ", want " + check::Show(check_e_)); \
  } while (0)
