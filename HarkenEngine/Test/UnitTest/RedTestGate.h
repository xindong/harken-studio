// Gate for contract-seam and intended-red tests. This suite had no prior
// red-test convention, so this establishes one: a contract that is pinned but
// cannot be met yet registers a *visible* test that skips by default, its
// message naming both the missing capability and who unskips it. The same test
// becomes a hard failure only when the environment sets HARKEN_RUN_RED_TESTS=1.
//
// The default (skip) keeps the ordinary suite green while preserving the pin as
// executable evidence; the opt-in (red) lets a reviewer watch the pin fail for
// exactly its missing capability, then flip to green when the capability lands
// without the assertion's meaning changing.
#ifndef HARKEN_TESTS_TESTSUPPORT_RED_TEST_GATE_H
#define HARKEN_TESTS_TESTSUPPORT_RED_TEST_GATE_H

#include <cstdlib>
#include <cstring>

namespace harkentest {

// True when HARKEN_RUN_RED_TESTS is set to "1" in the process environment. Read
// through std::getenv (the suite already defines _CRT_SECURE_NO_WARNINGS for
// the MS CRT deprecation aimed at it).
inline bool RunRedTestsEnabled() {
    const char* env = std::getenv("HARKEN_RUN_RED_TESTS");
    return env != nullptr && std::strcmp(env, "1") == 0;
}

} // namespace harkentest

#endif // HARKEN_TESTS_TESTSUPPORT_RED_TEST_GATE_H
